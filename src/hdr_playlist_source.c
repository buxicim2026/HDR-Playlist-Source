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

	obs_hotkey_id hk_play_pause;
	obs_hotkey_id hk_restart;
	obs_hotkey_id hk_stop;
	obs_hotkey_id hk_next;
	obs_hotkey_id hk_prev;

	bool showing;
	bool stopped;
	bool need_preload_retry;
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
	if (arr) {
		size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			const char *path;
			if (!item)
				continue;
			path = obs_data_get_string(item, "value");
			if (path && *path)
				hdrp_playlist_add_file(p->pl, path);
			obs_data_release(item);
		}
		obs_data_array_release(arr);
	}

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

	p->stopped = false;
	hdrp_switcher_play(p->sw, path, &opts);
	if (p->au)
		hdrp_attach_audio_to_active(p);

	obs_source_media_started(p->source);
	p->need_preload_retry = true;
}

static void hdrp_stop_playback(struct hdr_playlist *p)
{
	if (p->au) {
		hdrp_audio_detach(p->au);
		hdrp_audio_flush(p->au);
	}
	hdrp_switcher_stop(p->sw);
	p->stopped = true;
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

	UNUSED_PARAMETER(data);
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	static const char *media_filter =
		"Media files (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi "
		"*.webm *.gif *.mp3 *.aac *.ogg *.wav *.flac *.m4a *.opus)";

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
