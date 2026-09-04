# DLSS5 AnyWindow 1.5.0 Multi-GPU Runtime Selector

这是任意窗口 DLSSNR 实验后端的多显卡运行库自动选择版本。它没有把 DLSSNR 描述成 NVIDIA 正式公开的 DLSS 5 产品；运行时文件仍是本地提供的泄露或社区修改二进制，不属于源码仓库。

## 自动选择

- 从 Renderer 实际创建 D3D11 设备所使用的 DXGI Adapter 读取名称、Device ID 和 LUID。
- GeForce RTX 20/30 选择 `DLSSNR-Runtimes/RTX20-30-SF-v2/nvngx_dlssnr.dll`。
- GeForce RTX 40 选择 `DLSSNR-Runtimes/RTX40/nvngx_dlssnr.dll`。
- GeForce RTX 50 选择 `DLSSNR-Runtimes/RTX50/nvngx_dlssnr.dll`。
- 专业卡或 OEM 名称无法直接判断时，才动态查询 CUDA Compute Capability；普通 GeForce 初始化不会额外触发 CUDA 查询。
- 选中的 DLL 在 NGX Core 初始化之前预加载，避免 Windows 先绑定程序根目录的同名 DLL。
- 控制器共享状态与日志都会报告实际运行库；共享结构版本升级为 3。

## 本机构建输入

在不提交到 Git 的 `src/BuildOptions.props.user` 中配置：

```xml
<DLSSNRRuntimeSFV2Dir>D:\Path\To\SF-v2</DLSSNRRuntimeSFV2Dir>
<DLSSNRRuntimeRTX40Dir>D:\Path\To\RTX40</DLSSNRRuntimeRTX40Dir>
<DLSSNRRuntimeRTX50Dir>D:\Path\To\RTX50</DLSSNRRuntimeRTX50Dir>
```

`DLSSNRWindowDouble` 和完整 `Magpie` Release 构建会把存在的运行库复制到对应子目录。源码、GitHub 自动源码归档和可复现的公开构建不得包含这些 DLL。

## 手动诊断

环境变量 `MAGPIE_DLSSNR_RUNTIME` 可设为 `auto`、`sf-v2`、`rtx20`、`rtx30`、`rtx40`、`rtx50`、`original`，也可填写自定义 DLL 或目录路径。强制路径不存在时不会无声回退到其他模型。

## 验证范围

- Release x64 编译通过。
- RTX 4080 自动选择 RTX40 修改版，Feature 18 创建成功，Evaluate 连续成功。
- RTX 4080 强制选择 SF-v2，Feature 18 创建成功，Evaluate 连续成功。
- 两次测试均在程序根目录不存在 `nvngx_dlssnr.dll` 的条件下完成。
- RTX 20、30、50 仍需对应硬件真机验收；RTX 4080 上能加载 SF-v2 不等于已经证明它在所有旧卡上可用。
