/*
 * switcher.c — A/B dual-slot media switcher.
 *
 * Two persistent private `ffmpeg_source` children are kept alive. The owner
 * plays clip N on the active slot and preloads N+1 onto the idle slot, where
 * it is parked (seek 0 + paused) holding its first frame. When the active
 * clip ends the idle slot is promoted: we unpause it (it resumes from its
 * first frame) and, if an HDR-safe crossfade is available, blend the two
 * slots for a short period. Otherwise we hard-cut on the first available
 * frame — never tone-mapping HDR content down as a side effect.
 *
 * Threading: called from the OBS UI/video thread. The media_ended signal
 * arrives on an OBS media thread; the handler only sets a volatile flag that
 * tick() consumes on the video thread.
 */

#include <stdio.h>
#include <string.h>

#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include "switcher.h"

/* ------------------------------------------------------------------ */
/* internal helpers                                                    */
/* ------------------------------------------------------------------ */

static const char *hdrp_slot_name(int idx)
{
	return idx == 0 ? "HDR-PL slot A" : "HDR-PL slot B";
}

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

static obs_source_t *create_slot(obs_source_t *parent, int idx,
				 void (*sig_cb)(void *, calldata_t *),
				 struct hdrp_switcher *sw)
{
	obs_data_t *s = obs_data_create();
	make_base_settings(s);

	char name[64];
	snprintf(name, sizeof(name), "%s (%s)", hdrp_slot_name(idx),
		 obs_source_get_name(parent));
	obs_source_t *child = obs_source_create_private("ffmpeg_source", name, s);
	obs_data_release(s);
	if (!child)
		return NULL;

	signal_handler_t *sh = obs_source_get_signal_handler(child);
	signal_handler_connect(sh, "media_ended", sig_cb, sw);
	return child;
}

/* Park a playing child: seek to the first frame and pause it.  Implemented as
 * a no-op when the child is not playing yet; tick() retries until parked. */
static void park_child(obs_source_t *child)
{
	if (!child)
		return;
	obs_source_media_set_time(child, 0);
	obs_source_media_play_pause(child, true);
}

static void slot_update_settings(obs_source_t *child, const char *path,
				 const struct hdrp_switcher_opts *opts)
{
	obs_data_t *s = obs_source_get_settings(child);
	if (!s)
		return;
	obs_data_set_string(s, "local_file", path ? path : "");
	obs_data_set_bool(s, "hw_decode", opts->hw_decode);
	obs_data_set_int(s, "speed_percent", opts->speed_percent);
	obs_data_set_int(s, "color_range", VIDEO_RANGE_DEFAULT);
	obs_source_update(child, s);
	obs_data_release(s);
}

/* ------------------------------------------------------------------ */
/* effect (crossfade)                                                  */
/* ------------------------------------------------------------------ */

struct xfade {
	gs_effect_t *effect;
	gs_eparam_t *tex_a;
	gs_eparam_t *tex_b;
	gs_eparam_t *fade;
};

