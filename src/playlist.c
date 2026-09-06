/*
 * playlist.c — media playlist model (order list + cursor + folder expand).
 *
 * Uses libobs utility helpers (bstrdup/bfree, os_* directory API) which the
 * OBS process exports at runtime, mirroring how obs-ffmpeg does it.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <obs-data.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "playlist.h"

/* Depth limit for recursive folder expansion (protects against symlink/cycle
 * storms); 0 disables recursion entirely. */
#define HDRP_FOLDER_MAX_DEPTH 4

struct hdrp_playlist {
	char **files;
	size_t count;
	size_t cap;

	size_t current;  /* valid index, or count when has_current == false */
	bool has_current;

	hdrp_play_mode mode;

	uint64_t rng; /* xorshift64* for shuffle */
};

/* ----------------------------------------------------------------------- */
/* internal helpers                                                         */
/* ----------------------------------------------------------------------- */

static bool str_ends_with_ci(const char *s, const char *suffix)
{
	size_t sl = strlen(s);
	size_t xl = strlen(suffix);
	if (xl > sl)
		return false;
	s += sl - xl;
	for (size_t i = 0; i < xl; i++) {
		if (tolower((unsigned char)s[i]) != tolower((unsigned char)suffix[i]))
			return false;
	}
	return true;
}

static int cmp_cstr(const void *a, const void *b)
{
	const char *const *pa = a;
	const char *const *pb = b;
	return strcmp(*pa, *pb);
}

static uint64_t rng_next(struct hdrp_playlist *pl)
{
	uint64_t x = pl->rng;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	pl->rng = x;
	return x * 0x2545F4914F6CDD1DULL;
}

static bool files_push(struct hdrp_playlist *pl, const char *path)
{
	if (pl->count == pl->cap) {
		size_t ncap = pl->cap ? pl->cap * 2 : 16;
		char **nv = brealloc(pl->files, ncap * sizeof(*nv));
		if (!nv)
			return false;
		pl->files = nv;
		pl->cap = ncap;
	}
	pl->files[pl->count] = bstrdup(path);
	if (!pl->files[pl->count])
		return false;
	pl->count++;
	return true;
}

static void files_erase(struct hdrp_playlist *pl, size_t index)
{
	if (index >= pl->count)
		return;
	bfree(pl->files[index]);
	memmove(&pl->files[index], &pl->files[index + 1],
		(pl->count - index - 1) * sizeof(*pl->files));
	pl->count--;
}

/* Join dir + name into out (max out_size). Returns strlen or -1 truncation. */
static int join_path(char *out, size_t out_size, const char *dir,
		     const char *name)
{
	size_t dl = strlen(dir);
	bool has_sep = dl > 0 &&
		       (dir[dl - 1] == '/' || dir[dl - 1] == '\\');
	return snprintf(out, out_size, "%s%s%s", dir, has_sep ? "" : "/",
			name);
}

/* Probe: is `path` an openable directory? */
static bool path_is_dir(const char *path)
{
	os_dir_t *d = os_opendir(path);
	if (!d)
		return false;
	os_closedir(d);
	return true;
}

/* Expand a directory recursively (sorted per level, depth-limited). */
static size_t expand_dir(struct hdrp_playlist *pl, const char *dir,
			 int depth)
{
	os_dir_t *d = os_opendir(dir);
	if (!d)
		return 0;

	/* Collect entries first so we can sort names deterministically. */
	struct os_dirent *ent;
	size_t n = 0, cap = 0;
	char **names = NULL;
	while ((ent = os_readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			char **nv = brealloc(names, cap * sizeof(*nv));
			if (!nv)
				break;
			names = nv;
		}
		names[n] = bstrdup(ent->d_name);
		if (names[n])
			n++;
	}
	os_closedir(d);

	qsort(names, n, sizeof(*names), cmp_cstr);

	char full[4096];
	size_t added = 0;
	for (size_t i = 0; i < n; i++) {
		int len = join_path(full, sizeof(full), dir, names[i]);
		if (len <= 0 || (size_t)len >= sizeof(full)) {
			bfree(names[i]);
			continue;
		}

		if (path_is_dir(full)) {
			if (depth > 0)
				added += expand_dir(pl, full, depth - 1);
		} else if (hdrp_playlist_is_supported_file(full)) {
			if (files_push(pl, full))
				added++;
		}
		bfree(names[i]);
	}
	bfree(names);
	return added;
}

