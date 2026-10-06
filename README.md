<div align="center">

📺 **B站：[不息传播](https://space.bilibili.com/385015308)** ｜ 💬 **微信公众号：不息传播**

# 万能播放列表（HDR Playlist Source）

**一个插件，管住你 OBS 里所有视频。**

一款面向 OBS Studio 的播放列表插件。不管你是 HDR 直播环境，还是普通的非 HDR 环境，都能正常使用，轻松把一串视频排队连续播放。

[![GitHub release](https://img.shields.io/github/v/release/buxicim2026/HDR-Playlist-Source?style=flat-square)](https://github.com/buxicim2026/HDR-Playlist-Source/releases)
[![GitHub stars](https://img.shields.io/github/stars/buxicim2026/HDR-Playlist-Source?style=social)](https://github.com/buxicim2026/HDR-Playlist-Source)
[![Sponsor](https://img.shields.io/static/v1?label=Sponsor&message=%E2%9D%A4&logo=GitHub&color=%23fe8e86)](https://github.com/sponsors/buxicim2026)

</div>

---

## 目录

- [项目介绍](#项目介绍)  [主要特点](#主要特点)  [为什么叫“万能”](#为什么叫万能)  [适用版本](#适用版本)  [安装方法](#安装windows-11)  [Linux x64 / macOS (Apple Silicon)](#linux-x64--macos-apple-silicon)
- [HDR 输出配置](#hdr-输出配置要看到真-hdr-才需要)  [快速上手](#快速上手)  [选项说明](#选项说明) [控制方式](#控制方式) [常见问题](#常见问题)
- [版本历史](#版本历史)  [支持项目](#支持项目)  [联系与反馈](#联系与反馈)

---

## 项目介绍

**万能播放列表（HDR Playlist Source）** 是一个专注于 OBS Studio 的播放列表插件。

它的目标很简单：  
把一连串的视频文件（本地文件、文件夹、网络流）排队连续播放，不用再手动一个个切换视频源（因为OBS自带媒体源只能放单个视频），也不用为了放一个片单去折腾复杂的场景配置。

名字里虽然有 HDR，但它**不只是给 HDR 用户准备的**。经过专门优化，**在普通 SDR 直播环境下同样可以正常使用**，不满足 HDR 配置时插件会自动按 SDR 播放，由 OBS 正常做 HDR→SDR 的色调映射，不会硬塞 HDR，人人都可以用[reference:0]。

项目名称：

- 英文名：`HDR Playlist Source`
- 中文名：**万能播放列表**

一句话介绍：

> **万能播放列表，在OBS放视频更轻松。**

---

## 主要特点

### 🎬 面向 OBS Studio

OBS Studio 第三方插件，但高度依赖于OBS本身
OBS 30 及以上版本均可使用[reference:1]，30以下版本的环境未进行测试，请谨慎使用

### 📃 自动轮播

把多个视频（甚至网络直播地址）加入播放列表，插件会按顺序自动播放，播完自动切下一个。  
支持顺序 / 循环 / 随机三种播放模式[reference:2]。

### 🌈 HDR / 非 HDR 都能用

这是这个插件最“万能”的地方。  
走 OBS 原生 HDR 管线（P010 / Rec.2100 PQ·HLG 直通），不做 8bit 降级[reference:3]。  
不满足 HDR 配置时自动按 SDR 播放，普通用户不用特意配置 HDR 设备也能正常使用[reference:4]。

### 🎛 操作直观

在 OBS 界面里就能完成添加、排序、删除等操作。  
支持文件、**文件夹**（自动展开）与**网络地址**（HLS `.m3u8`、`.mpd`、rtmp/rtsp/srt 等）[reference:5]。

### 🔁 灵活播放

支持交叉淡化转场、去隔行、播放速度调节、自适应画布等选项，满足不同直播场景需求[reference:6]。

### 🧠 低内存模式

默认开启，只创建一个解码器、不预载下一片段，内存/显存占用约减半（4K HDR 尤其明显），减轻你的电脑负担[reference:7]。

### 🔓 开源透明

项目托管在 GitHub，代码公开可见，可以提交问题和建议。

---

## 为什么叫“万能”

很多播放列表插件要么只照顾 HDR 用户，要么在非 HDR 环境下表现不稳定。

万能播放列表想做的是：

> **不管你用什么设备、什么直播环境，只要你在 OBS 里想放一串视频，它都能帮你放。**

- HDR 用户可以正常用；
- 普通用户也可以正常用；
- 不需要为了实现视频联播而去安装别的软件

这就是“万能”的含义：**让更多人用得上，而不是只服务一小部分人。**

---

## 适用版本

- **适用于 OBS Studio 30 及以上版本。**
- **版本号 30 以下的 OBS 兼容性未知**（插件启动时若检测到低于 30.0.0 会在日志里给出警告）[reference:8]。

---

## 安装（Windows 11）

> 以下内容与仓库原始自述文件中的“安装（Windows 11）”一栏**完全一致，未做任何改动**。

1. 解压安装包，里面会有两个文件夹：`bin` 和 `data`；
2. 把 **`bin` 文件夹里面的文件**复制到：`"安装盘符"\obs-studio\obs-plugins\64bit\`；
3. 把 **`data` 文件夹里面的文件**复制到：`"安装盘符"\obs-studio\data\obs-plugins\HDR-Playlist-Source\`（**这个文件夹需要你自行提前创建**）；
4. 复制过程中需要**管理员权限**；
5. 重启 OBS，在「来源」里就能添加 **HDR Playlist Source**。

> 注意：`data` 里的内容必须一起复制，尤其是 `data\effects\crossfade.effect`——缺了它交叉淡化会失效、退化成硬切。

---

## Linux x64 / macOS (Apple Silicon)

- **Linux**：解压后把 `HDR-Playlist-Source` 放到 `~/.config/obs-studio/plugins/`
- **macOS**：解压后把 `HDR-Playlist-Source.plugin` 放到 `~/Library/Application Support/obs-studio/plugins/`

[reference:9]

---

## HDR 输出配置（要看到真 HDR 才需要）

OBS → 设置 → 高级：

- 色彩格式：**P010**
- 色彩空间：**Rec. 2100 (PQ)** 或 **Rec. 2100 (HLG)**
- 色彩范围：**Partial**
- 编码器：HEVC 10bit 或 AV1 10bit（配合你的 HDR 推流服务）
- 本地观看 HDR 需要 HDR 显示器，并在 OBS 中开启 HDR 预览
（其实你可以用手机观看推流之后的HDR画面的）

不满足上面的配置时，插件会自动按 SDR 播放（由 OBS 正常做 HDR→SDR 的色调映射），不会硬塞 HDR。[reference:10]

---

## 快速上手

1. 「来源」→ `+` → **HDR Playlist Source**；
2. 在**播放列表**里加入媒体文件；也可以点「添加文件夹」选一个文件夹（其中的媒体文件会自动展开加入），或者直接把文件夹路径/网络地址填进列表；
3. 选播放模式：顺序 / 循环 / 随机；
4. 点**确定**。源一旦可见（预览或直播）就会自动开始播放；
5. 一个文件播完后自动切到下一个（默认「低内存模式」不预载；需要无缝切换请关掉它）。

[reference:11]

---

## 选项说明

| 选项 | 含义 |
|---|---|
| 播放模式 | 顺序 / 循环 / 随机 |
| 硬件解码 | 建议开启，保留 10bit P010 HDR 帧不做降级 |
| 播放速度 | 50–200% |
| 交叉淡化（默认开启） | 片段间交叉淡化。HDR 画布使用 16F 中间缓冲保留高光；中间缓冲分辨率封顶 1080p，转场结束立即释放显存 |
| 低内存模式（**默认开启**） | 只创建一个解码器、不预载下一片段，内存/显存占用约减半（4K HDR 尤其明显）。关闭后才创建第二个解码器实现无缝切换。停播（含隐藏/播完）持续 20 秒后会自动关闭解码器 |
| 自适应画布（默认开） | 按 OBS 基础画布分辨率输出并等比缩放，避免每帧额外的全分辨率缩放/色彩转换 |
| 去隔行 | 逐行片源选「关闭」；隔行片源选 2x 系列，1080i25 → **50p**（OBS 画布帧率需 ≥ 50，建议 60） |
| 播放列表 | 支持文件、**文件夹**（自动展开）与**网络地址**（HLS `.m3u8`、`.mpd`、rtmp/rtsp/srt 等） |
| 混播策略 | **自动**（默认，推荐）：SDR 文件按 SDR 直出、HDR 文件按 HDR 直出；也可强制全部按 HDR（PQ/HLG）或强制全部按 SDR |
| 源隐藏时 | 停止并在再次可见时重播 / 暂停并续播 / 始终播放 / 停止并播下一个 |

[reference:12]

> 注：OBS 30/31 的源级色彩模型只能区分 **SDR 与 HDR**（PQ 还是 HLG 由 OBS「高级 → 色彩空间」全局设置决定）。因此「强制 PQ」与「强制 HLG」在 30/31 上都表现为"强制 HDR 直通"，两者保留为独立选项以便兼容后续 OBS 版本；HLG 文件在"自动"模式下会被正确识别并按 HDR 处理。[reference:13]

> 提示：播放列表编辑后**立即生效**——若正在播放的文件仍在列表里则保持播放并刷新预载；若已被删除则自动切到新列表的当前项。[reference:14]

---

## 控制方式

- OBS 媒体控制条：播放/暂停、重启、停止、下一个、上一个；
- 源级热键：播放/暂停、重启、停止、下一个文件、上一个文件。

[reference:15]

---

## 常见问题


**Q：添加后黑屏、不自动播放？**  
A：让源进入可见（预览或直播）即会起播；也可用 OBS 媒体控制条或源热键「播放/暂停」。[reference:17]

**Q：切换时画面比例奇怪？**  
A：v0.0.3 起上一条/下一条各自计算贴合比例（分辨率不同的片段之间切换也正常）。[reference:18]

**Q：全部播完后画面残留？**  
A：v0.0.3 起停止/播完/未起播时不再绘制，画面回到下层。[reference:19]

**Q：内存占用高 / 崩溃？**  
A：默认已开启「低内存模式」——此时只创建一个解码器，第二个解码器（及其解码帧缓存）根本不会被创建；转场中间缓冲封顶 1080p 且转场结束立即释放。[reference:20]

**Q：SDR 视频颜色不对？**  
A：把「混播策略」保持「自动」；若仍异常再试「强制 SDR」对比。[reference:21]

**Q：日志里的 FFmpeg 警告（如 `colocated POCs unavailable`）？**  
A：来自 OBS 自己的解码器，通常是片源特性/跳转时的提示，不影响播放。[reference:23]

---

## 版本历史

### v0.0.3（当前正式版）

- **音频**：改为 libobs 原生推送模型（`obs_source_output_audio`），移除会让源自持时钟、时间戳落不到混音窗口而被静默丢弃的 `audio_render` 回调；子源音频捕获回调的锁顺序保持修复（注册/注销均在锁外，避免 AB-BA 死锁）
- **交叉淡化修复**：此前打包脚本只拷 `data/locale`，`data/effects/crossfade.effect` 没进包 → 运行时找不到 → 只能硬切；现在三平台都会打包，且特效编译失败会把真实错误写进日志
- **切换几何修复**：上一条/下一条各自计算贴合比例；子源媒体未打开（0×0）时不绘制，避免首帧被放大
- **播完留白**：停止/播完/未起播时不再绘制最后一帧
- **属性面板**：顶部两行媒体信息（「现在播放视频参数：」分辨率·色彩空间·是否HDR·声道·去隔行；「目前直播配置参数：」画布尺寸@帧率·输出色彩空间·HDR/SDR 会话）；每 0.5 秒比对内容，切换片段后自动更新，但只有内容真的变化才重建面板（不做每秒刷新）；面板底部一行「本插件由不息传播制作 · 支持我们」，点击整行用系统默认浏览器打开赞助页
- **内存**：低内存模式下停播/隐藏/播完满 20 秒后自动关闭解码器（解复用器 + 编解码器 + 解码帧缓存），下次播放再打开；转场中间缓冲按需创建、封顶 1080p、用完立即释放。关闭解码器采用"清空 `local_file` + 延迟更新"，不销毁源对象，因此不会与 libobs 的源遍历冲突
- **代码与内存**：删除未使用的播放列表序列化/移除等死代码与无用本地化键；转场中间缓冲按需创建、用完立即释放；低内存模式只保留一个解码器
- **兼容性**：明确适用范围为 OBS 30+（启动时对低于 30 的版本给出警告）

[reference:24]

### v0.0.2（预发行版）

修复音频 use-after-free、新增 `activate` 自动起播、低内存模式释放空闲解码器。[reference:25]

### v0.0.1（预发行版）

首个可用版本。[reference:26]

---

## 支持项目

如果这个项目对你有帮助，欢迎通过 GitHub Sponsors 支持我继续维护：

[![Sponsor](https://img.shields.io/static/v1?label=Sponsor&message=%E2%9D%A4&logo=GitHub&color=%23fe8e86)](https://github.com/sponsors/buxicim2026)

你也可以在仓库页面点击右上角的 **Sponsor** 按钮进行捐赠。

> 提示：仓库右上角 Sponsor 按钮需要先在 GitHub 仓库的 **Settings → Sponsorships** 中启用，并确保 `.github/FUNDING.yml` 包含：
>
> ```yaml
> github: buxicim2026
> ```

---

## 联系与反馈

- B站：[不息传播](https://space.bilibili.com/385015308)
- 微信公众号：不息传播
- GitHub Issues：[提交问题](https://github.com/buxicim2026/HDR-Playlist-Source/issues)

---

本插件由 **不息传播** 制作 · [**支持我们**](https://afdian.com/p/11bd2a72b35211f1b8dc52540025c377)[reference:27]
