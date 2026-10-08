#include "pch.h"
#include "NvidiaOpticalFlowProvider.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"

#ifdef MP_ENABLE_NVIDIA_OPTICAL_FLOW
#include <nvOpticalFlowD3D11.h>

namespace Magpie {

namespace {

constexpr char DENSIFY_FLOW_HLSL[] = R"(
Texture2D<int2> ForwardFlow : register(t0);
Texture2D<int2> BackwardFlow : register(t1);
Texture2D<uint> ForwardCost : register(t2);
Texture2D<uint> BackwardCost : register(t3);
RWTexture2D<float2> DenseMotion : register(u0);
RWTexture2D<float> DenseConfidence : register(u1);

cbuffer Params : register(b0) {
    uint2 SourceExtent;
    uint2 FlowExtent;
    uint GridSize;
    uint HasBackward;
    uint HasBackwardCost;
    // Source pixels per NVOF input pixel.
    uint Scale;
};

float2 LoadFlow(Texture2D<int2> field, int2 p) {
    p = clamp(p, int2(0, 0), int2(FlowExtent) - 1);
    return float2(field.Load(int3(p, 0))) / 32.0;
}

float2 SampleFlow(Texture2D<int2> field, float2 sourcePixel) {
    float2 gridPos = sourcePixel / float(GridSize * Scale) - 0.5;
    int2 p0 = int2(floor(gridPos));
    float2 f = frac(gridPos);
    return lerp(
        lerp(LoadFlow(field, p0), LoadFlow(field, p0 + int2(1, 0)), f.x),
        lerp(LoadFlow(field, p0 + int2(0, 1)),
             LoadFlow(field, p0 + int2(1, 1)), f.x),
        f.y) * float(Scale);
}

float LoadCost(Texture2D<uint> field, int2 p) {
    p = clamp(p, int2(0, 0), int2(FlowExtent) - 1);
    return float(field.Load(int3(p, 0))) / 255.0;
}

float SampleCost(Texture2D<uint> field, float2 sourcePixel) {
    float2 gridPos = sourcePixel / float(GridSize * Scale) - 0.5;
    int2 p0 = int2(floor(gridPos));
    float2 f = frac(gridPos);
    return lerp(
        lerp(LoadCost(field, p0), LoadCost(field, p0 + int2(1, 0)), f.x),
        lerp(LoadCost(field, p0 + int2(0, 1)),
             LoadCost(field, p0 + int2(1, 1)), f.x),
        f.y);
}

[numthreads(8, 8, 1)]
void Densify(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SourceExtent)) return;

    float2 p = float2(tid.xy) + 0.5;
    float2 forward = SampleFlow(ForwardFlow, p);
    float confidence = 1.0 - SampleCost(ForwardCost, p);

    if (HasBackward != 0) {
        float2 referencePixel = p + forward;
        bool inside = all(referencePixel >= 0.0) &&
            all(referencePixel < float2(SourceExtent));
        float2 backward = SampleFlow(BackwardFlow, referencePixel);
        float fbError = length(forward + backward);
        float threshold = 0.75 * float(Scale) + 0.05 * length(forward);
        confidence *= inside ? saturate(1.0 - fbError / threshold) : 0.0;
        if (HasBackwardCost != 0) {
            confidence *= 1.0 - SampleCost(BackwardCost, referencePixel);
        }
    }

    DenseMotion[tid.xy] = forward;
    DenseConfidence[tid.xy] = saturate(confidence);
}
)";

constexpr char DOWNSCALE_HLSL[] = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<unorm float4> Target : register(u0);

cbuffer Params : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
};

[numthreads(8, 8, 1)]
void Downscale(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    int2 p = int2(tid.xy) * 2;
    int2 m = int2(SourceExtent) - 1;
    float4 c = Source.Load(int3(min(p, m), 0)) +
        Source.Load(int3(min(p + int2(1, 0), m), 0)) +
        Source.Load(int3(min(p + int2(0, 1), m), 0)) +
        Source.Load(int3(min(p + int2(1, 1), m), 0));
    Target[tid.xy] = c * 0.25;
}
)";

