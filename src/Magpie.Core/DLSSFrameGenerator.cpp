#include "pch.h"
#include "DLSSFrameGenerator.h"
#include "DeviceResources.h"
#include "FrameGuidanceD3D12Interop.h"
#include "Logger.h"

#ifdef MP_ENABLE_DLSS_FRAME_GENERATION
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_dlssg.h>

namespace Magpie {

struct DLSSFrameGenerator::Impl {
	~Impl();

	ID3D11Device5* device11 = nullptr;
	ID3D11DeviceContext4* context11 = nullptr;
	winrt::com_ptr<ID3D12Device> device12;
	winrt::com_ptr<ID3D12CommandQueue> queue12;
	winrt::com_ptr<ID3D12CommandAllocator> allocator12;
	winrt::com_ptr<ID3D12GraphicsCommandList> commandList12;
	winrt::com_ptr<ID3D11Texture2D> sharedInput11;
	winrt::com_ptr<ID3D11Texture2D> sharedGenerated11;
	winrt::com_ptr<ID3D12Resource> sharedInput12;
	winrt::com_ptr<ID3D12Resource> sharedGenerated12;
	winrt::com_ptr<ID3D12Resource> zeroMotion12;
	winrt::com_ptr<ID3D12Resource> zeroDepth12;
	std::unique_ptr<FrameGuidanceD3D12Interop> guidanceInterop;
	winrt::com_ptr<ID3D12DescriptorHeap> descriptorHeap12;
	winrt::com_ptr<ID3D11Fence> fence11;
	winrt::com_ptr<ID3D12Fence> fence12;
	NVSDK_NGX_Handle* feature = nullptr;
	NVSDK_NGX_Parameter* parameters = nullptr;
	uint64_t fenceValue = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;
	uint32_t multiplier = 2;
	uint32_t nextGeneratedFrames = 1;
	DLSSFrameGenerationSettings settings{};
	FrameGuidanceFrameId lastGuidanceResetFrameId =
		std::numeric_limits<FrameGuidanceFrameId>::max();
	uint8_t lastGuidanceBinding = UINT8_MAX;
	bool ngxInitialized = false;
	bool resetHistory = true;
};

static bool NGXSucceeded(NVSDK_NGX_Result result) noexcept {
	return NVSDK_NGX_SUCCEED(result);
}

static bool WaitForFence(DLSSFrameGenerator::Impl& impl, uint64_t value) noexcept {
	if (!value || impl.fence12->GetCompletedValue() >= value) {
		return true;
	}

	wil::unique_event_nothrow event;
	if (FAILED(event.create())) {
		return false;
	}
	if (FAILED(impl.fence12->SetEventOnCompletion(value, event.get()))) {
		return false;
	}
	event.wait();
	return true;
}

static bool WaitForQueue(DLSSFrameGenerator::Impl& impl) noexcept {
	const uint64_t value = ++impl.fenceValue;
	if (FAILED(impl.queue12->Signal(impl.fence12.get(), value))) {
		return false;
	}
	return WaitForFence(impl, value);
}

DLSSFrameGenerator::Impl::~Impl() {
	if (queue12 && fence12) {
		WaitForQueue(*this);
	}
	if (feature) {
		NVSDK_NGX_D3D12_ReleaseFeature(feature);
	}
	if (parameters) {
		NVSDK_NGX_D3D12_DestroyParameters(parameters);
	}
	if (ngxInitialized && device12) {
		NVSDK_NGX_D3D12_Shutdown1(device12.get());
	}
}

static bool CreateSharedTexture(
	DLSSFrameGenerator::Impl& impl,
	const D3D11_TEXTURE2D_DESC& sourceDesc,
	bool allowUav,
	winrt::com_ptr<ID3D11Texture2D>& texture11,
	winrt::com_ptr<ID3D12Resource>& texture12
) noexcept {
	D3D11_TEXTURE2D_DESC desc = sourceDesc;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.CPUAccessFlags = 0;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
		(allowUav ? D3D11_BIND_UNORDERED_ACCESS : 0);
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	HRESULT hr = impl.device11->CreateTexture2D(&desc, nullptr, texture11.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSFG shared D3D11 texture failed", hr);
		return false;
	}

	winrt::com_ptr<IDXGIResource1> dxgiResource;
	hr = texture11->QueryInterface(IID_PPV_ARGS(dxgiResource.put()));
	if (FAILED(hr)) {
		return false;
	}

	HANDLE rawHandle = nullptr;
	hr = dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawHandle);
	if (FAILED(hr)) {
		return false;
	}
	wil::unique_handle handle(rawHandle);
	hr = impl.device12->OpenSharedHandle(handle.get(), IID_PPV_ARGS(texture12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Open DLSSFG shared texture in D3D12 failed", hr);
		return false;
	}
	return true;
}

static bool CreateZeroTexture(
	DLSSFrameGenerator::Impl& impl,
	DXGI_FORMAT format,
	winrt::com_ptr<ID3D12Resource>& resource,
	D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
	D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle,
	D3D12_RESOURCE_BARRIER& barrier
) noexcept {
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = impl.renderWidth;
	desc.Height = impl.renderHeight;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	HRESULT hr = impl.device12->CreateCommittedResource(
		&heap,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(resource.put())
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSFG virtual input texture failed", hr);
		return false;
	}

	D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = format;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	impl.device12->CreateUnorderedAccessView(resource.get(), nullptr, &uav, cpuHandle);
	static constexpr float ZERO[4]{};
	impl.commandList12->ClearUnorderedAccessViewFloat(
		gpuHandle, cpuHandle, resource.get(), ZERO, 0, nullptr);

	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition = {
		resource.get(),
		D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
	};
	return true;
}

static void SetIdentity(float matrix[4][4]) noexcept {
	for (uint32_t row = 0; row < 4; ++row) {
		for (uint32_t column = 0; column < 4; ++column) {
			matrix[row][column] = row == column ? 1.0f : 0.0f;
		}
	}
}

DLSSFrameGenerator::DLSSFrameGenerator() = default;
DLSSFrameGenerator::~DLSSFrameGenerator() = default;

bool DLSSFrameGenerator::Initialize(
	DeviceResources& resources,
	ID3D11Texture2D* input,
	FrameGuidanceExtent guidanceExtent,
	const DLSSFrameGenerationSettings& settings
) noexcept {
	_requestedSettings = settings;
	_requestedSettings.multiplier = std::clamp(settings.multiplier, 2u, 4u);
	_impl.reset();
	auto impl = std::make_unique<Impl>();
	impl->device11 = resources.GetD3DDevice();
	impl->context11 = resources.GetD3DDC();
	impl->settings = _requestedSettings;

	D3D11_TEXTURE2D_DESC inputDesc{};
	input->GetDesc(&inputDesc);
	impl->width = inputDesc.Width;
	impl->height = inputDesc.Height;
	const bool guidanceRequested = impl->settings.useMotionVectors ||
		impl->settings.useEstimatedDepth;
	const bool compatibleGuidanceExtent = guidanceRequested &&
		guidanceExtent.IsValid() &&
		guidanceExtent.width <= impl->width &&
		guidanceExtent.height <= impl->height;
	impl->renderWidth = compatibleGuidanceExtent ?
		guidanceExtent.width : impl->width;
	impl->renderHeight = compatibleGuidanceExtent ?
		guidanceExtent.height : impl->height;
	if (guidanceRequested && !compatibleGuidanceExtent) {
		Logger::Get().Warn(fmt::format(
			"DLSS FG guidance extent {}x{} is incompatible with backbuffer {}x{}; "
			"using Zero guidance at backbuffer size",
			guidanceExtent.width, guidanceExtent.height,
			impl->width, impl->height));
	}

	HRESULT hr = D3D12CreateDevice(
		resources.GetGraphicsAdapter(), D3D_FEATURE_LEVEL_11_0,
		IID_PPV_ARGS(impl->device12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSFG D3D12 device failed", hr);
		return false;
	}

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	hr = impl->device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(impl->queue12.put()));
	if (SUCCEEDED(hr)) {
		hr = impl->device12->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(impl->allocator12.put()));
	}
	if (SUCCEEDED(hr)) {
		hr = impl->device12->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, impl->allocator12.get(), nullptr,
			IID_PPV_ARGS(impl->commandList12.put()));
	}
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSFG D3D12 command objects failed", hr);
		return false;
	}

	if (!CreateSharedTexture(*impl, inputDesc, false,
		impl->sharedInput11, impl->sharedInput12) ||
		!CreateSharedTexture(*impl, inputDesc, true,
			impl->sharedGenerated11, impl->sharedGenerated12)) {
		return false;
	}

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = 2;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = impl->device12->CreateDescriptorHeap(
		&heapDesc, IID_PPV_ARGS(impl->descriptorHeap12.put()));
	if (FAILED(hr)) {
		return false;
	}
	ID3D12DescriptorHeap* heaps[]{ impl->descriptorHeap12.get() };
	impl->commandList12->SetDescriptorHeaps(1, heaps);
	const UINT stride = impl->device12->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_CPU_DESCRIPTOR_HANDLE cpu =
		impl->descriptorHeap12->GetCPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE gpu =
		impl->descriptorHeap12->GetGPUDescriptorHandleForHeapStart();
	D3D12_RESOURCE_BARRIER auxBarriers[2]{};
	if (!CreateZeroTexture(*impl, DXGI_FORMAT_R16G16_FLOAT,
		impl->zeroMotion12, cpu, gpu, auxBarriers[0])) {
		return false;
	}
	cpu.ptr += stride;
	gpu.ptr += stride;
	if (!CreateZeroTexture(*impl, DXGI_FORMAT_R32_FLOAT,
		impl->zeroDepth12, cpu, gpu, auxBarriers[1])) {
		return false;
	}
	impl->commandList12->ResourceBarrier(2, auxBarriers);

	NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(
		"7c134ab9-9677-4af5-a2b2-bca943350861",
		NVSDK_NGX_ENGINE_TYPE_CUSTOM,
		"Magpie-DLSSFG-1",
		L"logs",
		impl->device12.get()
	);
	if (!NGXSucceeded(result)) {
		Logger::Get().Error(fmt::format(
			"NVSDK_NGX_D3D12_Init for DLSSFG failed ({:#x})", (uint32_t)result));
		return false;
	}
	impl->ngxInitialized = true;

	result = NVSDK_NGX_D3D12_GetCapabilityParameters(&impl->parameters);
	if (!NGXSucceeded(result) || !impl->parameters) {
		Logger::Get().Error(fmt::format(
			"Get DLSSFG capability parameters failed ({:#x})", (uint32_t)result));
		return false;
	}

	int available = 0;
	result = NVSDK_NGX_Parameter_GetI(
		impl->parameters, NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
	if (!NGXSucceeded(result) || !available) {
		int initResult = 0;
		NVSDK_NGX_Parameter_GetI(impl->parameters,
			NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &initResult);
		Logger::Get().Error(fmt::format(
			"DLSS Frame Generation is unavailable (result={:#x})", (uint32_t)initResult));
		return false;
	}

	uint32_t maxGeneratedFrames = 1;
	if (!NGXSucceeded(NVSDK_NGX_Parameter_GetUI(
		impl->parameters,
		NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,
		&maxGeneratedFrames))) {
		maxGeneratedFrames = 1;
	}
	maxGeneratedFrames = std::clamp(maxGeneratedFrames, 1u, 3u);
	impl->multiplier = std::min(
		_requestedSettings.multiplier, maxGeneratedFrames + 1);
	if (impl->multiplier != _requestedSettings.multiplier) {
		Logger::Get().Warn(fmt::format(
			"DLSSFG {}x requested, hardware supports up to {}x; using {}x",
			_requestedSettings.multiplier,
			maxGeneratedFrames + 1, impl->multiplier));
	}
	impl->nextGeneratedFrames = impl->multiplier - 1;

	const uint32_t neverProvidedFlags =
		NVSDK_NGX_DLSSG_ResourceFlags_HUDLess |
		NVSDK_NGX_DLSSG_ResourceFlags_UI |
		NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha |
		NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField |
		NVSDK_NGX_DLSSG_ResourceFlags_OutputReal |
		NVSDK_NGX_DLSSG_ResourceFlags_OutputDisableInterpolation;
	NVSDK_NGX_Parameter_SetUI(impl->parameters,
		NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags,
		neverProvidedFlags);

	NVSDK_NGX_DLSSG_Create_Params createParams{};
	createParams.Width = impl->width;
	createParams.Height = impl->height;
	createParams.NativeBackbufferFormat = inputDesc.Format;
	createParams.RenderWidth = impl->renderWidth;
	createParams.RenderHeight = impl->renderHeight;
	createParams.DynamicResolutionScaling = false;
	result = NGX_D3D12_CREATE_DLSSG(
		impl->commandList12.get(), 1, 1, &impl->feature,
		impl->parameters, &createParams);
	if (!NGXSucceeded(result) || !impl->feature) {
		Logger::Get().Error(fmt::format(
			"NGX_D3D12_CREATE_DLSSG failed ({:#x})", (uint32_t)result));
		return false;
	}

	hr = impl->commandList12->Close();
	if (FAILED(hr)) {
		return false;
	}
	ID3D12CommandList* lists[]{ impl->commandList12.get() };
	impl->queue12->ExecuteCommandLists(1, lists);

	hr = impl->device11->CreateFence(
		0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(impl->fence11.put()));
	if (FAILED(hr)) {
		return false;
	}
	HANDLE rawFence = nullptr;
	hr = impl->fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawFence);
	if (FAILED(hr)) {
		return false;
	}
	wil::unique_handle fenceHandle(rawFence);
	hr = impl->device12->OpenSharedHandle(
		fenceHandle.get(), IID_PPV_ARGS(impl->fence12.put()));
	if (FAILED(hr) || !WaitForQueue(*impl)) {
		return false;
	}
	impl->guidanceInterop = std::make_unique<FrameGuidanceD3D12Interop>();
	if (!impl->guidanceInterop->Initialize(
		impl->device12.get(), impl->fence12.get())) {
		return false;
	}

	Logger::Get().Info(fmt::format(
		"DLSS FG_Experimental initialized: backbuffer={}x{}, render={}x{}, "
		"multiplier={}x, requestedMotion={}, requestedDepth={}",
		impl->width, impl->height, impl->renderWidth, impl->renderHeight,
		impl->multiplier, impl->settings.useMotionVectors,
		impl->settings.useEstimatedDepth));
	_impl = std::move(impl);
	return true;
}

