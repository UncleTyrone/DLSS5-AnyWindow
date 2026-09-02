#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <shellscalingapi.h>

#include "../Shared/DLSS5StatsShared.h"
#include "../Shared/DLSS5WindowTarget.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr wchar_t WINDOW_CLASS[] = L"DLSS5DoubleFloatingController";
constexpr wchar_t INSTANCE_MUTEX[] = L"Local\\DLSS5DoubleFloatingController.SingleInstance";
constexpr wchar_t ENGINE_NAME[] = L"DLSSNRWindowDouble.exe";
constexpr wchar_t SETTINGS_FILE[] = L"DLSS5-settings.ini";
constexpr DWORD ENGINE_EXIT_EXISTING_SCALING = 5;
constexpr DWORD ENGINE_EXIT_ALREADY_RUNNING = 6;
constexpr UINT_PTR TIMER_SELECT = 1;
constexpr UINT_PTR TIMER_POLL = 2;
constexpr ULONGLONG ENGINE_START_TIMEOUT_MS = 30000;
constexpr ULONGLONG ENGINE_STOP_TIMEOUT_MS = 5000;
constexpr int COLLAPSED_WIDTH = 320;
constexpr int COLLAPSED_HEIGHT = 148;
constexpr int EXPANDED_WIDTH = 360;
constexpr int EXPANDED_HEIGHT = 500;

enum class Phase {
	Idle,
	Selecting,
	Starting,
	Running,
	Stopping
};

struct FilterSettings {
	int style = 2;
	int intensity = 100;
	int localTone = 100;
	int localStructure = 100;
	bool autoMask = false;
	int passes = 2;
	bool antiFlicker = true;
};

struct ControllerData {
	HWND hwnd = nullptr;
	Phase phase = Phase::Idle;
	int countdown = 3;
	bool buttonHover = false;
	bool buttonPressed = false;
	bool closeHover = false;
	bool closePressed = false;
	bool settingsExpanded = false;
	bool settingsHover = false;
	bool settingsPressed = false;
	int activeSlider = -1;
	DWORD dpi = 96;
	DWORD childPid = 0;
	PROCESS_INFORMATION child{};
	HWND targetHwnd = nullptr;
	ULONGLONG engineStartedTick = 0;
	ULONGLONG engineStopTick = 0;
	HANDLE stopEvent = nullptr;
	std::wstring stopEventName;
	HANDLE statsMapping = nullptr;
	Magpie::DLSS5StatsShared* sharedStats = nullptr;
	std::wstring statsMapName;
	PDH_HQUERY gpuQuery = nullptr;
	PDH_HCOUNTER gpuCounter = nullptr;
	ULONGLONG lastStatsTick = 0;
	ULONGLONG lastProcessTime = 0;
	uint32_t filterFps = 0;
	double cpuUsage = -1.0;
	double gpuUsage = -1.0;
	std::wstring targetTitle;
	FilterSettings settings;
	HFONT titleFont = nullptr;
	HFONT statusFont = nullptr;
	HFONT buttonFont = nullptr;
};

int Dip(const ControllerData& data, int value) {
	return MulDiv(value, static_cast<int>(data.dpi), 96);
}

RECT ButtonRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 18), client.bottom - Dip(data, 58),
		client.right - Dip(data, 18), client.bottom - Dip(data, 16) };
}

RECT CloseRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { client.right - Dip(data, 38), Dip(data, 10),
		client.right - Dip(data, 10), Dip(data, 38) };
}

RECT SettingsButtonRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { client.right - Dip(data, 70), Dip(data, 10),
		client.right - Dip(data, 42), Dip(data, 38) };
}

RECT StyleRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 18), Dip(data, 108), client.right - Dip(data, 18), Dip(data, 140) };
}

RECT SliderHitRect(const ControllerData& data, int index) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	const int rowY = 151 + index * 46;
	return { Dip(data, 18), Dip(data, rowY + 16), client.right - Dip(data, 18), Dip(data, rowY + 42) };
}

RECT AutoMaskRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 18), Dip(data, 287), client.right - Dip(data, 18), Dip(data, 319) };
}

RECT AntiFlickerRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 18), Dip(data, 323), client.right - Dip(data, 18), Dip(data, 355) };
}

RECT PassesRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 190), Dip(data, 365), client.right - Dip(data, 18), Dip(data, 397) };
}

RECT ResetRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { client.right - Dip(data, 92), Dip(data, 80), client.right - Dip(data, 18), Dip(data, 106) };
}

bool PointIn(const RECT& rect, POINT point) {
	return PtInRect(&rect, point) != FALSE;
}

std::wstring ExeDirectory() {
	std::wstring path(32768, L'\0');
	const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	if (!length || length >= path.size()) return L".";
	path.resize(length);
	const size_t slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring SettingsPath() {
	return ExeDirectory() + L"\\" + SETTINGS_FILE;
}

void LoadSettings(ControllerData& data) {
	const std::wstring path = SettingsPath();
	data.settings.style = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(L"Filter", L"Style", 2, path.c_str())), 0, 2);
	data.settings.intensity = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(L"Filter", L"Intensity", 100, path.c_str())), 0, 100);
	data.settings.localTone = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(L"Filter", L"LocalTone", 100, path.c_str())), 0, 100);
	data.settings.localStructure = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(L"Filter", L"LocalStructure", 100, path.c_str())), 0, 100);
	data.settings.autoMask = GetPrivateProfileIntW(
		L"Filter", L"AutoMask", 0, path.c_str()) != 0;
	data.settings.antiFlicker = GetPrivateProfileIntW(
		L"Filter", L"AntiFlicker", 1, path.c_str()) != 0;
	data.settings.passes = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(L"Filter", L"Passes", 2, path.c_str())), 1, 4);
}

void SaveSetting(const std::wstring& path, const wchar_t* key, int value) {
	const std::wstring text = std::to_wstring(value);
	WritePrivateProfileStringW(L"Filter", key, text.c_str(), path.c_str());
}

void SaveSettings(const ControllerData& data) {
	const std::wstring path = SettingsPath();
	SaveSetting(path, L"Style", data.settings.style);
	SaveSetting(path, L"Intensity", data.settings.intensity);
	SaveSetting(path, L"LocalTone", data.settings.localTone);
	SaveSetting(path, L"LocalStructure", data.settings.localStructure);
	SaveSetting(path, L"AutoMask", data.settings.autoMask ? 1 : 0);
	SaveSetting(path, L"AntiFlicker", data.settings.antiFlicker ? 1 : 0);
	SaveSetting(path, L"Passes", data.settings.passes);
}

