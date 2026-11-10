/*
 * audio.c — pushes the presented clip's PCM to the parent source with
 * obs_source_output_audio(), i.e. through libobs' own async-audio pipeline
 * (buffering, placement, timestamp smoothing and resampling all live there).
 *
 * The producer is the child's audio capture callback, which libobs invokes on
 * the child's decode thread with the child's already-prepared PCM. libobs
 * holds the child's audio_cb_mutex while running the callback and
 * obs_source_remove_audio_capture_callback() takes that same mutex, so a
 * removal waits for an in-flight callback to finish.
 *
 * Lifetime: `au` is refcounted. Each registered capture callback owns one
 * reference; hdrp_audio_destroy() marks the object dying, unregisters and
 * drops the creator's reference. The memory (and both mutexes) is freed only
 * once no callback can still be registered or be in flight.
 *
 * Lock order:
 *   reg_mtx -> mtx -> libobs (audio_cb_mutex)
 *   callback: libobs (audio_cb_mutex) -> mtx        [never takes reg_mtx]
 * reg_mtx therefore cannot form a cycle with libobs' callback lock.
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
	hdrp_mutex_t mtx;     /* child, parent_weak, counters */
	hdrp_mutex_t reg_mtx; /* serialises attach/detach sequences */

	hdrp_atomic_t refs;
	hdrp_atomic_t dying;

	obs_weak_source_t *parent_weak; /* source that receives the audio */
	obs_source_t *child;            /* currently captured child, or NULL */

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

static void au_addref(struct hdrp_audio *au)
{
	HDRP_ATOMIC_INC(&au->refs);
}

static void au_release(struct hdrp_audio *au)
{
	/* HDRP_ATOMIC_DEC() returns the *new* value. */
	if (HDRP_ATOMIC_DEC(&au->refs) == 0) {
		if (au->parent_weak)
			obs_weak_source_release(au->parent_weak);
		hdrp_mutex_destroy(&au->reg_mtx);
		hdrp_mutex_destroy(&au->mtx);
		bfree(au);
	}
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
	obs_weak_source_t *weak;
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

	/* Ignore a callback that raced with a clip switch, a bind or destroy. */
	if (HDRP_ATOMIC_LOAD(&au->dying) || au->child != source ||
	    !au->parent_weak) {
		hdrp_mutex_unlock(&au->mtx);
		return;
	}

	weak = au->parent_weak;
	obs_weak_source_addref(weak);
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

	/* The weak reference is held until refs reaches zero, which cannot
	 * happen while this callback is in flight. */
	parent = obs_weak_source_get_source(weak);
	obs_weak_source_release(weak);
	if (!parent)
		return; /* parent source already destroyed */

	memset(&out, 0, sizeof(out));
	/* libobs copies the planes with the *mixer's* channel count (see
	 * source_output_audio_push_back()), so every plane up to
	 * MAX_AUDIO_CHANNELS must be a valid pointer — a mono track would
	 * otherwise make libobs memcpy from NULL. Duplicate the last real plane
	 * for the extra channels; `speakers` still describes the real layout, so
	 * libobs' resampler interprets them correctly. */
	for (c = 0; c < HDRP_CH_MAX && c < MAX_AUDIO_CHANNELS; c++)
		out.data[c] = audio->data[c < channels ? c : channels - 1];
	out.frames = audio->frames;
	out.speakers = layout;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.samples_per_sec = rate;
	out.timestamp = audio->timestamp;

	/* libobs copies the planes into the parent's audio buffer before this
	 * returns, so the child's memory is free to go afterwards. */
	obs_source_output_audio(parent, &out);
	obs_source_release(parent);

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
	hdrp_mutex_init(&au->reg_mtx);
	HDRP_ATOMIC_SET(&au->refs, 1);
	HDRP_ATOMIC_SET(&au->dying, 0);
	return au;
}

void hdrp_audio_bind(struct hdrp_audio *au, obs_source_t *parent)
{
	obs_weak_source_t *weak;
	obs_weak_source_t *old;

	if (!au)
		return;
	weak = parent ? obs_source_get_weak_source(parent) : NULL;
	hdrp_mutex_lock(&au->mtx);
	old = au->parent_weak;
	au->parent_weak = weak;
	au->rate = mixer_rate();
	hdrp_mutex_unlock(&au->mtx);
	if (old)
		obs_weak_source_release(old);
}

void hdrp_audio_attach(struct hdrp_audio *au, obs_source_t *child)
{
	obs_source_t *old;

	if (!au || !child)
		return;

	/* Serialise the entire remove+add sequence against detach()/destroy().
	 * The old code moved these libobs calls outside au->mtx to avoid a real
	 * AB-BA deadlock (callback holds audio_cb_mutex then takes au->mtx),
	 * but that let a concurrent detach() remove nothing while attach()
	 * re-registered a callback for an object about to be freed. The
	 * callback never takes reg_mtx, so this lock cannot close that cycle. */
	hdrp_mutex_lock(&au->reg_mtx);

	hdrp_mutex_lock(&au->mtx);
	if (HDRP_ATOMIC_LOAD(&au->dying) || au->child == child) {
		hdrp_mutex_unlock(&au->mtx);
		hdrp_mutex_unlock(&au->reg_mtx);
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

	if (old) {
		/* The reference previously owned by `old` now belongs to
		 * `child`; the total number of registrations stays at one. */
		obs_source_remove_audio_capture_callback(old, on_audio_capture,
							 au);
	} else {
		au_addref(au);
	}
	obs_source_add_audio_capture_callback(child, on_audio_capture, au);

	hdrp_mutex_unlock(&au->reg_mtx);

	blog(LOG_INFO, "[HDR-PL] audio capture attached to '%s'",
	     obs_source_get_name(child));
}

void hdrp_audio_detach(struct hdrp_audio *au)
{
	obs_source_t *child;

	if (!au)
		return;

	hdrp_mutex_lock(&au->reg_mtx);
	hdrp_mutex_lock(&au->mtx);
	child = au->child;
	au->child = NULL;
	au->channels = 0;
	au->frames = 0;
	hdrp_mutex_unlock(&au->mtx);

	if (child) {
		/* libobs holds audio_cb_mutex while invoking the callback, so
		 * this removal also waits for any in-flight callback. */
		obs_source_remove_audio_capture_callback(child,
							 on_audio_capture, au);
		au_release(au);
	}
	hdrp_mutex_unlock(&au->reg_mtx);
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

	/* No new registrations from here on; detach() waits for the one
	 * currently in progress. */
	HDRP_ATOMIC_SET(&au->dying, 1);
	hdrp_audio_detach(au);
	/* Drop the creator's reference. If a callback is still registered the
	 * object stays alive; otherwise it is freed right here. */
	au_release(au);
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
