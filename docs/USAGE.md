# 使用说明 / Usage

本插件（**HDR Playlist Source**）让你用一个源播放**视频播放列表**，并且全程保留 HDR：
视频像素始终由 OBS 内置的 `ffmpeg_source` 解码/上屏，插件自己不碰像素，只做列表、切换、音频转发与画质策略。

---

## 1. 安装

下载对应平台的压缩包后，**保持包内目录结构**，让 `HDR-Playlist-Source`（macOS 是 `HDR-Playlist-Source.plugin`）文件夹完整落进 OBS 的插件目录：

| 平台 | 放这里 |
| --- | --- |
| Windows（当前用户，推荐） | `%APPDATA%\obs-studio\plugins\` |
| Windows（所有用户） | `C:\ProgramData\obs-studio\plugins\` |
| Linux | `~/.config/obs-studio/plugins/` |
| macOS (Apple Silicon) | `~/Library/Application Support/obs-studio/plugins/` |

安装后的结构（Windows/Linux 为例）：

```
HDR-Playlist-Source/
├── bin/64bit/HDR-Playlist-Source.dll      # Linux 为 .so；macOS 在 .plugin/Contents/MacOS/
├── data/locale/{en-US,zh-CN}.ini
└── README.md
```

重启 OBS，日志里应出现：

```
[HDR-PL] HDR Playlist Source plugin loaded (v0.0.3)
[HDR-PL] running on OBS 31.x.x
[HDR-PL] source registered
```

> 需要 **OBS Studio 31.0 或更新版本**。老版本会打印警告，HDR/无缝切换可能异常。

## 2. 添加源 / 快速上手

1. 「来源」→ `+` → **HDR Playlist Source**
2. 在**播放列表**里加入媒体文件；或点 **「添加文件夹…」** 选一个文件夹（其中媒体文件会自动加入）；也可以直接把文件夹路径拖/填进列表
3. 选择播放模式：顺序 / 循环 / 随机
4. 点**确定**。源一旦可见（预览或直播）就会自动开始播放
   - 若没自动开始，用 OBS 媒体控制条，或源热键里的「播放/暂停」

**播放列表中可以直接使用的条目**

| 类型 | 示例 | 说明 |
| --- | --- | --- |
| 本地文件 | `D:\media\a.mkv` | 支持 mp4/mkv/mov/ts/m2ts/mxf/flv/avi/webm/gif 及常见音频 |
| 文件夹 | `D:\media\` | **自动递归展开**（最多 4 层），并把展开结果写回列表 |
| 网络流 | `https://host/live.m3u8` | HLS（m3u8）、MPEG-DASH（mpd）、`rtmp(s)/rtsp/srt/rist/udp/tcp://`、普通 http(s) |

## 3. 属性面板

属性面板是**纯静态配置**：没有任何实时状态/进度读数，也不会每秒自我刷新（因此
不会出现"滚到哪儿都被顶回『交叉淡化转场』"的情况）。播放进度请看 OBS 的媒体
控制条，播放状态看日志里的 `[HDR-PL] hb:` 心跳行。

> 关于属性窗口顶部那块**会动的画面**：那是 OBS 自己的预览控件
> （`UI/window-basic-properties.cpp` 里的 `ui->preview` + `DrawPreview`），
> 只要源是"输入类 + 带 `OBS_SOURCE_VIDEO`"就一定会显示，**插件无法关闭**
> （去掉该标志就等于源不出画面）。它只是把当前这一帧缩放重画一次，不会额外
> 解码，开销很小；不想看到它，关掉属性窗口即可。

