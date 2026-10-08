<br>
<p align="center">
  <img src="./src/Magpie/Icons/SVG/Magpie Icon Full Disabled.svg" width="150px" height="150px" alt="Logo">
</p>
<h1 align="center">Magpie</h1>

<div align="center">

[![License](https://img.shields.io/github/license/Blinue/Magpie)](./LICENSE)
[![build](https://github.com/Blinue/Magpie/actions/workflows/build.yml/badge.svg)](https://github.com/Blinue/Magpie/actions/workflows/build.yml)
[![All Contributors](https://img.shields.io/github/all-contributors/Blinue/Magpie)](#acknowledgement-)
[![Translation status](https://hosted.weblate.org/widget/magpie/svg-badge.svg)](https://hosted.weblate.org/engage/magpie)

</div>

🌍 **English** | [简体中文](./README_ZH.md)

Magpie is a lightweight window upscaling tool that comes equipped with a variety of efficient scaling algorithms and filters.

## About this experimental fork

This repository is an independent experimental fork of [Blinue/Magpie](https://github.com/Blinue/Magpie). Magpie and the majority of this codebase were created by Blinue and the upstream contributors. This fork is not an official Magpie release and is not endorsed or supported by the upstream project.

The fork owner used OpenAI Codex as a development assistant to add and test experimental captured-frame integrations for:

- NVIDIA DLSS Super Resolution with shared NVIDIA Optical Flow motion and optional estimated depth.
- NVIDIA DLSS Frame Generation that targets the display refresh rate, using the configured 2x/3x/4x multiplier as a ceiling, with the same shared guidance.
- NVIDIA DLSSNR as a same-resolution SDR AI filter through a locally supplied direct-runtime path.
- AMD FidelityFX Super Resolution 2.2.1 with zero motion vectors, synthetic jitter metadata, or 50%-resolution colour optical flow.
- AMD FidelityFX Super Resolution 3.1.5 upscaling (without frame generation) through D3D11/D3D12 interoperability, with zero motion vectors, synthetic jitter metadata, or 50%-resolution colour optical flow.
- AMD FidelityFX Super Resolution 4.1.1 INT8 with experimental Zero-MV, synthetic-jitter, and 50%-resolution colour-optical-flow modes.
- Intel XeSS 3.0.1 Super Resolution with zero motion vectors, synthetic jitter metadata, or 50%-resolution colour optical flow through D3D11/D3D12 interoperability.
- Intel XeSS Frame Generation with a cross-vendor x2 path and an Intel Arc x2-x4 multi-frame path.
- NVIDIA VideoSuperRes for same-resolution denoising and VSR upscaling.
- MLAA and captured-colour-frame approximations of SMAA T2x/4x, with jittered and non-jittered variants.

These integrations do not have access to engine depth, engine motion vectors, exposure, reactive masks, camera matrices, or projection jitter. Renderer-owned optical flow and Depth Anything V2 are estimates from captured colour, so ghosting and unstable detail remain possible. These are research prototypes, not replacements for native in-game DLSS or FSR support.

### Initial test observations

The following observations are subjective results from a limited set of systems and games; they are not general performance or image-quality claims.

For normal use, choose either `DLSS SR_Experimental` or `RTXVideo_VSR_Ultra`; stacking them is not recommended. DLSS SR defaults to motion guidance on and estimated depth off. The synthetic-Jitter and old Optical Flow names remain legacy experiments.

#### DLSS Super Resolution

`DLSS SR_Experimental` keeps the compatible internal identifier `DLSS_ZeroMV` but exposes two independent inputs. `Use Motion Vectors` supplies current-to-previous source-pixel optical flow and defaults to on. `Use Estimated Depth` supplies normalized inverse relative depth and defaults to off. Depth can still use optical flow internally for temporal stabilization when motion delivery is disabled. Real guidance is only bound when the DLSS input size matches the captured source; otherwise the channel safely falls back to Zero.

| Effect | Observed advantages | Observed disadvantages | Recommendation |
| --- | --- | --- | --- |
| `DLSS SR_Experimental` | Reuses one Renderer-level motion/depth group and safely falls back per channel | Estimated inputs are not engine data and can still produce ghosting | Current non-Jitter test path |
| `DLSS_ZeroMV_Jitter` | Retains the earlier metadata-only experiment | The source projection is not jittered, so the metadata is not self-consistent | Legacy; not used by the new guidance path |
| `DLSS_OpticalFlow` | Compatible alias that now requests the shared motion provider | No estimated-depth control and the old name is retained only for profiles | Legacy |

Moving objects, camera motion, and disocclusion can still produce ghosting because the inputs are inferred from captured colour. The non-Jitter path is the current development target.

<details>
<summary>Supplement: driver-level DLSS model/preset overrides</summary>

M, L, J/K, and CNN below are DLSS models or presets selected through driver-level overrides. They are not separate Magpie Effects and do not change the priority of the three Effects above.

| DLSS model/preset | Observed advantages | Observed disadvantages | Recommendation |
| --- | --- | --- | --- |
| M | Good antialiasing with the best overall balance; its temporal weighting appeared lower than L, resulting in less ghosting | Slightly less smooth than L | Recommended; balanced |
| L | Smoothest antialiasing and edges | Slightly more temporal ghosting than M; expensive when used for DLAA or near-native-resolution rendering | For users who prioritize smoothness and have sufficient GPU headroom |
| J/K | Functional, but produced no compelling advantage over M/L in this colour-only integration | Overall reconstruction quality was unremarkable | Not recommended |
| CNN | Conservative but generally unremarkable in these tests | Antialiasing and reconstruction were weaker than M/L | Not recommended |

Creating a dedicated Magpie application profile in NVIDIA Profile Inspector is recommended when testing driver-level DLSS model or preset overrides. Switch between M and L per game: M is the more balanced choice, while L prioritizes smoother antialiasing. The public code still requests Balanced mode and Preset J; driver overrides are external to Magpie.

</details>

#### DLSSNR AI filter

`DLSSNR_AI_Filter` is a same-resolution SDR post-process and does not upscale. It consumes the shared Frame Guidance group. The Effect exposes Style, Intensity, Local Tone, Local Structure, Automatic Mask, and Frame Guidance controls; NR Preset is fixed internally.

Feature 18 uses the direct D3D12 exports of a locally supplied `nvngx_dlssnr.dll`; NGX Core only allocates and destroys the parameter block. The filter cannot separate UI already composited into the captured frame and may alter text/UI or produce ghosting and structural drift. HDR is not currently supported.

#### Frame Generation experiments

DLSS FG runs after the effect chain. It can bind the same captured-frame motion and optional estimated depth used by DLSS SR while keeping final colour at backbuffer resolution. XeSS FG remains on its earlier Zero-MV/flat-depth path. Do not combine a Frame Generation Effect with NVIDIA Smooth Motion or another Frame Generation Effect.

While Frame Generation is active, Magpie forces exact duplicate-frame filtering and stops synthesizing repeated base frames through its minimum-FPS timer. XeSSFG also ignores frontend Presents caused only by Magpie's software cursor or overlay, preventing mouse movement from inflating the SDK input rate; the cursor updates with the next genuine captured frame.

| Effect | Hardware and multiplier | Current status |
| --- | --- | --- |
| `DLSS FG_Experimental` | NVIDIA RTX; x2/x3/x4 ceiling, refresh-targeted | Motion defaults on; estimated depth defaults off. Output targets the display refresh rate, down to 1x and up to the configured multiplier. Shared D3D11/D3D12 resources use the same captured base-frame ID. Failure protection preserves real frames and disables DLSSFG only for the current scaling session after repeated failures. |
| XeSS Frame Generation x2 Zero-MV | Compatible Intel, NVIDIA, and AMD GPUs; x2 | Experimental cross-vendor path using the XeSS-FG D3D12 proxy swap chain. |
| XeSS Multi-Frame Generation x2-x4 Zero-MV | Intel Arc; x2/x3/x4 | Experimental Arc multi-frame path. The requested multiplier is clamped to the capability reported by the GPU and driver; non-Arc hardware is limited or falls back to x2. |

These paths can increase displayed frame rate but cannot reconstruct correct object motion, UI separation, disocclusion, or camera changes without game integration. They may add latency or visible interpolation artefacts and should be tested per application.

#### DLSS FG refresh-rate targeting

`Frame Multiplier` on `DLSS FG_Experimental` is a ceiling, not a fixed rate. Each generated frame occupies one display refresh, so presenting more frames than the display can show makes real captured frames wait or get dropped, and DLSS then has to interpolate across larger, uneven gaps. That shows up as ghosting with no gain in frame rate.

DLSS FG therefore aims output at the refresh rate of the monitor showing the scaled window:

- The refresh rate is read when frame generation starts.
- Every 0.5 s, Magpie measures the source's real frame rate. It counts every frame Windows Graphics Capture delivered, including older frames drained while the backend was busy, and scales that by the share the duplicate-frame filter kept. The measurement therefore does not fall when presentation falls behind.
- Generated frames per real frame = refresh ÷ source FPS − 1, clamped between 0 and `Frame Multiplier` − 1. Fractional targets are spread across real frames; 1.5x, for example, alternates one and zero generated frames.
- A real frame that receives no generated frame is still fed to DLSS, so frame-generation history stays continuous.

| Display | Source | Multiplier 2x | Multiplier 3x | Multiplier 4x |
| --- | --- | --- | --- | --- |
| 240 Hz | ~105 FPS | 2x | ~2.3x | ~2.3x |
| 240 Hz | 60 FPS | 2x | 3x | 4x |
| 90 Hz | 60 FPS | 1.5x | 1.5x | 1.5x |

There is no switch for this behaviour. Each per-second `DLSSFG presentation` log line reports the measured `source` FPS and the current `target` multiplier. Generated frames are presented back-to-back on a FLIP_SEQUENTIAL tearing swap chain with a maximum frame latency of 1; no sleep-based pacing or present queue sits in the frame path.

#### FSR 2.2.1

| Effect | Observed advantages | Observed disadvantages | Recommendation |
| --- | --- | --- | --- |
| `FSR2_ZeroMV` | The result was reasonably good and visually smooth; cross-vendor and lighter than the Optical Flow path | Motion can still leave history artefacts because real disocclusion data is unavailable | Recommended, especially when DLSS is unavailable |
| `FSR2_OpticalFlow` | Experimental colour-motion input | Image quality was substantially worse than Zero-MV while adding flow-estimation cost | Not recommended |

#### FSR 3.1.5 Upscaler

FSR3 uses AMD's signed DirectX 12 FSR API runtime. Magpie remains a D3D11 renderer, so this backend shares and copies resources and synchronizes fences across D3D11 and D3D12. Frame generation is not integrated.

| Effect | Observed advantages | Observed disadvantages | Recommendation |
| --- | --- | --- | --- |
| `FSR3_ZeroMV` | The backend may be functioning and remains useful as an integration experiment | Current image quality was poor, and the D3D11-to-D3D12 bridge adds copies, synchronization, and uncertainty about whether every input is handled correctly | Not recommended |
| `FSR3_OpticalFlow` | Tests the upscaler with estimated motion input | Combines the cross-API overhead with unreliable colour flow and produced poor results | Not recommended |

#### NVIDIA RTX Video

RTX Video provides same-resolution denoising and true VSR upscaling. Its strongest use cases were Galgames, visual novels, and streaming video, where it improved compressed imagery, blurred lines, text, and character edges.

| Effect/quality | Observed result | Recommendation |
| --- | --- | --- |
| Same-resolution Denoise | Improvement was generally modest and it does not upscale | Not recommended |
| VSR Low | Lower cost, but image-quality gains were limited | Not recommended |
| VSR Medium | Better than Low, but not compelling compared with High/Ultra | Not recommended |
| VSR High | Strong image-quality improvement with lower cost than Ultra | Recommended when Ultra is too expensive |
| VSR Ultra | Best reconstruction and denoising in Galgames and streaming video | First choice for Galgames and streaming video |

The trade-off is that RTX Video is NVIDIA-only and its VFX runtime and models greatly increase the distribution size. VSR worked on the developer's Windows 11 test machine, while one Windows 10 system failed to create VideoSuperRes; GPU, driver, OS, and runtime versions may all affect compatibility.

#### NVIDIA Smooth Motion compatibility mode

When Smooth Motion is enabled for Magpie through NVIDIA Profile Inspector, repeatedly starting and stopping scaling may cause driver-retained GPU memory to grow. Enable Smooth Motion compatibility mode under **Settings → General** to restart Magpie after each scaling session, allowing process exit to release those driver resources. The restart path preserves whether Magpie was windowed, minimized, or in the tray; the replacement waits for the previous process to exit before initializing and the old process releases shortcuts before replacement. The option is disabled by default and is intended only for Smooth Motion users.

#### Intel XeSS 3.0.1

XeSS uses the cross-vendor D3D12 DP4a path and can run on compatible Intel, NVIDIA, and AMD GPUs. Magpie shares textures and fences with D3D12; the Intel-only native D3D11 XeSS path is deliberately not used.

| Effect | Observed advantages | Observed disadvantages | Recommendation |
| --- | --- | --- | --- |
| `XeSS_ZeroMV` | The backend may be functioning and demonstrates the cross-vendor DP4a path | Current image quality was poor; constant depth cannot identify disocclusion, and the D3D11-to-D3D12 bridge adds overhead and integration uncertainty | Not recommended |
| `XeSS_OpticalFlow` | Tests XeSS with estimated motion input | Still lacks real depth, adds flow and cross-API overhead, and produced poor results | Not recommended |

#### Optical Flow and DLDSR

None of the current 50%-resolution colour Optical Flow variants are recommended, including the DLSS, FSR2, FSR3, and XeSS versions. Colour flow cannot replace engine-provided motion vectors and disocclusion data; it did not produce consistent image-quality gains and sometimes made the result substantially worse while consuming additional GPU time.

On NVIDIA systems, if a game and display mode support it and additional GPU cost is acceptable, DLDSR is generally a more sensible image-quality path than estimating motion from captured frames. DLDSR renders the game at a higher resolution and then downsamples it, so it is not a direct replacement for temporal upscaling, but it starts with more reliable source samples.

### External development dependencies

Third-party SDKs, models, wheels, and proprietary NVIDIA binaries are not part of this source repository. Obtain them from their original publishers and accept their respective licenses:

- [Original Magpie repository](https://github.com/Blinue/Magpie)
- [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS)
- NVIDIA Optical Flow API headers (`nvOpticalFlowCommon.h`, `nvOpticalFlowD3D11.h`) from the [NVIDIA Optical Flow SDK](https://developer.nvidia.com/opticalflow-sdk), enabled with `EnableNvidiaOpticalFlow` and `NvidiaOpticalFlowSdkDir`. The runtime `nvofapi64.dll` ships with the NVIDIA driver and is loaded dynamically; builds without it use the software half-resolution flow.
- [AMD FidelityFX FSR 2.2.1](https://github.com/GPUOpen-Effects/FidelityFX-FSR2/tree/v2.2.1)
- [Community FSR2 DirectX 11 backend](https://github.com/gameplug-labs/FidelityFX-FSR2-DX11)
- [AMD FSR SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
- [Intel XeSS SDK](https://github.com/intel/xess)
- [NVIDIA Video Effects SDK samples and setup instructions](https://github.com/NVIDIA-Maxine/VFX-SDK-Samples)
- [NVIDIA `nvidia-vfx` package](https://pypi.org/project/nvidia-vfx/)

To build the optional backends, create `src/BuildOptions.props.user` and define the feature switches and local SDK paths there. The file is excluded by `.gitignore`; it is machine-specific and must not be committed to the public repository. [Building with every optional backend](docs/BUILD_ALL_FEATURES.md) lists every switch with the tested dependency versions, download sources, required folder layouts, and an example file.

The v1.9.2 AnyWindow source supports separate runtime-selection branches for RTX 20, RTX 30, RTX 40, and RTX 50. An external RTX 3090 field test confirmed that the RTX 30 branch starts and produces a visible processed result. The controller provides separately editable filter-toggle and floating-window visibility hotkeys, two-key combinations such as `Ctrl+A`, and an About page with GitHub updates and the project source link. The updater can replace the controller, matching external-window renderer, and DLSSNR effect with SHA-256 verification and rollback while preserving local settings, GPU runtimes, and models. Runtime/model DLLs remain local build inputs only: they are not included in this source tree or GitHub's automatically generated source archives. See the [v1.9.2 release notes](docs/RELEASE_NOTES_v1.9.2-full-auto-update.md) for the update flow and safety behavior.

Native upscalers and RTX Video are dispatched through the shared `NativeEffectBackend` interface and `NativeEffectBackendFactory`. This keeps SDK-specific detection, creation, resize, and draw logic out of the main Renderer path. Frame-generation presenters remain separate because they publish additional frames at the terminal presentation stage.

See [Third-party components and redistribution](docs/THIRD_PARTY_AND_REDISTRIBUTION.md) for the per-component inventory and release checklist.

### Source and binary distribution

The Magpie-derived source remains licensed under GPLv3. Source distributions must preserve the upstream copyright and license notices. AMD FSR2/FSR3 and Intel XeSS have their own notices and terms. NVIDIA SDKs and runtime files remain under NVIDIA's proprietary terms.

Do not treat a GitHub download link as permission to redistribute a third-party SDK. In particular, do not commit SDK folders, models, wheels, or NVIDIA DLLs to this repository. Public binary releases that combine GPLv3 Magpie code with proprietary NVIDIA components require a separate license-compatibility and redistribution review. The FSR 4 capability-check override likewise requires a specific terms/permission review before binary distribution. Until those reviews are complete, publish source code only, or publish builds without the affected components. This is a project-maintainer precaution, not legal advice.

👉 [Original Magpie releases](https://github.com/Blinue/Magpie/releases)

👉 [FAQ](https://github.com/Blinue/Magpie/wiki/FAQ%20(EN))

👉 [Built-in effects](https://github.com/Blinue/Magpie/wiki/Built-in%20effects)

👉 [Compilation guide](https://github.com/Blinue/Magpie/wiki/Compilation%20guide)

## Features

* Supports both fullscreen and windowed scaling
* Includes a variety of built-in algorithms and filters, including [Anime4K](https://github.com/bloc97/Anime4K), [FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR), CRT shaders, and more
* WinUI-based user interface with support for light and dark themes
* Multi-monitor support

## Screenshots

<div style="display:flex; gap:10px;">
  <img src="img/main-window.png" alt= "Main window" height="300">
  <img src="img/screenshot.png" alt= "Main window" height="300">
</div>

## Hints

1. If you have set DPI scaling and the window you want to scale does not support high DPI (which is common in older games), it is recommended to first enter the program's compatibility settings and set "High DPI scaling override" to "Application".

2. Some games support zooming the window, but with extremely naive algorithms. Please set the resolution to the built-in (best) option.

## System requirements

1. Windows 10 v1903+ or Windows 11
2. DirectX feature level 11

## Localization

Thanks to [Weblate](https://weblate.org) for hosting! Click the image below to visit the translation page.

[![Translation status](https://hosted.weblate.org/widget/magpie/multi-auto.svg)](https://hosted.weblate.org/engage/magpie)

## Acknowledgement

Thanks go to these wonderful people:

<!-- ALL-CONTRIBUTORS-LIST:START - Do not remove or modify this section -->
<!-- prettier-ignore-start -->
<!-- markdownlint-disable -->
<table>
  <tbody>
    <tr>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/Blinue"><img src="https://avatars.githubusercontent.com/u/34770031?v=4?s=100" width="100px;" alt="Xu"/><br /><sub><b>Xu</b></sub></a><br /><a href="#maintenance-Blinue" title="Maintenance">🚧</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/hooke007"><img src="https://avatars.githubusercontent.com/u/41094733?v=4?s=100" width="100px;" alt="hooke007"/><br /><sub><b>hooke007</b></sub></a><br /><a href="https://github.com/Blinue/Magpie/commits?author=hooke007" title="Documentation">📖</a> <a href="#question-hooke007" title="Answering Questions">💬</a> <a href="#userTesting-hooke007" title="User Testing">📓</a> <a href="https://github.com/Blinue/Magpie/commits?author=hooke007" title="Code">💻</a></td>
      <td align="center" valign="top" width="14.28%"><a href="http://palxex.ys168.com"><img src="https://avatars.githubusercontent.com/u/58222?v=4?s=100" width="100px;" alt="Pal Lockheart"/><br /><sub><b>Pal Lockheart</b></sub></a><br /><a href="#userTesting-palxex" title="User Testing">📓</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://www.stevedonaghy.com/"><img src="https://avatars.githubusercontent.com/u/1029699?v=4?s=100" width="100px;" alt="Steve Donaghy"/><br /><sub><b>Steve Donaghy</b></sub></a><br /><a href="https://github.com/Blinue/Magpie/commits?author=neoKushan" title="Code">💻</a> <a href="#translation-neoKushan" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="http://gyrojeff.top"><img src="https://avatars.githubusercontent.com/u/30655701?v=4?s=100" width="100px;" alt="gyro永不抽风"/><br /><sub><b>gyro永不抽风</b></sub></a><br /><a href="https://github.com/Blinue/Magpie/commits?author=JeffersonQin" title="Code">💻</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/ButtERRbrod"><img src="https://avatars.githubusercontent.com/u/89013889?v=4?s=100" width="100px;" alt="ButtERRbrod"/><br /><sub><b>ButtERRbrod</b></sub></a><br /><a href="#translation-ButtERRbrod" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/0x4E69676874466F78"><img src="https://avatars.githubusercontent.com/u/4449851?v=4?s=100" width="100px;" alt="NightFox"/><br /><sub><b>NightFox</b></sub></a><br /><a href="#translation-0x4E69676874466F78" title="Translation">🌍</a></td>
    </tr>
    <tr>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/Tzugimaa"><img src="https://avatars.githubusercontent.com/u/4981077?v=4?s=100" width="100px;" alt="Tzugimaa"/><br /><sub><b>Tzugimaa</b></sub></a><br /><a href="https://github.com/Blinue/Magpie/commits?author=Tzugimaa" title="Code">💻</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/WHMHammer"><img src="https://avatars.githubusercontent.com/u/35433952?v=4?s=100" width="100px;" alt="WHMHammer"/><br /><sub><b>WHMHammer</b></sub></a><br /><a href="#translation-WHMHammer" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/kato-megumi"><img src="https://avatars.githubusercontent.com/u/29451351?v=4?s=100" width="100px;" alt="kato-megumi"/><br /><sub><b>kato-megumi</b></sub></a><br /><a href="https://github.com/Blinue/Magpie/commits?author=kato-megumi" title="Code">💻</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/MikeWang000000"><img src="https://avatars.githubusercontent.com/u/11748152?v=4?s=100" width="100px;" alt="Mike Wang"/><br /><sub><b>Mike Wang</b></sub></a><br /><a href="#userTesting-MikeWang000000" title="User Testing">📓</a></td>
      <td align="center" valign="top" width="14.28%"><a href="http://sammyhori.com"><img src="https://avatars.githubusercontent.com/u/116026761?v=4?s=100" width="100px;" alt="Sammy Hori"/><br /><sub><b>Sammy Hori</b></sub></a><br /><a href="#translation-sammyhori" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/NeilTohno"><img src="https://avatars.githubusercontent.com/u/28284594?v=4?s=100" width="100px;" alt="NeilTohno"/><br /><sub><b>NeilTohno</b></sub></a><br /><a href="#translation-NeilTohno" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/a0193143"><img src="https://avatars.githubusercontent.com/u/32773311?v=4?s=100" width="100px;" alt="a0193143"/><br /><sub><b>a0193143</b></sub></a><br /><a href="#translation-a0193143" title="Translation">🌍</a></td>
    </tr>
    <tr>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/soulset001"><img src="https://avatars.githubusercontent.com/u/121711747?v=4?s=100" width="100px;" alt="soulset001"/><br /><sub><b>soulset001</b></sub></a><br /><a href="#translation-soulset001" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/WluhWluh"><img src="https://avatars.githubusercontent.com/u/52004526?v=4?s=100" width="100px;" alt="WluhWluh"/><br /><sub><b>WluhWluh</b></sub></a><br /><a href="#design-WluhWluh" title="Design">🎨</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/SerdarSaglam"><img src="https://avatars.githubusercontent.com/u/42881121?v=4?s=100" width="100px;" alt="Serdar Sağlam"/><br /><sub><b>Serdar Sağlam</b></sub></a><br /><a href="#translation-SerdarSaglam" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/AndrusGerman"><img src="https://avatars.githubusercontent.com/u/30560543?v=4?s=100" width="100px;" alt="Andrus Diaz German"/><br /><sub><b>Andrus Diaz German</b></sub></a><br /><a href="#translation-AndrusGerman" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/Kefir2105"><img src="https://avatars.githubusercontent.com/u/103105829?v=4?s=100" width="100px;" alt="Kefir2105"/><br /><sub><b>Kefir2105</b></sub></a><br /><a href="#translation-Kefir2105" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/animeojisan"><img src="https://avatars.githubusercontent.com/u/132756551?v=4?s=100" width="100px;" alt="animeojisan"/><br /><sub><b>animeojisan</b></sub></a><br /><a href="#translation-animeojisan" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/MuscularPuky"><img src="https://avatars.githubusercontent.com/u/93962018?v=4?s=100" width="100px;" alt="MuscularPuky"/><br /><sub><b>MuscularPuky</b></sub></a><br /><a href="#translation-MuscularPuky" title="Translation">🌍</a></td>
    </tr>
    <tr>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/Zoommod"><img src="https://avatars.githubusercontent.com/u/71239440?v=4?s=100" width="100px;" alt="Zoommod"/><br /><sub><b>Zoommod</b></sub></a><br /><a href="#translation-Zoommod" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/fil08"><img src="https://avatars.githubusercontent.com/u/125665523?v=4?s=100" width="100px;" alt="fil08"/><br /><sub><b>fil08</b></sub></a><br /><a href="#translation-fil08" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/IsaiasYang"><img src="https://avatars.githubusercontent.com/u/20205571?v=4?s=100" width="100px;" alt="攸羚"/><br /><sub><b>攸羚</b></sub></a><br /><a href="https://github.com/Blinue/Magpie/commits?author=IsaiasYang" title="Code">💻</a></td>
      <td align="center" valign="top" width="14.28%"><a href="http://ohaiibuzzle.dev"><img src="https://avatars.githubusercontent.com/u/23693150?v=4?s=100" width="100px;" alt="OHaiiBuzzle"/><br /><sub><b>OHaiiBuzzle</b></sub></a><br /><a href="#translation-ohaiibuzzle" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/Rastadu23"><img src="https://avatars.githubusercontent.com/u/52637051?v=4?s=100" width="100px;" alt="Rastadu23"/><br /><sub><b>Rastadu23</b></sub></a><br /><a href="#translation-Rastadu23" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/hauuau"><img src="https://avatars.githubusercontent.com/u/52239673?v=4?s=100" width="100px;" alt="hauuau"/><br /><sub><b>hauuau</b></sub></a><br /><a href="https://github.com/Blinue/Magpie/commits?author=hauuau" title="Code">💻</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/nellydocs"><img src="https://avatars.githubusercontent.com/u/71311423?v=4?s=100" width="100px;" alt="nellydocs"/><br /><sub><b>nellydocs</b></sub></a><br /><a href="#translation-nellydocs" title="Translation">🌍</a></td>
    </tr>
    <tr>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/funnyplanter"><img src="https://avatars.githubusercontent.com/u/173073947?v=4?s=100" width="100px;" alt="funnyplanter"/><br /><sub><b>funnyplanter</b></sub></a><br /><a href="https://github.com/Blinue/Magpie/commits?author=funnyplanter" title="Code">💻</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/eriforce"><img src="https://avatars.githubusercontent.com/u/8393109?v=4?s=100" width="100px;" alt="Erich Yu"/><br /><sub><b>Erich Yu</b></sub></a><br /><a href="https://github.com/Blinue/Magpie/commits?author=eriforce" title="Code">💻</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/TamilNeram"><img src="https://avatars.githubusercontent.com/u/67970539?v=4?s=100" width="100px;" alt="தமிழ் நேரம்"/><br /><sub><b>தமிழ் நேரம்</b></sub></a><br /><a href="#translation-TamilNeram" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/mhtvsSFrpHdE"><img src="https://avatars.githubusercontent.com/u/10773245?v=4?s=100" width="100px;" alt="mhtvsSFrpHdE"/><br /><sub><b>mhtvsSFrpHdE</b></sub></a><br /><a href="https://github.com/Blinue/Magpie/commits?author=mhtvsSFrpHdE" title="Documentation">📖</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/kangurek-kao"><img src="https://avatars.githubusercontent.com/u/116571935?v=4?s=100" width="100px;" alt="Krzysztof"/><br /><sub><b>Krzysztof</b></sub></a><br /><a href="#translation-kangurek-kao" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/Howard20181"><img src="https://avatars.githubusercontent.com/u/40033067?v=4?s=100" width="100px;" alt="Howard Wu"/><br /><sub><b>Howard Wu</b></sub></a><br /><a href="https://github.com/Blinue/Magpie/commits?author=Howard20181" title="Code">💻</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/arifpedia"><img src="https://avatars.githubusercontent.com/u/4081293?v=4?s=100" width="100px;" alt="Arif Budiman"/><br /><sub><b>Arif Budiman</b></sub></a><br /><a href="#translation-arifpedia" title="Translation">🌍</a></td>
    </tr>
    <tr>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/Androidlate"><img src="https://avatars.githubusercontent.com/u/194900061?v=4?s=100" width="100px;" alt="Raphael"/><br /><sub><b>Raphael</b></sub></a><br /><a href="#translation-Androidlate" title="Translation">🌍</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/rezorrand"><img src="https://avatars.githubusercontent.com/u/7170353?v=4?s=100" width="100px;" alt="Pate L"/><br /><sub><b>Pate L</b></sub></a><br /><a href="#translation-rezorrand" title="Translation">🌍</a></td>
    </tr>
  </tbody>
</table>

<!-- markdownlint-restore -->
<!-- prettier-ignore-end -->

<!-- ALL-CONTRIBUTORS-LIST:END -->

This project follows the [all-contributors](https://allcontributors.org/) specification. Contributions of any kind are welcome!

## License

This project is licensed under GPLv3.
