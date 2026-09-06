/*
 * audio.h — forwards the active child's audio to the parent source.
 *
 * The switcher keeps two private ffmpeg_source children, but only the one
 * currently presented should contribute audio. This module attaches an audio
 * capture callback to that single child, rebases its timestamps onto a
 * continuous per-playback timeline and applies a short fade-in at every
 * clip boundary to suppress clicks.
 */
#pragma once

#include <obs.h>

struct hdrp_audio;

struct hdrp_audio *hdrp_audio_create(obs_source_t *parent);
void hdrp_audio_destroy(struct hdrp_audio *au);

/* Start forwarding `child`'s audio. Detaches the previous child. */
void hdrp_audio_attach(struct hdrp_audio *au, obs_source_t *child);
void hdrp_audio_detach_current(struct hdrp_audio *au);

/* Drop timeline continuity (used on first play / user restart). */
void hdrp_audio_reset_timeline(struct hdrp_audio *au);
