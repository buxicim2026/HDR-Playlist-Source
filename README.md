# HDR-Playlist-Source

OBS Studio source plugin: play a playlist of videos with **end-to-end HDR
preservation** and **gapless switching**.

## Problem

* OBS's built-in Media Source plays HDR (P010 / Rec.2100 PQ/HLG) correctly,
  but one source == one file — switching is manual and annoying.
* The third-party VLC Video Source plays playlists, but routes through
  libVLC's 8-bit callbacks, destroying 10-bit depth and the PQ transfer
  function → washed-out colors.

## Solution

`HDR-Playlist-Source` wraps **two private instances of OBS's built-in
`ffmpeg_source`** (A/B). The playlist module decides *what* to play, the
switcher preloads the next file on the idle slot and swaps at the end of the
current one, and the plugin itself never touches a single pixel → HDR is
handled entirely by OBS's native, already-verified P010 pipeline.

Features:

* Playlist of files or whole folders; sequential / loop / shuffle modes
* Gapless A/B switching (next file preloaded and parked on its first frame)
* Optional HDR-safe crossfade; auto fallback to hard cut when unsupported
  (never tone-maps HDR down to SDR as a side effect)
* OBS media controls (play/pause/stop/next/prev) and source hotkeys
* Color-space reporting delegated to the active child source — this is the
  piece most playlist plugins get wrong (they report sRGB and crush HDR)
* Configurable visibility behaviour, low-memory mode, mixed HDR/SDR policy
* en-US / zh-CN UI, MIT licensed

## Requirements

* OBS Studio **31.0 or newer**
* Windows x64 / Linux x64 / macOS (Apple Silicon)
* For actual HDR output: OBS → Settings → Advanced → Color Format **P010**,
  Color Space **Rec. 2100 (PQ)**, Color Range **Partial**, and a 10-bit
  HEVC/AV1 encoder (Main10) for recording/streaming.

## License

MIT. This plugin links only against libobs through its public API and never
bundles or links FFmpeg, so it has no GPL/LGPL copyleft implications.

See `docs/USAGE.md` / `docs/BUILD.md` / `docs/TESTING.md`.
