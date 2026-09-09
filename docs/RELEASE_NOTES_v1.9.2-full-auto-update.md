# DLSS5 AnyWindow 1.9.2 — 完整自动更新

## 用户可见变化

- “启动时自动更新”默认开启。启动时如果 GitHub Release 有新版，程序会自动下载、校验、安装新控制器并重启。
- 重启后，新控制器会在界面出现前校验同版本的窗口渲染器和 DLSSNR 滤镜；如果不一致或缺失，会自动下载并替换。
- “关于”页仍可关闭自动更新，关闭后可手动点击“立即检查更新”。

## 安全和回滚

- 控制器、窗口渲染器和 HLSL 滤镜全部使用固定 SHA-256 校验；内容不符时拒绝安装。
- 组件先下载到同目录临时文件，再用备份和原子替换安装。任一文件失败时回滚已替换文件。
- 更新不覆盖 `DLSS5-settings.ini`、`DLSSNR-Runtimes`、`FrameGuidance`、NGX 运行库、深度模型或其他本地文件。
- 无网络、GitHub 不可达、校验失败或文件被占用时，保留旧组件，下次启动可重试。

## 兼容旧版

1.9.2 Release 提供旧更新器能识别的控制器 EXE 及配套 SHA-256 文件。1.9.0 和 1.9.1 用户可直接通过程序内更新到 1.9.2；新控制器首次重启时会完成剩余组件更新。

## 公开附件

- `DLSS5FloatingController-1.9.2.exe`
- `DLSS5FloatingController-1.9.2.exe.sha256.txt`
- `DLSSNRWindowDouble-1.9.2.exe`
- `DLSSNR_AI_Filter-1.9.2.hlsl`
- 上述组件的 SHA-256 文件
- `DLSS5-AnyWindow-1.9.2-UpdatePatch.zip` 及 SHA-256，作为手动更新备用方式

公开附件不包含 NVIDIA 专有运行库或模型。

## English summary

Version 1.9.2 completes the updater. With startup automatic updates enabled, the controller downloads and verifies the new controller, restarts, then synchronizes the matching renderer and DLSSNR effect before showing the UI. All three public components are SHA-256 verified. Component replacement is staged and rollback-safe, while local settings, GPU-specific runtimes, NGX files, and models remain untouched.
