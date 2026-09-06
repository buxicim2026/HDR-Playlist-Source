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
`ffmpeg_source`** (A/B). The playlist decides *what* to play, the switcher
preloads the next file on the idle slot and swaps at the end of the current
one, and the plugin itself never touches a single pixel → HDR is handled
entirely by OBS's native, already-verified P010 pipeline.

Features:

* Playlist of media files; **Sequential / Loop / Shuffle** modes
* **Gapless A/B switching** — the next clip is opened and parked on its first
  frame while the current one plays
* **HDR-safe cuts**: on an HDR (PQ/HLG) canvas the switch is deliberately a
  hard cut (an 8-bit crossfade intermediate would clamp PQ values). In an
  sRGB session an optional crossfade is available
* OBS media controls (play/pause/stop/next/prev), source hotkeys, playback
  speed 50–200%
* Colour-space reporting delegated to the active child — the piece most
  playlist plugins get wrong (they report sRGB and crush HDR)
* Visibility behaviour (stop&restart / pause&resume / always / stop&play
  next), low-memory mode
* en-US / zh-CN UI, MIT licensed, no FFmpeg vendored

Roadmap: folder-import button in the UI (the recursive folder scanner already
exists in `playlist.c`), media-insert transitions, per-clip metadata.

## Requirements

* OBS Studio **31.0 or newer**
* Windows x64 / Linux x64 / macOS (Apple Silicon)
* For HDR output: OBS → Advanced → Color Format **P010**, Color Space
  **Rec. 2100 (PQ)** or HLG, Range **Partial**, 10-bit HEVC/AV1 encoder.

## Docs

* `docs/USAGE.md` — install, HDR configuration, controls
* `docs/BUILD.md` — build without compiling OBS
* `docs/TESTING.md` — ffprobe-based HDR acceptance checklist

## License

MIT. This plugin links only against libobs through its public API and never
bundles or links FFmpeg, so it has no GPL/LGPL copyleft implications.
