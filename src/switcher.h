/*
 * switcher.h — A/B dual-slot media switcher built on two private
 * OBS `ffmpeg_source` children.
 *
 * Responsibility split:
 *   * this file owns the two child slots, preload/park, promote, rendering
 *     (incl. HDR-safe crossfade) and color-space delegation;
 *   * hdr_playlist_source.c owns the playlist, UI, media controls and audio
 *     forwarding, driving the switcher through the API below.
 *
 * Threading: all public functions must be called from the OBS UI or video
 * thread. Signal callbacks (media_ended) only set an atomic "advance
 * requested" flag, which tick() consumes on the video thread.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <obs.h>

/* Options the owner copies into the switcher on each play/preload. */
struct hdrp_switcher_opts {
	bool hw_decode;    /* keep 10-bit P010 frames */
	int speed_percent; /* 1..200 */
	bool clear_on_end; /* drop the last frame when a clip ends */
};

struct hdrp_switcher;

/* opaque = owner pointer passed back to `cb` when the current clip asks the
 * owner to move on (playlist advance / stop at end). */
struct hdrp_switcher *hdrp_switcher_create(obs_source_t *parent, void *opaque,
					   void (*cb)(void *opaque));
void hdrp_switcher_destroy(struct hdrp_switcher *sw);

/* ------------------------------------------------------------------ */
/* control (owner thread)                                              */
/* ------------------------------------------------------------------ */

/* Play `path` immediately in the active slot. Returns true on success. */
bool hdrp_switcher_play(struct hdrp_switcher *sw, const char *path,
			const struct hdrp_switcher_opts *opts);

/* Open `path` on the idle slot and park it on its first frame (paused).
 * Later promotion resumes it. Cancels any previous preload. */
bool hdrp_switcher_preload(struct hdrp_switcher *sw, const char *path,
			   const struct hdrp_switcher_opts *opts);

/* Stop all playback and idle both slots. */
void hdrp_switcher_stop(struct hdrp_switcher *sw);
void hdrp_switcher_set_paused(struct hdrp_switcher *sw, bool pause);
bool hdrp_switcher_seek(struct hdrp_switcher *sw, int64_t ms);

/* Switch immediately to `path` (manual next/previous/selection). Uses the
 * already-parked copy when available, otherwise restarts the active slot. */
void hdrp_switcher_force_advance(struct hdrp_switcher *sw, const char *path,
				 const struct hdrp_switcher_opts *opts);
/* True while a crossfade is in flight; owners should defer preloads. */
bool hdrp_switcher_is_transitioning(const struct hdrp_switcher *sw);

int64_t hdrp_switcher_get_time(struct hdrp_switcher *sw);
int64_t hdrp_switcher_get_duration(struct hdrp_switcher *sw);
enum obs_media_state hdrp_switcher_get_state(struct hdrp_switcher *sw);

const char *hdrp_switcher_active_path(const struct hdrp_switcher *sw);

/* ------------------------------------------------------------------ */
/* per-frame (video thread)                                            */
/* ------------------------------------------------------------------ */

/* Drives preload/promote/transition state machines. Call once per frame. */
void hdrp_switcher_tick(struct hdrp_switcher *sw, float seconds);

/* Renders the active child (or the in-flight crossfade). */
void hdrp_switcher_render(struct hdrp_switcher *sw);

/* Color-space delegation for the parent source's video_get_color_space. */
enum gs_color_space
hdrp_switcher_get_color_space(struct hdrp_switcher *sw, size_t count,
			      const enum gs_color_space *preferred);

/* Crossfade control (0 disables => hard cut on first available frame). */
void hdrp_switcher_set_transition_ms(struct hdrp_switcher *sw, int ms);
int hdrp_switcher_get_transition_ms(const struct hdrp_switcher *sw);
/* True once per promote: audio owner should switch capture to the new slot. */
bool hdrp_switcher_consume_promote_event(struct hdrp_switcher *sw);

/* Exposed so hdrp_playlist_source.c can attach its audio capture callbacks. */
obs_source_t *hdrp_switcher_active_child(const struct hdrp_switcher *sw);
obs_source_t *hdrp_switcher_idle_child(const struct hdrp_switcher *sw);
