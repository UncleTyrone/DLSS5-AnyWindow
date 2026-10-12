// SPDX-License-Identifier: MIT
// Adapted from MFGAdaUnlock-RenoDx (mavismmg), src/addons/mfgunlock/midpoint.hpp.
// Copyright (c) 2026 mavismmg. The technique is from dashdogy's RTX40MFG-Unlock.
//
// On Ada, the DLSSG interpolation kernel blends with a compiled-in 0.5, so every
// extra frame is the same halfway picture. This decompresses that sm_89 PTX,
// feeds the kernel's own frame parameter in as the blend weight, drops the
// precompiled cubin so the driver JITs the rewritten PTX, and points every
// matching descriptor at one process-lifetime copy. The file on disk is never
// modified. A signature mismatch leaves the image untouched.

#include "pch.h"
#include "Logger.h"
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace Magpie::DLSSGAdaUnlock {
namespace {

constexpr uint32_t kFatbinMagic = 0xBA55ED50u;
constexpr size_t kOuterHeader = 16;
constexpr uint32_t kPtxKind = 1;
constexpr uint32_t kAdaArch = 89;
constexpr uint64_t kUncompressedFlags = 0x41;
constexpr size_t kExpectedMidpoints = 104;
constexpr char kJoinLabel[] = "$L__BB0_3:";
constexpr char kMidpointBits[] = "0f3F000000";
constexpr char kMulPrefix[] = "mul.ftz.f32 ";

struct TemporalProfile {
	size_t ptxBytes;
	const char* entryName;
	const char* descriptorName;
	ptrdiff_t entryNameOffset;
	ptrdiff_t descriptorNameOffset;
};

constexpr TemporalProfile kProfiles[] = {
	{99362, "main_kernel", "dlfg_kernel", 0x10, 0x28},
	{99626, "Kernel_EstimateIntermMvecsScatter", "EstimateIntermMvecsScatter", 0x10, -0x08},
};

uint16_t ReadU16(const uint8_t* p) {
	uint16_t v = 0;
	std::memcpy(&v, p, sizeof(v));
	return v;
}
uint32_t ReadU32(const uint8_t* p) {
	uint32_t v = 0;
	std::memcpy(&v, p, sizeof(v));
	return v;
}
uint64_t ReadU64(const uint8_t* p) {
	uint64_t v = 0;
	std::memcpy(&v, p, sizeof(v));
	return v;
}

bool Lz4BlockDecompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
	size_t in = 0;
	size_t out = 0;
	while (in < srcSize) {
		const uint8_t token = src[in++];
		size_t literals = token >> 4;
		if (literals == 15) {
			uint8_t ext = 0;
			do {
				if (in >= srcSize) return false;
				ext = src[in++];
				literals += ext;
			} while (ext == 0xFF);
		}
		if (literals > srcSize - in || literals > dstSize - out) return false;
		std::memcpy(dst + out, src + in, literals);
		in += literals;
		out += literals;
		if (in == srcSize) break;
		if (srcSize - in < 2) return false;
		const size_t back = static_cast<size_t>(src[in]) | (static_cast<size_t>(src[in + 1]) << 8);
		in += 2;
		if (back == 0 || back > out) return false;
		size_t match = 4 + (token & 0x0F);
		if ((token & 0x0F) == 15) {
			uint8_t ext = 0;
			do {
				if (in >= srcSize) return false;
				ext = src[in++];
				match += ext;
			} while (ext == 0xFF);
		}
		if (match > dstSize - out) return false;
		for (size_t i = 0; i < match; ++i) {
			dst[out + i] = dst[out + i - back];
		}
		out += match;
	}
	return in == srcSize && out == dstSize;
}

bool FindAdaPtxEntry(const uint8_t* fat, size_t fatSize, size_t& entryOffset) {
	if (fatSize < kOuterHeader || ReadU32(fat) != kFatbinMagic) return false;
	if (ReadU16(fat + 6) != kOuterHeader) return false;
	const uint64_t declared = ReadU64(fat + 8);
	if (declared + kOuterHeader != fatSize) return false;

	size_t p = kOuterHeader;
	while (p + 64 <= fatSize) {
		const uint32_t kind = ReadU16(fat + p);
		const uint32_t hdr = ReadU32(fat + p + 4);
		const uint64_t payload = ReadU64(fat + p + 8);
		if (hdr < 64 || payload == 0) return false;
		if (p + hdr + payload > fatSize) return false;
		if (kind == kPtxKind && ReadU32(fat + p + 28) == kAdaArch) {
			entryOffset = p;
			return true;
		}
		p += hdr + static_cast<size_t>(payload);
	}
	return false;
}

const TemporalProfile* FindTemporalProfile(const uint8_t* fat, size_t fatSize) {
	size_t entry = 0;
	if (!FindAdaPtxEntry(fat, fatSize, entry)) return nullptr;
	const uint64_t raw = ReadU64(fat + entry + 56);
	for (const TemporalProfile& profile : kProfiles) {
		if (raw == profile.ptxBytes) return &profile;
	}
	return nullptr;
}

bool PointsToCString(const uint8_t* base, size_t imageSize, uint64_t value, const char* expected) {
	const auto start = reinterpret_cast<uintptr_t>(base);
	if (value < start || value >= start + imageSize) return false;
	const size_t len = std::strlen(expected);
	if (value + len + 1 > start + imageSize) return false;
	return std::memcmp(reinterpret_cast<const char*>(value), expected, len + 1) == 0;
}

bool ReadRelativePointer(
	const uint8_t* base, size_t imageSize, const uint8_t* slot,
	ptrdiff_t displacement, uint64_t& value) {
	const uintptr_t start = reinterpret_cast<uintptr_t>(base);
	const uintptr_t end = start + imageSize;
	const uintptr_t address = reinterpret_cast<uintptr_t>(slot);
	uintptr_t field = address;
	if (displacement >= 0) {
		const auto distance = static_cast<uintptr_t>(displacement);
		if (distance > UINTPTR_MAX - address) return false;
		field = address + distance;
	} else {
		const auto distance = static_cast<uintptr_t>(-(displacement + 1)) + 1;
		if (distance > address) return false;
		field = address - distance;
	}
	if (field < start || field > end || sizeof(value) > end - field) return false;
	std::memcpy(&value, reinterpret_cast<const void*>(field), sizeof(value));
	return true;
}

bool BuildTemporalFatbin(
	const uint8_t* fat, size_t fatSize, const TemporalProfile& profile,
	std::vector<uint8_t>& out, std::string& why) {
	size_t entry = 0;
	if (!FindAdaPtxEntry(fat, fatSize, entry)) {
		why = "no sm_89 PTX entry";
		return false;
	}
	const uint32_t hdr = ReadU32(fat + entry + 4);
	const uint32_t compressed = ReadU32(fat + entry + 16);
	const uint64_t raw = ReadU64(fat + entry + 56);
	if (compressed == 0 || raw == 0 || raw > (8u << 20)) {
		why = "PTX entry is not compressed as expected";
		return false;
	}
	if (raw != profile.ptxBytes) {
		std::ostringstream s;
		s << "PTX is " << raw << " bytes, expected " << profile.ptxBytes;
		why = s.str();
		return false;
	}

	std::vector<uint8_t> ptx(static_cast<size_t>(raw));
	if (!Lz4BlockDecompress(fat + entry + hdr, compressed, ptx.data(), ptx.size())) {
		why = "LZ4 decompression failed";
		return false;
	}

	const std::string entrySignature = std::string(".entry ") + profile.entryName + "(";
	const std::string parameterName = std::string(profile.entryName) + "_param_0";
	const std::string parameterSignature =
		std::string(".param .align 8 .b8 ") + parameterName + "[144]";
	const std::string ptxText(reinterpret_cast<const char*>(ptx.data()), ptx.size());
	if (ptxText.find(entrySignature) == std::string::npos ||
		ptxText.find(parameterSignature) == std::string::npos ||
		ptxText.find(".reg .f32 %f<1362>;") == std::string::npos) {
		why = "temporal kernel signature changed";
		return false;
	}

	const char* begin = reinterpret_cast<const char*>(ptx.data());
	const size_t n = ptx.size();
	const size_t labelLen = sizeof(kJoinLabel) - 1;
	size_t label = SIZE_MAX;
	for (size_t i = 0; i + labelLen <= n; ++i) {
		if (std::memcmp(begin + i, kJoinLabel, labelLen) != 0) continue;
		if (label != SIZE_MAX) {
			why = "join label is not unique";
			return false;
		}
		label = i;
	}
	if (label == SIZE_MAX) {
		why = "join label not found";
		return false;
	}
	size_t insertion = label + labelLen;
	while (insertion < n && begin[insertion] != '\n') ++insertion;
	if (insertion >= n) {
		why = "join label has no line end";
		return false;
	}
	++insertion;

	const size_t midLen = sizeof(kMidpointBits) - 1;
	const size_t mulLen = sizeof(kMulPrefix) - 1;
	std::vector<size_t> marks;
	marks.reserve(kExpectedMidpoints);
	for (size_t i = 0; i + midLen < n; ++i) {
		if (std::memcmp(begin + i, kMidpointBits, midLen) != 0) continue;
		if (begin[i + midLen] != ';') continue;
		size_t line = i;
		while (line > 0 && begin[line - 1] != '\n') --line;
		if (i - line < mulLen) continue;
		if (std::memcmp(begin + line, kMulPrefix, mulLen) != 0) continue;
		marks.push_back(i);
	}
	if (marks.size() != kExpectedMidpoints) {
		std::ostringstream s;
		s << "found " << marks.size() << " midpoint multiplies, expected " << kExpectedMidpoints;
		why = s.str();
		return false;
	}
	if (marks.front() <= insertion) {
		why = "first midpoint precedes the injection point";
		return false;
	}

	const std::string temporalInput =
		"ld.param.f32 %f134, [" + parameterName + "+32];\r\n"
		"mov.f32 %f135, 0f3F800000;\r\n"
		"sub.ftz.f32 %f136, %f135, %f134;\r\n";

	std::vector<uint8_t> patched;
	patched.reserve(n + temporalInput.size());
	auto append = [&patched](const void* p, size_t bytes) {
		const auto* b = static_cast<const uint8_t*>(p);
		patched.insert(patched.end(), b, b + bytes);
	};
	append(ptx.data(), insertion);
	append(temporalInput.data(), temporalInput.size());
	size_t src = insertion;
	const size_t half = kExpectedMidpoints / 2;
	for (size_t i = 0; i < marks.size(); ++i) {
		append(ptx.data() + src, marks[i] - src);
		const char* scale = (i < half) ? "%f136" : "%f134";
		append(scale, 5);
		src = marks[i] + midLen;
	}
	append(ptx.data() + src, n - src);

	const size_t padded = (patched.size() + 7) & ~size_t{7};
	const size_t finalSize = entry + hdr + padded;
	out.assign(fat, fat + entry + hdr);
	out.resize(finalSize, 0);
	std::memcpy(out.data() + entry + hdr, patched.data(), patched.size());

	const uint64_t payload64 = padded;
	const uint32_t zero32 = 0;
	const uint64_t zero64 = 0;
	std::memcpy(out.data() + entry + 8, &payload64, sizeof(payload64));
	std::memcpy(out.data() + entry + 16, &zero32, sizeof(zero32));
	std::memcpy(out.data() + entry + 40, &kUncompressedFlags, sizeof(kUncompressedFlags));
	std::memcpy(out.data() + entry + 56, &zero64, sizeof(zero64));
	const uint64_t outer = finalSize - kOuterHeader;
	std::memcpy(out.data() + 8, &outer, sizeof(outer));
	why = profile.descriptorName;
	return true;
}

} // namespace

