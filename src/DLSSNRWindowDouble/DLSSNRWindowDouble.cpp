#include "../Magpie.Core/pch.h"
#include "../Magpie.Core/DLSSNRFilter.h"
#include "../Magpie.Core/include/ScalingOptions.h"
#include "../Magpie.Core/include/ScalingRuntime.h"
#include "../Shared/DLSS5StatsShared.h"
#include "../Shared/DLSS5WindowTarget.h"
#include "../Shared/Logger.h"
#include "../Shared/StrHelper.h"

#include <shellapi.h>
#include <conio.h>
#include <atomic>
#include <iostream>
#include <optional>

using namespace Magpie;
using namespace std::chrono_literals;

namespace {

constexpr int STOP_HOTKEY_ID = 0xD155;
constexpr int STOP_ESCAPE_HOTKEY_ID = 0xD156;
constexpr UINT WM_STOP_REQUEST = WM_APP + 0x155;
constexpr wchar_t ENGINE_INSTANCE_MUTEX[] = L"Local\\DLSS5AnyWindow.Engine.SingleInstance";
constexpr int EXIT_INVALID_TARGET = 2;
constexpr int EXIT_NO_NVIDIA = 3;
constexpr int EXIT_EXISTING_SCALING = 5;
constexpr int EXIT_ALREADY_RUNNING = 6;

std::atomic_bool g_stopRequested = false;
DWORD g_mainThreadId = 0;
HHOOK g_keyboardHook = nullptr;

LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam) {
	if (code == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
		const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
		if (event->vkCode == VK_ESCAPE) {
			g_stopRequested.store(true, std::memory_order_relaxed);
			PostThreadMessageW(g_mainThreadId, WM_STOP_REQUEST, 0, 0);
			return 1;
		}
	}
	return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

struct Arguments {
	HWND hwnd = nullptr;
	int delaySeconds = 4;
	int autoStopSeconds = 0;
	std::wstring stopEventName;
	std::wstring statsMapName;
	DWORD controllerPid = 0;
	// 0 experimental DLSSNR, 1 stable single-frame CAS.
	int backend = 0;
	int style = 2;
	float intensity = 1.0f;
	float localToneStrength = 1.0f;
	float localStructureStrength = 1.0f;
	bool useAutoMask = false;
	int guidanceMode = 0;
	int depthInferenceInterval = 4;
	int passes = 1;
	int historyMode = 0;
};

std::filesystem::path ExeDirectory() {
	std::wstring path(32768, L'\0');
	const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	if (!length || length >= path.size()) {
		return std::filesystem::current_path();
	}
	path.resize(length);
	return std::filesystem::path(path).parent_path();
}

std::optional<unsigned long long> ParseUnsigned(std::wstring_view value) {
	if (value.empty()) return std::nullopt;
	wchar_t* end = nullptr;
	const std::wstring copy(value);
	const unsigned long long parsed = std::wcstoull(copy.c_str(), &end, 0);
	if (end == copy.c_str() || *end != L'\0') return std::nullopt;
	return parsed;
}

std::optional<float> ParseFloat(std::wstring_view value) {
	if (value.empty()) return std::nullopt;
	wchar_t* end = nullptr;
	const std::wstring copy(value);
	const float parsed = std::wcstof(copy.c_str(), &end);
	if (end == copy.c_str() || *end != L'\0' || !std::isfinite(parsed)) return std::nullopt;
	return parsed;
}

Arguments ParseArguments() {
	Arguments result;
	int argc = 0;
	wil::unique_hlocal_ptr<wchar_t*> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
	if (!argv) return result;

	for (int i = 1; i < argc; ++i) {
		const std::wstring_view arg = argv.get()[i];
		if (arg == L"--hwnd" && i + 1 < argc) {
			if (auto value = ParseUnsigned(argv.get()[++i])) {
				result.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(*value));
			}
		} else if (arg == L"--delay" && i + 1 < argc) {
			if (auto value = ParseUnsigned(argv.get()[++i])) {
				result.delaySeconds = static_cast<int>(std::min<unsigned long long>(*value, 30));
			}
		} else if (arg == L"--seconds" && i + 1 < argc) {
			if (auto value = ParseUnsigned(argv.get()[++i])) {
				result.autoStopSeconds = static_cast<int>(std::min<unsigned long long>(*value, 3600));
			}
		} else if (arg == L"--stop-event" && i + 1 < argc) {
			result.stopEventName = argv.get()[++i];
		} else if (arg == L"--stats-map" && i + 1 < argc) {
			result.statsMapName = argv.get()[++i];
		} else if (arg == L"--controller-pid" && i + 1 < argc) {
			if (auto value = ParseUnsigned(argv.get()[++i])) {
				result.controllerPid = static_cast<DWORD>(
					std::min<unsigned long long>(*value, MAXDWORD));
			}
		} else if (arg == L"--backend" && i + 1 < argc) {
			if (auto value = ParseUnsigned(argv.get()[++i])) {
				result.backend = static_cast<int>(std::min<unsigned long long>(*value, 1));
			}
		} else if (arg == L"--style" && i + 1 < argc) {
			if (auto value = ParseUnsigned(argv.get()[++i])) {
				result.style = static_cast<int>(std::min<unsigned long long>(*value, 2));
			}
		} else if (arg == L"--intensity" && i + 1 < argc) {
			if (auto value = ParseFloat(argv.get()[++i])) {
				result.intensity = std::clamp(*value, 0.0f, 1.0f);
			}
		} else if (arg == L"--local-tone" && i + 1 < argc) {
			if (auto value = ParseFloat(argv.get()[++i])) {
				result.localToneStrength = std::clamp(*value, 0.0f, 1.0f);
			}
		} else if (arg == L"--local-structure" && i + 1 < argc) {
			if (auto value = ParseFloat(argv.get()[++i])) {
				result.localStructureStrength = std::clamp(*value, 0.0f, 1.0f);
			}
		} else if (arg == L"--auto-mask" && i + 1 < argc) {
			if (auto value = ParseUnsigned(argv.get()[++i])) {
				result.useAutoMask = *value != 0;
			}
		} else if (arg == L"--guidance" && i + 1 < argc) {
			if (auto value = ParseUnsigned(argv.get()[++i])) {
				result.guidanceMode = static_cast<int>(std::min<unsigned long long>(*value, 3));
			}
		} else if (arg == L"--depth-interval" && i + 1 < argc) {
			if (auto value = ParseUnsigned(argv.get()[++i])) {
				result.depthInferenceInterval = static_cast<int>(
					std::clamp<unsigned long long>(*value, 1, 8));
			}
		} else if (arg == L"--passes" && i + 1 < argc) {
			if (auto value = ParseUnsigned(argv.get()[++i])) {
				result.passes = static_cast<int>(
					std::clamp<unsigned long long>(*value, 1, 4));
			}
		} else if (arg == L"--history-mode" && i + 1 < argc) {
			if (auto value = ParseUnsigned(argv.get()[++i])) {
				result.historyMode = static_cast<int>(
					std::min<unsigned long long>(*value, 2));
			}
		} else if (arg == L"--anti-flicker" && i + 1 < argc) {
			if (auto value = ParseUnsigned(argv.get()[++i])) {
				// Backward compatibility: the old toggle selected either an
				// every-frame reset or fully continuous history.
				result.historyMode = *value != 0 ? 1 : 2;
			}
		}
	}
	return result;
}