## 4. 画质与性能选项

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| **硬件解码** | 开 | 保留 10bit P010 HDR 帧不被降级。若某编码无硬解会回退软解（4K 可能跑不动） |
| **自适应画布尺寸** | 开 | 按 OBS **基础画布**分辨率输出并等比缩放视频，避免每帧额外的全分辨率缩放/色彩转换。关掉则输出片源原生分辨率 |
| **低内存模式** | **开** | 只创建一个解码器、不预载下一片段，内存/显存占用约减半（4K HDR 尤其明显）。**关闭**后才会创建第二个解码器实现无缝切换 |
| **交叉淡化转场** | 关 | 片段间淡入淡出。开启后：HDR 画布使用 16F 中间缓冲保留高光，缓冲分辨率封顶 1080p 且转场结束立即释放 |
| **播放速度** | 100% | 50–200% |
| **HDR/SDR 混播策略** | 自动 | 见下一节 |
| **源隐藏时** | 停止并在再次可见时重播 | 另可选：暂停并续播 / 始终播放 / 停止并播下一首 |

> 内存提示：每个解码器都会缓存解码帧（4K HDR 单帧约 25 MB）。所以「低内存模式」是最大的内存开关；需要无缝切换时再关掉它。

## 5. 隔行片源（1080i / 576i 等）

隔行素材**必须**开启去隔行，否则画面会拉丝。带 **2x** 的选项会让每个场输出一帧：

| 选项 | 结果 | 说明 |
| --- | --- | --- |
| 关闭（逐行片源） | 原样 | **逐行素材请保持此项**，否则白费 GPU |
| Yadif 2x | 1080i25 → **50p** | 效果最好，GPU 开销最大 |
| 线性 2x | 1080i25 → **50p** | 折中 |
| 混合 2x | 1080i25 → **50p** | 最省 GPU |
| Yadif / 线性混合 | 1080i25 → 25p | 只去隔行，不补帧 |
| 丢弃一场 | 帧率减半 | 最省，但会丢运动信息 |

**前提**：想看 50p，OBS 的视频帧率必须 ≥ 50（建议设 60）。30fps 画布最多只能显示 30 帧。
OBS 的帧数据里没有"是否隔行"标记，所以无法自动识别，只能手动选。

## 6. HDR 与色彩策略

真实 HDR 输出需要 OBS 侧配置正确：

1. 设置 → 高级 → 色彩格式 **P010**
2. 色彩空间 **Rec. 2100 (PQ)** 或 **Rec. 2100 (HLG)**
3. 色彩范围 **Partial**
4. 输出编码器：10bit **HEVC (Main10)** 或 **AV1**，且平台支持 HDR 推流

插件会**每次读取 OBS 实际配置**并写进日志：

```
[HDR-PL] canvas 1920x1080, output 1920x1080, format=.., colorspace=.. -> SDR session
```

| OBS 输出配置 | 插件行为 |
| --- | --- |
| 非 HDR（如 NV12 + Rec.709） | **全部按 SDR 播放**，不做 HDR 处理（HDR 片源由 OBS 正常转 SDR） |
| HDR（P010 + Rec.2100） | HDR 片源按 HDR 直通；SDR 片源仍按 SDR 上报（OBS 负责扩展） |

「HDR/SDR 混播策略」可覆盖该行为：

- **自动**（推荐）：SDR 按 SDR、HDR 按 HDR 上报，不强制转换
- **强制 PQ / 强制 HLG**：强制按 HDR 呈现（OBS 31 的源级色彩模型只能区分 SDR/HDR，PQ 与 HLG 的具体输出由 OBS 的「色彩空间」全局设置决定，因此两者在此版本行为一致）
- **强制 SDR**：强制按 SDR 呈现

> 说明：OBS 31 的 `gs_color_space` 只有 `SRGB / SRGB_16F / 709_EXTENDED / 709_SCRGB` 四种，插件不会伪造 PQ 标记，所以 SDR 片源不会被误判成 HDR —— 这正是其它列表插件偏色的原因。

## 7. 控制方式

- **媒体控制坞**：播放/暂停、重启、停止、下一个、上一个
- **热键**（设置 → 热键，搜 `HDR-Playlist-Source`）：播放/暂停、重启、停止、下一个文件、上一个文件
- 属性面板里的 **「▶ 立即切换 / Play now」**：任何状态下一键强制起播

## 8. 播放列表编辑

