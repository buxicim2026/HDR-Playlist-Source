/*
 * util.c — small cross-platform helpers (mutex + path utilities).
 */

#include <string.h>

#include "util.h"

#include <util/bmem.h>

void hdrp_mutex_init(hdrp_mutex_t *m)
{
#ifdef _WIN32
	InitializeCriticalSection(m);
#else
	pthread_mutex_init(m, NULL);
#endif
}

void hdrp_mutex_destroy(hdrp_mutex_t *m)
{
#ifdef _WIN32
	DeleteCriticalSection(m);
#else
	pthread_mutex_destroy(m);
#endif
}

void hdrp_mutex_lock(hdrp_mutex_t *m)
{
#ifdef _WIN32
	EnterCriticalSection(m);
#else
	pthread_mutex_lock(m);
#endif
}

void hdrp_mutex_unlock(hdrp_mutex_t *m)
{
#ifdef _WIN32
	LeaveCriticalSection(m);
#else
	pthread_mutex_unlock(m);
#endif
}

char *hdrp_basename_dup(const char *path)
{
	if (!path)
		return bstrdup("");

	const char *base = path;
	for (const char *p = path; *p; p++) {
		if (*p == '/' || *p == '\\')
			base = p + 1;
	}
	return bstrdup(base);
}
