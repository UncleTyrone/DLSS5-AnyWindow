#include "pch.h"
#include "DLSSNRNvapi.h"
#include "DLSSNRHybridNative.h"
#include "Logger.h"
#include <atomic>
#include <cstring>
#include <filesystem>

namespace Magpie::DLSSNRNvapi {
namespace {

using QueryFn = void*(__cdecl*)(unsigned);
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);

std::atomic<unsigned> g_interval{1};
std::atomic<QueryFn> g_query{nullptr};
std::atomic<GetProcAddressFn> g_originalGetProcAddress{nullptr};
std::atomic<void**> g_getProcAddressSlot{nullptr};
std::atomic<bool> g_loggedHybrid{false};
std::atomic<bool> g_loggedVitOff{false};

// 310.8 resolves nvapi_QueryInterface once and keeps the pointer. Patching the import
// afterwards never sees CreateCuFunction. Replace that cached pointer when it is already set.
void* __cdecl WrappedQueryInterface(unsigned id);
void RedirectCachedQuery(HMODULE module) noexcept {
	if (!module || g_query.load(std::memory_order_acquire)) return;
	auto* base = reinterpret_cast<uint8_t*>(module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return;
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return;
	const auto* section = IMAGE_FIRST_SECTION(nt);
	const uint8_t marker[] = {0xB9, 0x22, 0x6E, 0x43, 0xE2};
	for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
		if (std::memcmp(section[i].Name, ".text", 5) != 0) continue;
		uint8_t* text = base + section[i].VirtualAddress;
		const size_t size = section[i].Misc.VirtualSize;
		for (size_t n = 0; n + sizeof(marker) < size; ++n) {
			if (std::memcmp(text + n, marker, sizeof(marker)) != 0) continue;
			for (size_t back = 7; back < 40 && back <= n; ++back) {
				uint8_t* insn = text + n - back;
				if (insn[0] != 0x48 || insn[1] != 0x8B || insn[2] != 0x05) continue;
				const int32_t disp = *reinterpret_cast<const int32_t*>(insn + 3);
				auto** slot = reinterpret_cast<void**>(insn + 7 + disp);
				void* current = nullptr;
				if (!ReadProcessMemory(GetCurrentProcess(), slot, &current, sizeof(current), nullptr) || !current) break;
				if (current == reinterpret_cast<void*>(&WrappedQueryInterface)) return;
				g_query.store(reinterpret_cast<QueryFn>(current), std::memory_order_release);
				DWORD oldProtection = 0;
				if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection)) return;
				InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(&WrappedQueryInterface));
				DWORD ignored = 0;
				VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
				FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
				Logger::Get().Info("DLSSNR NVFP4 hybrid attached to the runtime's cached NvAPI lookup.");
				return;
			}
		}
	}
}

void** FindGetProcAddressSlot(HMODULE module) noexcept {
	if (!module) return nullptr;
	auto* base = reinterpret_cast<uint8_t*>(module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
	const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!directory.VirtualAddress || !directory.Size) return nullptr;
	auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
	const auto* descriptorEnd = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
		base + directory.VirtualAddress + directory.Size);
	for (; descriptor < descriptorEnd && descriptor->Name; ++descriptor) {
		if (descriptor->Name >= nt->OptionalHeader.SizeOfImage) continue;
		const char* libraryName = reinterpret_cast<const char*>(base + descriptor->Name);
		if (_stricmp(libraryName, "KERNEL32.dll") != 0 &&
			_stricmp(libraryName, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0 &&
			_stricmp(libraryName, "api-ms-win-core-libraryloader-l1-1-0.dll") != 0) {
			continue;
		}
		if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) continue;
		auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
		auto* addressThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
		for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addressThunk) {
			if (IMAGE_SNAP_BY_ORDINAL64(nameThunk->u1.Ordinal)) continue;
			const uint32_t nameRva = static_cast<uint32_t>(nameThunk->u1.AddressOfData);
			if (nameRva >= nt->OptionalHeader.SizeOfImage) return nullptr;
			const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + nameRva);
			if (std::strcmp(reinterpret_cast<const char*>(import->Name), "GetProcAddress") == 0) {
				return reinterpret_cast<void**>(&addressThunk->u1.Function);
			}
		}
	}
	return nullptr;
}

void* __cdecl WrappedQueryInterface(unsigned id) {
	const auto query = g_query.load(std::memory_order_acquire);
	void* raw = query ? query(id) : nullptr;
	return DlssNrNative::WrapNvapi(id, raw);
}

FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR name) {
	const auto original = g_originalGetProcAddress.load(std::memory_order_acquire);
	if (!original) {
		SetLastError(ERROR_INVALID_FUNCTION);
		return nullptr;
	}
	const FARPROC result = original(module, name);
	if (name && std::strcmp(name, "nvapi_QueryInterface") == 0 && result) {
		g_query.store(reinterpret_cast<QueryFn>(result), std::memory_order_release);
		return reinterpret_cast<FARPROC>(&WrappedQueryInterface);
	}
	return result;
}

} // namespace

