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

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

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
	if (!x->tex_a || !x->tex_b || !x->fade) {
		xfade_free(x);
		return false;
	}
	obs_enter_graphics();
	x->linear_tech = !!gs_effect_get_technique(x->effect, "FadeLinear");
	obs_leave_graphics();
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

	obs_source_t *slot[2];
	int active_idx;  /* presented slot, -1 = none */
	int preload_idx; /* parked slot, -1 = none */
	bool preload_enabled; /* second decoder allowed to exist */

	struct hdrp_switcher_opts opts;
	struct hdrp_switcher_opts preload_opts;
	char *active_path;
	char *preload_path;

	bool preload_pending; /* update issued, waiting to park */
	bool preload_ready;   /* parked on its first frame, paused */

	volatile bool end_requested; /* media_ended fired for the active slot */
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
	obs_source_add_active_child(sw->parent, sw->slot[idx]);
	return sw->slot[idx];
}

static void release_slot(struct hdrp_switcher *sw, int idx)
{
	if (idx < 0 || idx > 1 || !sw->slot[idx])
		return;
	if (sw->active_idx == idx)
		return; /* never destroy the slot we are presenting */

	obs_source_remove_active_child(sw->parent, sw->slot[idx]);
	signal_handler_t *sh = obs_source_get_signal_handler(sw->slot[idx]);
	signal_handler_disconnect(sh, "media_ended", on_media_ended, sw);
	obs_source_release(sw->slot[idx]);
	sw->slot[idx] = NULL;
	blog(LOG_INFO, "[HDR-PL] released idle decoder slot %d to save memory",
	     idx);
}

void hdrp_switcher_set_preload_enabled(struct hdrp_switcher *sw, bool enable)
{
	if (!sw)
		return;
	sw->preload_enabled = enable;
	if (enable)
		return;

	hdrp_switcher_drop_preload(sw);
	sw->xfade_active = false;
	sw->xfade_out_idx = -1;
	release_slot(sw, sw->active_idx == 1 ? 0 : 1);
}

static void on_media_ended(void *data, calldata_t *cd)
{
	struct hdrp_switcher *sw = data;
	obs_source_t *src = NULL;
	if (!sw || sw->active_idx < 0)
		return;
	calldata_get_ptr(cd, "source", &src);
	if (src == sw->slot[sw->active_idx])
		sw->end_requested = true;
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
	sw->parent = parent;
	sw->opaque = opaque;
	sw->ended_cb = cb;
	sw->active_idx = -1;
	sw->preload_idx = -1;
	sw->xfade_out_idx = -1;
	sw->xf_tex_fmt = GS_RGBA;
	sw->opts.hw_decode = true;
	sw->opts.speed_percent = 100;
	sw->xf_supported = xfade_load(&sw->xf);
	if (!sw->xf_supported)
		blog(LOG_INFO, "[HDR-PL] crossfade effect unavailable; "
			       "switching uses hard cuts");

	/* Only the presenting decoder is created up front. The second one is
	 * instantiated on the first preload and destroyed again when the owner
	 * disables preloading (low-memory mode). */
	sw->preload_enabled = true;
	ensure_slot(sw, 0);
	return sw;
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
	if (sw->xf_tex[0] || sw->xf_tex[1]) {
		obs_enter_graphics();
		xf_texrender_release(sw);
		obs_leave_graphics();
	}
	xfade_free(&sw->xf);
	for (int i = 0; i < 2; i++) {
		if (!sw->slot[i])
			continue;
		obs_source_remove_active_child(sw->parent, sw->slot[i]);
		signal_handler_t *sh = obs_source_get_signal_handler(sw->slot[i]);
		signal_handler_disconnect(sh, "media_ended", on_media_ended, sw);
		obs_source_release(sw->slot[i]);
		sw->slot[i] = NULL;
	}
	bfree(sw->active_path);
	bfree(sw->preload_path);
	bfree(sw);
}

/* ------------------------------------------------------------------ */
/* control                                                             */
/* ------------------------------------------------------------------ */

static void clear_preload(struct hdrp_switcher *sw)
{
	sw->preload_pending = false;
	sw->preload_ready = false;
	sw->preload_idx = -1;
	bfree(sw->preload_path);
	sw->preload_path = NULL;
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
	bfree(sw->active_path);
	sw->active_path = bstrdup(path);

	clear_preload(sw);
	sw->end_requested = false;
	sw->xfade_active = false;
	sw->xfade_out_idx = -1;
	return true;
}

