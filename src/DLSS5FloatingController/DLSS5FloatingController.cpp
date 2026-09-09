#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <bcrypt.h>
#include <dwmapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <winhttp.h>

#include "../Shared/DLSS5StatsShared.h"
#include "../Shared/DLSS5WindowTarget.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t WINDOW_CLASS[] = L"DLSS5DoubleFloatingController";
constexpr wchar_t INSTANCE_MUTEX[] = L"Local\\DLSS5DoubleFloatingController.SingleInstance";
constexpr wchar_t INSTANCE_MUTEX_QA[] = L"Local\\DLSS5DoubleFloatingController.PixelQaInstance";
constexpr wchar_t ENGINE_NAME[] = L"DLSSNRWindowDouble.exe";
constexpr wchar_t SETTINGS_FILE[] = L"DLSS5-settings.ini";
constexpr wchar_t APP_VERSION[] = L"1.9.2";
constexpr int APP_VERSION_MAJOR = 1;
constexpr int APP_VERSION_MINOR = 9;
constexpr int APP_VERSION_PATCH = 2;
constexpr wchar_t GITHUB_REPOSITORY_URL[] =
	L"https://github.com/Shangyuwang11/DLSS5-AnyWindow";
constexpr wchar_t GITHUB_RELEASES_API[] =
	L"https://api.github.com/repos/Shangyuwang11/DLSS5-AnyWindow/releases?per_page=20";
constexpr wchar_t COMPONENT_UPDATE_BASE_URL[] =
	L"https://github.com/Shangyuwang11/DLSS5-AnyWindow/releases/download/"
	L"v1.9.2-dlss5-anywindow/";
constexpr wchar_t RENDERER_UPDATE_ASSET[] = L"DLSSNRWindowDouble-1.9.2.exe";
constexpr wchar_t EFFECT_UPDATE_ASSET[] = L"DLSSNR_AI_Filter-1.9.2.hlsl";
constexpr wchar_t RENDERER_SHA256[] =
	L"1c9de16142c177ec05e19edfb69a1292cb4a1797458656acba0027e8d0762674";
constexpr wchar_t EFFECT_SHA256[] =
	L"e425bcbbaa92eeacbb0b70cc78d5ba9a9e2b737f5986ea071eef394c618e4159";
constexpr int TOGGLE_HOTKEY_ID = 0xD157;
constexpr int VISIBILITY_HOTKEY_ID = 0xD158;
constexpr UINT WM_UPDATE_WORKER_RESULT = WM_APP + 0x157;
constexpr DWORD ENGINE_EXIT_EXISTING_SCALING = 5;
constexpr DWORD ENGINE_EXIT_ALREADY_RUNNING = 6;
constexpr UINT_PTR TIMER_SELECT = 1;
constexpr UINT_PTR TIMER_POLL = 2;
constexpr ULONGLONG ENGINE_START_TIMEOUT_MS = 30000;
constexpr ULONGLONG ENGINE_STOP_TIMEOUT_MS = 5000;
constexpr int COLLAPSED_WIDTH = 320;
constexpr int COLLAPSED_HEIGHT = 148;
constexpr int EXPANDED_WIDTH = 360;
constexpr int EXPANDED_HEIGHT = 756;

struct UiPalette {
	COLORREF panel;
	COLORREF panelDark;
	COLORREF track;
	COLORREF borderLight;
	COLORREF borderDark;
	COLORREF text;
	COLORREF textMuted;
	COLORREF textDisabled;
	COLORREF accent;
	COLORREF accentHover;
	COLORREF accentPressed;
	COLORREF accentDark;
	COLORREF danger;
	COLORREF dangerHover;
	COLORREF dangerPressed;
};

constexpr UiPalette UI_PALETTES[] = {
	// Wood brown (default).
	{ RGB(70, 47, 37), RGB(55, 37, 31), RGB(83, 56, 43),
	  RGB(132, 91, 62), RGB(31, 21, 18), RGB(244, 224, 181),
	  RGB(194, 158, 115), RGB(119, 89, 68), RGB(208, 155, 68),
	  RGB(229, 181, 87), RGB(169, 116, 47), RGB(111, 72, 31),
	  RGB(158, 79, 55), RGB(185, 98, 66), RGB(122, 59, 44) },
	// Obsidian black.
	{ RGB(28, 30, 34), RGB(20, 22, 26), RGB(47, 51, 58),
	  RGB(78, 84, 94), RGB(12, 14, 17), RGB(241, 242, 244),
	  RGB(171, 176, 184), RGB(96, 101, 110), RGB(210, 161, 72),
	  RGB(232, 186, 96), RGB(172, 122, 48), RGB(103, 72, 28),
	  RGB(175, 74, 72), RGB(205, 93, 89), RGB(132, 53, 52) },
	// Deep blue.
	{ RGB(24, 38, 57), RGB(18, 29, 45), RGB(35, 54, 78),
	  RGB(67, 103, 142), RGB(11, 20, 31), RGB(227, 239, 250),
	  RGB(146, 180, 212), RGB(81, 105, 130), RGB(72, 158, 220),
	  RGB(99, 182, 239), RGB(45, 120, 177), RGB(28, 80, 121),
	  RGB(184, 79, 76), RGB(211, 100, 94), RGB(139, 57, 56) }
};
constexpr int UI_THEME_COUNT = static_cast<int>(std::size(UI_PALETTES));

COLORREF UI_PANEL = UI_PALETTES[0].panel;
COLORREF UI_PANEL_DARK = UI_PALETTES[0].panelDark;
COLORREF UI_TRACK = UI_PALETTES[0].track;
COLORREF UI_BORDER_LIGHT = UI_PALETTES[0].borderLight;
COLORREF UI_BORDER_DARK = UI_PALETTES[0].borderDark;
COLORREF UI_TEXT = UI_PALETTES[0].text;
COLORREF UI_TEXT_MUTED = UI_PALETTES[0].textMuted;
COLORREF UI_TEXT_DISABLED = UI_PALETTES[0].textDisabled;
COLORREF UI_ACCENT = UI_PALETTES[0].accent;
COLORREF UI_ACCENT_HOVER = UI_PALETTES[0].accentHover;
COLORREF UI_ACCENT_PRESSED = UI_PALETTES[0].accentPressed;
COLORREF UI_ACCENT_DARK = UI_PALETTES[0].accentDark;
COLORREF UI_DANGER = UI_PALETTES[0].danger;
COLORREF UI_DANGER_HOVER = UI_PALETTES[0].dangerHover;
COLORREF UI_DANGER_PRESSED = UI_PALETTES[0].dangerPressed;

// Existing drawing code uses these semantic roles. They are references to the
// active palette, not fixed wood-theme constants.
COLORREF& WOOD_PANEL = UI_PANEL;
COLORREF& WOOD_PANEL_DARK = UI_PANEL_DARK;
COLORREF& WOOD_TRACK = UI_TRACK;
COLORREF& WOOD_BORDER_LIGHT = UI_BORDER_LIGHT;
COLORREF& WOOD_BORDER_DARK = UI_BORDER_DARK;
COLORREF& PARCHMENT = UI_TEXT;
COLORREF& PARCHMENT_MUTED = UI_TEXT_MUTED;
COLORREF& PARCHMENT_DISABLED = UI_TEXT_DISABLED;
COLORREF& GOLD = UI_ACCENT;
COLORREF& GOLD_HOVER = UI_ACCENT_HOVER;
COLORREF& GOLD_PRESSED = UI_ACCENT_PRESSED;
COLORREF& GOLD_DARK = UI_ACCENT_DARK;
COLORREF& CLAY = UI_DANGER;
COLORREF& CLAY_HOVER = UI_DANGER_HOVER;
COLORREF& CLAY_PRESSED = UI_DANGER_PRESSED;

enum class Phase {
	Idle,
	Selecting,
	Starting,
	Running,
	Stopping
};

enum class HotkeyCapture {
	None,
	FilterToggle,
	WindowVisibility
};

enum class UpdateState {
	Idle,
	Checking,
	Current,
	Available,
	Downloading,
	Failed
};

struct VersionNumber {
	int major = 0;
	int minor = 0;
	int patch = 0;
};

struct UpdateWorkerResult {
	bool download = false;
	bool success = false;
	bool updateAvailable = false;
	std::wstring version;
	std::wstring releaseUrl;
	std::wstring assetUrl;
	std::wstring hashUrl;
	std::wstring downloadedPath;
	std::wstring message;
};

struct FilterSettings {
	// 0 experimental DLSSNR, 1 stable single-frame CAS.
	int backend = 0;
	// Internal DLSSNR extent. The overlay and final output keep their size.
	int processingResolutionPercent = 100;
	int style = 0;
	int intensity = 100;
	int localTone = 100;
	int localStructure = 100;
	bool autoMask = true;
	int passes = 1;
	// 0 adaptive, 1 reset every source frame, 2 continuous history.
	int historyMode = 2;
	// 0 auto/available, 1 flat/zero, 2 motion only, 3 depth only.
	int guidanceMode = 3;
	int depthInferenceInterval = 1;
	UINT hotkeyModifiers = MOD_CONTROL | MOD_ALT;
	UINT hotkeyVirtualKey = VK_F10;
};

struct UiSettings {
	// 0 wood brown, 1 obsidian black, 2 deep blue.
	int theme = 0;
	// Layered-window alpha percentage. Keep a readable lower bound.
	int opacity = 100;
	UINT visibilityHotkeyModifiers = MOD_CONTROL | MOD_ALT;
	UINT visibilityHotkeyVirtualKey = VK_F9;
	bool autoCheckUpdates = true;
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
	int settingsPage = 0;
	bool settingsHover = false;
	bool settingsPressed = false;
	HotkeyCapture hotkeyCapture = HotkeyCapture::None;
	bool hotkeyRegistered = false;
	bool visibilityHotkeyRegistered = false;
	UINT capturedHotkeyModifiers = 0;
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
	bool motionGuidanceAvailable = false;
	bool motionGuidanceHardware = false;
	bool motionGuidanceSoftware = false;
	bool depthGuidanceAvailable = false;
	UpdateState updateState = UpdateState::Idle;
	std::wstring updateVersion;
	std::wstring updateReleaseUrl;
	std::wstring updateAssetUrl;
	std::wstring updateHashUrl;
	std::wstring updateMessage;
	bool automaticUpdateCheck = false;
	FilterSettings settings;
	UiSettings uiSettings;
	HFONT titleFont = nullptr;
	HFONT statusFont = nullptr;
	HFONT buttonFont = nullptr;
};

int Dip(const ControllerData& data, int value) {
	return MulDiv(value, static_cast<int>(data.dpi), 96);
}

void ApplyUiPalette(int theme) {
	const UiPalette& palette = UI_PALETTES[std::clamp(theme, 0, UI_THEME_COUNT - 1)];
	UI_PANEL = palette.panel;
	UI_PANEL_DARK = palette.panelDark;
	UI_TRACK = palette.track;
	UI_BORDER_LIGHT = palette.borderLight;
	UI_BORDER_DARK = palette.borderDark;
	UI_TEXT = palette.text;
	UI_TEXT_MUTED = palette.textMuted;
	UI_TEXT_DISABLED = palette.textDisabled;
	UI_ACCENT = palette.accent;
	UI_ACCENT_HOVER = palette.accentHover;
	UI_ACCENT_PRESSED = palette.accentPressed;
	UI_ACCENT_DARK = palette.accentDark;
	UI_DANGER = palette.danger;
	UI_DANGER_HOVER = palette.dangerHover;
	UI_DANGER_PRESSED = palette.dangerPressed;
}

void ApplyWindowOpacity(const ControllerData& data) {
	const BYTE alpha = static_cast<BYTE>(MulDiv(
		std::clamp(data.uiSettings.opacity, 40, 100), 255, 100));
	SetLayeredWindowAttributes(data.hwnd, 0, alpha, LWA_ALPHA);
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

RECT HistoryModeRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 126), Dip(data, 323), client.right - Dip(data, 18), Dip(data, 355) };
}

RECT PassesRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 190), Dip(data, 365), client.right - Dip(data, 18), Dip(data, 397) };
}

RECT GuidanceRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 18), Dip(data, 427), client.right - Dip(data, 18), Dip(data, 459) };
}

RECT DepthIntervalRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 190), Dip(data, 469), client.right - Dip(data, 18), Dip(data, 501) };
}

RECT BackendRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 130), Dip(data, 579), client.right - Dip(data, 18), Dip(data, 611) };
}

RECT ProcessingResolutionRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 124), Dip(data, 621), client.right - Dip(data, 18), Dip(data, 653) };
}

RECT SettingsTabRect(const ControllerData& data, int page) {
	const int left = 18 + page * 72;
	return { Dip(data, left), Dip(data, 80), Dip(data, left + 68), Dip(data, 106) };
}

RECT UiThemeRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 18), Dip(data, 139), client.right - Dip(data, 18), Dip(data, 175) };
}

RECT OpacitySliderHitRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 18), Dip(data, 205), client.right - Dip(data, 18), Dip(data, 240) };
}

