/*
 * hdr_playlist_source.c — "HDR Playlist Source" parent source (rewritten).
 *
 * Owns:
 *   * playlist model      (playlist.c)
 *   * A/B media switcher  (switcher.c)
 *   * audio ring buffer   (audio.c)
 *
 * The parent never touches pixels and never resamples audio: video comes from
 * private ffmpeg_source children through OBS's own HDR pipeline, and audio is
 * pulled from a small ring buffer by the `audio_render` callback.
 */

#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "audio.h"
#include "hdr_playlist_source.h"
#include "playlist.h"
#include "plugin-support.h"
#include "switcher.h"
#include "util.h"

/* ------------------------------------------------------------------ */
/* settings keys                                                       */
/* ------------------------------------------------------------------ */

#define KEY_FILES          "files"
#define KEY_MODE           "play_mode"
#define KEY_HW_DECODE      "hw_decode"
#define KEY_SPEED          "speed_percent"
#define KEY_USE_TRANSITION "use_transition"
#define KEY_TRANSITION_MS  "transition_ms"
#define KEY_LOW_MEMORY     "low_memory"
#define KEY_VISIBILITY     "visibility"
#define KEY_MIXED          "mixed_content"
#define KEY_SAVED_INDEX    "saved_index"
#define KEY_ADAPT_CANVAS   "adapt_canvas"
#define KEY_DEINTERLACE    "deinterlace"
#define KEY_ADD_FOLDER     "hdrp_add_folder"

enum {
	VIS_STOP_RESTART = 0,
	VIS_PAUSE_RESUME,
	VIS_ALWAYS_PLAY,
	VIS_STOP_NEXT,
};

enum {
	MIXED_AUTO = 0,
	MIXED_FORCE_PQ,
	MIXED_FORCE_SDR,
	MIXED_FORCE_HLG,
};

struct hdr_playlist {
	obs_source_t *source;

	struct hdrp_playlist *pl;
	struct hdrp_switcher *sw;
	struct hdrp_audio *au;

	hdrp_mutex_t mutex;

	bool hw_decode;
	int speed_percent;
	bool use_transition;
	int transition_ms;
	bool low_memory;
	int visibility;
	int mixed;
	bool adapt_canvas;
	int deinterlace_mode;

	/* cached OBS output configuration */
	bool output_hdr;
	uint32_t canvas_w;
	uint32_t canvas_h;
	bool cfg_logged;

	obs_hotkey_id hk_play_pause;
	obs_hotkey_id hk_restart;
	obs_hotkey_id hk_stop;
	obs_hotkey_id hk_next;
	obs_hotkey_id hk_prev;

	bool showing;
	bool stopped;
	bool need_preload_retry;
	float hb_seconds;    /* diagnostics heartbeat */
	int64_t hb_last_pos; /* position at the previous heartbeat (ms) */
	float props_seconds; /* throttles the properties refresh */
};

/* ------------------------------------------------------------------ */

static void hdrp_start_current(struct hdr_playlist *p);
static void hdrp_stop_playback(struct hdr_playlist *p);
static void hdrp_update(void *data, obs_data_t *settings);

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void hdrp_apply_opts(const struct hdr_playlist *p,
			    struct hdrp_switcher_opts *opts)
{
	opts->hw_decode = p->hw_decode;
	opts->speed_percent = p->speed_percent;
	opts->clear_on_end = false;
}

/* ------------------------------------------------------------------ */
/* properties panel status                                             */
/* ------------------------------------------------------------------ */

static void hdrp_build_now_playing(struct hdr_playlist *p, char *buf,
				   size_t size)
{
	const char *path;
	char *name;
	int64_t dur, pos;
	size_t idx;

	if (!p || p->stopped || !(path = hdrp_switcher_active_path(p->sw))) {
		snprintf(buf, size, "%s", obs_module_text("StatusIdle"));
		return;
	}

	name = hdrp_basename_dup(path);
	dur = hdrp_switcher_get_duration(p->sw);
	pos = hdrp_switcher_get_time(p->sw);
	idx = hdrp_playlist_has_current(p->pl)
		      ? hdrp_playlist_current_index(p->pl) + 1
		      : 0;

	if (dur > 0) {
		int pct = (int)((pos * 100) / dur);
		snprintf(buf, size, "%s  [%zu/%zu]  %llds / %llds  (%d%%)",
			 name, idx, hdrp_playlist_count(p->pl),
			 (long long)(pos / 1000), (long long)(dur / 1000), pct);
	} else {
		snprintf(buf, size, "%s  [%zu/%zu]  %llds  (%s)", name, idx,
			 hdrp_playlist_count(p->pl), (long long)(pos / 1000),
			 obs_module_text("StatusLive"));
	}
	bfree(name);
}

static void hdrp_build_session_info(struct hdr_playlist *p, char *buf,
				    size_t size)
{
	if (!p) {
		snprintf(buf, size, "-");
		return;
	}
	snprintf(buf, size, "%ux%u · %s · %s", p->canvas_w, p->canvas_h,
		 p->output_hdr ? "HDR" : "SDR",
		 p->low_memory ? "low-mem" : "gapless");
}

/* Queued onto the UI thread so we never emit the properties signal from the
 * graphics thread (and the weak ref keeps this safe if the source dies). */
