// SPDX-License-Identifier: MIT
// Adapted from MFGAdaUnlock-RenoDx (mavismmg), src/addons/mfgunlock/thin_geometry.hpp.
// Copyright (c) 2026 mavismmg. The validated-warp experiment is informed by the
// public behavior of Tony Joaca's qualityValidWarp option. This file keeps only
// the base rewrite: accepted blend weights are raised to at least 0.85, the
// Blackwell PTX is retargeted to sm_89, and the descriptor is redirected.
// Offline cubin quality tables are not applied.

#include "pch.h"
#include "Logger.h"
#include <algorithm>
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
constexpr uint32_t kBlackwellArch = 120;
constexpr uint64_t kUncompressedFlags = 0x41;
constexpr size_t kMaxFatbinSize = 4u * 1024u * 1024u;
constexpr uint32_t kProviderTimestamp = 0x6A986031u;
constexpr uint32_t kProviderImageSize = 7565312u;
constexpr size_t kDeclaredRawSize = 39639u;
constexpr size_t kNormalizedSize = 39638u;
constexpr uint64_t kRawFnv = 0x7a6f5f41105c6d85ull;
constexpr char kEntryName[] = "Kernel_BlendCandidatesFused";
constexpr char kParameterSignature[] =
	".param .align 8 .b8 Kernel_BlendCandidatesFused_param_0[240]";

uint16_t ReadU16(const uint8_t* bytes) {
	uint16_t value = 0;
	std::memcpy(&value, bytes, sizeof(value));
	return value;
}
uint32_t ReadU32(const uint8_t* bytes) {
	uint32_t value = 0;
	std::memcpy(&value, bytes, sizeof(value));
	return value;
}
uint64_t ReadU64(const uint8_t* bytes) {
	uint64_t value = 0;
	std::memcpy(&value, bytes, sizeof(value));
	return value;
}

uint64_t Fnv1a64(const uint8_t* bytes, size_t size) {
	uint64_t value = 0xcbf29ce484222325ull;
	for (size_t index = 0; index < size; ++index) {
		value = (value ^ bytes[index]) * 0x100000001b3ull;
	}
	return value;
}

bool Lz4BlockDecompress(const uint8_t* source, size_t sourceSize, uint8_t* destination, size_t destinationSize) {
	size_t input = 0;
	size_t output = 0;
	while (input < sourceSize) {
		const uint8_t token = source[input++];
		size_t literals = token >> 4;
		if (literals == 15) {
			uint8_t extra = 0;
			do {
				if (input >= sourceSize) return false;
				extra = source[input++];
				literals += extra;
			} while (extra == 0xFF);
		}
		if (literals > sourceSize - input || literals > destinationSize - output) return false;
		std::memcpy(destination + output, source + input, literals);
		input += literals;
		output += literals;
		if (input == sourceSize) break;
		if (sourceSize - input < 2) return false;
		const size_t distance = source[input] | (static_cast<size_t>(source[input + 1]) << 8);
		input += 2;
		if (distance == 0 || distance > output) return false;
		size_t match = 4 + (token & 0x0F);
		if ((token & 0x0F) == 15) {
			uint8_t extra = 0;
			do {
				if (input >= sourceSize) return false;
				extra = source[input++];
				match += extra;
			} while (extra == 0xFF);
		}
		if (match > destinationSize - output) return false;
		for (size_t index = 0; index < match; ++index) {
			destination[output + index] = destination[output + index - distance];
		}
		output += match;
	}
	return input == sourceSize && output == destinationSize;
}

bool ReplaceOnce(std::string& text, const std::string& needle, const std::string& replacement, std::string& why) {
	const size_t first = text.find(needle);
	if (first == std::string::npos) {
		why = "required PTX anchor is missing";
		return false;
	}
	if (text.find(needle, first + needle.size()) != std::string::npos) {
		why = "required PTX anchor is ambiguous";
		return false;
	}
	text.replace(first, needle.size(), replacement);
	return true;
}

