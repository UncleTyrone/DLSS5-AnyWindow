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
		const PublishCallback& publishGeneratedFrame) noexcept;
	void RequestHistoryReset() noexcept;

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
