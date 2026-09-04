#include "pch.h"
#include "HalfResOpticalFlow.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"

namespace Magpie {

static constexpr char FLOW_HLSL[] = R"(
cbuffer FlowConstants : register(b0) {
    uint2 FullSize;
    uint2 HalfSize;
};
Texture2D<float4> CurrentColor : register(t0);
Texture2D<float4> PreviousColor : register(t1);
RWTexture2D<float2> HalfFlowOut : register(u0);
RWTexture2D<float> HalfConfidenceOut : register(u1);
RWTexture2D<float> ActivityOut : register(u1);

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
groupshared float ActivitySamples[256];

[numthreads(16, 16, 1)]
void MeasureActivity(uint3 tid : SV_GroupThreadID, uint index : SV_GroupIndex) {
    float2 sampleUv = (float2(tid.xy) + 0.5) / 16.0;
    int2 p = min(int2(sampleUv * float2(FullSize)), int2(FullSize) - 1);
    ActivitySamples[index] = abs(
        Luma(CurrentColor.Load(int3(p, 0)).rgb) -
        Luma(PreviousColor.Load(int3(p, 0)).rgb));
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 128; stride != 0; stride >>= 1) {
        if (index < stride) ActivitySamples[index] += ActivitySamples[index + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) ActivityOut[uint2(0, 0)] = ActivitySamples[0] / 256.0;
}

int2 ClampPixel(int2 p) { return clamp(p, int2(0, 0), int2(FullSize) - 1); }
float SampleLuma(Texture2D<float4> tex, int2 p) { return Luma(tex.Load(int3(ClampPixel(p), 0)).rgb); }

[numthreads(8, 8, 1)]
void EstimateHalf(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= HalfSize)) return;
    int2 p = min(int2(tid.xy * 2 + 1), int2(FullSize) - 1);
    static const int2 taps[5] = {
        int2(0,0), int2(-2,0), int2(2,0), int2(0,-2), int2(0,2)
    };
    float bestError = 3.402823e+38;
    int2 bestOffset = int2(0, 0);
    [unroll] for (int y = -2; y <= 2; ++y) {
        [unroll] for (int x = -2; x <= 2; ++x) {
            // The flow grid is half-resolution, but vectors are expressed in
            // full-resolution pixels. Search every integer pixel so common
            // 1 px/frame video motion is representable.
            int2 candidate = int2(x, y);
            float error = 0.0;
            [unroll] for (int i = 0; i < 5; ++i) {
                float a = SampleLuma(CurrentColor, p + taps[i]);
                float b = SampleLuma(PreviousColor, p + candidate + taps[i]);
                float d = a - b;
                error += d * d;
            }
            // Prefer smaller motion when candidates are nearly equivalent.
            error += dot(float2(candidate), float2(candidate)) * 0.000002;
            if (error < bestError) { bestError = error; bestOffset = candidate; }
        }
    }
    HalfFlowOut[tid.xy] = float2(bestOffset);
    // Convert the winning block error into a conservative confidence value.
    // Exact/static matches approach one; ambiguous or changed regions approach
    // zero so depth reprojection does not trust a bad vector.
    HalfConfidenceOut[tid.xy] = saturate(1.0 - sqrt(bestError / 5.0) * 6.0);
}

Texture2D<float2> HalfFlowIn : register(t0);
Texture2D<float> HalfConfidenceIn : register(t1);
RWTexture2D<float2> FullFlowOut : register(u0);
RWTexture2D<float> FullConfidenceOut : register(u1);

[numthreads(8, 8, 1)]
void UpsampleFlow(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= FullSize)) return;
    float2 hp = (float2(tid.xy) + 0.5) * 0.5 - 0.5;
    int2 p0 = int2(floor(hp));
    float2 f = frac(hp);
    int2 hi = int2(HalfSize) - 1;
    float2 a = HalfFlowIn.Load(int3(clamp(p0, int2(0,0), hi), 0));
    float2 b = HalfFlowIn.Load(int3(clamp(p0 + int2(1,0), int2(0,0), hi), 0));
    float2 c = HalfFlowIn.Load(int3(clamp(p0 + int2(0,1), int2(0,0), hi), 0));
    float2 d = HalfFlowIn.Load(int3(clamp(p0 + int2(1,1), int2(0,0), hi), 0));
    FullFlowOut[tid.xy] = lerp(lerp(a,b,f.x), lerp(c,d,f.x), f.y);
    float ca = HalfConfidenceIn.Load(int3(clamp(p0, int2(0,0), hi), 0));
    float cb = HalfConfidenceIn.Load(int3(clamp(p0 + int2(1,0), int2(0,0), hi), 0));
    float cc = HalfConfidenceIn.Load(int3(clamp(p0 + int2(0,1), int2(0,0), hi), 0));
    float cd = HalfConfidenceIn.Load(int3(clamp(p0 + int2(1,1), int2(0,0), hi), 0));
    FullConfidenceOut[tid.xy] = lerp(lerp(ca,cb,f.x), lerp(cc,cd,f.x), f.y);
}
)";

