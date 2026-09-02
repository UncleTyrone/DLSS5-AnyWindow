#pragma once
#include "NativeEffectBackend.h"

namespace Magpie {

class DeviceResources;

enum class DLSSNRRuntimeState : int32_t {
	Failed = -1,
	Pending = 0,
	FeatureCreated = 1,
	Evaluating = 2
};

struct DLSSNRTelemetry {
	DLSSNRRuntimeState state = DLSSNRRuntimeState::Pending;
	uint32_t evaluateSuccessCount = 0;
	uint32_t evaluateFailureCount = 0;
};

struct DLSSNRSettings {
	int style = 0;
	float intensity = 1.0f;
	float localToneStrength = 1.0f;
	float localStructureStrength = 1.0f;
	bool useAutoMask = false;
	// 0 available/both, 1 force Zero, 2 motion only, 3 depth only.
	int guidanceMode = 0;
	uint32_t depthInferenceInterval = 4;
	// Re-evaluate the same feature with the first pass output as the next input.
	// This avoids creating a second NGX session, which the signed snippet rejects.
	uint32_t passes = 1;
	// Reset temporal history at the start of every source frame. This keeps a
	// deterministic within-frame pass chain when real motion vectors are absent.
	bool antiFlicker = false;
};

// Experimental same-resolution DLSS neural filter. Magpie only owns the
// composited colour frame, so valid zero-filled motion/depth textures are used
// as explicit temporal guides.
class DLSSNRFilter final : public NativeEffectBackend {
public:
	struct Impl;

	DLSSNRFilter();
	DLSSNRFilter(const DLSSNRFilter&) = delete;
	DLSSNRFilter& operator=(const DLSSNRFilter&) = delete;
	~DLSSNRFilter() override;

	static void ResetRuntimeTelemetry() noexcept;
	static DLSSNRTelemetry RuntimeTelemetry() noexcept;

	FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept override;

	bool Initialize(
		DeviceResources& resources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output,
		const DLSSNRSettings& settings
	) noexcept;

	bool Resize(
		DeviceResources& resources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output
	) noexcept override;

	bool Draw(const NativeEffectDrawContext& context) noexcept override;

private:
	bool _DrawOnce(
		const NativeEffectDrawContext& context,
		bool forceHistoryReset = false
	) noexcept;

	std::unique_ptr<Impl> _impl;
	DLSSNRSettings _settings;
};

}
