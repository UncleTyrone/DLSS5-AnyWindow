# DLSS5 AnyWindow 1.8.0 — Floating-window Hotkey and Defaults

## 浮窗隐藏/显示快捷键

- 新增独立的全局“隐藏/显示浮窗”快捷键，默认是 `Ctrl+Alt+F9`。
- 隐藏控制浮窗不会停止滤镜核心，适合在图片或视频上截图；再次按下同一快捷键即可恢复。
- “外观”设置页可分别修改滤镜开关快捷键与浮窗快捷键。
- 两个快捷键都会持久化到 `DLSS5-settings.ini`，并检测重复或被其他程序占用的组合键。
- `Esc` 取消录入；`Backspace` 或 `Delete` 可单独停用当前录入的快捷键。

## 新的初始默认参数

- 处理引擎：`DLSS5 / DLSSNR`
- 风格：默认
- 效果强度、局部色调、局部结构：100%
- 自动遮罩：开启
- 历史策略：连续
- 处理次数：1 次
- 帧引导：深度
- 深度更新间隔：1 帧

如果便携包没有完整的深度模型与 DirectML 运行组件，控制器会自动回退到平面引导，避免启动失败。已有用户配置不会被覆盖；点击“恢复默认”即可应用新默认值。

## Verification

- Release x64 controller build passed.
- A private controller window verified the requested defaults, separate hotkey registration, editable visibility shortcut persistence, and hide/show behavior.
- Hiding and restoring the controller leaves the process alive and does not send a stop request to the renderer.

The application remains an external capture-and-overlay renderer. No game injection component was added. Proprietary DLSSNR runtime/model files remain local build inputs and are not part of the source tag.
