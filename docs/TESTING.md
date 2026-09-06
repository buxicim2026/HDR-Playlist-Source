# Testing / acceptance

## 1. HDR end-to-end (the critical path)

1. OBS → Advanced → Color Format `P010`, Color Space `Rec. 2100 (PQ)`,
   Range `Partial`.
2. Recording → 10-bit HEVC Main10 (or AV1) encoder.
3. Add **HDR Playlist Source**, play a true HDR10 file (e.g. HEVC + P010,
   smpte2084/bt2020), record 10 seconds.
4. Inspect the recording:

```bash
ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,pix_fmt,color_space,color_transfer,color_primaries -show_entries stream_side_data -of json out.mkv
```

Expect `pix_fmt = p010le`, `color_transfer = smpte2084`,
`color_primaries = bt2020`, and side data `Mastering display metadata` /
`Content light level metadata` **present**.

If instead you see 8-bit or 709, check in this order: OBS HDR settings,
`Hardware decoding` checkbox, then the file itself (`ffprobe` the source).

## 2. Gapless switching

Add two clips to a playlist in Loop mode, record the boundary.

* Decode frame-by-frame around the switch point and verify there is **no
  black/all-zero frame** and no large brightness jump:

```bash
ffmpeg -i out.mkv -vf "select='gt(t,5)',showinfo" -f null - 2>&1 | grep -i "pts_time" | head
```

* With HDR active the switch is a cut on the preloaded first frame; slight
  brightness continuity differences between *different* clips are normal.

## 3. Crossfade behaviour

Run an sRGB session and enable *Crossfade* → confirm smooth blending. Run an
HDR (P010/PQ) session → confirm the plugin falls back to a hard cut and logs
it; recorded HDR must still satisfy the §1 ffprobe checks (no PQ clamping).

## 4. Media controls / hotkeys

Verify in the Media Controls dock: play/pause/stop/next/previous state and
time display; bind the plugin hotkeys and exercise them.

## 5. Persistence & locale

* Restart OBS → playlist order/mode/clip index restored.
* Switch UI language to 简体中文 → all property labels translate.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| No `[HDR-PL]` log / source absent | plugin folder layout (`bin/64bit`, `data/locale`); restart OBS |
| Source visible but black | source must be in a visible scene; check `Hardware decoding`; try SDR file |
| Washed-out colors | OBS not in P010/PQ; or source file is SDR being shown on an HDR canvas |
| Switch shows a black gap | Low-memory mode on, or preload failed (check OBS log for ffmpeg errors) |