/* ----------------------------------------------------------------------- */
/* public API                                                               */
/* ----------------------------------------------------------------------- */

bool hdrp_playlist_is_supported_file(const char *path)
{
	static const char *const video[] = {
		"mp4", "m4v", "ts",   "mov", "mxf",
		"flv", "mkv", "avi",  "webm", "gif",
	};
	static const char *const audio[] = {
		"mp3", "aac", "ogg", "wav", "flac", "m4a", "opus",
	};
	for (size_t i = 0; i < sizeof(video) / sizeof(video[0]); i++)
		if (str_ends_with_ci(path, video[i]))
			return true;
	for (size_t i = 0; i < sizeof(audio) / sizeof(audio[0]); i++)
		if (str_ends_with_ci(path, audio[i]))
			return true;
	return false;
}

struct hdrp_playlist *hdrp_playlist_create(void)
{
	struct hdrp_playlist *pl = bzalloc(sizeof(*pl));
	pl->current = 0;
	pl->has_current = false;
	pl->mode = HDRP_MODE_SEQUENTIAL;
	pl->rng = (uint64_t)os_gettime_ns() ^ 0x9E3779B97F4A7C15ULL;
	if (!pl->rng)
		pl->rng = 1;
	return pl;
}

void hdrp_playlist_destroy(struct hdrp_playlist *pl)
{
	if (!pl)
		return;
	hdrp_playlist_clear(pl);
	bfree(pl->files);
	bfree(pl);
}

bool hdrp_playlist_add_file(struct hdrp_playlist *pl, const char *path)
{
	if (!path || !*path)
		return false;
	return files_push(pl, path);
}

size_t hdrp_playlist_add_folder(struct hdrp_playlist *pl, const char *dir,
				int max_depth)
{
	if (max_depth < 0)
		max_depth = 0;
	if (max_depth > HDRP_FOLDER_MAX_DEPTH)
		max_depth = HDRP_FOLDER_MAX_DEPTH;
	return expand_dir(pl, dir, max_depth);
}

void hdrp_playlist_remove(struct hdrp_playlist *pl, size_t index)
{
	if (index >= pl->count)
		return;
	bool removed_before_current = pl->has_current && index < pl->current;
	bool removed_current = pl->has_current && index == pl->current;
	files_erase(pl, index);
	if (removed_before_current && pl->has_current)
		pl->current--;
	else if (removed_current)
		pl->has_current = false;
	if (pl->has_current && pl->current >= pl->count) {
		pl->current = pl->count;
		pl->has_current = false;
	}
}

void hdrp_playlist_clear(struct hdrp_playlist *pl)
{
	for (size_t i = 0; i < pl->count; i++)
		bfree(pl->files[i]);
	pl->count = 0;
	pl->has_current = false;
	pl->current = 0;
}

size_t hdrp_playlist_count(const struct hdrp_playlist *pl)
{
	return pl->count;
}

const char *hdrp_playlist_at(const struct hdrp_playlist *pl, size_t index)
{
	if (index >= pl->count)
		return NULL;
	return pl->files[index];
}

void hdrp_playlist_set_mode(struct hdrp_playlist *pl, hdrp_play_mode mode)
{
	if (mode <= HDRP_MODE_SHUFFLE)
		pl->mode = mode;
}

hdrp_play_mode hdrp_playlist_get_mode(const struct hdrp_playlist *pl)
{
	return pl->mode;
}

bool hdrp_playlist_has_current(const struct hdrp_playlist *pl)
{
	return pl->has_current && pl->current < pl->count;
}

size_t hdrp_playlist_current_index(const struct hdrp_playlist *pl)
{
	return pl->current;
}

