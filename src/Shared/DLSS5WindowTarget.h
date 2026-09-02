#pragma once

#include <windows.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace Magpie::DLSS5WindowTarget {

inline constexpr wchar_t CONTROLLER_WINDOW_CLASS[] = L"DLSS5DoubleFloatingController";
inline constexpr wchar_t SCALING_WINDOW_CLASS[] =
	L"Window_Magpie_967EB565-6F73-4E94-AE53-00CC42592A22";
inline constexpr wchar_t RENDERER_WINDOW_CLASS[] =
	L"Magpie_Renderer";

struct Info {
	HWND hwnd = nullptr;
	DWORD processId = 0;
	std::wstring title;
	std::wstring className;
	std::wstring processPath;
	RECT windowRect{};
	bool isWindow = false;
	bool isVisible = false;
};

inline std::wstring ReadWindowText(HWND hwnd) {
	const int length = GetWindowTextLengthW(hwnd);
	std::wstring result(static_cast<size_t>(std::max(length, 0)) + 1, L'\0');
	const int copied = GetWindowTextW(hwnd, result.data(), static_cast<int>(result.size()));
	result.resize(static_cast<size_t>(std::max(copied, 0)));
	return result;
}

inline std::wstring ReadWindowClass(HWND hwnd) {
	wchar_t buffer[256]{};
	const int copied = GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer)));
	return copied > 0 ? std::wstring(buffer, static_cast<size_t>(copied)) : std::wstring();
}

inline std::wstring ReadProcessPath(DWORD processId) {
	if (!processId) return {};
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
	if (!process) return {};
	std::wstring result(32768, L'\0');
	DWORD size = static_cast<DWORD>(result.size());
	if (!QueryFullProcessImageNameW(process, 0, result.data(), &size)) {
		result.clear();
	} else {
		result.resize(size);
	}
	CloseHandle(process);
	return result;
}

inline Info Inspect(HWND hwnd) {
	Info result;
	result.hwnd = hwnd;
	result.isWindow = hwnd && IsWindow(hwnd);
	if (!result.isWindow) return result;
	GetWindowThreadProcessId(hwnd, &result.processId);
	result.title = ReadWindowText(hwnd);
	result.className = ReadWindowClass(hwnd);
	result.processPath = ReadProcessPath(result.processId);
	GetWindowRect(hwnd, &result.windowRect);
	result.isVisible = IsWindowVisible(hwnd) != FALSE;
	return result;
}

inline bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right) {
	return left.size() == right.size() &&
		_wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

inline std::wstring_view FileName(std::wstring_view path) {
	const size_t slash = path.find_last_of(L"\\/");
	return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
}

inline bool IsForbidden(const Info& info, DWORD callerProcessId, std::wstring& reason) {
	if (!info.isWindow) {
		reason = L"没有选中有效窗口";
		return true;
	}
	if (!info.processId) {
		reason = L"无法识别目标窗口所属进程";
		return true;
	}
	if (info.processId == callerProcessId) {
		reason = L"请选择本控制器以外的窗口";
		return true;
	}
	if (EqualsIgnoreCase(info.className, CONTROLLER_WINDOW_CLASS)) {
		reason = L"不能把 DLSS5 悬浮控制器作为目标";
		return true;
	}
	if (EqualsIgnoreCase(info.className, SCALING_WINDOW_CLASS) ||
		EqualsIgnoreCase(info.className, RENDERER_WINDOW_CLASS)) {
		reason = L"不能把正在运行的 DLSS5 输出窗口作为目标";
		return true;
	}
	const std::wstring_view fileName = FileName(info.processPath);
	if (EqualsIgnoreCase(fileName, L"DLSSNRWindowDouble.exe") ||
		EqualsIgnoreCase(fileName, L"DLSS5FloatingController.exe") ||
		EqualsIgnoreCase(fileName, L"DLSS5双次滤镜悬浮控制器.exe")) {
		reason = L"不能把 DLSS5 程序自身作为目标";
		return true;
	}
	reason.clear();
	return false;
}

} // namespace Magpie::DLSS5WindowTarget
