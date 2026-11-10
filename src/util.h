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

/* Portable atomics with the same semantics as libobs' os_atomic_*:
 * SET returns the OLD value, LOAD returns the current value, INC/DEC return
 * the NEW value. We cannot include <util/threading.h> for these — OBS 31's
 * threading.h includes <pthread.h> unconditionally, which is not shipped to
 * a plain MSVC plugin build (the Windows CI only provides libobs headers). */
#ifdef _WIN32
typedef volatile long hdrp_atomic_t;
#define HDRP_ATOMIC_SET(p, v)                                                \
	InterlockedExchange((volatile LONG *)(p), (LONG)(v))
#define HDRP_ATOMIC_LOAD(p)                                                  \
	InterlockedCompareExchange((volatile LONG *)(p), 0L, 0L)
#define HDRP_ATOMIC_INC(p) InterlockedIncrement((volatile LONG *)(p))
#define HDRP_ATOMIC_DEC(p) InterlockedDecrement((volatile LONG *)(p))
#else
typedef volatile long hdrp_atomic_t;
#define HDRP_ATOMIC_SET(p, v)                                                \
	__atomic_exchange_n((p), (long)(v), __ATOMIC_SEQ_CST)
#define HDRP_ATOMIC_LOAD(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define HDRP_ATOMIC_INC(p)  __atomic_add_fetch((p), 1L, __ATOMIC_SEQ_CST)
#define HDRP_ATOMIC_DEC(p)  __atomic_sub_fetch((p), 1L, __ATOMIC_SEQ_CST)
#endif

void hdrp_mutex_init(hdrp_mutex_t *m);
void hdrp_mutex_destroy(hdrp_mutex_t *m);
void hdrp_mutex_lock(hdrp_mutex_t *m);
void hdrp_mutex_unlock(hdrp_mutex_t *m);

/* Returns a bstrdup'd basename of `path` (strip leading directories for both
 * '/' and '\\'); free the result with bfree(). Never returns NULL. */
char *hdrp_basename_dup(const char *path);

/* Opens `url` in the user's default browser (ShellExecute on Windows,
 * `open` on macOS, `xdg-open` elsewhere). */
void hdrp_open_url(const char *url);