bool hdrp_switcher_preload(struct hdrp_switcher *sw, const char *path,
			   const struct hdrp_switcher_opts *opts)
{
	int idle;
	obs_source_t *child;

	if (!sw || !path || !*path)
		return false;
	if (sw->xfade_active)
		return false;

	idle = 1 - (sw->active_idx >= 0 ? sw->active_idx : 0);
	child = ensure_slot(sw, idle);
	if (!child)
		return false;
	if (sw->preload_idx == idle && sw->preload_path &&
	    strcmp(sw->preload_path, path) == 0)
		return true;

	clear_preload(sw);
	child = sw->slot[idle];
	obs_source_media_stop(child);
	slot_update_settings(child, path, opts);

	sw->preload_idx = idle;
	sw->preload_path = bstrdup(path);
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
	sw->end_requested = false;
	sw->xfade_active = false;
	sw->xfade_out_idx = -1;
	bfree(sw->active_path);
	sw->active_path = NULL;
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

const char *hdrp_switcher_active_path(const struct hdrp_switcher *sw)
{
	return sw ? sw->active_path : NULL;
}

obs_source_t *hdrp_switcher_active_child(const struct hdrp_switcher *sw)
{
	if (!sw || sw->active_idx < 0)
		return NULL;
	return sw->slot[sw->active_idx];
}

obs_source_t *hdrp_switcher_slot(const struct hdrp_switcher *sw, int idx)
{
	if (!sw || idx < 0 || idx > 1)
		return NULL;
	return sw->slot[idx];
}

uint32_t hdrp_switcher_get_width(struct hdrp_switcher *sw)
{
	obs_source_t *c = hdrp_switcher_active_child(sw);
	return c ? obs_source_get_width(c) : 0;
}

uint32_t hdrp_switcher_get_height(struct hdrp_switcher *sw)
{
	obs_source_t *c = hdrp_switcher_active_child(sw);
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

int hdrp_switcher_get_transition_ms(const struct hdrp_switcher *sw)
{
	return sw ? sw->xfade_ms : 0;
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
	bfree(sw->active_path);
	sw->active_path = sw->preload_path;
	sw->preload_path = NULL;
	sw->preload_idx = -1;
	sw->preload_pending = false;
	sw->preload_ready = false;
	sw->end_requested = false;

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
				park_child(child);
			} else if (st == OBS_MEDIA_STATE_PAUSED) {
				sw->preload_pending = false;
				sw->preload_ready = true;
			} else if (st == OBS_MEDIA_STATE_ENDED ||
				   st == OBS_MEDIA_STATE_STOPPED ||
				   st == OBS_MEDIA_STATE_ERROR) {
				clear_preload(sw);
			}
		}
	}

	/* --- 2. react to the end of the active clip --- */
	if (sw->end_requested) {
		sw->end_requested = false;
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

static void render_child_to_tex(struct hdrp_switcher *sw, obs_source_t *child,
				int w, int h, int slot_tex,
				enum gs_color_space space)
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
	obs_source_video_render(child);
	gs_enable_blending(true);
	gs_texrender_end(sw->xf_tex[slot_tex]);
	sw->xf_tex_valid |= (1 << slot_tex);
}

void hdrp_switcher_render(struct hdrp_switcher *sw)
{
	obs_source_t *active;
	obs_source_t *outgoing = NULL;
	uint32_t w, h;
	int tw, th;
	float t;
	enum gs_color_space space;
	enum gs_color_format fmt;
	gs_texture_t *a;
	gs_texture_t *b;
	const bool previous = gs_framebuffer_srgb_enabled();
	bool hdr;

	if (!sw)
		return;

	/* Free the fade intermediates whenever they are not in use. */
	if (!sw->xfade_active)
		xf_texrender_release(sw);

	active = hdrp_switcher_active_child(sw);

	if (!sw->xfade_active || sw->xfade_out_idx < 0) {
		if (active)
			obs_source_video_render(active);
		return;
	}

	outgoing = sw->slot[sw->xfade_out_idx];
	if (!active || !outgoing) {
		if (active)
			obs_source_video_render(active);
		return;
	}

	w = obs_source_get_width(active);
	h = obs_source_get_height(active);
	if (!w || !h) {
		obs_source_video_render(active);
		return;
	}

	space = gs_get_color_space();
	hdr = space != GS_CS_SRGB;
	fmt = gs_get_format_from_space(space);

	xf_size((int)w, (int)h, &tw, &th);
	xf_texrender_ensure(sw, tw, th, fmt);

	render_child_to_tex(sw, outgoing, tw, th, 0, space);
	render_child_to_tex(sw, active, tw, th, 1, space);

	if ((sw->xf_tex_valid & 3) != 3) {
		sw->xfade_active = false;
		obs_source_video_render(active);
		return;
	}

	a = gs_texrender_get_texture(sw->xf_tex[0]);
	b = gs_texrender_get_texture(sw->xf_tex[1]);
	if (!a || !b) {
		sw->xfade_active = false;
		obs_source_video_render(active);
		return;
	}

	t = sw->xfade_ms > 0
		    ? (float)(sw->xfade_elapsed * 1000.0 / (double)sw->xfade_ms)
		    : 1.0f;
	if (t > 1.0f)
		t = 1.0f;

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
		gs_draw_sprite(NULL, 0, (uint32_t)w, (uint32_t)h);
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