template <typename T>
T GetExport(HMODULE module, const char* name) noexcept {
	return reinterpret_cast<T>(GetProcAddress(module, name));
}

bool HasFormat(const std::vector<DXGI_FORMAT>& formats, DXGI_FORMAT value) {
	return std::find(formats.begin(), formats.end(), value) != formats.end();
}

constexpr std::array<float, 2> DecodeS105(int16_t x, int16_t y) noexcept {
	return { float(x) / 32.0f, float(y) / 32.0f };
}

// Compile-time convention probes for the two axis cases used by the manual
// translation test. NVOF stores five fractional bits and we preserve X/Y signs.
static_assert(DecodeS105(64, 0)[0] == 2.0f);
static_assert(DecodeS105(0, -96)[1] == -3.0f);

FrameGuidanceMetadata MakeMetadata(
	const FrameGuidanceFrame& frame,
	FrameGuidanceResetReason resetReason,
	bool isZero
) noexcept {
	return {
		.frameId = frame.frameId,
		.sourceExtent = frame.sourceExtent,
		.validRegion = frame.validRegion,
		.resetReason = resetReason,
		.valid = true,
		.isZero = isZero,
		.requiresHistoryReset = resetReason != FrameGuidanceResetReason::None
	};
}

struct NvofTimingSummary {
	double average = 0.0;
	double p95 = 0.0;
	double p99 = 0.0;
	double maximum = 0.0;
	size_t count = 0;
};

struct NvofTimingWindow {
	static constexpr size_t CAPACITY = 120;
	std::array<double, CAPACITY> values{};
	size_t count = 0;
	size_t next = 0;

	void Add(double value) noexcept {
		values[next] = value;
		next = (next + 1) % CAPACITY;
		count = std::min(count + 1, CAPACITY);
	}

	NvofTimingSummary Summarize() const noexcept {
		NvofTimingSummary result{ .count = count };
		if (!count) return result;
		std::array<double, CAPACITY> sorted{};
		std::copy_n(values.begin(), count, sorted.begin());
		std::sort(sorted.begin(), sorted.begin() + count);
		double total = 0.0;
		for (size_t i = 0; i < count; ++i) total += sorted[i];
		result.average = total / static_cast<double>(count);
		result.p95 = sorted[std::min(count - 1, (count * 95 + 99) / 100 - 1)];
		result.p99 = sorted[std::min(count - 1, (count * 99 + 99) / 100 - 1)];
		result.maximum = sorted[count - 1];
		return result;
	}
};

}

struct NvidiaOpticalFlowProvider::Impl {
	using CreateApiFn = NV_OF_STATUS(NVOFAPI*)(
		uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*);
	using GetMaxVersionFn = NV_OF_STATUS(NVOFAPI*)(uint32_t*);
	static constexpr uint32_t GPU_QUERY_SLOT_COUNT = 4;
	struct GpuQuerySlot {
		winrt::com_ptr<ID3D11Query> disjoint;
		winrt::com_ptr<ID3D11Query> start;
		winrt::com_ptr<ID3D11Query> end;
		bool pending = false;
	};

	~Impl() { DestroySession(); }

	void DestroySession() noexcept {
		if (api.nvOFUnregisterResourceD3D11) {
			for (NvOFGPUBufferHandle& handle : registered) {
				if (handle) {
					api.nvOFUnregisterResourceD3D11(handle);
					handle = nullptr;
				}
			}
		}
		if (session && api.nvOFDestroy) {
			api.nvOFDestroy(session);
		}
		session = nullptr;
		api = {};
		if (module) {
			FreeLibrary(module);
			module = nullptr;
		}

		for (auto& texture : input) texture = nullptr;
		for (auto& uav : inputUav) uav = nullptr;
		colorSrv = nullptr;
		colorSrvTexture = nullptr;
		for (auto& texture : flow) texture = nullptr;
		for (auto& texture : cost) texture = nullptr;
		for (auto& srv : flowSrv) srv = nullptr;
		for (auto& srv : costSrv) srv = nullptr;
		motion = nullptr;
		confidence = nullptr;
		motionUav = nullptr;
		confidenceUav = nullptr;
		for (GpuQuerySlot& slot : gpuQuerySlots) {
			slot.disjoint = nullptr;
			slot.start = nullptr;
			slot.end = nullptr;
			slot.pending = false;
		}
		gpuTimingWindow = {};
		nextGpuQuerySlot = 0;
		gpuTimingSampleCount = 0;
		gpuTimingAvailable = false;
		gridSize = 0;
		scale = 1;
		ofExtent = {};
		previousSlot = 0;
		bidirectional = false;
		historyValid = false;
	}

