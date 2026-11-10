/*
 * switcher.c — A/B dual-slot media switcher (rewritten).
 *
 * Two persistent private `ffmpeg_source` children. The owner plays clip N on
 * the active slot and preloads N+1 onto the idle slot, where it is parked
 * (seek 0 + paused) holding its first frame. When the active clip ends the
 * idle slot is promoted and, if enabled, blended for a short crossfade.
 *
 * The plugin never touches pixels: video comes from OBS's own media pipeline,
 * which is what keeps P010 / PQ / HLG intact.
 *
 * Threading: public functions run on the OBS video/UI thread. The media_ended
 * signal arrives on a media thread and only sets a flag that tick() consumes.
 */

#include <stdio.h>
#include <string.h>

/* util.h must come before the obs headers on Windows (it pulls in windows.h
 * and defines our cross-platform mutex). */
#include "util.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include "playlist.h"
#include "switcher.h"

/* ------------------------------------------------------------------ */
/* crossfade                                                           */
/* ------------------------------------------------------------------ */

/* Hard cap on the fade intermediate. Two 4K RGBA16F buffers cost ~265 MB of
 * VRAM; for a 150-400 ms transition a 1080p intermediate is indistinguishable
 * and cannot bring the GPU (or OBS) down. */
#define HDRP_XF_MAX_W 1920
#define HDRP_XF_MAX_H 1080

struct xfade {
	gs_effect_t *effect;
	gs_eparam_t *tex_a;
	gs_eparam_t *tex_b;
	gs_eparam_t *fade;
	bool linear_tech;
};

static void xfade_free(struct xfade *x);

static bool xfade_load(struct xfade *x)
{
	char *error = NULL;
	char *file;

	memset(x, 0, sizeof(*x));
	file = obs_module_file("effects/crossfade.effect");
	if (!file) {
		blog(LOG_WARNING,
		     "[HDR-PL] effects/crossfade.effect is missing from the "
		     "plugin's data directory");
		return false;
	}
	/* Every gs_* call — including the parameter and technique lookups —
	 * must run inside the graphics context. The old code queried them
	 * after obs_leave_graphics(), which is invalid on most backends. */
	obs_enter_graphics();
	x->effect = gs_effect_create_from_file(file, &error);
	if (x->effect) {
		x->tex_a = gs_effect_get_param_by_name(x->effect, "tex_a");
		x->tex_b = gs_effect_get_param_by_name(x->effect, "tex_b");
		x->fade = gs_effect_get_param_by_name(x->effect, "fade_val");
		x->linear_tech =
			!!gs_effect_get_technique(x->effect, "FadeLinear");
	}
	obs_leave_graphics();

	if (!x->effect) {
		blog(LOG_WARNING, "[HDR-PL] failed to compile '%s': %s", file,
		     error ? error : "(no error reported)");
		bfree(error);
		bfree(file);
		return false;
	}
	bfree(error);
	bfree(file);
	if (!x->tex_a || !x->tex_b || !x->fade) {
		blog(LOG_WARNING, "[HDR-PL] crossfade effect is missing one of "
				  "tex_a/tex_b/fade_val");
		xfade_free(x);
		return false;
	}
	return true;
}

static void xfade_free(struct xfade *x)
{
	if (x->effect) {
		obs_enter_graphics();
		gs_effect_destroy(x->effect);
		obs_leave_graphics();
	}
	memset(x, 0, sizeof(*x));
}

/* ------------------------------------------------------------------ */
/* main struct                                                         */
/* ------------------------------------------------------------------ */

struct hdrp_switcher {
	obs_source_t *parent;
	void *opaque;
	void (*ended_cb)(void *opaque);

	/* Guards the two heap strings and release_pending[]. NEVER held while
	 * calling into libobs: a media/audio callback may already hold one of
	 * libobs' internal locks, so nesting the two would deadlock. */
	hdrp_mutex_t mtx;

	obs_source_t *slot[2];
	/* A control thread (properties/hotkeys) must not destroy a child that
	 * the video thread may still be rendering. It only sets this flag; the
	 * video thread performs the actual release at the start of a tick. */
	bool release_pending[2];
	bool ready; /* graphics effect + slot 0 created on the video thread */
	int active_idx;  /* presented slot, -1 = none */
	int preload_idx; /* parked slot, -1 = none */
	bool preload_enabled; /* second decoder allowed to exist */

	struct hdrp_switcher_opts opts;
	struct hdrp_switcher_opts preload_opts;
	char *active_path;
	char *preload_path;

	bool preload_pending; /* update issued, waiting to park */
	bool preload_ready;   /* parked on its first frame, paused */
	bool park_sent;       /* seek+pause already issued for this preload */
	int preload_ticks;    /* watchdog against a preload that never parks */

	/* Written by the media_ended signal (media thread) with
	 * HDRP_ATOMIC_SET(), consumed by the video thread. A plain bool or
	 * volatile is a data race in C; atomics also provide the memory
	 * barriers. This is the ONLY field the media thread may touch. */
	hdrp_atomic_t end_requested;
	bool promote_event;