bool RewriteValidatedWarpBlend(std::string& ptx, std::string& why) {
	if (ptx.find("MFGUNLOCK_VALIDATED_WARP_BLEND_V1") != std::string::npos) {
		why = "validated-warp PTX is already modified";
		return false;
	}
	if (!ReplaceOnce(ptx, ".reg .pred %p<260>;\n",
		".reg .pred %p<260>;\n.reg .pred %qv<7>;\n.reg .f32 %qf<12>;\n", why)) {
		return false;
	}
	constexpr char kProgram[] = R"ptx(// MFGUNLOCK_VALIDATED_WARP_BLEND_V1
cvt.rn.f32.u32 %qf0, %r10;
cvt.rn.f32.u32 %qf1, %r11;
div.approx.ftz.f32 %qf0, 0f3F000000, %qf0;
div.approx.ftz.f32 %qf1, 0f3F000000, %qf1;
sub.ftz.f32 %qf2, 0f3F800000, %qf0;
sub.ftz.f32 %qf3, 0f3F800000, %qf1;
setp.ge.f32 %qv0, %f123, %qf0;
setp.le.f32 %qv2, %f123, %qf2;
and.pred %qv0, %qv0, %qv2;
setp.ge.f32 %qv2, %f124, %qf1;
and.pred %qv0, %qv0, %qv2;
setp.le.f32 %qv2, %f124, %qf3;
and.pred %qv0, %qv0, %qv2;
not.pred %qv2, %p17;
and.pred %qv0, %qv0, %qv2;
setp.ge.f32 %qv1, %f129, %qf0;
setp.le.f32 %qv2, %f129, %qf2;
and.pred %qv1, %qv1, %qv2;
setp.ge.f32 %qv2, %f130, %qf1;
and.pred %qv1, %qv1, %qv2;
setp.le.f32 %qv2, %f130, %qf3;
and.pred %qv1, %qv1, %qv2;
not.pred %qv2, %p16;
and.pred %qv1, %qv1, %qv2;
abs.f32 %qf4, %f125;
abs.f32 %qf5, %f126;
abs.f32 %qf6, %f127;
add.f32 %qf4, %qf4, %qf5;
add.f32 %qf4, %qf4, %qf6;
setp.lt.f32 %qv2, %qf4, 0f7F800000;
and.pred %qv0, %qv0, %qv2;
abs.f32 %qf5, %f131;
abs.f32 %qf6, %f132;
abs.f32 %qf7, %f133;
add.f32 %qf5, %qf5, %qf6;
add.f32 %qf5, %qf5, %qf7;
setp.lt.f32 %qv2, %qf5, 0f7F800000;
and.pred %qv1, %qv1, %qv2;
and.pred %qv3, %qv0, %qv1;
sub.f32 %qf6, %f115, %f119;
sub.f32 %qf7, %f116, %f120;
sub.f32 %qf8, %f117, %f121;
abs.f32 %qf6, %qf6;
abs.f32 %qf7, %qf7;
abs.f32 %qf8, %qf8;
add.f32 %qf6, %qf6, %qf7;
add.f32 %qf6, %qf6, %qf8;
sub.f32 %qf9, %f125, %f131;
sub.f32 %qf10, %f126, %f132;
sub.f32 %qf11, %f127, %f133;
abs.f32 %qf9, %qf9;
abs.f32 %qf10, %qf10;
abs.f32 %qf11, %qf11;
add.f32 %qf9, %qf9, %qf10;
add.f32 %qf9, %qf9, %qf11;
add.f32 %qf10, %qf9, 0f3DA3D70A;
setp.lt.f32 %qv4, %qf10, %qf6;
setp.lt.f32 %qv2, %qf9, 0f3E19999A;
and.pred %qv4, %qv4, %qv2;
and.pred %qv4, %qv4, %qv3;
setp.gt.f32 %qv2, %qf6, 0f3E800000;
setp.ge.f32 %qv5, %f148, 0f3E4CCCCD;
and.pred %qv5, %qv5, %qv2;
or.pred %qv5, %qv5, %qv4;
and.pred %qv0, %qv0, %qv5;
setp.ge.f32 %qv6, %f149, 0f3E4CCCCD;
and.pred %qv6, %qv6, %qv2;
or.pred %qv6, %qv6, %qv4;
and.pred %qv1, %qv1, %qv6;
max.f32 %qf0, %f148, 0f3F59999A;
min.f32 %qf0, %qf0, 0f3F800000;
max.f32 %qf1, %f149, 0f3F59999A;
min.f32 %qf1, %qf1, 0f3F800000;
sub.f32 %qf2, %f125, %f115;
sub.f32 %qf3, %f126, %f116;
sub.f32 %qf4, %f127, %f117;
@%qv0 fma.rn.f32 %f39, %qf0, %qf2, %f115;
@%qv0 fma.rn.f32 %f38, %qf0, %qf3, %f116;
@%qv0 fma.rn.f32 %f37, %qf0, %qf4, %f117;
sub.f32 %qf2, %f131, %f119;
sub.f32 %qf3, %f132, %f120;
sub.f32 %qf4, %f133, %f121;
@%qv1 fma.rn.f32 %f43, %qf1, %qf2, %f119;
@%qv1 fma.rn.f32 %f42, %qf1, %qf3, %f120;
@%qv1 fma.rn.f32 %f41, %qf1, %qf4, %f121;
)ptx";
	constexpr char kInsertion[] = "ld.param.u8 %rs8, [%rd6+220];\n";
	return ReplaceOnce(ptx, kInsertion, std::string(kProgram) + kInsertion, why);
}