bool DLSSFrameGenerator::Resize(
	DeviceResources& resources,
	ID3D11Texture2D* input,
	FrameGuidanceExtent guidanceExtent
) noexcept {
	return Initialize(resources, input, guidanceExtent, _requestedSettings);
}

FrameGuidanceRequirements
DLSSFrameGenerator::GetFrameGuidanceRequirements() const noexcept {
	FrameGuidanceRequirements result{ .zero = true };
	result.motion = _requestedSettings.useMotionVectors ||
		_requestedSettings.useEstimatedDepth;
	result.depth = _requestedSettings.useEstimatedDepth;
	return result;
}

uint32_t DLSSFrameGenerator::Multiplier() const noexcept {
	return _impl ? _impl->multiplier : _requestedSettings.multiplier;
}

void DLSSFrameGenerator::SetGeneratedFrameCount(uint32_t count) noexcept {
	if (_impl) {
		_impl->nextGeneratedFrames = std::min(count, _impl->multiplier - 1);
	}
}

bool DLSSFrameGenerator::Draw(
	ID3D11Texture2D* input,
	FrameGuidanceFrameId frameId,
	const FrameGuidanceView& guidance,
	const FrameGuidanceView& zeroGuidance,
	const PublishCallback& publishGeneratedFrame
) noexcept {
	if (!_impl || !_impl->feature || !_impl->parameters) {
		return false;
	}
	Impl& impl = *_impl;
	const FrameGuidanceExtent renderExtent{
		impl.renderWidth, impl.renderHeight
	};
	const FrameGuidanceView selected = SelectFrameGuidanceChannels(
		guidance, zeroGuidance, frameId, renderExtent,
		impl.settings.useMotionVectors,
		impl.settings.useEstimatedDepth);
	bool sharedGuidanceBound = false;
	bool realMotion = false;
	bool realDepth = false;
	if ((impl.settings.useMotionVectors || impl.settings.useEstimatedDepth) &&
		selected.IsValidFor(frameId, renderExtent) &&
		impl.guidanceInterop->Update(selected, frameId, renderExtent) &&
		impl.guidanceInterop->WaitForProducer(impl.context11, selected)) {
		sharedGuidanceBound = true;
		realMotion = impl.settings.useMotionVectors &&
			!selected.motion.metadata.isZero;
		realDepth = impl.settings.useEstimatedDepth &&
			!selected.depth.metadata.isZero;
	}

	const uint8_t guidanceBinding = uint8_t(realMotion) |
		(uint8_t(realDepth) << 1) |
		(uint8_t(impl.settings.useMotionVectors) << 2) |
		(uint8_t(impl.settings.useEstimatedDepth) << 3) |
		(uint8_t(sharedGuidanceBound) << 4);
	const bool bindingChanged = impl.lastGuidanceBinding != UINT8_MAX &&
		impl.lastGuidanceBinding != guidanceBinding;
	if (impl.lastGuidanceBinding != guidanceBinding) {
		Logger::Get().Info(fmt::format(
			"DLSS FG guidance frameId={}: requested motion={} depth={}, "
			"produced motion={} depth={}, bound motion={} depth={}, fallback={}",
			frameId, impl.settings.useMotionVectors,
			impl.settings.useEstimatedDepth,
			guidance.motion.metadata.valid && !guidance.motion.metadata.isZero,
			guidance.depth.metadata.valid && !guidance.depth.metadata.isZero,
			realMotion ? "real" : "zero", realDepth ? "real" : "zero",
			!sharedGuidanceBound ? "interop-or-extent-zero" :
			((impl.settings.useMotionVectors && !realMotion) ||
			 (impl.settings.useEstimatedDepth && !realDepth) ?
				"provider-zero" : "none")));
	}
	const bool guidanceReset = bindingChanged ||
		(sharedGuidanceBound && selected.requiresHistoryReset &&
			impl.lastGuidanceResetFrameId != frameId);

	impl.context11->CopyResource(impl.sharedInput11.get(), input);
	const uint64_t inputReady = ++impl.fenceValue;
	HRESULT hr = impl.context11->Signal(impl.fence11.get(), inputReady);
	if (FAILED(hr)) {
		return false;
	}
	impl.context11->Flush();
	hr = impl.queue12->Wait(impl.fence12.get(), inputReady);
	if (FAILED(hr)) {
		return false;
	}

	const bool ingestOnly = impl.resetHistory || impl.nextGeneratedFrames == 0;
	const uint32_t generatedFrameCount =
		ingestOnly ? 1 : impl.nextGeneratedFrames;
	for (uint32_t frameIndex = 1; frameIndex <= generatedFrameCount; ++frameIndex) {
		hr = impl.allocator12->Reset();
		if (SUCCEEDED(hr)) {
			hr = impl.commandList12->Reset(impl.allocator12.get(), nullptr);
		}
		if (FAILED(hr)) {
			return false;
		}

		D3D12_RESOURCE_BARRIER barriers[2]{};
		barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[0].Transition = {
			impl.sharedInput12.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
		};
		barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[1].Transition = {
			impl.sharedGenerated12.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS
		};
		impl.commandList12->ResourceBarrier(2, barriers);
		if (sharedGuidanceBound) {
			impl.guidanceInterop->Transition(
				impl.commandList12.get(), D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}

		NVSDK_NGX_D3D12_DLSSG_Eval_Params evalParams{};
		evalParams.pBackbuffer = impl.sharedInput12.get();
		evalParams.pDepth = sharedGuidanceBound ?
			impl.guidanceInterop->Depth() : impl.zeroDepth12.get();
		evalParams.pMVecs = sharedGuidanceBound ?
			impl.guidanceInterop->Motion() : impl.zeroMotion12.get();
		evalParams.pOutputInterpFrame = impl.sharedGenerated12.get();

		NVSDK_NGX_DLSSG_Opt_Eval_Params optionalParams{};
		optionalParams.multiFrameCount = generatedFrameCount;
		optionalParams.multiFrameIndex = frameIndex;
		SetIdentity(optionalParams.cameraViewToClip);
		SetIdentity(optionalParams.clipToCameraView);
		SetIdentity(optionalParams.clipToLensClip);
		SetIdentity(optionalParams.clipToPrevClip);
		SetIdentity(optionalParams.prevClipToClip);
		// Frame Guidance motion is current-to-previous in render-pixel units;
		// DLSSG consumes normalized vectors.
		optionalParams.mvecScale[0] = 1.0f / float(impl.renderWidth);
		optionalParams.mvecScale[1] = 1.0f / float(impl.renderHeight);
		optionalParams.cameraUp[1] = 1.0f;
		optionalParams.cameraRight[0] = 1.0f;
		optionalParams.cameraFwd[2] = 1.0f;
		optionalParams.cameraNear = 0.1f;
		optionalParams.cameraFar = 1000.0f;
		optionalParams.cameraFOV = 1.04719755f;
		optionalParams.cameraAspectRatio =
			float(impl.renderWidth) / float(impl.renderHeight);
		optionalParams.depthInverted = impl.settings.useEstimatedDepth;
		optionalParams.cameraMotionIncluded = realMotion;
		optionalParams.reset = impl.resetHistory || guidanceReset;
		optionalParams.motionVectorsInvalidValue = 0.0f;
		optionalParams.motionVectorsDilated = realMotion;
		optionalParams.menuDetectionEnabled = false;
		const FrameGuidanceRegion guidanceRegion = sharedGuidanceBound ?
			selected.motion.metadata.validRegion :
			FrameGuidanceRegion::Full(renderExtent);
		optionalParams.mvecsSubrectBase = {
			guidanceRegion.x, guidanceRegion.y
		};
		optionalParams.mvecsSubrectSize = {
			guidanceRegion.width, guidanceRegion.height
		};
		const FrameGuidanceRegion depthRegion = sharedGuidanceBound ?
			selected.depth.metadata.validRegion :
			FrameGuidanceRegion::Full(renderExtent);
		optionalParams.depthSubrectBase = {
			depthRegion.x, depthRegion.y
		};
		optionalParams.depthSubrectSize = {
			depthRegion.width, depthRegion.height
		};
		optionalParams.backbufferSubrectSize = {
			impl.width, impl.height
		};
		optionalParams.outputInterpSubrectSize = {
			impl.width, impl.height
		};

		const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSSG(
			impl.commandList12.get(), impl.feature, impl.parameters,
			&evalParams, &optionalParams);
		if (!NGXSucceeded(result)) {
			impl.commandList12->Close();
			Logger::Get().Error(fmt::format(
				"NGX_D3D12_EVALUATE_DLSSG failed ({:#x}, index={}/{})",
				(uint32_t)result, frameIndex, generatedFrameCount));
			return false;
		}

		for (D3D12_RESOURCE_BARRIER& barrier : barriers) {
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		}
		if (sharedGuidanceBound) {
			impl.guidanceInterop->Transition(
				impl.commandList12.get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COMMON);
		}
		impl.commandList12->ResourceBarrier(2, barriers);
		hr = impl.commandList12->Close();
		if (FAILED(hr)) {
			return false;
		}
		ID3D12CommandList* lists[]{ impl.commandList12.get() };
		impl.queue12->ExecuteCommandLists(1, lists);
		const uint64_t outputReady = ++impl.fenceValue;
		hr = impl.queue12->Signal(impl.fence12.get(), outputReady);
		if (SUCCEEDED(hr) && sharedGuidanceBound) {
			impl.guidanceInterop->MarkSubmitted(outputReady);
		}
		if (SUCCEEDED(hr)) {
			hr = impl.context11->Wait(impl.fence11.get(), outputReady);
		}
		if (FAILED(hr)) {
			return false;
		}
		if (!ingestOnly && !publishGeneratedFrame(impl.sharedGenerated11.get())) {
			return false;
		}
	}

	impl.resetHistory = false;
	impl.lastGuidanceBinding = guidanceBinding;
	if (guidanceReset) impl.lastGuidanceResetFrameId = frameId;
	return true;
}

void DLSSFrameGenerator::RequestHistoryReset() noexcept {
	if (_impl) {
		_impl->resetHistory = true;
	}
}

}

