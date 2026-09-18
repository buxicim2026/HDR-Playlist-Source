/*
 * audio.h — audio forwarding from the presented clip to the parent source.
 *
 * Two paths, in order of preference:
 *
 *  1. Pull the child's own mix. The slot children are registered as active
 *     children (obs_source_add_active_child + enum_active_sources), so libobs
 *     already renders their audio on every audio tick: it resamples to the
 *     mixer rate and stores the result, timestamped in its own timeline, in
 *     the child's obs_source_audio_mix. Copying that out is exactly what a
 *     scene does for each of its items, so the parent ends up behaving like a
 *     plain media source: same rate, same clock, same mixer alignment.
 *
 *  2. Fallback ring buffer, fed by the child's audio capture callback, used
 *     only while the child has nothing buffered yet (clip start, audio-less
 *     file, stream reconnect). That path has to invent its own timestamp,
 *     which is why it is not the primary one: libobs aligns every source by
 *     timestamp, and a value outside the current mix window drops the audio.
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

/* Primary path: copy the child's current audio mix into the parent's
 * audio_render output and report the child's timestamp. Returns false when the
 * child has no audio to hand over right now (libobs then treats the parent as
 * having no audio for this tick, which is what a real media source does). */
bool hdrp_audio_copy_child(obs_source_t *child, uint64_t *ts_out,
			   struct obs_source_audio_mix *audio_output,
			   uint32_t mixers, size_t channels);

/* Fallback path: emit from our own ring buffer. Returns false when there is
 * nothing buffered. */
bool hdrp_audio_render(struct hdrp_audio *au, uint64_t *ts_out,
		       struct obs_source_audio_mix *audio_output,
		       uint32_t mixers, size_t channels, size_t sample_rate);

/* Diagnostics. */
size_t hdrp_audio_fill(struct hdrp_audio *au);     /* buffered frames */
uint32_t hdrp_audio_channels(struct hdrp_audio *au);
/* True once the child's capture callback has delivered audio. */
bool hdrp_audio_capture_active(struct hdrp_audio *au);
