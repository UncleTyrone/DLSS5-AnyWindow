#include "pch.h"
#include "DLSSZeroMVUpscaler.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"

#ifdef MP_ENABLE_DLSS_ZEROMV
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

namespace Magpie {

static bool NGXSucceeded(NVSDK_NGX_Result result) noexcept {
	return NVSDK_NGX_SUCCEED(result);
}

DLSSZeroMVUpscaler::~DLSSZeroMVUpscaler() {
	_Reset();
}

void DLSSZeroMVUpscaler::_Reset() noexcept {
	if (_feature) {
		NVSDK_NGX_D3D11_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(_feature));
		_feature = nullptr;
	}
	if (_parameters) {
		NVSDK_NGX_D3D11_DestroyParameters(static_cast<NVSDK_NGX_Parameter*>(_parameters));
		_parameters = nullptr;
	}
	if (_ngxInitialized) {
		NVSDK_NGX_D3D11_Shutdown1(_device);
		_ngxInitialized = false;
	}

	_zeroDepthUav = nullptr;
	_zeroDepth = nullptr;
	_biasCurrentColorMaskUav = nullptr;
	_biasCurrentColorMask = nullptr;
	_zeroMotionVectorsUav = nullptr;
	_zeroMotionVectors = nullptr;
	_device = nullptr;
	_d3dDC = nullptr;
	_resetHistory = true;
	_settings = {};
	_frameIndex = 0;
	_lastGuidanceBinding = UINT8_MAX;
	_lastGuidanceResetFrameId =
		std::numeric_limits<FrameGuidanceFrameId>::max();
}

