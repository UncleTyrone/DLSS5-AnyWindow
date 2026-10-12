# DLSS5 AnyWindow 1.10.0

标签：`v1.10.0-dlss5-anywindow`

本版相对 [Shangyuwang11/DLSS5-AnyWindow](https://github.com/Shangyuwang11/DLSS5-AnyWindow) 的上游实验分支，补上 1.5.0 到 1.9.2 已发布的控制器与运行库工作，以及这一版的帧生成和 DLSSNR。写法与上游 README 的实验观察一致：先说能做什么，再写还没有的部分。

## 相对上游已经发布的版本

这些变化已有单独说明，本版把它们收在同一条发布线上：

- [1.5.0](RELEASE_NOTES_v1.5.0-multigpu-runtime.md) 按显卡选择 DLSSNR 运行库。
- [1.6.0](RELEASE_NOTES_v1.6.0-wood-ui-hotkey.md) 调整浮窗外观和快捷键。
- [1.7.0](RELEASE_NOTES_v1.7.0-rtx30-runtime.md) 分开 RTX 30 运行库。
- [1.8.0](RELEASE_NOTES_v1.8.0-floating-window-hotkey.md) 增加独立的浮窗显隐快捷键。
- [1.9.0](RELEASE_NOTES_v1.9.0-auto-update.md) 增加带校验的控制器更新和“关于”页。
- [1.9.1](RELEASE_NOTES_v1.9.1-processing-resolution.md) 增加 DLSSNR 内部处理分辨率。
- [1.9.2](RELEASE_NOTES_v1.9.2-full-auto-update.md) 启动时同步控制器、外部窗口渲染器和 DLSSNR 效果，失败则回滚。

## 用户可见变化

- **DLSS 帧生成，2x 到 6x。** 输出停在缩放窗口所在显示器的刷新率，不会超过。每张真实帧都按所选倍率请求；会使输出超过刷新率的生成帧被跳过。生成帧在 FLIP_SEQUENTIAL、允许撕裂、最大帧延迟为 1 的交换链上连续呈现，每一帧都会保留到扫描输出。
- **动态帧生成。** 按刷新率或你设定的帧率选择倍率。开启时固定倍率变灰。刷新率预算仍然是上限。
- **RTX 40 多帧生成。** 打开 Ada 时序修复后，3x 到 6x 把额外帧均匀排进两帧真实画面之间，并把已接受的混合权重抬到至少 0.85。RTX 50 本身支持这些倍率，补丁不会改动它。离线质量内核还不在这个版本里。其他 GPU 最高到 4x。驱动报告的更低上限优先。
- **DLSSNR。** 按原尺寸重画窗口，不放大。可设风格、强度、局部色调、结构、皮肤、预设，处理 1–4 次。Detail Reuse 设 1 为隔帧。ViT Reuse 设 2 为隔帧。RTX 50 把 NVFP4 Hybrid 设为 1。
- **HDR 桌面。** HDR 桌面上的 SDR 窗口以 scRGB 浮点捕获，按 SDR 白点的尼特值转成 sRGB，再以 8 位交给 DWM 呈现。效果链和帧生成后备缓冲保持 8 位 sRGB。纸白、色彩曲线和 `.cube` 色彩查找表还不在这个版本里。
- **NVFP4 Hybrid。** RTX 50 上设为 1。`DLSSNR-Hybrid` 要放在 `Magpie.exe` 旁边。
- **DLSS 超分辨率** 仍可用于任意窗口，带光流运动和可选的估算深度。
- **FSR 2、FSR 3.1、FSR 4** 上采样，**XeSS** 上采样，以及 **XeSS 帧生成**（Intel、NVIDIA、AMD 上 2x；Intel Arc 上最高 4x）。
- **RTX Video** 上采样和降噪，以及从捕获画面估算的 **Depth Anything V2** 深度（TensorRT 或 DirectML）。

## 发行包

把下面的压缩包解压到同一个文件夹。`DLSSNR-Hybrid` 会落在 `Magpie.exe` 旁边。

- `v1.10.0-dlss5-anywindow-app.zip`：程序、效果、DLSS 与 DLSSNR 运行库、NVFP4 hybrid、深度、许可证和本说明。
- `v1.10.0-dlss5-anywindow-tensorrt-1.zip` 与 `tensorrt-2.zip`：TensorRT。分开是因为体积超过单个附件上限。

校验见同目录的 `SHA256SUMS.json`。

包内还有：

- **NVIDIA DLSS 超分辨率：** `nvngx_dlss.dll`。
- **NVIDIA DLSS 帧生成：** `nvngx_dlssg.dll`。需要 RTX 40 或 RTX 50。
- **NVIDIA DLSSNR：** `DLSSNR-Runtimes\RTX20`、`RTX30`、`RTX40`、`RTX50`。按显卡自动选择。
- **许可证：** `Licenses\janblade-OptiScaler-GPL-3.0.txt`（NVFP4 hybrid），`Licenses\MFGAdaUnlock-RenoDx-MIT.txt`（RTX 40 帧间距）。
- **NVIDIA Optical Flow：** 没有额外文件。使用已安装驱动里的光流硬件。其他 GPU 使用软件光流。
- **NVIDIA RTX Video：** `NVVideoEffects.dll`、`NVCVImage.dll`、`nvngx_vsr.dll` 及其 CUDA 库。
- **AMD FSR 2.2.1：** `ffx_fsr2_api_x64.dll`、`ffx_fsr2_api_dx11_x64.dll`。
- **AMD FSR 3.1 / FSR 4：** `amd_fidelityfx_loader_dx12.dll`、`amd_fidelityfx_upscaler_dx12.dll`。
- **Intel XeSS 3.0.1：** `libxess.dll`、`libxess_fg.dll`、`libxell.dll`。
- **Depth Anything V2：** `FrameGuidance\DepthAnythingV2\` 与 `FrameGuidance\DirectML\`。

## 要求

- Windows 10/11 x64。
- DLSS 帧生成需要 RTX 40 或 RTX 50。DLSSNR、DLSS 超分辨率、RTX Video 和 TensorRT 深度需要 NVIDIA RTX。
- 较新的 NVIDIA 驱动。
- 大约 5.2 GB 磁盘空间，主要是 CUDA、cuDNN 和 TensorRT。

## 更新器

程序内更新器检查本仓库，查找标签 `v1.10.0-dlss5-anywindow`。更新附件是 `DLSS5FloatingController-1.10.0.exe`、`DLSSNRWindowDouble-1.10.0.exe` 和 `DLSSNR_AI_Filter-1.10.0.hlsl`，每个都带 `.sha256.txt`。本地设置、GPU 运行库和模型不会被覆盖。

## [Releases](https://github.com/UncleTyrone/DLSS5-AnyWindow/releases)

## English

DLSS5 AnyWindow 1.10.0 is this fork's release on top of [Shangyuwang11/DLSS5-AnyWindow](https://github.com/Shangyuwang11/DLSS5-AnyWindow). Versions 1.5.0 through 1.9.2, linked above, cover runtime selection, the floating controller, and the updater. This note covers frame generation and DLSSNR in the same order as the upstream README.

- **DLSS Frame Generation, 2x through 6x.** Output stays on the refresh rate of the monitor showing the scaled window. Every real frame asks for the selected multiplier. Generated frames that would pass that rate are skipped. They are presented back-to-back on a FLIP_SEQUENTIAL tearing swap chain with a maximum frame latency of 1, so each generated frame stays until scanout.
- **Dynamic Frame Generation.** The multiplier follows the refresh rate, or a frame rate you set. While it is on, the fixed multiplier is grayed. The refresh-rate budget remains the ceiling.
- **RTX 40 multi-frame generation.** With Ada Temporal Fix on, 3x through 6x space the extra frames across the interval and raise an accepted warp blend to at least 0.85. RTX 50 already supports these multipliers, and the patch leaves that GPU unchanged. The extra offline quality kernels are not in this build. Other GPUs stop at 4x. A lower limit from the driver wins.
- **DLSSNR.** Redraws the window at the same size. It does not upscale. Set style, intensity, local tone, structure, skin, and preset, and run 1–4 passes. Detail Reuse `1` is every other frame. ViT Reuse `2` is every other frame. On RTX 50, set NVFP4 Hybrid to `1`.
- **HDR desktops.** An SDR window on an HDR desktop is captured as scRGB float, converted to sRGB with the SDR white level, and presented as 8-bit through DWM. The effect chain and the frame-generation backbuffer stay 8-bit sRGB. Paper white, colour curves, and `.cube` looks are not in this build.
- **NVFP4 Hybrid.** Set it to `1` on RTX 50. `DLSSNR-Hybrid` has to sit beside `Magpie.exe`.
- **DLSS Super Resolution** still runs on any window, with optical-flow motion and optional estimated depth.
- **FSR 2, FSR 3.1, and FSR 4** upscaling, **XeSS** upscaling, and **XeSS Frame Generation** (2x on Intel, NVIDIA, and AMD; up to 4x on Intel Arc).
- **RTX Video** upscaling and denoising, and **Depth Anything V2** depth from the captured image (TensorRT or DirectML).

Extract `v1.10.0-dlss5-anywindow-app.zip` and the two TensorRT zips into the same folder. Hashes are in `SHA256SUMS.json`. The in-app updater looks for tag `v1.10.0-dlss5-anywindow` on this repository.
