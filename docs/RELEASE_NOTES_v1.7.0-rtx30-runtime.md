# DLSS5 AnyWindow 1.7.0 — RTX 30 Runtime Split

## Changes

- RTX 20 and RTX 30 now use separate runtime folders and separate automatic-selection branches.
- RTX 30 uses the newly supplied `310.8.0.0` community-patched runtime; RTX 20 keeps the SF-v2 FP16 runtime.
- `MAGPIE_DLSSNR_RUNTIME=rtx20` and `MAGPIE_DLSSNR_RUNTIME=rtx30` can force either branch for diagnosis.
- The floating controller reports `RTX20 SF-v2 FP16`, `RTX30 PATCH`, `RTX40 PATCH`, or `RTX50 ORIGINAL` after the feature starts.
- The application remains an external AnyWindow capture-and-overlay renderer. No game injection component is included.

## Compatibility note

- The RTX 40 automatic branch and the forced RTX 20/30 branches were loaded and evaluated successfully on the development RTX 4080.
- An external RTX 3090 test subsequently confirmed that the automatic RTX 30 branch starts successfully and produces a visible processed result.
- This confirms the current RTX 30 path on the tested machine; it is not a guarantee for every RTX 30 model, driver, Windows build, or protected-content window.
- RTX 20 and RTX 50 still require generation-matched field testing.

## RTX 30 实机结果

- 外部 RTX 3090 实机已经确认：自动识别 RTX 30 分支、Feature 18 正常启动，并能观察到实际处理效果。
- 这说明当前 RTX 30 路径在该测试环境中可用，但不代表所有 RTX 30 型号、驱动版本、Windows 版本和受保护窗口均保证兼容。
- RTX 20 和 RTX 50 仍需对应显卡实机验收。

## Safety and redistribution

This remains an experimental compatibility build. The DLSSNR model/runtime files are not an official public NVIDIA release. The source tag does not contain these proprietary binaries. Do not describe the project as official DLSS 5 support, and do not publish proprietary binaries before their redistribution terms have been reviewed.