std::wstring WindowTitle(HWND hwnd) {
	const int length = GetWindowTextLengthW(hwnd);
	std::wstring title(static_cast<size_t>(std::max(length, 0)) + 1, L'\0');
	const int copied = GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()));
	title.resize(static_cast<size_t>(std::max(copied, 0)));
	return title.empty() ? L"（无标题窗口）" : title;
}

void ShowToast(HWND, std::wstring_view) noexcept {
	Logger::Get().Info("Core requested a toast notification");
}

void ShowError(HWND, ScalingError error) noexcept {
	Logger::Get().Error(fmt::format("缩放失败，错误码 {}", static_cast<int>(error)));
	const wchar_t* detail = L"启动滤镜失败，请查看 DLSS5-Double.log。";
	if (error == ScalingError::BannedInWindowedMode || error == ScalingError::Maximized) {
		detail = L"目标窗口仍处于最大化或全屏状态，请先还原窗口再试。";
	} else if (error == ScalingError::InvalidSourceWindow) {
		detail = L"没有找到可用的目标窗口，请重新运行并在倒计时内切换过去。";
	} else if (error == ScalingError::CaptureFailed) {
		detail = L"无法捕获这个窗口，请确认它没有最小化，并尝试以相同权限运行。";
	}
	MessageBoxW(nullptr, detail, L"DLSS5 多次滤镜", MB_OK | MB_ICONERROR | MB_TOPMOST);
}