static void hdrp_ui_refresh_task(void *param)
{
	obs_weak_source_t *weak = param;
	obs_source_t *src = obs_weak_source_get_source(weak);
	if (src) {
		obs_source_update_properties(src);
		obs_source_release(src);
	}
	obs_weak_source_release(weak);
}

static void hdrp_queue_props_refresh(struct hdr_playlist *p)
{
	obs_weak_source_t *w;

	if (!p || !p->source)
		return;
	w = obs_source_get_weak_source(p->source);
	if (w)
		obs_queue_task(OBS_TASK_UI, hdrp_ui_refresh_task, w, false);
}

/* Is OBS configured to *output* HDR? (Advanced -> Color Format P010/I010/...
 * together with Rec.2100 PQ/HLG.) Everything else is treated as SDR. */
static bool hdrp_output_is_hdr(const struct obs_video_info *ovi)
{
	switch (ovi->output_format) {
	case VIDEO_FORMAT_P010:
	case VIDEO_FORMAT_I010:
	case VIDEO_FORMAT_I210:
	case VIDEO_FORMAT_I412:
	case VIDEO_FORMAT_YA2L:
	case VIDEO_FORMAT_R10L:
		break;
	default:
		return false;
	}
	return ovi->colorspace == VIDEO_CS_2100_PQ ||
	       ovi->colorspace == VIDEO_CS_2100_HLG;
}

/* Reads the OBS canvas/output configuration and pushes it into the switcher:
 *   * the source reports (and renders at) the base canvas resolution, so the
 *     compositor stays 1:1 instead of scaling a native 4K frame every frame;
 *   * whether the session is HDR decides how color spaces are reported.
 * Called from update() and every tick; obs_get_video_info() is cheap. */
static void hdrp_sync_output_config(struct hdr_playlist *p)
{
	struct obs_video_info ovi;

	if (!p || !obs_get_video_info(&ovi))
		return;

	bool hdr = hdrp_output_is_hdr(&ovi);
	if (!p->cfg_logged || hdr != p->output_hdr ||
	    ovi.base_width != p->canvas_w ||
	    ovi.base_height != p->canvas_h) {
		p->output_hdr = hdr;
		p->canvas_w = ovi.base_width;
		p->canvas_h = ovi.base_height;
		blog(LOG_INFO,
		     "[HDR-PL] canvas %ux%u, output %ux%u, format=%d "
		     "colorspace=%d -> %s session",
		     ovi.base_width, ovi.base_height, ovi.output_width,
		     ovi.output_height, (int)ovi.output_format,
		     (int)ovi.colorspace, hdr ? "HDR" : "SDR");
		p->cfg_logged = true;
	}

	/* Always report the canvas size so the video adapts to any canvas; the
	 * adaptive flag only controls whether we override the native size. */
	hdrp_switcher_set_output_size(p->sw, p->canvas_w, p->canvas_h,
				      p->adapt_canvas);
}

/* Make the playlist cursor point at `path`; returns index or SIZE_MAX. */
static size_t cursor_to_path(struct hdr_playlist *p, const char *path)
{
	size_t n = hdrp_playlist_count(p->pl);
	for (size_t i = 0; i < n; i++) {
		const char *cur = hdrp_playlist_at(p->pl, i);
		if (cur && path && strcmp(cur, path) == 0) {
			hdrp_playlist_set_current(p->pl, i);
			return i;
		}
	}
	return SIZE_MAX;
}

static void load_files_from_settings(struct hdr_playlist *p,
				     obs_data_t *settings)
{
	char *keep = NULL;
	const char *cur = hdrp_playlist_current(p->pl);
	long long saved;

	if (cur)
		keep = bstrdup(cur);

	hdrp_playlist_clear(p->pl);

	obs_data_array_t *arr = obs_data_get_array(settings, KEY_FILES);
	obs_data_array_t *out = obs_data_array_create();
	bool expanded = false;

	if (arr) {
		size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			const char *path;
			if (!item)
				continue;
			path = obs_data_get_string(item, "value");
			if (path && *path) {
				obs_data_t *it;
				if (hdrp_playlist_path_is_dir(path)) {
					/* A folder was dropped (or typed) into
					 * the list: expand it into its media
					 * files so the entry is playable. */
					size_t before =
						hdrp_playlist_count(p->pl);
					size_t after;
					hdrp_playlist_add_folder(p->pl, path, 4);
					after = hdrp_playlist_count(p->pl);
					for (size_t k = before; k < after; k++) {
						it = obs_data_create();
						obs_data_set_string(
							it, "value",
							hdrp_playlist_at(
								p->pl, k));
						obs_data_array_push_back(
							out, it);
						obs_data_release(it);
					}
					expanded = true;
					blog(LOG_INFO,
					     "[HDR-PL] expanded folder '%s' "
					     "-> %zu file(s)",
					     path, after - before);
				} else {
					hdrp_playlist_add_file(p->pl, path);
					it = obs_data_create();
					obs_data_set_string(it, "value", path);
					obs_data_array_push_back(out, it);
					obs_data_release(it);
				}
			}
			obs_data_release(item);
		}
		obs_data_array_release(arr);
	}

	/* Write the expanded list back so the properties dialog shows the
	 * individual files instead of the folder entry. */
	if (expanded)
		obs_data_set_array(settings, KEY_FILES, out);
	obs_data_array_release(out);

	if (keep) {
		if (cursor_to_path(p, keep) == SIZE_MAX &&
		    hdrp_playlist_count(p->pl) > 0)
			hdrp_playlist_set_current(p->pl, 0);
		bfree(keep);
	}

	saved = obs_data_get_int(settings, KEY_SAVED_INDEX);
	if (saved >= 0 && (size_t)saved < hdrp_playlist_count(p->pl) &&
	    !hdrp_playlist_has_current(p->pl))
		hdrp_playlist_set_current(p->pl, (size_t)saved);
}

