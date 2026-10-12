#include "pch.h"
#include "DLSSGAdaUnlock.h"
#include "Logger.h"

#include <vector>
#include <string>
#include <cstring>

namespace Magpie::DLSSGAdaUnlock {
namespace {

constexpr std::string_view kAdvertise309 = "81 FD B0 01 00 00 0F 8C ?? ?? ?? ?? BF 05 00 00 00";
constexpr std::string_view kValidate309 = "3D B0 01 00 00 0F 93 C0";
constexpr std::string_view kAdvertiseLegacy =
	"BB 01 00 00 00 41 B8 03 00 00 00 81 FF B0 01 00 00 44 0F 4C C3";
constexpr std::string_view kValidateLegacy = "3D B0 01 00 00 7C ?? 83 FB 03 76";

struct Patch {
	uint8_t* address = nullptr;
	std::vector<uint8_t> original;
	std::vector<uint8_t> replacement;
};

bool IsWildcardToken(std::string_view token) noexcept {
	return token == "??";
}

std::vector<int> ParsePattern(std::string_view text) {
	std::vector<int> bytes;
	size_t i = 0;
	while (i < text.size()) {
		while (i < text.size() && text[i] == ' ') ++i;
		if (i >= text.size()) break;
		const size_t end = text.find(' ', i);
		const std::string_view token = text.substr(i, end == std::string_view::npos ? text.size() - i : end - i);
		bytes.push_back(IsWildcardToken(token) ? -1 : static_cast<int>(std::strtoul(std::string(token).c_str(), nullptr, 16)));
		i = end == std::string_view::npos ? text.size() : end + 1;
	}
	return bytes;
}

std::vector<uint8_t*> FindPattern(HMODULE module, std::string_view pattern) {
	std::vector<uint8_t*> found;
	const auto bytes = ParsePattern(pattern);
	if (bytes.empty()) return found;
	auto* base = reinterpret_cast<uint8_t*>(module);
	const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return found;
	const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return found;
	const auto* section = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
		if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
		uint8_t* start = base + section->VirtualAddress;
		const size_t size = section->Misc.VirtualSize;
		if (size < bytes.size()) continue;
		for (size_t off = 0; off + bytes.size() <= size; ++off) {
			bool match = true;
			for (size_t b = 0; b < bytes.size(); ++b) {
				if (bytes[b] >= 0 && start[off + b] != static_cast<uint8_t>(bytes[b])) {
					match = false;
					break;
				}
			}
			if (match) found.push_back(start + off);
		}
	}
	return found;
}

bool WritePatch(Patch& patch) noexcept {
	DWORD old = 0;
	if (!VirtualProtect(patch.address, patch.replacement.size(), PAGE_EXECUTE_READWRITE, &old)) {
		return false;
	}
	std::memcpy(patch.address, patch.replacement.data(), patch.replacement.size());
	DWORD ignored = 0;
	VirtualProtect(patch.address, patch.replacement.size(), old, &ignored);
	FlushInstructionCache(GetCurrentProcess(), patch.address, patch.replacement.size());
	return true;
}

void Rollback(std::vector<Patch>& applied) noexcept {
	for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
		DWORD old = 0;
		if (VirtualProtect(it->address, it->original.size(), PAGE_EXECUTE_READWRITE, &old)) {
			std::memcpy(it->address, it->original.data(), it->original.size());
			DWORD ignored = 0;
			VirtualProtect(it->address, it->original.size(), old, &ignored);
			FlushInstructionCache(GetCurrentProcess(), it->address, it->original.size());
		}
	}
}

bool AddBytes(std::vector<Patch>& plan, uint8_t* address, const uint8_t* bytes, size_t size) {
	Patch patch;
	patch.address = address;
	patch.original.assign(address, address + size);
	patch.replacement.assign(bytes, bytes + size);
	plan.push_back(std::move(patch));
	return true;
}

bool ApplyPlan(std::vector<Patch>& plan) noexcept {
	std::vector<Patch> applied;
	for (auto& patch : plan) {
		if (!WritePatch(patch)) {
			Rollback(applied);
			return false;
		}
		applied.push_back(patch);
	}
	return true;
}

std::wstring ExeDirectory() {
	wchar_t path[MAX_PATH]{};
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::wstring text(path);
	const auto slash = text.find_last_of(L"\\/");
	return slash == std::wstring::npos ? L"." : text.substr(0, slash);
}

std::wstring UnlockMode() {
	wchar_t value[32]{};
	const DWORD n = GetEnvironmentVariableW(L"MAGPIE_DLSSG_ADA_UNLOCK", value, 32);
	return n > 0 && n < 32 ? std::wstring(value) : std::wstring();
}