const wchar_t* StyleName(int style) {
	switch (style) {
	case 0: return L"默认";
	case 1: return L"自然";
	default: return L"电影";
	}
}

std::wstring SettingsSummary(const ControllerData& data) {
	return std::wstring(StyleName(data.settings.style)) + L" · 强度 " +
		std::to_wstring(data.settings.intensity) + L"% · " +
		std::to_wstring(data.settings.passes) + L" 次处理" +
		(data.settings.antiFlicker ? L" · 抗闪" : L"");
}

std::wstring PerformanceText(const ControllerData& data) {
	if (data.phase != Phase::Running && data.phase != Phase::Starting) {
		return L"FPS -- · 帧间隔 -- · GPU -- · CPU --";
	}
	const std::wstring fps = data.filterFps > 0 ? std::to_wstring(data.filterFps) : L"--";
	wchar_t frameTime[24]{};
	if (data.filterFps > 0) {
		swprintf_s(frameTime, L"%.1f ms", 1000.0 / data.filterFps);
	} else {
		wcscpy_s(frameTime, L"--");
	}
	auto percent = [](double value) {
		if (value < 0.0) return std::wstring(L"--");
		return std::to_wstring(static_cast<int>(std::lround(value))) + L"%";
	};
	return L"FPS " + fps + L" · 帧间隔 " + frameTime +
		L" · GPU " + percent(data.gpuUsage) + L" · CPU " + percent(data.cpuUsage);
}

uint64_t FileTimeValue(const FILETIME& value) {
	ULARGE_INTEGER converted{};
	converted.LowPart = value.dwLowDateTime;
	converted.HighPart = value.dwHighDateTime;
	return converted.QuadPart;
}

uint64_t ProcessTimeValue(HANDLE process) {
	FILETIME created{}, exited{}, kernel{}, user{};
	if (!process || !GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
	return FileTimeValue(kernel) + FileTimeValue(user);
}

void ClosePerformanceMonitor(ControllerData& data) {
	if (data.gpuQuery) PdhCloseQuery(data.gpuQuery);
	if (data.sharedStats) UnmapViewOfFile(data.sharedStats);
	if (data.statsMapping) CloseHandle(data.statsMapping);
	data.gpuQuery = nullptr;
	data.gpuCounter = nullptr;
	data.sharedStats = nullptr;
	data.statsMapping = nullptr;
	data.statsMapName.clear();
	data.lastStatsTick = 0;
	data.lastProcessTime = 0;
	data.filterFps = 0;
	data.cpuUsage = -1.0;
	data.gpuUsage = -1.0;
}

bool CreateStatsMapping(ControllerData& data) {
	data.statsMapName = L"Local\\DLSS5DoubleStats_" +
		std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
	data.statsMapping = CreateFileMappingW(
		INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
		static_cast<DWORD>(sizeof(Magpie::DLSS5StatsShared)), data.statsMapName.c_str());
	if (!data.statsMapping) return false;
	data.sharedStats = static_cast<Magpie::DLSS5StatsShared*>(MapViewOfFile(
		data.statsMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Magpie::DLSS5StatsShared)));
	if (!data.sharedStats) {
		CloseHandle(data.statsMapping);
		data.statsMapping = nullptr;
		data.statsMapName.clear();
		return false;
	}
	data.sharedStats->magic = Magpie::DLSS5_STATS_MAGIC;
	data.sharedStats->version = 2;
	InterlockedExchange(&data.sharedStats->fps, 0);
	InterlockedExchange(&data.sharedStats->featureState, Magpie::DLSS5FeaturePending);
	InterlockedExchange(&data.sharedStats->evaluateSuccessCount, 0);
	InterlockedExchange(&data.sharedStats->evaluateFailureCount, 0);
	InterlockedExchange(&data.sharedStats->configuredPasses, data.settings.passes);
	return true;
}

void StartPerformanceMonitor(ControllerData& data) {
	data.lastStatsTick = GetTickCount64();
	data.lastProcessTime = ProcessTimeValue(data.child.hProcess);
	if (PdhOpenQueryW(nullptr, 0, &data.gpuQuery) != ERROR_SUCCESS) return;
	if (PdhAddEnglishCounterW(data.gpuQuery,
		L"\\GPU Engine(*)\\Utilization Percentage", 0, &data.gpuCounter) != ERROR_SUCCESS) {
		PdhCloseQuery(data.gpuQuery);
		data.gpuQuery = nullptr;
		data.gpuCounter = nullptr;
		return;
	}
	PdhCollectQueryData(data.gpuQuery);
}

double ReadGpuUsage(const ControllerData& data) {
	if (!data.gpuCounter || !data.childPid) return -1.0;
	DWORD bufferSize = 0;
	DWORD itemCount = 0;
	PDH_STATUS status = PdhGetFormattedCounterArrayW(
		data.gpuCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, nullptr);
	if (status != PDH_MORE_DATA || bufferSize == 0) return -1.0;
	std::vector<unsigned char> buffer(bufferSize);
	auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
	status = PdhGetFormattedCounterArrayW(
		data.gpuCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items);
	if (status != ERROR_SUCCESS) return -1.0;
	const std::wstring pidToken = L"pid_" + std::to_wstring(data.childPid) + L"_";
	double busiestEngine = -1.0;
	for (DWORD index = 0; index < itemCount; ++index) {
		if (!items[index].szName || !wcsstr(items[index].szName, pidToken.c_str())) continue;
		if (items[index].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
			items[index].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
		busiestEngine = std::max(busiestEngine, items[index].FmtValue.doubleValue);
	}
	return busiestEngine < 0.0 ? -1.0 : std::clamp(busiestEngine, 0.0, 100.0);
}

void UpdatePerformanceMonitor(ControllerData& data) {
	const ULONGLONG now = GetTickCount64();
	const ULONGLONG elapsedMs = now - data.lastStatsTick;
	if (!data.child.hProcess || elapsedMs < 1000) return;
	if (data.sharedStats && data.sharedStats->magic == Magpie::DLSS5_STATS_MAGIC) {
		data.filterFps = static_cast<uint32_t>(std::max<LONG>(0,
			InterlockedCompareExchange(&data.sharedStats->fps, 0, 0)));
	}
	const uint64_t processTime = ProcessTimeValue(data.child.hProcess);
	if (processTime >= data.lastProcessTime && data.lastProcessTime > 0) {
		const DWORD processorCount = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
		data.cpuUsage = std::clamp(
			static_cast<double>(processTime - data.lastProcessTime) /
			(10000.0 * elapsedMs * processorCount) * 100.0, 0.0, 100.0);
	}
	data.lastProcessTime = processTime;
	data.lastStatsTick = now;
	if (data.gpuQuery && PdhCollectQueryData(data.gpuQuery) == ERROR_SUCCESS) {
		data.gpuUsage = ReadGpuUsage(data);
	}
	InvalidateRect(data.hwnd, nullptr, FALSE);
}

std::wstring WindowTitle(HWND hwnd) {
	const int length = GetWindowTextLengthW(hwnd);
	std::wstring title(static_cast<size_t>(std::max(length, 0)) + 1, L'\0');
	const int copied = GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()));
	title.resize(static_cast<size_t>(std::max(copied, 0)));
	return title.empty() ? L"未命名窗口" : title;
}