static bool files_list_changed(struct hdr_playlist *p, obs_data_t *settings)
{
	obs_data_array_t *arr = obs_data_get_array(settings, KEY_FILES);
	bool changed;
	size_t n;
	size_t i;

	if (!arr)
		return hdrp_playlist_count(p->pl) > 0;

	n = obs_data_array_count(arr);
	changed = n != hdrp_playlist_count(p->pl);
	for (i = 0; i < n && !changed; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		const char *path;
		const char *cur;
		if (!item)
			continue;
		path = obs_data_get_string(item, "value");
		cur = hdrp_playlist_at(p->pl, i);
		if (!cur || !path || strcmp(cur, path) != 0)
			changed = true;
		obs_data_release(item);
	}
	obs_data_array_release(arr);
	return changed;
}

/* ------------------------------------------------------------------ */
/* playback orchestration                                              */
/* ------------------------------------------------------------------ */

static void hdrp_attach_audio_to_active(struct hdr_playlist *p)
{
	obs_source_t *child = hdrp_switcher_active_child(p->sw);
	if (child) {
		hdrp_audio_flush(p->au);
		hdrp_audio_attach(p->au, child);
	}
}

static void hdrp_start_current(struct hdr_playlist *p)
{
	const char *path = hdrp_playlist_current(p->pl);
	struct hdrp_switcher_opts opts;

	if (!path && hdrp_playlist_count(p->pl) > 0) {
		hdrp_playlist_set_current(p->pl, 0);
		path = hdrp_playlist_current(p->pl);
	}
	if (!path)
		return;

	hdrp_apply_opts(p, &opts);

	{
		char *name = hdrp_basename_dup(path);
		blog(LOG_INFO, "[HDR-PL] playing '%s'", name);
		bfree(name);
	}

	p->stopped = false;
	p->hb_seconds = 0.0f;
	hdrp_switcher_play(p->sw, path, &opts);
	if (p->au)
		hdrp_attach_audio_to_active(p);

	obs_source_media_started(p->source);
	p->need_preload_retry = true;
	hdrp_queue_props_refresh(p);
}

static void hdrp_stop_playback(struct hdr_playlist *p)
{
	if (p->au) {
		hdrp_audio_detach(p->au);
		hdrp_audio_flush(p->au);
	}
	hdrp_switcher_stop(p->sw);
	p->stopped = true;
	hdrp_queue_props_refresh(p);
}

static void hdrp_finish_at_end(struct hdr_playlist *p)
{
	if (p->au) {
		hdrp_audio_detach(p->au);
		hdrp_audio_flush(p->au);
	}
	hdrp_switcher_stop(p->sw);
	p->stopped = true;
	p->need_preload_retry = false;
	obs_source_media_ended(p->source);
}

static void hdrp_try_preload_next(struct hdr_playlist *p)
{
	const char *next;
	struct hdrp_switcher_opts opts;

	if (p->low_memory || hdrp_playlist_count(p->pl) == 0)
		return;
	if (hdrp_switcher_is_transitioning(p->sw))
		return;

	next = hdrp_playlist_peek_next(p->pl);
	if (!next)
		return;

	hdrp_apply_opts(p, &opts);
	if (!hdrp_switcher_preload(p->sw, next, &opts))
		p->need_preload_retry = true;
}

/* Switcher asked us to move on: the active clip ended with nothing parked. */
static void hdrp_switcher_ended(void *opaque)
{
	struct hdr_playlist *p = opaque;
	const char *path;
	struct hdrp_switcher_opts opts;

	if (!p || p->stopped)
		return;

	path = hdrp_playlist_peek_next(p->pl);
	if (!path) {
		hdrp_finish_at_end(p);
		return;
	}

	hdrp_playlist_next(p->pl);
	path = hdrp_playlist_current(p->pl);
	if (!path) {
		hdrp_finish_at_end(p);
		return;
	}

	hdrp_apply_opts(p, &opts);
	hdrp_switcher_play(p->sw, path, &opts);
	hdrp_attach_audio_to_active(p);
	obs_source_media_started(p->source);
	p->need_preload_retry = true;
	hdrp_queue_props_refresh(p);
}

static void hdrp_jump(struct hdr_playlist *p, bool forward)
{
	const char *target;
	struct hdrp_switcher_opts opts;

	if (hdrp_playlist_count(p->pl) == 0)
		return;
	if (hdrp_playlist_count(p->pl) > 1) {
		const char *t = forward ? hdrp_playlist_next(p->pl)
					: hdrp_playlist_previous(p->pl);
		if (!t && forward)
			hdrp_playlist_set_current(p->pl, 0);
	}

	target = hdrp_playlist_current(p->pl);
	if (!target)
		return;

	if (p->stopped) {
		hdrp_start_current(p);
		return;
	}

	hdrp_apply_opts(p, &opts);
	hdrp_switcher_drop_preload(p->sw);
	hdrp_switcher_play(p->sw, target, &opts);
	hdrp_attach_audio_to_active(p);
	p->need_preload_retry = true;
	hdrp_queue_props_refresh(p);
}