bool FindPtxEntry(const uint8_t* fatbin, size_t fatbinSize, size_t& entryOffset, std::vector<uint8_t>& ptx, std::string& why) {
	if (fatbinSize < kOuterHeader || ReadU32(fatbin) != kFatbinMagic ||
		ReadU16(fatbin + 6) != kOuterHeader ||
		ReadU64(fatbin + 8) + kOuterHeader != fatbinSize) {
		why = "invalid fatbin header";
		return false;
	}
	size_t cursor = kOuterHeader;
	while (cursor + 64 <= fatbinSize) {
		const uint32_t header = ReadU32(fatbin + cursor + 4);
		const uint64_t payload = ReadU64(fatbin + cursor + 8);
		if (header < 64 || payload == 0 || cursor + header + payload > fatbinSize) {
			why = "invalid fatbin entry";
			return false;
		}
		if (ReadU16(fatbin + cursor) == kPtxKind && ReadU32(fatbin + cursor + 28) == kBlackwellArch) {
			const uint32_t compressed = ReadU32(fatbin + cursor + 16);
			const uint64_t raw = ReadU64(fatbin + cursor + 56);
			if (raw != kDeclaredRawSize || compressed == 0 || compressed > payload || raw > (8u << 20)) {
				why = "PTX size/compression does not match the supported profile";
				return false;
			}
			ptx.resize(static_cast<size_t>(raw));
			if (!Lz4BlockDecompress(fatbin + cursor + header, compressed, ptx.data(), ptx.size())) {
				why = "PTX LZ4 decompression failed";
				return false;
			}
			ptx.erase(std::remove(ptx.begin(), ptx.end(), static_cast<uint8_t>('\r')), ptx.end());
			while (!ptx.empty() && ptx.back() == 0) ptx.pop_back();
			if (ptx.size() != kNormalizedSize || Fnv1a64(ptx.data(), ptx.size()) != kRawFnv) {
				why = "PTX identity hash does not match the supported profile";
				return false;
			}
			const std::string text(reinterpret_cast<const char*>(ptx.data()), ptx.size());
			const std::string entry = std::string(".entry ") + kEntryName + "(";
			if (text.find(entry) == std::string::npos || text.find(kParameterSignature) == std::string::npos) {
				why = "PTX entry or parameter layout changed";
				return false;
			}
			entryOffset = cursor;
			return true;
		}
		cursor += header + static_cast<size_t>(payload);
	}
	why = "required PTX architecture entry is missing";
	return false;
}