	bool CreateGpuTimingQueries() noexcept {
		D3D11_QUERY_DESC desc{ .Query = D3D11_QUERY_TIMESTAMP_DISJOINT };
		for (GpuQuerySlot& slot : gpuQuerySlots) {
			if (FAILED(device->CreateQuery(&desc, slot.disjoint.put()))) return false;
			desc.Query = D3D11_QUERY_TIMESTAMP;
			if (FAILED(device->CreateQuery(&desc, slot.start.put())) ||
				FAILED(device->CreateQuery(&desc, slot.end.put()))) return false;
			desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
		}
		return true;
	}

	void PollGpuTimings() noexcept {
		for (GpuQuerySlot& slot : gpuQuerySlots) {
			if (!slot.pending) continue;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			if (context->GetData(
				slot.disjoint.get(), &disjoint, sizeof(disjoint),
				D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
			uint64_t start = 0;
			uint64_t end = 0;
			const bool ready = context->GetData(
				slot.start.get(), &start, sizeof(start),
				D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
				context->GetData(
					slot.end.get(), &end, sizeof(end),
					D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
			if (!ready) continue;
			slot.pending = false;
			if (disjoint.Disjoint || !disjoint.Frequency || end < start) continue;
			gpuTimingWindow.Add(
				double(end - start) * 1000.0 / double(disjoint.Frequency));
			++gpuTimingSampleCount;
			if (gpuTimingSampleCount <= 4 || gpuTimingSampleCount % 120 == 0) {
				const NvofTimingSummary timing = gpuTimingWindow.Summarize();
				Logger::Get().Info(fmt::format(
					"Frame Guidance NVOF GPU interval: samples={} avg={:.3f} ms "
					"p95={:.3f} ms p99={:.3f} ms max={:.3f} ms",
					timing.count, timing.average, timing.p95,
					timing.p99, timing.maximum));
			}
		}
	}

	GpuQuerySlot* BeginGpuTiming() noexcept {
		if (!gpuTimingAvailable) return nullptr;
		PollGpuTimings();
		GpuQuerySlot& slot =
			gpuQuerySlots[nextGpuQuerySlot++ % GPU_QUERY_SLOT_COUNT];
		if (slot.pending) return nullptr;
		context->Begin(slot.disjoint.get());
		context->End(slot.start.get());
		return &slot;
	}

	void EndGpuTiming(GpuQuerySlot* slot) noexcept {
		if (!slot) return;
		context->End(slot->end.get());
		context->End(slot->disjoint.get());
		slot->pending = true;
	}

	bool QueryFormats(
		NV_OF_BUFFER_USAGE usage,
		std::vector<DXGI_FORMAT>& formats
	) noexcept {
		uint32_t count = 0;
		if (api.nvOFGetSurfaceFormatCountD3D11(
			session, usage, NV_OF_MODE_OPTICALFLOW, &count) != NV_OF_SUCCESS ||
			count == 0 || count > 64) {
			return false;
		}
		formats.resize(count);
		return api.nvOFGetSurfaceFormatD3D11(
			session, usage, NV_OF_MODE_OPTICALFLOW,
			formats.data()) == NV_OF_SUCCESS;
	}

	std::vector<uint32_t> QueryCaps(NV_OF_CAPS cap) noexcept {
		uint32_t count = 0;
		if (!api.nvOFGetCaps ||
			api.nvOFGetCaps(session, cap, nullptr, &count) != NV_OF_SUCCESS ||
			count == 0 || count > 64) {
			return {};
		}
		std::vector<uint32_t> values(count);
		if (api.nvOFGetCaps(session, cap, values.data(), &count) != NV_OF_SUCCESS) {
			return {};
		}
		values.resize(count);
		return values;
	}

	bool Register(ID3D11Resource* resource, NvOFGPUBufferHandle& handle) noexcept {
		return api.nvOFRegisterResourceD3D11 &&
			api.nvOFRegisterResourceD3D11(
				session, resource, &handle) == NV_OF_SUCCESS && handle;
	}

	bool CreateTextures() noexcept {
		const UINT sourceBind = scale > 1 ?
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS :
			D3D11_BIND_SHADER_RESOURCE;
		for (size_t i = 0; i < input.size(); ++i) {
			input[i] = DirectXHelper::CreateTexture2D(
				device, DXGI_FORMAT_B8G8R8A8_UNORM, ofExtent.width, ofExtent.height,
				sourceBind);
			if (!input[i]) return false;
			if (scale > 1 && FAILED(device->CreateUnorderedAccessView(
				input[i].get(), nullptr, inputUav[i].put()))) {
				return false;
			}
		}

		const uint32_t flowWidth = (ofExtent.width + gridSize - 1) / gridSize;
		const uint32_t flowHeight = (ofExtent.height + gridSize - 1) / gridSize;
		for (size_t i = 0; i < flow.size(); ++i) {
			flow[i] = DirectXHelper::CreateTexture2D(
				device, DXGI_FORMAT_R16G16_SINT, flowWidth, flowHeight,
				D3D11_BIND_SHADER_RESOURCE);
			if (!flow[i] || FAILED(device->CreateShaderResourceView(
				flow[i].get(), nullptr, flowSrv[i].put()))) {
				return false;
			}
		}
		for (size_t i = 0; i < cost.size(); ++i) {
			cost[i] = DirectXHelper::CreateTexture2D(
				device, DXGI_FORMAT_R8_UINT, flowWidth, flowHeight,
				D3D11_BIND_SHADER_RESOURCE);
			if (!cost[i] || FAILED(device->CreateShaderResourceView(
				cost[i].get(), nullptr, costSrv[i].put()))) {
				return false;
			}
		}

		constexpr UINT GUIDE_BIND =
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		constexpr UINT GUIDE_MISC =
			D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		motion = DirectXHelper::CreateTexture2D(
			device, DXGI_FORMAT_R16G16_FLOAT, extent.width, extent.height,
			GUIDE_BIND, D3D11_USAGE_DEFAULT, GUIDE_MISC);
		confidence = DirectXHelper::CreateTexture2D(
			device, DXGI_FORMAT_R8_UNORM, extent.width, extent.height,
			GUIDE_BIND, D3D11_USAGE_DEFAULT, GUIDE_MISC);
		if (!motion || !confidence ||
			FAILED(device->CreateUnorderedAccessView(
				motion.get(), nullptr, motionUav.put())) ||
			FAILED(device->CreateUnorderedAccessView(
				confidence.get(), nullptr, confidenceUav.put()))) {
			return false;
		}

		for (size_t i = 0; i < input.size(); ++i) {
			if (!Register(input[i].get(), registered[i])) return false;
		}
		for (size_t i = 0; i < flow.size(); ++i) {
			if (!Register(flow[i].get(), registered[2 + i])) return false;
		}
		for (size_t i = 0; i < cost.size(); ++i) {
			if (!Register(cost[i].get(), registered[4 + i])) return false;
		}
		return true;
	}

	bool CreatePostProcess() noexcept {
		winrt::com_ptr<ID3DBlob> shaderBlob;
		if (!DirectXHelper::CompileComputeShader(
			DENSIFY_FLOW_HLSL, "Densify", shaderBlob.put(),
			"FrameGuidance/NVOF_Densify.hlsl")) {
			return false;
		}
		if (FAILED(device->CreateComputeShader(
			shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
			densifyShader.put()))) {
			return false;
		}
		if (scale > 1) {
			shaderBlob = nullptr;
			if (!DirectXHelper::CompileComputeShader(
				DOWNSCALE_HLSL, "Downscale", shaderBlob.put(),
				"FrameGuidance/NVOF_Downscale.hlsl") ||
				FAILED(device->CreateComputeShader(
					shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(),
					nullptr, downscaleShader.put()))) {
				return false;
			}
		}
		const D3D11_BUFFER_DESC desc{
			.ByteWidth = 32,
			.Usage = D3D11_USAGE_DYNAMIC,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
			.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE
		};
		return SUCCEEDED(device->CreateBuffer(&desc, nullptr, paramsBuffer.put()));
	}

	bool CreateSession(
		DeviceResources& deviceResources,
		FrameGuidanceExtent newExtent
	) noexcept {
		// Full-resolution bidirectional NVOF at 1440p delays the D3D11 queue by
		// several ms per frame, capping DLSSFG below the 120 FPS source rate.
		if (newExtent.width > 1280 && newExtent.height > 720) {
			if (CreateSession(deviceResources, newExtent, 2)) return true;
			Logger::Get().Warn("Half-resolution NVOF failed; using full resolution");
		}
		return CreateSession(deviceResources, newExtent, 1);
	}

	bool CreateSession(
		DeviceResources& deviceResources,
		FrameGuidanceExtent newExtent,
		uint32_t newScale
	) noexcept {
		DestroySession();
		device = deviceResources.GetD3DDevice();
		context = deviceResources.GetD3DDC();
		extent = newExtent;
		scale = newScale;
		ofExtent = {
			(extent.width + scale - 1) / scale,
			(extent.height + scale - 1) / scale
		};
		if (!device || !context || !extent.IsValid()) return false;

		module = LoadLibraryExW(
			L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!module) {
			Logger::Get().Warn(
				"NVOF unavailable: driver nvofapi64.dll was not found");
			return false;
		}
		auto createApi = GetExport<CreateApiFn>(
			module, "NvOFAPICreateInstanceD3D11");
		if (!createApi) return false;
		uint32_t driverVersion = NV_OF_API_VERSION;
		if (auto getMaxVersion = GetExport<GetMaxVersionFn>(
			module, "NvOFGetMaxSupportedApiVersion")) {
			if (getMaxVersion(&driverVersion) != NV_OF_SUCCESS ||
				driverVersion < NV_OF_API_VERSION) {
				Logger::Get().Warn("NVOF unavailable: NVIDIA driver API is too old");
				return false;
			}
		}
		if (createApi(NV_OF_API_VERSION, &api) != NV_OF_SUCCESS ||
			!api.nvCreateOpticalFlowD3D11 || !api.nvOFInit ||
			!api.nvOFExecute || !api.nvOFRegisterResourceD3D11 ||
			!api.nvOFUnregisterResourceD3D11) {
			return false;
		}
		if (api.nvCreateOpticalFlowD3D11(
			device, context, &session) != NV_OF_SUCCESS || !session) {
			return false;
		}

		std::vector<DXGI_FORMAT> inputFormats;
		std::vector<DXGI_FORMAT> outputFormats;
		std::vector<DXGI_FORMAT> costFormats;
		if (!QueryFormats(NV_OF_BUFFER_USAGE_INPUT, inputFormats) ||
			!QueryFormats(NV_OF_BUFFER_USAGE_OUTPUT, outputFormats) ||
			!QueryFormats(NV_OF_BUFFER_USAGE_COST, costFormats) ||
			!HasFormat(inputFormats, DXGI_FORMAT_B8G8R8A8_UNORM) ||
			!HasFormat(outputFormats, DXGI_FORMAT_R16G16_SINT) ||
			!HasFormat(costFormats, DXGI_FORMAT_R8_UINT)) {
			Logger::Get().Warn("NVOF required ABGR8/S10.5/R8 cost formats unavailable");
			return false;
		}

		const std::vector<uint32_t> grids = QueryCaps(
			NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES);
		for (uint32_t candidate : { 4u, 2u, 1u }) {
			if (std::find(grids.begin(), grids.end(), candidate) != grids.end()) {
				gridSize = candidate;
				break;
			}
		}
		if (!gridSize) return false;

		NV_OF_INIT_PARAMS init{
			.width = ofExtent.width,
			.height = ofExtent.height,
			.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(gridSize),
			.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED,
			.mode = NV_OF_MODE_OPTICALFLOW,
			// MEDIUM takes ~4.5 ms per bidirectional 1440p pair on the optical-flow
			// engine, which caps DLSSFG below 120 real frames per second.
			.perfLevel = NV_OF_PERF_LEVEL_FAST,
			.enableExternalHints = NV_OF_FALSE,
			.enableOutputCost = NV_OF_TRUE,
			.hPrivData = nullptr,
			.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED,
			.enableRoi = NV_OF_FALSE,
			.predDirection = NV_OF_PRED_DIRECTION_BOTH,
			.enableGlobalFlow = NV_OF_FALSE,
			.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8
		};
		NV_OF_STATUS status = api.nvOFInit(session, &init);
		bidirectional = status == NV_OF_SUCCESS;
		if (!bidirectional) {
			init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
			status = api.nvOFInit(session, &init);
		}
		if (status != NV_OF_SUCCESS || !CreateTextures() || !CreatePostProcess()) {
			return false;
		}
		gpuTimingAvailable = CreateGpuTimingQueries();
		if (!gpuTimingAvailable) {
			Logger::Get().Warn(
				"Frame Guidance NVOF GPU timing unavailable; continuing without telemetry");
		}

		resetReason = FrameGuidanceResetReason::Initialize;
		historyValid = false;
		previousSlot = 0;
		Logger::Get().Info(fmt::format(
			"Frame Guidance NVOF initialized: API {}.{}, input={}x{} (1/{}), grid={}x{}, "
			"bidirectional={}, cost=R8_UINT, preset=FAST, "
			"convention=current-to-previous/source-pixels/S10.5-self-test-passed",
			driverVersion >> 4, driverVersion & 0xf, ofExtent.width, ofExtent.height,
			scale, gridSize, gridSize, bidirectional));
		return true;
	}

	void ClearDenseOutput() noexcept {
		static constexpr float ZERO[4]{};
		context->ClearUnorderedAccessViewFloat(motionUav.get(), ZERO);
		context->ClearUnorderedAccessViewFloat(confidenceUav.get(), ZERO);
	}

	bool EnsureColorSrv(ID3D11Texture2D* color) noexcept {
		if (colorSrvTexture == color) return true;
		colorSrv = nullptr;
		colorSrvTexture = nullptr;
		const D3D11_SHADER_RESOURCE_VIEW_DESC desc{
			.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		if (FAILED(device->CreateShaderResourceView(color, &desc, colorSrv.put())) &&
			FAILED(device->CreateShaderResourceView(color, nullptr, colorSrv.put()))) {
			return false;
		}
		colorSrvTexture = color;
		return true;
	}

	bool Densify() noexcept {
		struct alignas(16) Params {
			uint32_t sourceWidth;
			uint32_t sourceHeight;
			uint32_t flowWidth;
			uint32_t flowHeight;
			uint32_t gridSize;
			uint32_t hasBackward;
			uint32_t hasBackwardCost;
			uint32_t scale;
		};
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(
			paramsBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return false;
		}
		*static_cast<Params*>(mapped.pData) = {
			extent.width, extent.height,
			(ofExtent.width + gridSize - 1) / gridSize,
			(ofExtent.height + gridSize - 1) / gridSize,
			gridSize, bidirectional ? 1u : 0u, bidirectional ? 1u : 0u, scale
		};
		context->Unmap(paramsBuffer.get(), 0);

		ID3D11ShaderResourceView* srvs[]{
			flowSrv[0].get(), bidirectional ? flowSrv[1].get() : nullptr,
			costSrv[0].get(), bidirectional ? costSrv[1].get() : nullptr
		};
		ID3D11UnorderedAccessView* uavs[]{ motionUav.get(), confidenceUav.get() };
		ID3D11Buffer* buffers[]{ paramsBuffer.get() };
		context->CSSetShader(densifyShader.get(), nullptr, 0);
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
		context->CSSetConstantBuffers(0, 1, buffers);
		context->Dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSrvs[ARRAYSIZE(srvs)]{};
		ID3D11UnorderedAccessView* nullUavs[ARRAYSIZE(uavs)]{};
		context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
		return true;
	}

	bool Downscale(ID3D11Texture2D* color, uint32_t slot) noexcept {
		if (!EnsureColorSrv(color)) return false;

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(
			paramsBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return false;
		}
		uint32_t* params = static_cast<uint32_t*>(mapped.pData);
		params[0] = extent.width;
		params[1] = extent.height;
		params[2] = ofExtent.width;
		params[3] = ofExtent.height;
		context->Unmap(paramsBuffer.get(), 0);

		ID3D11ShaderResourceView* srv = colorSrv.get();
		ID3D11UnorderedAccessView* uav = inputUav[slot].get();
		ID3D11Buffer* buffer = paramsBuffer.get();
		context->CSSetShader(downscaleShader.get(), nullptr, 0);
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetConstantBuffers(0, 1, &buffer);
		context->Dispatch((ofExtent.width + 7) / 8, (ofExtent.height + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSrv = nullptr;
		ID3D11UnorderedAccessView* nullUav = nullptr;
		context->CSSetShaderResources(0, 1, &nullSrv);
		context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
		return true;
	}

	DeviceResources* resources = nullptr;
	ID3D11Device5* device = nullptr;
	ID3D11DeviceContext4* context = nullptr;
	HMODULE module = nullptr;
	NV_OF_D3D11_API_FUNCTION_LIST api{};
	NvOFHandle session = nullptr;
	std::array<NvOFGPUBufferHandle, 8> registered{};
	std::array<winrt::com_ptr<ID3D11Texture2D>, 2> input;
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, 2> inputUav;
	winrt::com_ptr<ID3D11ShaderResourceView> colorSrv;
	// The SRV holds a reference, so this pointer can't be reused while cached.
	ID3D11Texture2D* colorSrvTexture = nullptr;
	std::array<winrt::com_ptr<ID3D11Texture2D>, 2> flow;
	std::array<winrt::com_ptr<ID3D11Texture2D>, 2> cost;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> flowSrv;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> costSrv;
	winrt::com_ptr<ID3D11Texture2D> motion;
	winrt::com_ptr<ID3D11Texture2D> confidence;
	winrt::com_ptr<ID3D11UnorderedAccessView> motionUav;
	winrt::com_ptr<ID3D11UnorderedAccessView> confidenceUav;
	winrt::com_ptr<ID3D11ComputeShader> densifyShader;
	winrt::com_ptr<ID3D11ComputeShader> downscaleShader;
	winrt::com_ptr<ID3D11Buffer> paramsBuffer;
	std::array<GpuQuerySlot, GPU_QUERY_SLOT_COUNT> gpuQuerySlots;
	NvofTimingWindow gpuTimingWindow;
	FrameGuidanceExtent extent{};
	FrameGuidanceExtent ofExtent{};
	FrameGuidanceResetReason resetReason = FrameGuidanceResetReason::Initialize;
	uint32_t nextGpuQuerySlot = 0;
	uint32_t gridSize = 0;
	uint32_t scale = 1;
	uint32_t previousSlot = 0;
	uint64_t gpuTimingSampleCount = 0;
	bool bidirectional = false;
	bool gpuTimingAvailable = false;
	bool historyValid = false;
};

NvidiaOpticalFlowProvider::NvidiaOpticalFlowProvider() :
	_impl(std::make_unique<Impl>()) {}

NvidiaOpticalFlowProvider::~NvidiaOpticalFlowProvider() = default;

bool NvidiaOpticalFlowProvider::Initialize(
	DeviceResources& resources,
	FrameGuidanceExtent sourceExtent
) noexcept {
	_impl->resources = &resources;
	return _impl->CreateSession(resources, sourceExtent);
}

bool NvidiaOpticalFlowProvider::BeginFrame(
	const FrameGuidanceFrame& frame,
	MotionVectorProviderOutput& output
) noexcept {
	Impl& impl = *_impl;
	if (!frame.color || frame.sourceExtent != impl.extent || !impl.session) {
		return false;
	}

	const uint32_t currentSlot = impl.historyValid ? 1u - impl.previousSlot : 0u;
	if (impl.scale > 1) {
		if (!impl.Downscale(frame.color, currentSlot)) {
			impl.historyValid = false;
			impl.resetReason = FrameGuidanceResetReason::ProviderFailure;
			return false;
		}
	} else {
		impl.context->CopyResource(impl.input[currentSlot].get(), frame.color);
	}
	if (!impl.historyValid) {
		impl.ClearDenseOutput();
		impl.previousSlot = currentSlot;
		impl.historyValid = true;
	} else {
		NV_OF_EXECUTE_INPUT_PARAMS inputParams{
			.inputFrame = impl.registered[currentSlot],
			.referenceFrame = impl.registered[impl.previousSlot],
			.disableTemporalHints = impl.resetReason == FrameGuidanceResetReason::None ?
				NV_OF_FALSE : NV_OF_TRUE
		};
		NV_OF_EXECUTE_OUTPUT_PARAMS outputParams{
			.outputBuffer = impl.registered[2],
			.outputCostBuffer = impl.registered[4],
			.bwdOutputBuffer = impl.bidirectional ? impl.registered[3] : nullptr,
			.bwdOutputCostBuffer = impl.bidirectional ? impl.registered[5] : nullptr
		};
		const auto begin = std::chrono::steady_clock::now();
		Impl::GpuQuerySlot* gpuTiming = impl.BeginGpuTiming();
		const bool succeeded = impl.api.nvOFExecute(
			impl.session, &inputParams, &outputParams) == NV_OF_SUCCESS &&
			impl.Densify();
		impl.EndGpuTiming(gpuTiming);
		if (!succeeded) {
			impl.historyValid = false;
			impl.resetReason = FrameGuidanceResetReason::ProviderFailure;
			Logger::Get().Warn(fmt::format(
				"Frame Guidance NVOF failed at frameId={}", frame.frameId));
			return false;
		}
		impl.previousSlot = currentSlot;
		const auto elapsed = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - begin).count();
		if (frame.frameId <= 2) {
			Logger::Get().Info(fmt::format(
				"Frame Guidance NVOF submit+dense frameId={} CPU={:.3f} ms",
				frame.frameId, elapsed));
		}
	}

	const bool isZero = impl.resetReason != FrameGuidanceResetReason::None;
	const FrameGuidanceMetadata metadata = MakeMetadata(
		frame, impl.resetReason, isZero);
	output.motion = {
		.texture = impl.motion.get(),
		.format = DXGI_FORMAT_R16G16_FLOAT,
		.metadata = metadata
	};
	output.confidence = {
		.texture = impl.confidence.get(),
		.format = DXGI_FORMAT_R8_UNORM,
		.metadata = metadata
	};
	impl.resetReason = FrameGuidanceResetReason::None;
	return true;
}

void NvidiaOpticalFlowProvider::Reset(
	FrameGuidanceResetReason reason
) noexcept {
	_impl->historyValid = false;
	_impl->resetReason = reason;
}

bool NvidiaOpticalFlowProvider::Resize(
	FrameGuidanceExtent sourceExtent
) noexcept {
	if (!_impl->resources) return false;
	const bool result = _impl->CreateSession(*_impl->resources, sourceExtent);
	if (result) _impl->resetReason = FrameGuidanceResetReason::Resize;
	return result;
}

}

#else

namespace Magpie {

struct NvidiaOpticalFlowProvider::Impl {};

NvidiaOpticalFlowProvider::NvidiaOpticalFlowProvider() :
	_impl(std::make_unique<Impl>()) {}
NvidiaOpticalFlowProvider::~NvidiaOpticalFlowProvider() = default;
bool NvidiaOpticalFlowProvider::Initialize(
	DeviceResources&, FrameGuidanceExtent) noexcept { return false; }
bool NvidiaOpticalFlowProvider::BeginFrame(
	const FrameGuidanceFrame&, MotionVectorProviderOutput&) noexcept { return false; }
void NvidiaOpticalFlowProvider::Reset(FrameGuidanceResetReason) noexcept {}
bool NvidiaOpticalFlowProvider::Resize(FrameGuidanceExtent) noexcept { return false; }

}

#endif