/* The playlist was edited. Make it take effect immediately. */
static void hdrp_playlist_changed(struct hdr_playlist *p)
{
	const char *playing = hdrp_switcher_active_path(p->sw);

	if (!p->stopped && playing && cursor_to_path(p, playing) != SIZE_MAX) {
		/* The running clip survived the edit: keep it, but re-arm the
		 * preload so "next" follows the new order. */
		blog(LOG_INFO, "[HDR-PL] playlist updated; current clip kept");
		hdrp_switcher_drop_preload(p->sw);
		p->need_preload_retry = true;
		return;
	}

	if (p->stopped && !p->showing)
		return; /* nothing on screen; the next play uses the new list */

	blog(LOG_INFO, "[HDR-PL] playlist updated; switching to the new entry");
	hdrp_start_current(p);
}

/* ------------------------------------------------------------------ */
/* hotkeys                                                             */
/* ------------------------------------------------------------------ */

static void hotkey_play_pause(void *data, obs_hotkey_id id,
			      obs_hotkey_t *hotkey, bool pressed)
{
	struct hdr_playlist *p = data;
	enum obs_media_state st;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!p || !pressed)
		return;

	st = hdrp_switcher_get_state(p->sw);
	if (st == OBS_MEDIA_STATE_PLAYING)
		hdrp_switcher_set_paused(p->sw, true);
	else if (st == OBS_MEDIA_STATE_PAUSED)
		hdrp_switcher_set_paused(p->sw, false);
	else
		hdrp_start_current(p);
}

static void hotkey_restart(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			   bool pressed)
{
	struct hdr_playlist *p = data;
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (p && pressed)
		hdrp_start_current(p);
}

static void hotkey_stop(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			bool pressed)
{
	struct hdr_playlist *p = data;
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (p && pressed)
		hdrp_stop_playback(p);
}

static void hotkey_next(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			bool pressed)
{
	struct hdr_playlist *p = data;
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (p && pressed)
		hdrp_jump(p, true);
}

static void hotkey_prev(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			bool pressed)
{
	struct hdr_playlist *p = data;
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (p && pressed)
		hdrp_jump(p, false);
}

/* ------------------------------------------------------------------ */
/* source callbacks                                                    */
/* ------------------------------------------------------------------ */

static const char *hdrp_get_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("Name");
}

static void *hdrp_create(obs_data_t *settings, obs_source_t *source)
{
	struct hdr_playlist *p = bzalloc(sizeof(*p));

	p->source = source;
	p->pl = hdrp_playlist_create();
	p->sw = hdrp_switcher_create(source, p, hdrp_switcher_ended);
	p->au = hdrp_audio_create();

	hdrp_mutex_init(&p->mutex);

	p->hw_decode = true;
	p->speed_percent = 100;
	p->use_transition = false;
	p->transition_ms = 200;
	p->low_memory = true; /* memory-first default: one decoder, no preload */
	p->visibility = VIS_STOP_RESTART;
	p->mixed = MIXED_AUTO;
	p->adapt_canvas = true;
	p->stopped = true;

	hdrp_switcher_set_transition_ms(p->sw, 0);

	p->hk_play_pause = obs_hotkey_register_source(
		source, "HDR-Playlist-Source.PlayPause",
		obs_module_text("HotkeyPlayPause"), hotkey_play_pause, p);
	p->hk_restart = obs_hotkey_register_source(
		source, "HDR-Playlist-Source.Restart",
		obs_module_text("HotkeyRestart"), hotkey_restart, p);
	p->hk_stop = obs_hotkey_register_source(
		source, "HDR-Playlist-Source.Stop",
		obs_module_text("HotkeyStop"), hotkey_stop, p);
	p->hk_next = obs_hotkey_register_source(
		source, "HDR-Playlist-Source.Next",
		obs_module_text("HotkeyNext"), hotkey_next, p);
	p->hk_prev = obs_hotkey_register_source(
		source, "HDR-Playlist-Source.Prev",
		obs_module_text("HotkeyPrev"), hotkey_prev, p);

	hdrp_update(p, settings);
	hdrp_sync_output_config(p);
	return p;
}

static void hdrp_destroy(void *data)
{
	struct hdr_playlist *p = data;
	if (!p)
		return;

	hdrp_audio_destroy(p->au);
	hdrp_switcher_destroy(p->sw);
	hdrp_playlist_destroy(p->pl);
	hdrp_mutex_destroy(&p->mutex);
	bfree(p);
}

