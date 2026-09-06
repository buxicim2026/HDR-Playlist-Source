/*
 * util.h — small cross-platform helpers.
 *
 * NOTE: include this header BEFORE any obs headers on Windows (it pulls in
 * <windows.h>; libobs headers expect winsock/windows types to already exist).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION hdrp_mutex_t;
#else
#include <pthread.h>
typedef pthread_mutex_t hdrp_mutex_t;
#endif

void hdrp_mutex_init(hdrp_mutex_t *m);
void hdrp_mutex_destroy(hdrp_mutex_t *m);
void hdrp_mutex_lock(hdrp_mutex_t *m);
void hdrp_mutex_unlock(hdrp_mutex_t *m);

/* Returns a bstrdup'd basename of `path` (strip leading directories for both
 * '/' and '\\'); free the result with bfree(). Never returns NULL. */
char *hdrp_basename_dup(const char *path);
