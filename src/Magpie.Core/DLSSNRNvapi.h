#pragma once
#include <Windows.h>

struct ID3D12GraphicsCommandList;

namespace Magpie::DLSSNRNvapi {

// Patches the signed snippet so its nvapi_QueryInterface lookup can see the
// ViT launch wrapper. Interval 1 leaves every launch on the original function.
bool Install(HMODULE snippet) noexcept;
void Restore(HMODULE snippet) noexcept;

// 1 runs the bottleneck every evaluation. 2 reuses it on the other frames.
void SetReuseInterval(unsigned interval) noexcept;

void BeginEvaluate(const void* feature, bool reset, long long frameSlot, ID3D12GraphicsCommandList* cmd) noexcept;
void EndEvaluate(ID3D12GraphicsCommandList* cmd) noexcept;

// NVFP4 stays on the original FP8 model until the pinned hybrid files exist.
void NoteHybridRequest(bool requested) noexcept;

}