bool HalfResOpticalFlow::Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
	ID3D11Texture2D* input) noexcept {
	_device = device;
	_context = context;
	D3D11_TEXTURE2D_DESC desc{};
	input->GetDesc(&desc);
	_width = desc.Width;
	_height = desc.Height;
	_halfWidth = (_width + 1) / 2;
	_halfHeight = (_height + 1) / 2;

	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = 0;
	desc.CPUAccessFlags = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	HRESULT hr = _device->CreateTexture2D(&desc, nullptr, _previous.put());
	if (SUCCEEDED(hr)) hr = _device->CreateShaderResourceView(_previous.get(), nullptr, _previousSrv.put());
	if (FAILED(hr) || !_CreateInputSrv(input)) {
		Logger::Get().ComError("Create optical-flow history resources failed", hr);
		return false;
	}

	_halfFlow = DirectXHelper::CreateTexture2D(_device, DXGI_FORMAT_R16G16_FLOAT,
		_halfWidth, _halfHeight, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	_halfConfidence = DirectXHelper::CreateTexture2D(_device, DXGI_FORMAT_R8_UNORM,
		_halfWidth, _halfHeight, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	constexpr UINT GUIDE_BIND =
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	constexpr UINT GUIDE_MISC =
		D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	_fullFlow = DirectXHelper::CreateTexture2D(_device, DXGI_FORMAT_R16G16_FLOAT,
		_width, _height, GUIDE_BIND, D3D11_USAGE_DEFAULT, GUIDE_MISC);
	_fullConfidence = DirectXHelper::CreateTexture2D(_device, DXGI_FORMAT_R8_UNORM,
		_width, _height, GUIDE_BIND, D3D11_USAGE_DEFAULT, GUIDE_MISC);
	_activity = DirectXHelper::CreateTexture2D(_device, DXGI_FORMAT_R32_FLOAT,
		1, 1, D3D11_BIND_UNORDERED_ACCESS);
	D3D11_TEXTURE2D_DESC activityReadbackDesc{};
	activityReadbackDesc.Width = 1;
	activityReadbackDesc.Height = 1;
	activityReadbackDesc.MipLevels = 1;
	activityReadbackDesc.ArraySize = 1;
	activityReadbackDesc.Format = DXGI_FORMAT_R32_FLOAT;
	activityReadbackDesc.SampleDesc.Count = 1;
	activityReadbackDesc.Usage = D3D11_USAGE_STAGING;
	activityReadbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	hr = _device->CreateTexture2D(
		&activityReadbackDesc, nullptr, _activityReadback.put());
	if (!_halfFlow || !_halfConfidence || !_fullFlow || !_fullConfidence ||
		!_activity || !_activityReadback || FAILED(hr)) return false;
	hr = _device->CreateShaderResourceView(_halfFlow.get(), nullptr, _halfFlowSrv.put());
	if (SUCCEEDED(hr)) hr = _device->CreateUnorderedAccessView(_halfFlow.get(), nullptr, _halfFlowUav.put());
	if (SUCCEEDED(hr)) hr = _device->CreateShaderResourceView(
		_halfConfidence.get(), nullptr, _halfConfidenceSrv.put());
	if (SUCCEEDED(hr)) hr = _device->CreateUnorderedAccessView(
		_halfConfidence.get(), nullptr, _halfConfidenceUav.put());
	if (SUCCEEDED(hr)) hr = _device->CreateUnorderedAccessView(_fullFlow.get(), nullptr, _fullFlowUav.put());
	if (SUCCEEDED(hr)) hr = _device->CreateUnorderedAccessView(
		_fullConfidence.get(), nullptr, _fullConfidenceUav.put());
	if (SUCCEEDED(hr)) hr = _device->CreateUnorderedAccessView(
		_activity.get(), nullptr, _activityUav.put());
	if (FAILED(hr)) return false;

	winrt::com_ptr<ID3DBlob> blob;
	if (!DirectXHelper::CompileComputeShader(FLOW_HLSL, "EstimateHalf", blob.put(), "HalfResOpticalFlow")) return false;
	hr = _device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, _estimateShader.put());
	blob = nullptr;
	if (FAILED(hr) || !DirectXHelper::CompileComputeShader(FLOW_HLSL, "UpsampleFlow", blob.put(), "HalfResOpticalFlow")) return false;
	hr = _device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, _upsampleShader.put());
	blob = nullptr;
	if (FAILED(hr) || !DirectXHelper::CompileComputeShader(
		FLOW_HLSL, "MeasureActivity", blob.put(), "HalfResOpticalFlow")) return false;
	hr = _device->CreateComputeShader(
		blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, _activityShader.put());
	if (FAILED(hr)) return false;

	struct Constants { UINT full[2]; UINT half[2]; } constants{
		{_width, _height}, {_halfWidth, _halfHeight}
	};
	D3D11_BUFFER_DESC cbd{ .ByteWidth = sizeof(Constants), .Usage = D3D11_USAGE_IMMUTABLE,
		.BindFlags = D3D11_BIND_CONSTANT_BUFFER };
	D3D11_SUBRESOURCE_DATA initial{ .pSysMem = &constants };
	hr = _device->CreateBuffer(&cbd, &initial, _constantBuffer.put());
	if (FAILED(hr)) return false;

	static constexpr float ZERO[4]{};
	_context->ClearUnorderedAccessViewFloat(_fullFlowUav.get(), ZERO);
	_context->ClearUnorderedAccessViewFloat(_fullConfidenceUav.get(), ZERO);
	_context->CopyResource(_previous.get(), input);
	_hasHistory = false;
	Logger::Get().Info(fmt::format("Half-resolution optical flow initialized: {}x{} -> {}x{}",
		_width, _height, _halfWidth, _halfHeight));
	return true;
}

