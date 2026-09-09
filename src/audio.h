/*
 * audio.h — audio forwarding using a bounded ring buffer.
 *
 * Why this design (rewrite of the original capture+re-emit approach):
 *
 *   The previous version called obs_source_output_audio() from inside the
 *   child's audio capture callback, i.e. from the ffmpeg decode thread.
 *   That drags libobs' whole async audio pipeline (resampler setup, deque
 *   placement, timestamp smoothing, "audio is lagging" recovery) onto a
 *   foreign thread, and any mistake in samples_per_sec silently drops the
 *   audio.
 *
 *   Instead we now use the *pull* model: the child's audio capture callback
 *   only copies PCM into our own bounded ring buffer, and the parent
 *   implements the `audio_render` source callback, which libobs invokes on
 *   the audio thread. No resampler, no deque, no timestamp heuristics, and
 *   libobs skips all of its recovery logic for sources with an audio_render
 *   callback.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <obs.h>

struct hdrp_audio;

struct hdrp_audio *hdrp_audio_create(void);
void hdrp_audio_destroy(struct hdrp_audio *au);

/* Start/stop pulling audio from `child` (the presented slot). */
void hdrp_audio_attach(struct hdrp_audio *au, obs_source_t *child);
void hdrp_audio_detach(struct hdrp_audio *au);

/* Drop everything buffered (clip change / stop). */
void hdrp_audio_flush(struct hdrp_audio *au);

/* Use this as the parent source's `audio_render` callback. */
bool hdrp_audio_render(struct hdrp_audio *au, uint64_t *ts_out,
		       struct obs_source_audio_mix *audio_output,
		       uint32_t mixers, size_t channels, size_t sample_rate);