void SaveOptions(const ScalingOptions&, HWND) noexcept {
}

std::optional<GraphicsCardId> FindNvidiaAdapter() noexcept {
	winrt::com_ptr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) return std::nullopt;

	for (UINT index = 0;; ++index) {
		winrt::com_ptr<IDXGIAdapter1> adapter;
		const HRESULT result = factory->EnumAdapters1(index, adapter.put());
		if (result == DXGI_ERROR_NOT_FOUND) break;
		if (FAILED(result)) continue;

		DXGI_ADAPTER_DESC1 description{};
		if (FAILED(adapter->GetDesc1(&description)) ||
			description.VendorId != 0x10de ||
			(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
			continue;
		}

		Logger::Get().Info(fmt::format(
			"Auto-selected NVIDIA adapter index={} vendor={:#x} device={:#x}",
			index, description.VendorId, description.DeviceId));
		return GraphicsCardId{
			.idx = static_cast<int>(index),
			.vendorId = description.VendorId,
			.deviceId = description.DeviceId
		};
	}

	Logger::Get().Warn("No NVIDIA adapter was detected; DLSS5 cannot initialize");
	return std::nullopt;
}

ScalingOptions MakeOptions(const Arguments& args) {
	ScalingOptions options;
	EffectOption effect;
	if (args.backend == 1) {
		effect.name = "CAS\\CAS";
		effect.parameters = { { "sharpness", args.intensity } };
		Logger::Get().Info(fmt::format(
			"Renderer backend=stable-cas sharpness={:.2f} temporalHistory=false", args.intensity));
	} else {
		if (const auto nvidiaAdapter = FindNvidiaAdapter()) {
			options.graphicsCardId = *nvidiaAdapter;
		}
		effect.name = "DLSSNR\\DLSSNR_AI_Filter";
		effect.parameters = {
			{ "style", static_cast<float>(args.style) },
			{ "intensity", args.intensity },
			{ "localToneStrength", args.localToneStrength },
			{ "localStructureStrength", args.localStructureStrength },
			{ "useAutoMask", args.useAutoMask ? 1.0f : 0.0f },
			{ "guidanceMode", static_cast<float>(args.guidanceMode) },
			{ "depthInferenceInterval", static_cast<float>(args.depthInferenceInterval) },
			{ "passes", static_cast<float>(args.passes) },
			{ "historyMode", static_cast<float>(args.historyMode) }
		};
		Logger::Get().Info("Renderer backend=experimental-dlssnr");
	}
	options.effects.push_back(std::move(effect));
	options.captureMethod = CaptureMethod::GraphicsCapture;
	options.duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Dynamic;
	// “任意窗口滤镜”应覆盖在原窗口位置，而不是把画面放大到全屏。
	options.IsWindowedMode(true);
	options.initialWindowedScaleFactor = 1.0f;
	options.exactWindowedSize = true;
	options.IsAllowScalingMaximized(true);
	options.IsCaptureTitleBar(false);
	options.screenshotsDir = ExeDirectory() / L"screenshots";
	std::error_code error;
	std::filesystem::create_directories(options.screenshotsDir, error);
	if (error || !std::filesystem::is_directory(options.screenshotsDir, error)) {
		Logger::Get().Error(StrHelper::Concat(
			"Cannot create screenshots directory: ",
			StrHelper::UTF16ToUTF8(options.screenshotsDir.native()),
			"; error=", std::to_string(error.value())));
		std::error_code tempError;
		const std::filesystem::path fallback =
			std::filesystem::temp_directory_path(tempError) / L"DLSS5-AnyWindow-screenshots";
		if (!tempError) {
			std::filesystem::create_directories(fallback, tempError);
			if (!tempError) options.screenshotsDir = fallback;
		}
	}
	Logger::Get().Info(StrHelper::Concat(
		"Screenshots directory: ", StrHelper::UTF16ToUTF8(options.screenshotsDir.native())));
	options.showToast = &ShowToast;
	options.showError = &ShowError;
	options.save = &SaveOptions;
	return options;
}