RECT HotkeyRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 130), Dip(data, 283), client.right - Dip(data, 18), Dip(data, 317) };
}

RECT VisibilityHotkeyRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 130), Dip(data, 365), client.right - Dip(data, 18), Dip(data, 399) };
}

RECT AboutAutoUpdateRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 18), Dip(data, 155), client.right - Dip(data, 18), Dip(data, 187) };
}

RECT AboutUpdateActionRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 18), Dip(data, 257), client.right - Dip(data, 18), Dip(data, 301) };
}

RECT AboutSourceLinkRect(const ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	return { Dip(data, 18), Dip(data, 349), client.right - Dip(data, 18), Dip(data, 387) };
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

bool FileExists(const std::filesystem::path& path) {
	std::error_code error;
	return std::filesystem::is_regular_file(path, error);
}

bool BuildManifestFeatureEnabled(std::string_view feature) {
	std::ifstream stream(
		std::filesystem::path(ExeDirectory()) / L"build-manifest.json",
		std::ios::binary);
	if (!stream) return false;
	const std::string contents{
		std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
	const std::string key = "\"" + std::string(feature) + "\"";
	const size_t keyPosition = contents.find(key);
	if (keyPosition == std::string::npos) return false;
	const size_t colon = contents.find(':', keyPosition + key.size());
	if (colon == std::string::npos) return false;
	size_t value = contents.find_first_not_of(" \t\r\n", colon + 1);
	if (value != std::string::npos && contents[value] == '"') ++value;
	return value != std::string::npos && contents.compare(value, 4, "true") == 0;
}

void DetectGuidanceCapabilities(ControllerData& data) {
	const std::filesystem::path directory(ExeDirectory());
	const bool motionBuilt = BuildManifestFeatureEnabled("EnableNvidiaOpticalFlow");
	const bool softwareMotionBuilt =
		BuildManifestFeatureEnabled("EnableSoftwareOpticalFlow");
	const bool depthBuilt = BuildManifestFeatureEnabled("EnableDepthAnythingV2");

	HMODULE opticalFlow = LoadLibraryExW(
		L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	data.motionGuidanceHardware = motionBuilt && opticalFlow != nullptr;
	data.motionGuidanceSoftware = softwareMotionBuilt;
	data.motionGuidanceAvailable =
		data.motionGuidanceHardware || data.motionGuidanceSoftware;
	if (opticalFlow) FreeLibrary(opticalFlow);

	const std::filesystem::path guidance = directory / L"FrameGuidance";
	data.depthGuidanceAvailable = depthBuilt &&
		FileExists(guidance / L"DepthAnythingV2" / L"model_fp16.onnx") &&
		FileExists(guidance / L"DirectML" / L"onnxruntime.dll") &&
		FileExists(guidance / L"DirectML" / L"DirectML.dll");
}

bool GuidanceModeAvailable(const ControllerData& data, int mode) {
	switch (mode) {
	case 0: return data.motionGuidanceAvailable || data.depthGuidanceAvailable;
	case 1: return true;
	case 2: return data.motionGuidanceAvailable;
	case 3: return data.depthGuidanceAvailable;
	default: return false;
	}
}

void LoadSettings(ControllerData& data) {
	const std::wstring path = SettingsPath();
	data.settings.backend = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(L"Filter", L"Backend", 0, path.c_str())), 0, 1);
	data.settings.processingResolutionPercent = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(
			L"Filter", L"ProcessingResolutionPercent", 100, path.c_str())), 25, 100);
	data.settings.style = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(L"Filter", L"Style", 0, path.c_str())), 0, 2);
	data.settings.intensity = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(L"Filter", L"Intensity", 100, path.c_str())), 0, 100);
	data.settings.localTone = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(L"Filter", L"LocalTone", 100, path.c_str())), 0, 100);
	data.settings.localStructure = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(L"Filter", L"LocalStructure", 100, path.c_str())), 0, 100);
	data.settings.autoMask = GetPrivateProfileIntW(
		L"Filter", L"AutoMask", 1, path.c_str()) != 0;
	const int savedHistoryMode = static_cast<int>(GetPrivateProfileIntW(
		L"Filter", L"HistoryMode", -1, path.c_str()));
	if (savedHistoryMode >= 0 && savedHistoryMode <= 2) {
		data.settings.historyMode = savedHistoryMode;
	} else {
		const int legacyAntiFlicker = static_cast<int>(GetPrivateProfileIntW(
			L"Filter", L"AntiFlicker", -1, path.c_str()));
		data.settings.historyMode = legacyAntiFlicker < 0 ? 2 :
			(legacyAntiFlicker != 0 ? 1 : 2);
	}
	data.settings.passes = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(L"Filter", L"Passes", 1, path.c_str())), 1, 4);
	data.settings.guidanceMode = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(L"Filter", L"GuidanceMode", 3, path.c_str())), 0, 3);
	data.settings.depthInferenceInterval = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(L"Filter", L"DepthInferenceInterval", 1, path.c_str())), 1, 8);
	data.settings.hotkeyModifiers = static_cast<UINT>(GetPrivateProfileIntW(
		L"Filter", L"HotkeyModifiers", MOD_CONTROL | MOD_ALT, path.c_str())) &
		(MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
	data.settings.hotkeyVirtualKey = static_cast<UINT>(std::clamp<int>(
		GetPrivateProfileIntW(L"Filter", L"HotkeyVirtualKey", VK_F10, path.c_str()), 0, 0xff));
	data.uiSettings.theme = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(L"UI", L"Theme", 0, path.c_str())),
		0, UI_THEME_COUNT - 1);
	data.uiSettings.opacity = std::clamp(
		static_cast<int>(GetPrivateProfileIntW(L"UI", L"Opacity", 100, path.c_str())),
		40, 100);
	data.uiSettings.visibilityHotkeyModifiers = static_cast<UINT>(GetPrivateProfileIntW(
		L"UI", L"VisibilityHotkeyModifiers", MOD_CONTROL | MOD_ALT, path.c_str())) &
		(MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
	data.uiSettings.visibilityHotkeyVirtualKey = static_cast<UINT>(std::clamp<int>(
		GetPrivateProfileIntW(L"UI", L"VisibilityHotkeyVirtualKey", VK_F9, path.c_str()),
		0, 0xff));
	data.uiSettings.autoCheckUpdates = GetPrivateProfileIntW(
		L"UI", L"AutoCheckUpdates", 1, path.c_str()) != 0;
	if (!GuidanceModeAvailable(data, data.settings.guidanceMode)) {
		data.settings.guidanceMode = 1;
	}
	ApplyUiPalette(data.uiSettings.theme);
}

void SaveSetting(
	const std::wstring& path, const wchar_t* section,
	const wchar_t* key, int value
) {
	const std::wstring text = std::to_wstring(value);
	WritePrivateProfileStringW(section, key, text.c_str(), path.c_str());
}

void SaveFilterSetting(const std::wstring& path, const wchar_t* key, int value) {
	SaveSetting(path, L"Filter", key, value);
}

void SaveSettings(const ControllerData& data) {
	const std::wstring path = SettingsPath();
	SaveFilterSetting(path, L"Backend", data.settings.backend);
	SaveFilterSetting(path, L"ProcessingResolutionPercent",
		data.settings.processingResolutionPercent);
	SaveFilterSetting(path, L"Style", data.settings.style);
	SaveFilterSetting(path, L"Intensity", data.settings.intensity);
	SaveFilterSetting(path, L"LocalTone", data.settings.localTone);
	SaveFilterSetting(path, L"LocalStructure", data.settings.localStructure);
	SaveFilterSetting(path, L"AutoMask", data.settings.autoMask ? 1 : 0);
	SaveFilterSetting(path, L"HistoryMode", data.settings.historyMode);
	SaveFilterSetting(path, L"AntiFlicker", data.settings.historyMode == 1 ? 1 : 0);
	SaveFilterSetting(path, L"Passes", data.settings.passes);
	SaveFilterSetting(path, L"GuidanceMode", data.settings.guidanceMode);
	SaveFilterSetting(path, L"DepthInferenceInterval", data.settings.depthInferenceInterval);
	SaveFilterSetting(path, L"HotkeyModifiers", static_cast<int>(data.settings.hotkeyModifiers));
	SaveFilterSetting(path, L"HotkeyVirtualKey", static_cast<int>(data.settings.hotkeyVirtualKey));
	SaveSetting(path, L"UI", L"Theme", data.uiSettings.theme);
	SaveSetting(path, L"UI", L"Opacity", data.uiSettings.opacity);
	SaveSetting(path, L"UI", L"VisibilityHotkeyModifiers",
		static_cast<int>(data.uiSettings.visibilityHotkeyModifiers));
	SaveSetting(path, L"UI", L"VisibilityHotkeyVirtualKey",
		static_cast<int>(data.uiSettings.visibilityHotkeyVirtualKey));
	SaveSetting(path, L"UI", L"AutoCheckUpdates",
		data.uiSettings.autoCheckUpdates ? 1 : 0);
}

std::wstring Utf8ToWide(std::string_view text) {
	if (text.empty()) return {};
	const int length = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (length <= 0) return {};
	std::wstring result(static_cast<size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
		static_cast<int>(text.size()), result.data(), length);
	return result;
}

std::string WideToUtf8(std::wstring_view text) {
	if (text.empty()) return {};
	const int length = WideCharToMultiByte(
		CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (length <= 0) return {};
	std::string result(static_cast<size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
		result.data(), length, nullptr, nullptr);
	return result;
}

bool HttpGet(
	std::wstring_view url, size_t maximumBytes,
	std::vector<uint8_t>& response, std::wstring& error
) {
	URL_COMPONENTS components{ sizeof(components) };
	components.dwSchemeLength = static_cast<DWORD>(-1);
	components.dwHostNameLength = static_cast<DWORD>(-1);
	components.dwUrlPathLength = static_cast<DWORD>(-1);
	components.dwExtraInfoLength = static_cast<DWORD>(-1);
	if (!WinHttpCrackUrl(url.data(), static_cast<DWORD>(url.size()), 0, &components)) {
		error = L"网址解析失败（" + std::to_wstring(GetLastError()) + L"）";
		return false;
	}

	const std::wstring host(components.lpszHostName, components.dwHostNameLength);
	std::wstring resource(components.lpszUrlPath, components.dwUrlPathLength);
	if (components.dwExtraInfoLength) {
		resource.append(components.lpszExtraInfo, components.dwExtraInfoLength);
	}
	if (resource.empty()) resource = L"/";

	HINTERNET session = WinHttpOpen(
		L"DLSS5-AnyWindow-Updater/1.9.2",
		WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		error = L"网络初始化失败（" + std::to_wstring(GetLastError()) + L"）";
		return false;
	}
	WinHttpSetTimeouts(session, 5000, 5000, 10000, 15000);
	HINTERNET connection = WinHttpConnect(
		session, host.c_str(), components.nPort, 0);
	if (!connection) {
		error = L"连接 GitHub 失败（" + std::to_wstring(GetLastError()) + L"）";
		WinHttpCloseHandle(session);
		return false;
	}
	const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS
		? WINHTTP_FLAG_SECURE : 0;
	HINTERNET request = WinHttpOpenRequest(
		connection, L"GET", resource.c_str(), nullptr, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
	if (!request) {
		error = L"创建更新请求失败（" + std::to_wstring(GetLastError()) + L"）";
		WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);
		return false;
	}
	DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
	WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY,
		&redirectPolicy, sizeof(redirectPolicy));
	constexpr wchar_t headers[] =
		L"Accept: application/vnd.github+json\r\n"
		L"X-GitHub-Api-Version: 2022-11-28\r\n";
	bool ok = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1),
		WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE &&
		WinHttpReceiveResponse(request, nullptr) != FALSE;
	if (!ok) {
		error = L"GitHub 请求失败（" + std::to_wstring(GetLastError()) + L"）";
	} else {
		DWORD status = 0;
		DWORD statusSize = sizeof(status);
		if (!WinHttpQueryHeaders(request,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
			WINHTTP_NO_HEADER_INDEX) || status < 200 || status >= 300) {
			error = L"GitHub 返回 HTTP " + std::to_wstring(status);
			ok = false;
		}
	}
	while (ok) {
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(request, &available)) {
			error = L"读取更新数据失败（" + std::to_wstring(GetLastError()) + L"）";
			ok = false;
			break;
		}
		if (!available) break;
		if (response.size() + available > maximumBytes) {
			error = L"更新数据大小异常";
			ok = false;
			break;
		}
		const size_t oldSize = response.size();
		response.resize(oldSize + available);
		DWORD read = 0;
		if (!WinHttpReadData(request, response.data() + oldSize, available, &read)) {
			error = L"下载更新数据失败（" + std::to_wstring(GetLastError()) + L"）";
			ok = false;
			break;
		}
		response.resize(oldSize + read);
	}
	WinHttpCloseHandle(request);
	WinHttpCloseHandle(connection);
	WinHttpCloseHandle(session);
	return ok;
}