bool HalfResOpticalFlow::_CreateInputSrv(ID3D11Texture2D* input) noexcept {
	if (_input == input && _inputSrv) return true;
	_input = input;
	_inputSrv = nullptr;
	return SUCCEEDED(_device->CreateShaderResourceView(input, nullptr, _inputSrv.put()));
}

bool HalfResOpticalFlow::_MeasureActivity() noexcept {
	ID3D11Buffer* cb = _constantBuffer.get();
	ID3D11ShaderResourceView* srvs[2]{ _inputSrv.get(), _previousSrv.get() };
	ID3D11UnorderedAccessView* activityUav = _activityUav.get();
	_context->CSSetConstantBuffers(0, 1, &cb);
	_context->CSSetShader(_activityShader.get(), nullptr, 0);
	_context->CSSetShaderResources(0, 2, srvs);
	_context->CSSetUnorderedAccessViews(1, 1, &activityUav, nullptr);
	_context->Dispatch(1, 1, 1);
	ID3D11ShaderResourceView* nullSrvs[2]{};
	ID3D11UnorderedAccessView* nullUav = nullptr;
	_context->CSSetShaderResources(0, 2, nullSrvs);
	_context->CSSetUnorderedAccessViews(1, 1, &nullUav, nullptr);
	_context->CSSetShader(nullptr, nullptr, 0);
	_context->CopyResource(_activityReadback.get(), _activity.get());
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(_context->Map(
		_activityReadback.get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
	_lastMeanLumaDifference = *static_cast<const float*>(mapped.pData);
	_context->Unmap(_activityReadback.get(), 0);
	_isStatic = _lastMeanLumaDifference <= 1.0f / 255.0f;
	return true;
}

bool HalfResOpticalFlow::Estimate(ID3D11Texture2D* input) noexcept {
	if (!_CreateInputSrv(input)) return false;
	if (!_hasHistory) {
		static constexpr float ZERO[4]{};
		_context->ClearUnorderedAccessViewFloat(_fullFlowUav.get(), ZERO);
		_context->ClearUnorderedAccessViewFloat(_fullConfidenceUav.get(), ZERO);
		_context->CopyResource(_previous.get(), input);
		_hasHistory = true;
		return true;
	}
	if (!_MeasureActivity()) return false;

	ID3D11Buffer* cb = _constantBuffer.get();
	_context->CSSetConstantBuffers(0, 1, &cb);
	ID3D11ShaderResourceView* estimateSrvs[2]{ _inputSrv.get(), _previousSrv.get() };
	ID3D11UnorderedAccessView* halfUavs[]{
		_halfFlowUav.get(), _halfConfidenceUav.get()
	};
	_context->CSSetShader(_estimateShader.get(), nullptr, 0);
	_context->CSSetShaderResources(0, 2, estimateSrvs);
	_context->CSSetUnorderedAccessViews(0, ARRAYSIZE(halfUavs), halfUavs, nullptr);
	_context->Dispatch((_halfWidth + 7) / 8, (_halfHeight + 7) / 8, 1);

	ID3D11ShaderResourceView* nullSrvs[2]{};
	ID3D11UnorderedAccessView* nullUavs[2]{};
	_context->CSSetShaderResources(0, 2, nullSrvs);
	_context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
	ID3D11ShaderResourceView* halfSrvs[]{
		_halfFlowSrv.get(), _halfConfidenceSrv.get()
	};
	ID3D11UnorderedAccessView* fullUavs[]{
		_fullFlowUav.get(), _fullConfidenceUav.get()
	};
	_context->CSSetShader(_upsampleShader.get(), nullptr, 0);
	_context->CSSetShaderResources(0, ARRAYSIZE(halfSrvs), halfSrvs);
	_context->CSSetUnorderedAccessViews(0, ARRAYSIZE(fullUavs), fullUavs, nullptr);
	_context->Dispatch((_width + 7) / 8, (_height + 7) / 8, 1);
	_context->CSSetShaderResources(0, 2, nullSrvs);
	_context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
	_context->CSSetShader(nullptr, nullptr, 0);
	_context->CopyResource(_previous.get(), input);
	return true;
}

static FrameGuidanceMetadata MakeSoftwareFlowMetadata(
	const FrameGuidanceFrame& frame,
	FrameGuidanceResetReason resetReason,
	bool isStatic
) noexcept {
	return {
		.frameId = frame.frameId,
		.sourceExtent = frame.sourceExtent,
		.validRegion = frame.validRegion,
		.resetReason = resetReason,
		.valid = true,
		.isZero = resetReason != FrameGuidanceResetReason::None || isStatic,
		.requiresHistoryReset = resetReason != FrameGuidanceResetReason::None
	};
}

bool SoftwareOpticalFlowProvider::Initialize(
	DeviceResources& resources,
	FrameGuidanceExtent sourceExtent
) noexcept {
	_resources = &resources;
	_extent = sourceExtent;
	_flow.reset();
	_resetReason = FrameGuidanceResetReason::Initialize;
	return sourceExtent.IsValid();
}

bool SoftwareOpticalFlowProvider::BeginFrame(
	const FrameGuidanceFrame& frame,
	MotionVectorProviderOutput& output
) noexcept {
	if (!_resources || !frame.color || frame.sourceExtent != _extent) return false;
	if (!_flow) {
		_flow = std::make_unique<HalfResOpticalFlow>();
		if (!_flow->Initialize(
			_resources->GetD3DDevice(), _resources->GetD3DDC(), frame.color)) {
			_flow.reset();
			_resetReason = FrameGuidanceResetReason::ProviderFailure;
			return false;
		}
		Logger::Get().Info(
			"Frame Guidance software optical-flow fallback initialized");
	}
	if (!_flow->Estimate(frame.color)) {
		_flow->ResetHistory();
		_resetReason = FrameGuidanceResetReason::ProviderFailure;
		return false;
	}
	const FrameGuidanceMetadata metadata = MakeSoftwareFlowMetadata(
		frame, _resetReason, _flow->IsStatic());
	output.motion = {
		.texture = _flow->GetMotionTexture(),
		.format = DXGI_FORMAT_R16G16_FLOAT,
		.metadata = metadata
	};
	output.confidence = {
		.texture = _flow->GetConfidenceTexture(),
		.format = DXGI_FORMAT_R8_UNORM,
		.metadata = metadata
	};
	_resetReason = FrameGuidanceResetReason::None;
	return true;
}

void SoftwareOpticalFlowProvider::Reset(
	FrameGuidanceResetReason reason
) noexcept {
	if (_flow) _flow->ResetHistory();
	_resetReason = reason;
}

bool SoftwareOpticalFlowProvider::Resize(
	FrameGuidanceExtent sourceExtent
) noexcept {
	if (!sourceExtent.IsValid()) return false;
	_extent = sourceExtent;
	_flow.reset();
	_resetReason = FrameGuidanceResetReason::Resize;
	return true;
}

}
