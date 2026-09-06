/*
 * audio.c — child audio capture & forwarding with timestamp continuity.
 *
 * obs_source_add_audio_capture_callback() hands us the child's *pre-volume*
 * PCM (float planar, already at the OBS mixer rate like any media source).
 * We re-emit it from the parent via obs_source_output_audio(). Timestamps are
 * rebased onto one continuous timeline per playback session so OBS never sees
 * a brand-new source at clip boundaries, and a ~15 ms fade-in is applied at
 * every switch to suppress clicks.
 */

#include <string.h>

#include <obs.h>
#include <media-io/audio-io.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "audio.h"

#define HDRP_FADE_NS (15 * 1000000LL) /* 15 ms de-click ramp */

struct hdrp_audio {
	obs_source_t *parent;
	obs_source_t *active; /* currently captured child, or NULL */

	int64_t offset;     /* child_ts -> parent_ts rebase (ns) */
	int64_t fade_until; /* wall clock ns; gain ramps until here */
};

static void on_audio_capture(void *param, obs_source_t *source,
			     const struct audio_data *audio, bool muted)
{
	struct hdrp_audio *au = param;
	if (!au || !audio || audio->frames == 0 || muted)
		return;

	const enum speaker_layout layout = obs_source_get_speaker_layout(source);
	const size_t channels = get_audio_channels(layout);
	if (channels == 0 || channels > MAX_AUDIO_PLANES)
		return;

	const size_t frames = audio->frames;

	const float *planes[MAX_AUDIO_PLANES];
	for (size_t c = 0; c < channels; c++) {
		planes[c] = (const float *)audio->data[c];
		if (!planes[c])
			return;
	}

	/* Snapshot so our output pointers stay valid for libobs. */
	float *copy[MAX_AUDIO_PLANES];
	for (size_t c = 0; c < channels; c++) {
		copy[c] = bmalloc(frames * sizeof(float));
		memcpy(copy[c], planes[c], frames * sizeof(float));
	}

	int64_t ts = (int64_t)audio->timestamp + au->offset;

	/* De-click fade over the first 15 ms after a switch. */
	int64_t now = os_gettime_ns();
	if (now < au->fade_until) {
		const int64_t left = au->fade_until - now;
		const float start = left > 0
					    ? (float)((double)left /
						      (double)HDRP_FADE_NS)
					    : 0.0f;
		for (size_t c = 0; c < channels; c++) {
			float *pcm = copy[c];
			for (size_t i = 0; i < frames; i++) {
				const float t = (float)i / (float)frames;
				pcm[i] *= start + t * (1.0f - start);
			}
		}
	}

	struct obs_source_audio out;
	memset(&out, 0, sizeof(out));
	for (size_t c = 0; c < channels; c++)
		out.data[c] = copy[c];
	out.frames = (uint32_t)frames;
	out.speakers = layout;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.timestamp = (uint64_t)ts;
	obs_source_output_audio(au->parent, &out);

	/* Keep the mapping explicit: ts_child -> ts_parent = offset + ts_child.
	 * The offset is intentionally *not* reset when a new clip is attached,
	 * so each clip's timeline continues exactly where the previous one
	 * ended. Only hdrp_audio_reset_timeline() drops it (first play). */
	au->offset = ts - (int64_t)audio->timestamp;

	for (size_t c = 0; c < channels; c++)
		bfree(copy[c]);
}

/* ------------------------------------------------------------------ */

struct hdrp_audio *hdrp_audio_create(obs_source_t *parent)
{
	struct hdrp_audio *au = bzalloc(sizeof(*au));
	au->parent = parent;
	au->active = NULL;
	au->offset = 0;
	return au;
}

void hdrp_audio_destroy(struct hdrp_audio *au)
{
	if (!au)
		return;
	hdrp_audio_detach_current(au);
	bfree(au);
}

void hdrp_audio_attach(struct hdrp_audio *au, obs_source_t *child)
{
	if (!au || !child)
		return;
	if (au->active == child)
		return;
	hdrp_audio_detach_current(au);
	au->active = child;
	obs_source_add_audio_capture_callback(child, on_audio_capture, au);

	/* New clip: keep `offset` (which points at the end of the previous
	 * clip) and let the first callback anchor child_ts~0 right there. */
	au->fade_until = os_gettime_ns() + HDRP_FADE_NS;
}

void hdrp_audio_detach_current(struct hdrp_audio *au)
{
	if (!au || !au->active)
		return;
	obs_source_remove_audio_capture_callback(au->active, on_audio_capture,
						 au);
	au->active = NULL;
}

void hdrp_audio_reset_timeline(struct hdrp_audio *au)
{
	if (!au)
		return;
	au->offset = 0;
	au->fade_until = os_gettime_ns() + HDRP_FADE_NS;
}