const char *hdrp_playlist_current(const struct hdrp_playlist *pl)
{
	return hdrp_playlist_has_current(pl) ? pl->files[pl->current] : NULL;
}

void hdrp_playlist_set_current(struct hdrp_playlist *pl, size_t index)
{
	if (index < pl->count) {
		pl->current = index;
		pl->has_current = true;
	}
}

const char *hdrp_playlist_next(struct hdrp_playlist *pl)
{
	if (pl->count == 0)
		return NULL;
	if (!pl->has_current) {
		pl->current = 0;
		pl->has_current = true;
		return pl->files[0];
	}

	switch (pl->mode) {
	case HDRP_MODE_SEQUENTIAL:
		if (pl->current + 1 >= pl->count)
			return NULL; /* caller decides to stop */
		pl->current++;
		break;
	case HDRP_MODE_LOOP:
		pl->current = (pl->current + 1) % pl->count;
		break;
	case HDRP_MODE_SHUFFLE: {
		size_t pick;
		if (pl->count == 1) {
			pick = 0;
		} else {
			do {
				pick = (size_t)(rng_next(pl) % pl->count);
			} while (pick == pl->current);
		}
		pl->current = pick;
		break;
	}
	}
	return pl->files[pl->current];
}

const char *hdrp_playlist_previous(struct hdrp_playlist *pl)
{
	if (pl->count == 0)
		return NULL;
	if (!pl->has_current) {
		pl->current = 0;
		pl->has_current = true;
		return pl->files[0];
	}

	switch (pl->mode) {
	case HDRP_MODE_SEQUENTIAL:
		if (pl->current > 0)
			pl->current--;
		break;
	case HDRP_MODE_LOOP:
		pl->current = (pl->current + pl->count - 1) % pl->count;
		break;
	case HDRP_MODE_SHUFFLE: {
		size_t pick;
		if (pl->count == 1) {
			pick = 0;
		} else {
			do {
				pick = (size_t)(rng_next(pl) % pl->count);
			} while (pick == pl->current);
		}
		pl->current = pick;
		break;
	}
	}
	return pl->files[pl->current];
}

const char *hdrp_playlist_peek_next(struct hdrp_playlist *pl)
{
	/* Work on a shallow copy: scalars (current/has_current/rng) are
	 * per-copy; the file array is shared but read-only here. */
	struct hdrp_playlist tmp = *pl;
	hdrp_playlist_next(&tmp);
	return hdrp_playlist_has_current(&tmp) ? tmp.files[tmp.current] : NULL;
}

void hdrp_playlist_save(struct hdrp_playlist *pl, obs_data_t *settings)
{
	obs_data_array_t *arr = obs_data_array_create();
	for (size_t i = 0; i < pl->count; i++) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "path", pl->files[i]);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(settings, "playlist_files", arr);
	obs_data_array_release(arr);
	obs_data_set_int(settings, "playlist_mode", (long long)pl->mode);
	if (hdrp_playlist_has_current(pl))
		obs_data_set_int(settings, "playlist_index",
				 (long long)pl->current);
	else
		obs_data_set_int(settings, "playlist_index", -1);
}

void hdrp_playlist_load(struct hdrp_playlist *pl, obs_data_t *settings)
{
	hdrp_playlist_clear(pl);

	obs_data_array_t *arr = obs_data_get_array(settings, "playlist_files");
	if (arr) {
		size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			if (item) {
				const char *p = obs_data_get_string(item, "path");
				if (p && *p)
					files_push(pl, p);
				obs_data_release(item);
			}
		}
		obs_data_array_release(arr);
	}

	long long mode = obs_data_get_int(settings, "playlist_mode");
	if (mode >= HDRP_MODE_SEQUENTIAL && mode <= HDRP_MODE_SHUFFLE)
		pl->mode = (hdrp_play_mode)mode;

	long long idx = obs_data_get_int(settings, "playlist_index");
	if (idx >= 0 && (size_t)idx < pl->count) {
		pl->current = (size_t)idx;
		pl->has_current = true;
	}
}