struct LocatedFatbin {
	const uint8_t* address = nullptr;
	size_t size = 0;
};

bool LocateUniqueFatbin(HMODULE module, LocatedFatbin& located, std::string& why) {
	auto* base = reinterpret_cast<uint8_t*>(module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	const size_t imageSize = nt->OptionalHeader.SizeOfImage;
	size_t matches = 0;
	const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
		if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0 ||
			(section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 ||
			section->VirtualAddress >= imageSize) {
			continue;
		}
		const size_t sectionSize = std::min<size_t>(section->Misc.VirtualSize, imageSize - section->VirtualAddress);
		const uint8_t* bytes = base + section->VirtualAddress;
		for (size_t offset = 0; offset + kOuterHeader <= sectionSize; ++offset) {
			if (ReadU32(bytes + offset) != kFatbinMagic || ReadU16(bytes + offset + 6) != kOuterHeader) continue;
			const uint64_t declared = ReadU64(bytes + offset + 8);
			if (declared == 0 || declared > kMaxFatbinSize || declared > sectionSize - offset - kOuterHeader) continue;
			const size_t total = static_cast<size_t>(declared) + kOuterHeader;
			size_t ignoredEntry = 0;
			std::vector<uint8_t> ignoredPtx;
			std::string ignoredReason;
			if (!FindPtxEntry(bytes + offset, total, ignoredEntry, ignoredPtx, ignoredReason)) continue;
			located = {bytes + offset, total};
			++matches;
			offset += total - 1;
		}
	}
	if (matches != 1) {
		std::ostringstream stream;
		stream << "found " << matches << " exact PTX fatbins, expected one";
		why = stream.str();
		located = {};
		return false;
	}
	return true;
}

bool BuildRedirectedFatbin(const uint8_t* fatbin, size_t fatbinSize, std::vector<uint8_t>& rebuilt, std::string& why) {
	size_t entry = 0;
	std::vector<uint8_t> source;
	if (!FindPtxEntry(fatbin, fatbinSize, entry, source, why)) return false;
	std::string ptx(reinterpret_cast<const char*>(source.data()), source.size());
	if (!ReplaceOnce(ptx, ".target sm_120", ".target sm_89", why)) return false;
	if (!RewriteValidatedWarpBlend(ptx, why)) return false;

	const uint32_t header = ReadU32(fatbin + entry + 4);
	const uint64_t originalPayload64 = ReadU64(fatbin + entry + 8);
	if (originalPayload64 > fatbinSize - entry - header) {
		why = "target PTX entry extends beyond its fatbin";
		return false;
	}
	const size_t originalPayload = static_cast<size_t>(originalPayload64);
	const size_t padded = (ptx.size() + 7u) & ~size_t{7u};
	const size_t suffixOffset = entry + header + originalPayload;
	if (padded > std::numeric_limits<size_t>::max() - (fatbinSize - originalPayload)) {
		why = "replacement fatbin size overflow";
		return false;
	}
	const size_t finalSize = fatbinSize - originalPayload + padded;
	rebuilt.assign(fatbin, fatbin + entry + header);
	rebuilt.resize(finalSize, 0);
	std::memcpy(rebuilt.data() + entry + header, ptx.data(), ptx.size());
	std::memcpy(rebuilt.data() + entry + header + padded, fatbin + suffixOffset, fatbinSize - suffixOffset);
	const uint64_t payload = padded;
	const uint32_t zero32 = 0;
	const uint64_t zero64 = 0;
	std::memcpy(rebuilt.data() + entry + 8, &payload, sizeof(payload));
	std::memcpy(rebuilt.data() + entry + 16, &zero32, sizeof(zero32));
	std::memcpy(rebuilt.data() + entry + 28, &kAdaArch, sizeof(kAdaArch));
	std::memcpy(rebuilt.data() + entry + 40, &kUncompressedFlags, sizeof(kUncompressedFlags));
	std::memcpy(rebuilt.data() + entry + 56, &zero64, sizeof(zero64));
	const uint64_t outer = finalSize - kOuterHeader;
	std::memcpy(rebuilt.data() + 8, &outer, sizeof(outer));
	return true;
}

} // namespace

