# DLSS5 AnyWindow 1.9.1 — 极限处理分辨率

## 新增档位

- DLSS5 / DLSSNR 内部处理分辨率现在提供 `100 / 75 / 67 / 50 / 33 / 25%` 六档，默认仍为 `100%`。
- `33% / 25%` 为极限性能实验档，可进一步降低 DLSSNR、引导图和残差路径的内部像素数量。
- 目标窗口的位置、窗口尺寸和最终输出分辨率保持不变；低分辨率处理后的残差会重新放大并合成到原尺寸画面。
- 极限档会明显减少文字、细线和纹理细节，并可能增加动态画面的闪烁或不稳定，建议用户按实际 GPU 性能和内容选择。

## 升级说明

本功能同时修改悬浮控制器、窗口渲染器、Magpie Core 和 DLSSNR 效果定义。由 1.9.0 升级时必须下载并解压完整便携包，不能只替换悬浮控制器。

1.9.0 的自动检查会识别本 Release 并显示新版提醒。由于本次不是单文件控制器更新，点击更新按钮会打开 GitHub Release 页面。已有 1.9.0 完整包的用户可下载 1.9.1 更新补丁，把补丁中的三个文件覆盖到原目录；也可以重新获取完整便携包。

公开更新补丁只包含本项目编译出的控制器、窗口渲染器和 DLSSNR 效果定义，不包含 NVIDIA DLL、泄露或社区修改运行库、深度模型。Release 不提供会被旧版误认为“可单文件自动安装”的独立控制器附件。

## English summary

DLSS5 AnyWindow 1.9.1 adds 33% and 25% experimental internal processing-resolution options. The overlay and output resolution remain unchanged. These modes reduce internal GPU work at a significant image-quality cost. Upgrading from 1.9.0 requires either the public three-file update patch or a refreshed complete portable package because both the controller and rendering runtime changed.