std::string JsonStringAfter(
	const std::string& json, std::string_view key,
	size_t start, size_t end = std::string::npos
) {
	const std::string needle = "\"" + std::string(key) + "\"";
	const size_t keyPosition = json.find(needle, start);
	if (keyPosition == std::string::npos || keyPosition >= end) return {};
	const size_t colon = json.find(':', keyPosition + needle.size());
	if (colon == std::string::npos || colon >= end) return {};
	const size_t quote = json.find('"', colon + 1);
	if (quote == std::string::npos || quote >= end) return {};
	std::string value;
	for (size_t index = quote + 1; index < json.size() && index < end; ++index) {
		const char ch = json[index];
		if (ch == '"') return value;
		if (ch == '\\' && index + 1 < json.size()) {
			const char escaped = json[++index];
			switch (escaped) {
			case '"': value.push_back('"'); break;
			case '\\': value.push_back('\\'); break;
			case '/': value.push_back('/'); break;
			case 'b': value.push_back('\b'); break;
			case 'f': value.push_back('\f'); break;
			case 'n': value.push_back('\n'); break;
			case 'r': value.push_back('\r'); break;
			case 't': value.push_back('\t'); break;
			default: return {};
			}
		} else {
			value.push_back(ch);
		}
	}
	return {};
}

bool ParseReleaseVersion(std::string_view tag, VersionNumber& version) {
	if (tag.empty() || (tag.front() != 'v' && tag.front() != 'V')) return false;
	size_t position = 1;
	auto parsePart = [&](int& value) {
		if (position >= tag.size() || tag[position] < '0' || tag[position] > '9') return false;
		value = 0;
		while (position < tag.size() && tag[position] >= '0' && tag[position] <= '9') {
			value = value * 10 + (tag[position++] - '0');
			if (value > 100000) return false;
		}
		return true;
	};
	if (!parsePart(version.major) || position >= tag.size() || tag[position++] != '.' ||
		!parsePart(version.minor) || position >= tag.size() || tag[position++] != '.' ||
		!parsePart(version.patch)) return false;
	return position == tag.size() || tag[position] == '-';
}

bool VersionIsNewer(const VersionNumber& candidate, const VersionNumber& current) {
	if (candidate.major != current.major) return candidate.major > current.major;
	if (candidate.minor != current.minor) return candidate.minor > current.minor;
	return candidate.patch > current.patch;
}

std::string FindReleaseAssetUrl(
	const std::string& json, size_t start, size_t end,
	std::string_view expectedName
) {
	size_t cursor = start;
	while (cursor < end) {
		const size_t namePosition = json.find("\"name\"", cursor);
		if (namePosition == std::string::npos || namePosition >= end) return {};
		const std::string name = JsonStringAfter(json, "name", namePosition, end);
		if (name == expectedName) {
			return JsonStringAfter(json, "browser_download_url", namePosition, end);
		}
		cursor = namePosition + 6;
	}
	return {};
}

std::unique_ptr<UpdateWorkerResult> CheckForUpdateWorker() {
	auto result = std::make_unique<UpdateWorkerResult>();
	std::vector<uint8_t> body;
	if (!HttpGet(GITHUB_RELEASES_API, 4 * 1024 * 1024, body, result->message)) {
		return result;
	}
	const std::string json(body.begin(), body.end());
	const VersionNumber current{ APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH };
	VersionNumber best = current;
	size_t cursor = 0;
	while (true) {
		const size_t tagPosition = json.find("\"tag_name\"", cursor);
		if (tagPosition == std::string::npos) break;
		const size_t nextTag = json.find("\"tag_name\"", tagPosition + 10);
		const size_t releaseEnd = nextTag == std::string::npos ? json.size() : nextTag;
		const std::string tag = JsonStringAfter(json, "tag_name", tagPosition, releaseEnd);
		VersionNumber candidate{};
		if (ParseReleaseVersion(tag, candidate) &&
			VersionIsNewer(candidate, best)) {
			best = candidate;
			result->updateAvailable = true;
			result->version = std::to_wstring(candidate.major) + L"." +
				std::to_wstring(candidate.minor) + L"." + std::to_wstring(candidate.patch);
			result->releaseUrl = std::wstring(GITHUB_REPOSITORY_URL) +
				L"/releases/tag/" + Utf8ToWide(tag);
			const std::string assetName = "DLSS5FloatingController-" +
				std::to_string(candidate.major) + "." +
				std::to_string(candidate.minor) + "." +
				std::to_string(candidate.patch) + ".exe";
			result->assetUrl = Utf8ToWide(FindReleaseAssetUrl(
				json, tagPosition, releaseEnd, assetName));
			result->hashUrl = Utf8ToWide(FindReleaseAssetUrl(
				json, tagPosition, releaseEnd, assetName + ".sha256.txt"));
		}
		cursor = releaseEnd;
	}
	result->success = true;
	if (!result->updateAvailable) result->message = L"已是最新版";
	return result;
}

std::wstring Sha256Hex(const std::vector<uint8_t>& bytes) {
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	DWORD objectSize = 0;
	DWORD resultSize = 0;
	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
		BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0) < 0) {
		if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
		return {};
	}
	std::vector<uint8_t> hashObject(objectSize);
	std::array<uint8_t, 32> digest{};
	if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectSize,
		nullptr, 0, 0) < 0 ||
		BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
			static_cast<ULONG>(bytes.size()), 0) < 0 ||
		BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
		if (hash) BCryptDestroyHash(hash);
		BCryptCloseAlgorithmProvider(algorithm, 0);
		return {};
	}
	BCryptDestroyHash(hash);
	BCryptCloseAlgorithmProvider(algorithm, 0);
	constexpr wchar_t digits[] = L"0123456789abcdef";
	std::wstring output;
	output.reserve(64);
	for (const uint8_t byte : digest) {
		output.push_back(digits[byte >> 4]);
		output.push_back(digits[byte & 0x0f]);
	}
	return output;
}

std::wstring FirstSha256(std::string_view text) {
	std::wstring hash;
	for (const unsigned char ch : text) {
		const char lower = ch >= 'A' && ch <= 'F' ? static_cast<char>(ch - 'A' + 'a') : ch;
		if ((lower >= '0' && lower <= '9') || (lower >= 'a' && lower <= 'f')) {
			hash.push_back(static_cast<wchar_t>(lower));
			if (hash.size() == 64) return hash;
		} else if (!hash.empty()) {
			hash.clear();
		}
	}
	return {};
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	if (!stream) return false;
	stream.write(reinterpret_cast<const char*>(bytes.data()),
		static_cast<std::streamsize>(bytes.size()));
	return stream.good();
}