bool ApplyWarpBlend(HMODULE module, std::string& detail) noexcept {
	detail.clear();
	try {
		if (!module) {
			detail = "provider module is null";
			return false;
		}
		auto* base = reinterpret_cast<uint8_t*>(module);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
			detail = "provider has no valid DOS header";
			return false;
		}
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
			detail = "provider has no valid x64 PE header";
			return false;
		}
		if (nt->FileHeader.TimeDateStamp != kProviderTimestamp || nt->OptionalHeader.SizeOfImage != kProviderImageSize) {
			std::ostringstream stream;
			stream << "unsupported provider metadata (timestamp=0x" << std::hex
				<< nt->FileHeader.TimeDateStamp << std::dec
				<< ", image-size=" << nt->OptionalHeader.SizeOfImage << ')';
			detail = stream.str();
			return false;
		}

		LocatedFatbin located;
		if (!LocateUniqueFatbin(module, located, detail)) return false;
		std::vector<uint8_t> rebuilt;
		if (!BuildRedirectedFatbin(located.address, located.size, rebuilt, detail)) return false;

		void* allocation = ::VirtualAlloc(nullptr, rebuilt.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!allocation) {
			detail = "replacement fatbin allocation failed";
			return false;
		}
		std::memcpy(allocation, rebuilt.data(), rebuilt.size());

		const size_t imageSize = nt->OptionalHeader.SizeOfImage;
		const uint64_t expected = reinterpret_cast<uint64_t>(located.address);
		struct Slot { uint64_t* address; uint64_t original; };
		std::vector<Slot> slots;
		const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
		for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
			if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0 ||
				(section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 ||
				section->VirtualAddress >= imageSize) {
				continue;
			}
			const size_t sectionSize = std::min<size_t>(section->Misc.VirtualSize, imageSize - section->VirtualAddress);
			uint8_t* bytes = base + section->VirtualAddress;
			for (size_t offset = 0; offset + sizeof(uint64_t) <= sectionSize; offset += alignof(uint64_t)) {
				uint64_t value = 0;
				std::memcpy(&value, bytes + offset, sizeof(value));
				if (value != expected) continue;
				auto* slot = reinterpret_cast<uint64_t*>(bytes + offset);
				DWORD oldProtection = 0;
				if (!::VirtualProtect(slot, sizeof(uint64_t), PAGE_READWRITE, &oldProtection)) continue;
				const uint64_t original = *slot;
				*slot = reinterpret_cast<uint64_t>(allocation);
				DWORD ignored = 0;
				::VirtualProtect(slot, sizeof(uint64_t), oldProtection, &ignored);
				slots.push_back({slot, original});
			}
		}
		if (slots.empty() || slots.size() > 32) {
			for (const Slot& patch : slots) {
				DWORD oldProtection = 0;
				if (::VirtualProtect(patch.address, sizeof(uint64_t), PAGE_READWRITE, &oldProtection)) {
					*patch.address = patch.original;
					DWORD ignored = 0;
					::VirtualProtect(patch.address, sizeof(uint64_t), oldProtection, &ignored);
				}
			}
			::VirtualFree(allocation, 0, MEM_RELEASE);
			detail = "descriptor reference count is outside the safe 1..32 range";
			return false;
		}
		::FlushInstructionCache(::GetCurrentProcess(), nullptr, 0);
		std::ostringstream stream;
		stream << "redirected " << slots.size()
			<< " warp-blend descriptor(s) from a " << located.size
			<< "-byte provider fatbin to a " << rebuilt.size() << "-byte PTX rebuild";
		detail = stream.str();
		return true;
	} catch (...) {
		detail = "warp blend correction failed";
		return false;
	}
}

} // namespace Magpie::DLSSGAdaUnlock