bool PumpMessagesUntil(
	std::chrono::steady_clock::time_point deadline,
	ScalingRuntime& runtime,
	HANDLE stopEvent = nullptr
) {
	while (std::chrono::steady_clock::now() < deadline) {
		if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
			runtime.Stop();
			return false;
		}
		// 即使低级键盘钩子安装失败，Esc 按键状态轮询仍可兜底。
		const SHORT escapeState = GetAsyncKeyState(VK_ESCAPE);
		const bool polledStop = (escapeState & 1) != 0;
		if (g_stopRequested.exchange(false, std::memory_order_relaxed) || polledStop) {
			runtime.Stop();
			return false;
		}
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if ((msg.message == WM_HOTKEY &&
				(msg.wParam == STOP_HOTKEY_ID || msg.wParam == STOP_ESCAPE_HOTKEY_ID)) ||
				msg.message == WM_STOP_REQUEST) {
				runtime.Stop();
				return false;
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		std::this_thread::sleep_for(10ms);
	}
	return true;
}

} // namespace

int wmain() {
	wil::unique_handle instanceMutex(CreateMutexW(nullptr, FALSE, ENGINE_INSTANCE_MUTEX));
	if (!instanceMutex) return EXIT_ALREADY_RUNNING;
	if (GetLastError() == ERROR_ALREADY_EXISTS) return EXIT_ALREADY_RUNNING;

	SetConsoleOutputCP(CP_UTF8);
	const std::filesystem::path exeDir = ExeDirectory();
	SetCurrentDirectoryW(exeDir.c_str());
	Logger::Get().Initialize(spdlog::level::info, L"DLSS5-Double.log", 2 * 1024 * 1024, 2);
	Logger::Get().Info(fmt::format(
		"DLSS5 Double Window launcher started pid={}", GetCurrentProcessId()));

	const Arguments args = ParseArguments();
	wil::unique_handle stopEvent;
	if (!args.stopEventName.empty()) {
		stopEvent.reset(OpenEventW(SYNCHRONIZE, FALSE, args.stopEventName.c_str()));
		if (!stopEvent) {
			Logger::Get().Win32Warn("OpenEvent stop signal failed");
		}
	}
	wil::unique_handle statsMapping;
	DLSS5StatsShared* sharedStats = nullptr;
	if (!args.statsMapName.empty()) {
		statsMapping.reset(OpenFileMappingW(
			FILE_MAP_WRITE, FALSE, args.statsMapName.c_str()));
		if (statsMapping) {
			sharedStats = static_cast<DLSS5StatsShared*>(MapViewOfFile(
				statsMapping.get(), FILE_MAP_WRITE, 0, 0, sizeof(DLSS5StatsShared)));
		} else {
			Logger::Get().Win32Warn("OpenFileMapping stats failed");
		}
	}
	if (sharedStats && sharedStats->magic == DLSS5_STATS_MAGIC) {
		sharedStats->version = 3;
		InterlockedExchange(&sharedStats->featureState, DLSS5FeaturePending);
		InterlockedExchange(&sharedStats->evaluateSuccessCount, 0);
		InterlockedExchange(&sharedStats->evaluateFailureCount, 0);
		InterlockedExchange(&sharedStats->configuredPasses, args.backend == 1 ? 1 : args.passes);
		InterlockedExchange(&sharedStats->runtimeKind, DLSS5RuntimeUnknown);
		InterlockedExchange(&sharedStats->cudaComputeMajor, 0);
		InterlockedExchange(&sharedStats->cudaComputeMinor, 0);
	}
	auto unmapStats = wil::scope_exit([&]() {
		if (sharedStats) UnmapViewOfFile(sharedStats);
	});
	HWND target = args.hwnd;
	if (!target) {
		std::wcout << L"DLSS5 多次叠加试验\n\n";
		std::wcout << L"请在倒计时结束前切换到要处理的窗口。\n";
		for (int remaining = args.delaySeconds; remaining > 0; --remaining) {
			std::wcout << L"  " << remaining << L" 秒...\n";
			std::this_thread::sleep_for(1s);
		}
		target = GetForegroundWindow();
	}

	const DLSS5WindowTarget::Info targetInfo = DLSS5WindowTarget::Inspect(target);
	Logger::Get().Info(fmt::format(
		"Target HWND={} PID={} class=\"{}\" title=\"{}\" exe=\"{}\" "
		"rect={},{},{},{} visible={}",
		reinterpret_cast<uintptr_t>(targetInfo.hwnd), targetInfo.processId,
		StrHelper::UTF16ToUTF8(targetInfo.className),
		StrHelper::UTF16ToUTF8(targetInfo.title),
		StrHelper::UTF16ToUTF8(targetInfo.processPath),
		targetInfo.windowRect.left, targetInfo.windowRect.top,
		targetInfo.windowRect.right, targetInfo.windowRect.bottom,
		targetInfo.isVisible));
	std::wstring rejectionReason;
	if (target == GetConsoleWindow() ||
		DLSS5WindowTarget::IsForbidden(targetInfo, GetCurrentProcessId(), rejectionReason)) {
		std::wcerr << L"没有找到有效目标窗口。请重新运行并及时切换窗口。\n";
		Logger::Get().Error(StrHelper::Concat(
			"Rejected target window: ", StrHelper::UTF16ToUTF8(rejectionReason)));
		return EXIT_INVALID_TARGET;
	}

	const std::wstring title = targetInfo.title.empty() ? WindowTitle(target) : targetInfo.title;
	std::wcout << L"目标窗口：" << title << L"\n";
	if (args.backend == 1) {
		std::wcout << L"正在启动稳定高速 CAS；按 Esc 关闭。\n";
	} else {
		std::wcout << L"正在启动 " << args.passes << L" 次 DLSS5；按 Esc 关闭。\n";
	}

	if (HWND existing = FindWindowW(DLSS5WindowTarget::SCALING_WINDOW_CLASS, nullptr)) {
		DWORD existingPid = 0;
		GetWindowThreadProcessId(existing, &existingPid);
		Logger::Get().Warn(fmt::format(
			"Existing scaling window detected HWND={} PID={}",
			reinterpret_cast<uintptr_t>(existing), existingPid));
		return EXIT_EXISTING_SCALING;
	}

	wil::unique_process_handle controllerProcess;
	if (args.controllerPid && args.controllerPid != GetCurrentProcessId()) {
		controllerProcess.reset(OpenProcess(SYNCHRONIZE, FALSE, args.controllerPid));
		if (!controllerProcess) {
			Logger::Get().Win32Warn("OpenProcess controller failed");
		} else {
			Logger::Get().Info(fmt::format("Controller PID={}", args.controllerPid));
		}
	}

	winrt::init_apartment(winrt::apartment_type::single_threaded);
	ScalingOptions options = MakeOptions(args);
	if (args.backend == 0 && options.graphicsCardId.idx < 0) {
		MessageBoxW(nullptr,
			L"未检测到 NVIDIA 显卡。此便携版需要兼容的 NVIDIA RTX 显卡和已安装的官方驱动。",
			L"DLSS5 多次滤镜", MB_OK | MB_ICONERROR | MB_TOPMOST);
		return EXIT_NO_NVIDIA;
	}

	if (!args.controllerPid &&
		!RegisterHotKey(nullptr, STOP_HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F10)) {
		Logger::Get().Win32Warn("RegisterHotKey Ctrl+Alt+F10 failed");
		std::wcout << L"提示：备用热键注册失败，仍可按 Esc 关闭。\n";
	}
	if (!RegisterHotKey(nullptr, STOP_ESCAPE_HOTKEY_ID, MOD_NOREPEAT, VK_ESCAPE)) {
		Logger::Get().Win32Warn("RegisterHotKey Escape failed");
	}
	g_mainThreadId = GetCurrentThreadId();
	// 清除启动前残留的 Esc 按键状态，避免刚启动就退出。
	GetAsyncKeyState(VK_ESCAPE);
	g_keyboardHook = SetWindowsHookExW(
		WH_KEYBOARD_LL, &KeyboardHookProc, GetModuleHandleW(nullptr), 0);
	if (!g_keyboardHook) {
		Logger::Get().Win32Warn("SetWindowsHookExW keyboard hook failed");
	}
	{
		DLSSNRFilter::ResetRuntimeTelemetry();
		ScalingRuntime runtime;
		runtime.Start(target, std::move(options), true);

		const auto startedAt = std::chrono::steady_clock::now();
		bool everScaling = false;
		ScalingState previousState = ScalingState::Idle;
		while (true) {
			if (!PumpMessagesUntil(
				std::chrono::steady_clock::now() + 100ms, runtime, stopEvent.get())) break;
			if (sharedStats && sharedStats->magic == DLSS5_STATS_MAGIC) {
				InterlockedExchange(&sharedStats->fps, static_cast<LONG>(runtime.FPS()));
				if (args.backend == 1) {
					const ScalingState state = runtime.State();
					InterlockedExchange(
						&sharedStats->featureState,
						state == ScalingState::Scaling
							? DLSS5FeatureEvaluating
							: (state == ScalingState::Waiting
								? DLSS5FeatureFailed : DLSS5FeaturePending));
				} else {
					const DLSSNRTelemetry telemetry = DLSSNRFilter::RuntimeTelemetry();
					InterlockedExchange(
						&sharedStats->featureState, static_cast<LONG>(telemetry.state));
					InterlockedExchange(
						&sharedStats->evaluateSuccessCount,
						static_cast<LONG>(std::min<uint32_t>(telemetry.evaluateSuccessCount, LONG_MAX)));
					InterlockedExchange(
						&sharedStats->evaluateFailureCount,
						static_cast<LONG>(std::min<uint32_t>(telemetry.evaluateFailureCount, LONG_MAX)));
					InterlockedExchange(
						&sharedStats->runtimeKind,
						static_cast<LONG>(telemetry.runtimeKind));
					InterlockedExchange(
						&sharedStats->cudaComputeMajor, telemetry.cudaComputeMajor);
					InterlockedExchange(
						&sharedStats->cudaComputeMinor, telemetry.cudaComputeMinor);
				}
			}
			if (!IsWindow(target)) {
				Logger::Get().Info("Target window was destroyed; stopping renderer");
				runtime.Stop();
				break;
			}
			if (controllerProcess &&
				WaitForSingleObject(controllerProcess.get(), 0) == WAIT_OBJECT_0) {
				Logger::Get().Warn("Controller process exited; stopping orphan renderer");
				runtime.Stop();
				break;
			}
			const ScalingState state = runtime.State();
			if (state != previousState) {
				Logger::Get().Info(fmt::format(
					"Scaling state changed {} -> {}",
					static_cast<int>(previousState), static_cast<int>(state)));
				previousState = state;
			}
			everScaling = everScaling || state == ScalingState::Scaling;
			if (everScaling && state == ScalingState::Idle) {
				std::wcout << L"目标窗口已关闭或 DLSS5 已停止。\n";
				break;
			}
			if (args.autoStopSeconds > 0 &&
				std::chrono::steady_clock::now() - startedAt >= std::chrono::seconds(args.autoStopSeconds)) {
				runtime.Stop();
				PumpMessagesUntil(std::chrono::steady_clock::now() + 500ms, runtime, nullptr);
				break;
			}
		}
	}

	UnregisterHotKey(nullptr, STOP_HOTKEY_ID);
	UnregisterHotKey(nullptr, STOP_ESCAPE_HOTKEY_ID);
	if (g_keyboardHook) {
		UnhookWindowsHookEx(g_keyboardHook);
		g_keyboardHook = nullptr;
	}
	Logger::Get().Info("DLSS5 Double Window launcher stopped");
	Logger::Get().Flush();
	return 0;
}
