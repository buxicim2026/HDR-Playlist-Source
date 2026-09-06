# Usage

## Install

Extract the package so the top-level folder keeps its layout:

| Platform | Destination |
| --- | --- |
| Windows | `%APPDATA%\obs-studio\plugins\HDR-Playlist-Source\` |
| Linux | `~/.config/obs-studio/plugins/HDR-Playlist-Source/` |
| macOS | `~/Library/Application Support/obs-studio/plugins/HDR-Playlist-Source.plugin` |

Restart OBS. The plugin logs `[HDR-PL] HDR Playlist Source plugin loaded`.

## Requirements for real HDR

OBS must be configured for HDR output **and** the media files must be HDR:

1. Settings → Advanced → Color Format **P010**
2. Settings → Advanced → Color Space **Rec. 2100 (PQ)** (or HLG to taste)
3. Settings → Advanced → Color Range **Partial**
4. Settings → Output → Recording/Streaming: 10-bit **HEVC (Main10)** or
   **AV1**; the target platform must support HDR streaming.

Without those settings OBS tone-maps HDR → SDR, exactly like any other source.
The plugin's job is only to never be the thing that destroys the HDR signal.

## Add the source

1. Sources → `+` → **HDR Playlist Source**
2. In *Playlist*, add media files (or keep them on disk and add files).
3. Pick a playback mode: Sequential / Loop playlist / Shuffle.
4. Keep **Hardware decoding** enabled — this is what lets the underlying
   media source hand over 10-bit P010 frames instead of 8-bit.
5. Click OK. The first clip starts as soon as the source becomes visible.

## Controls

The source registers as `OBS_SOURCE_CONTROLLABLE_MEDIA`, so it appears in the
OBS **Media Controls** dock (play/pause/stop/next/previous) and its hotkeys
(*Play/Pause*, *Restart*, *Stop*, *Next file*, *Previous file*) can be bound
in Settings → Hotkeys.

## Behaviours

* **Gapless switching**: while clip N plays, clip N+1 is preloaded on the
  idle decoder and parked on its first frame; at the end of N the slots swap.
  When HDR is active a **hard cut** is used deliberately (a crossfade through
  an 8-bit intermediate would clamp PQ); in an sRGB session an optional
  crossfade is available under *Crossfade*.
* **Low-memory mode**: disables preloading — saves memory, may stutter.
* **When hidden**: choose Stop&restart / Pause&resume / Always play /
  Stop & play next (affects scene switching).
* **Playback speed** 50–200%.
* Files and the clip index are persisted with the scene collection.

## Folder import

Picking a whole folder from the properties panel is a roadmap item (the
underlying `hdrp_playlist_add_folder()` scanner already expands folders
recursively). For now, add the individual files, or drag them in.