bool Install(HMODULE snippet) noexcept {
	RedirectCachedQuery(snippet);
	if (g_getProcAddressSlot.load(std::memory_order_acquire)) return true;
	void** slot = FindGetProcAddressSlot(snippet);
	if (!slot) {
		Logger::Get().Warn("DLSSNR ViT reuse is unavailable because GetProcAddress is not imported");
		return true;
	}
	DWORD oldProtection = 0;
	if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection)) {
		Logger::Get().Warn("DLSSNR ViT reuse is unavailable because the import table could not be patched");
		return true;
	}
	void* original = InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(&HookGetProcAddress));
	GetProcAddressFn originalFunction = nullptr;
	std::memcpy(&originalFunction, &original, sizeof(original));
	g_originalGetProcAddress.store(originalFunction, std::memory_order_release);
	g_getProcAddressSlot.store(slot, std::memory_order_release);
	DWORD ignored = 0;
	VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
	FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
	if (!originalFunction) {
		Logger::Get().Warn("DLSSNR ViT reuse is unavailable because GetProcAddress was null");
		return true;
	}
	Logger::Get().Info("DLSSNR ViT reuse can drop the bottleneck on alternate frames. Interval 1 leaves it unchanged.");
	return true;
}

void Restore(HMODULE snippet) noexcept {
	void** slot = g_getProcAddressSlot.load(std::memory_order_acquire);
	const auto original = g_originalGetProcAddress.load(std::memory_order_acquire);
	if (!slot || !original || !snippet) return;
	DWORD oldProtection = 0;
	if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection)) return;
	InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(original));
	DWORD ignored = 0;
	VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
	FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
	g_getProcAddressSlot.store(nullptr, std::memory_order_release);
	g_originalGetProcAddress.store(nullptr, std::memory_order_release);
}

void SetReuseInterval(unsigned interval) noexcept {
	if (interval < 1) interval = 1;
	if (interval > 2) interval = 2;
	g_interval.store(interval, std::memory_order_release);
}

bool IsBlackwellGpu() noexcept {
	const HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!cuda) return false;
	using CuInit = int(__stdcall*)(unsigned);
	using CuGet = int(__stdcall*)(int*, int);
	using CuAttr = int(__stdcall*)(int*, int, int);
	const auto cuInit = reinterpret_cast<CuInit>(GetProcAddress(cuda, "cuInit"));
	const auto cuDeviceGet = reinterpret_cast<CuGet>(GetProcAddress(cuda, "cuDeviceGet"));
	const auto cuAttr = reinterpret_cast<CuAttr>(GetProcAddress(cuda, "cuDeviceGetAttribute"));
	int device = 0;
	int major = 0;
	const bool ok = cuInit && cuDeviceGet && cuAttr &&
		cuInit(0) == 0 && cuDeviceGet(&device, 0) == 0 &&
		cuAttr(&major, 75, device) == 0 && major >= 12;
	FreeLibrary(cuda);
	return ok;
}

void BeginEvaluate(const void* feature, bool reset, long long frameSlot, ID3D12GraphicsCommandList* cmd) noexcept {
	try {
		const unsigned interval = g_interval.load(std::memory_order_acquire);
		// The janblade census samples 3 of every 240 evaluations and names the kernels that ran.
		DlssNrNative::BeginEvaluate(feature, reset, interval, interval, frameSlot, cmd, true);
	} catch (...) {
		g_interval.store(1, std::memory_order_release);
	}
}

void EndEvaluate(ID3D12GraphicsCommandList* cmd) noexcept {
	try {
		static std::atomic<bool> loggedActive{false};
		static int loggedStatus = 0;
		static std::string lastStatus;
		if (DlssNrNative::EndEvaluate(cmd) && !g_loggedVitOff.exchange(true)) {
			Logger::Get().Warn(
				"DLSSNR ViT reuse is off for this session because the kernel order was not the expected one");
		}
		const std::string status = DlssNrNative::Status();
		const bool hybrid = status.find("Candidate hybrid") != std::string::npos ||
			status.find("Fused hybrid") != std::string::npos ||
			status.find("Hybrid FFN") != std::string::npos ||
			status.find("Hybrid device") != std::string::npos ||
			status.find("fallback") != std::string::npos ||
			status.find("Restart required") != std::string::npos;
		if (hybrid && status != lastStatus && loggedStatus < 24) {
			lastStatus = status;
			++loggedStatus;
			const bool failed = status.find("fallback") != std::string::npos ||
				status.find("Restart required") != std::string::npos;
			if (failed) {
				Logger::Get().Warn(std::string("DLSSNR NVFP4 hybrid stayed on FP8. ") + status);
			} else {
				Logger::Get().Info(std::string("DLSSNR NVFP4 hybrid: ") + status);
			}
		}
		if (DlssNrNative::IsActive() && !loggedActive.exchange(true)) {
			Logger::Get().Info(std::string("DLSSNR NVFP4 hybrid is active. ") + status);
		}
		for (const std::string& report : DlssNrNative::TakeProfileReports()) {
			Logger::Get().Info(report);
		}
	} catch (...) {
		g_interval.store(1, std::memory_order_release);
	}
}

void NoteHybridRequest(bool requested) noexcept {
	if (!requested) {
		DlssNrNative::SetEnabled(false);
		return;
	}
	if (g_loggedHybrid.exchange(true)) {
		DlssNrNative::SetEnabled(true);
		return;
	}
	if (!IsBlackwellGpu()) {
		Logger::Get().Warn("DLSSNR NVFP4 was requested on a GPU that is not RTX 50, so the model stays on FP8.");
		return;
	}
	wchar_t exePath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	const auto assetDir = std::filesystem::path(exePath).parent_path() / L"DLSSNR-Hybrid" / L"weights.bin";
	if (!std::filesystem::exists(assetDir)) {
		Logger::Get().Warn(
			"DLSSNR NVFP4 was requested, but DLSSNR-Hybrid\\weights.bin is not beside Magpie.exe. The model stays on FP8.");
		return;
	}
	DlssNrNative::SetEnabled(true);
	Logger::Get().Info(
		"DLSSNR NVFP4 hybrid files are loaded. The first supported frames may still run FP8 while the fused kernels start.");
}

} // namespace Magpie::DLSSNRNvapi