static void hdrp_update(void *data, obs_data_t *settings)
{
	struct hdr_playlist *p = data;
	bool list_changed = false;
	long long mode;
	int ms;

	if (!p)
		return;

	hdrp_mutex_lock(&p->mutex);
	mode = obs_data_get_int(settings, KEY_MODE);
	if (mode >= HDRP_MODE_SEQUENTIAL && mode <= HDRP_MODE_SHUFFLE)
		hdrp_playlist_set_mode(p->pl, (hdrp_play_mode)mode);
	p->hw_decode = obs_data_get_bool(settings, KEY_HW_DECODE);
	p->speed_percent = (int)obs_data_get_int(settings, KEY_SPEED);
	if (p->speed_percent < 1 || p->speed_percent > 200)
		p->speed_percent = 100;
	p->use_transition = obs_data_get_bool(settings, KEY_USE_TRANSITION);
	p->transition_ms = (int)obs_data_get_int(settings, KEY_TRANSITION_MS);
	if (p->transition_ms < 0)
		p->transition_ms = 0;
	p->low_memory = obs_data_get_bool(settings, KEY_LOW_MEMORY);
	p->visibility = (int)obs_data_get_int(settings, KEY_VISIBILITY);
	p->mixed = (int)obs_data_get_int(settings, KEY_MIXED);
	p->adapt_canvas = obs_data_get_bool(settings, KEY_ADAPT_CANVAS);
	p->deinterlace_mode = (int)obs_data_get_int(settings, KEY_DEINTERLACE);

	/* "Add folder…" picker: expand the chosen directory straight into the
	 * playlist settings, then clear the picker so it can be reused. */
	{
		const char *add_dir = obs_data_get_string(settings, KEY_ADD_FOLDER);
		if (add_dir && *add_dir) {
			obs_data_array_t *old = obs_data_get_array(settings,
								   KEY_FILES);
			obs_data_array_t *merged = obs_data_array_create();
			struct hdrp_playlist *tmp = hdrp_playlist_create();
			size_t added;

			if (old) {
				size_t n = obs_data_array_count(old);
				for (size_t i = 0; i < n; i++) {
					obs_data_t *it =
						obs_data_array_item(old, i);
					if (it) {
						obs_data_array_push_back(
							merged, it);
						obs_data_release(it);
					}
				}
				obs_data_array_release(old);
			}

			hdrp_playlist_add_folder(tmp, add_dir, 4);
			added = hdrp_playlist_count(tmp);
			for (size_t i = 0; i < added; i++) {
				obs_data_t *it = obs_data_create();
				obs_data_set_string(it, "value",
						    hdrp_playlist_at(tmp, i));
				obs_data_array_push_back(merged, it);
				obs_data_release(it);
			}
			hdrp_playlist_destroy(tmp);

			obs_data_set_array(settings, KEY_FILES, merged);
			obs_data_array_release(merged);
			obs_data_set_string(settings, KEY_ADD_FOLDER, "");
			blog(LOG_INFO,
			     "[HDR-PL] added %zu file(s) from folder '%s'",
			     added, add_dir);
		}
	}

	if (files_list_changed(p, settings)) {
		load_files_from_settings(p, settings);
		list_changed = true;
	}
	hdrp_mutex_unlock(&p->mutex);

	ms = (p->use_transition && !p->low_memory) ? p->transition_ms : 0;
	hdrp_switcher_set_transition_ms(p->sw, ms);

	/* Memory: without preloading the second decoder is destroyed and the
	 * plugin runs a single ffmpeg instance. */
	hdrp_switcher_set_preload_enabled(p->sw, !p->low_memory);
	hdrp_switcher_set_deinterlace(p->sw, p->deinterlace_mode);

	hdrp_sync_output_config(p);

	if (list_changed)
		hdrp_playlist_changed(p);
}

static void hdrp_save(void *data, obs_data_t *settings)
{
	struct hdr_playlist *p = data;
	if (!p)
		return;
	hdrp_mutex_lock(&p->mutex);
	obs_data_set_int(settings, KEY_SAVED_INDEX,
			 hdrp_playlist_has_current(p->pl)
				 ? (long long)hdrp_playlist_current_index(p->pl)
				 : -1);
	hdrp_mutex_unlock(&p->mutex);
}

static void hdrp_show(void *data)
{
	struct hdr_playlist *p = data;
	if (!p)
		return;
	p->showing = true;

	if (p->stopped && hdrp_playlist_count(p->pl) > 0) {
		if (p->visibility == VIS_STOP_NEXT)
			hdrp_jump(p, true);
		else
			hdrp_start_current(p);
		return;
	}

	if (p->visibility == VIS_PAUSE_RESUME && !p->stopped)
		hdrp_switcher_set_paused(p->sw, false);
}

static void hdrp_hide(void *data)
{
	struct hdr_playlist *p = data;
	if (!p)
		return;
	p->showing = false;

	if (p->visibility == VIS_ALWAYS_PLAY)
		return;
	if (p->visibility == VIS_PAUSE_RESUME) {
		hdrp_switcher_set_paused(p->sw, true);
		return;
	}
	hdrp_stop_playback(p);
}

static void hdrp_activate(void *data)
{
	struct hdr_playlist *p = data;
	if (!p)
		return;
	if (hdrp_playlist_count(p->pl) == 0 || !p->stopped)
		return;
	hdrp_start_current(p);
}

