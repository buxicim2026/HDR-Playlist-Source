/*
 * audio.c — bounded ring buffer between the child's audio capture callback
 * (decode thread) and the parent's audio_render callback (audio thread).
 *
 * Overflow policy: drop the oldest samples. That keeps latency bounded and
 * makes it impossible for the buffer to grow without limit if the consumer
 * stalls. Underflow policy: output silence, never block.
 */

#include <string.h>

/* util.h must come before the obs headers on Windows (it pulls in windows.h
 * and defines our cross-platform mutex). */
#include "util.h"

#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "audio.h"

#define HDRP_CH_MAX 8
#define HDRP_RING_MS 500

struct hdrp_audio {
	hdrp_mutex_t mtx;

	float *buf[HDRP_CH_MAX];
	size_t cap_frames; /* ring capacity, in frames */
	size_t head;       /* read position */
	size_t count;      /* frames currently buffered */
	uint32_t channels;
	bool ready; /* buffers allocated & channel count known */

	obs_source_t *child; /* currently captured child, or NULL */

	uint64_t ts_next; /* timestamp we hand back to libobs */
	bool log_once;
};

/* ------------------------------------------------------------------ */
/* ring helpers (caller holds the mutex)                               */
/* ------------------------------------------------------------------ */

static void ring_write(struct hdrp_audio *au, const float *const *src,
		       size_t frames)
{
	const size_t cap = au->cap_frames;
	size_t w = (au->head + au->count) % cap;

	for (uint32_t c = 0; c < au->channels; c++) {
		size_t first = frames;
		if (w + first > cap)
			first = cap - w;
		memcpy(au->buf[c] + w, src[c], first * sizeof(float));
		if (first < frames)
			memcpy(au->buf[c], src[c] + first,
			       (frames - first) * sizeof(float));
	}
}

static void ring_read(const struct hdrp_audio *au, uint32_t ch, float *dst,
		      size_t frames)
{
	const size_t cap = au->cap_frames;
	size_t r = au->head;
	size_t first = frames;
	if (r + first > cap)
		first = cap - r;
	memcpy(dst, au->buf[ch] + r, first * sizeof(float));
	if (first < frames)
		memcpy(dst + first, au->buf[ch],
		       (frames - first) * sizeof(float));
}

static void ring_free(struct hdrp_audio *au)
{
	for (uint32_t c = 0; c < HDRP_CH_MAX; c++) {
		bfree(au->buf[c]);
		au->buf[c] = NULL;
	}
	au->cap_frames = 0;
	au->head = 0;
	au->count = 0;
	au->ready = false;
}

static bool ring_alloc(struct hdrp_audio *au, uint32_t channels,
		       uint32_t sample_rate)
{
	size_t cap;

	ring_free(au);
	if (!channels || !sample_rate)
		return false;

	cap = (size_t)sample_rate * HDRP_RING_MS / 1000;
	if (cap < AUDIO_OUTPUT_FRAMES * 4)
		cap = AUDIO_OUTPUT_FRAMES * 4;

	for (uint32_t c = 0; c < channels; c++) {
		au->buf[c] = bzalloc(cap * sizeof(float));
		if (!au->buf[c]) {
			ring_free(au);
			return false;
		}
	}
	au->channels = channels;
	au->cap_frames = cap;
	au->ready = true;
	return true;
}

/* ------------------------------------------------------------------ */
/* producer: child audio capture callback                              */
/* ------------------------------------------------------------------ */

static void on_audio_capture(void *param, obs_source_t *source,
			     const struct audio_data *audio, bool muted)
{
	struct hdrp_audio *au = param;
	const float *src[HDRP_CH_MAX];
	uint32_t channels;
	uint32_t rate;
	audio_t *aout;
	size_t overflow;

	if (!au || !audio || !audio->frames || muted)
		return;

	/* struct audio_data carries no channel count; the child has already
	 * been resampled to the mixer rate by libobs. */
	channels = (uint32_t)get_audio_channels(
		obs_source_get_speaker_layout(source));
	if (channels == 0 || channels > HDRP_CH_MAX)
		return;

	aout = obs_get_audio();
	rate = aout ? audio_output_get_sample_rate(aout) : 48000;
	if (!rate)
		rate = 48000;

	for (uint32_t c = 0; c < channels; c++) {
		src[c] = (const float *)audio->data[c];
		if (!src[c])
			return;
	}

	hdrp_mutex_lock(&au->mtx);

	if (!au->ready || au->channels != channels)
		ring_alloc(au, channels, rate);
	if (!au->ready) {
		hdrp_mutex_unlock(&au->mtx);
		return;
	}

	/* Overflow: drop the oldest samples so latency stays bounded. */
	if (au->count + audio->frames > au->cap_frames) {
		overflow = au->count + audio->frames - au->cap_frames;
		au->head = (au->head + overflow) % au->cap_frames;
		au->count -= overflow;
	}

	ring_write(au, src, audio->frames);
	au->count += audio->frames;

	if (!au->log_once) {
		au->log_once = true;
		blog(LOG_INFO,
		     "[HDR-PL] audio forwarding active: %u ch @ %u Hz",
		     channels, rate);
	}

	hdrp_mutex_unlock(&au->mtx);
}