static bool xfade_load(struct xfade *x)
{
	memset(x, 0, sizeof(*x));
	char *file = obs_module_file("effects/crossfade.effect");
	if (!file)
		return false;
	obs_enter_graphics();
	x->effect = gs_effect_create_from_file(file, NULL);
	obs_leave_graphics();
	bfree(file);
	if (!x->effect)
		return false;
	x->tex_a = gs_effect_get_param_by_name(x->effect, "tex_a");
	x->tex_b = gs_effect_get_param_by_name(x->effect, "tex_b");
	x->fade = gs_effect_get_param_by_name(x->effect, "fade_val");
	return x->tex_a && x->tex_b && x->fade;
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
/* main struct & lifecycle                                             */
/* ------------------------------------------------------------------ */

struct hdrp_switcher {
	obs_source_t *parent;
	void *opaque;
	void (*ended_cb)(void *opaque);

	obs_source_t *slot[2];
	int active_idx;  /* currently displayed/presented slot, -1 = none */
	int preload_idx; /* slot being parked, -1 = none */

	struct hdrp_switcher_opts opts;       /* options of the active clip */
	struct hdrp_switcher_opts preload_opts; /* options of the parked clip */
	char *active_path;
	char *preload_path;

	bool preloading;        /* update issued, waiting to park */
	bool preload_ready;     /* parked on first frame, paused */
	bool preload_failed;

	volatile bool end_requested; /* media_ended fired for active slot */
	bool promote_pending;        /* owner asked to switch ASAP */
	bool promote_event;          /* one-shot notification for the audio owner */

	/* crossfade */
	struct xfade xf;
	int xfade_ms;
	bool xfade_active;
	bool xf_supported;      /* effect file loaded */
	double xfade_elapsed;   /* seconds into the crossfade */
	int xf_tex_valid;
	int xf_tex_w;           /* texrender size currently allocated */
	int xf_tex_h;
	gs_texrender_t *xf_tex[2]; /* [0]=outgoing(old) [1]=incoming(new) */
};

/* ------------------------------------------------------------------ */
/* signal handler                                                      */
/* ------------------------------------------------------------------ */

static void on_media_ended(void *data, calldata_t *cd)
{
	struct hdrp_switcher *sw = data;
	if (!sw || sw->active_idx < 0)
		return;
	obs_source_t *src = NULL;
	calldata_get_ptr(cd, "source", &src);
	if (src == sw->slot[sw->active_idx])
		sw->end_requested = true;
}

static void destroy_slot(obs_source_t *child)
{
	if (!child)
		return;
	obs_source_release(child);
}

struct hdrp_switcher *
hdrp_switcher_create(obs_source_t *parent, void *opaque,
		     void (*cb)(void *opaque))
{
	struct hdrp_switcher *sw = bzalloc(sizeof(*sw));
	sw->parent = parent;
	sw->opaque = opaque;
	sw->ended_cb = cb;
	sw->active_idx = -1;
	sw->preload_idx = -1;
	sw->opts.hw_decode = true;
	sw->opts.speed_percent = 100;
	sw->opts.clear_on_end = false;
	sw->xf_supported = xfade_load(&sw->xf);
	if (!sw->xf_supported)
		blog(LOG_INFO, "[HDR-PL] crossfade effect unavailable; "
			       "switching will use hard cuts");

	for (int i = 0; i < 2; i++) {
		sw->slot[i] = create_slot(parent, i, on_media_ended, sw);
		if (!sw->slot[i]) {
			blog(LOG_ERROR,
			     "[HDR-PL] failed to create ffmpeg_source slot %d",
			     i);
			continue;
		}
		/* Mark the child as an active child of the parent. libobs only
		 * ticks (and therefore uploads async textures for) sources
		 * that are active; without this the private slots would
		 * decode but never present video. */
		obs_source_add_active_child(parent, sw->slot[i]);
	}
	return sw;
}

void hdrp_switcher_destroy(struct hdrp_switcher *sw)
{
	if (!sw)
		return;
	if (sw->xf_tex[0]) {
		obs_enter_graphics();
		gs_texrender_destroy(sw->xf_tex[0]);
		gs_texrender_destroy(sw->xf_tex[1]);
		obs_leave_graphics();
	}
	xfade_free(&sw->xf);
	for (int i = 0; i < 2; i++) {
		if (sw->slot[i]) {
			obs_source_remove_active_child(sw->parent,
						       sw->slot[i]);
			signal_handler_t *sh =
				obs_source_get_signal_handler(sw->slot[i]);
			signal_handler_disconnect(sh, "media_ended",
						  on_media_ended, sw);
			destroy_slot(sw->slot[i]);
		}
	}
	bfree(sw->active_path);
	bfree(sw->preload_path);
	bfree(sw);
}

/* ------------------------------------------------------------------ */
/* crossfade rendering                                                 */
/* ------------------------------------------------------------------ */

static void xf_texrender_ensure(struct hdrp_switcher *sw, int w, int h)
{
	if (w == sw->xf_tex_w && h == sw->xf_tex_h && sw->xf_tex[0] &&
	    sw->xf_tex[1])
		return;
	for (int i = 0; i < 2; i++) {
		if (sw->xf_tex[i]) {
			gs_texrender_destroy(sw->xf_tex[i]);
			sw->xf_tex[i] = NULL;
		}
		sw->xf_tex[i] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	}
	sw->xf_tex_w = w;
	sw->xf_tex_h = h;
}

static void render_child_to_tex(struct hdrp_switcher *sw, obs_source_t *child,
				int w, int h, int slot_tex)
{
	if (!child || slot_tex < 0 || slot_tex > 1)
		return;
	if (!sw->xf_tex[slot_tex])
		return;
	gs_texrender_begin(sw->xf_tex[slot_tex], w, h);
	obs_source_video_render(child);
	gs_texrender_end(sw->xf_tex[slot_tex]);
	sw->xf_tex_valid |= (1 << slot_tex);
}

/* Returns false when either side could not be captured (caller falls back to
 * a direct render of the incoming clip). */
static bool draw_xfade(struct hdrp_switcher *sw, obs_source_t *outgoing,
		       obs_source_t *incoming, float t, int w, int h)
{
	render_child_to_tex(sw, outgoing, w, h, 0);
	render_child_to_tex(sw, incoming, w, h, 1);

	if ((sw->xf_tex_valid & 3) != 3)
		return false;

	gs_texture_t *a = gs_texrender_get_texture(sw->xf_tex[0]);
	gs_texture_t *b = gs_texrender_get_texture(sw->xf_tex[1]);
	if (!a || !b)
		return false;

	/* Same technique as OBS's built-in fade transition: sample non-linear
	 * and linearize on output to an sRGB framebuffer. On an HDR canvas we
	 * never get here (do_promote refuses the crossfade) so PQ values are
	 * never clamped by the 8-bit intermediate. */
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture(sw->xf.tex_a, a);
	gs_effect_set_texture(sw->xf.tex_b, b);
	gs_effect_set_float(sw->xf.fade, t);
	while (gs_effect_loop(sw->xf.effect, "Fade"))
		gs_draw_sprite(NULL, 0, w, h);
	gs_enable_framebuffer_srgb(previous);
	return true;
}

/* A crossfade is only safe on a plain sRGB canvas: the intermediate texrender
 * is 8-bit and would clamp PQ/wide-gamut values on an HDR canvas. Be
 * conservative and treat every non-sRGB canvas (PQ/HLG/709-EXTENDED/…) as
 * HDR so we never crush highlights. */
static bool canvas_is_hdr(void)
{
	return gs_get_color_space() != GS_CS_SRGB;
}

/* ------------------------------------------------------------------ */
/* control                                                             */
/* ------------------------------------------------------------------ */

static void clear_preload_state(struct hdrp_switcher *sw)
{
	sw->preloading = false;
	sw->preload_ready = false;
	sw->preload_failed = false;
	sw->preload_idx = -1;
	bfree(sw->preload_path);
	sw->preload_path = NULL;
}

static void begin_play_on_slot(struct hdrp_switcher *sw, obs_source_t *child,
			       const char *path,
			       const struct hdrp_switcher_opts *opts)
{
	slot_update_settings(child, path, opts);
	sw->opts = *opts;
	bfree(sw->active_path);
	sw->active_path = bstrdup(path);
}

bool hdrp_switcher_play(struct hdrp_switcher *sw, const char *path,
			const struct hdrp_switcher_opts *opts)
{
	if (!sw || !path || !*path)
		return false;

	/* After stop() there is no active slot yet; take slot 0. */
	if (sw->active_idx < 0)
		sw->active_idx = 0;
	if (!sw->slot[sw->active_idx])
		return false;

	/* Stop whatever may still be running on the active slot. */
	obs_source_t *child = sw->slot[sw->active_idx];
	obs_source_media_stop(child);

	begin_play_on_slot(sw, child, path, opts);
	clear_preload_state(sw);
	sw->end_requested = false;
	sw->promote_pending = false;
	sw->xfade_active = false;
	sw->xf_tex_valid = 0;

	/* restart_on_activate=false on the slot means updating `local_file`
	 * makes ffmpeg_source open *and* start the media thread for us. */
	return true;
}

bool hdrp_switcher_preload(struct hdrp_switcher *sw, const char *path,
			   const struct hdrp_switcher_opts *opts)
{
	if (!sw || !path || !*path)
		return false;

	/* Choose the idle slot. */
	int idle = 1 - (sw->active_idx >= 0 ? sw->active_idx : 0);
	if (sw->preload_idx == idle && sw->preload_path &&
	    strcmp(sw->preload_path, path) == 0 && sw->preload_ready)
		return true; /* already parked with this file */

	if (sw->xfade_active)
		return false; /* wait until the fade completes */

	clear_preload_state(sw);
	sw->preload_idx = idle;
	sw->preload_path = bstrdup(path);
	sw->preload_opts = *opts;

	obs_source_t *child = sw->slot[idle];
	obs_source_media_stop(child);

	/* Open the file and start decoding; tick() parks it on frame 0. */
	slot_update_settings(child, path, opts);
	sw->preloading = true;
	sw->preload_ready = false;
	sw->preload_failed = false;
	return true;
}

void hdrp_switcher_stop(struct hdrp_switcher *sw)
{
	if (!sw)
		return;
	for (int i = 0; i < 2; i++) {
		if (sw->slot[i])
			obs_source_media_stop(sw->slot[i]);
	}
	clear_preload_state(sw);
	sw->active_idx = -1;
	sw->promote_pending = false;
	sw->end_requested = false;
	sw->xfade_active = false;
	bfree(sw->active_path);
	sw->active_path = NULL;
}

void hdrp_switcher_set_paused(struct hdrp_switcher *sw, bool pause)
{
	if (!sw || sw->active_idx < 0)
		return;
	obs_source_t *child = sw->slot[sw->active_idx];
	if (child) {
		if (obs_source_media_get_state(child) != OBS_MEDIA_STATE_STOPPED)
			obs_source_media_play_pause(child, pause);
	}
}

bool hdrp_switcher_seek(struct hdrp_switcher *sw, int64_t ms)
{
	if (!sw || sw->active_idx < 0)
		return false;
	obs_source_t *child = sw->slot[sw->active_idx];
	if (!child)
		return false;
	obs_source_media_set_time(child, ms);
	return true;
}

int64_t hdrp_switcher_get_time(struct hdrp_switcher *sw)
{
	if (!sw || sw->active_idx < 0)
		return 0;
	obs_source_t *child = sw->slot[sw->active_idx];
	return child ? obs_source_media_get_time(child) : 0;
}

int64_t hdrp_switcher_get_duration(struct hdrp_switcher *sw)
{
	if (!sw || sw->active_idx < 0)
		return 0;
	obs_source_t *child = sw->slot[sw->active_idx];
	return child ? obs_source_media_get_duration(child) : 0;
}

enum obs_media_state hdrp_switcher_get_state(struct hdrp_switcher *sw)
{
	if (!sw || sw->active_idx < 0)
		return OBS_MEDIA_STATE_STOPPED;
	obs_source_t *child = sw->slot[sw->active_idx];
	if (!child)
		return OBS_MEDIA_STATE_STOPPED;
	return obs_source_media_get_state(child);
}

const char *hdrp_switcher_active_path(const struct hdrp_switcher *sw)
{
	return sw ? sw->active_path : NULL;
}

void hdrp_switcher_set_transition_ms(struct hdrp_switcher *sw, int ms)
{
	if (!sw)
		return;
	sw->xfade_ms = ms > 0 ? ms : 0;
}

int hdrp_switcher_get_transition_ms(const struct hdrp_switcher *sw)
{
	return sw ? sw->xfade_ms : 0;
}

bool hdrp_switcher_consume_promote_event(struct hdrp_switcher *sw)
{
	if (!sw)
		return false;
	bool e = sw->promote_event;
	sw->promote_event = false;
	return e;
}

bool hdrp_switcher_is_transitioning(const struct hdrp_switcher *sw)
{
	return sw ? sw->xfade_active : false;
}

obs_source_t *
hdrp_switcher_active_child(const struct hdrp_switcher *sw)
{
	if (!sw || sw->active_idx < 0)
		return NULL;
	return sw->slot[sw->active_idx];
}

obs_source_t *hdrp_switcher_idle_child(const struct hdrp_switcher *sw)
{
	if (!sw || sw->active_idx < 0)
		return NULL;
	return sw->slot[1 - sw->active_idx];
}

/* Release the inactive slot when low-memory mode is on and nothing is
 * parked there. */
void hdrp_switcher_idle_stop_if_playing(struct hdrp_switcher *sw)
{
	if (!sw || sw->active_idx < 0 || sw->preload_idx >= 0)
		return;
	obs_source_t *idle = sw->slot[1 - sw->active_idx];
	if (!idle)
		return;
	enum obs_media_state st = obs_source_media_get_state(idle);
	if (st == OBS_MEDIA_STATE_PLAYING ||
	    st == OBS_MEDIA_STATE_PAUSED)
		obs_source_media_stop(idle);
}

/* Force a switch now (manual next/previous, or any seek to a specific file).
 * If a matching preload is ready it is used (no gap); otherwise this stops the
 * current clip and starts `path` on the active slot. */
void hdrp_switcher_force_advance(struct hdrp_switcher *sw, const char *path,
				 const struct hdrp_switcher_opts *opts)
{
	if (!sw || !path)
		return;

	if (sw->preload_ready && sw->preload_path &&
	    strcmp(sw->preload_path, path) == 0) {
		/* same clip already parked -> promote immediately */
		sw->promote_pending = true;
		return;
	}
	/* Hard switch (accept a brief gap for manual jumps). */
	clear_preload_state(sw);
	hdrp_switcher_play(sw, path, opts);
	if (sw->ended_cb && sw->opaque)
		sw->ended_cb(sw->opaque);
}

/* ------------------------------------------------------------------ */
/* per-frame logic                                                     */
/* ------------------------------------------------------------------ */

/* Promote the parked slot to be the active slot. */
static void do_promote(struct hdrp_switcher *sw)
{
	int new_idx = sw->preload_idx;
	obs_source_t *incoming = new_idx >= 0 ? sw->slot[new_idx] : NULL;
	if (!incoming)
		return;

	obs_source_t *outgoing =
		sw->active_idx >= 0 ? sw->slot[sw->active_idx] : NULL;
	int old_idx = sw->active_idx;

	/* Hand the previous path over for the crossfade snapshot. */
	if (outgoing && old_idx >= 0 &&
	    sw->xfade_ms > 0 && sw->xf_supported &&
	    canvas_is_hdr() == false) {
		sw->xfade_active = true;
		sw->xfade_elapsed = 0.0;
		sw->xf_tex_valid = 0;
	} else {
		sw->xfade_active = false;
	}

	/* Stop the previous clip only when it is still actively playing (a
	 * manual jump). If it ended naturally its last frame is kept for the
	 * crossfade; it is released once the fade completes (see tick()). */
	if (outgoing && old_idx >= 0 && old_idx != new_idx) {
		enum obs_media_state ost = obs_source_media_get_state(outgoing);
		if (ost == OBS_MEDIA_STATE_PLAYING ||
		    ost == OBS_MEDIA_STATE_PAUSED)
			obs_source_media_stop(outgoing);
	}

	/* Resume incoming from its parked first frame. */
	sw->active_idx = new_idx;
	sw->opts = sw->preload_opts;
	bfree(sw->active_path);
	sw->active_path = sw->preload_path;
	sw->preload_path = NULL;

	if (obs_source_media_get_state(incoming) == OBS_MEDIA_STATE_PAUSED)
		obs_source_media_play_pause(incoming, false);

	clear_preload_state(sw);
	sw->end_requested = false;
	sw->promote_pending = false;
	sw->promote_event = true;
}

void hdrp_switcher_tick(struct hdrp_switcher *sw, float seconds)
{
	if (!sw)
		return;

	/* --- finish parking a preload clip --- */
	if (sw->preloading && !sw->preload_ready) {
		obs_source_t *child =
			sw->preload_idx >= 0 ? sw->slot[sw->preload_idx] : NULL;
		if (!child) {
			clear_preload_state(sw);
		} else {
			enum obs_media_state st = obs_source_media_get_state(child);
			if (st == OBS_MEDIA_STATE_PLAYING) {
				/* decode running: park now */
				park_child(child);
			} else if (st == OBS_MEDIA_STATE_PAUSED) {
				sw->preloading = false;
				sw->preload_ready = true;
			} else if (st == OBS_MEDIA_STATE_ENDED ||
				   st == OBS_MEDIA_STATE_STOPPED ||
				   st == OBS_MEDIA_STATE_ERROR) {
				sw->preloading = false;
				sw->preload_failed = true;
			}
			/* OPENING/BUFFERING: wait for the next frame */
		}
	}

	/* --- handle an explicit/forced switch request --- */
	if (sw->promote_pending) {
		if (sw->preload_ready) {
			do_promote(sw);
			if (sw->ended_cb && sw->opaque)
				sw->ended_cb(sw->opaque);
		} else if (sw->preload_failed) {
			clear_preload_state(sw);
			sw->promote_pending = false;
		}
		/* not ready yet: keep waiting */
	}

	/* --- natural end of the active clip --- */
	if (sw->end_requested) {
		sw->end_requested = false;
		if (sw->preload_ready) {
			do_promote(sw);
			if (sw->ended_cb && sw->opaque)
				sw->ended_cb(sw->opaque);
		} else {
			/* nothing parked -> ask the owner to advance (gap) */
			if (sw->ended_cb && sw->opaque)
				sw->ended_cb(sw->opaque);
		}
	}

	/* --- advance crossfade timer --- */
	if (sw->xfade_active) {
		sw->xfade_elapsed += (double)seconds;
		if (sw->xfade_elapsed * 1000.0 >= (double)sw->xfade_ms) {
			sw->xfade_active = false;
			sw->xf_tex_valid = 0;
			/* Release the retired (ended) clip that we kept alive
			 * for the fade. */
			if (sw->active_idx >= 0) {
				int idle = 1 - sw->active_idx;
				obs_source_t *idle_src = sw->slot[idle];
				if (idle_src && !sw->preloading &&
				    obs_source_media_get_state(idle_src) ==
					    OBS_MEDIA_STATE_ENDED)
					obs_source_media_stop(idle_src);
			}
		}
	}
}

/* Owner hooks into switcher tick()/render via this single entry. */
void hdrp_switcher_render(struct hdrp_switcher *sw)
{
	if (!sw)
		return;

	obs_source_t *active =
		sw->active_idx >= 0 ? sw->slot[sw->active_idx] : NULL;
	obs_source_t *outgoing = NULL;
	if (sw->xfade_active && sw->active_idx >= 0) {
		int old_idx = 1 - sw->active_idx;
		obs_source_t *idle = sw->slot[old_idx];
		if (idle && !sw->preload_ready && !sw->preloading)
			outgoing = idle;
	}

	if (!sw->xfade_active || !outgoing) {
		if (active)
			obs_source_video_render(active);
		return;
	}

	/* Crossfade outgoing -> active over transition_ms. */
	uint32_t w = obs_source_get_width(outgoing);
	uint32_t h = obs_source_get_height(outgoing);
	if (w == 0 || h == 0) {
		if (active)
			obs_source_video_render(active);
		return;
	}

	xf_texrender_ensure(sw, (int)w, (int)h);

	float t = 0.0f;
	if (sw->xfade_ms > 0)
		t = (float)(sw->xfade_elapsed * 1000.0 /
			    (double)sw->xfade_ms);
	if (t > 1.0f)
		t = 1.0f;

	if (!draw_xfade(sw, outgoing, active, t, (int)w, (int)h)) {
		/* Intermediates unavailable: stop the fade, hard-cut. */
		sw->xfade_active = false;
		sw->xf_tex_valid = 0;
		if (active)
			obs_source_video_render(active);
	}
}

enum gs_color_space
hdrp_switcher_get_color_space(struct hdrp_switcher *sw, size_t count,
			      const enum gs_color_space *preferred)
{
	obs_source_t *child = NULL;
	if (sw && sw->active_idx >= 0)
		child = sw->slot[sw->active_idx];
	if (child)
		return obs_source_get_color_space(child, count, preferred);
	if (count > 0 && preferred)
		return preferred[0];
	return GS_CS_SRGB;
}