void LogScan(HMODULE module) noexcept {
	const auto a309 = FindPattern(module, kAdvertise309).size();
	const auto v309 = FindPattern(module, kValidate309).size();
	const auto aLegacy = FindPattern(module, kAdvertiseLegacy).size();
	const auto vLegacy = FindPattern(module, kValidateLegacy).size();
	Logger::Get().Info(fmt::format(
		"DLSSG Ada unlock scan: advertise309={} validate309={} advertiseLegacy={} validateLegacy={}. "
		"Driver profile keys 0x104D6667 and 0x10562D0F must stay 0 or a game profile can clamp multi-frame generation.",
		a309, v309, aLegacy, vLegacy));
}

bool BuildGatePlan(HMODULE module, std::vector<Patch>& plan) {
	const auto a309 = FindPattern(module, kAdvertise309);
	const auto v309 = FindPattern(module, kValidate309);
	const auto aLegacy = FindPattern(module, kAdvertiseLegacy);
	const auto vLegacy = FindPattern(module, kValidateLegacy);
	if (a309.size() == 1 && v309.size() == 1 && aLegacy.empty() && vLegacy.empty()) {
		const uint8_t nopJl[] = { 0x0F, 0x1F, 0x44, 0x00, 0x00, 0x90 };
		const uint8_t always[] = { 0xB0, 0x01, 0x90 };
		AddBytes(plan, a309[0] + 6, nopJl, sizeof(nopJl));
		AddBytes(plan, v309[0] + 5, always, sizeof(always));
		return true;
	}
	if (aLegacy.size() == 1 && vLegacy.size() == 1 && a309.empty() && v309.empty()) {
		const uint8_t count[] = { 5 };
		const uint8_t nopCmov[] = { 0x0F, 0x1F, 0x40, 0x00 };
		const uint8_t nopJl[] = { 0x90, 0x90 };
		AddBytes(plan, aLegacy[0] + 7, count, 1);
		AddBytes(plan, aLegacy[0] + 17, nopCmov, sizeof(nopCmov));
		AddBytes(plan, vLegacy[0] + 5, nopJl, sizeof(nopJl));
		AddBytes(plan, vLegacy[0] + 9, count, 1);
		return true;
	}
	return false;
}

// Retarget: one .target sm_120 in each fatbin becomes sm_89, Ada images park at arch 122.
bool BuildRetargetPlan(HMODULE module, std::vector<Patch>& plan, unsigned& containers) {
	auto* base = reinterpret_cast<uint8_t*>(module);
	const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
	const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	const auto* section = IMAGE_FIRST_SECTION(nt);
	const uint8_t magic[] = { 0x50, 0xED, 0x55, 0xBA };
	containers = 0;
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
		if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
		uint8_t* start = base + section->VirtualAddress;
		uint8_t* end = start + section->Misc.VirtualSize;
		for (uint8_t* cursor = start; cursor + 16 < end;) {
			auto* container = std::search(cursor, end, magic, magic + 4);
			if (container == end) break;
			cursor = container + 1;
			if (container + 16 > end) return false;
			const uint16_t headerSize = *reinterpret_cast<uint16_t*>(container + 6);
			const uint64_t fatSize = *reinterpret_cast<uint64_t*>(container + 8);
			if (headerSize != 0x10 || fatSize == 0 || container + 16 + fatSize > end) continue;
			uint8_t* containerEnd = container + 16 + static_cast<size_t>(fatSize);
			uint8_t* blackwell = nullptr;
			std::vector<uint8_t*> ada;
			for (uint8_t* image = container + 16; image + 32 < containerEnd;) {
				const uint16_t kind = *reinterpret_cast<uint16_t*>(image);
				const uint32_t imageHeader = *reinterpret_cast<uint32_t*>(image + 4);
				const uint64_t payload = *reinterpret_cast<uint64_t*>(image + 8);
				const uint32_t arch = *reinterpret_cast<uint32_t*>(image + 28);
				if (imageHeader < 32 || image + imageHeader + payload > containerEnd) break;
				if (kind == 1 && arch == 120) {
					if (blackwell) return false;
					blackwell = image;
				} else if (arch == 89) {
					ada.push_back(image);
				}
				image += imageHeader + static_cast<size_t>(payload);
			}
			if (!blackwell || ada.empty()) {
				cursor = containerEnd;
				continue;
			}
			const char from[] = ".target sm_120";
			const char to[] = ".target sm_89 ";
			uint8_t* body = blackwell + *reinterpret_cast<uint32_t*>(blackwell + 4);
			const size_t payload = static_cast<size_t>(*reinterpret_cast<uint64_t*>(blackwell + 8));
			auto* target = std::search(body, body + payload, from, from + sizeof(from) - 1);
			if (target == body + payload) {
				cursor = containerEnd;
				continue;
			}
			std::vector<uint8_t> patched(container, containerEnd);
			std::memcpy(patched.data() + (target - container), to, sizeof(to) - 1);
			const uint32_t adaArch = 89;
			const uint32_t parked = 122;
			std::memcpy(patched.data() + (blackwell + 28 - container), &adaArch, sizeof(adaArch));
			for (auto* image : ada) {
				std::memcpy(patched.data() + (image + 28 - container), &parked, sizeof(parked));
			}
			AddBytes(plan, container, patched.data(), patched.size());
			++containers;
			cursor = containerEnd;
		}
	}
	return containers > 0;
}

} // namespace

