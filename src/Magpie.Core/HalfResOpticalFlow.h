#pragma once

#include "FrameGuidanceProvider.h"

namespace Magpie {

class DeviceResources;

// Lightweight colour-only block matching. Motion is estimated on a half-size
// grid and expanded to render resolution for temporal upscalers.
class HalfResOpticalFlow {
public:
	HalfResOpticalFlow() = default;
	HalfResOpticalFlow(const HalfResOpticalFlow&) = delete;
	HalfResOpticalFlow& operator=(const HalfResOpticalFlow&) = delete;

	bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
		ID3D11Texture2D* input) noexcept;
	bool Estimate(ID3D11Texture2D* input) noexcept;
	ID3D11Texture2D* GetMotionTexture() const noexcept { return _fullFlow.get(); }
	ID3D11Texture2D* GetConfidenceTexture() const noexcept {
		return _fullConfidence.get();
	}
	bool IsStatic() const noexcept { return _isStatic; }
	float LastMeanLumaDifference() const noexcept { return _lastMeanLumaDifference; }
	void ResetHistory() noexcept {
		_hasHistory = false;
		_isStatic = true;
	}

private:
	bool _CreateInputSrv(ID3D11Texture2D* input) noexcept;
	bool _MeasureActivity() noexcept;

	ID3D11Device* _device = nullptr;
	ID3D11DeviceContext* _context = nullptr;
	ID3D11Texture2D* _input = nullptr;
	UINT _width = 0;
	UINT _height = 0;
	UINT _halfWidth = 0;
	UINT _halfHeight = 0;
	bool _hasHistory = false;
	bool _isStatic = true;
	float _lastMeanLumaDifference = 0.0f;
	winrt::com_ptr<ID3D11Texture2D> _previous;
	winrt::com_ptr<ID3D11ShaderResourceView> _previousSrv;
	winrt::com_ptr<ID3D11ShaderResourceView> _inputSrv;
	winrt::com_ptr<ID3D11Texture2D> _halfFlow;
	winrt::com_ptr<ID3D11ShaderResourceView> _halfFlowSrv;
	winrt::com_ptr<ID3D11UnorderedAccessView> _halfFlowUav;
	winrt::com_ptr<ID3D11Texture2D> _halfConfidence;
	winrt::com_ptr<ID3D11ShaderResourceView> _halfConfidenceSrv;
	winrt::com_ptr<ID3D11UnorderedAccessView> _halfConfidenceUav;
	winrt::com_ptr<ID3D11Texture2D> _fullFlow;
	winrt::com_ptr<ID3D11UnorderedAccessView> _fullFlowUav;
	winrt::com_ptr<ID3D11Texture2D> _fullConfidence;
	winrt::com_ptr<ID3D11UnorderedAccessView> _fullConfidenceUav;
	winrt::com_ptr<ID3D11Texture2D> _activity;
	winrt::com_ptr<ID3D11UnorderedAccessView> _activityUav;
	winrt::com_ptr<ID3D11Texture2D> _activityReadback;
	winrt::com_ptr<ID3D11ComputeShader> _estimateShader;
	winrt::com_ptr<ID3D11ComputeShader> _upsampleShader;
	winrt::com_ptr<ID3D11ComputeShader> _activityShader;
	winrt::com_ptr<ID3D11Buffer> _constantBuffer;
};

// Colour-only motion-vector fallback for builds that do not include the
// NVIDIA Optical Flow SDK headers. It is intentionally labelled as software
// guidance and never pretends to be NVOF.
class SoftwareOpticalFlowProvider final : public IMotionVectorProvider {
public:
	bool Initialize(
		DeviceResources& resources,
		FrameGuidanceExtent sourceExtent
	) noexcept override;
	bool BeginFrame(
		const FrameGuidanceFrame& frame,
		MotionVectorProviderOutput& output
	) noexcept override;
	void Reset(FrameGuidanceResetReason reason) noexcept override;
	bool Resize(FrameGuidanceExtent sourceExtent) noexcept override;

private:
	DeviceResources* _resources = nullptr;
	FrameGuidanceExtent _extent{};
	std::unique_ptr<HalfResOpticalFlow> _flow;
	FrameGuidanceResetReason _resetReason = FrameGuidanceResetReason::Initialize;
};

}