bool DLSSZeroMVUpscaler::Initialize(
	DeviceResources& deviceResources,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output,
	const DLSSSRSettings& settings
) noexcept {
	_Reset();
	_settings = settings;
	_device = deviceResources.GetD3DDevice();
	_d3dDC = deviceResources.GetD3DDC();

	D3D11_TEXTURE2D_DESC inputDesc{};
	D3D11_TEXTURE2D_DESC outputDesc{};
	input->GetDesc(&inputDesc);
	output->GetDesc(&outputDesc);
	if (inputDesc.Width > outputDesc.Width || inputDesc.Height > outputDesc.Height) {
		Logger::Get().Error("DLSS Zero-MV only supports upscaling");
		_Reset();
		return false;
	}

	_zeroMotionVectors = DirectXHelper::CreateTexture2D(
		_device,
		DXGI_FORMAT_R16G16_FLOAT,
		inputDesc.Width,
		inputDesc.Height,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
	);
	_zeroDepth = DirectXHelper::CreateTexture2D(
		_device,
		DXGI_FORMAT_R32_FLOAT,
		inputDesc.Width,
		inputDesc.Height,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
	);
	_biasCurrentColorMask = DirectXHelper::CreateTexture2D(
		_device,
		DXGI_FORMAT_R8_UNORM,
		inputDesc.Width,
		inputDesc.Height,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
	);
	if (!_zeroMotionVectors || !_zeroDepth || !_biasCurrentColorMask) {
		Logger::Get().Error("Create DLSS Zero-MV auxiliary textures failed");
		_Reset();
		return false;
	}
	HRESULT hr = _device->CreateUnorderedAccessView(
		_zeroMotionVectors.get(), nullptr, _zeroMotionVectorsUav.put());
	if (SUCCEEDED(hr)) {
		hr = _device->CreateUnorderedAccessView(
			_zeroDepth.get(), nullptr, _zeroDepthUav.put());
	}
	if (SUCCEEDED(hr)) {
		hr = _device->CreateUnorderedAccessView(
			_biasCurrentColorMask.get(), nullptr, _biasCurrentColorMaskUav.put());
	}
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSS Zero-MV UAV failed", hr);
		_Reset();
		return false;
	}

	// A stable custom project identifier is permitted by NGX when an NVIDIA
	// application ID has not been assigned to an integration.
	NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init_with_ProjectID(
		"7c134ab9-9677-4af5-a2b2-bca943350861",
		NVSDK_NGX_ENGINE_TYPE_CUSTOM,
		"Magpie-ZeroMV-1",
		L"logs",
		_device
	);
	if (!NGXSucceeded(result)) {
		Logger::Get().Error(fmt::format("NVSDK_NGX_D3D11_Init failed ({:#x})", (uint32_t)result));
		_Reset();
		return false;
	}
	_ngxInitialized = true;

	NVSDK_NGX_Parameter* parameters = nullptr;
	result = NVSDK_NGX_D3D11_GetCapabilityParameters(&parameters);
	if (!NGXSucceeded(result) || !parameters) {
		Logger::Get().Error(fmt::format("NVSDK_NGX_D3D11_GetCapabilityParameters failed ({:#x})", (uint32_t)result));
		_Reset();
		return false;
	}
	_parameters = parameters;

	const double upscaleRatio = std::max(
		outputDesc.Width / static_cast<double>(inputDesc.Width),
		outputDesc.Height / static_cast<double>(inputDesc.Height));
	const NVSDK_NGX_PerfQuality_Value qualityMode = upscaleRatio >= 2.75 ?
		NVSDK_NGX_PerfQuality_Value_UltraPerformance :
		upscaleRatio >= 1.85 ? NVSDK_NGX_PerfQuality_Value_MaxPerf :
		upscaleRatio >= 1.45 ? NVSDK_NGX_PerfQuality_Value_Balanced :
		NVSDK_NGX_PerfQuality_Value_MaxQuality;
	const char* qualityName = qualityMode == NVSDK_NGX_PerfQuality_Value_UltraPerformance ?
		"UltraPerformance" : qualityMode == NVSDK_NGX_PerfQuality_Value_MaxPerf ?
		"Performance" : qualityMode == NVSDK_NGX_PerfQuality_Value_Balanced ?
		"Balanced" : "Quality";
	const char* presetParameter = qualityMode == NVSDK_NGX_PerfQuality_Value_UltraPerformance ?
		NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance :
		qualityMode == NVSDK_NGX_PerfQuality_Value_MaxPerf ?
		NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance :
		qualityMode == NVSDK_NGX_PerfQuality_Value_Balanced ?
		NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced :
		NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
	// Preset J remains the established default for this captured-frame path.
	// The performance class follows the actual scale requested by the UI.
	NVSDK_NGX_Parameter_SetI(
		parameters,
		presetParameter,
		NVSDK_NGX_DLSS_Hint_Render_Preset_J
	);

	NVSDK_NGX_DLSS_Create_Params createParams{
		.Feature{
			.InWidth = inputDesc.Width,
			.InHeight = inputDesc.Height,
			.InTargetWidth = outputDesc.Width,
			.InTargetHeight = outputDesc.Height,
			.InPerfQualityValue = qualityMode
		},
		.InFeatureCreateFlags = uint32_t(
			NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
			NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
			(_settings.useEstimatedDepth ?
				NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0)),
		.InEnableOutputSubrects = false
	};

	NVSDK_NGX_Handle* feature = nullptr;
	result = NGX_D3D11_CREATE_DLSS_EXT(_d3dDC, &feature, parameters, &createParams);
	if (!NGXSucceeded(result) || !feature) {
		Logger::Get().Error(fmt::format("NGX_D3D11_CREATE_DLSS_EXT failed ({:#x})", (uint32_t)result));
		_Reset();
		return false;
	}
	_feature = feature;

	Logger::Get().Info(fmt::format(
		"DLSS SR_Experimental initialized ({}, preset J): {}x{} -> {}x{}, "
		"requestedMotion={}, requestedDepth={}, jitter={}",
		qualityName, inputDesc.Width, inputDesc.Height, outputDesc.Width, outputDesc.Height,
		_settings.useMotionVectors, _settings.useEstimatedDepth,
		_settings.enableJitter));
	return true;
}

