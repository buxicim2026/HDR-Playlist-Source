/*
 * audio.c — child audio capture & forwarding with timestamp continuity.
 *
 * `obs_source_add_audio_capture_callback()` hands us the child's *pre-volume*
 * PCM (float planar, already at the OBS mixer rate like any media source).
 * We re-emit it from the parent via `obs_source_output_audio()`.
 *
 * IMPORTANT: libobs does **not** synchronously copy the audio data we hand
 * to `obs_source_output_audio()`; it queues it for the audio output thread
 * which reads the planes asynchronously. Freeing our buffers right after
 * the call is therefore a use-after-free that crashes OBS. To make this
 * safe we keep each frame's data alive in a small ring buffer long enough
 * for the audio thread to consume it.
 *
 * Timestamps are rebased onto a continuous per-playback timeline so the
 * parent never looks like a brand-new source at clip boundaries, and a
 * ~15 ms fade-in is applied at every switch to suppress clicks.
 */

#include <string.h>

#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "audio.h"

#define HDRP_FADE_NS  (15 * 1000000LL) /* 15 ms de-click ramp       */
#define HDRP_RING_LEN 8                 /* ~165 ms of PCM headroom   */
/* libobs bounds: obs_source_audio.data[] is MAX_AV_PLANES (8) entries and a
 * speaker layout never exceeds 8 channels. */
#define HDRP_PLANES_MAX 8

struct hdrp_frame {
	float *plane[HDRP_PLANES_MAX];
	uint8_t channels;
	bool used;
};

struct hdrp_audio {
	obs_source_t *parent;
	obs_source_t *active; /* currently captured child, or NULL */

	int64_t offset;     /* child_ts -> parent_ts rebase (ns) */
	int64_t fade_until; /* wall clock ns; gain ramps until here */

	/* One-shot diagnostics so the OBS log can prove audio is flowing. */
	bool log_first;

	/* Ring of frames awaiting consumption by the audio output thread.
	 * ring_head == index of the *next* slot we will write into. When the
	 * ring is full we recycle the oldest slot, which by then has been
	 * drained by the audio thread (~165 ms at 48 kHz). */
	struct hdrp_frame ring[HDRP_RING_LEN];
	int ring_head;
	int ring_filled; /* number of currently-used slots, <= HDRP_RING_LEN */
};

static void release_frame(struct hdrp_frame *f)
{
	if (!f || !f->used)
		return;
	for (uint8_t c = 0; c < f->channels; c++)
		bfree(f->plane[c]);
	memset(f, 0, sizeof(*f));
}

static void reset_ring(struct hdrp_audio *au)
{
	for (int i = 0; i < HDRP_RING_LEN; i++)
		release_frame(&au->ring[i]);
	au->ring_head = 0;
	au->ring_filled = 0;
}

/* ------------------------------------------------------------------ */
/* capture callback                                                   */
/* ------------------------------------------------------------------ */

static void on_audio_capture(void *param, obs_source_t *source,
			     const struct audio_data *audio, bool muted)
{
	struct hdrp_audio *au = param;
	if (!au || !audio || audio->frames == 0 || muted)
		return;

	/* The capture callback hands us PCM that libobs already resampled to
	 * the OBS mixer rate — report that same rate back to the parent or
	 * libobs will try to (re)create a resampler from 0 Hz and silently
	 * drop the audio. */
	audio_t *aout = obs_get_audio();
	uint32_t mix_rate = aout ? audio_output_get_sample_rate(aout) : 48000;
	uint32_t mix_channels = aout ? audio_output_get_channels(aout) : 2;
	if (!mix_rate)
		mix_rate = 48000;
	if (!mix_channels)
		mix_channels = 2;

	enum speaker_layout layout = obs_source_get_speaker_layout(source);
	size_t channels = get_audio_channels(layout);
	if (channels == 0) {
		/* Child has not reported a layout yet (first frame): fall back
		 * to the mixer layout instead of dropping the audio. */
		channels = mix_channels;
		layout = channels >= 2 ? SPEAKERS_STEREO : SPEAKERS_MONO;
	}
	if (channels > HDRP_PLANES_MAX)
		return;

	if (!au->log_first) {
		au->log_first = true;
		blog(LOG_INFO,
		     "[HDR-PL] audio forwarding active: %d channels @ %d Hz",
		     (int)channels, (int)mix_rate);
	}

	const size_t bytes = audio->frames * sizeof(float);

	const float *planes[HDRP_PLANES_MAX];
	for (size_t c = 0; c < channels; c++) {
		planes[c] = (const float *)audio->data[c];
		if (!planes[c])
			return;
	}

	/* Recycle the oldest frame if the ring is full. By construction the
	 * ring is large enough that the audio output thread has long since
	 * finished reading from that slot. */
	struct hdrp_frame *slot = &au->ring[au->ring_head];
	release_frame(slot);

	for (size_t c = 0; c < channels; c++) {
		slot->plane[c] = bmalloc(bytes);
		memcpy(slot->plane[c], planes[c], bytes);
	}
	slot->channels = (uint8_t)channels;
	slot->used = true;

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
			float *pcm = slot->plane[c];
			for (size_t i = 0; i < audio->frames; i++) {
				const float t = (float)i / (float)audio->frames;
				pcm[i] *= start + t * (1.0f - start);
			}
		}
	}

	struct obs_source_audio out;
	memset(&out, 0, sizeof(out));
	for (size_t c = 0; c < channels; c++)
		out.data[c] = (uint8_t *)slot->plane[c];
	out.frames = (uint32_t)audio->frames;
	out.speakers = layout;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.samples_per_sec = mix_rate;
	out.timestamp = (uint64_t)ts;
	obs_source_output_audio(au->parent, &out);

	/* Advance the ring. */
	au->ring_head = (au->ring_head + 1) % HDRP_RING_LEN;
	if (au->ring_filled < HDRP_RING_LEN)
		au->ring_filled++;

	/* Keep the rebase mapping explicit: ts_parent = offset + ts_child. */
	au->offset = ts - (int64_t)audio->timestamp;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

struct hdrp_audio *hdrp_audio_create(obs_source_t *parent)
{
	struct hdrp_audio *au = bzalloc(sizeof(*au));
	au->parent = parent;
	au->active = NULL;
	au->offset = 0;
	au->fade_until = 0;
	return au;
}

void hdrp_audio_destroy(struct hdrp_audio *au)
{
	if (!au)
		return;
	hdrp_audio_detach_current(au);
	reset_ring(au);
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
	blog(LOG_INFO, "[HDR-PL] audio capture attached to '%s'",
	     obs_source_get_name(child));
	obs_source_add_audio_capture_callback(child, on_audio_capture, au);

	/* The OBS audio thread is responsible for keeping the ring drained;
	 * reset it so any stale frames from a previous child cannot leak into
	 * the new child's playback. */
	reset_ring(au);

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
	reset_ring(au);
	au->fade_until = os_gettime_ns() + HDRP_FADE_NS;
}