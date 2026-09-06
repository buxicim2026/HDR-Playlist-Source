/*
 * playlist.h — media playlist model.
 *
 * Thread-safety: none inside.  The owner (hdr_playlist_source) serialises all
 * calls with its own mutex; the video hot path only reads count/index/current.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <obs-data.h>

typedef enum {
	HDRP_MODE_SEQUENTIAL = 0,
	HDRP_MODE_LOOP,
	HDRP_MODE_SHUFFLE,
} hdrp_play_mode;

struct hdrp_playlist;

struct hdrp_playlist *hdrp_playlist_create(void);
void hdrp_playlist_destroy(struct hdrp_playlist *pl);

/* File management. All paths are stored as given (UTF-8). */
bool hdrp_playlist_add_file(struct hdrp_playlist *pl, const char *path);
size_t hdrp_playlist_add_folder(struct hdrp_playlist *pl, const char *dir,
				int max_depth); /* returns #files added */
void hdrp_playlist_remove(struct hdrp_playlist *pl, size_t index);
void hdrp_playlist_clear(struct hdrp_playlist *pl);

size_t hdrp_playlist_count(const struct hdrp_playlist *pl);
/* Path of a list entry; NULL when out of range. Valid until next mutation. */
const char *hdrp_playlist_at(const struct hdrp_playlist *pl, size_t index);

void hdrp_playlist_set_mode(struct hdrp_playlist *pl, hdrp_play_mode mode);
hdrp_play_mode hdrp_playlist_get_mode(const struct hdrp_playlist *pl);

/* "Cursor" management. */
bool hdrp_playlist_has_current(const struct hdrp_playlist *pl);
size_t hdrp_playlist_current_index(const struct hdrp_playlist *pl);
const char *hdrp_playlist_current(const struct hdrp_playlist *pl);
void hdrp_playlist_set_current(struct hdrp_playlist *pl, size_t index);
/* Step the cursor. For SEQUENTIAL at the end returns NULL and keeps the
 * cursor valid at the last item (caller decides to stop). */
const char *hdrp_playlist_next(struct hdrp_playlist *pl);
const char *hdrp_playlist_previous(struct hdrp_playlist *pl);
/* Like hdrp_playlist_next() but does NOT move the cursor: returns the path
 * that would play next, or NULL at the end of SEQUENTIAL mode. The pointer
 * stays valid until the list is mutated. */
const char *hdrp_playlist_peek_next(struct hdrp_playlist *pl);

/* Persistence: array of {"path": <str>} plus mode/index fields. */
void hdrp_playlist_save(struct hdrp_playlist *pl, obs_data_t *settings);
void hdrp_playlist_load(struct hdrp_playlist *pl, obs_data_t *settings);

/* True if `path` is a media file we can play (extension filter). */
bool hdrp_playlist_is_supported_file(const char *path);
