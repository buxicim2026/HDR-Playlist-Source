# HDR-Playlist-Source

OBS Studio 插件：以**端到端 HDR 保真** + **近乎无缝切换**播放视频播放列表。

OBS 内置媒体源对 HDR（P010 / Rec.2100 PQ/HLG）支持正确，但一个源只能播一个文件；第三方 VLC 媒体源支持列表播放，却经由 libVLC 8bit 回调破坏 10bit 深度与 PQ 曲线，导致颜色灰白。本插件用两个私有 `ffmpeg_source`（A/B）组成切换器：列表决定播什么，插件本身不碰任何像素，HDR 完全交给 OBS 原生 P010 管线处理。

---

## 中文使用说明（v0.0.3）

### 1. 安装

到 [Releases](https://github.com/buxicim2026/HDR-Playlist-Source/releases) 下载对应平台的 `0.0.3` 压缩包：

| 平台 | 包 | 放置位置 |
|---|---|---|
| Windows | `...-obs-win-x64-0.0.3.zip` | 解压后把 `obs-plugins\64bit\HDR-Playlist-Source.dll` 与 `data\obs-plugins\HDR-Playlist-Source\` 复制到 OBS 安装目录的对应 `obs-plugins\`、`data\obs-plugins\` 下 |
| Linux x64 | `...-obs-linux-x64-0.0.3.tar.gz` | 解压后按内部目录结构放到 `~/.config/obs-studio/plugins/`（或 OBS 插件目录） |
| macOS (Apple Silicon) | `...-obs-macos-arm64-0.0.3.tar.gz` | 解压后放到 `~/Library/Application Support/obs-studio/plugins/` |

重启 OBS 后，在「源」中添加 **HDR Playlist Source**。

### 2. HDR 输出配置（重要）

OBS → 设置 → 高级：

- 色彩格式：**P010**
- 色彩空间：**Rec. 2100 (PQ)** 或 **Rec. 2100 (HLG)**
- 色彩范围：**Partial**
- 编码器：HEVC 10bit 或 AV1 10bit（配合你的 HDR 推流服务）
- 本地预览 HDR 需要 HDR 显示器并在 OBS 中开启 HDR 预览

### 3. 快速上手

1. 场景中添加源「HDR Playlist Source」；
2. 属性窗口中把媒体文件加进「播放列表」；
3. 源进入可见（预览或直播）即自动开始播放；若没开始，用 OBS 媒体控制条或源热键里的「播放/暂停」；
4. 第一个文件播完后自动切到下一个（默认低内存模式不预载；需要无缝切换请关闭「低内存模式」）。

### 4. 选项说明

| 选项 | 含义 |
|---|---|
| 播放模式 | 顺序 / 循环 / 随机 |
| 硬件解码 | 建议开启，保留 10bit P010 HDR 帧不做降级 |
| 播放速度 | 50–200% |
| 交叉淡化转场（默认开启） | 片段间交叉淡化。HDR 画布使用 16F 中间缓冲保留高光；缓冲分辨率上限 1080p，转场结束立即释放显存 |
| 低内存模式（**默认开启**） | 只创建一个解码器、不预载下一片段，内存/显存占用约减半（4K HDR 尤其明显）。关闭后才会创建第二个解码器实现无缝切换 |
| 自适应画布尺寸（默认开） | 按 OBS 基础画布分辨率输出并等比缩放视频，避免每帧额外的全分辨率缩放/色彩转换 |
| 去隔行 | 逐行片源选「关闭」；隔行片源选 2x 系列，1080i25 → **50p**（OBS 画布帧率需 ≥ 50，建议 60） |
| 播放列表 | 支持拖入/填入**文件夹**（自动展开）与**网络地址**（HLS `.m3u8`、`.mpd`、rtmp/rtsp/srt 等） |
| HDR/SDR 混播策略 | **自动**（默认，推荐）：SDR 文件按 SDR 直出、HDR 文件按 HDR 直出，不做强制转换；也可强制全部按 HDR（Rec.2100 PQ/HLG）或强制全部按 SDR 输出 |
| 源隐藏时 | 停止并再次可见时重播 / 暂停并续播 / 始终播放 / 停止并播下一个 |

> 注：OBS 31 的源级色彩模型只能区分 **SDR 与 HDR**（具体 PQ 或 HLG 由 OBS「高级 → 色彩空间」全局设置决定）。因此「强制 PQ」与「强制 HLG」在该版本下都表现为"强制 HDR 直通"，两者保留为独立选项以便兼容后续 OBS 版本；HLG 文件本身会被"自动"模式正确识别并按 HDR 处理。

> 提示：播放列表编辑后**立即生效**——若正在播放的文件仍在列表则保持播放并刷新预载；若已被删除则自动切到新列表当前项。

### 5. 控制方式

- OBS 媒体控制条（播放/暂停、重启、停止、下一个、上一个）；
- 源级热键：播放/暂停、重启、停止、下一个文件、上一个文件。

### 6. 常见问题

- **没有声音**：本版不再自造播放时钟，而是**直接取用子源已被 OBS 混好的音频**（`obs_source_get_audio_mix` + `obs_source_get_audio_timestamp`，与场景处理普通媒体源的方式完全一致），采样率/时间戳/声道对齐全部交给 OBS。若仍无声，请把 OBS 日志（帮助 → 日志文件）里含 `[HDR-PL]` 的片段发我，心跳行 `audio=2ch fill=…` 会直接指出是哪一侧没有数据。
- **添加后黑屏、不自动播放**：让源进入可见（预览或直播）即会起播；也可用 OBS 媒体控制条或源热键「播放/暂停」。
- **内存占用高 / 崩溃**：默认已开启「低内存模式」——此时**只创建一个解码器**，第二个解码器（及其解码帧缓存）根本不会被创建，4K HDR 下可省掉数百 MB。只有当你关闭该模式追求无缝切换时，才会创建第二个解码器；转场中间缓冲也封顶 1080p 且用完立即释放。
- **SDR 视频颜色不对**：把「HDR/SDR 混播策略」保持「自动」——自动模式下 SDR 文件不会被误标成 HDR；若仍异常再选「强制 SDR」对比。

### 7. 版本历史

- **v0.0.3**（当前正式版）：
  - **音频重写（真·修复无声 / 崩溃）**：父源的 `audio_render` 改为拉取 `obs_source_get_audio_mix()` + `obs_source_get_audio_timestamp()`——子源音频本来就被 libobs 每个音频 tick 混好（已重采样到混音率、时间戳归 OBS 管理），与场景拉取每个 item 的方式一字不差。旧的"自造时间戳（`os_gettime_ns()`）"会让时间戳落在混音窗口之外，libobs 于是每次都跳过本源 → **永久静音**；同一个偏差还会让场景算出越界偏移 → **崩溃**。自带环形缓冲降级为「片段刚开始/子源尚无音频」时的兜底，且无数据时返回 false 而不是喂一段带假时间戳的静音
  - **属性面板不再每秒刷新**：移除实时状态行与 `obs_source_update_properties()` 调用——之前每秒重建一次属性面板，正是**「鼠标滚到底部又弹回『交叉淡化转场』」**的元凶，也顺带省掉每秒一次的 UI 重建开销。面板恢复为纯静态配置
  - **音频改为拉取模型**：彻底不再从解码线程调用 `obs_source_output_audio`（旧做法会因采样率/时间戳/线程问题静音或崩溃）；音频改为由 OBS 音频线程经 `audio_render` 回调拉取，自带环形缓冲仅作兜底
  - **修正子源活动登记**：实现 `enum_active_sources`，子源激活/显示/音频枚举才正确
  - **播放列表编辑即时生效**：保留当前播放项或在被删除时立即切到新列表当前项，并刷新预载
  - **性能**：按画布尺寸输出（避免每帧全分辨率缩放/色彩转换）、修复预载每帧 seek 风暴、5 秒心跳诊断日志
  - **自适应配置**：读取 OBS 实际输出配置，非 HDR 会话一律按 SDR 播放，HDR 会话才启用 HDR 策略
  - **去隔行**：新增 2x 系列选项，1080i25 → 50p（画布帧率需 ≥ 50）
  - **新功能**：文件夹自动展开（含「添加文件夹…」选择器）、网络流播放（HLS `.m3u8`、`.mpd`、rtmp/rtsp/srt 等）
  - 详细说明见 [`docs/USAGE.md`](docs/USAGE.md)
- **v0.0.2**（预发行版）：修复音频 use-after-free、新增 `activate` 自动起播与「Play now」、低内存模式释放空闲解码器。
- **v0.0.1**（预发行版）：首个可用版本。

---

## English summary

*Play a video playlist in OBS with real HDR passthrough (P010 / Rec.2100 PQ·HLG) and gapless A/B switching, without the 8-bit crushing of VLC-based playlist sources.*

**How it works:** the plugin owns two private instances of OBS's built-in `ffmpeg_source` (slots A/B). The playlist picks what to play; the switcher preloads the next clip on the idle slot and promotes it when the current one ends. The plugin never touches pixels — HDR colour stays on OBS's native, verified pipeline.

Features:

- Sequential / Loop / Shuffle playlist modes
- **Folders** dropped/typed into the list are expanded into their media files automatically
- **Network streams**: HLS (`.m3u8`), MPEG-DASH (`.mpd`), RTMP/RTSP/SRT and plain http(s) URLs
- **Live status in the properties panel**: current file, playlist position, elapsed/total time, detected canvas/HDR session
- **Deinterlacing** incl. 2x modes (1080i25 → 50p; canvas fps must be ≥ 50)
- **Canvas-adaptive output**: reports/renders at the OBS base canvas size and aspect-fits, avoiding a per-frame full-resolution scale/colour pass
- Gapless A/B preload & switch
- Crossfade transition, default 200 ms — on an **HDR canvas** it renders through a capped-1080p 16F intermediate (released immediately after the fade) so PQ/HLG highlights are never clamped
- **SDR passthrough**: an SDR clip reports as SDR and OBS applies its normal SDR→HDR expansion, instead of being mislabelled HDR (the bug that washes out other playlist plugins)
- Colour-space policy: Auto / Force PQ / Force HLG / Force SDR
- OBS media controls + source hotkeys (play/pause, restart, stop, next, prev)
- Live playlist editing — takes effect immediately, current clip keeps playing when still present
- Visibility policy (stop&restart / pause&resume / always / stop&play-next), low-memory mode (no preload, releases the idle decoder)
- en-US / zh-CN UI, MIT license, no vendored FFmpeg

**Requirements:** OBS Studio 31.0+ · Windows x64 / Linux x64 / macOS (Apple Silicon). For HDR output use Advanced → Color Format **P010**, Color Space Rec.2100 PQ or HLG, Partial range, 10-bit HEVC/AV1 encoder.

Docs: `docs/USAGE.md` · `docs/BUILD.md` · `docs/TESTING.md`

License: MIT (links only against libobs's public API; never bundles or links FFmpeg, no GPL/LGPL copyleft implications).
