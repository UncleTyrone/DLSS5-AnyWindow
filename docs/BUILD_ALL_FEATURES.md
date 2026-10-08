# Building with every optional backend / 启用全部可选后端的编译方法

All optional backends are disabled in `src/BuildOptions.props`. Enable them in the git-ignored `src/BuildOptions.props.user`. Keep every SDK, runtime, wheel, and model outside the repository (for example in a sibling `DLSS5-Deps` folder) and never commit them. See [Third-party components and redistribution](THIRD_PARTY_AND_REDISTRIBUTION.md) before sharing any binary built this way.

`src/BuildOptions.props` 中所有可选后端默认关闭，请在已被 Git 忽略的 `src/BuildOptions.props.user` 中启用。所有 SDK、运行库、wheel 和模型都应放在仓库之外（例如同级的 `DLSS5-Deps` 目录），不得提交。分发此类二进制前请先阅读[第三方组件与再分发](THIRD_PARTY_AND_REDISTRIBUTION.md)。

## Switches and dependencies / 开关与依赖

| Switch | Dependency (tested version) | Source | Path properties |
| --- | --- | --- | --- |
| `EnableDLSSZeroMV`, `EnableDLSSFrameGeneration` | NVIDIA DLSS SDK | [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) | `DLSSSdkDir` |
| `EnableDLSSNR` | `nvngx_dlssnr.dll` (locally supplied, see the redistribution notes) | `DLSSNR-Runtimes` folder of a DLSS5 AnyWindow portable package | `DLSSNRRuntimeDir`, `DLSSNRRuntimeSFV2Dir`, `DLSSNRRuntimeRTX30Dir`, `DLSSNRRuntimeRTX40Dir`, `DLSSNRRuntimeRTX50Dir` |
| `EnableNvidiaOpticalFlow` | NVIDIA Optical Flow SDK headers | [Optical Flow SDK](https://developer.nvidia.com/opticalflow-sdk) | `NvidiaOpticalFlowSdkDir` |
| `EnableFSR2ZeroMV` | FSR 2.2.1 and the community DX11 backend | See the README dependency list | `FSR2SdkDir`, `FSR2RuntimeDir` |
| `EnableFSR3ZeroMV` (also FSR4 effects) | AMD FSR SDK v2.3.0 | [Source archive of tag v2.3.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v2.3.0.zip); the signed DLLs are in `Kits/FidelityFX/signedbin` | `FSR3SdkDir` |
| `EnableXeSSZeroMV`, `EnableXeSSFrameGeneration` | Intel XeSS SDK 3.0.1 | [`XeSS_SDK_3.0.1.zip`](https://github.com/intel/xess/releases/tag/v3.0.1) | `XeSSSdkDir` |
| `EnableRTXVideoDenoise` (RTX Video VSR and denoise effects) | NVIDIA VFX SDK headers/proxies and `nvidia-vfx` 0.2.0.0 runtime | [VFX SDK samples](https://github.com/NVIDIA-Maxine/VFX-SDK-Samples); wheel `nvidia_vfx-0.2.0.0-cp310-cp310-win_amd64.whl` from `https://pypi.nvidia.com/nvidia-vfx/` | `VFXSdkDir`, `VFXRuntimeDir`, `VFXLicenseDir` |
| `EnableDepthAnythingV2` (DirectML path) | ONNX Runtime DirectML 1.24.4, DirectML 1.15.4, Depth Anything V2 Small FP16 ONNX | NuGet `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 and `Microsoft.AI.DirectML` 1.15.4; `model_fp16.onnx` from `FrameGuidance/DepthAnythingV2` of a portable package | `OnnxRuntimeDirectMLDir`, `DirectMLRuntimeDir`, `DepthAnythingModelDir` |
| `EnableDepthAnythingV2` (TensorRT path) | ONNX Runtime GPU 1.24.4, TensorRT 10.14.1.48, CUDA 12.9 runtime, cuDNN 9, cuBLAS 12, cuFFT 11 | NuGet `Microsoft.ML.OnnxRuntime.Gpu.Windows` 1.24.4; `tensorrt_cu12_libs-10.14.1.48.post1` from `https://pypi.nvidia.com/tensorrt-cu12-libs/`; PyPI wheels `nvidia-cuda-runtime-cu12`, `nvidia-cudnn-cu12`, `nvidia-cublas-cu12`, `nvidia-cufft-cu12` | `OnnxRuntimeTensorRTDir`, `TensorRTRuntimeDir` |

NuGet packages and wheels are ZIP files; extract them with any unzip tool.

NuGet 包和 wheel 都是 ZIP 文件，可直接解压。

## Required layouts / 目录结构要求

- **DLSSNR:** set `DLSSNRRuntimeDir` whenever `EnableDLSSNR` is on. `Magpie.vcxproj` copies `$(DLSSNRRuntimeDir)\nvngx_dlssnr.dll` without an existence check. Pointing it at the RTX 50 runtime is fine; the per-generation folders are copied to `DLSSNR-Runtimes\RTX20` to `RTX50`.
- **ONNX Runtime:** keep the NuGet layout. `OnnxRuntimeDirectMLDir` and `OnnxRuntimeTensorRTDir` point to `runtimes\win-x64\native`. Headers are read from `..\..\..\build\native\include` of the DirectML package, and `LICENSE` and `ThirdPartyNotices.txt` from the package root. The ONNX Runtime version must stay at 1.24.4 to match the headers and the TensorRT cache key.
- **DirectML:** `DirectMLRuntimeDir` points to `bin\x64-win` of the extracted `Microsoft.AI.DirectML` package, with `LICENSE.txt` and `ThirdPartyNotices.txt` two levels up.
- **CUDA libraries:** copy `cudart64_12.dll`, all `cudnn*64_9.dll`, `cublas64_12.dll`, `cublasLt64_12.dll`, and `cufft64_11.dll` into the ONNX Runtime GPU `runtimes\win-x64\native` folder. The build copies every DLL there into `FrameGuidance\TensorRT`. The TensorRT backend is skipped when `cudnn64_9.dll` is not found there or on `PATH`.
- **TensorRT:** `TensorRTRuntimeDir` needs `nvinfer_10.dll`, `nvinfer_plugin_10.dll`, `nvonnxparser_10.dll`, and the builder resources for the target GPUs. The build copies every `nvinfer*_10.dll`, so keep only the resources you need: `nvinfer_builder_resource_sm120_10.dll` for RTX 50 and `nvinfer_builder_resource_ptx_10.dll` as a fallback. The full set adds about 2.7 GB to the output.
- **RTX Video:** `VFXRuntimeDir` is `nvvfx\libs` of the extracted wheel and `VFXLicenseDir` is `nvidia_vfx-0.2.0.0.dist-info\licenses\packaging`. The runtime DLLs are copied next to `Magpie.exe`, and Magpie sets `NV_VIDEO_EFFECTS_PATH=USE_APP_PATH`, so no system-wide VFX installation is needed.
- **Depth Anything V2:** `DepthAnythingModelDir` must contain `model_fp16.onnx` and `LICENSE.Depth-Anything-V2.txt`.

- **DLSSNR：** 启用 `EnableDLSSNR` 时必须设置 `DLSSNRRuntimeDir`，因为 `Magpie.vcxproj` 会不检查存在性直接复制 `$(DLSSNRRuntimeDir)\nvngx_dlssnr.dll`。可直接指向 RTX 50 运行库；各代 GPU 的目录会被复制到 `DLSSNR-Runtimes\RTX20` 至 `RTX50`。
- **ONNX Runtime：** 保持 NuGet 原始目录结构。`OnnxRuntimeDirectMLDir` 和 `OnnxRuntimeTensorRTDir` 指向 `runtimes\win-x64\native`；头文件取自 DirectML 包的 `..\..\..\build\native\include`，`LICENSE` 和 `ThirdPartyNotices.txt` 取自包根目录。ONNX Runtime 版本需保持 1.24.4，以匹配头文件和 TensorRT 缓存键。
- **DirectML：** `DirectMLRuntimeDir` 指向解压后 `Microsoft.AI.DirectML` 包的 `bin\x64-win`，其上两级目录需包含 `LICENSE.txt` 和 `ThirdPartyNotices.txt`。
- **CUDA 库：** 将 `cudart64_12.dll`、全部 `cudnn*64_9.dll`、`cublas64_12.dll`、`cublasLt64_12.dll` 和 `cufft64_11.dll` 复制到 ONNX Runtime GPU 包的 `runtimes\win-x64\native`。编译时该目录下所有 DLL 会被复制到 `FrameGuidance\TensorRT`。若该目录和 `PATH` 中都找不到 `cudnn64_9.dll`，TensorRT 后端会被跳过。
- **TensorRT：** `TensorRTRuntimeDir` 需要 `nvinfer_10.dll`、`nvinfer_plugin_10.dll`、`nvonnxparser_10.dll` 以及目标 GPU 的 builder resource。编译会复制所有 `nvinfer*_10.dll`，因此只保留所需文件：RTX 50 使用 `nvinfer_builder_resource_sm120_10.dll`，另保留 `nvinfer_builder_resource_ptx_10.dll` 作为后备。完整集合会让输出增加约 2.7 GB。
- **RTX Video：** `VFXRuntimeDir` 为解压后 wheel 的 `nvvfx\libs`，`VFXLicenseDir` 为 `nvidia_vfx-0.2.0.0.dist-info\licenses\packaging`。运行库会复制到 `Magpie.exe` 旁，Magpie 会设置 `NV_VIDEO_EFFECTS_PATH=USE_APP_PATH`，无需系统级安装 VFX。
- **Depth Anything V2：** `DepthAnythingModelDir` 需包含 `model_fp16.onnx` 和 `LICENSE.Depth-Anything-V2.txt`。

## Example `BuildOptions.props.user` / 示例

Replace `DepsDir`, `DLSSSdkDir`, and the other machine-specific roots with your own paths.

请将 `DepsDir`、`DLSSSdkDir` 等本机路径替换为实际位置。

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <DepsDir>C:\path\to\DLSS5-Deps</DepsDir>

    <EnableDLSSZeroMV>true</EnableDLSSZeroMV>
    <EnableDLSSFrameGeneration>true</EnableDLSSFrameGeneration>
    <DLSSSdkDir>C:\path\to\NVIDIA-DLSS-SDK</DLSSSdkDir>

    <EnableNvidiaOpticalFlow>true</EnableNvidiaOpticalFlow>
    <NvidiaOpticalFlowSdkDir>C:\path\to\NVIDIA-Optical-Flow-SDK</NvidiaOpticalFlowSdkDir>

    <EnableFSR2ZeroMV>true</EnableFSR2ZeroMV>
    <FSR2SdkDir>C:\path\to\AMD-FSR2</FSR2SdkDir>
    <FSR2RuntimeDir>C:\path\to\AMD-FSR2\MagpieRuntime</FSR2RuntimeDir>

    <EnableDLSSNR>true</EnableDLSSNR>
    <DLSSNRRuntimeDir>$(DepsDir)\DLSSNR-Runtimes\RTX50</DLSSNRRuntimeDir>
    <DLSSNRRuntimeSFV2Dir>$(DepsDir)\DLSSNR-Runtimes\RTX20</DLSSNRRuntimeSFV2Dir>
    <DLSSNRRuntimeRTX30Dir>$(DepsDir)\DLSSNR-Runtimes\RTX30</DLSSNRRuntimeRTX30Dir>
    <DLSSNRRuntimeRTX40Dir>$(DepsDir)\DLSSNR-Runtimes\RTX40</DLSSNRRuntimeRTX40Dir>
    <DLSSNRRuntimeRTX50Dir>$(DepsDir)\DLSSNR-Runtimes\RTX50</DLSSNRRuntimeRTX50Dir>

    <EnableDepthAnythingV2>true</EnableDepthAnythingV2>
    <DepthAnythingModelDir>$(DepsDir)\DepthAnythingV2</DepthAnythingModelDir>
    <OnnxRuntimeTensorRTDir>$(DepsDir)\onnxruntime-gpu-1.24.4\runtimes\win-x64\native</OnnxRuntimeTensorRTDir>
    <OnnxRuntimeDirectMLDir>$(DepsDir)\onnxruntime-directml-1.24.4\runtimes\win-x64\native</OnnxRuntimeDirectMLDir>
    <DirectMLRuntimeDir>$(DepsDir)\Microsoft.AI.DirectML-1.15.4\bin\x64-win</DirectMLRuntimeDir>
    <TensorRTRuntimeDir>$(DepsDir)\TensorRT-10.14.1.48</TensorRTRuntimeDir>

    <EnableFSR3ZeroMV>true</EnableFSR3ZeroMV>
    <FSR3SdkDir>$(DepsDir)\FidelityFX-SDK-2.3.0</FSR3SdkDir>

    <EnableXeSSZeroMV>true</EnableXeSSZeroMV>
    <EnableXeSSFrameGeneration>true</EnableXeSSFrameGeneration>
    <XeSSSdkDir>$(DepsDir)\XeSS-SDK-3.0.1</XeSSSdkDir>

    <EnableRTXVideoDenoise>true</EnableRTXVideoDenoise>
    <VFXSdkDir>C:\path\to\NVIDIA-VFX-SDK</VFXSdkDir>
    <VFXRuntimeDir>$(DepsDir)\nvidia-vfx-0.2.0.0\nvvfx\libs</VFXRuntimeDir>
    <VFXLicenseDir>$(DepsDir)\nvidia-vfx-0.2.0.0\nvidia_vfx-0.2.0.0.dist-info\licenses\packaging</VFXLicenseDir>
  </PropertyGroup>
</Project>
```

## Verification and runtime notes / 验证与运行说明

- DLSS Zero-MV, DLSS Frame Generation, DLSSNR, XeSS Frame Generation, and RTX Video log `... is disabled at build time` when their switch was off. A full build never logs these messages.
- The first TensorRT depth run builds an engine and caches it in `%LOCALAPPDATA%\Magpie\FrameGuidance\TensorRTCache`; later starts load the cache. If TensorRT cannot start, the DirectML backend is used.
- A build with every backend enabled produces an output folder of about 5.6 GB, mostly CUDA, cuDNN, and TensorRT.
- The remaining switches in `BuildOptions.props` are not backends. Leave `UseCompSwapchain` off: it replaces the FLIP_SEQUENTIAL swap chain that DLSS FG presents through. `DebugBorder` and `DebugInfoOnOverlay` are debugging aids, `IsPackaged` is unsupported, and `UseClangCL`/`UseNativeMicroArch` only change the compiler.

- DLSS Zero-MV、DLSS Frame Generation、DLSSNR、XeSS Frame Generation 和 RTX Video 在开关关闭时会在日志中输出 `... is disabled at build time`；完整编译不会出现这类日志。
- TensorRT 深度推理首次运行时会构建引擎并缓存到 `%LOCALAPPDATA%\Magpie\FrameGuidance\TensorRTCache`，之后启动直接加载缓存。TensorRT 无法启动时改用 DirectML 后端。
- 启用全部后端后输出目录约 5.6 GB，主要来自 CUDA、cuDNN 和 TensorRT。
- `BuildOptions.props` 中的其余开关不是后端。请保持 `UseCompSwapchain` 关闭：它会替换 DLSS FG 使用的 FLIP_SEQUENTIAL 交换链。`DebugBorder` 和 `DebugInfoOnOverlay` 仅用于调试，`IsPackaged` 暂不支持，`UseClangCL`/`UseNativeMicroArch` 只影响编译器。