编辑后**立即生效**（点确定时）：

- 正在播放的文件仍在列表里 → 保持播放，只刷新"下一首"的预载
- 正在播放的文件被删掉 → 立即切到新列表的当前项
- 当前未播放 → 下次播放使用新列表

## 9. 常见问题 / 日志速查

| 现象 | 先看日志 | 结论 |
| --- | --- | --- |
| 没声音 | 有 `[HDR-PL] audio forwarding active: N ch @ 48000 Hz` 吗？ | 有 = 音频链路通了，检查 OBS 混音器音量/静音；没有 = 音频捕获未触发，请把日志发我 |
| 画面卡、掉帧 | `[HDR-PL] hb: ... pos=..s`，连续两条 `pos` 是否前进？ | `pos` 不动并带 `decode too slow` 警告 = **解码跟不上**（4K HDR 软解常见），需硬解或换片源分辨率 |
| 音频卡顿/爆音 | `[HDR-PL] hb: ... fill=..`（环形缓冲剩余帧数） | `fill` 长期顶在上限 = 音频产出快于消费，请把日志发我 |
| 播放列表不生效 | `[HDR-PL] playlist updated; ...` | 有 `current clip kept` = 当前片段仍在列表，播完自然会到新条目 |
| 文件夹没展开 | `[HDR-PL] expanded folder '...' -> N file(s)` | N=0 表示该目录里没有识别到的媒体文件 |
| 启动即失败 | `[HDR-PL]` 与 `Failed to create source` | OBS 版本低于 31.0，或插件目录结构放错 |

其它提示：

- **直播流不会自动切下一条**（没有"播放结束"事件）。需要"每条播 N 分钟后切换"可以提需求。
- **4K HDR 素材**如果硬解不支持而回退软解，任何插件都无法跑满帧率——请优先保证硬解可用。
- SDR 片源在 HDR 会话下由 OBS 做标准 SDR→HDR 扩展；若你觉得观感不对，可试「强制 SDR」对比。

---

## English summary

**Install:** extract so the `HDR-Playlist-Source` folder (`.plugin` on macOS) lands in OBS's plugin directory — `%APPDATA%\obs-studio\plugins\` (Windows, current user), `C:\ProgramData\obs-studio\plugins\` (all users), `~/.config/obs-studio/plugins/` (Linux), `~/Library/Application Support/obs-studio/plugins/` (macOS). Restart OBS; requires OBS Studio 31.0+.

**Playlist entries** can be local files, **folders** (auto-expanded recursively, 4 levels, also via the *Add folder…* picker) and **network streams** (HLS `.m3u8`, MPEG-DASH `.mpd`, `http(s)/rtmp(s)/rtsp/srt/rist/udp/tcp`).

**Properties panel** shows a live status line (current file, `[index/total]`, elapsed/total or "live") plus the detected session (canvas size · HDR/SDR · memory mode), refreshed every second.

**Options:** hardware decoding (keeps 10-bit P010), **Adapt to canvas size** (on), **Low-memory mode** (on by default — one decoder, no preload; turn off for gapless), crossfade (off by default; on an HDR canvas it uses a capped-1080p 16F intermediate released right after the fade), playback speed, **Deinterlacing** (2x modes turn 1080i25 into 50p — the OBS canvas must run at ≥ 50 fps), HDR/SDR policy (Auto / Force PQ / Force HLG / Force SDR), and hidden-source behaviour.

**Colour policy:** the plugin reads OBS's actual output configuration (`obs_get_video_info`) each tick. A non-HDR session plays everything as SDR; HDR handling is only used when OBS outputs P010 + Rec.2100. SDR clips always report as SDR, so OBS applies its normal SDR→HDR expansion instead of mislabelling them.

**Diagnostics:** `[HDR-PL]` log lines cover audio forwarding, a 5-second heartbeat (`pos` stuck ⇒ decode too slow; `fill` pinned ⇒ audio consumer issue), canvas/HDR detection, folder expansion and live playlist updates.