#else

namespace Magpie {

struct DLSSFrameGenerator::Impl {};
DLSSFrameGenerator::DLSSFrameGenerator() = default;
DLSSFrameGenerator::~DLSSFrameGenerator() = default;
bool DLSSFrameGenerator::Initialize(
	DeviceResources&, ID3D11Texture2D*, FrameGuidanceExtent,
	const DLSSFrameGenerationSettings&) noexcept {
	Logger::Get().Error("DLSS Frame Generation is disabled at build time");
	return false;
}
bool DLSSFrameGenerator::Resize(
	DeviceResources&, ID3D11Texture2D*, FrameGuidanceExtent) noexcept {
	return false;
}
bool DLSSFrameGenerator::Draw(
	ID3D11Texture2D*, FrameGuidanceFrameId,
	const FrameGuidanceView&, const FrameGuidanceView&,
	const PublishCallback&) noexcept {
	return false;
}
void DLSSFrameGenerator::RequestHistoryReset() noexcept {}
FrameGuidanceRequirements
DLSSFrameGenerator::GetFrameGuidanceRequirements() const noexcept { return {}; }
uint32_t DLSSFrameGenerator::Multiplier() const noexcept {
	return _requestedSettings.multiplier;
}
void DLSSFrameGenerator::SetGeneratedFrameCount(uint32_t) noexcept {}

}

#endif
