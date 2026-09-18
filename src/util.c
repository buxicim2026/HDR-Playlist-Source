/*
 * util.c — small cross-platform helpers (mutex + path utilities).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#include <obs-module.h>
#include <util/bmem.h>

#ifdef _WIN32
#include <shellapi.h>
#endif

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

/* Opens `url` in the user's default browser. */
void hdrp_open_url(const char *url)
{
	char cmd[768];

	if (!url || !*url)
		return;

#ifdef _WIN32
	if ((INT_PTR)ShellExecuteA(NULL, "open", url, NULL, NULL,
				   SW_SHOWNORMAL) <= 32)
		blog(LOG_WARNING, "[HDR-PL] could not open '%s'", url);
#elif defined(__APPLE__)
	snprintf(cmd, sizeof(cmd), "open \"%s\" >/dev/null 2>&1", url);
	if (system(cmd) != 0)
		blog(LOG_WARNING, "[HDR-PL] could not open '%s'", url);
#else
	snprintf(cmd, sizeof(cmd), "xdg-open \"%s\" >/dev/null 2>&1 &", url);
	if (system(cmd) != 0)
		blog(LOG_WARNING, "[HDR-PL] could not open '%s'", url);
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
