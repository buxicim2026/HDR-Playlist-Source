/*
 * HDR-Playlist-Source — OBS module entry point.
 *
 * Registers the "HDR Playlist Source" input and enforces the minimum OBS
 * version.  The source itself lives in hdr_playlist_source.c (added when the
 * source module lands); this file only owns module lifecycle.
 */

#include <obs-module.h>

#include "hdr_playlist_source.h"
#include "plugin-support.h"

/* OBS encodes the runtime version as (major<<24)|(minor<<16)|(patch<<8). */
static void hdrp_log_runtime_obs(void)
{
	uint32_t v = obs_get_version();
	blog(LOG_INFO, "[HDR-PL] running on OBS %u.%u.%u",
	     (v >> 24) & 0xffu, (v >> 16) & 0xffu, (v >> 8) & 0xffu);
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[HDR-PL] HDR Playlist Source plugin loaded (v%s)",
	     HDRP_VERSION);
	hdrp_log_runtime_obs();

	if (obs_get_version() < HDRP_MIN_OBS_VERSION) {
		/* The module is built against 31.0.0 headers and is NOT linked
		 * against a real libobs, so symbols are resolved at load time
		 * with no ABI validation. Continuing here used to produce
		 * mismatched structs / missing symbols and crash OBS. */
		blog(LOG_ERROR,
		     "[HDR-PL] OBS 31.0.0 or newer is required (running %u.%u.%u); "
		     "refusing to load this plugin",
		     (obs_get_version() >> 24) & 0xffu,
		     (obs_get_version() >> 16) & 0xffu,
		     (obs_get_version() >> 8) & 0xffu);
		return false;
	}

	hdrp_register_sources();
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[HDR-PL] HDR Playlist Source plugin unloaded");
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(HDRP_MODULE_NAME, "en-US")
