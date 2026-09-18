# HDR Playlist Source

OBS 的**HDR 播放列表源**：把一串视频文件（本地文件、文件夹、网络流）排队连续播放，走 OBS 原生 HDR 管线（P010 / Rec.2100 PQ·HLG 直通），不做 8bit 降级。

---

## 1. 适用版本

- **适用于 OBS Studio 30 及以上版本。**
- **版本号 30 以下的 OBS 兼容性未知**（插件启动时若检测到低于 30.0.0 会在日志里给出警告）。

## 2. 安装（Windows 11）

1. 解压安装包，里面会有两个文件夹：`bin` 和 `data`；
2. 把 **`bin` 文件夹里面的文件**复制到：`"安装盘符"\obs-studio\obs-plugins\64bit\`；
3. 把 **`data` 文件夹里面的文件**复制到：`"安装盘符"\obs-studio\data\obs-plugins\HDR-Playlist-Source\`
   （**这个文件夹需要你自行提前创建**）；
4. 复制过程中需要**管理员权限**；
5. 重启 OBS，在「来源」里就能添加 **HDR Playlist Source**。

> 注意：`data` 里的内容必须一起复制，尤其是 `data\effects\crossfade.effect`——缺了它交叉淡化会失效、退化成硬切。

### Linux x64 / macOS (Apple Silicon)

- Linux：解压后把 `HDR-Playlist-Source` 放到 `~/.config/obs-studio/plugins/`
- macOS：解压后把 `HDR-Playlist-Source.plugin` 放到 `~/Library/Application Support/obs-studio/plugins/`

## 3. HDR 输出配置（要看到真 HDR 才需要）

OBS → 设置 → 高级：

- 色彩格式：**P010**
- 色彩空间：**Rec. 2100 (PQ)** 或 **Rec. 2100 (HLG)**
- 色彩范围：**Partial**
- 编码器：HEVC 10bit 或 AV1 10bit（配合你的 HDR 推流服务）
- 本地观看 HDR 需要 HDR 显示器，并在 OBS 中开启 HDR 预览

不满足上面的配置时，插件会自动按 SDR 播放（由 OBS 正常做 HDR→SDR 的色调映射），不会硬塞 HDR。

## 4. 快速上手

1. 「来源」→ `+` → **HDR Playlist Source**；
2. 在**播放列表**里加入媒体文件；也可以点「添加文件夹」选一个文件夹（其中的媒体文件会自动展开加入），或者直接把文件夹路径/网络地址填进列表；
3. 选播放模式：顺序 / 循环 / 随机；
4. 点**确定**。源一旦可见（预览或直播）就会自动开始播放；
5. 一个文件播完后自动切到下一个（默认「低内存模式」不预载；需要无缝切换请关掉它）。

## 5. 属性面板

面板顶部有两行**当前媒体信息**（在打开面板或切换片段时更新，不按秒刷新）：

```
1920x1080 · Rec.709 (SDR) · SDR · audio 2ch · deint off
1920x1080 @ 60fps · Rec.2100 PQ · HDR session
```

- 第一行：片源**分辨率**、**色彩空间**、**是否 HDR**、音频声道数、当前**去隔行**模式
- 第二行：OBS **画布分辨率与帧率**、**输出色彩空间**、当前是否 HDR 会话

> 说明：OBS 的插件 API 不暴露媒体文件自身的帧率与"是否隔行"标记，所以这里显示的是**输出帧率**与**插件正在使用的去隔行设置**。
> 面板底部是本插件署名与「支持我们」按钮（点击用系统默认浏览器打开）。

### 关于属性窗口顶部那块会动的画面

那是 **OBS 自带的预览控件**（`UI/window-basic-properties.cpp` 里的 `ui->preview` + `DrawPreview`）：只要源是"输入类 + 带视频"，OBS 就一定会在它的属性窗口里显示实时画面，**插件无法关闭**。它只是把当前帧缩放重画一次（不会额外解码），不想看到就关掉属性窗口。

## 6. 选项说明

| 选项 | 含义 |
|---|---|
| 播放模式 | 顺序 / 循环 / 随机 |
| 硬件解码 | 建议开启，保留 10bit P010 HDR 帧不做降级 |
| 播放速度 | 50–200% |
| 交叉淡化（默认开启） | 片段间交叉淡化。HDR 画布使用 16F 中间缓冲保留高光；中间缓冲分辨率封顶 1080p，转场结束立即释放显存 |
| 低内存模式（**默认开启**） | 只创建一个解码器、不预载下一片段，内存/显存占用约减半（4K HDR 尤其明显）。关闭后才创建第二个解码器实现无缝切换 |
| 自适应画布（默认开） | 按 OBS 基础画布分辨率输出并等比缩放，避免每帧额外的全分辨率缩放/色彩转换 |
| 去隔行 | 逐行片源选「关闭」；隔行片源选 2x 系列，1080i25 → **50p**（OBS 画布帧率需 ≥ 50，建议 60） |
| 播放列表 | 支持文件、**文件夹**（自动展开）与**网络地址**（HLS `.m3u8`、`.mpd`、rtmp/rtsp/srt 等） |
| 混播策略 | **自动**（默认，推荐）：SDR 文件按 SDR 直出、HDR 文件按 HDR 直出；也可强制全部按 HDR（PQ/HLG）或强制全部按 SDR |
| 源隐藏时 | 停止并在再次可见时重播 / 暂停并续播 / 始终播放 / 停止并播下一个 |

> 注：OBS 30/31 的源级色彩模型只能区分 **SDR 与 HDR**（PQ 还是 HLG 由 OBS「高级 → 色彩空间」全局设置决定）。因此「强制 PQ」与「强制 HLG」在 30/31 上都表现为"强制 HDR 直通"，两者保留为独立选项以便兼容后续 OBS 版本；HLG 文件在"自动"模式下会被正确识别并按 HDR 处理。

> 提示：播放列表编辑后**立即生效**——若正在播放的文件仍在列表里则保持播放并刷新预载；若已被删除则自动切到新列表的当前项。

## 7. 控制方式

- OBS 媒体控制条：播放/暂停、重启、停止、下一个、上一个；
- 源级热键：播放/暂停、重启、停止、下一个文件、上一个文件。

## 8. 常见问题

- **没有声音**：本版走 libobs 原生音频推送（子源的音频在捕获回调里直接 `obs_source_output_audio()` 推给插件源），缓冲/对齐/重采样/时间戳全部由 OBS 负责。判断方法：看 OBS 日志（帮助 → 日志文件）里 `[HDR-PL]` 的行——
  `audio capture attached to ...` → `audio forwarding active: 2 ch @ 48000 Hz from ...` → 每 5 秒 `hb: ... audio=2ch push=2xxxxx/5s`。
  `push` 大约 240000/5s 说明音频在正常推送；`push=0` 说明该文件没有音轨（或子源被静音）。
- **添加后黑屏、不自动播放**：让源进入可见（预览或直播）即会起播；也可用 OBS 媒体控制条或源热键「播放/暂停」。
- **切换时画面比例奇怪**：v0.0.3 起上一条/下一条各自计算贴合比例（分辨率不同的片段之间切换也正常）。
- **全部播完后画面残留**：v0.0.3 起停止/播完/未起播时不再绘制，画面回到下层。
- **内存占用高 / 崩溃**：默认已开启「低内存模式」——此时**只创建一个解码器**，第二个解码器（及其解码帧缓存）根本不会被创建；转场中间缓冲封顶 1080p 且转场结束立即释放。
- **SDR 视频颜色不对**：把「混播策略」保持「自动」；若仍异常再试「强制 SDR」对比。
- **属性窗口一动就滚回顶部**：本版属性面板**不再每秒刷新**，只有打开面板/切换片段时才重建，滚动位置不会再被顶回。
- **日志里的 FFmpeg 警告**（如 `co located POCs unavailable`）来自 OBS 自己的解码器，通常是片源特性/跳转时的提示，不影响播放。

## 9. 版本历史

- **v0.0.3**（当前正式版）：
  - **音频**：改为 libobs 原生推送模型（`obs_source_output_audio`），移除会让源自持时钟、时间戳落不到混音窗口而被静默丢弃的 `audio_render` 回调；子源音频捕获回调的锁顺序保持修复（注册/注销均在锁外，避免 AB-BA 死锁）
  - **交叉淡化修复**：此前打包脚本只拷 `data/locale`，`data/effects/crossfade.effect` 没进包 → 运行时找不到 → 只能硬切；现在三平台都会打包，且特效编译失败会把真实错误写进日志
  - **切换几何修复**：上一条/下一条各自计算贴合比例；子源媒体未打开（0×0）时不绘制，避免首帧被放大
  - **播完留白**：停止/播完/未起播时不再绘制最后一帧
  - **属性面板**：新增当前媒体的分辨率/色彩空间/HDR/声道/去隔行 + 输出分辨率帧率/色彩空间两行信息（仅在打开面板与切换片段时更新，不做每秒刷新）；面板底部加入署名与「支持我们」按钮
  - **代码与内存**：删除未使用的播放列表序列化/移除等死代码与无用本地化键；转场中间缓冲按需创建、用完立即释放；低内存模式只保留一个解码器
  - **兼容性**：明确适用范围为 OBS 30+（启动时对低于 30 的版本给出警告）
  - 详细说明见 [`docs/USAGE.md`](docs/USAGE.md)
- **v0.0.2**（预发行版）：修复音频 use-after-free、新增 `activate` 自动起播、低内存模式释放空闲解码器。
- **v0.0.1**（预发行版）：首个可用版本。

---

本插件由 **不息传播** 制作 · [**支持我们**](https://afdian.com/p/11bd2a72b35211f1b8dc52540025c377)

---

## English summary

*Play a video playlist in OBS with real HDR passthrough (P010 / Rec.2100 PQ·HLG) and optional gapless A/B switching.*

- **Requires OBS Studio 30 or newer**; older versions are untested.
- Windows install: copy the contents of `bin` into `"drive"\obs-studio\obs-plugins\64bit\` and the contents of `data` into `"drive"\obs-studio\data\obs-plugins\HDR-Playlist-Source\` (create that folder first); administrator rights are required.
- Features: sequential/loop/shuffle playlists, folders (auto-expanded), network streams (HLS/DASH/RTMP/SRT), crossfade (HDR-safe 16F intermediate, capped and released), deinterlacing, HDR/SDR policy, low-memory mode (single decoder), OBS media controls + hotkeys, and a properties panel that reports the current clip's resolution, colour space, HDR state and the output format.

Made by 不息传播 · [Support us](https://afdian.com/p/11bd2a72b35211f1b8dc52540025c377)
