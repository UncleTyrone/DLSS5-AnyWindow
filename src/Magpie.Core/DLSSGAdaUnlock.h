#pragma once
#include <cstdint>

namespace Magpie::DLSSGAdaUnlock {

enum class TemporalFix : uint32_t {
	Retarget = 0,
	Midpoint = 1
};

// Loads nvngx_dlssg.dll from the exe directory and, on Ada (compute 8.9),
// patches the in-memory image so 3x-6x frame generation is accepted.
// MAGPIE_DLSSG_ADA_UNLOCK=scan reports pattern matches and does not write.
// The signed file on disk is never modified.
bool Prepare(TemporalFix temporal) noexcept;

// True for compute capability 8.9 (RTX 40 / Ada). RTX 50 and other GPUs are false.
bool IsAdaGpu() noexcept;

// 6 on RTX 50, and on RTX 40 while the Ada temporal fix is on. 4 otherwise.
uint32_t MaxMultiplier(bool adaTemporalFix) noexcept;

}