	/* crossfade */
	struct xfade xf;
	bool xf_supported;
	int xfade_ms;
	bool xfade_active;
	double xfade_elapsed;
	int xfade_out_idx;
	int xf_tex_valid;
	int xf_tex_w;
	int xf_tex_h;
	enum gs_color_format xf_tex_fmt;
	gs_texrender_t *xf_tex[2];

	/* canvas adaptation */
	bool adaptive;
	uint32_t out_w;
	uint32_t out_h;

	int deinterlace_mode; /* enum obs_deinterlace_mode */
};

/* ------------------------------------------------------------------ */
/* slots                                                               */
/* ------------------------------------------------------------------ */

static void make_base_settings(obs_data_t *s)
{
	obs_data_set_string(s, "local_file", "");
	obs_data_set_bool(s, "is_local_file", true);
	obs_data_set_bool(s, "looping", false);
	obs_data_set_bool(s, "restart_on_activate", false);
	obs_data_set_bool(s, "close_when_inactive", false);
	obs_data_set_bool(s, "clear_on_media_end", false);
	obs_data_set_bool(s, "log_changes", false);
}

static void on_media_ended(void *data, calldata_t *cd);
void hdrp_switcher_drop_preload(struct hdrp_switcher *sw);

static obs_source_t *create_slot(obs_source_t *parent, int idx,
				 struct hdrp_switcher *sw)
{
	obs_data_t *s = obs_data_create();
	make_base_settings(s);

	char name[64];
	snprintf(name, sizeof(name), "HDR-PL slot %c (%s)",
		 idx == 0 ? 'A' : 'B', obs_source_get_name(parent));

	obs_source_t *child = obs_source_create_private("ffmpeg_source", name, s);
	obs_data_release(s);
	if (!child)
		return NULL;

	signal_handler_t *sh = obs_source_get_signal_handler(child);
	signal_handler_connect(sh, "media_ended", on_media_ended, sw);
	return child;
}

/* Slot 0 always exists; slot 1 only exists while preloading is enabled, so
 * gapless-off setups never pay for a second decoder (and its frame cache). */
static obs_source_t *ensure_slot(struct hdrp_switcher *sw, int idx)
{
	if (idx < 0 || idx > 1)
		return NULL;
	if (sw->slot[idx])
		return sw->slot[idx];
	if (idx == 1 && !sw->preload_enabled)
		return NULL;

	sw->slot[idx] = create_slot(sw->parent, idx, sw);
	if (!sw->slot[idx]) {
		blog(LOG_ERROR, "[HDR-PL] failed to create slot %d", idx);
		return NULL;
	}
	obs_source_set_deinterlace_mode(
		sw->slot[idx], (enum obs_deinterlace_mode)sw->deinterlace_mode);
	obs_source_add_active_child(sw->parent, sw->slot[idx]);
	return sw->slot[idx];
}

void hdrp_switcher_set_deinterlace(struct hdrp_switcher *sw, int mode)
{
	if (!sw)
		return;
	if (mode < OBS_DEINTERLACE_MODE_DISABLE ||
	    mode > OBS_DEINTERLACE_MODE_YADIF_2X)
		mode = OBS_DEINTERLACE_MODE_DISABLE;
	if (sw->deinterlace_mode == mode)
		return;
	sw->deinterlace_mode = mode;
	for (int i = 0; i < 2; i++) {
		if (sw->slot[i])
			obs_source_set_deinterlace_mode(
				sw->slot[i],
				(enum obs_deinterlace_mode)mode);
	}
	blog(LOG_INFO, "[HDR-PL] deinterlace mode set to %d", mode);
}


/* Called from control threads (properties/hotkeys/media controls). Destroying
 * a private child here is unsafe: the video thread may be inside
 * video_render() or enum_active_sources() with that exact pointer. Instead we
 * only mark the slot; drain_releases() does the destroy on the video thread.
 * The child pointer stays valid in the meantime, so readers cannot fault. */
static void release_slot(struct hdrp_switcher *sw, int idx)
{
	if (idx < 0 || idx > 1 || !sw->slot[idx])
		return;
	if (sw->active_idx == idx)
		return; /* never destroy the slot we are presenting */

	hdrp_mutex_lock(&sw->mtx);
	if (!sw->release_pending[idx]) {
		sw->release_pending[idx] = true;
		blog(LOG_INFO, "[HDR-PL] queued idle decoder slot %d for "
			       "release on the video thread", idx);
	}
	hdrp_mutex_unlock(&sw->mtx);
}

/* Runs at the start of video_tick(), and once from destroy() (libobs defers
 * destroy until it is not inside a tick, see obs_source_destroy_defer). */
static void drain_releases(struct hdrp_switcher *sw)
{
	for (int i = 0; i < 2; i++) {
		bool pending;
		obs_source_t *child = NULL;

		hdrp_mutex_lock(&sw->mtx);
		pending = sw->release_pending[i];
		sw->release_pending[i] = false;
		/* Detach the pointer under the SAME lock the enumerator uses
		 * to take strong references. Once slot[i] is NULL nobody can
		 * obtain a new reference to this child; any reference taken
		 * before this point keeps it alive until the enumerator is
		 * done with cb(). */
		if (pending && sw->active_idx != i) {
			child = sw->slot[i];
			sw->slot[i] = NULL;
		}
		hdrp_mutex_unlock(&sw->mtx);

		if (!child)
			continue;

		obs_source_remove_active_child(sw->parent, child);
		signal_handler_t *sh = obs_source_get_signal_handler(child);
		signal_handler_disconnect(sh, "media_ended", on_media_ended, sw);
		obs_source_release(child);
		blog(LOG_INFO, "[HDR-PL] released idle decoder slot %d", i);
	}
}

