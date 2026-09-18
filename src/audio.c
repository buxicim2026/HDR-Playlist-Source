/*
 * audio.c — pushes the presented clip's PCM to the parent source with
 * obs_source_output_audio(), i.e. through libobs' own async-audio pipeline
 * (buffering, placement, timestamp smoothing and resampling all live there).
 *
 * The producer is the child's audio capture callback, which libobs invokes on
 * the child's decode thread with the child's already-prepared PCM.
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

struct hdrp_audio {
	hdrp_mutex_t mtx;

	obs_source_t *parent; /* source that receives the audio, or NULL */
	obs_source_t *child;  /* currently captured child, or NULL */

	uint32_t rate;     /* mixer sample rate we declare to libobs */
	uint32_t channels; /* child's channel count (diagnostics) */
	uint64_t frames;   /* frames pushed since the last take (diagnostics) */
	uint64_t total;    /* frames pushed since the child was attached */
	bool log_once;
};

static uint32_t mixer_rate(void)
{
	audio_t *audio = obs_get_audio();
	uint32_t rate = audio ? audio_output_get_sample_rate(audio) : 48000;
	return rate ? rate : 48000;
}

/* ------------------------------------------------------------------ */
/* producer: child audio capture callback (decode thread)              */
/* ------------------------------------------------------------------ */

static void on_audio_capture(void *param, obs_source_t *source,
			     const struct audio_data *audio, bool muted)
{
	struct hdrp_audio *au = param;
	struct obs_source_audio out;
	obs_source_t *parent;
	enum speaker_layout layout;
	uint32_t channels, rate;
	bool log_now = false;
	size_t c;

	if (!au || !audio || !audio->frames || muted)
		return;

	/* struct audio_data carries no layout, so ask the child; media sources
	 * push at the mixer rate, which is what we declare below. */
	layout = obs_source_get_speaker_layout(source);
	channels = (uint32_t)get_audio_channels(layout);
	if (channels == 0 || channels > HDRP_CH_MAX) {
		/* Layout not reported (first packets): forward as stereo
		 * instead of dropping the audio. */
		layout = SPEAKERS_STEREO;
		channels = 2;
	}

	for (c = 0; c < channels; c++) {
		if (!audio->data[c])
			return;
	}

	hdrp_mutex_lock(&au->mtx);

	/* Ignore a callback that raced with a clip switch or a bind. */
	if (au->child != source || !au->parent) {
		hdrp_mutex_unlock(&au->mtx);
		return;
	}

	parent = au->parent;
	rate = au->rate ? au->rate : mixer_rate();
	au->rate = rate;
	au->channels = channels;
	au->frames += audio->frames;
	au->total += audio->frames;
	if (!au->log_once) {
		au->log_once = true;
		log_now = true;
	}
	hdrp_mutex_unlock(&au->mtx);

	memset(&out, 0, sizeof(out));
	for (c = 0; c < channels; c++)
		out.data[c] = audio->data[c];
	out.frames = audio->frames;
	out.speakers = layout;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.samples_per_sec = rate;
	out.timestamp = audio->timestamp;

	/* libobs copies the planes into the parent's audio buffer before this
	 * returns, so the child's memory is free to go afterwards. */
	obs_source_output_audio(parent, &out);

	if (log_now)
		blog(LOG_INFO,
		     "[HDR-PL] audio forwarding active: %u ch @ %u Hz from '%s'",
		     (unsigned)channels, (unsigned)rate,
		     obs_source_get_name(source));
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

struct hdrp_audio *hdrp_audio_create(void)
{
	struct hdrp_audio *au = bzalloc(sizeof(*au));
	hdrp_mutex_init(&au->mtx);
	return au;
}

void hdrp_audio_bind(struct hdrp_audio *au, obs_source_t *parent)
{
	if (!au)
		return;
	hdrp_mutex_lock(&au->mtx);
	au->parent = parent;
	au->rate = mixer_rate();
	hdrp_mutex_unlock(&au->mtx);
}

void hdrp_audio_attach(struct hdrp_audio *au, obs_source_t *child)
{
	obs_source_t *old;

	if (!au || !child)
		return;

	/* Swap the target under our lock, but NEVER call into libobs while
	 * holding it: the capture callback runs with the child's
	 * audio_cb_mutex held and takes this same lock, so registering from
	 * inside the critical section is a classic AB-BA deadlock that wedges
	 * the OBS audio thread (the symptom is total silence while video keeps
	 * playing). */
	hdrp_mutex_lock(&au->mtx);
	if (au->child == child) {
		hdrp_mutex_unlock(&au->mtx);
		return;
	}
	old = au->child;
	au->child = child;
	au->rate = mixer_rate();
	au->channels = 0;
	au->frames = 0;
	au->total = 0;
	au->log_once = false;
	hdrp_mutex_unlock(&au->mtx);

	if (old)
		obs_source_remove_audio_capture_callback(old, on_audio_capture,
							 au);
	obs_source_add_audio_capture_callback(child, on_audio_capture, au);

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
	au->channels = 0;
	au->frames = 0;
	hdrp_mutex_unlock(&au->mtx);

	if (child)
		obs_source_remove_audio_capture_callback(child,
							 on_audio_capture, au);
}

void hdrp_audio_flush(struct hdrp_audio *au)
{
	if (!au)
		return;
	hdrp_mutex_lock(&au->mtx);
	au->frames = 0;
	au->channels = 0;
	hdrp_mutex_unlock(&au->mtx);
}

void hdrp_audio_destroy(struct hdrp_audio *au)
{
	if (!au)
		return;
	hdrp_audio_detach(au);
	hdrp_mutex_destroy(&au->mtx);
	bfree(au);
}

/* Frames pushed since the previous call (the diagnostics heartbeat uses this
 * to show whether the child is producing audio at all). */
uint64_t hdrp_audio_take_frames(struct hdrp_audio *au)
{
	uint64_t n;

	if (!au)
		return 0;
	hdrp_mutex_lock(&au->mtx);
	n = au->frames;
	au->frames = 0;
	hdrp_mutex_unlock(&au->mtx);
	return n;
}

uint32_t hdrp_audio_channels(struct hdrp_audio *au)
{
	uint32_t c;

	if (!au)
		return 0;
	hdrp_mutex_lock(&au->mtx);
	c = au->channels;
	hdrp_mutex_unlock(&au->mtx);
	return c;
}
