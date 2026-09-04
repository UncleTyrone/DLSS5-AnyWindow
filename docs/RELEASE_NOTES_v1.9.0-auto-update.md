# DLSS5 AnyWindow 1.9.0 — Automatic Updates and Project Links

## 自动更新

- 悬浮控制器新增“关于”页，默认在启动时后台检查 GitHub Release。
- 检查不会阻塞界面；发现新版本后，由用户点击“下载并安装”确认更新。
- 自动安装只替换悬浮控制器自身，不会覆盖 `DLSS5-settings.ini`、`DLSSNR-Runtimes`、`FrameGuidance`、NGX 运行库或其他滤镜文件。
- 更新附件通过 HTTPS 下载，并要求同一 Release 中存在配套的 SHA-256 校验文件；附件缺失或校验失败时拒绝自动安装。
- 如果新版只有源码、没有控制器更新附件，界面会改为打开 Release 页面，不会尝试修改本机文件。

## 项目信息

- “关于”页新增 GitHub 源代码链接：<https://github.com/Shangyuwang11/DLSS5-AnyWindow>
- Windows 文件属性中加入产品名和 `1.9.0` 版本信息。
- “关于”页不提供作者个人主页入口。
- 快捷键支持一个修饰键加一个普通键，例如 `Ctrl+A` 或 `Alt+Q`，不要求三个键。

## Release 附件约定

自动更新只识别以下两个同版本附件：

- `DLSS5FloatingController-1.9.0.exe`
- `DLSS5FloatingController-1.9.0.exe.sha256.txt`

后续版本按相同命名规则递增版本号。公开附件仅包含独立悬浮控制器；第三方或专有运行库仍不进入源码仓库及该自动更新附件。

## English summary

The floating controller now has an About page, non-blocking GitHub Release checks, explicit user-confirmed self-update, SHA-256 verification, the repository link, and two-key shortcut capture. The updater replaces only the controller executable and preserves all user settings, models, and local rendering runtimes.