void hdrp_switcher_set_preload_enabled(struct hdrp_switcher *sw, bool enable)
{
	if (!sw)
		return;
	sw->preload_enabled = enable;
	if (enable) {
		/* A disable/enable within the same frame must not leave a
		 * stale destruction queued for the slot we are about to reuse. */
		hdrp_mutex_lock(&sw->mtx);
		sw->release_pending[0] = false;
		sw->release_pending[1] = false;
		hdrp_mutex_unlock(&sw->mtx);
		return;
	}

	hdrp_switcher_drop_preload(sw);
	sw->xfade_active = false;
	sw->xfade_out_idx = -1;
	release_slot(sw, sw->active_idx == 1 ? 0 : 1);
}

static void on_media_ended(void *data, calldata_t *cd)
{
	struct hdrp_switcher *sw = data;
	UNUSED_PARAMETER(cd);

	/* Media thread: touch NOTHING but the atomic flag. Reading active_idx
	 * or slot[] here used to race with promote()/stop() on the video
	 * thread. play()/stop()/promote() clear the flag, so a late signal
	 * from a clip that is no longer current is discarded as before. */
	if (sw)
		HDRP_ATOMIC_SET(&sw->end_requested, 1);
}

static void slot_update_settings(obs_source_t *child, const char *path,
				 const struct hdrp_switcher_opts *opts)
{
	obs_data_t *s = obs_source_get_settings(child);
	bool is_url;

	if (!s)
		return;

	is_url = hdrp_playlist_is_url(path);

	if (is_url) {
		/* Network stream (HLS/M3U8, RTMP, SRT, ...): ffmpeg_source
		 * switches to URL mode. A larger read buffer and a short
		 * reconnect delay keep live streams alive. */
		obs_data_set_bool(s, "is_local_file", false);
		obs_data_set_string(s, "input", path ? path : "");
		obs_data_set_string(s, "input_format",
				    (path && strstr(path, ".m3u8")) ? "m3u8"
								    : "");
		obs_data_set_int(s, "buffering_mb", 8);
		obs_data_set_int(s, "reconnect_delay_sec", 2);
		obs_data_set_bool(s, "seekable", false);
		obs_data_set_bool(s, "looping", false);
	} else {
		obs_data_set_bool(s, "is_local_file", true);
		obs_data_set_string(s, "local_file", path ? path : "");
		obs_data_set_string(s, "input", "");
		obs_data_set_string(s, "input_format", "");
	}

	obs_data_set_bool(s, "hw_decode", opts->hw_decode);
	obs_data_set_int(s, "speed_percent", opts->speed_percent);
	obs_data_set_int(s, "color_range", VIDEO_RANGE_DEFAULT);
	obs_source_update(child, s);
	obs_data_release(s);
}

