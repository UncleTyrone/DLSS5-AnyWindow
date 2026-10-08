# Magpie 下一实验版 / Next Experimental Release

当前待发布版本为 [DLSS5 AnyWindow 1.9.2 完整自动更新](RELEASE_NOTES_v1.9.2-full-auto-update.md)。

The pending source release is [DLSS5 AnyWindow 1.9.2 full automatic updates](RELEASE_NOTES_v1.9.2-full-auto-update.md).

## 未发布变更 / Unreleased changes

### DLSS FG：以刷新率为目标的倍率 / Refresh-targeted multiplier

- `Frame Multiplier` 现在是上限。DLSS FG 读取显示缩放窗口的显示器刷新率，每 0.5 秒测量源的真实帧率，并按 `刷新率 ÷ 源帧率 − 1` 决定每张真实帧的生成帧数，范围为 0 到 `Frame Multiplier` − 1。
- 小数目标会分摊到各真实帧（例如 2.3x 时大多数真实帧生成 1 张、部分生成 2 张；90 Hz 显示器配 60 FPS 源时为 1.5x）。
- 不生成帧的真实帧仍会送入 DLSS，帧生成历史保持连续。
- 该行为始终启用，没有开关。

- `Frame Multiplier` is now a ceiling. DLSS FG reads the refresh rate of the monitor showing the scaled window, measures the source's real frame rate every 0.5 s, and generates `refresh ÷ source FPS − 1` frames per real frame, clamped to 0 through `Frame Multiplier` − 1.
- Fractional targets are spread across real frames: at about 2.3x most real frames get one generated frame and some get two, and a 60 FPS source on a 90 Hz display runs at 1.5x.
- Real frames that receive no generated frame are still fed to DLSS, so frame-generation history stays continuous.
- Previously, 3x/4x on a fast source (for example a ~105 FPS capture-card preview on a 240 Hz display) could not exceed the refresh rate. Presenting the extra frames made real frames wait or drop, so DLSS interpolated across larger gaps and ghosted. The behaviour is always on and has no setting.

### 源帧率测量 / Source frame-rate measurement

- `FrameSourceBase::TakeSourceFrameCounts()` 报告捕获 API 交付的帧数和送入重复帧过滤的帧数。Graphics Capture 会统计后端繁忙时被跳过的旧帧，因此测得的源帧率不会因呈现跟不上而下降。
- `FrameSourceBase::TakeSourceFrameCounts()` reports frames delivered by the capture API and frames passed to duplicate filtering. Graphics Capture counts older frames drained while the backend was busy, so the measured source rate does not fall when presentation falls behind.

### DLSS FG 呈现路径 / Presentation path

- 移除基于 sleep 的生成帧节奏控制。它会阻塞捕获线程并导致卡顿；生成帧现在在 FLIP_SEQUENTIAL、允许撕裂、最大帧延迟为 1 的交换链上连续呈现。设置帧率过滤器时仍按 DWM 合成节奏控制。
- 移除每次 Present 都查询帧统计的 `PRESENTER DIAG` 诊断日志。
- 每秒的 `DLSSFG presentation` 日志改为报告 `captured`、`submitted`、`source` 帧率和当前 `target` 倍率。

- Removed sleep-based pacing of generated frames. It blocked the capture thread and caused hitching. Generated frames are now presented back-to-back on a FLIP_SEQUENTIAL, tearing-enabled swap chain with a maximum frame latency of 1. DWM-composition pacing still applies when a frame-rate filter is set.
- Removed the per-present `PRESENTER DIAG` logging and its frame-statistics queries.
- The per-second `DLSSFG presentation` log line now reports `captured`, `submitted`, and `source` FPS and the current `target` multiplier.

### Frame Guidance 历史 / History resets

- 光标变为可见时不再重置 Frame Guidance 历史，捕获间隔超过 500 ms 时也不再重置。这两种重置会让 DLSS FG 在一帧内绑定零运动并重置历史，造成短暂的鬼影。
- Frame Guidance history is no longer reset when the cursor becomes visible or after a capture gap longer than 500 ms. Each reset bound zero motion for a frame and reset DLSS FG history, which caused brief bursts of ghosting.

### 编译文档 / Build documentation

- 新增[启用全部可选后端的编译方法](BUILD_ALL_FEATURES.md)：DLSS、DLSSNR、NVIDIA Optical Flow、FSR2、FSR3/FSR4、XeSS SR/FG、RTX Video 和 Depth Anything V2（TensorRT 与 DirectML）的开关、已测试依赖版本、下载来源、目录结构和示例 `BuildOptions.props.user`。
- 本地编译此前未启用 `EnableDLSSNR`，因此 DLSSNR 日志显示 “support is disabled at build time”。这是本机编译配置问题，并非代码变更导致；新文档说明了所需的 `DLSSNRRuntimeDir`。

- Added [Building with every optional backend](BUILD_ALL_FEATURES.md). It covers the switches, tested dependency versions, download sources, folder layouts, and an example `BuildOptions.props.user` for DLSS, DLSSNR, NVIDIA Optical Flow, FSR2, FSR3/FSR4, XeSS SR/FG, RTX Video, and Depth Anything V2 (TensorRT and DirectML).
- Local builds without `EnableDLSSNR` log "DLSSNR support is disabled at build time" and pass frames through unchanged. This is a build-configuration issue, not a code regression; the new guide lists the required `DLSSNRRuntimeDir`.

### 缩放窗口 / Scaling window

- 首次渲染时如果源窗口位置仍在变化，不再立即停止缩放。此前缩放窗口可能在显示前就被关闭。
- A source-window state change during the first render no longer stops scaling. Before, scaling could close before its window was shown.