FrameGuidanceRequirements
DLSSZeroMVUpscaler::GetFrameGuidanceRequirements() const noexcept {
	FrameGuidanceRequirements result{ .zero = true };
	// Estimated depth uses motion internally for temporal reprojection even
	// when the user chooses not to bind motion to DLSS SR.
	result.motion = _settings.useMotionVectors || _settings.useEstimatedDepth;
	result.depth = _settings.useEstimatedDepth;
	return result;
}

bool DLSSZeroMVUpscaler::Resize(
	DeviceResources& deviceResources,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output
) noexcept {
	const DLSSSRSettings settings = _settings;
	return Initialize(deviceResources, input, output, settings);
}

static float Halton(uint32_t index, uint32_t base) noexcept {
	float result = 0.0f;
	float fraction = 1.0f;
	while (index) {
		fraction /= (float)base;
		result += fraction * (float)(index % base);
		index /= base;
	}
	return result;
}

bool DLSSZeroMVUpscaler::Draw(const NativeEffectDrawContext& context) noexcept {
	ID3D11Texture2D* input = context.input;
	ID3D11Texture2D* output = context.output;
	if (!_feature || !_parameters || !_zeroMotionVectorsUav || !_zeroDepthUav ||
		!_biasCurrentColorMaskUav) {
		return false;
	}

	static constexpr float ZERO[4]{};
	static constexpr float BIAS_CURRENT_COLOR[4]{ 0.5f,0.5f,0.5f,0.5f };
	ID3D11Texture2D* motionVectors = _zeroMotionVectors.get();
	ID3D11Texture2D* depth = _zeroDepth.get();
	bool guidanceReset = false;
	bool realMotion = false;
	bool realDepth = false;

	D3D11_TEXTURE2D_DESC inputDesc{};
	input->GetDesc(&inputDesc);
	const FrameGuidanceExtent inputExtent{ inputDesc.Width, inputDesc.Height };
	const FrameGuidanceView guidance = SelectFrameGuidanceChannels(
		context.frameGuidance, context.zeroFrameGuidance,
		context.frameId, inputExtent,
		_settings.useMotionVectors, _settings.useEstimatedDepth);
	if (guidance.IsValidFor(context.frameId, inputExtent)) {
		if (_settings.useMotionVectors) {
			motionVectors = guidance.motion.texture;
			realMotion = !guidance.motion.metadata.isZero;
		}
		if (_settings.useEstimatedDepth) {
			depth = guidance.depth.texture;
			realDepth = !guidance.depth.metadata.isZero;
		}
		guidanceReset = guidance.requiresHistoryReset;
		for (const FrameGuidanceResource* resource :
			{ &guidance.motion, &guidance.depth }) {
			if (resource->metadata.sync.fence && resource->metadata.sync.value &&
				FAILED(_d3dDC->Wait(
					resource->metadata.sync.fence,
					resource->metadata.sync.value))) {
				Logger::Get().Warn("DLSS SR Frame Guidance producer wait failed; using Zero");
				motionVectors = _zeroMotionVectors.get();
				depth = _zeroDepth.get();
				realMotion = false;
				realDepth = false;
				guidanceReset = true;
				break;
			}
		}
	}

	const uint8_t binding = uint8_t(realMotion) |
		(uint8_t(realDepth) << 1) |
		(uint8_t(_settings.useMotionVectors) << 2) |
		(uint8_t(_settings.useEstimatedDepth) << 3);
	const bool bindingChanged = _lastGuidanceBinding != UINT8_MAX &&
		binding != _lastGuidanceBinding;
	if (binding != _lastGuidanceBinding) {
		Logger::Get().Info(fmt::format(
			"DLSS SR guidance frameId={}: requested motion={} depth={}, "
			"bound motion={} depth={}, fallback={}",
			context.frameId, _settings.useMotionVectors,
			_settings.useEstimatedDepth,
			realMotion ? "real" : "zero", realDepth ? "real" : "zero",
			(!realMotion && _settings.useMotionVectors) ||
			(!realDepth && _settings.useEstimatedDepth) ? "zero" : "none"));
	}
	guidanceReset = bindingChanged ||
		(guidanceReset && _lastGuidanceResetFrameId != context.frameId);

	if (motionVectors == _zeroMotionVectors.get()) {
		_d3dDC->ClearUnorderedAccessViewFloat(_zeroMotionVectorsUav.get(), ZERO);
	}
	if (depth == _zeroDepth.get()) {
		_d3dDC->ClearUnorderedAccessViewFloat(_zeroDepthUav.get(), ZERO);
	}
	_d3dDC->ClearUnorderedAccessViewFloat(_biasCurrentColorMaskUav.get(), BIAS_CURRENT_COLOR);

	NVSDK_NGX_D3D11_DLSS_Eval_Params evalParams{};
	evalParams.Feature.InSharpness = 0.3f;
	evalParams.Feature.pInColor = input;
	evalParams.Feature.pInOutput = output;
	evalParams.pInDepth = depth;
	evalParams.pInMotionVectors = motionVectors;
	evalParams.pInBiasCurrentColorMask = _biasCurrentColorMask.get();
	if (_settings.enableJitter) {
		// An 8-sample Halton(2,3) sequence centered around zero. Since Magpie
		// cannot jitter the source application's projection, this is deliberately
		// exposed as a separate metadata-only experiment.
		const uint32_t sample = (_frameIndex++ & 7u) + 1u;
		evalParams.InJitterOffsetX = Halton(sample, 2) - 0.5f;
		evalParams.InJitterOffsetY = Halton(sample, 3) - 0.5f;
	} else {
		evalParams.InJitterOffsetX = 0.0f;
		evalParams.InJitterOffsetY = 0.0f;
	}
	evalParams.InRenderSubrectDimensions = { inputDesc.Width, inputDesc.Height };
	evalParams.InReset = _resetHistory || guidanceReset ? 1 : 0;
	evalParams.InMVScaleX = 1.0f;
	evalParams.InMVScaleY = 1.0f;
	evalParams.InPreExposure = 1.0f;
	evalParams.InExposureScale = 1.0f;

	const NVSDK_NGX_Result result = NGX_D3D11_EVALUATE_DLSS_EXT(
		_d3dDC,
		static_cast<NVSDK_NGX_Handle*>(_feature),
		static_cast<NVSDK_NGX_Parameter*>(_parameters),
		&evalParams
	);
	if (!NGXSucceeded(result)) {
		Logger::Get().Error(fmt::format("NGX_D3D11_EVALUATE_DLSS_EXT failed ({:#x})", (uint32_t)result));
		return false;
	}

	_resetHistory = false;
	_lastGuidanceBinding = binding;
	if (guidanceReset) _lastGuidanceResetFrameId = context.frameId;
	return true;
}

}

#else

namespace Magpie {

DLSSZeroMVUpscaler::~DLSSZeroMVUpscaler() = default;
void DLSSZeroMVUpscaler::_Reset() noexcept {}

bool DLSSZeroMVUpscaler::Initialize(
	DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*,
	const DLSSSRSettings&) noexcept {
	Logger::Get().Error("DLSS Zero-MV is disabled at build time");
	return false;
}

FrameGuidanceRequirements
DLSSZeroMVUpscaler::GetFrameGuidanceRequirements() const noexcept { return {}; }

bool DLSSZeroMVUpscaler::Resize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*) noexcept {
	return false;
}

bool DLSSZeroMVUpscaler::Draw(const NativeEffectDrawContext&) noexcept {
	return false;
}

}

#endif