static void hdrp_video_tick(void *data, float seconds)
{
	struct hdr_playlist *p = data;
	if (!p)
		return;

	hdrp_sync_output_config(p);
	hdrp_switcher_tick(p->sw, seconds);

	if (hdrp_switcher_consume_promote_event(p->sw))
		hdrp_attach_audio_to_active(p);

	if (p->need_preload_retry && !p->low_memory && !p->stopped &&
	    !hdrp_switcher_is_transitioning(p->sw)) {
		p->need_preload_retry = false;
		hdrp_try_preload_next(p);
	}

	if (p->low_memory && !p->stopped)
		hdrp_switcher_idle_stop_if_playing(p->sw);

	/* Diagnostics: one line every 5 s. If "pos" stops advancing the decode
	 * stalled; if "fill" stays at the ring capacity the audio consumer is
	 * not running; "state" tells whether the media source is playing. */
	if (!p->stopped) {
		p->hb_seconds += seconds;
		if (p->hb_seconds >= 5.0f) {
			const char *path = hdrp_switcher_active_path(p->sw);
			char *name = path ? hdrp_basename_dup(path) : NULL;
			enum obs_media_state st = hdrp_switcher_get_state(p->sw);
			int64_t pos = hdrp_switcher_get_time(p->sw);
			bool stuck = st == OBS_MEDIA_STATE_PLAYING &&
				     pos == p->hb_last_pos;
			p->hb_seconds = 0.0f;
			p->hb_last_pos = pos;
			blog(stuck || st == OBS_MEDIA_STATE_ERROR
				     ? LOG_WARNING
				     : LOG_INFO,
			     "[HDR-PL] hb: '%s' state=%d pos=%.1fs dur=%.1fs "
			     "audio=%uch fill=%zu canvas=%ux%u %s%s",
			     name ? name : "(none)", (int)st,
			     (double)pos / 1000.0,
			     (double)hdrp_switcher_get_duration(p->sw) / 1000.0,
			     hdrp_audio_channels(p->au), hdrp_audio_fill(p->au),
			     p->canvas_w, p->canvas_h,
			     p->output_hdr ? "HDR" : "SDR",
			     stuck ? " [position not advancing - decode too "
				     "slow for this resolution]"
				   : "");
			bfree(name);
		}
	} else {
		p->hb_seconds = 0.0f;
		p->hb_last_pos = 0;
	}

	/* Keep the properties panel's progress readout ticking (1 Hz, and
	 * only while playing — the refresh is queued onto the UI thread). */
	if (!p->stopped) {
		p->props_seconds += seconds;
		if (p->props_seconds >= 1.0f) {
			p->props_seconds = 0.0f;
			hdrp_queue_props_refresh(p);
		}
	} else {
		p->props_seconds = 0.0f;
	}
}

static void hdrp_video_render(void *data, gs_effect_t *effect)
{
	struct hdr_playlist *p = data;
	UNUSED_PARAMETER(effect);
	if (p)
		hdrp_switcher_render(p->sw);
}

static uint32_t hdrp_get_width(void *data)
{
	struct hdr_playlist *p = data;
	return p ? hdrp_switcher_get_width(p->sw) : 0;
}

static uint32_t hdrp_get_height(void *data)
{
	struct hdr_playlist *p = data;
	return p ? hdrp_switcher_get_height(p->sw) : 0;
}

static bool hdrp_source_audio_render(void *data, uint64_t *ts_out,
				     struct obs_source_audio_mix *audio_output,
				     uint32_t mixers, size_t channels,
				     size_t sample_rate)
{
	struct hdr_playlist *p = data;
	if (!p || p->stopped)
		return false;
	return hdrp_audio_render(p->au, ts_out, audio_output, mixers, channels,
				 sample_rate);
}

/* libobs requires this for sources that host children: it drives
 * activation/show propagation and the audio render order. */
static void hdrp_enum_active_sources(void *data, obs_source_enum_proc_t cb,
				     void *param)
{
	struct hdr_playlist *p = data;
	obs_source_t *child;
	if (!p)
		return;
	for (int i = 0; i < 2; i++) {
		child = hdrp_switcher_slot(p->sw, i);
		if (child)
			cb(p->source, child, param);
	}
}

static enum gs_color_space
hdrp_video_get_color_space(void *data, size_t count,
			   const enum gs_color_space *preferred_spaces)
{
	struct hdr_playlist *p = data;
	if (!p || !preferred_spaces || count == 0)
		return GS_CS_SRGB;

	/* If OBS is not configured to output HDR (e.g. a 1080p Rec.709
	 * stream), everything is played as SDR: OBS then tonemaps HDR clips
	 * down the normal way instead of us pretending to be HDR. */
	if (!p->output_hdr)
		return GS_CS_SRGB;

	/* OBS 31 only distinguishes SDR from HDR at the source level; the
	 * PQ/HLG choice lives in OBS's output settings, so both "force"
	 * entries map to the same HDR presentation.
	 *
	 *   auto        -> SDR clips report SDR (no forced conversion!)
	 *   force PQ/HLG-> force HDR presentation
	 *   force SDR   -> force SDR presentation */
	if (p->mixed == MIXED_FORCE_SDR)
		return GS_CS_SRGB;
	if (p->mixed == MIXED_FORCE_PQ || p->mixed == MIXED_FORCE_HLG)
		return GS_CS_709_EXTENDED;

	return hdrp_switcher_content_space(p->sw, count, preferred_spaces);
}

static void hdrp_media_play_pause(void *data, bool pause)
{
	struct hdr_playlist *p = data;
	enum obs_media_state st;

	if (!p)
		return;
	st = hdrp_switcher_get_state(p->sw);
	if (pause) {
		if (st == OBS_MEDIA_STATE_PLAYING)
			hdrp_switcher_set_paused(p->sw, true);
		return;
	}
	if (st == OBS_MEDIA_STATE_PAUSED) {
		hdrp_switcher_set_paused(p->sw, false);
		return;
	}
	hdrp_start_current(p);
}

