# DLSS5 AnyWindow 1.1.0 Portable Fixed

这是“任意窗口 DLSSNR 滤镜”的源码版本，对应本地验证过的 `DLSS5-AnyWindow-1.1.0-Portable-Fixed` 便携包。

> 这是实验性第三方项目，并非 NVIDIA 官方 DLSS 产品。“DLSS5”只是本项目沿用的界面名称；实际核心调用实验性 DLSSNR Feature 18。

## 本版内容

- 悬浮控制器可选择当前窗口并启动或停止滤镜。
- 支持 1–4 次串联处理，并在运行时显示捕获帧率、CPU/GPU 占用和估算延迟。
- 捕获时不主动改变目标窗口的位置、尺寸或最大化状态。
- 增加目标窗口合法性检查、重复实例拦截、全屏/最小化状态处理及退出清理。
- 增加独立的离屏处理入口，供后续图像和视频渲染器调用。
- 运行日志会记录 Feature 18 创建结果、Evaluate 成功/失败计数和实际处理次数，不能只凭 NVIDIA Indicator 判断是否生效。

## 源码位置

- `src/DLSS5FloatingController/`：悬浮控制器。
- `src/DLSSNRWindowDouble/`：窗口捕获与 1–4 次 DLSSNR 处理核心。
- `src/DLSSNROffscreen/`：实验性离屏图像帧处理入口。
- `src/Magpie.Core/`：DLSSNR 后端、资源桥接及状态统计。
- `src/Shared/DLSS5WindowTarget.h`：窗口选择与目标验证。
- `src/Shared/DLSS5StatsShared.h`：控制器和渲染核心共享的统计结构。

## 构建

要求 Windows x64、Visual Studio 2022 v143、Windows SDK 10.0.26100，以及由使用者自行取得并接受许可证的 NVIDIA DLSS SDK/运行库。

不要修改或提交默认的 `src/BuildOptions.props`。在本机创建已被 Git 忽略的 `src/BuildOptions.props.user`，示例：

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <EnableDLSSNR>true</EnableDLSSNR>
    <EnableDLSSZeroMV>true</EnableDLSSZeroMV>
    <DLSSSdkDir>D:\Path\To\DLSS-SDK</DLSSSdkDir>
    <DLSSNRRuntimeDir>D:\Path\To\DLSSNR-Runtime</DLSSNRRuntimeDir>
  </PropertyGroup>
</Project>
```

先按 Magpie 的标准编译流程还原 NuGet/Conan 依赖并生成 `Magpie.Core`，再构建：

```powershell
msbuild src\DLSSNRWindowDouble\DLSSNRWindowDouble.vcxproj /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
msbuild src\DLSS5FloatingController\DLSS5FloatingController.vcxproj /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
msbuild src\DLSSNROffscreen\DLSSNROffscreen.vcxproj /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\"
```

## 发布与许可证

派生源码遵循仓库根目录的 GPLv3。NVIDIA SDK、导入库和运行时 DLL 不属于本源码，也不得提交到 Git 历史。

当前本地便携包含实验性 `nvngx_dlssnr.dll`。其公开再分发权限以及它与 GPLv3 组合分发的兼容性尚未完成审核，因此源码发布不自动授权公开上传该 DLL 或包含它的完整便携包。详见 [`THIRD_PARTY_AND_REDISTRIBUTION.md`](THIRD_PARTY_AND_REDISTRIBUTION.md)。

本地验收包 SHA-256：

```text
B75823048F9C32654C82952E4F6D3C656D508BD0930EF818F4C32DC693A0919E
```
