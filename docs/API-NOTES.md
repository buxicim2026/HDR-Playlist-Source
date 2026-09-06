# OBS 31 API Notes（ffmpeg_source 兼容探测结论）

> 来源：obs-studio tag `31.0.0` 的 `plugins/obs-ffmpeg/obs-ffmpeg-source.c`
> （已逐条核对），供 switcher/兼容探测层实现使用。运行时可再用
> `obs_get_source_defaults("ffmpeg_source")` 复核键名。

## ffmpeg_source 默认设置键（obs_data 键名 = 源码键名，勿改）

| 键 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `is_local_file` | bool | true | 本地文件模式 |
| `local_file` | string | 空 | 本地文件路径 |
| `looping` | bool | false | 循环 |
| `restart_on_activate` | bool | true | **必须设 false**：否则子源被激活(activate)时自动重启，破坏预载控制 |
| `close_when_inactive` | bool | false | 非活动时关闭解码器（设为 false 让预载槽保持解码器） |
| `clear_on_media_end` | bool | true | 播完清画面；无缝切换需视槽位用途设 false 保留末帧 |
| `hw_decode` | bool | (无默认=0) | 硬解开关，开启才走 P010 直通 |
| `speed_percent` | int | 100 | 1-200 |
| `buffering_mb` | int | 2 | 仅网络流用，本地文件可忽略 |
| `color_range` | int(list) | VIDEO_RANGE_DEFAULT | 0/1/2 = DEFAULT/PARTIAL/FULL |
| `linear_alpha` | bool | false | |
| `seekable` | bool | false | 网络流 seek |
| `ffmpeg_options` | string | 空 | 透传 FFmpeg 选项 |
| `full_decode` | bool | false | 逐帧精确解码（stinger 用） |
| `is_stinger` / `is_track_matte` | bool | false | 私有属性，勿用于普通播放 |

## 行为要点（对 A/B 切换器设计的约束）

1. `ffmpeg_source_update()`：当媒体需重建（文件/硬解/色深/速度等变化）时
   `should_restart_media=true`，随后：
   - `(!close_when_inactive || active) && should_restart_media` → `ffmpeg_source_open()`
   - `(!restart_on_activate || active) && should_restart_media` → `ffmpeg_source_start()`
   因此**槽位子源用 `restart_on_activate=false` + `close_when_inactive=false`
   + `update(local_file=…)` 即可在后台自动 open+start**。
2. `activate` 回调：仅当 `restart_on_activate=true` 才 `media_restart`；
   `deactivate` 同理。→ 槽位控制权完全由我们通过
   `obs_source_media_play_pause / media_stop / media_set_time` 接管。
3. `media_play_pause(pause)` 会 `media_playback_play_pause`；非活动/未开始的
   media 需先经历 `start`（见上），paused 后再 `set_time(0)` 可实现"停在首帧"。
4. 播放自然结束 → `media_stopped` 回调：若 `clear_on_media_end` 且非
   track_matte 则 `obs_source_output_video(NULL)`；状态置 ENDED 并发
   `media_ended` 信号（父源据此驱动切下一集）。
5. 支持媒体控制回调：`media_play_pause/restart/stop/get_duration/get_time/
   set_time/get_state`；signal：`media_started/media_ended`；proc：
   `restart`、`preload_first_frame`、`get_duration`、`get_nb_frames`。
6. `output_flags = ASYNC_VIDEO|AUDIO|DO_NOT_DUPLICATE|CONTROLLABLE_MEDIA`，
   **没有实现 `video_get_color_space`** → async 帧靠帧内
   `color_matrix/full_range/trc/max_luminance` 上报，因此父源必须实现
   `video_get_color_space` 且绝不能对 HDR 返回 `GS_CS_SRGB`。

## 我们插件里要写的槽位 settings

```c
obs_data_set_string(s, "local_file", path);
obs_data_set_bool(s, "is_local_file", true);
obs_data_set_bool(s, "looping", false);
obs_data_set_bool(s, "restart_on_activate", false);
obs_data_set_bool(s, "close_when_inactive", false);
obs_data_set_bool(s, "clear_on_media_end", false);   /* 播放槽也可 true，末帧保留更稳 */
obs_data_set_bool(s, "hw_decode", hw_decode);
obs_data_set_int (s, "speed_percent", speed_percent);
obs_data_set_int (s, "color_range", VIDEO_RANGE_DEFAULT);
```
创建子源：`obs_source_create_private("ffmpeg_source", name, settings)`；
释放：`obs_source_release`；取设置更新：`obs_source_update(child, settings)`。
