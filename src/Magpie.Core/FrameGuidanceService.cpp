#include "pch.h"
#include "FrameGuidanceService.h"
#include "DeviceResources.h"
#include "Logger.h"

namespace Magpie {

static FrameGuidanceExtent GetTextureExtent(ID3D11Texture2D* texture) noexcept {
	if (!texture) {
		return {};
	}
	D3D11_TEXTURE2D_DESC desc{};
	texture->GetDesc(&desc);
	return { desc.Width, desc.Height };
}

static std::string_view ResetReasonName(
	FrameGuidanceResetReason reason
) noexcept {
	switch (reason) {
	case FrameGuidanceResetReason::None: return "None";
	case FrameGuidanceResetReason::Initialize: return "Initialize";
	case FrameGuidanceResetReason::Resize: return "Resize";
	case FrameGuidanceResetReason::SceneChange: return "SceneChange";
	case FrameGuidanceResetReason::CaptureInterrupted: return "CaptureInterrupted";
	case FrameGuidanceResetReason::DeviceRecreated: return "DeviceRecreated";
	case FrameGuidanceResetReason::LongPause: return "LongPause";
	case FrameGuidanceResetReason::ProviderFailure: return "ProviderFailure";
	default: return "Unknown";
	}
}

FrameGuidanceService::FrameGuidanceService() noexcept :
	_zeroDepthProvider(_zeroResources),
	_zeroMotionProvider(_zeroResources) {}

bool FrameGuidanceService::SetDepthProvider(
	std::unique_ptr<IDepthProvider> provider
) noexcept {
	if (IsInitialized()) {
		return false;
	}
	_depthProvider = std::move(provider);
	return true;
}

bool FrameGuidanceService::SetMotionVectorProvider(
	std::unique_ptr<IMotionVectorProvider> provider
) noexcept {
	if (IsInitialized()) {
		return false;
	}
	_motionProvider = std::move(provider);
	return true;
}

bool FrameGuidanceService::Initialize(
	DeviceResources& resources,
	ID3D11Texture2D* sourceFrame,
	const FrameGuidanceRequirements& requirements
) noexcept {
	static_cast<void>(requirements);
	_resources = &resources;
	_sourceExtent = GetTextureExtent(sourceFrame);
	if (!_sourceExtent.IsValid() ||
		!_zeroDepthProvider.Initialize(resources, _sourceExtent) ||
		!_zeroMotionProvider.Initialize(resources, _sourceExtent)) {
		_resources = nullptr;
		Logger::Get().Error("Initialize Frame Guidance zero providers failed");
		return false;
	}
	_depthProviderReady = !_depthProvider ||
		_depthProvider->Initialize(resources, _sourceExtent);
	_motionProviderReady = !_motionProvider ||
		_motionProvider->Initialize(resources, _sourceExtent);
	if (!_depthProviderReady) {
		Logger::Get().Warn(
			"Frame Guidance depth provider initialization failed; using Zero Depth");
		_zeroDepthProvider.Reset(FrameGuidanceResetReason::ProviderFailure);
	}
	if (!_motionProviderReady) {
		Logger::Get().Warn(
			"Frame Guidance motion provider initialization failed; using Zero Motion");
		_zeroMotionProvider.Reset(FrameGuidanceResetReason::ProviderFailure);
	}

	// Capture has not started yet. The frame-source texture is only an
	// allocation and its contents are undefined, so the first real capture
	// must seed every temporal provider.
	_hasCachedFrame = false;
	_view = {};
	_zeroView = {};
	return true;
}

const FrameGuidanceView& FrameGuidanceService::BeginFrame(
	FrameGuidanceFrameId frameId,
	ID3D11Texture2D* sourceFrame,
	const FrameGuidanceRequirements& requirements
) noexcept {
	const FrameGuidanceExtent extent = GetTextureExtent(sourceFrame);
	if (_hasCachedFrame && _cachedFrameId == frameId && extent == _sourceExtent &&
		_cachedRequirements == requirements) {
		return _view;
	}
	if (extent != _sourceExtent) {
		if (!Resize(extent, frameId, requirements)) {
			return _view;
		}
	}
	return _Produce({
		.color = sourceFrame,
		.frameId = frameId,
		.sourceExtent = _sourceExtent,
		.validRegion = FrameGuidanceRegion::Full(_sourceExtent)
	}, requirements);
}

bool FrameGuidanceService::Resize(
	FrameGuidanceExtent sourceExtent,
	FrameGuidanceFrameId currentFrameId,
	const FrameGuidanceRequirements& requirements
) noexcept {
	static_cast<void>(currentFrameId);
	if (!sourceExtent.IsValid() || !_resources) {
		return false;
	}
	if (!_zeroDepthProvider.Resize(sourceExtent) ||
		!_zeroMotionProvider.Resize(sourceExtent)) {
		Logger::Get().Error("Resize Frame Guidance zero providers failed");
		return false;
	}
	if (requirements.depth && _depthProviderReady && _depthProvider &&
		!_depthProvider->Resize(sourceExtent)) {
		_depthProviderReady = false;
		_zeroDepthProvider.Reset(FrameGuidanceResetReason::ProviderFailure);
		Logger::Get().Warn(
			"Resize Frame Guidance depth provider failed; using Zero Depth");
	}
	if (requirements.motion && _motionProviderReady && _motionProvider &&
		!_motionProvider->Resize(sourceExtent)) {
		_motionProviderReady = false;
		_zeroMotionProvider.Reset(FrameGuidanceResetReason::ProviderFailure);
		Logger::Get().Warn(
			"Resize Frame Guidance motion provider failed; using Zero Motion");
	}
	_sourceExtent = sourceExtent;
	_hasCachedFrame = false;
	_view = {};
	_zeroView = {};
	return true;
}

void FrameGuidanceService::ResetHistory(
	FrameGuidanceResetReason reason
) noexcept {
	Logger::Get().Info(fmt::format(
		"Frame Guidance history reset: reason={}", ResetReasonName(reason)));
	_zeroDepthProvider.Reset(reason);
	_zeroMotionProvider.Reset(reason);
	if (_depthProvider) {
		_depthProvider->Reset(reason);
	}
	if (_motionProvider) {
		_motionProvider->Reset(reason);
	}
	_view.requiresHistoryReset = true;
	_view.depth.metadata.requiresHistoryReset = true;
	_view.depth.metadata.resetReason = reason;
	_view.motion.metadata.requiresHistoryReset = true;
	_view.motion.metadata.resetReason = reason;
	_view.confidence.metadata.requiresHistoryReset = true;
	_view.confidence.metadata.resetReason = reason;
	_hasCachedFrame = false;
}

const FrameGuidanceView& FrameGuidanceService::_Produce(
	const FrameGuidanceFrame& frame,
	const FrameGuidanceRequirements& requirements
) noexcept {
	if (!_hasLoggedRequirements || requirements != _lastLoggedRequirements) {
		Logger::Get().Info(fmt::format(
			"Frame Guidance requirements frameId={}: zero={} motion={} depth={} "
			"depthInterval={} motionAction={} depthAction={}",
			frame.frameId, requirements.zero,
			requirements.motion, requirements.depth,
			requirements.depthInferenceInterval,
			requirements.motion ?
				(_motionProviderReady && _motionProvider ? "run" : "zero-unavailable") :
				"skip-unrequested",
			requirements.depth ?
				(_depthProviderReady && _depthProvider ? "run" : "zero-unavailable") :
				"skip-unrequested"));
		_lastLoggedRequirements = requirements;
		_hasLoggedRequirements = true;
	}
	DepthProviderOutput zeroDepth;
	MotionVectorProviderOutput zeroMotion;
	if (!_zeroDepthProvider.BeginFrame(frame, zeroDepth) ||
		!_zeroMotionProvider.BeginFrame(frame, zeroMotion) ||
		!zeroDepth.depth.IsValid(
			DXGI_FORMAT_R32_FLOAT, frame.frameId, frame.sourceExtent) ||
		!zeroMotion.motion.IsValid(
			DXGI_FORMAT_R16G16_FLOAT, frame.frameId, frame.sourceExtent) ||
		!zeroMotion.confidence.IsValid(
			DXGI_FORMAT_R8_UNORM, frame.frameId, frame.sourceExtent)) {
		Logger::Get().Error("Produce zero Frame Guidance failed");
		_view = {};
		_zeroView = {};
		_hasCachedFrame = false;
		return _view;
	}
	_zeroView = {
		.depth = zeroDepth.depth,
		.motion = zeroMotion.motion,
		.confidence = zeroMotion.confidence,
		.requiresHistoryReset =
			zeroDepth.depth.metadata.requiresHistoryReset ||
			zeroMotion.motion.metadata.requiresHistoryReset
	};

	MotionVectorProviderOutput motion;
	bool motionValid = false;
	const bool attemptedMotionProvider = requirements.motion &&
		_motionProviderReady && _motionProvider;
	if (attemptedMotionProvider) {
		motionValid = _motionProvider->BeginFrame(frame, motion) &&
			motion.motion.IsValid(
				DXGI_FORMAT_R16G16_FLOAT, frame.frameId, frame.sourceExtent) &&
			motion.confidence.IsValid(
				DXGI_FORMAT_R8_UNORM, frame.frameId, frame.sourceExtent) &&
			motion.motion.metadata.validRegion ==
				motion.confidence.metadata.validRegion;
	}
	if (!motionValid) {
		motion = zeroMotion;
		motionValid = true;
		if (attemptedMotionProvider) {
			motion.motion.metadata.resetReason =
				FrameGuidanceResetReason::ProviderFailure;
			motion.confidence.metadata.resetReason =
				FrameGuidanceResetReason::ProviderFailure;
			motion.motion.metadata.requiresHistoryReset = true;
			motion.confidence.metadata.requiresHistoryReset = true;
		}
	}

	FrameGuidanceFrame depthFrame = frame;
	depthFrame.motionGuidance = &motion;
	DepthProviderOutput depth;
	bool depthValid = false;
	const bool attemptedDepthProvider = requirements.depth &&
		_depthProviderReady && _depthProvider;
	if (attemptedDepthProvider) {
		depthValid = _depthProvider->BeginFrame(depthFrame, depth) &&
			depth.depth.IsValid(
				DXGI_FORMAT_R32_FLOAT, frame.frameId, frame.sourceExtent);
	}
	if (!depthValid) {
		depth = zeroDepth;
		depthValid = true;
		if (attemptedDepthProvider) {
			depth.depth.metadata.resetReason =
				FrameGuidanceResetReason::ProviderFailure;
			depth.depth.metadata.requiresHistoryReset = true;
		}
	}

	_view.depth = depth.depth;
	_view.motion = motion.motion;
	_view.confidence = motion.confidence;
	_view.rawDepth = depth.rawDepth;
	_view.depthResidual = depth.depthResidual;
	_view.requiresHistoryReset =
		_view.depth.metadata.requiresHistoryReset ||
		_view.motion.metadata.requiresHistoryReset ||
		_view.confidence.metadata.requiresHistoryReset;
	if (!_view.IsValidFor(frame.frameId, frame.sourceExtent)) {
		Logger::Get().Warn(fmt::format(
			"Frame Guidance coherence failure at frameId={}; using whole Zero group",
			frame.frameId));
		_view = _zeroView;
		_view.requiresHistoryReset = true;
	}
	_cachedFrameId = frame.frameId;
	_cachedRequirements = requirements;
	_hasCachedFrame = true;
	return _view;
}

}
