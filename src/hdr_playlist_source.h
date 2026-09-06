/*
 * hdr_playlist_source.h — the "HDR Playlist Source" input.
 */
#pragma once

/* Registers hdr_playlist_source (id HDRP_SOURCE_ID). Called from
 * obs_module_load() once libobs is ready. */
void hdrp_register_sources(void);