std::wstring FileSha256Hex(const std::filesystem::path& path, size_t maximumBytes) {
	std::ifstream stream(path, std::ios::binary | std::ios::ate);
	if (!stream) return {};
	const std::streamoff length = stream.tellg();
	if (length < 0 || static_cast<uint64_t>(length) > maximumBytes) return {};
	stream.seekg(0, std::ios::beg);
	std::vector<uint8_t> bytes(static_cast<size_t>(length));
	if (!bytes.empty()) {
		stream.read(reinterpret_cast<char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
		if (!stream) return {};
	}
	return Sha256Hex(bytes);
}

std::wstring ComponentUpdateBaseUrl(bool qaInstance) {
	if (qaInstance) {
		std::wstring value(32768, L'\0');
		const DWORD length = GetEnvironmentVariableW(
			L"DLSS5_UPDATE_BASE_URL", value.data(), static_cast<DWORD>(value.size()));
		if (length > 0 && length < value.size()) {
			value.resize(length);
			if (value.back() != L'/') value.push_back(L'/');
			return value;
		}
		// Other controller UI tests intentionally carry no rendering payload.
		// Only opt into component synchronization when the QA server is explicit.
		return {};
	}
	return COMPONENT_UPDATE_BASE_URL;
}

struct CompanionUpdate {
	std::filesystem::path target;
	std::filesystem::path temporary;
	std::filesystem::path backup;
	std::wstring asset;
	std::wstring expectedHash;
	size_t maximumBytes = 0;
	bool hadOriginal = false;
	bool installed = false;
};

bool EnsureCompanionComponents(bool qaInstance, std::wstring& error) {
	const std::filesystem::path directory(ExeDirectory());
	std::array<CompanionUpdate, 2> components{
		CompanionUpdate{
			.target = directory / ENGINE_NAME,
			.asset = RENDERER_UPDATE_ASSET,
			.expectedHash = RENDERER_SHA256,
			.maximumBytes = 32 * 1024 * 1024 },
		CompanionUpdate{
			.target = directory / L"effects" / L"DLSSNR" / L"DLSSNR_AI_Filter.hlsl",
			.asset = EFFECT_UPDATE_ASSET,
			.expectedHash = EFFECT_SHA256,
			.maximumBytes = 1024 * 1024 }
	};

	std::vector<CompanionUpdate*> pending;
	for (CompanionUpdate& component : components) {
		if (FileSha256Hex(component.target, component.maximumBytes) !=
			component.expectedHash) pending.push_back(&component);
	}
	if (pending.empty()) return true;
	auto cleanupTemporary = [&] {
		for (CompanionUpdate* staged : pending) {
			if (!staged->temporary.empty()) DeleteFileW(staged->temporary.c_str());
		}
	};

	const std::wstring baseUrl = ComponentUpdateBaseUrl(qaInstance);
	if (baseUrl.empty()) return true;
	for (CompanionUpdate* component : pending) {
		std::vector<uint8_t> bytes;
		std::wstring httpError;
		if (!HttpGet(baseUrl + component->asset, component->maximumBytes,
			bytes, httpError)) {
			error = L"下载渲染组件失败：" + httpError;
			cleanupTemporary();
			return false;
		}
		if (Sha256Hex(bytes) != component->expectedHash) {
			error = L"渲染组件 SHA-256 校验失败，已拒绝安装";
			cleanupTemporary();
			return false;
		}
		std::error_code directoryError;
		std::filesystem::create_directories(
			component->target.parent_path(), directoryError);
		if (directoryError) {
			error = L"无法创建渲染组件目录";
			cleanupTemporary();
			return false;
		}
		component->temporary = component->target;
		component->temporary += L".update-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
		component->backup = component->target;
		component->backup += L".update-backup";
		DeleteFileW(component->temporary.c_str());
		if (!WriteBytes(component->temporary, bytes)) {
			error = L"无法暂存渲染组件更新";
			cleanupTemporary();
			return false;
		}
	}

	auto rollback = [&] {
		for (auto iterator = pending.rbegin(); iterator != pending.rend(); ++iterator) {
			CompanionUpdate& component = **iterator;
			if (!component.installed) continue;
			DeleteFileW(component.target.c_str());
			if (component.hadOriginal) {
				MoveFileExW(component.backup.c_str(), component.target.c_str(),
					MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
			}
			component.installed = false;
		}
	};

	for (CompanionUpdate* component : pending) {
		component->hadOriginal = FileExists(component->target);
		DeleteFileW(component->backup.c_str());
		if (component->hadOriginal && !MoveFileExW(
			component->target.c_str(), component->backup.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			error = L"渲染组件正在使用，无法更新（" +
				std::to_wstring(GetLastError()) + L"）";
			rollback();
			cleanupTemporary();
			return false;
		}
		if (!MoveFileExW(component->temporary.c_str(), component->target.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			const DWORD moveError = GetLastError();
			if (component->hadOriginal) {
				MoveFileExW(component->backup.c_str(), component->target.c_str(),
					MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
			}
			error = L"安装渲染组件失败（" + std::to_wstring(moveError) + L"）";
			rollback();
			cleanupTemporary();
			return false;
		}
		component->installed = true;
	}
	for (CompanionUpdate* component : pending) DeleteFileW(component->backup.c_str());
	return true;
}

std::unique_ptr<UpdateWorkerResult> DownloadUpdateWorker(
	std::wstring version, std::wstring assetUrl, std::wstring hashUrl,
	std::wstring releaseUrl
) {
	auto result = std::make_unique<UpdateWorkerResult>();
	result->download = true;
	result->version = std::move(version);
	result->releaseUrl = std::move(releaseUrl);
	if (assetUrl.empty() || hashUrl.empty()) {
		result->message = L"这个版本没有可自动安装的应用附件";
		return result;
	}
	std::vector<uint8_t> expectedBody;
	if (!HttpGet(hashUrl, 64 * 1024, expectedBody, result->message)) return result;
	const std::wstring expected = FirstSha256(std::string_view(
		reinterpret_cast<const char*>(expectedBody.data()), expectedBody.size()));
	if (expected.empty()) {
		result->message = L"更新校验文件格式不正确";
		return result;
	}
	std::vector<uint8_t> executable;
	if (!HttpGet(assetUrl, 32 * 1024 * 1024, executable, result->message)) return result;
	if (executable.size() < 2 || executable[0] != 'M' || executable[1] != 'Z') {
		result->message = L"下载的更新不是有效的 Windows 程序";
		return result;
	}
	const std::wstring actual = Sha256Hex(executable);
	if (actual.empty() || actual != expected) {
		result->message = L"更新文件 SHA-256 校验失败，已拒绝安装";
		return result;
	}
	std::error_code pathError;
	const std::filesystem::path tempDirectory = std::filesystem::temp_directory_path(pathError);
	if (pathError) {
		result->message = L"无法访问系统临时目录";
		return result;
	}
	const std::filesystem::path destination = tempDirectory /
		(L"DLSS5FloatingController-" + result->version + L"-" +
			std::to_wstring(GetCurrentProcessId()) + L".exe");
	if (!WriteBytes(destination, executable)) {
		result->message = L"无法写入临时更新文件";
		return result;
	}
	result->downloadedPath = destination.wstring();
	result->success = true;
	return result;
}

void PostWorkerResult(HWND hwnd, std::unique_ptr<UpdateWorkerResult> result) {
	UpdateWorkerResult* raw = result.release();
	if (!PostMessageW(hwnd, WM_UPDATE_WORKER_RESULT, 0,
		reinterpret_cast<LPARAM>(raw))) delete raw;
}

void StartUpdateCheck(ControllerData& data, bool automatic = false) {
	if (data.updateState == UpdateState::Checking ||
		data.updateState == UpdateState::Downloading) return;
	data.automaticUpdateCheck = automatic;
	data.updateState = UpdateState::Checking;
	data.updateMessage = L"正在连接 GitHub…";
	InvalidateRect(data.hwnd, nullptr, FALSE);
	const HWND hwnd = data.hwnd;
	std::thread([hwnd] {
		PostWorkerResult(hwnd, CheckForUpdateWorker());
	}).detach();
}

void StartUpdateDownload(ControllerData& data) {
	if (data.updateState != UpdateState::Available) return;
	if (data.updateAssetUrl.empty() || data.updateHashUrl.empty()) {
		ShellExecuteW(data.hwnd, L"open", data.updateReleaseUrl.c_str(),
			nullptr, nullptr, SW_SHOWNORMAL);
		return;
	}
	data.updateState = UpdateState::Downloading;
	data.updateMessage = L"正在下载并校验应用更新…";
	InvalidateRect(data.hwnd, nullptr, FALSE);
	const HWND hwnd = data.hwnd;
	const std::wstring version = data.updateVersion;
	const std::wstring assetUrl = data.updateAssetUrl;
	const std::wstring hashUrl = data.updateHashUrl;
	const std::wstring releaseUrl = data.updateReleaseUrl;
	std::thread([hwnd, version, assetUrl, hashUrl, releaseUrl] {
		PostWorkerResult(hwnd, DownloadUpdateWorker(
			version, assetUrl, hashUrl, releaseUrl));
	}).detach();
}

std::wstring ModulePath() {
	std::wstring path(32768, L'\0');
	const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	if (!length || length >= path.size()) return {};
	path.resize(length);
	return path;
}

std::wstring EscapePowerShellLiteral(std::wstring_view value) {
	std::wstring escaped;
	escaped.reserve(value.size() + 8);
	for (const wchar_t ch : value) {
		escaped.push_back(ch);
		if (ch == L'\'') escaped.push_back(L'\'');
	}
	return escaped;
}

bool WriteUtf8BomFile(const std::filesystem::path& path, std::wstring_view contents) {
	const std::string utf8 = WideToUtf8(contents);
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	if (!stream) return false;
	constexpr unsigned char bom[] = { 0xef, 0xbb, 0xbf };
	stream.write(reinterpret_cast<const char*>(bom), sizeof(bom));
	stream.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
	return stream.good();
}

bool LaunchSelfUpdate(std::wstring_view downloadedPath, std::wstring& error) {
	const std::wstring currentPath = ModulePath();
	if (currentPath.empty() || !FileExists(std::filesystem::path(downloadedPath))) {
		error = L"更新文件已经不存在";
		return false;
	}
	std::error_code pathError;
	const std::filesystem::path tempDirectory = std::filesystem::temp_directory_path(pathError);
	if (pathError) {
		error = L"无法访问系统临时目录";
		return false;
	}
	const std::filesystem::path scriptPath = tempDirectory /
		(L"DLSS5AnyWindowUpdate-" + std::to_wstring(GetCurrentProcessId()) + L".ps1");
	const std::wstring currentDirectory =
		std::filesystem::path(currentPath).parent_path().wstring();
	const std::wstring source = EscapePowerShellLiteral(downloadedPath);
	const std::wstring destination = EscapePowerShellLiteral(currentPath);
	const std::wstring workingDirectory = EscapePowerShellLiteral(currentDirectory);
	const std::wstring scriptLiteral = EscapePowerShellLiteral(scriptPath.wstring());
	std::wstring script =
		L"$ErrorActionPreference = 'SilentlyContinue'\r\n"
		L"Wait-Process -Id " + std::to_wstring(GetCurrentProcessId()) +
		L" -ErrorAction SilentlyContinue\r\n"
		L"$copied = $false\r\n"
		L"for ($i = 0; $i -lt 50 -and -not $copied; $i++) {\r\n"
		L"  try { Copy-Item -LiteralPath '" + source + L"' -Destination '" +
		destination + L"' -Force -ErrorAction Stop; $copied = $true }\r\n"
		L"  catch { Start-Sleep -Milliseconds 200 }\r\n"
		L"}\r\n"
		L"if ($copied) { Start-Process -FilePath '" + destination +
		L"' -WorkingDirectory '" + workingDirectory + L"' }\r\n"
		L"Remove-Item -LiteralPath '" + source + L"' -Force -ErrorAction SilentlyContinue\r\n"
		L"Remove-Item -LiteralPath '" + scriptLiteral + L"' -Force -ErrorAction SilentlyContinue\r\n";
	if (!WriteUtf8BomFile(scriptPath, script)) {
		error = L"无法创建更新辅助脚本";
		return false;
	}
	wchar_t windowsDirectory[MAX_PATH]{};
	if (!GetWindowsDirectoryW(windowsDirectory, MAX_PATH)) {
		error = L"无法定位 Windows PowerShell";
		return false;
	}
	const std::wstring powershell = std::wstring(windowsDirectory) +
		L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
	std::wstring command = L"\"" + powershell +
		L"\" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
		scriptPath.wstring() + L"\"";
	STARTUPINFOW startup{ sizeof(startup) };
	startup.dwFlags = STARTF_USESHOWWINDOW;
	startup.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION process{};
	if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
		currentDirectory.c_str(), &startup, &process)) {
		error = L"无法启动更新安装程序（" + std::to_wstring(GetLastError()) + L"）";
		DeleteFileW(scriptPath.c_str());
		return false;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return true;
}

void UnregisterToggleHotkey(ControllerData& data) {
	if (!data.hotkeyRegistered) return;
	UnregisterHotKey(data.hwnd, TOGGLE_HOTKEY_ID);
	data.hotkeyRegistered = false;
}

bool RegisterToggleHotkey(ControllerData& data) {
	UnregisterToggleHotkey(data);
	if (!data.settings.hotkeyVirtualKey) return true;
	data.hotkeyRegistered = RegisterHotKey(
		data.hwnd, TOGGLE_HOTKEY_ID,
		data.settings.hotkeyModifiers | MOD_NOREPEAT,
		data.settings.hotkeyVirtualKey) != FALSE;
	return data.hotkeyRegistered;
}

void UnregisterVisibilityHotkey(ControllerData& data) {
	if (!data.visibilityHotkeyRegistered) return;
	UnregisterHotKey(data.hwnd, VISIBILITY_HOTKEY_ID);
	data.visibilityHotkeyRegistered = false;
}

bool RegisterVisibilityHotkey(ControllerData& data) {
	UnregisterVisibilityHotkey(data);
	if (!data.uiSettings.visibilityHotkeyVirtualKey) return true;
	data.visibilityHotkeyRegistered = RegisterHotKey(
		data.hwnd, VISIBILITY_HOTKEY_ID,
		data.uiSettings.visibilityHotkeyModifiers | MOD_NOREPEAT,
		data.uiSettings.visibilityHotkeyVirtualKey) != FALSE;
	return data.visibilityHotkeyRegistered;
}

void UnregisterControllerHotkeys(ControllerData& data) {
	UnregisterToggleHotkey(data);
	UnregisterVisibilityHotkey(data);
}

bool RegisterControllerHotkeys(ControllerData& data) {
	const bool toggleRegistered = RegisterToggleHotkey(data);
	const bool visibilityRegistered = RegisterVisibilityHotkey(data);
	return toggleRegistered && visibilityRegistered;
}

bool IsModifierKey(UINT key) {
	return key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
		key == VK_MENU || key == VK_LMENU || key == VK_RMENU ||
		key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
		key == VK_LWIN || key == VK_RWIN;
}

UINT ModifierFlagForKey(UINT key) {
	if (key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL) return MOD_CONTROL;
	if (key == VK_MENU || key == VK_LMENU || key == VK_RMENU) return MOD_ALT;
	if (key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT) return MOD_SHIFT;
	if (key == VK_LWIN || key == VK_RWIN) return MOD_WIN;
	return 0;
}

UINT PressedHotkeyModifiers() {
	UINT modifiers = 0;
	if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
	if (GetKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
	if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
	if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) modifiers |= MOD_WIN;
	return modifiers;
}

std::wstring VirtualKeyName(UINT key) {
	if (key >= VK_F1 && key <= VK_F24) {
		return L"F" + std::to_wstring(key - VK_F1 + 1);
	}
	if (key >= L'A' && key <= L'Z') return std::wstring(1, static_cast<wchar_t>(key));
	if (key >= L'0' && key <= L'9') return std::wstring(1, static_cast<wchar_t>(key));
	UINT scanCode = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
	if (key == VK_LEFT || key == VK_UP || key == VK_RIGHT || key == VK_DOWN ||
		key == VK_PRIOR || key == VK_NEXT || key == VK_END || key == VK_HOME ||
		key == VK_INSERT || key == VK_DELETE || key == VK_DIVIDE || key == VK_NUMLOCK) {
		scanCode |= 0x100;
	}
	wchar_t name[64]{};
	if (GetKeyNameTextW(static_cast<LONG>(scanCode << 16), name, ARRAYSIZE(name)) > 0) {
		return name;
	}
	wchar_t fallback[16]{};
	swprintf_s(fallback, L"0x%02X", key);
	return fallback;
}

std::wstring HotkeyText(UINT modifiers, UINT virtualKey) {
	if (!virtualKey) return L"未设置";
	std::wstring text;
	auto append = [&text](const wchar_t* part) {
		if (!text.empty()) text += L" + ";
		text += part;
	};
	if (modifiers & MOD_CONTROL) append(L"Ctrl");
	if (modifiers & MOD_ALT) append(L"Alt");
	if (modifiers & MOD_SHIFT) append(L"Shift");
	if (modifiers & MOD_WIN) append(L"Win");
	const std::wstring keyName = VirtualKeyName(virtualKey);
	append(keyName.c_str());
	return text;
}

std::wstring HotkeyText(const FilterSettings& settings) {
	return HotkeyText(settings.hotkeyModifiers, settings.hotkeyVirtualKey);
}

std::wstring VisibilityHotkeyText(const UiSettings& settings) {
	return HotkeyText(
		settings.visibilityHotkeyModifiers, settings.visibilityHotkeyVirtualKey);
}

bool IsCapturingHotkey(const ControllerData& data) {
	return data.hotkeyCapture != HotkeyCapture::None;
}

void BeginHotkeyCapture(ControllerData& data, HotkeyCapture capture) {
	if (data.phase != Phase::Idle) return;
	UnregisterControllerHotkeys(data);
	data.hotkeyCapture = capture;
	data.capturedHotkeyModifiers = 0;
	data.targetTitle = capture == HotkeyCapture::WindowVisibility
		? L"请按浮窗组合键，例如 Alt+Q" : L"请按滤镜组合键，例如 Ctrl+A";
	SetFocus(data.hwnd);
	InvalidateRect(data.hwnd, nullptr, FALSE);
}

void CancelHotkeyCapture(ControllerData& data) {
	if (!IsCapturingHotkey(data)) return;
	data.hotkeyCapture = HotkeyCapture::None;
	data.capturedHotkeyModifiers = 0;
	data.targetTitle = RegisterControllerHotkeys(data)
		? L"快捷键未更改" : L"原快捷键中有组合键已被其他程序占用";
	InvalidateRect(data.hwnd, nullptr, FALSE);
}

void CommitHotkeyCapture(ControllerData& data, UINT key) {
	if (!IsCapturingHotkey(data)) return;
	if (key == VK_ESCAPE) {
		CancelHotkeyCapture(data);
		return;
	}
	const HotkeyCapture capture = data.hotkeyCapture;
	UINT& configuredModifiers = capture == HotkeyCapture::WindowVisibility
		? data.uiSettings.visibilityHotkeyModifiers : data.settings.hotkeyModifiers;
	UINT& configuredKey = capture == HotkeyCapture::WindowVisibility
		? data.uiSettings.visibilityHotkeyVirtualKey : data.settings.hotkeyVirtualKey;
	if (key == VK_BACK || key == VK_DELETE) {
		configuredModifiers = 0;
		configuredKey = 0;
		data.hotkeyCapture = HotkeyCapture::None;
		data.capturedHotkeyModifiers = 0;
		RegisterControllerHotkeys(data);
		data.targetTitle = capture == HotkeyCapture::WindowVisibility
			? L"浮窗快捷键已停用" : L"滤镜快捷键已停用";
		SaveSettings(data);
		InvalidateRect(data.hwnd, nullptr, FALSE);
		return;
	}
	if (IsModifierKey(key)) {
		data.capturedHotkeyModifiers |= ModifierFlagForKey(key);
		return;
	}
	const UINT modifiers = PressedHotkeyModifiers() | data.capturedHotkeyModifiers;
	const bool functionKey = key >= VK_F1 && key <= VK_F24;
	if (!modifiers && !functionKey) {
		data.targetTitle = L"字母或数字至少搭配一个修饰键";
		InvalidateRect(data.hwnd, nullptr, FALSE);
		return;
	}
	const UINT oldModifiers = configuredModifiers;
	const UINT oldKey = configuredKey;
	configuredModifiers = modifiers;
	configuredKey = key;
	data.hotkeyCapture = HotkeyCapture::None;
	data.capturedHotkeyModifiers = 0;
	if (RegisterControllerHotkeys(data)) {
		SaveSettings(data);
		data.targetTitle = (capture == HotkeyCapture::WindowVisibility
			? L"浮窗快捷键已改为 " : L"滤镜快捷键已改为 ") +
			HotkeyText(configuredModifiers, configuredKey);
	} else {
		configuredModifiers = oldModifiers;
		configuredKey = oldKey;
		RegisterControllerHotkeys(data);
		data.targetTitle = L"这个组合键已被占用，已恢复原设置";
	}
	InvalidateRect(data.hwnd, nullptr, FALSE);
}

const wchar_t* StyleName(int style) {
	switch (style) {
	case 0: return L"默认";
	case 1: return L"自然";
	default: return L"电影";
	}
}

const wchar_t* GuidanceModeName(int mode) {
	switch (mode) {
	case 0: return L"自动引导";
	case 2: return L"运动引导";
	case 3: return L"深度引导";
	default: return L"平面引导";
	}
}

const wchar_t* HistoryModeName(int mode) {
	switch (mode) {
	case 1: return L"静态稳定";
	case 2: return L"连续历史";
	default: return L"自适应历史";
	}
}

const wchar_t* BackendName(int backend) {
	return backend == 1 ? L"CAS SHARPEN" : L"DLSS5 / DLSSNR";
}

std::wstring RuntimeShortName(const ControllerData& data) {
	if (!data.sharedStats || data.sharedStats->magic != Magpie::DLSS5_STATS_MAGIC) {
		return L"自动选择中";
	}
	const LONG kind = InterlockedCompareExchange(
		&data.sharedStats->runtimeKind, 0, 0);
	switch (kind) {
	case Magpie::DLSS5RuntimeShortFuseFp16: return L"RTX20 SF-v2 FP16";
	case Magpie::DLSS5RuntimeRtx30Patched: return L"RTX30 PATCH";
	case Magpie::DLSS5RuntimeRtx40Patched: return L"RTX40 PATCH";
	case Magpie::DLSS5RuntimeRtx50Original: return L"RTX50 ORIGINAL";
	case Magpie::DLSS5RuntimeCustom: return L"手动运行库";
	case Magpie::DLSS5RuntimeLegacyRoot: return L"兼容回退";
	default: return L"自动选择中";
	}
}

std::wstring GuidanceAvailabilityText(const ControllerData& data) {
	const wchar_t* motionName = data.motionGuidanceHardware ?
		L"硬件运动预测" : L"软件运动预测";
	if (data.motionGuidanceAvailable && data.depthGuidanceAvailable) {
		return std::wstring(L"可用组件：") + motionName + L" + 深度预测";
	}
	if (data.motionGuidanceAvailable) {
		return std::wstring(L"可用组件：") + motionName + L"（无深度模型）";
	}
	if (data.depthGuidanceAvailable) return L"可用组件：深度预测（运动预测不可用）";
	return L"当前为精简组件：只能使用平面引导";
}

std::wstring SettingsSummary(const ControllerData& data) {
	if (data.settings.backend == 1) {
		return std::wstring(BackendName(data.settings.backend)) + L" · 锐度 " +
			std::to_wstring(data.settings.intensity) + L"% · 单帧无历史";
	}
	return std::wstring(StyleName(data.settings.style)) + L" · 强度 " +
		std::to_wstring(data.settings.intensity) + L"% · " +
		L"处理 " + std::to_wstring(data.settings.processingResolutionPercent) + L"% · " +
		std::to_wstring(data.settings.passes) + L" 次处理 · " +
		HistoryModeName(data.settings.historyMode) + L" · " +
		GuidanceModeName(data.settings.guidanceMode);
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
	data.sharedStats->version = 3;
	InterlockedExchange(&data.sharedStats->fps, 0);
	InterlockedExchange(&data.sharedStats->featureState, Magpie::DLSS5FeaturePending);
	InterlockedExchange(&data.sharedStats->evaluateSuccessCount, 0);
	InterlockedExchange(&data.sharedStats->evaluateFailureCount, 0);
	InterlockedExchange(&data.sharedStats->configuredPasses, data.settings.passes);
	InterlockedExchange(&data.sharedStats->runtimeKind, Magpie::DLSS5RuntimeUnknown);
	InterlockedExchange(&data.sharedStats->cudaComputeMajor, 0);
	InterlockedExchange(&data.sharedStats->cudaComputeMinor, 0);
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
		if (data.settings.backend == 1) {
			if (featureState == Magpie::DLSS5FeatureFailed) {
				return L"CAS SHARPEN 初始化失败";
			}
			if (featureState == Magpie::DLSS5FeatureEvaluating) {
				return L"CAS SHARPEN 已生效 · " +
					(data.targetTitle.empty() ? L"目标窗口" : data.targetTitle);
			}
			return L"正在初始化 CAS SHARPEN…";
		}
		if (featureState == Magpie::DLSS5FeatureFailed) {
			return L"DLSS5 初始化失败 · " + RuntimeShortName(data) + L" · 当前是直通画面";
		}
		if (featureState == Magpie::DLSS5FeatureEvaluating) {
			return L"DLSS5 已生效 · " + RuntimeShortName(data) + L" · " +
				std::to_wstring(data.settings.passes) + L" 次/帧";
		}
		if (featureState == Magpie::DLSS5FeatureCreated) {
			return L"Feature 18 已创建 · " + RuntimeShortName(data) + L" · 等待首帧";
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
			if (featureState == Magpie::DLSS5FeatureFailed) return CLAY;
			if (featureState == Magpie::DLSS5FeatureEvaluating) return GOLD;
		}
		return GOLD;
	case Phase::Selecting:
	case Phase::Starting: return GOLD;
	case Phase::Stopping: return CLAY;
	default: return PARCHMENT_MUTED;
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

void FillPanel(
	HDC dc, const RECT& rect, COLORREF fill,
	COLORREF light = WOOD_BORDER_LIGHT, COLORREF dark = WOOD_BORDER_DARK
) {
	(void)light;
	(void)dark;
	FillRounded(dc, rect, fill, 9);
}

void DrawPercentageSlider(
	HDC dc, const ControllerData& data, int sliderId, int rowY,
	const wchar_t* label, int value, bool enabled = true
) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	SetTextColor(dc, enabled ? PARCHMENT : PARCHMENT_DISABLED);
	SelectObject(dc, data.statusFont);
	RECT labelRect{ Dip(data, 19), Dip(data, rowY), client.right - Dip(data, 70), Dip(data, rowY + 22) };
	DrawTextW(dc, label, -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	const std::wstring valueText = std::to_wstring(value) + L"%";
	SetTextColor(dc, enabled ? PARCHMENT_MUTED : PARCHMENT_DISABLED);
	RECT valueRect{ client.right - Dip(data, 70), Dip(data, rowY),
		client.right - Dip(data, 19), Dip(data, rowY + 22) };
	DrawTextW(dc, valueText.c_str(), -1, &valueRect,
		DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	const int left = Dip(data, 20);
	const int right = client.right - Dip(data, 20);
	const int centerY = Dip(data, rowY + 30);
	RECT track{ left, centerY - Dip(data, 2), right, centerY + Dip(data, 2) };
	FillRounded(dc, track, WOOD_BORDER_DARK, Dip(data, 4));
	const int thumbX = left + MulDiv(right - left, value, 100);
	RECT filled{ left, centerY - Dip(data, 2), std::max(left + 1, thumbX), centerY + Dip(data, 2) };
	FillRounded(dc, filled, enabled ? GOLD : WOOD_TRACK, Dip(data, 4));
	const int thumbRadius = Dip(data, data.activeSlider == sliderId ? 7 : 6);
	HBRUSH thumbBrush = CreateSolidBrush(enabled ? PARCHMENT : PARCHMENT_DISABLED);
	HGDIOBJ oldBrush = SelectObject(dc, thumbBrush);
	HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
	Ellipse(dc, thumbX - thumbRadius, centerY - thumbRadius,
		thumbX + thumbRadius, centerY + thumbRadius);
	SelectObject(dc, oldPen);
	SelectObject(dc, oldBrush);
	DeleteObject(thumbBrush);
}

void DrawSlider(
	HDC dc, const ControllerData& data, int index,
	const wchar_t* label, int value, bool enabled = true
) {
	DrawPercentageSlider(dc, data, index, 151 + index * 46,
		label, value, enabled);
}

void DrawToggle(
	HDC dc, const ControllerData& data, const wchar_t* label,
	int rowY, bool value, bool enabled = true
) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	RECT labelRect{ Dip(data, 19), Dip(data, rowY),
		client.right - Dip(data, 70), Dip(data, rowY + 32) };
	SetTextColor(dc, enabled ? PARCHMENT : PARCHMENT_DISABLED);
	SelectObject(dc, data.statusFont);
	DrawTextW(dc, label, -1, &labelRect,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	RECT toggle{ client.right - Dip(data, 58), Dip(data, rowY + 5),
		client.right - Dip(data, 20), Dip(data, rowY + 27) };
	FillRounded(dc, toggle,
		enabled && value ? GOLD_DARK : WOOD_TRACK, Dip(data, 18));
	const int knobRadius = Dip(data, 8);
	const int knobCenterX = value
		? toggle.right - Dip(data, 11) : toggle.left + Dip(data, 11);
	const int knobCenterY = (toggle.top + toggle.bottom) / 2;
	HBRUSH knobBrush = CreateSolidBrush(enabled ? PARCHMENT : PARCHMENT_DISABLED);
	HGDIOBJ oldBrush = SelectObject(dc, knobBrush);
	HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
	Ellipse(dc, knobCenterX - knobRadius, knobCenterY - knobRadius,
		knobCenterX + knobRadius, knobCenterY + knobRadius);
	SelectObject(dc, oldPen);
	SelectObject(dc, oldBrush);
	DeleteObject(knobBrush);
}

void PaintSettingsTabs(HDC dc, const ControllerData& data) {
	const wchar_t* labels[] = { L"滤镜", L"外观", L"关于" };
	for (int page = 0; page < 3; ++page) {
		RECT tab = SettingsTabRect(data, page);
		if (data.settingsPage == page) {
			FillPanel(dc, tab, WOOD_PANEL_DARK);
		}
		SetTextColor(dc, data.settingsPage == page ? GOLD : PARCHMENT_MUTED);
		DrawTextW(dc, labels[page], -1, &tab,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}
}

std::wstring UpdateStatusText(const ControllerData& data) {
	switch (data.updateState) {
	case UpdateState::Checking:
		return L"正在连接 GitHub…";
	case UpdateState::Current:
		return L"已是最新版 · " + std::wstring(APP_VERSION);
	case UpdateState::Available:
		return L"发现新版本 · " + data.updateVersion;
	case UpdateState::Downloading:
		return L"正在下载并验证更新…";
	case UpdateState::Failed:
		return data.updateMessage.empty() ? L"检查更新失败" : data.updateMessage;
	case UpdateState::Idle:
	default:
		return L"尚未检查更新";
	}
}

std::wstring UpdateActionText(const ControllerData& data) {
	switch (data.updateState) {
	case UpdateState::Checking: return L"正在检查…";
	case UpdateState::Downloading: return L"正在下载…";
	case UpdateState::Available:
		return data.updateAssetUrl.empty() || data.updateHashUrl.empty()
			? L"打开发布页面" : L"下载并安装";
	default: return L"立即检查更新";
	}
}

void PaintAboutSettings(HDC dc, ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	SelectObject(dc, data.statusFont);

	SetTextColor(dc, PARCHMENT);
	RECT product{ Dip(data, 19), Dip(data, 116), client.right - Dip(data, 19), Dip(data, 143) };
	DrawTextW(dc, L"DLSS5 任意窗口滤镜", -1, &product,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	SetTextColor(dc, GOLD);
	std::wstring version = L"当前版本  " + std::wstring(APP_VERSION);
	RECT versionRect{ Dip(data, 19), Dip(data, 137), client.right - Dip(data, 19), Dip(data, 159) };
	DrawTextW(dc, version.c_str(), -1, &versionRect,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	DrawToggle(dc, data, L"启动时自动更新", 155,
		data.uiSettings.autoCheckUpdates);

	RECT statusCard{ Dip(data, 18), Dip(data, 202), client.right - Dip(data, 18), Dip(data, 246) };
	FillPanel(dc, statusCard, WOOD_PANEL_DARK, WOOD_BORDER_LIGHT, WOOD_BORDER_DARK);
	SetTextColor(dc, data.updateState == UpdateState::Failed ? CLAY_HOVER : PARCHMENT_MUTED);
	RECT statusText = statusCard;
	statusText.left += Dip(data, 12);
	statusText.right -= Dip(data, 12);
	const std::wstring updateStatus = UpdateStatusText(data);
	DrawTextW(dc, updateStatus.c_str(), -1, &statusText,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

	RECT action = AboutUpdateActionRect(data);
	const bool busy = data.updateState == UpdateState::Checking ||
		data.updateState == UpdateState::Downloading;
	FillPanel(dc, action, busy ? WOOD_TRACK : GOLD,
		busy ? WOOD_BORDER_LIGHT : GOLD_HOVER,
		busy ? WOOD_BORDER_DARK : GOLD_DARK);
	SetTextColor(dc, busy ? PARCHMENT_DISABLED : WOOD_BORDER_DARK);
	SelectObject(dc, data.buttonFont);
	const std::wstring actionText = UpdateActionText(data);
	DrawTextW(dc, actionText.c_str(), -1, &action,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	SelectObject(dc, data.statusFont);
	SetTextColor(dc, PARCHMENT_MUTED);
	RECT safeHint{ Dip(data, 19), Dip(data, 309), client.right - Dip(data, 19), Dip(data, 337) };
	DrawTextW(dc, L"自动更新应用组件，保留运行库、模型和设置", -1, &safeHint,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

	RECT source = AboutSourceLinkRect(data);
	FillPanel(dc, source, WOOD_PANEL_DARK, WOOD_BORDER_LIGHT, WOOD_BORDER_DARK);
	SetTextColor(dc, GOLD);
	DrawTextW(dc, L"GitHub 源代码  ↗", -1, &source,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	SetTextColor(dc, PARCHMENT_MUTED);
	RECT linkHint{ Dip(data, 19), Dip(data, 401), client.right - Dip(data, 19), Dip(data, 447) };
	DrawTextW(dc, L"更新通过 HTTPS 从项目 Release 获取，并校验 SHA-256。", -1, &linkHint,
		DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
}

void PaintAppearanceSettings(HDC dc, ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	SelectObject(dc, data.statusFont);

	SetTextColor(dc, PARCHMENT);
	RECT themeLabel{ Dip(data, 19), Dip(data, 112), client.right - Dip(data, 19), Dip(data, 136) };
	DrawTextW(dc, L"界面配色", -1, &themeLabel,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	RECT themes = UiThemeRect(data);
	FillPanel(dc, themes, WOOD_PANEL_DARK);
	const wchar_t* themeLabels[] = { L"木棕", L"暗黑", L"深蓝" };
	const int segmentWidth = (themes.right - themes.left) / UI_THEME_COUNT;
	for (int index = 0; index < UI_THEME_COUNT; ++index) {
		RECT segment{ themes.left + segmentWidth * index, themes.top,
			index == UI_THEME_COUNT - 1 ? themes.right : themes.left + segmentWidth * (index + 1),
			themes.bottom };
		if (data.uiSettings.theme == index) {
			RECT selected{ segment.left + Dip(data, 2), segment.top + Dip(data, 2),
				segment.right - Dip(data, 2), segment.bottom - Dip(data, 2) };
			FillPanel(dc, selected, GOLD);
		}
		const UiPalette& preview = UI_PALETTES[index];
		const int swatchSize = Dip(data, 8);
		const int swatchX = segment.left + Dip(data, 15);
		const int swatchY = (segment.top + segment.bottom - swatchSize) / 2;
		RECT swatch{ swatchX, swatchY, swatchX + swatchSize, swatchY + swatchSize };
		HBRUSH swatchBrush = CreateSolidBrush(preview.accent);
		HGDIOBJ oldBrush = SelectObject(dc, swatchBrush);
		HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
		Ellipse(dc, swatch.left, swatch.top, swatch.right, swatch.bottom);
		SelectObject(dc, oldPen);
		SelectObject(dc, oldBrush);
		DeleteObject(swatchBrush);

		RECT textRect{ segment.left + Dip(data, 22), segment.top,
			segment.right - Dip(data, 5), segment.bottom };
		SetTextColor(dc, data.uiSettings.theme == index ? WOOD_BORDER_DARK : PARCHMENT_MUTED);
		DrawTextW(dc, themeLabels[index], -1, &textRect,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}

	DrawPercentageSlider(dc, data, 3, 194,
		L"窗口透明度", data.uiSettings.opacity);
	SetTextColor(dc, PARCHMENT_MUTED);
	RECT opacityHint{ Dip(data, 19), Dip(data, 243), client.right - Dip(data, 19), Dip(data, 266) };
	DrawTextW(dc, L"只影响这个控制窗口，不影响目标画面", -1, &opacityHint,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	SetTextColor(dc, PARCHMENT);
	RECT hotkeyLabel{ Dip(data, 19), Dip(data, 283), Dip(data, 124), Dip(data, 317) };
	DrawTextW(dc, L"滤镜快捷键", -1, &hotkeyLabel,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	RECT hotkey = HotkeyRect(data);
	const bool capturingFilterHotkey =
		data.hotkeyCapture == HotkeyCapture::FilterToggle;
	FillPanel(dc, hotkey,
		capturingFilterHotkey ? GOLD_DARK : WOOD_PANEL_DARK,
		capturingFilterHotkey ? GOLD_HOVER : WOOD_BORDER_LIGHT,
		WOOD_BORDER_DARK);
	SetTextColor(dc, capturingFilterHotkey ? PARCHMENT : GOLD);
	const std::wstring hotkeyValue = capturingFilterHotkey
		? L"> 按下组合键 <" : HotkeyText(data.settings);
	DrawTextW(dc, hotkeyValue.c_str(), -1, &hotkey,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

	SetTextColor(dc, PARCHMENT_MUTED);
	RECT hotkeyHint{ Dip(data, 19), Dip(data, 322), client.right - Dip(data, 19), Dip(data, 345) };
	DrawTextW(dc, L"双键可用：Ctrl+A / Alt+Q · Backspace 停用", -1, &hotkeyHint,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

	SetTextColor(dc, PARCHMENT);
	RECT visibilityHotkeyLabel{
		Dip(data, 19), Dip(data, 365), Dip(data, 124), Dip(data, 399) };
	DrawTextW(dc, L"隐藏浮窗", -1, &visibilityHotkeyLabel,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	RECT visibilityHotkey = VisibilityHotkeyRect(data);
	const bool capturingVisibilityHotkey =
		data.hotkeyCapture == HotkeyCapture::WindowVisibility;
	FillPanel(dc, visibilityHotkey,
		capturingVisibilityHotkey ? GOLD_DARK : WOOD_PANEL_DARK,
		capturingVisibilityHotkey ? GOLD_HOVER : WOOD_BORDER_LIGHT,
		WOOD_BORDER_DARK);
	SetTextColor(dc, capturingVisibilityHotkey ? PARCHMENT : GOLD);
	const std::wstring visibilityHotkeyValue = capturingVisibilityHotkey
		? L"> 按下组合键 <" : VisibilityHotkeyText(data.uiSettings);
	DrawTextW(dc, visibilityHotkeyValue.c_str(), -1, &visibilityHotkey,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

	SetTextColor(dc, PARCHMENT_MUTED);
	RECT visibilityHotkeyHint{
		Dip(data, 19), Dip(data, 404), client.right - Dip(data, 19), Dip(data, 427) };
	DrawTextW(dc, L"隐藏后滤镜继续运行，再按一次恢复浮窗", -1, &visibilityHotkeyHint,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

	SetTextColor(dc, GOLD);
	RECT liveHint{ Dip(data, 19), Dip(data, 457), client.right - Dip(data, 19), Dip(data, 481) };
	DrawTextW(dc, L"配色与透明度会即时预览并自动保存", -1, &liveHint,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void PaintSettingsPanel(HDC dc, ControllerData& data) {
	RECT client{};
	GetClientRect(data.hwnd, &client);
	const bool experimental = data.settings.backend == 0;
	HPEN separator = CreatePen(PS_SOLID, std::max(1, Dip(data, 2)), WOOD_BORDER_DARK);
	HGDIOBJ oldPen = SelectObject(dc, separator);
	MoveToEx(dc, Dip(data, 18), Dip(data, 76), nullptr);
	LineTo(dc, client.right - Dip(data, 18), Dip(data, 76));
	SelectObject(dc, oldPen);
	DeleteObject(separator);

	SelectObject(dc, data.statusFont);
	PaintSettingsTabs(dc, data);
	if (data.settingsPage != 2) {
		SetTextColor(dc, GOLD);
		RECT reset = ResetRect(data);
		DrawTextW(dc, L"恢复默认", -1, &reset,
			DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}
	if (data.settingsPage == 1) {
		PaintAppearanceSettings(dc, data);
		return;
	}
	if (data.settingsPage == 2) {
		PaintAboutSettings(dc, data);
		return;
	}

	RECT style = StyleRect(data);
	FillPanel(dc, style, WOOD_PANEL_DARK);
	const wchar_t* labels[] = { L"默认", L"自然", L"电影" };
	const int segmentWidth = (style.right - style.left) / 3;
	for (int index = 0; index < 3; ++index) {
		RECT segment{ style.left + segmentWidth * index, style.top,
			index == 2 ? style.right : style.left + segmentWidth * (index + 1), style.bottom };
		if (experimental && data.settings.style == index) {
			RECT selected{ segment.left + Dip(data, 2), segment.top + Dip(data, 2),
				segment.right - Dip(data, 2), segment.bottom - Dip(data, 2) };
			FillPanel(dc, selected, GOLD, GOLD_HOVER, GOLD_DARK);
		}
		SetTextColor(dc, !experimental ? PARCHMENT_DISABLED :
			(data.settings.style == index ? WOOD_BORDER_DARK : PARCHMENT_MUTED));
		SelectObject(dc, data.statusFont);
		DrawTextW(dc, labels[index], -1, &segment,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}

	DrawSlider(dc, data, 0, L"效果强度", data.settings.intensity);
	DrawSlider(dc, data, 1, L"局部色调", data.settings.localTone, experimental);
	DrawSlider(dc, data, 2, L"局部结构", data.settings.localStructure, experimental);

	DrawToggle(dc, data, L"自动遮罩", 287, data.settings.autoMask, experimental);
	SetTextColor(dc, experimental ? PARCHMENT : PARCHMENT_DISABLED);
	RECT historyLabel{ Dip(data, 19), Dip(data, 323), Dip(data, 120), Dip(data, 355) };
	DrawTextW(dc, L"历史策略", -1, &historyLabel,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	RECT history = HistoryModeRect(data);
	FillPanel(dc, history, WOOD_PANEL_DARK);
	const wchar_t* historyLabels[] = { L"自适应", L"静态稳", L"连续" };
	const int historySegmentWidth = (history.right - history.left) / 3;
	for (int mode = 0; mode < 3; ++mode) {
		RECT segment{ history.left + historySegmentWidth * mode, history.top,
			mode == 2 ? history.right : history.left + historySegmentWidth * (mode + 1), history.bottom };
		if (experimental && data.settings.historyMode == mode) {
			RECT selected{ segment.left + Dip(data, 2), segment.top + Dip(data, 2),
				segment.right - Dip(data, 2), segment.bottom - Dip(data, 2) };
			FillPanel(dc, selected, GOLD, GOLD_HOVER, GOLD_DARK);
		}
		SetTextColor(dc, !experimental ? PARCHMENT_DISABLED :
			(data.settings.historyMode == mode ? WOOD_BORDER_DARK : PARCHMENT_MUTED));
		DrawTextW(dc, historyLabels[mode], -1, &segment,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}

	RECT passesLabel{ Dip(data, 19), Dip(data, 365), Dip(data, 180), Dip(data, 397) };
	SetTextColor(dc, experimental ? PARCHMENT : PARCHMENT_DISABLED);
	DrawTextW(dc, L"处理次数", -1, &passesLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	RECT passes = PassesRect(data);
	FillPanel(dc, passes, WOOD_PANEL_DARK);
	const int passSegmentWidth = (passes.right - passes.left) / 4;
	for (int index = 0; index < 4; ++index) {
		RECT segment{ passes.left + passSegmentWidth * index, passes.top,
			index == 3 ? passes.right : passes.left + passSegmentWidth * (index + 1), passes.bottom };
		if (experimental && data.settings.passes == index + 1) {
			RECT selected{ segment.left + Dip(data, 2), segment.top + Dip(data, 2),
				segment.right - Dip(data, 2), segment.bottom - Dip(data, 2) };
			FillPanel(dc, selected, GOLD, GOLD_HOVER, GOLD_DARK);
		}
		SetTextColor(dc, !experimental ? PARCHMENT_DISABLED :
			(data.settings.passes == index + 1 ? WOOD_BORDER_DARK : PARCHMENT_MUTED));
		const std::wstring text = std::to_wstring(index + 1) + L"次";
		DrawTextW(dc, text.c_str(), -1, &segment,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}

	SetTextColor(dc, experimental ? PARCHMENT : PARCHMENT_DISABLED);
	RECT guidanceLabel{ Dip(data, 19), Dip(data, 403), client.right - Dip(data, 19), Dip(data, 425) };
	DrawTextW(dc, L"帧引导", -1, &guidanceLabel,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	RECT guidance = GuidanceRect(data);
	FillPanel(dc, guidance, WOOD_PANEL_DARK);
	const wchar_t* guidanceLabels[] = { L"自动", L"平面", L"运动", L"深度" };
	const int guidanceSegmentWidth = (guidance.right - guidance.left) / 4;
	for (int mode = 0; mode < 4; ++mode) {
		RECT segment{ guidance.left + guidanceSegmentWidth * mode, guidance.top,
			mode == 3 ? guidance.right : guidance.left + guidanceSegmentWidth * (mode + 1), guidance.bottom };
		const bool available = experimental && GuidanceModeAvailable(data, mode);
		if (available && data.settings.guidanceMode == mode) {
			RECT selected{ segment.left + Dip(data, 2), segment.top + Dip(data, 2),
				segment.right - Dip(data, 2), segment.bottom - Dip(data, 2) };
			FillPanel(dc, selected, GOLD, GOLD_HOVER, GOLD_DARK);
		}
		SetTextColor(dc, !available ? PARCHMENT_DISABLED :
			(data.settings.guidanceMode == mode ? WOOD_BORDER_DARK : PARCHMENT_MUTED));
		DrawTextW(dc, guidanceLabels[mode], -1, &segment,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}

	RECT depthLabel{ Dip(data, 19), Dip(data, 469), Dip(data, 184), Dip(data, 501) };
	SetTextColor(dc, experimental && data.depthGuidanceAvailable ?
		PARCHMENT : PARCHMENT_DISABLED);
	DrawTextW(dc, L"深度更新间隔", -1, &depthLabel,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	RECT interval = DepthIntervalRect(data);
	FillPanel(dc, interval, WOOD_PANEL_DARK);
	constexpr int intervalValues[] = { 1, 2, 4, 8 };
	const int intervalSegmentWidth = (interval.right - interval.left) / 4;
	for (int index = 0; index < 4; ++index) {
		RECT segment{ interval.left + intervalSegmentWidth * index, interval.top,
			index == 3 ? interval.right : interval.left + intervalSegmentWidth * (index + 1), interval.bottom };
		const bool selected = experimental && data.depthGuidanceAvailable &&
			data.settings.depthInferenceInterval == intervalValues[index];
		if (selected) {
			RECT selectedRect{ segment.left + Dip(data, 2), segment.top + Dip(data, 2),
				segment.right - Dip(data, 2), segment.bottom - Dip(data, 2) };
			FillPanel(dc, selectedRect, GOLD, GOLD_HOVER, GOLD_DARK);
		}
		SetTextColor(dc, !experimental || !data.depthGuidanceAvailable ? PARCHMENT_DISABLED :
			(selected ? WOOD_BORDER_DARK : PARCHMENT_MUTED));
		const std::wstring text = std::to_wstring(intervalValues[index]);
		DrawTextW(dc, text.c_str(), -1, &segment,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}

	SetTextColor(dc, !experimental ? GOLD :
		((data.motionGuidanceAvailable || data.depthGuidanceAvailable)
			? GOLD : PARCHMENT_MUTED));
	const std::wstring availability = experimental
		? GuidanceAvailabilityText(data)
		: L"CAS SHARPEN：AMD CAS 单帧锐化，不使用运动、深度或历史";
	RECT availabilityHint{ Dip(data, 19), Dip(data, 507), client.right - Dip(data, 19), Dip(data, 529) };
	DrawTextW(dc, availability.c_str(), -1, &availabilityHint,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

	SetTextColor(dc, PARCHMENT_MUTED);
	RECT depthHint{ Dip(data, 19), Dip(data, 529), client.right - Dip(data, 19), Dip(data, 550) };
	DrawTextW(dc, experimental
		? L"自适应：有运动引导时保留历史，否则逐帧重置"
		: L"优点：低负载、无历史闪烁；限制：不会生成新细节", -1, &depthHint,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	RECT hint{ Dip(data, 19), Dip(data, 549), client.right - Dip(data, 19), Dip(data, 570) };
	DrawTextW(dc, data.settings.backend == 1
		? L"CAS SHARPEN 仅使用效果强度作为锐度，其余参数不参与"
		: L"多次处理会放大动态闪烁；视频建议 1 次", -1, &hint,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	SetTextColor(dc, PARCHMENT);
	RECT backendLabel{ Dip(data, 19), Dip(data, 579), Dip(data, 124), Dip(data, 611) };
	DrawTextW(dc, L"ENGINE", -1, &backendLabel,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	RECT backend = BackendRect(data);
	FillPanel(dc, backend, WOOD_PANEL_DARK);
	const wchar_t* backendLabels[] = { L"DLSS5 / DLSSNR", L"CAS SHARPEN" };
	const int backendSegmentWidth = (backend.right - backend.left) / 2;
	for (int mode = 0; mode < 2; ++mode) {
		RECT segment{ backend.left + backendSegmentWidth * mode, backend.top,
			mode == 1 ? backend.right : backend.left + backendSegmentWidth * (mode + 1), backend.bottom };
		if (data.settings.backend == mode) {
			RECT selected{ segment.left + Dip(data, 2), segment.top + Dip(data, 2),
				segment.right - Dip(data, 2), segment.bottom - Dip(data, 2) };
			FillPanel(dc, selected, GOLD, GOLD_HOVER, GOLD_DARK);
		}
		SetTextColor(dc, data.settings.backend == mode ? WOOD_BORDER_DARK : PARCHMENT_MUTED);
		DrawTextW(dc, backendLabels[mode], -1, &segment,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}

	SetTextColor(dc, experimental ? PARCHMENT : PARCHMENT_DISABLED);
	RECT resolutionLabel{ Dip(data, 19), Dip(data, 621), Dip(data, 120), Dip(data, 653) };
	DrawTextW(dc, L"处理分辨率 %", -1, &resolutionLabel,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	RECT resolution = ProcessingResolutionRect(data);
	FillPanel(dc, resolution, WOOD_PANEL_DARK);
	constexpr int resolutionValues[] = { 100, 75, 67, 50, 33, 25 };
	constexpr int resolutionValueCount = static_cast<int>(std::size(resolutionValues));
	const int resolutionSegmentWidth =
		(resolution.right - resolution.left) / resolutionValueCount;
	for (int index = 0; index < resolutionValueCount; ++index) {
		RECT segment{ resolution.left + resolutionSegmentWidth * index, resolution.top,
			index == resolutionValueCount - 1 ? resolution.right :
				resolution.left + resolutionSegmentWidth * (index + 1), resolution.bottom };
		const bool selected = experimental &&
			data.settings.processingResolutionPercent == resolutionValues[index];
		if (selected) {
			RECT selectedRect{ segment.left + Dip(data, 2), segment.top + Dip(data, 2),
				segment.right - Dip(data, 2), segment.bottom - Dip(data, 2) };
			FillPanel(dc, selectedRect, GOLD, GOLD_HOVER, GOLD_DARK);
		}
		SetTextColor(dc, !experimental ? PARCHMENT_DISABLED :
			(selected ? WOOD_BORDER_DARK : PARCHMENT_MUTED));
		const std::wstring text = std::to_wstring(resolutionValues[index]);
		DrawTextW(dc, text.c_str(), -1, &segment,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}
	SetTextColor(dc, experimental ? PARCHMENT_MUTED : PARCHMENT_DISABLED);
	RECT resolutionHint{ Dip(data, 19), Dip(data, 655), client.right - Dip(data, 19), Dip(data, 679) };
	DrawTextW(dc, L"≤33% 为极限档；输出尺寸、窗口位置不变", -1, &resolutionHint,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

}

void PaintController(ControllerData& data) {
	PAINTSTRUCT ps{};
	HDC dc = BeginPaint(data.hwnd, &ps);
	RECT client{};
	GetClientRect(data.hwnd, &client);
	ApplyUiPalette(data.uiSettings.theme);

	HDC memory = CreateCompatibleDC(dc);
	HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
	HGDIOBJ oldBitmap = SelectObject(memory, bitmap);

	HBRUSH background = CreateSolidBrush(WOOD_PANEL);
	FillRect(memory, &client, background);
	DeleteObject(background);

	HPEN borderPen = CreatePen(PS_SOLID, 1, WOOD_BORDER_LIGHT);
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
	SetTextColor(memory, PARCHMENT);
	SelectObject(memory, data.titleFont);
	RECT titleRect{ Dip(data, 35), Dip(data, 14), client.right - Dip(data, 78), Dip(data, 40) };
	DrawTextW(memory, L"DLSS5 多次滤镜", -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	SetTextColor(memory, PARCHMENT_MUTED);
	SelectObject(memory, data.statusFont);
	RECT statusRect{ Dip(data, 19), Dip(data, 42), client.right - Dip(data, 18), Dip(data, 61) };
	const std::wstring status = StatusText(data);
	DrawTextW(memory, status.c_str(), -1, &statusRect,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

	SetTextColor(memory, data.phase == Phase::Running ? GOLD : PARCHMENT_MUTED);
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
		buttonColor = WOOD_TRACK;
	} else if (data.phase == Phase::Running || data.phase == Phase::Starting) {
		buttonColor = data.buttonPressed ? CLAY_PRESSED :
			(data.buttonHover ? CLAY_HOVER : CLAY);
	} else {
		buttonColor = data.buttonPressed ? GOLD_PRESSED :
			(data.buttonHover ? GOLD_HOVER : GOLD);
	}
	FillPanel(memory, button, buttonColor,
		data.phase == Phase::Running || data.phase == Phase::Starting ? CLAY_HOVER : GOLD_HOVER,
		data.phase == Phase::Running || data.phase == Phase::Starting ? CLAY_PRESSED : GOLD_DARK);

	SetTextColor(memory,
		data.phase == Phase::Running || data.phase == Phase::Starting ? PARCHMENT : WOOD_BORDER_DARK);
	SelectObject(memory, data.buttonFont);
	const std::wstring buttonText = ButtonText(data);
	DrawTextW(memory, buttonText.c_str(), -1, &button,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	RECT settings = SettingsButtonRect(data);
	if (data.settingsHover || data.settingsPressed || data.settingsExpanded) {
		FillPanel(memory, settings,
			data.settingsPressed ? WOOD_TRACK : WOOD_PANEL_DARK);
	}
	HPEN settingsPen = CreatePen(PS_SOLID, std::max(1, Dip(data, 1)),
		data.settingsExpanded ? GOLD : PARCHMENT_MUTED);
	oldPen = SelectObject(memory, settingsPen);
	const int lineLeft = settings.left + Dip(data, 7);
	const int lineRight = settings.right - Dip(data, 7);
	for (int index = 0; index < 3; ++index) {
		const int y = settings.top + Dip(data, 8 + index * 5);
		MoveToEx(memory, lineLeft, y, nullptr);
		LineTo(memory, lineRight, y);
		const int knobX = index == 1 ? lineLeft + Dip(data, 3) : lineRight - Dip(data, 4);
		HBRUSH knob = CreateSolidBrush(
			data.settingsExpanded ? GOLD : PARCHMENT_MUTED);
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
		FillPanel(memory, close,
			data.closePressed ? CLAY_PRESSED : WOOD_PANEL_DARK);
	}
	HPEN closePen = CreatePen(PS_SOLID, std::max(1, Dip(data, 1)), PARCHMENT_MUTED);
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
	if (data.settingsExpanded && IsCapturingHotkey(data)) {
		CancelHotkeyCapture(data);
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
	else if (index == 3) {
		data.uiSettings.opacity = std::clamp(value, 40, 100);
		ApplyWindowOpacity(data);
	}
	InvalidateRect(data.hwnd, nullptr, FALSE);
}

bool HandleSettingsPress(ControllerData& data, POINT point) {
	if (!data.settingsExpanded || data.phase != Phase::Idle) return false;
	if (data.settingsPage != 2 && PointIn(ResetRect(data), point)) {
		UnregisterControllerHotkeys(data);
		data.hotkeyCapture = HotkeyCapture::None;
		data.capturedHotkeyModifiers = 0;
		data.settings = {};
		data.uiSettings = {};
		if (!GuidanceModeAvailable(data, data.settings.guidanceMode)) {
			data.settings.guidanceMode = 1;
		}
		ApplyUiPalette(data.uiSettings.theme);
		ApplyWindowOpacity(data);
		SaveSettings(data);
		data.targetTitle = RegisterControllerHotkeys(data)
			? L"已恢复默认设置" : L"一个或多个默认快捷键被其他程序占用";
		InvalidateRect(data.hwnd, nullptr, FALSE);
		return true;
	}
	for (int page = 0; page < 3; ++page) {
		if (!PointIn(SettingsTabRect(data, page), point)) continue;
		if (IsCapturingHotkey(data)) CancelHotkeyCapture(data);
		data.settingsPage = page;
		data.activeSlider = -1;
		InvalidateRect(data.hwnd, nullptr, FALSE);
		return true;
	}
	if (data.settingsPage == 2) {
		if (PointIn(AboutAutoUpdateRect(data), point)) {
			data.uiSettings.autoCheckUpdates = !data.uiSettings.autoCheckUpdates;
			SaveSettings(data);
			InvalidateRect(data.hwnd, nullptr, FALSE);
			return true;
		}
		if (PointIn(AboutUpdateActionRect(data), point)) {
			if (data.updateState == UpdateState::Available) {
				StartUpdateDownload(data);
			} else if (data.updateState != UpdateState::Checking &&
				data.updateState != UpdateState::Downloading) {
				StartUpdateCheck(data);
			}
			return true;
		}
		if (PointIn(AboutSourceLinkRect(data), point)) {
			ShellExecuteW(data.hwnd, L"open", GITHUB_REPOSITORY_URL,
				nullptr, nullptr, SW_SHOWNORMAL);
			return true;
		}
		return point.y >= Dip(data, 76) && point.y < ButtonRect(data).top;
	}
	if (data.settingsPage == 1) {
		if (PointIn(HotkeyRect(data), point)) {
			BeginHotkeyCapture(data, HotkeyCapture::FilterToggle);
			return true;
		}
		if (PointIn(VisibilityHotkeyRect(data), point)) {
			BeginHotkeyCapture(data, HotkeyCapture::WindowVisibility);
			return true;
		}
		if (PointIn(UiThemeRect(data), point)) {
			const RECT rect = UiThemeRect(data);
			const int rectWidth = std::max(1, static_cast<int>(rect.right - rect.left));
			data.uiSettings.theme = std::clamp(
				static_cast<int>(point.x - rect.left) * UI_THEME_COUNT / rectWidth,
				0, UI_THEME_COUNT - 1);
			ApplyUiPalette(data.uiSettings.theme);
			SaveSettings(data);
			InvalidateRect(data.hwnd, nullptr, FALSE);
			return true;
		}
		if (PointIn(OpacitySliderHitRect(data), point)) {
			data.activeSlider = 3;
			UpdateSliderFromPoint(data, 3, point);
			SetCapture(data.hwnd);
			return true;
		}
		return point.y >= Dip(data, 76) && point.y < ButtonRect(data).top;
	}
	if (data.settings.backend == 1) {
		if (PointIn(SliderHitRect(data, 0), point)) {
			data.activeSlider = 0;
			UpdateSliderFromPoint(data, 0, point);
			SetCapture(data.hwnd);
			return true;
		}
		if (PointIn(BackendRect(data), point)) {
			const RECT rect = BackendRect(data);
			const int rectWidth = std::max(1, static_cast<int>(rect.right - rect.left));
			data.settings.backend = std::clamp(
				static_cast<int>(point.x - rect.left) * 2 / rectWidth, 0, 1);
			data.targetTitle.clear();
			SaveSettings(data);
			InvalidateRect(data.hwnd, nullptr, FALSE);
			return true;
		}
		// The remaining controls belong to the experimental temporal backend.
		return point.y >= Dip(data, 76) && point.y < ButtonRect(data).top;
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
	if (PointIn(HistoryModeRect(data), point)) {
		const RECT rect = HistoryModeRect(data);
		const int rectWidth = std::max(1, static_cast<int>(rect.right - rect.left));
		data.settings.historyMode = std::clamp(
			static_cast<int>(point.x - rect.left) * 3 / rectWidth, 0, 2);
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
	if (PointIn(GuidanceRect(data), point)) {
		const RECT rect = GuidanceRect(data);
		const int rectWidth = std::max(1, static_cast<int>(rect.right - rect.left));
		const int mode = std::clamp(
			static_cast<int>(point.x - rect.left) * 4 / rectWidth, 0, 3);
		if (!GuidanceModeAvailable(data, mode)) {
			data.targetTitle = mode == 3
				? L"当前便携包未包含深度模型，不能启用深度引导"
				: L"当前便携包或显卡驱动不支持这个帧引导模式";
		} else {
			data.settings.guidanceMode = mode;
			data.targetTitle.clear();
			SaveSettings(data);
		}
		InvalidateRect(data.hwnd, nullptr, FALSE);
		return true;
	}
	if (PointIn(DepthIntervalRect(data), point)) {
		if (!data.depthGuidanceAvailable) {
			data.targetTitle = L"深度模型未安装，更新间隔暂不可调";
		} else {
			constexpr int values[] = { 1, 2, 4, 8 };
			const RECT rect = DepthIntervalRect(data);
			const int rectWidth = std::max(1, static_cast<int>(rect.right - rect.left));
			const int index = std::clamp(
				static_cast<int>(point.x - rect.left) * 4 / rectWidth, 0, 3);
			data.settings.depthInferenceInterval = values[index];
			data.targetTitle.clear();
			SaveSettings(data);
		}
		InvalidateRect(data.hwnd, nullptr, FALSE);
		return true;
	}
	if (PointIn(BackendRect(data), point)) {
		const RECT rect = BackendRect(data);
		const int rectWidth = std::max(1, static_cast<int>(rect.right - rect.left));
		data.settings.backend = std::clamp(
			static_cast<int>(point.x - rect.left) * 2 / rectWidth, 0, 1);
		data.targetTitle.clear();
		SaveSettings(data);
		InvalidateRect(data.hwnd, nullptr, FALSE);
		return true;
	}
	if (PointIn(ProcessingResolutionRect(data), point)) {
		constexpr int values[] = { 100, 75, 67, 50, 33, 25 };
		const RECT rect = ProcessingResolutionRect(data);
		const int rectWidth = std::max(1, static_cast<int>(rect.right - rect.left));
		constexpr int valueCount = static_cast<int>(std::size(values));
		const int index = std::clamp(
			static_cast<int>(point.x - rect.left) * valueCount / rectWidth,
			0, valueCount - 1);
		data.settings.processingResolutionPercent = values[index];
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
		data.stopEventName + L"\" --backend " + std::to_wstring(data.settings.backend) +
		L" --processing-resolution " +
			std::to_wstring(data.settings.processingResolutionPercent) +
		L" --style " + std::to_wstring(data.settings.style) +
		L" --intensity " + std::to_wstring(data.settings.intensity / 100.0f) +
		L" --local-tone " + std::to_wstring(data.settings.localTone / 100.0f) +
		L" --local-structure " + std::to_wstring(data.settings.localStructure / 100.0f) +
		L" --auto-mask " + std::to_wstring(data.settings.autoMask ? 1 : 0) +
		L" --history-mode " + std::to_wstring(data.settings.historyMode) +
		L" --passes " + std::to_wstring(data.settings.passes) +
		L" --guidance " + std::to_wstring(data.settings.guidanceMode) +
		L" --depth-interval " + std::to_wstring(data.settings.depthInferenceInterval) +
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

void ToggleControllerVisibility(ControllerData& data) {
	if (IsWindowVisible(data.hwnd)) {
		ShowWindow(data.hwnd, SW_HIDE);
		return;
	}
	ShowWindow(data.hwnd, SW_SHOWNOACTIVATE);
	SetWindowPos(data.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	InvalidateRect(data.hwnd, nullptr, FALSE);
}

void ShutdownController(ControllerData& data) {
	KillTimer(data.hwnd, TIMER_SELECT);
	KillTimer(data.hwnd, TIMER_POLL);
	UnregisterControllerHotkeys(data);
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
		DetectGuidanceCapabilities(*data);
		LoadSettings(*data);
		ApplyWindowOpacity(*data);
		RecreateFonts(*data);
		if (!RegisterControllerHotkeys(*data)) {
			data->targetTitle = L"一个或多个快捷键被占用，请在设置中更换";
		}
		SetTimer(hwnd, TIMER_POLL, 100, nullptr);
		if (data->uiSettings.autoCheckUpdates) StartUpdateCheck(*data, true);
		return 0;
	case WM_UPDATE_WORKER_RESULT:
	{
		std::unique_ptr<UpdateWorkerResult> result(
			reinterpret_cast<UpdateWorkerResult*>(lParam));
		if (!result) return 0;
		if (result->download) {
			if (!result->success) {
				data->updateState = UpdateState::Failed;
				data->updateMessage = result->message;
			} else {
				std::wstring installError;
				if (LaunchSelfUpdate(result->downloadedPath, installError)) {
					DestroyWindow(hwnd);
					return 0;
				}
				DeleteFileW(result->downloadedPath.c_str());
				data->updateState = UpdateState::Failed;
				data->updateMessage = std::move(installError);
			}
		} else if (!result->success) {
			data->updateState = UpdateState::Failed;
			data->updateMessage = result->message;
		} else if (result->updateAvailable) {
			data->updateState = UpdateState::Available;
			data->updateVersion = std::move(result->version);
			data->updateReleaseUrl = std::move(result->releaseUrl);
			data->updateAssetUrl = std::move(result->assetUrl);
			data->updateHashUrl = std::move(result->hashUrl);
			data->updateMessage.clear();
			if (data->automaticUpdateCheck &&
				!data->updateAssetUrl.empty() && !data->updateHashUrl.empty()) {
				StartUpdateDownload(*data);
			}
		} else {
			data->updateState = UpdateState::Current;
			data->updateMessage = L"已是最新版";
		}
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}
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
		if (IsCapturingHotkey(*data)) {
			const RECT captureRect = data->hotkeyCapture == HotkeyCapture::WindowVisibility
				? VisibilityHotkeyRect(*data) : HotkeyRect(*data);
			if (!PointIn(captureRect, point)) CancelHotkeyCapture(*data);
		}
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
	case WM_SYSKEYDOWN:
		if (IsCapturingHotkey(*data)) {
			CommitHotkeyCapture(*data, static_cast<UINT>(wParam));
		} else if (wParam == VK_SPACE || wParam == VK_RETURN) ActivateButton(*data);
		else if (wParam == VK_ESCAPE) {
			if (data->phase == Phase::Selecting) CancelSelection(*data);
			else if (data->phase == Phase::Running || data->phase == Phase::Starting) StopEngine(*data);
		}
		return 0;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		if (IsCapturingHotkey(*data) && IsModifierKey(static_cast<UINT>(wParam))) {
			data->capturedHotkeyModifiers &= ~ModifierFlagForKey(static_cast<UINT>(wParam));
		}
		return 0;
	case WM_HOTKEY:
		if (wParam == TOGGLE_HOTKEY_ID && !IsCapturingHotkey(*data)) {
			ActivateButton(*data);
			return 0;
		}
		if (wParam == VISIBILITY_HOTKEY_ID && !IsCapturingHotkey(*data)) {
			ToggleControllerVisibility(*data);
			return 0;
		}
		break;
	case WM_ACTIVATE:
		if (LOWORD(wParam) == WA_INACTIVE && IsCapturingHotkey(*data)) {
			CancelHotkeyCapture(*data);
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
	// Keep the window frame and Dip() layout on the same monitor-specific DPI.
	// Without an explicit awareness context Windows can virtualize the initial
	// CreateWindowEx size while GetDpiForWindow returns the physical monitor DPI,
	// which clips the right and bottom edges on mixed-scale desktops.
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	const bool qaInstance = commandLine && wcsstr(commandLine, L"--qa-instance") != nullptr;
	HANDLE instanceMutex = CreateMutexW(
		nullptr, TRUE, qaInstance ? INSTANCE_MUTEX_QA : INSTANCE_MUTEX);
	if (!qaInstance && instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
		if (HWND existing = FindWindowW(WINDOW_CLASS, nullptr)) {
			ShowWindow(existing, SW_SHOWNORMAL);
			SetForegroundWindow(existing);
		}
		CloseHandle(instanceMutex);
		return 0;
	}
	std::wstring componentUpdateError;
	if (!EnsureCompanionComponents(qaInstance, componentUpdateError)) {
		MessageBoxW(nullptr,
			(L"控制器已启动，但渲染组件自动更新失败。\n\n" + componentUpdateError +
				L"\n\n旧组件已保留；可稍后重新启动重试。").c_str(),
			L"DLSS5 任意窗口自动更新", MB_OK | MB_ICONWARNING | MB_TOPMOST);
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
		WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
		WINDOW_CLASS, L"DLSS5 多次滤镜", WS_POPUP,
		x, y, width, height, nullptr, nullptr, instance, &data);
	if (!hwnd) {
		if (instanceMutex) CloseHandle(instanceMutex);
		return 1;
	}
	// WM_CREATE now knows the window's actual monitor DPI. Re-apply the
	// logical dimensions once so the outer size matches that DPI even when
	// the pre-create monitor query was virtualized by Windows.
	ResizeController(data, false);

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