/* ------------------------------------------------------------------ */
/* consumer: parent source audio_render                                */
/* ------------------------------------------------------------------ */

bool hdrp_audio_render(struct hdrp_audio *au, uint64_t *ts_out,
		       struct obs_source_audio_mix *audio_output,
		       uint32_t mixers, size_t channels, size_t sample_rate)
{
	size_t want = AUDIO_OUTPUT_FRAMES;
	size_t take = 0;
	uint32_t mix;

	if (ts_out)
		*ts_out = os_gettime_ns();

	if (!au || !audio_output) {
		if (audio_output) {
			for (mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
				if ((mixers & (1u << mix)) == 0)
					continue;
				for (size_t ch = 0; ch < channels; ch++) {
					if (audio_output->output[mix].data[ch])
						memset(audio_output->output[mix]
							       .data[ch],
						       0, want * sizeof(float));
				}
			}
		}
		return true;
	}

	hdrp_mutex_lock(&au->mtx);
	take = au->count < want ? au->count : want;

	for (mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		size_t ch;
		if ((mixers & (1u << mix)) == 0)
			continue;
		for (ch = 0; ch < channels; ch++) {
			float *dst = audio_output->output[mix].data[ch];
			if (!dst)
				continue;
			if (take && au->ready) {
				uint32_t srcch =
					ch < au->channels
						? (uint32_t)ch
						: au->channels - 1;
				ring_read(au, srcch, dst, take);
			}
			if (take < want)
				memset(dst + take, 0,
				       (want - take) * sizeof(float));
		}
	}

	if (take) {
		au->head = (au->head + take) % au->cap_frames;
		au->count -= take;
	}
	hdrp_mutex_unlock(&au->mtx);

	UNUSED_PARAMETER(sample_rate);
	return true;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

struct hdrp_audio *hdrp_audio_create(void)
{
	struct hdrp_audio *au = bzalloc(sizeof(*au));
	hdrp_mutex_init(&au->mtx);
	au->ts_next = 0;
	return au;
}

void hdrp_audio_destroy(struct hdrp_audio *au)
{
	if (!au)
		return;
	hdrp_audio_detach(au);
	hdrp_mutex_lock(&au->mtx);
	ring_free(au);
	hdrp_mutex_unlock(&au->mtx);
	hdrp_mutex_destroy(&au->mtx);
	bfree(au);
}

void hdrp_audio_attach(struct hdrp_audio *au, obs_source_t *child)
{
	if (!au || !child)
		return;

	hdrp_mutex_lock(&au->mtx);
	if (au->child == child) {
		hdrp_mutex_unlock(&au->mtx);
		return;
	}
	if (au->child)
		obs_source_remove_audio_capture_callback(
			au->child, on_audio_capture, au);
	au->child = child;
	au->head = 0;
	au->count = 0;
	obs_source_add_audio_capture_callback(child, on_audio_capture, au);
	hdrp_mutex_unlock(&au->mtx);

	blog(LOG_INFO, "[HDR-PL] audio capture attached to '%s'",
	     obs_source_get_name(child));
}

void hdrp_audio_detach(struct hdrp_audio *au)
{
	obs_source_t *child;

	if (!au)
		return;
	hdrp_mutex_lock(&au->mtx);
	child = au->child;
	au->child = NULL;
	au->count = 0;
	au->head = 0;
	if (child)
		obs_source_remove_audio_capture_callback(child,
							 on_audio_capture, au);
	hdrp_mutex_unlock(&au->mtx);
}

void hdrp_audio_flush(struct hdrp_audio *au)
{
	if (!au)
		return;
	hdrp_mutex_lock(&au->mtx);
	au->count = 0;
	au->head = 0;
	hdrp_mutex_unlock(&au->mtx);
}