static void hdrp_media_restart(void *data)
{
	struct hdr_playlist *p = data;
	if (p)
		hdrp_start_current(p);
}

static void hdrp_media_stop(void *data)
{
	struct hdr_playlist *p = data;
	if (p)
		hdrp_stop_playback(p);
}

static void hdrp_media_next(void *data)
{
	struct hdr_playlist *p = data;
	if (p)
		hdrp_jump(p, true);
}

static void hdrp_media_previous(void *data)
{
	struct hdr_playlist *p = data;
	if (p)
		hdrp_jump(p, false);
}

static int64_t hdrp_media_get_duration(void *data)
{
	struct hdr_playlist *p = data;
	return p ? hdrp_switcher_get_duration(p->sw) : 0;
}

static int64_t hdrp_media_get_time(void *data)
{
	struct hdr_playlist *p = data;
	return p ? hdrp_switcher_get_time(p->sw) : 0;
}

static void hdrp_media_set_time(void *data, int64_t ms)
{
	struct hdr_playlist *p = data;
	if (p)
		hdrp_switcher_seek(p->sw, ms);
}

static enum obs_media_state hdrp_media_get_state(void *data)
{
	struct hdr_playlist *p = data;
	if (!p || hdrp_playlist_count(p->pl) == 0 || p->stopped)
		return OBS_MEDIA_STATE_STOPPED;
	return hdrp_switcher_get_state(p->sw);
}

static obs_missing_files_t *hdrp_missing_files(void *data)
{
	UNUSED_PARAMETER(data);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* properties                                                          */
/* ------------------------------------------------------------------ */

static void hdrp_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, KEY_MODE, HDRP_MODE_SEQUENTIAL);
	obs_data_set_default_bool(settings, KEY_HW_DECODE, true);
	obs_data_set_default_int(settings, KEY_SPEED, 100);
	obs_data_set_default_bool(settings, KEY_USE_TRANSITION, false);
	obs_data_set_default_int(settings, KEY_TRANSITION_MS, 200);
	obs_data_set_default_bool(settings, KEY_LOW_MEMORY, true);
	obs_data_set_default_int(settings, KEY_VISIBILITY, VIS_STOP_RESTART);
	obs_data_set_default_int(settings, KEY_MIXED, MIXED_AUTO);
	obs_data_set_default_bool(settings, KEY_ADAPT_CANVAS, true);
	obs_data_set_default_int(settings, KEY_DEINTERLACE,
				 OBS_DEINTERLACE_MODE_DISABLE);
	obs_data_set_default_int(settings, KEY_SAVED_INDEX, -1);
}

static bool hdrp_play_now_clicked(obs_properties_t *props,
				  obs_property_t *property, void *data)
{
	struct hdr_playlist *p = data;
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	if (p)
		hdrp_start_current(p);
	return false;
}

