#pragma once
#include "FrameGuidanceTypes.h"

namespace Magpie {

class DeviceResources;

struct DLSSFrameGenerationSettings {
	uint32_t multiplier = 2;
	bool useMotionVectors = true;
	bool useEstimatedDepth = false;
};

// Experimental DLSS Frame Generation adapter. It consumes final effect-chain
// color plus Renderer-owned guidance from the same captured base frame.
class DLSSFrameGenerator {
public:
	struct Impl;
	using PublishCallback = std::function<bool(ID3D11Texture2D*)>;
	// Called on the first generated frame: 0 after the input is submitted,
	// 1 after the D3D11 queue waits for the DLSSG output.
	using TimingCallback = std::function<void(uint32_t)>;

	DLSSFrameGenerator();
	DLSSFrameGenerator(const DLSSFrameGenerator&) = delete;
	DLSSFrameGenerator& operator=(const DLSSFrameGenerator&) = delete;
	~DLSSFrameGenerator();

	bool Initialize(
		DeviceResources& resources,
		ID3D11Texture2D* input,
		FrameGuidanceExtent guidanceExtent,
		const DLSSFrameGenerationSettings& settings
	) noexcept;
	bool Resize(
		DeviceResources& resources,
		ID3D11Texture2D* input,
		FrameGuidanceExtent guidanceExtent
	) noexcept;
	bool Draw(
		ID3D11Texture2D* input,
		FrameGuidanceFrameId frameId,
		const FrameGuidanceView& guidance,
		const FrameGuidanceView& zeroGuidance,
		const PublishCallback& publishGeneratedFrame,
		const TimingCallback& markTiming = {}) noexcept;
	void RequestHistoryReset() noexcept;
	// GPU time of DLSSG evaluations completed since the last call.
	void ConsumeEvalGpuTime(double& totalMs, uint32_t& count) noexcept;
	// CPU-clock latency from submitting each DLSSG evaluation to its GPU start
	// and end, for evaluations completed since the last call.
	void ConsumeEvalLatency(double& startMs, double& doneMs, uint32_t& count) noexcept;
	FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept;
	const DLSSFrameGenerationSettings& Settings() const noexcept {
		return _requestedSettings;
	}
	uint32_t Multiplier() const noexcept;
	// Generated frames for the next Draw, clamped to Multiplier() - 1. Zero
	// still feeds the frame to DLSS so its history stays continuous.
	void SetGeneratedFrameCount(uint32_t count) noexcept;

private:
	std::unique_ptr<Impl> _impl;
	DLSSFrameGenerationSettings _requestedSettings{};
};

}
