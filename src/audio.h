/*
 * audio.h — forwards the presented clip's audio to the parent source.
 *
 * Why the push model: obs_source_output_audio() is the API libobs is built
 * around for sources that produce PCM (every media source uses it). libobs
 * copies the data into its own per-source buffer, resamples and aligns it with
 * the mixer clock, and hands it to the audio thread — the same code path that
 * already works for every other source in the scene.
 *
 * The alternative (implementing the `audio_render` callback) forces the source
 * to own the clock and the buffering, and libobs drops any block whose
 * timestamp does not land inside the current mix window — which is what made
 * this source silent. So: no audio_render callback, no self-invented clock.
 *
 * The child's audio capture callback runs on the child's decode thread, which
 * is exactly how OBS's own media sources push their audio.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <obs.h>

struct hdrp_audio;

struct hdrp_audio *hdrp_audio_create(void);
void hdrp_audio_destroy(struct hdrp_audio *au);

/* Bind the source that receives the audio (the parent source). */
void hdrp_audio_bind(struct hdrp_audio *au, obs_source_t *parent);

/* Start/stop forwarding the audio of `child` (the presented slot). */
void hdrp_audio_attach(struct hdrp_audio *au, obs_source_t *child);
void hdrp_audio_detach(struct hdrp_audio *au);

/* Drop bookkeeping (clip change / stop). */
void hdrp_audio_flush(struct hdrp_audio *au);

/* Diagnostics: frames forwarded since the previous call, and the channel count
 * of the child we are currently capturing. */
uint64_t hdrp_audio_take_frames(struct hdrp_audio *au);
uint32_t hdrp_audio_channels(struct hdrp_audio *au);