static void park_child(obs_source_t *child)
{
	if (!child)
		return;
	obs_source_media_set_time(child, 0);
	obs_source_media_play_pause(child, true);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

struct hdrp_switcher *
hdrp_switcher_create(obs_source_t *parent, void *opaque,
		     void (*cb)(void *opaque))
{
	struct hdrp_switcher *sw = bzalloc(sizeof(*sw));
	hdrp_mutex_init(&sw->mtx);
	HDRP_ATOMIC_SET(&sw->end_requested, 0);
	sw->parent = parent;
	sw->opaque = opaque;
	sw->ended_cb = cb;
	sw->active_idx = -1;
	sw->preload_idx = -1;
	sw->xfade_out_idx = -1;
	sw->xf_tex_fmt = GS_RGBA;
	sw->opts.hw_decode = true;
	sw->opts.speed_percent = 100;
	sw->preload_enabled = true;

	/* create() runs on the UI thread. Loading a graphics effect and
	 * creating private media children here would enter the graphics
	 * context and mutate obs->data.sources while the video thread is
	 * rendering. Defer both to hdrp_switcher_ensure_ready() on the
	 * video thread. */
	return sw;
}

/* Video thread, at the top of every tick. Idempotent after the first call. */
void hdrp_switcher_ensure_ready(struct hdrp_switcher *sw)
{
	if (!sw || sw->ready)
		return;
	sw->ready = true;

	sw->xf_supported = xfade_load(&sw->xf);
	if (!sw->xf_supported)
		blog(LOG_INFO, "[HDR-PL] crossfade effect unavailable; "
			       "switching uses hard cuts");

	/* Only the presenting decoder is created up front; the second one is
	 * instantiated on the first preload. If obs-ffmpeg was not available
	 * yet, hdrp_switcher_play() retries ensure_slot() later. */
	ensure_slot(sw, 0);
}

static void xf_texrender_release(struct hdrp_switcher *sw)
{
	for (int i = 0; i < 2; i++) {
		if (sw->xf_tex[i]) {
			gs_texrender_destroy(sw->xf_tex[i]);
			sw->xf_tex[i] = NULL;
		}
	}
	sw->xf_tex_w = 0;
	sw->xf_tex_h = 0;
	sw->xf_tex_fmt = GS_RGBA;
	sw->xf_tex_valid = 0;
}

void hdrp_switcher_destroy(struct hdrp_switcher *sw)
{
	if (!sw)
		return;

	/* Flush any deferred child release before we tear down the mutex. */
	drain_releases(sw);

	if (sw->xf_tex[0] || sw->xf_tex[1]) {
		obs_enter_graphics();
		xf_texrender_release(sw);
		obs_leave_graphics();
	}
	xfade_free(&sw->xf);
	for (int i = 0; i < 2; i++) {
		obs_source_t *child;

		/* Detach under the same lock the enumerator uses. */
		hdrp_mutex_lock(&sw->mtx);
		child = sw->slot[i];
		sw->slot[i] = NULL;
		hdrp_mutex_unlock(&sw->mtx);
		if (!child)
			continue;

		obs_source_remove_active_child(sw->parent, child);
		signal_handler_t *sh = obs_source_get_signal_handler(child);
		signal_handler_disconnect(sh, "media_ended", on_media_ended, sw);
		obs_source_release(child);
	}
	bfree(sw->active_path);
	bfree(sw->preload_path);
	hdrp_mutex_destroy(&sw->mtx);
	bfree(sw);
}

/* ------------------------------------------------------------------ */
/* control                                                             */
/* ------------------------------------------------------------------ */

static void clear_preload(struct hdrp_switcher *sw)
{
	/* Called from both control and video threads. Keep the free/clear pair
	 * atomic with respect to any reader of preload_path. */
	hdrp_mutex_lock(&sw->mtx);
	sw->preload_pending = false;
	sw->preload_ready = false;
	sw->park_sent = false;
	sw->preload_ticks = 0;
	sw->preload_idx = -1;
	bfree(sw->preload_path);
	sw->preload_path = NULL;
	hdrp_mutex_unlock(&sw->mtx);
}

bool hdrp_switcher_play(struct hdrp_switcher *sw, const char *path,
			const struct hdrp_switcher_opts *opts)
{
	if (!sw || !path || !*path)
		return false;

	if (sw->active_idx < 0)
		sw->active_idx = 0;

	obs_source_t *child = ensure_slot(sw, sw->active_idx);
	if (!child)
		return false;

	obs_source_media_stop(child);

	slot_update_settings(child, path, opts);
	sw->opts = *opts;
	hdrp_mutex_lock(&sw->mtx);
	bfree(sw->active_path);
	sw->active_path = bstrdup(path);
	hdrp_mutex_unlock(&sw->mtx);

	clear_preload(sw);
	HDRP_ATOMIC_SET(&sw->end_requested, 0);
	sw->xfade_active = false;
	sw->xfade_out_idx = -1;
	return true;
}

bool hdrp_switcher_preload(struct hdrp_switcher *sw, const char *path,
			   const struct hdrp_switcher_opts *opts)
{
	int idle;
	obs_source_t *child;
	bool already;

	if (!sw || !path || !*path)
		return false;
	if (sw->xfade_active)
		return false;

	idle = 1 - (sw->active_idx >= 0 ? sw->active_idx : 0);
	child = ensure_slot(sw, idle);
	if (!child)
		return false;

	hdrp_mutex_lock(&sw->mtx);
	already = sw->preload_idx == idle && sw->preload_path &&
		  strcmp(sw->preload_path, path) == 0;
	hdrp_mutex_unlock(&sw->mtx);
	if (already)
		return true;

	clear_preload(sw);
	child = sw->slot[idle];
	obs_source_media_stop(child);
	slot_update_settings(child, path, opts);

	hdrp_mutex_lock(&sw->mtx);
	sw->preload_idx = idle;
	sw->preload_path = bstrdup(path);
	hdrp_mutex_unlock(&sw->mtx);
	sw->preload_opts = *opts;
	sw->preload_pending = true;
	return true;
}

void hdrp_switcher_drop_preload(struct hdrp_switcher *sw)
{
	if (!sw)
		return;
	if (sw->preload_idx >= 0 && sw->slot[sw->preload_idx])
		obs_source_media_stop(sw->slot[sw->preload_idx]);
	clear_preload(sw);
}

void hdrp_switcher_stop(struct hdrp_switcher *sw)
{
	if (!sw)
		return;
	for (int i = 0; i < 2; i++) {
		if (sw->slot[i])
			obs_source_media_stop(sw->slot[i]);
	}
	clear_preload(sw);
	sw->active_idx = -1;
	HDRP_ATOMIC_SET(&sw->end_requested, 0);
	sw->xfade_active = false;
	sw->xfade_out_idx = -1;
	hdrp_mutex_lock(&sw->mtx);
	bfree(sw->active_path);
	sw->active_path = NULL;
	hdrp_mutex_unlock(&sw->mtx);
}

void hdrp_switcher_unload(struct hdrp_switcher *sw)
{
	if (!sw)
		return;

	hdrp_switcher_stop(sw);

	/* Close the file instead of destroying the source object: what actually
	 * holds the memory is the media-playback object (demuxer, codec contexts
	 * and the decoded frame cache), and clearing "local_file" makes libobs
	 * free it. obs_source_update() is deferred by libobs to the next tick,
	 * so this stays safe no matter which thread calls us — destroying the
	 * source here would mutate obs->data.sources while it is being ticked. */
	for (int i = 0; i < 2; i++) {
		obs_data_t *s;

		if (!sw->slot[i])
			continue;
		s = obs_source_get_settings(sw->slot[i]);
		if (!s)
			continue;
		make_base_settings(s);
		obs_source_update(sw->slot[i], s);
		obs_data_release(s);
	}
	blog(LOG_INFO, "[HDR-PL] closed the decoders while idle (low-memory)");
}

void hdrp_switcher_set_paused(struct hdrp_switcher *sw, bool pause)
{
	if (!sw || sw->active_idx < 0)
		return;
	obs_source_t *child = sw->slot[sw->active_idx];
	if (child && obs_source_media_get_state(child) != OBS_MEDIA_STATE_STOPPED)
		obs_source_media_play_pause(child, pause);
}

bool hdrp_switcher_seek(struct hdrp_switcher *sw, int64_t ms)
{
	if (!sw || sw->active_idx < 0 || !sw->slot[sw->active_idx])
		return false;
	obs_source_media_set_time(sw->slot[sw->active_idx], ms);
	return true;
}

int64_t hdrp_switcher_get_time(struct hdrp_switcher *sw)
{
	if (!sw || sw->active_idx < 0 || !sw->slot[sw->active_idx])
		return 0;
	return obs_source_media_get_time(sw->slot[sw->active_idx]);
}

int64_t hdrp_switcher_get_duration(struct hdrp_switcher *sw)
{
	if (!sw || sw->active_idx < 0 || !sw->slot[sw->active_idx])
		return 0;
	return obs_source_media_get_duration(sw->slot[sw->active_idx]);
}

enum obs_media_state hdrp_switcher_get_state(struct hdrp_switcher *sw)
{
	if (!sw || sw->active_idx < 0 || !sw->slot[sw->active_idx])
		return OBS_MEDIA_STATE_STOPPED;
	return obs_source_media_get_state(sw->slot[sw->active_idx]);
}

/* Returns a bstrdup'd copy; the caller owns it and must bfree() it.
 * active_path is freed/rewired on the video thread while UI threads read it,
 * so exposing the raw pointer was a use-after-free. */
char *hdrp_switcher_active_path_dup(struct hdrp_switcher *sw)
{
	char *copy = NULL;

	if (!sw)
		return NULL;
	hdrp_mutex_lock(&sw->mtx);
	if (sw->active_path)
		copy = bstrdup(sw->active_path);
	hdrp_mutex_unlock(&sw->mtx);
	return copy;
}

obs_source_t *hdrp_switcher_active_child(const struct hdrp_switcher *sw)
{
	if (!sw || sw->active_idx < 0)
		return NULL;
	return sw->slot[sw->active_idx];
}

void hdrp_switcher_slot_refs(struct hdrp_switcher *sw, obs_source_t *out[2])
{
	obs_source_t *a, *b;

	if (!sw) {
		out[0] = NULL;
		out[1] = NULL;
		return;
	}
	/* Take strong references while holding the lock that drain_releases()
	 * uses to detach slots. obs_source_get_ref() is a plain atomic refcount
	 * increment and takes no libobs locks, so it cannot deadlock. The
	 * caller must obs_source_release() each non-NULL entry. */
	hdrp_mutex_lock(&sw->mtx);
	a = sw->slot[0];
	b = sw->slot[1];
	out[0] = a ? obs_source_get_ref(a) : NULL;
	out[1] = b ? obs_source_get_ref(b) : NULL;
	hdrp_mutex_unlock(&sw->mtx);
}

void hdrp_switcher_set_output_size(struct hdrp_switcher *sw, uint32_t w,
				   uint32_t h, bool adaptive)
{
	if (!sw)
		return;
	sw->adaptive = adaptive && w && h;
	sw->out_w = w;
	sw->out_h = h;
}

uint32_t hdrp_switcher_get_width(struct hdrp_switcher *sw)
{
	obs_source_t *c = hdrp_switcher_active_child(sw);
	if (sw && sw->adaptive && sw->out_w)
		return sw->out_w;
	return c ? obs_source_get_width(c) : 0;
}

uint32_t hdrp_switcher_get_height(struct hdrp_switcher *sw)
{
	obs_source_t *c = hdrp_switcher_active_child(sw);
	if (sw && sw->adaptive && sw->out_h)
		return sw->out_h;
	return c ? obs_source_get_height(c) : 0;
}

bool hdrp_switcher_consume_promote_event(struct hdrp_switcher *sw)
{
	bool e;
	if (!sw)
		return false;
	e = sw->promote_event;
	sw->promote_event = false;
	return e;
}

bool hdrp_switcher_is_transitioning(const struct hdrp_switcher *sw)
{
	return sw ? sw->xfade_active : false;
}

void hdrp_switcher_set_transition_ms(struct hdrp_switcher *sw, int ms)
{
	if (sw)
		sw->xfade_ms = ms > 0 ? ms : 0;
}

void hdrp_switcher_get_media_size(struct hdrp_switcher *sw, uint32_t *w,
				  uint32_t *h)
{
	obs_source_t *child = hdrp_switcher_active_child(sw);

	if (w)
		*w = child ? obs_source_get_width(child) : 0;
	if (h)
		*h = child ? obs_source_get_height(child) : 0;
}

void hdrp_switcher_idle_stop_if_playing(struct hdrp_switcher *sw)
{
	obs_source_t *idle;
	enum obs_media_state st;

	if (!sw || sw->active_idx < 0 || sw->preload_idx >= 0)
		return;
	idle = sw->slot[1 - sw->active_idx];
	if (!idle)
		return;
	st = obs_source_media_get_state(idle);
	if (st == OBS_MEDIA_STATE_PLAYING || st == OBS_MEDIA_STATE_PAUSED)
		obs_source_media_stop(idle);
}

/* ------------------------------------------------------------------ */
/* promote / per-frame state machine                                   */
/* ------------------------------------------------------------------ */

static void promote(struct hdrp_switcher *sw)
{
	int new_idx = sw->preload_idx;
	obs_source_t *incoming;
	obs_source_t *outgoing;
	enum obs_media_state ost;
	bool do_xf;

	if (new_idx < 0 || !sw->slot[new_idx])
		return;

	incoming = sw->slot[new_idx];
	outgoing = sw->active_idx >= 0 ? sw->slot[sw->active_idx] : NULL;
	do_xf = outgoing && outgoing != incoming && sw->xfade_ms > 0 &&
		sw->xf_supported;

	if (outgoing && outgoing != incoming) {
		ost = obs_source_media_get_state(outgoing);
		if (ost == OBS_MEDIA_STATE_PLAYING ||
		    ost == OBS_MEDIA_STATE_PAUSED) {
			/* Keep the last frame alive for the fade; a hard cut
			 * releases it right away. */
			if (do_xf)
				obs_source_media_play_pause(outgoing, true);
			else
				obs_source_media_stop(outgoing);
		}
	}

	sw->active_idx = new_idx;
	sw->opts = sw->preload_opts;
	/* Hand the string over atomically: a control thread calling
	 * active_path_dup() must never observe a freed or half-swapped pair. */
	hdrp_mutex_lock(&sw->mtx);
	bfree(sw->active_path);
	sw->active_path = sw->preload_path;
	sw->preload_path = NULL;
	sw->preload_idx = -1;
	sw->preload_pending = false;
	sw->preload_ready = false;
	sw->park_sent = false;
	sw->preload_ticks = 0;
	hdrp_mutex_unlock(&sw->mtx);
	HDRP_ATOMIC_SET(&sw->end_requested, 0);

	if (do_xf) {
		sw->xfade_active = true;
		sw->xfade_elapsed = 0.0;
		sw->xfade_out_idx = 1 - new_idx;
		sw->xf_tex_valid = 0;
	} else {
		sw->xfade_active = false;
		sw->xfade_out_idx = -1;
	}

	if (obs_source_media_get_state(incoming) == OBS_MEDIA_STATE_PAUSED)
		obs_source_media_play_pause(incoming, false);

	sw->promote_event = true;
}

void hdrp_switcher_tick(struct hdrp_switcher *sw, float seconds)
{
	obs_source_t *child;
	enum obs_media_state st;

	if (!sw)
		return;

	/* Perform any child destruction queued by a control thread. From here
	 * on this frame no other thread can invalidate slot[] pointers. */
	drain_releases(sw);

	/* Note: the children do NOT need (and must not) be ticked by us.
	 * libobs' tick_sources() walks every source in obs->data.sources —
	 * private ones included — and calls obs_source_video_tick() on each,
	 * which also runs the deferred obs_source_update() that makes a new
	 * "local_file" take effect. obs_source_video_tick() is internal API
	 * and not available to plugins. */

	/* --- 1. finish parking a preload --- */
	if (sw->preload_pending && sw->preload_idx >= 0) {
		child = sw->slot[sw->preload_idx];
		if (!child) {
			clear_preload(sw);
		} else {
			st = obs_source_media_get_state(child);
			if (st == OBS_MEDIA_STATE_PLAYING) {
				/* Issue seek+pause exactly once: doing it every
				 * frame keeps the decoder in a seek storm and
				 * drags the whole session down. */
				if (!sw->park_sent) {
					sw->park_sent = true;
					park_child(child);
				}
			} else if (st == OBS_MEDIA_STATE_PAUSED) {
				sw->preload_pending = false;
				sw->park_sent = false;
				sw->preload_ticks = 0;
				sw->preload_ready = true;
			} else if (st == OBS_MEDIA_STATE_ENDED ||
				   st == OBS_MEDIA_STATE_STOPPED ||
				   st == OBS_MEDIA_STATE_ERROR) {
				clear_preload(sw);
			}

			/* Watchdog: give up on a preload that never parks
			 * (~3 s), otherwise it would block preloading for the
			 * rest of the session. */
			if (sw->preload_pending &&
			    ++sw->preload_ticks > 180) {
				blog(LOG_WARNING,
				     "[HDR-PL] preload did not park in time; "
				     "dropping it");
				clear_preload(sw);
			}
		}
	}

	/* --- 2. react to the end of the active clip --- */
	/* HDRP_ATOMIC_SET() returns the previous value, so this is an atomic
	 * test-and-clear. No torn read / lost wakeup against the media thread. */
	if (HDRP_ATOMIC_SET(&sw->end_requested, 0)) {
		if (sw->preload_ready) {
			promote(sw);
		} else if (sw->ended_cb && sw->opaque) {
			sw->ended_cb(sw->opaque); /* owner advances or stops */
		}
	}

	/* --- 3. crossfade timer --- */
	if (sw->xfade_active) {
		sw->xfade_elapsed += (double)seconds;
		if (sw->xfade_elapsed * 1000.0 >= (double)sw->xfade_ms) {
			sw->xfade_active = false;
			sw->xf_tex_valid = 0;
			if (sw->xfade_out_idx >= 0 &&
			    sw->xfade_out_idx != sw->active_idx &&
			    sw->slot[sw->xfade_out_idx]) {
				obs_source_media_stop(sw->slot[sw->xfade_out_idx]);
			}
			sw->xfade_out_idx = -1;
		}
	}
}

/* ------------------------------------------------------------------ */
/* rendering                                                           */
/* ------------------------------------------------------------------ */

static void xf_texrender_ensure(struct hdrp_switcher *sw, int w, int h,
				enum gs_color_format fmt)
{
	if (w == sw->xf_tex_w && h == sw->xf_tex_h &&
	    fmt == sw->xf_tex_fmt && sw->xf_tex[0] && sw->xf_tex[1])
		return;
	xf_texrender_release(sw);
	for (int i = 0; i < 2; i++)
		sw->xf_tex[i] = gs_texrender_create(fmt, GS_ZS_NONE);
	sw->xf_tex_w = w;
	sw->xf_tex_h = h;
	sw->xf_tex_fmt = fmt;
}

static void xf_size(int w, int h, int *out_w, int *out_h)
{
	double scale = 1.0;
	if (w > HDRP_XF_MAX_W)
		scale = (double)HDRP_XF_MAX_W / (double)w;
	if (h > HDRP_XF_MAX_H) {
		double s = (double)HDRP_XF_MAX_H / (double)h;
		if (s < scale)
			scale = s;
	}
	*out_w = (int)((double)w * scale + 0.5);
	*out_h = (int)((double)h * scale + 0.5);
	if (*out_w < 1)
		*out_w = 1;
	if (*out_h < 1)
		*out_h = 1;
}

/* Draws `child` into the intermediate, scaled/positioned by (scale, ox, oy)
 * which have already been mapped into the intermediate's pixel space. */
static void render_child_to_tex(struct hdrp_switcher *sw, obs_source_t *child,
				int w, int h, int slot_tex,
				enum gs_color_space space, float scale,
				float ox, float oy)
{
	struct vec4 clear;

	if (!child || slot_tex < 0 || slot_tex > 1 || !sw->xf_tex[slot_tex])
		return;
	gs_texrender_reset(sw->xf_tex[slot_tex]);
	if (!gs_texrender_begin_with_color_space(sw->xf_tex[slot_tex], w, h,
						 space))
		return;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	gs_enable_blending(false);
	gs_matrix_push();
	gs_matrix_translate3f(ox, oy, 0.0f);
	gs_matrix_scale3f(scale, scale, 1.0f);
	obs_source_video_render(child);
	gs_matrix_pop();
	gs_enable_blending(true);
	gs_texrender_end(sw->xf_tex[slot_tex]);
	sw->xf_tex_valid |= (1 << slot_tex);
}

/* Aspect-fit the source into the output rectangle. */
static void fit_into(uint32_t cw, uint32_t ch, uint32_t ow, uint32_t oh,
		     float *scale, float *ox, float *oy)
{
	float ax, ay;

	if (!cw || !ch) {
		*scale = 1.0f;
		*ox = 0.0f;
		*oy = 0.0f;
		return;
	}
	ax = (float)ow / (float)cw;
	ay = (float)oh / (float)ch;
	*scale = ax < ay ? ax : ay;
	*ox = ((float)ow - (float)cw * (*scale)) * 0.5f;
	*oy = ((float)oh - (float)ch * (*scale)) * 0.5f;
}

static void render_single(obs_source_t *child, float scale, float ox, float oy)
{
	gs_matrix_push();
	gs_matrix_translate3f(ox, oy, 0.0f);
	gs_matrix_scale3f(scale, scale, 1.0f);
	obs_source_video_render(child);
	gs_matrix_pop();
}

void hdrp_switcher_render(struct hdrp_switcher *sw)
{
	obs_source_t *active;
	obs_source_t *outgoing;
	uint32_t cw, ch, ow, oh;
	int tw, th;
	float scale, ox, oy, is, ix, iy;
	float os_s, oox, ooy;
	float t;
	enum gs_color_space space;
	enum gs_color_format fmt;
	gs_texture_t *a;
	gs_texture_t *b;
	bool hdr;
	bool previous;

	if (!sw)
		return;

	/* Free the fade intermediates whenever they are not in use. */
	if (!sw->xfade_active)
		xf_texrender_release(sw);

	active = hdrp_switcher_active_child(sw);
	if (!active)
		return;

	cw = obs_source_get_width(active);
	ch = obs_source_get_height(active);

	/* Output rectangle: the OBS base canvas when adaptive, else native. */
	ow = (sw->adaptive && sw->out_w) ? sw->out_w : cw;
	oh = (sw->adaptive && sw->out_h) ? sw->out_h : ch;
	if (!ow || !oh)
		return;

	if (!cw || !ch) {
		/* The clip's media has not opened yet, so it reports 0x0.
		 * Drawing it now would show its first frame at a bogus scale;
		 * stay empty for the one or two frames this lasts. */
		return;
	}

	fit_into(cw, ch, ow, oh, &scale, &ox, &oy);

	if (!sw->xfade_active || sw->xfade_out_idx < 0) {
		render_single(active, scale, ox, oy);
		return;
	}

	outgoing = (sw->xfade_out_idx >= 0) ? sw->slot[sw->xfade_out_idx] : NULL;
	if (!outgoing || outgoing == active || !obs_source_get_width(outgoing) ||
	    !obs_source_get_height(outgoing)) {
		sw->xfade_active = false;
		sw->xfade_out_idx = -1;
		render_single(active, scale, ox, oy);
		return;
	}

	space = gs_get_color_space();
	hdr = space != GS_CS_SRGB;
	fmt = gs_get_format_from_space(space);

	/* Intermediate at the output aspect, capped (see HDRP_XF_MAX_*). */
	xf_size((int)ow, (int)oh, &tw, &th);
	xf_texrender_ensure(sw, tw, th, fmt);

	/* Map the output-space fit into intermediate pixels, one fit per child:
	 * the outgoing clip usually has a different resolution than the
	 * incoming one, and sharing a single fit made the last frame of the
	 * old clip show up at the wrong scale for the whole fade. */
	is = scale * ((float)tw / (float)ow);
	ix = ox * ((float)tw / (float)ow);
	iy = oy * ((float)th / (float)oh);

	fit_into(obs_source_get_width(outgoing), obs_source_get_height(outgoing),
		 ow, oh, &os_s, &oox, &ooy);
	os_s *= (float)tw / (float)ow;
	oox *= (float)tw / (float)ow;
	ooy *= (float)th / (float)oh;

	render_child_to_tex(sw, outgoing, tw, th, 0, space, os_s, oox, ooy);
	render_child_to_tex(sw, active, tw, th, 1, space, is, ix, iy);

	if ((sw->xf_tex_valid & 3) != 3) {
		sw->xfade_active = false;
		render_single(active, scale, ox, oy);
		return;
	}

	a = gs_texrender_get_texture(sw->xf_tex[0]);
	b = gs_texrender_get_texture(sw->xf_tex[1]);
	if (!a || !b) {
		sw->xfade_active = false;
		render_single(active, scale, ox, oy);
		return;
	}

	t = sw->xfade_ms > 0
		    ? (float)(sw->xfade_elapsed * 1000.0 / (double)sw->xfade_ms)
		    : 1.0f;
	if (t > 1.0f)
		t = 1.0f;

	previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	if (hdr) {
		gs_effect_set_texture_srgb(sw->xf.tex_a, a);
		gs_effect_set_texture_srgb(sw->xf.tex_b, b);
	} else {
		gs_effect_set_texture(sw->xf.tex_a, a);
		gs_effect_set_texture(sw->xf.tex_b, b);
	}
	gs_effect_set_float(sw->xf.fade, t);
	while (gs_effect_loop(sw->xf.effect,
			      (hdr && sw->xf.linear_tech) ? "FadeLinear"
							  : "Fade"))
		gs_draw_sprite(NULL, 0, ow, oh);
	gs_enable_framebuffer_srgb(previous);
}

/* ------------------------------------------------------------------ */
/* color space                                                         */
/* ------------------------------------------------------------------ */

enum gs_color_space
hdrp_switcher_content_space(struct hdrp_switcher *sw, size_t count,
			    const enum gs_color_space *preferred)
{
	obs_source_t *child = hdrp_switcher_active_child(sw);
	static const enum gs_color_space cands[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
		GS_CS_709_SCRGB,
	};
	enum gs_color_space c;
	enum gs_color_space other;
	enum gs_color_space pref[2];

	if (!child)
		return (count > 0 && preferred) ? preferred[0] : GS_CS_SRGB;

	/* libobs returns the *last* preferred space for async sources when
	 * none matches, so a plain query cannot tell us whether the media is
	 * HDR. Probing with [C, X] returns C iff the child's real space is C. */
	for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
		c = cands[i];
		other = (c == GS_CS_SRGB) ? GS_CS_709_EXTENDED : GS_CS_SRGB;
		pref[0] = c;
		pref[1] = other;
		if (obs_source_get_color_space(child, 2, pref) == c)
			return c;
	}
	return GS_CS_SRGB;
}