void DeleteFonts(ControllerData& data) {
	if (data.titleFont) DeleteObject(data.titleFont);
	if (data.statusFont) DeleteObject(data.statusFont);
	if (data.buttonFont) DeleteObject(data.buttonFont);
	data.titleFont = data.statusFont = data.buttonFont = nullptr;
}

HFONT MakeFont(const ControllerData& data, int sizeDip, int weight) {
	return CreateFontW(
		-Dip(data, sizeDip), 0, 0, 0, weight, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
}

void RecreateFonts(ControllerData& data) {
	DeleteFonts(data);
	data.titleFont = MakeFont(data, 16, FW_SEMIBOLD);
	data.statusFont = MakeFont(data, 12, FW_NORMAL);
	data.buttonFont = MakeFont(data, 13, FW_SEMIBOLD);
}

std::wstring StatusText(const ControllerData& data) {
	switch (data.phase) {
	case Phase::Selecting:
		return L"请在 " + std::to_wstring(data.countdown) + L" 秒内切换到目标窗口";
	case Phase::Starting:
		return data.targetTitle.empty() ? L"正在启动滤镜…" : L"正在连接 · " + data.targetTitle;
	case Phase::Running: {
		LONG featureState = Magpie::DLSS5FeaturePending;
		if (data.sharedStats && data.sharedStats->magic == Magpie::DLSS5_STATS_MAGIC) {
			featureState = InterlockedCompareExchange(&data.sharedStats->featureState, 0, 0);
		}
		if (featureState == Magpie::DLSS5FeatureFailed) {
			return L"DLSS5 初始化失败 · 当前是直通画面";
		}
		if (featureState == Magpie::DLSS5FeatureEvaluating) {
			return L"DLSS5 已生效（" + std::to_wstring(data.settings.passes) + L" 次/帧） · " +
				(data.targetTitle.empty() ? L"目标窗口" : data.targetTitle);
		}
		if (featureState == Magpie::DLSS5FeatureCreated) {
			return L"Feature 18 已创建 · 等待首帧验证";
		}
		return L"正在初始化 DLSS5 Feature 18…";
	}
	case Phase::Stopping:
		return L"正在关闭滤镜…";
	default:
		return data.targetTitle.empty()
			? SettingsSummary(data)
			: data.targetTitle;
	}
}

std::wstring ButtonText(const ControllerData& data) {
	switch (data.phase) {
	case Phase::Selecting: return L"取消选择";
	case Phase::Starting:
	case Phase::Running: return L"关闭滤镜";
	case Phase::Stopping: return L"正在关闭…";
	default: return L"选择窗口并开启";
	}
}

COLORREF IndicatorColor(const ControllerData& data) {
	switch (data.phase) {
	case Phase::Running:
		if (data.sharedStats && data.sharedStats->magic == Magpie::DLSS5_STATS_MAGIC) {
			const LONG featureState =
				InterlockedCompareExchange(&data.sharedStats->featureState, 0, 0);
			if (featureState == Magpie::DLSS5FeatureFailed) return RGB(240, 82, 82);
			if (featureState == Magpie::DLSS5FeatureEvaluating) return RGB(55, 214, 132);
		}
		return RGB(245, 174, 65);
	case Phase::Selecting:
	case Phase::Starting: return RGB(245, 174, 65);
	case Phase::Stopping: return RGB(240, 112, 93);
	default: return RGB(119, 126, 141);
	}
}

void FillRounded(HDC dc, const RECT& rect, COLORREF color, int radius) {
	HBRUSH brush = CreateSolidBrush(color);
	HPEN pen = CreatePen(PS_SOLID, 1, color);
	HGDIOBJ oldBrush = SelectObject(dc, brush);
	HGDIOBJ oldPen = SelectObject(dc, pen);
	RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
	SelectObject(dc, oldPen);
	SelectObject(dc, oldBrush);
	DeleteObject(pen);
	DeleteObject(brush);
}

void DrawSlider(
	HDC dc, const ControllerData& data, int index,
	const wchar_t* label, int value
) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	const int rowY = 151 + index * 46;
	SetTextColor(dc, RGB(210, 214, 223));
	SelectObject(dc, data.statusFont);
	RECT labelRect{ Dip(data, 19), Dip(data, rowY), client.right - Dip(data, 70), Dip(data, rowY + 22) };
	DrawTextW(dc, label, -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	const std::wstring valueText = std::to_wstring(value) + L"%";
	SetTextColor(dc, RGB(158, 166, 181));
	RECT valueRect{ client.right - Dip(data, 70), Dip(data, rowY),
		client.right - Dip(data, 19), Dip(data, rowY + 22) };
	DrawTextW(dc, valueText.c_str(), -1, &valueRect,
		DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	const int left = Dip(data, 20);
	const int right = client.right - Dip(data, 20);
	const int centerY = Dip(data, rowY + 30);
	RECT track{ left, centerY - Dip(data, 2), right, centerY + Dip(data, 2) };
	FillRounded(dc, track, RGB(54, 59, 70), Dip(data, 4));
	const int thumbX = left + MulDiv(right - left, value, 100);
	RECT filled{ left, centerY - Dip(data, 2), std::max(left + 1, thumbX), centerY + Dip(data, 2) };
	FillRounded(dc, filled, RGB(74, 137, 231), Dip(data, 4));
	const int thumbRadius = Dip(data, data.activeSlider == index ? 7 : 6);
	HBRUSH thumbBrush = CreateSolidBrush(RGB(235, 241, 252));
	HGDIOBJ oldBrush = SelectObject(dc, thumbBrush);
	HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
	Ellipse(dc, thumbX - thumbRadius, centerY - thumbRadius,
		thumbX + thumbRadius, centerY + thumbRadius);
	SelectObject(dc, oldPen);
	SelectObject(dc, oldBrush);
	DeleteObject(thumbBrush);
}

void DrawToggle(
	HDC dc, const ControllerData& data, const wchar_t* label,
	int rowY, bool enabled
) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	RECT labelRect{ Dip(data, 19), Dip(data, rowY),
		client.right - Dip(data, 70), Dip(data, rowY + 32) };
	SetTextColor(dc, RGB(210, 214, 223));
	SelectObject(dc, data.statusFont);
	DrawTextW(dc, label, -1, &labelRect,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	RECT toggle{ client.right - Dip(data, 58), Dip(data, rowY + 5),
		client.right - Dip(data, 20), Dip(data, rowY + 27) };
	FillRounded(dc, toggle,
		enabled ? RGB(62, 122, 218) : RGB(56, 61, 72), Dip(data, 18));
	const int knobRadius = Dip(data, 8);
	const int knobCenterX = enabled
		? toggle.right - Dip(data, 11) : toggle.left + Dip(data, 11);
	const int knobCenterY = (toggle.top + toggle.bottom) / 2;
	HBRUSH knobBrush = CreateSolidBrush(RGB(247, 248, 251));
	HGDIOBJ oldBrush = SelectObject(dc, knobBrush);
	HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
	Ellipse(dc, knobCenterX - knobRadius, knobCenterY - knobRadius,
		knobCenterX + knobRadius, knobCenterY + knobRadius);
	SelectObject(dc, oldPen);
	SelectObject(dc, oldBrush);
	DeleteObject(knobBrush);
}

void PaintSettingsPanel(HDC dc, ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	HPEN separator = CreatePen(PS_SOLID, 1, RGB(43, 47, 56));
	HGDIOBJ oldPen = SelectObject(dc, separator);
	MoveToEx(dc, Dip(data, 18), Dip(data, 76), nullptr);
	LineTo(dc, client.right - Dip(data, 18), Dip(data, 76));
	SelectObject(dc, oldPen);
	DeleteObject(separator);

	SelectObject(dc, data.statusFont);
	SetTextColor(dc, RGB(226, 229, 236));
	RECT heading{ Dip(data, 19), Dip(data, 82), Dip(data, 170), Dip(data, 106) };
	DrawTextW(dc, L"滤镜设置", -1, &heading, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	SetTextColor(dc, RGB(116, 165, 238));
	RECT reset = ResetRect(data);
	DrawTextW(dc, L"恢复默认", -1, &reset, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	RECT style = StyleRect(data);
	FillRounded(dc, style, RGB(31, 34, 41), Dip(data, 9));
	const wchar_t* labels[] = { L"默认", L"自然", L"电影" };
	const int segmentWidth = (style.right - style.left) / 3;
	for (int index = 0; index < 3; ++index) {
		RECT segment{ style.left + segmentWidth * index, style.top,
			index == 2 ? style.right : style.left + segmentWidth * (index + 1), style.bottom };
		if (data.settings.style == index) {
			RECT selected{ segment.left + Dip(data, 2), segment.top + Dip(data, 2),
				segment.right - Dip(data, 2), segment.bottom - Dip(data, 2) };
			FillRounded(dc, selected, RGB(58, 113, 201), Dip(data, 7));
		}
		SetTextColor(dc, data.settings.style == index ? RGB(255, 255, 255) : RGB(160, 167, 181));
		SelectObject(dc, data.statusFont);
		DrawTextW(dc, labels[index], -1, &segment,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}

	DrawSlider(dc, data, 0, L"效果强度", data.settings.intensity);
	DrawSlider(dc, data, 1, L"局部色调", data.settings.localTone);
	DrawSlider(dc, data, 2, L"局部结构", data.settings.localStructure);

	DrawToggle(dc, data, L"自动遮罩", 287, data.settings.autoMask);
	DrawToggle(dc, data, L"抗闪烁（推荐）", 323, data.settings.antiFlicker);

	RECT passesLabel{ Dip(data, 19), Dip(data, 365), Dip(data, 180), Dip(data, 397) };
	SetTextColor(dc, RGB(210, 214, 223));
	DrawTextW(dc, L"处理次数", -1, &passesLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	RECT passes = PassesRect(data);
	FillRounded(dc, passes, RGB(31, 34, 41), Dip(data, 9));
	const int passSegmentWidth = (passes.right - passes.left) / 4;
	for (int index = 0; index < 4; ++index) {
		RECT segment{ passes.left + passSegmentWidth * index, passes.top,
			index == 3 ? passes.right : passes.left + passSegmentWidth * (index + 1), passes.bottom };
		if (data.settings.passes == index + 1) {
			RECT selected{ segment.left + Dip(data, 2), segment.top + Dip(data, 2),
				segment.right - Dip(data, 2), segment.bottom - Dip(data, 2) };
			FillRounded(dc, selected, RGB(58, 113, 201), Dip(data, 7));
		}
		SetTextColor(dc, data.settings.passes == index + 1 ? RGB(255, 255, 255) : RGB(160, 167, 181));
		const std::wstring text = std::to_wstring(index + 1) + L"次";
		DrawTextW(dc, text.c_str(), -1, &segment,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}

	SetTextColor(dc, RGB(120, 127, 142));
	RECT hint{ Dip(data, 19), Dip(data, 403), client.right - Dip(data, 19), Dip(data, 426) };
	DrawTextW(dc, L"设置自动保存，将在下一次开启时使用", -1, &hint,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void PaintController(ControllerData& data) {
	PAINTSTRUCT ps{};
	HDC dc = BeginPaint(data.hwnd, &ps);
	RECT client{};
	GetClientRect(data.hwnd, &client);

	HDC memory = CreateCompatibleDC(dc);
	HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
	HGDIOBJ oldBitmap = SelectObject(memory, bitmap);

	HBRUSH background = CreateSolidBrush(RGB(20, 22, 27));
	FillRect(memory, &client, background);
	DeleteObject(background);

	HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(51, 55, 65));
	HGDIOBJ oldPen = SelectObject(memory, borderPen);
	HGDIOBJ oldBrush = SelectObject(memory, GetStockObject(NULL_BRUSH));
	RoundRect(memory, 0, 0, client.right, client.bottom, Dip(data, 14), Dip(data, 14));
	SelectObject(memory, oldBrush);
	SelectObject(memory, oldPen);
	DeleteObject(borderPen);

	const int dotSize = Dip(data, 8);
	RECT dot{ Dip(data, 19), Dip(data, 22), Dip(data, 19) + dotSize, Dip(data, 22) + dotSize };
	HBRUSH dotBrush = CreateSolidBrush(IndicatorColor(data));
	HGDIOBJ priorBrush = SelectObject(memory, dotBrush);
	Ellipse(memory, dot.left, dot.top, dot.right, dot.bottom);
	SelectObject(memory, priorBrush);
	DeleteObject(dotBrush);

	SetBkMode(memory, TRANSPARENT);
	SetTextColor(memory, RGB(241, 243, 247));
	SelectObject(memory, data.titleFont);
	RECT titleRect{ Dip(data, 35), Dip(data, 14), client.right - Dip(data, 78), Dip(data, 40) };
	DrawTextW(memory, L"DLSS5 多次滤镜", -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	SetTextColor(memory, RGB(157, 164, 179));
	SelectObject(memory, data.statusFont);
	RECT statusRect{ Dip(data, 19), Dip(data, 42), client.right - Dip(data, 18), Dip(data, 61) };
	const std::wstring status = StatusText(data);
	DrawTextW(memory, status.c_str(), -1, &statusRect,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

	SetTextColor(memory, data.phase == Phase::Running ? RGB(116, 165, 238) : RGB(126, 133, 148));
	RECT performanceRect{ Dip(data, 19), Dip(data, 60), client.right - Dip(data, 18), Dip(data, 77) };
	const std::wstring performance = PerformanceText(data);
	DrawTextW(memory, performance.c_str(), -1, &performanceRect,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

	if (data.settingsExpanded) {
		PaintSettingsPanel(memory, data);
	}

	RECT button = ButtonRect(data);
	COLORREF buttonColor;
	if (data.phase == Phase::Stopping) {
		buttonColor = RGB(52, 56, 65);
	} else if (data.phase == Phase::Running || data.phase == Phase::Starting) {
		buttonColor = data.buttonPressed ? RGB(175, 63, 67) :
			(data.buttonHover ? RGB(220, 76, 80) : RGB(205, 68, 73));
	} else {
		buttonColor = data.buttonPressed ? RGB(47, 105, 194) :
			(data.buttonHover ? RGB(73, 138, 235) : RGB(62, 122, 218));
	}
	HBRUSH buttonBrush = CreateSolidBrush(buttonColor);
	HPEN buttonPen = CreatePen(PS_SOLID, 1, buttonColor);
	oldPen = SelectObject(memory, buttonPen);
	priorBrush = SelectObject(memory, buttonBrush);
	RoundRect(memory, button.left, button.top, button.right, button.bottom,
		Dip(data, 10), Dip(data, 10));
	SelectObject(memory, priorBrush);
	SelectObject(memory, oldPen);
	DeleteObject(buttonBrush);
	DeleteObject(buttonPen);

	SetTextColor(memory, RGB(255, 255, 255));
	SelectObject(memory, data.buttonFont);
	const std::wstring buttonText = ButtonText(data);
	DrawTextW(memory, buttonText.c_str(), -1, &button,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	RECT settings = SettingsButtonRect(data);
	if (data.settingsHover || data.settingsPressed || data.settingsExpanded) {
		FillRounded(memory, settings,
			data.settingsPressed ? RGB(67, 70, 79) : RGB(50, 53, 62), Dip(data, 8));
	}
	HPEN settingsPen = CreatePen(PS_SOLID, std::max(1, Dip(data, 1)),
		data.settingsExpanded ? RGB(116, 165, 238) : RGB(178, 184, 196));
	oldPen = SelectObject(memory, settingsPen);
	const int lineLeft = settings.left + Dip(data, 7);
	const int lineRight = settings.right - Dip(data, 7);
	for (int index = 0; index < 3; ++index) {
		const int y = settings.top + Dip(data, 8 + index * 5);
		MoveToEx(memory, lineLeft, y, nullptr);
		LineTo(memory, lineRight, y);
		const int knobX = index == 1 ? lineLeft + Dip(data, 3) : lineRight - Dip(data, 4);
		HBRUSH knob = CreateSolidBrush(
			data.settingsExpanded ? RGB(116, 165, 238) : RGB(178, 184, 196));
		HGDIOBJ knobOldBrush = SelectObject(memory, knob);
		HGDIOBJ knobOldPen = SelectObject(memory, GetStockObject(NULL_PEN));
		Ellipse(memory, knobX - Dip(data, 2), y - Dip(data, 2),
			knobX + Dip(data, 2), y + Dip(data, 2));
		SelectObject(memory, knobOldPen);
		SelectObject(memory, knobOldBrush);
		DeleteObject(knob);
	}
	SelectObject(memory, oldPen);
	DeleteObject(settingsPen);

	RECT close = CloseRect(data);
	if (data.closeHover || data.closePressed) {
		HBRUSH closeBg = CreateSolidBrush(data.closePressed ? RGB(67, 70, 79) : RGB(50, 53, 62));
		HPEN closeBgPen = CreatePen(PS_SOLID, 1, data.closePressed ? RGB(67, 70, 79) : RGB(50, 53, 62));
		oldPen = SelectObject(memory, closeBgPen);
		priorBrush = SelectObject(memory, closeBg);
		RoundRect(memory, close.left, close.top, close.right, close.bottom,
			Dip(data, 8), Dip(data, 8));
		SelectObject(memory, priorBrush);
		SelectObject(memory, oldPen);
		DeleteObject(closeBg);
		DeleteObject(closeBgPen);
	}
	HPEN closePen = CreatePen(PS_SOLID, std::max(1, Dip(data, 1)), RGB(178, 184, 196));
	oldPen = SelectObject(memory, closePen);
	MoveToEx(memory, close.left + Dip(data, 9), close.top + Dip(data, 9), nullptr);
	LineTo(memory, close.right - Dip(data, 9), close.bottom - Dip(data, 9));
	MoveToEx(memory, close.right - Dip(data, 9), close.top + Dip(data, 9), nullptr);
	LineTo(memory, close.left + Dip(data, 9), close.bottom - Dip(data, 9));
	SelectObject(memory, oldPen);
	DeleteObject(closePen);

	BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
	SelectObject(memory, oldBitmap);
	DeleteObject(bitmap);
	DeleteDC(memory);
	EndPaint(data.hwnd, &ps);
}

void ResizeController(ControllerData& data, bool expanded) {
	RECT current{};
	GetWindowRect(data.hwnd, &current);
	const int width = Dip(data, expanded ? EXPANDED_WIDTH : COLLAPSED_WIDTH);
	const int height = Dip(data, expanded ? EXPANDED_HEIGHT : COLLAPSED_HEIGHT);
	int x = current.right - width;
	int y = current.top;
	HMONITOR monitor = MonitorFromWindow(data.hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO monitorInfo{ .cbSize = sizeof(MONITORINFO) };
	if (GetMonitorInfoW(monitor, &monitorInfo)) {
		const int workLeft = static_cast<int>(monitorInfo.rcWork.left);
		const int workTop = static_cast<int>(monitorInfo.rcWork.top);
		const int workRight = static_cast<int>(monitorInfo.rcWork.right);
		const int workBottom = static_cast<int>(monitorInfo.rcWork.bottom);
		x = std::clamp(x, workLeft, std::max(workLeft, workRight - width));
		y = std::clamp(y, workTop, std::max(workTop, workBottom - height));
	}
	SetWindowPos(data.hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
}

void ToggleSettings(ControllerData& data) {
	if (data.phase != Phase::Idle) {
		data.targetTitle = L"请先关闭滤镜，再调整参数";
		InvalidateRect(data.hwnd, nullptr, FALSE);
		return;
	}
	data.settingsExpanded = !data.settingsExpanded;
	ResizeController(data, data.settingsExpanded);
	InvalidateRect(data.hwnd, nullptr, FALSE);
}

void UpdateSliderFromPoint(ControllerData& data, int index, POINT point) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	const int left = Dip(data, 20);
	const int right = static_cast<int>(client.right) - Dip(data, 20);
	const int clampedX = std::clamp(static_cast<int>(point.x), left, right);
	int value = MulDiv(clampedX - left, 100, std::max(1, right - left));
	value = std::clamp(((value + 2) / 5) * 5, 0, 100);
	if (index == 0) data.settings.intensity = value;
	else if (index == 1) data.settings.localTone = value;
	else if (index == 2) data.settings.localStructure = value;
	InvalidateRect(data.hwnd, nullptr, FALSE);
}

bool HandleSettingsPress(ControllerData& data, POINT point) {
	if (!data.settingsExpanded || data.phase != Phase::Idle) return false;
	if (PointIn(ResetRect(data), point)) {
		data.settings = {};
		SaveSettings(data);
		InvalidateRect(data.hwnd, nullptr, FALSE);
		return true;
	}
	if (PointIn(StyleRect(data), point)) {
		const RECT rect = StyleRect(data);
		const int rectWidth = std::max(1, static_cast<int>(rect.right - rect.left));
		data.settings.style = std::clamp(
			static_cast<int>(point.x - rect.left) * 3 / rectWidth, 0, 2);
		SaveSettings(data);
		InvalidateRect(data.hwnd, nullptr, FALSE);
		return true;
	}
	for (int index = 0; index < 3; ++index) {
		if (PointIn(SliderHitRect(data, index), point)) {
			data.activeSlider = index;
			UpdateSliderFromPoint(data, index, point);
			SetCapture(data.hwnd);
			return true;
		}
	}
	if (PointIn(AutoMaskRect(data), point)) {
		data.settings.autoMask = !data.settings.autoMask;
		SaveSettings(data);
		InvalidateRect(data.hwnd, nullptr, FALSE);
		return true;
	}
	if (PointIn(AntiFlickerRect(data), point)) {
		data.settings.antiFlicker = !data.settings.antiFlicker;
		SaveSettings(data);
		InvalidateRect(data.hwnd, nullptr, FALSE);
		return true;
	}
	if (PointIn(PassesRect(data), point)) {
		const RECT rect = PassesRect(data);
		const int rectWidth = std::max(1, static_cast<int>(rect.right - rect.left));
		data.settings.passes = std::clamp(
			static_cast<int>(point.x - rect.left) * 4 / rectWidth + 1, 1, 4);
		SaveSettings(data);
		InvalidateRect(data.hwnd, nullptr, FALSE);
		return true;
	}
	return point.y >= Dip(data, 76) && point.y < ButtonRect(data).top;
}

void CloseChildHandles(ControllerData& data) {
	ClosePerformanceMonitor(data);
	if (data.child.hThread) CloseHandle(data.child.hThread);
	if (data.child.hProcess) CloseHandle(data.child.hProcess);
	if (data.stopEvent) CloseHandle(data.stopEvent);
	data.child = {};
	data.stopEvent = nullptr;
	data.childPid = 0;
	data.targetHwnd = nullptr;
	data.engineStartedTick = 0;
	data.engineStopTick = 0;
	data.stopEventName.clear();
}

void SetIdle(ControllerData& data, std::wstring status = {}) {
	CloseChildHandles(data);
	data.phase = Phase::Idle;
	data.targetTitle = std::move(status);
	InvalidateRect(data.hwnd, nullptr, FALSE);
}

void StopEngine(ControllerData& data) {
	if (!data.child.hProcess) {
		SetIdle(data);
		return;
	}
	if (data.stopEvent) SetEvent(data.stopEvent);
	data.engineStopTick = GetTickCount64();
	data.phase = Phase::Stopping;
	InvalidateRect(data.hwnd, nullptr, FALSE);
}

bool StartEngine(ControllerData& data, HWND target) {
	const Magpie::DLSS5WindowTarget::Info targetInfo = Magpie::DLSS5WindowTarget::Inspect(target);
	std::wstring rejectionReason;
	if (Magpie::DLSS5WindowTarget::IsForbidden(
		targetInfo, GetCurrentProcessId(), rejectionReason)) {
		data.phase = Phase::Idle;
		data.targetTitle = rejectionReason + L"，请重新选择";
		InvalidateRect(data.hwnd, nullptr, FALSE);
		return false;
	}

	data.stopEventName = L"Local\\DLSS5DoubleStop_" +
		std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
	data.stopEvent = CreateEventW(nullptr, TRUE, FALSE, data.stopEventName.c_str());
	if (!data.stopEvent) {
		MessageBoxW(data.hwnd, L"无法创建关闭信号。", L"DLSS5 多次滤镜", MB_OK | MB_ICONERROR);
		SetIdle(data);
		return false;
	}
	CreateStatsMapping(data);

	const std::wstring directory = ExeDirectory();
	const std::wstring engine = directory + L"\\" + ENGINE_NAME;
	std::wstring command = L"\"" + engine + L"\" --hwnd " +
		std::to_wstring(reinterpret_cast<uintptr_t>(target)) + L" --stop-event \"" +
		data.stopEventName + L"\" --style " + std::to_wstring(data.settings.style) +
		L" --intensity " + std::to_wstring(data.settings.intensity / 100.0f) +
		L" --local-tone " + std::to_wstring(data.settings.localTone / 100.0f) +
		L" --local-structure " + std::to_wstring(data.settings.localStructure / 100.0f) +
		L" --auto-mask " + std::to_wstring(data.settings.autoMask ? 1 : 0) +
		L" --anti-flicker " + std::to_wstring(data.settings.antiFlicker ? 1 : 0) +
		L" --passes " + std::to_wstring(data.settings.passes) +
		L" --controller-pid " + std::to_wstring(GetCurrentProcessId());
	if (data.sharedStats) {
		command += L" --stats-map \"" + data.statsMapName + L"\"";
	}
	std::vector<wchar_t> commandBuffer(command.begin(), command.end());
	commandBuffer.push_back(L'\0');

	STARTUPINFOW startup{ .cb = sizeof(STARTUPINFOW) };
	PROCESS_INFORMATION process{};
	if (!CreateProcessW(
		engine.c_str(), commandBuffer.data(), nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process)) {
		MessageBoxW(data.hwnd,
			L"无法启动多次渲染核心，请确认 DLSSNRWindowDouble.exe 位于同一目录。",
			L"DLSS5 多次滤镜", MB_OK | MB_ICONERROR);
		SetIdle(data);
		return false;
	}

	data.child = process;
	data.childPid = process.dwProcessId;
	data.targetHwnd = target;
	data.engineStartedTick = GetTickCount64();
	data.engineStopTick = 0;
	StartPerformanceMonitor(data);
	data.targetTitle = targetInfo.title.empty() ? WindowTitle(target) : targetInfo.title;
	data.phase = Phase::Starting;
	InvalidateRect(data.hwnd, nullptr, FALSE);
	return true;
}

void BeginSelection(ControllerData& data) {
	if (data.settingsExpanded) {
		data.settingsExpanded = false;
		ResizeController(data, false);
	}
	data.phase = Phase::Selecting;
	data.countdown = 3;
	data.targetTitle.clear();
	SetTimer(data.hwnd, TIMER_SELECT, 1000, nullptr);
	InvalidateRect(data.hwnd, nullptr, FALSE);
}

void CancelSelection(ControllerData& data) {
	KillTimer(data.hwnd, TIMER_SELECT);
	data.phase = Phase::Idle;
	InvalidateRect(data.hwnd, nullptr, FALSE);
}

void ActivateButton(ControllerData& data) {
	switch (data.phase) {
	case Phase::Idle: BeginSelection(data); break;
	case Phase::Selecting: CancelSelection(data); break;
	case Phase::Starting:
	case Phase::Running: StopEngine(data); break;
	case Phase::Stopping: break;
	}
}

void PollEngine(ControllerData& data) {
	if (!data.child.hProcess) return;
	if (WaitForSingleObject(data.child.hProcess, 0) == WAIT_OBJECT_0) {
		DWORD exitCode = 0;
		GetExitCodeProcess(data.child.hProcess, &exitCode);
		if (data.phase == Phase::Stopping) {
			SetIdle(data, L"滤镜已关闭");
		} else if (exitCode == ENGINE_EXIT_ALREADY_RUNNING ||
			exitCode == ENGINE_EXIT_EXISTING_SCALING) {
			SetIdle(data, L"已有一个滤镜在运行，请先关闭旧实例");
		} else if (exitCode == 2) {
			SetIdle(data, L"目标窗口无效或选中了 DLSS5 自身，请重新选择");
		} else if (exitCode != 0) {
			SetIdle(data, L"滤镜核心退出（错误码 " + std::to_wstring(exitCode) + L"），请查看日志");
		} else {
			SetIdle(data, L"目标窗口已关闭，滤镜已停止");
		}
		return;
	}
	const ULONGLONG now = GetTickCount64();
	if ((data.phase == Phase::Starting || data.phase == Phase::Running) &&
		data.targetHwnd && !IsWindow(data.targetHwnd)) {
		if (data.stopEvent) SetEvent(data.stopEvent);
		data.engineStopTick = now;
		data.phase = Phase::Stopping;
		data.targetTitle = L"目标窗口已关闭，正在清理滤镜";
		InvalidateRect(data.hwnd, nullptr, FALSE);
	}
	if (data.phase == Phase::Starting && data.engineStartedTick &&
		now - data.engineStartedTick >= ENGINE_START_TIMEOUT_MS) {
		if (data.stopEvent) SetEvent(data.stopEvent);
		data.engineStopTick = now;
		data.phase = Phase::Stopping;
		data.targetTitle = L"启动超时，正在清理滤镜核心";
		InvalidateRect(data.hwnd, nullptr, FALSE);
	}
	if (data.phase == Phase::Stopping && data.engineStopTick &&
		now - data.engineStopTick >= ENGINE_STOP_TIMEOUT_MS) {
		TerminateProcess(data.child.hProcess, 0);
		WaitForSingleObject(data.child.hProcess, 1000);
		SetIdle(data, L"滤镜核心未响应，已强制清理");
		return;
	}
	if (data.phase == Phase::Starting) {
		HWND scaling = FindWindowW(Magpie::DLSS5WindowTarget::SCALING_WINDOW_CLASS, nullptr);
		DWORD scalingPid = 0;
		if (scaling) GetWindowThreadProcessId(scaling, &scalingPid);
		if (scalingPid == data.childPid) {
			data.phase = Phase::Running;
			InvalidateRect(data.hwnd, nullptr, FALSE);
		}
	}
	UpdatePerformanceMonitor(data);
	if (data.phase == Phase::Running || data.phase == Phase::Starting) {
		SetWindowPos(data.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
}

void ShutdownController(ControllerData& data) {
	KillTimer(data.hwnd, TIMER_SELECT);
	KillTimer(data.hwnd, TIMER_POLL);
	if (data.child.hProcess) {
		if (data.stopEvent) SetEvent(data.stopEvent);
		if (WaitForSingleObject(data.child.hProcess, 3000) != WAIT_OBJECT_0) {
			TerminateProcess(data.child.hProcess, 0);
			WaitForSingleObject(data.child.hProcess, 1000);
		}
	}
	CloseChildHandles(data);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
	ControllerData* data = reinterpret_cast<ControllerData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
		data = static_cast<ControllerData*>(create->lpCreateParams);
		data->hwnd = hwnd;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
	}
	if (!data) return DefWindowProcW(hwnd, message, wParam, lParam);

	switch (message) {
	case WM_CREATE:
		data->dpi = GetDpiForWindow(hwnd);
		LoadSettings(*data);
		RecreateFonts(*data);
		SetTimer(hwnd, TIMER_POLL, 100, nullptr);
		return 0;
	case WM_DPICHANGED:
		data->dpi = HIWORD(wParam);
		RecreateFonts(*data);
		if (const RECT* suggested = reinterpret_cast<const RECT*>(lParam)) {
			SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
				suggested->right - suggested->left, suggested->bottom - suggested->top,
				SWP_NOZORDER | SWP_NOACTIVATE);
		}
		ResizeController(*data, data->settingsExpanded);
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_PAINT:
		PaintController(*data);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_NCHITTEST:
	{
		POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		ScreenToClient(hwnd, &point);
		if (PointIn(ButtonRect(*data), point) || PointIn(CloseRect(*data), point) ||
			PointIn(SettingsButtonRect(*data), point) ||
			(data->settingsExpanded && point.y >= Dip(*data, 76))) return HTCLIENT;
		return HTCAPTION;
	}
	case WM_MOUSEMOVE:
	{
		POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (data->activeSlider >= 0) {
			UpdateSliderFromPoint(*data, data->activeSlider, point);
			return 0;
		}
		const bool buttonHover = PointIn(ButtonRect(*data), point);
		const bool closeHover = PointIn(CloseRect(*data), point);
		const bool settingsHover = PointIn(SettingsButtonRect(*data), point);
		if (buttonHover != data->buttonHover || closeHover != data->closeHover ||
			settingsHover != data->settingsHover) {
			data->buttonHover = buttonHover;
			data->closeHover = closeHover;
			data->settingsHover = settingsHover;
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		TRACKMOUSEEVENT tracking{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
		TrackMouseEvent(&tracking);
		return 0;
	}
	case WM_MOUSELEAVE:
		data->buttonHover = data->closeHover = data->settingsHover = false;
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_LBUTTONDOWN:
	{
		POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		data->buttonPressed = PointIn(ButtonRect(*data), point);
		data->closePressed = PointIn(CloseRect(*data), point);
		data->settingsPressed = PointIn(SettingsButtonRect(*data), point);
		if (data->buttonPressed || data->closePressed || data->settingsPressed) {
			SetCapture(hwnd);
		} else {
			HandleSettingsPress(*data, point);
		}
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}
	case WM_LBUTTONUP:
	{
		POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (data->activeSlider >= 0) {
			UpdateSliderFromPoint(*data, data->activeSlider, point);
			data->activeSlider = -1;
			SaveSettings(*data);
			ReleaseCapture();
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
		const bool activateButton = data->buttonPressed && PointIn(ButtonRect(*data), point);
		const bool activateClose = data->closePressed && PointIn(CloseRect(*data), point);
		const bool activateSettings = data->settingsPressed && PointIn(SettingsButtonRect(*data), point);
		data->buttonPressed = data->closePressed = data->settingsPressed = false;
		ReleaseCapture();
		InvalidateRect(hwnd, nullptr, FALSE);
		if (activateClose) DestroyWindow(hwnd);
		else if (activateButton) ActivateButton(*data);
		else if (activateSettings) ToggleSettings(*data);
		return 0;
	}
	case WM_KEYDOWN:
		if (wParam == VK_SPACE || wParam == VK_RETURN) ActivateButton(*data);
		else if (wParam == VK_ESCAPE) {
			if (data->phase == Phase::Selecting) CancelSelection(*data);
			else if (data->phase == Phase::Running || data->phase == Phase::Starting) StopEngine(*data);
		}
		return 0;
	case WM_TIMER:
		if (wParam == TIMER_SELECT) {
			if (data->countdown > 1) {
				--data->countdown;
				InvalidateRect(hwnd, nullptr, FALSE);
			} else {
				KillTimer(hwnd, TIMER_SELECT);
				HWND target = GetForegroundWindow();
				StartEngine(*data, target);
			}
		} else if (wParam == TIMER_POLL) {
			PollEngine(*data);
		}
		return 0;
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		ShutdownController(*data);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
	HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, INSTANCE_MUTEX);
	if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
		if (HWND existing = FindWindowW(WINDOW_CLASS, nullptr)) {
			ShowWindow(existing, SW_SHOWNORMAL);
			SetForegroundWindow(existing);
		}
		CloseHandle(instanceMutex);
		return 0;
	}

	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = instance;
	windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
	windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	windowClass.hbrBackground = nullptr;
	windowClass.lpszMenuName = nullptr;
	windowClass.lpszClassName = WINDOW_CLASS;
	windowClass.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(101));
	RegisterClassExW(&windowClass);

	POINT cursor{};
	GetCursorPos(&cursor);
	HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
	MONITORINFO monitorInfo{ .cbSize = sizeof(MONITORINFO) };
	GetMonitorInfoW(monitor, &monitorInfo);
	UINT dpi = GetDpiForSystem();
	UINT dpiY = dpi;
	GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &dpiY);
	const int width = MulDiv(COLLAPSED_WIDTH, dpi, 96);
	const int height = MulDiv(COLLAPSED_HEIGHT, dpi, 96);
	const int x = monitorInfo.rcWork.right - width - MulDiv(18, dpi, 96);
	const int y = monitorInfo.rcWork.top + MulDiv(18, dpi, 96);

	ControllerData data;
	HWND hwnd = CreateWindowExW(
		WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
		WINDOW_CLASS, L"DLSS5 多次滤镜", WS_POPUP,
		x, y, width, height, nullptr, nullptr, instance, &data);
	if (!hwnd) {
		if (instanceMutex) CloseHandle(instanceMutex);
		return 1;
	}

	const int cornerPreference = 2; // DWMWCP_ROUND
	DwmSetWindowAttribute(hwnd, 33, &cornerPreference, sizeof(cornerPreference));
	const BOOL darkMode = TRUE;
	DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode));
	ShowWindow(hwnd, SW_SHOWNOACTIVATE);
	UpdateWindow(hwnd);

	MSG message{};
	while (GetMessageW(&message, nullptr, 0, 0) > 0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}

	DeleteFonts(data);
	if (instanceMutex) CloseHandle(instanceMutex);
	return static_cast<int>(message.wParam);
}