static obs_properties_t *hdrp_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *list;
	obs_property_t *mode;
	obs_property_t *speed;
	obs_properties_t *gcontent;
	obs_property_t *dur;
	obs_property_t *vis;
	obs_property_t *mixed;
	obs_property_t *adapt;
	obs_property_t *deint;

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	static const char *media_filter =
		"Media files (*.mp4 *.m4v *.ts *.m2ts *.mov *.mxf *.flv *.mkv "
		"*.avi *.webm *.gif *.m3u8 *.mpd *.mp3 *.aac *.ogg *.wav "
		"*.flac *.m4a *.opus)";

	/* Live status: current file + progress, and the detected session. */
	{
		char status[256];
		hdrp_build_now_playing(data, status, sizeof(status));
		obs_properties_add_text(props, "hdrp_status_now", status,
					OBS_TEXT_INFO);
		hdrp_build_session_info(data, status, sizeof(status));
		obs_properties_add_text(props, "hdrp_status_session", status,
					OBS_TEXT_INFO);
	}

	list = obs_properties_add_editable_list(
		props, KEY_FILES, obs_module_text("PlaylistFiles"),
		OBS_EDITABLE_LIST_TYPE_FILES, media_filter, NULL);
	obs_property_set_long_description(list,
					  obs_module_text("PlaylistDescription"));

	obs_properties_add_list(props, KEY_MODE, obs_module_text("PlaybackMode"),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	mode = obs_properties_get(props, KEY_MODE);
	obs_property_list_add_int(mode, obs_module_text("ModeSequential"),
				  HDRP_MODE_SEQUENTIAL);
	obs_property_list_add_int(mode, obs_module_text("ModeLoop"),
				  HDRP_MODE_LOOP);
	obs_property_list_add_int(mode, obs_module_text("ModeShuffle"),
				  HDRP_MODE_SHUFFLE);

	obs_properties_add_bool(props, KEY_HW_DECODE,
				obs_module_text("HardwareDecode"));

	speed = obs_properties_add_int_slider(props, KEY_SPEED,
					      obs_module_text("Speed"), 1, 200,
					      1);
	obs_property_int_set_suffix(speed, "%");

	gcontent = obs_properties_create();
	dur = obs_properties_add_int(gcontent, KEY_TRANSITION_MS,
				     obs_module_text("TransitionDuration"), 0,
				     2000, 50);
	obs_property_int_set_suffix(dur, " ms");
	obs_properties_add_group(props, KEY_USE_TRANSITION,
				 obs_module_text("Transition"),
				 OBS_GROUP_CHECKABLE, gcontent);

	obs_properties_add_bool(props, KEY_LOW_MEMORY,
				obs_module_text("LowMemory"));

	obs_properties_add_path(props, KEY_ADD_FOLDER,
				obs_module_text("AddFolderPick"),
				OBS_PATH_DIRECTORY, NULL, NULL);

	adapt = obs_properties_add_bool(props, KEY_ADAPT_CANVAS,
					obs_module_text("AdaptToCanvas"));
	obs_property_set_long_description(
		adapt, obs_module_text("AdaptToCanvasHint"));

	obs_properties_add_list(props, KEY_DEINTERLACE,
				obs_module_text("Deinterlace"),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	deint = obs_properties_get(props, KEY_DEINTERLACE);
	obs_property_set_long_description(deint,
					  obs_module_text("DeinterlaceHint"));
	obs_property_list_add_int(deint, obs_module_text("DeintOff"),
				  OBS_DEINTERLACE_MODE_DISABLE);
	/* The *_2X modes emit one frame per field, i.e. 1080i25 -> 50p. */
	obs_property_list_add_int(deint, obs_module_text("DeintYadif2x"),
				  OBS_DEINTERLACE_MODE_YADIF_2X);
	obs_property_list_add_int(deint, obs_module_text("DeintLinear2x"),
				  OBS_DEINTERLACE_MODE_LINEAR_2X);
	obs_property_list_add_int(deint, obs_module_text("DeintBlend2x"),
				  OBS_DEINTERLACE_MODE_BLEND_2X);
	obs_property_list_add_int(deint, obs_module_text("DeintYadif"),
				  OBS_DEINTERLACE_MODE_YADIF);
	obs_property_list_add_int(deint, obs_module_text("DeintLinear"),
				  OBS_DEINTERLACE_MODE_LINEAR);
	obs_property_list_add_int(deint, obs_module_text("DeintDiscard"),
				  OBS_DEINTERLACE_MODE_DISCARD);

	obs_properties_add_list(props, KEY_VISIBILITY,
				obs_module_text("Visibility"),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	vis = obs_properties_get(props, KEY_VISIBILITY);
	obs_property_list_add_int(vis, obs_module_text("VisStopRestart"),
				  VIS_STOP_RESTART);
	obs_property_list_add_int(vis, obs_module_text("VisPauseResume"),
				  VIS_PAUSE_RESUME);
	obs_property_list_add_int(vis, obs_module_text("VisAlwaysPlay"),
				  VIS_ALWAYS_PLAY);
	obs_property_list_add_int(vis, obs_module_text("VisStopNext"),
				  VIS_STOP_NEXT);

	obs_properties_add_list(props, KEY_MIXED,
				obs_module_text("MixedContent"),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	mixed = obs_properties_get(props, KEY_MIXED);
	obs_property_set_long_description(
		mixed, obs_module_text("MixedContentHint"));
	obs_property_list_add_int(mixed, obs_module_text("MixedAuto"),
				  MIXED_AUTO);
	obs_property_list_add_int(mixed, obs_module_text("MixedForcePQ"),
				  MIXED_FORCE_PQ);
	obs_property_list_add_int(mixed, obs_module_text("MixedForceHLG"),
				  MIXED_FORCE_HLG);
	obs_property_list_add_int(mixed, obs_module_text("MixedForceSDR"),
				  MIXED_FORCE_SDR);

	obs_properties_add_button(props, "hdrp_play_now",
				  obs_module_text("PlayNow"),
				  hdrp_play_now_clicked);

	return props;
}

/* ------------------------------------------------------------------ */
/* descriptor                                                          */
/* ------------------------------------------------------------------ */

static const struct obs_source_info hdrp_source_info = {
	.id = HDRP_SOURCE_ID,
	.version = 1,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
			OBS_SOURCE_AUDIO | OBS_SOURCE_CONTROLLABLE_MEDIA |
			OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = hdrp_get_name,
	.create = hdrp_create,
	.destroy = hdrp_destroy,
	.get_width = hdrp_get_width,
	.get_height = hdrp_get_height,
	.get_defaults = hdrp_defaults,
	.get_properties = hdrp_properties,
	.update = hdrp_update,
	.save = hdrp_save,
	.show = hdrp_show,
	.hide = hdrp_hide,
	.activate = hdrp_activate,
	.video_tick = hdrp_video_tick,
	.video_render = hdrp_video_render,
	.video_get_color_space = hdrp_video_get_color_space,
	.audio_render = hdrp_source_audio_render,
	.enum_active_sources = hdrp_enum_active_sources,
	.missing_files = hdrp_missing_files,
	.media_play_pause = hdrp_media_play_pause,
	.media_restart = hdrp_media_restart,
	.media_stop = hdrp_media_stop,
	.media_next = hdrp_media_next,
	.media_previous = hdrp_media_previous,
	.media_get_duration = hdrp_media_get_duration,
	.media_get_time = hdrp_media_get_time,
	.media_set_time = hdrp_media_set_time,
	.media_get_state = hdrp_media_get_state,
	.icon_type = OBS_ICON_TYPE_MEDIA,
};

void hdrp_register_sources(void)
{
	obs_register_source(&hdrp_source_info);
	blog(LOG_INFO, "[HDR-PL] source registered");
}