bool ApplyMidpoint(HMODULE module, std::string& detail) noexcept {
	detail.clear();
	try {
		auto* base = reinterpret_cast<uint8_t*>(module);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
			detail = "DLSSG image is unreadable";
			return false;
		}
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) {
			detail = "DLSSG is not an x64 image";
			return false;
		}
		const size_t imageSize = nt->OptionalHeader.SizeOfImage;
		const auto start = reinterpret_cast<uintptr_t>(base);

		std::vector<uint64_t*> slots;
		const uint8_t* fat = nullptr;
		size_t fatSize = 0;
		const TemporalProfile* selected = nullptr;

		const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
			if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0) continue;
			if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) continue;
			uint8_t* sec = base + section->VirtualAddress;
			const size_t size = section->Misc.VirtualSize;
			for (size_t off = 0; off + sizeof(uint64_t) <= size; off += sizeof(uint64_t)) {
				uint64_t value = 0;
				std::memcpy(&value, sec + off, sizeof(value));
				if (value < start || imageSize < kOuterHeader ||
					value > start + imageSize - kOuterHeader) {
					continue;
				}
				const auto* candidate = reinterpret_cast<const uint8_t*>(value);
				if (ReadU32(candidate) != kFatbinMagic) continue;

				const TemporalProfile* nameProfile = nullptr;
				for (const TemporalProfile& profile : kProfiles) {
					uint64_t entryName = 0;
					uint64_t descName = 0;
					if (!ReadRelativePointer(base, imageSize, sec + off, profile.entryNameOffset, entryName) ||
						!ReadRelativePointer(base, imageSize, sec + off, profile.descriptorNameOffset, descName)) {
						continue;
					}
					if (PointsToCString(base, imageSize, entryName, profile.entryName) &&
						PointsToCString(base, imageSize, descName, profile.descriptorName)) {
						nameProfile = &profile;
						break;
					}
				}
				if (!nameProfile) continue;

				const uint64_t declared = ReadU64(candidate + 8);
				if (declared > static_cast<uint64_t>((std::numeric_limits<size_t>::max)() - kOuterHeader)) {
					continue;
				}
				const size_t total = static_cast<size_t>(declared) + kOuterHeader;
				if (total < 1024 || total > (16u << 20)) continue;
				if (total > start + imageSize - value) continue;
				const TemporalProfile* fatProfile = FindTemporalProfile(candidate, total);
				if (!fatProfile || fatProfile != nameProfile) continue;
				if (!fat) {
					fat = candidate;
					fatSize = total;
					selected = fatProfile;
				} else if (candidate != fat) {
					continue;
				}
				slots.push_back(reinterpret_cast<uint64_t*>(sec + off));
			}
		}

		if (!fat || slots.empty() || !selected) {
			detail = "no supported temporal-kernel descriptor found";
			return false;
		}

		std::vector<uint8_t> rebuilt;
		std::string why;
		if (!BuildTemporalFatbin(fat, fatSize, *selected, rebuilt, why)) {
			detail = why;
			return false;
		}

		void* mem = ::VirtualAlloc(nullptr, rebuilt.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!mem) {
			detail = "could not keep the corrected fatbin";
			return false;
		}
		std::memcpy(mem, rebuilt.data(), rebuilt.size());

		unsigned redirected = 0;
		for (uint64_t* slot : slots) {
			DWORD oldProtect = 0;
			if (::VirtualProtect(slot, sizeof(uint64_t), PAGE_READWRITE, &oldProtect) == 0) continue;
			*slot = reinterpret_cast<uint64_t>(mem);
			DWORD ignored = 0;
			::VirtualProtect(slot, sizeof(uint64_t), oldProtect, &ignored);
			++redirected;
		}
		if (redirected == 0) {
			::VirtualFree(mem, 0, MEM_RELEASE);
			detail = "no descriptor slot was writable";
			return false;
		}
		::FlushInstructionCache(::GetCurrentProcess(), nullptr, 0);

		std::ostringstream s;
		s << "redirected " << redirected << " " << selected->descriptorName
			<< " descriptor(s) from a " << fatSize << "-byte fatbin to a " << rebuilt.size()
			<< "-byte temporal-corrected rebuild";
		detail = s.str();
		return true;
	} catch (...) {
		detail = "midpoint correction failed";
		return false;
	}
}

} // namespace Magpie::DLSSGAdaUnlock
