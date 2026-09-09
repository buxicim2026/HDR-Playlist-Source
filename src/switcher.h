/*
 * switcher.h — A/B dual-slot media switcher built on two private
 * OBS `ffmpeg_source` children.
 *
 * Ticking: libobs' tick_sources() walks *every* source (private ones
 * included) and calls obs_source_video_tick() on each, so the private
 * ffmpeg children are ticked automatically — including the deferred
 * obs_source_update() that makes a new "local_file" take effect.
 * obs_source_video_tick() is internal API, so a plugin must never call it.
 *
 * What a plugin *must* do instead is register the children with
 * obs_source_add_active_child() and implement enum_active_sources() (see
 * hdr_playlist_source.c) so activation/show/audio enumeration reach them.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <obs.h>

/* Options copied into the switcher on each play/preload. */
struct hdrp_switcher_opts {
	bool hw_decode;    /* keep 10-bit P010 frames */
	int speed_percent; /* 1..200 */
	bool clear_on_end; /* drop the last frame when a clip ends */
};

struct hdrp_switcher;

/* opaque = owner pointer passed to `cb` when the current clip asks the owner
 * to move on (playlist advance / stop at end). */
struct hdrp_switcher *hdrp_switcher_create(obs_source_t *parent, void *opaque,
					   void (*cb)(void *opaque));
void hdrp_switcher_destroy(struct hdrp_switcher *sw);

/* ------------------------------------------------------------------ */
/* control (owner thread)                                              */
/* ------------------------------------------------------------------ */

/* Play `path` on the active slot. */
bool hdrp_switcher_play(struct hdrp_switcher *sw, const char *path,
			const struct hdrp_switcher_opts *opts);

/* Open `path` on the idle slot and park it on its first frame (paused). */
bool hdrp_switcher_preload(struct hdrp_switcher *sw, const char *path,
			   const struct hdrp_switcher_opts *opts);
void hdrp_switcher_drop_preload(struct hdrp_switcher *sw);

void hdrp_switcher_stop(struct hdrp_switcher *sw);
void hdrp_switcher_set_paused(struct hdrp_switcher *sw, bool pause);
bool hdrp_switcher_seek(struct hdrp_switcher *sw, int64_t ms);

int64_t hdrp_switcher_get_time(struct hdrp_switcher *sw);
int64_t hdrp_switcher_get_duration(struct hdrp_switcher *sw);
enum obs_media_state hdrp_switcher_get_state(struct hdrp_switcher *sw);

const char *hdrp_switcher_active_path(const struct hdrp_switcher *sw);
obs_source_t *hdrp_switcher_active_child(const struct hdrp_switcher *sw);
/* Slot access for enum_active_sources(): idx must be 0 or 1, may be NULL. */
obs_source_t *hdrp_switcher_slot(const struct hdrp_switcher *sw, int idx);

/* ------------------------------------------------------------------ */
/* per-frame (video thread)                                            */
/* ------------------------------------------------------------------ */

/* Ticks both children, then drives preload/promote/transition. */
void hdrp_switcher_tick(struct hdrp_switcher *sw, float seconds);
/* Renders the active child (or the in-flight crossfade). */
void hdrp_switcher_render(struct hdrp_switcher *sw);

uint32_t hdrp_switcher_get_width(struct hdrp_switcher *sw);
uint32_t hdrp_switcher_get_height(struct hdrp_switcher *sw);

/* True once per promote: the audio owner should re-attach to the new slot. */
bool hdrp_switcher_consume_promote_event(struct hdrp_switcher *sw);
bool hdrp_switcher_is_transitioning(const struct hdrp_switcher *sw);

/* Real color space of the media currently playing (see switcher.c). */
enum gs_color_space
hdrp_switcher_content_space(struct hdrp_switcher *sw, size_t count,
			    const enum gs_color_space *preferred);

/* Preload (gapless) control. When disabled the second decoder is destroyed
 * and only re-created if preloading is enabled again — this is the single
 * biggest memory lever, since every ffmpeg_source keeps its own decoded
 * frame cache. */
void hdrp_switcher_set_preload_enabled(struct hdrp_switcher *sw, bool enable);

/* Crossfade: 0 disables (hard cut). */
void hdrp_switcher_set_transition_ms(struct hdrp_switcher *sw, int ms);
int hdrp_switcher_get_transition_ms(const struct hdrp_switcher *sw);

/* Release the idle decoder (low-memory mode). */
void hdrp_switcher_idle_stop_if_playing(struct hdrp_switcher *sw);