struct ComputeCapability {
	int major = 0;
	int minor = 0;
	bool valid = false;
};

ComputeCapability QueryComputeCapability() noexcept {
	ComputeCapability result{};
	const HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!cuda) return result;
	using CuInit = int(__stdcall*)(unsigned);
	using CuGet = int(__stdcall*)(int*, int);
	using CuAttr = int(__stdcall*)(int*, int, int);
	const auto cuInit = reinterpret_cast<CuInit>(GetProcAddress(cuda, "cuInit"));
	const auto cuDeviceGet = reinterpret_cast<CuGet>(GetProcAddress(cuda, "cuDeviceGet"));
	const auto cuAttr = reinterpret_cast<CuAttr>(GetProcAddress(cuda, "cuDeviceGetAttribute"));
	int device = 0;
	result.valid = cuInit && cuDeviceGet && cuAttr &&
		cuInit(0) == 0 && cuDeviceGet(&device, 0) == 0 &&
		cuAttr(&result.major, 75, device) == 0 &&
		cuAttr(&result.minor, 76, device) == 0;
	FreeLibrary(cuda);
	return result;
}

bool IsAdaGpu() noexcept {
	const ComputeCapability cc = QueryComputeCapability();
	return cc.valid && cc.major == 8 && cc.minor == 9;
}

uint32_t MaxMultiplier(bool adaTemporalFix) noexcept {
	const ComputeCapability cc = QueryComputeCapability();
	if (!cc.valid) return 4;
	if (cc.major >= 10) return 6;
	if (cc.major == 8 && cc.minor == 9 && adaTemporalFix) return 6;
	return 4;
}

bool ApplyMidpoint(HMODULE module, std::string& detail) noexcept;
bool ApplyWarpBlend(HMODULE module, std::string& detail) noexcept;

bool Prepare(TemporalFix temporal) noexcept {
	const std::wstring mode = UnlockMode();
	const bool scanOnly = mode == L"scan";
	const std::wstring dllPath = ExeDirectory() + L"\\nvngx_dlssg.dll";
	const HMODULE module = LoadLibraryExW(
		dllPath.c_str(), nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!module) {
		Logger::Get().Info("DLSSG Ada unlock: nvngx_dlssg.dll was not preloaded from the exe directory");
		return true;
	}
	LogScan(module);
	if (scanOnly || !IsAdaGpu()) {
		if (!scanOnly) {
			Logger::Get().Info("DLSSG Ada unlock: GPU is not Ada; image left unchanged");
		}
		return true;
	}

	std::vector<Patch> plan;
	if (!BuildGatePlan(module, plan)) {
		Logger::Get().Warn("DLSSG Ada unlock: gate signatures did not match exactly once; left unchanged");
		return true;
	}
	unsigned containers = 0;
	if (temporal == TemporalFix::Retarget && !BuildRetargetPlan(module, plan, containers)) {
		Logger::Get().Warn("DLSSG Ada unlock: no unambiguous Blackwell PTX to retarget; gates still applied");
	}
	if (!ApplyPlan(plan)) {
		Logger::Get().Error("DLSSG Ada unlock: patch transaction rolled back");
		return false;
	}
	if (temporal == TemporalFix::Midpoint) {
		std::string detail;
		if (!ApplyMidpoint(module, detail)) {
			Rollback(plan);
			Logger::Get().Error(fmt::format(
				"DLSSG Ada unlock: midpoint correction was not applied ({}); gates rolled back",
				detail));
			return true;
		}
		Logger::Get().Info(fmt::format("DLSSG Ada unlock: {}", detail));
		std::string quality;
		if (!ApplyWarpBlend(module, quality)) {
			Logger::Get().Warn(fmt::format(
				"DLSSG Ada quality: warp blend was not applied ({}); midpoint stays, and the offline quality kernels are not in this build",
				quality));
		} else {
			Logger::Get().Info(fmt::format(
				"DLSSG Ada quality: {}; offline quality kernels are not in this build",
				quality));
		}
	}
	Logger::Get().Info(fmt::format(
		"DLSSG Ada unlock applied (temporal={}, retarget containers={})",
		temporal == TemporalFix::Midpoint ? "midpoint" : "retarget", containers));
	return true;
}

} // namespace Magpie::DLSSGAdaUnlock
