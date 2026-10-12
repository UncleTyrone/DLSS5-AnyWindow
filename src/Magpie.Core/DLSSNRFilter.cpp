#include "pch.h"
#include "DLSSNRFilter.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include "FrameGuidanceD3D12Interop.h"
#include "FrameGuidancePerformance.h"
#include "DLSSNRNvapi.h"

#include <atomic>

namespace Magpie {

namespace {

std::atomic<int32_t> g_dlssnrRuntimeState{
	static_cast<int32_t>(DLSSNRRuntimeState::Pending)
};
std::atomic<uint32_t> g_dlssnrEvaluateSuccessCount{ 0 };
std::atomic<uint32_t> g_dlssnrEvaluateFailureCount{ 0 };
std::atomic<int32_t> g_dlssnrRuntimeKind{
	static_cast<int32_t>(DLSSNRRuntimeKind::Unknown)
};
std::atomic<int32_t> g_dlssnrCudaComputeMajor{ 0 };
std::atomic<int32_t> g_dlssnrCudaComputeMinor{ 0 };

}

void DLSSNRFilter::ResetRuntimeTelemetry() noexcept {
	g_dlssnrRuntimeState.store(
		static_cast<int32_t>(DLSSNRRuntimeState::Pending), std::memory_order_relaxed);
	g_dlssnrEvaluateSuccessCount.store(0, std::memory_order_relaxed);
	g_dlssnrEvaluateFailureCount.store(0, std::memory_order_relaxed);
	g_dlssnrRuntimeKind.store(
		static_cast<int32_t>(DLSSNRRuntimeKind::Unknown), std::memory_order_relaxed);
	g_dlssnrCudaComputeMajor.store(0, std::memory_order_relaxed);
	g_dlssnrCudaComputeMinor.store(0, std::memory_order_relaxed);
}

DLSSNRTelemetry DLSSNRFilter::RuntimeTelemetry() noexcept {
	return {
		.state = static_cast<DLSSNRRuntimeState>(
			g_dlssnrRuntimeState.load(std::memory_order_relaxed)),
		.evaluateSuccessCount = g_dlssnrEvaluateSuccessCount.load(std::memory_order_relaxed),
		.evaluateFailureCount = g_dlssnrEvaluateFailureCount.load(std::memory_order_relaxed),
		.runtimeKind = static_cast<DLSSNRRuntimeKind>(
			g_dlssnrRuntimeKind.load(std::memory_order_relaxed)),
		.cudaComputeMajor = g_dlssnrCudaComputeMajor.load(std::memory_order_relaxed),
		.cudaComputeMinor = g_dlssnrCudaComputeMinor.load(std::memory_order_relaxed)
	};
}

}

#ifdef MP_ENABLE_DLSSNR
#include <d3d12.h>
#include <nvsdk_ngx.h>

namespace Magpie {

namespace {

void LogDlssnrStatus(std::string message, bool error = false) noexcept {
	if (error) {
		Logger::Get().Error(message);
	} else {
		Logger::Get().Info(message);
	}
	message.push_back('\n');
	OutputDebugStringA(message.c_str());
}

constexpr NVSDK_NGX_Feature FEATURE_DLSSNR =
	static_cast<NVSDK_NGX_Feature>(18);
constexpr unsigned long long DLSSNR_SIGNED_SNIPPET_APPLICATION_ID = 0x0876232Cull;
constexpr int DEFAULT_NR_PRESET = 1;
// Keep the unsupported Core Feature 18 route as an explicit diagnostic only.
// It must never run before the signed snippet in a production session.
constexpr bool ENABLE_CORE_FEATURE18_DIAGNOSTIC = false;

constexpr char PARAM_WIDTH[] = "DLSSNR.Width";
constexpr char PARAM_HEIGHT[] = "DLSSNR.Height";
constexpr char PARAM_INPUT_WIDTH[] = "DLSSNR.InputWidth";
constexpr char PARAM_INPUT_HEIGHT[] = "DLSSNR.InputHeight";
constexpr char PARAM_OUTPUT_WIDTH[] = "DLSSNR.OutputWidth";
constexpr char PARAM_OUTPUT_HEIGHT[] = "DLSSNR.OutputHeight";
constexpr char PARAM_OUTPUT_DOT_WIDTH[] = "DLSSNR.Output.Width";
constexpr char PARAM_OUTPUT_DOT_HEIGHT[] = "DLSSNR.Output.Height";
constexpr char PARAM_UPSCALING[] = "DLSSNR.Upscaling";
constexpr char PARAM_SCALE[] = "DLSSNR.Scale";
constexpr char PARAM_SCALING_RATIO[] = "DLSSNR.ScalingRatio";
constexpr char PARAM_SCALING_RATIO_CALLBACK[] = "DLSSNRComputeScalingRatioCallback";
constexpr char PARAM_PRESET[] = "DLSSNR.Hint.Render.Preset";
constexpr char PARAM_COLOR[] = "DLSSNR.Color";
constexpr char PARAM_OUTPUT[] = "DLSSNR.Output";
constexpr char PARAM_MVEC[] = "DLSSNR.MVec";
constexpr char PARAM_DEPTH[] = "DLSSNR.Depth";
constexpr char PARAM_MVEC_SCALE_X[] = "DLSSNR.MVecScaleX";
constexpr char PARAM_MVEC_SCALE_Y[] = "DLSSNR.MVecScaleY";
constexpr char PARAM_DEPTH_INVERTED[] = "DLSSNR.DepthInverted";
constexpr char PARAM_ENABLED[] = "DLSSNR.Enabled";
constexpr char PARAM_RESET[] = "DLSSNR.Reset";
constexpr char PARAM_STYLE[] = "DLSSNR.Style";
constexpr char PARAM_INTENSITY[] = "DLSSNR.Intensity";
constexpr char PARAM_LOCAL_TONE[] = "DLSSNR.LocalToneStrength";
constexpr char PARAM_LOCAL_STRUCTURE[] = "DLSSNR.LocalStructureStrength";
constexpr char PARAM_AUTO_MASK[] = "DLSSNR.UseAutoMask";
constexpr char PARAM_UI_CORRECTION[] = "DLSSNR.UICorrection";
constexpr char PARAM_INDICATOR_INVERT_X[] = "DLSS.Indicator.Invert.X.Axis";
constexpr char PARAM_INDICATOR_INVERT_Y[] = "DLSS.Indicator.Invert.Y.Axis";

struct ResourceParameters {
	const char* baseX;
	const char* baseY;
	const char* width;
	const char* height;
};

constexpr ResourceParameters RESOURCE_PARAMETERS[]{
	{ "DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
		"DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight" },
	{ "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY",
		"DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight" },
	{ "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY",
		"DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight" },
	{ "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY",
		"DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight" }
};

constexpr char COLOR_CONVERT_HLSL[] = R"(
Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);

[numthreads(8, 8, 1)]
void ConvertToRgba(uint3 tid : SV_DispatchThreadID) {
    uint width, height;
    OutputColor.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) return;
    OutputColor[tid.xy] = InputColor.Load(int3(tid.xy, 0));
}
)";

constexpr char PASS_FEEDBACK_HLSL[] = R"(
Texture2D<float4> ProxyColor : register(t0);
Texture2D<float4> AnswerColor : register(t1);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer Feedback : register(b0) {
    float FeedbackAmount;
    float3 FeedbackPad;
};

[numthreads(8, 8, 1)]
void BlendPassFeedback(uint3 tid : SV_DispatchThreadID) {
    uint width, height;
    OutputColor.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) return;
    float3 proxy = ProxyColor.Load(int3(tid.xy, 0)).rgb;
    float3 answer = AnswerColor.Load(int3(tid.xy, 0)).rgb;
    OutputColor[tid.xy] = float4(lerp(proxy, answer, FeedbackAmount), 1);
}
)";

// Janblade detail reuse without a depth guide (dlssnr_detail_reuse.hlsl, HasDepth == false).
// Detail is answer - input. The next frame adds the moved detail times a soft trust from the
// colour box and the motion test. Fill then borrows trusted neighbours so a rejected pixel does
// not flash back to the raw picture. An exact match rejects capture noise and is what flickers.
constexpr char DETAIL_REPROJECT_HLSL[] = R"(
cbuffer Params : register(b0) {
    uint mode;
    uint workWidth;
    uint workHeight;
    float clipGamma;
    float clipFalloff;
    float sigmaFloor;
    float motionReject;
    float fillStrength;
    float fillRadius;
    float pad0;
    float pad1;
    float pad2;
};

Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t2 : register(t2);
Texture2D<float2> t3 : register(t3);
SamplerState gLinear : register(s0);
RWTexture2D<float4> u0 : register(u0);
RWTexture2D<float4> u1 : register(u1);

float2 WorkSize() { return float2(workWidth, workHeight); }

float3 ToYCoCg(float3 c) {
    return float3(
        dot(c, float3(0.25, 0.5, 0.25)),
        dot(c, float3(0.5, 0.0, -0.5)),
        dot(c, float3(-0.25, 0.5, -0.25)));
}

void ColourBox(uint2 p, out float3 mean, out float3 sigma) {
    const int2 last = int2(WorkSize()) - 1;
    float3 sum = 0.0, sumSquares = 0.0;
    [unroll] for (int cy = -1; cy <= 1; ++cy) {
        [unroll] for (int cx = -1; cx <= 1; ++cx) {
            const float3 c = ToYCoCg(t0.Load(int3(clamp(int2(p) + int2(cx, cy), int2(0, 0), last), 0)).rgb);
            sum += c;
            sumSquares += c * c;
        }
    }
    mean = sum / 9.0;
    sigma = max(sqrt(max(sumSquares / 9.0 - mean * mean, 0.0)), max(sigmaFloor, 1e-5));
}

float2 MotionAt(int2 p) {
    const int2 last = int2(WorkSize()) - 1;
    return t3.Load(int3(clamp(p, int2(0, 0), last), 0));
}

float MotionTrust(float2 previousUv, float2 herePixels) {
    if (motionReject <= 0.0) return 1.0;
    const int2 therePx = int2(previousUv * WorkSize());
    const float2 therePixels = MotionAt(therePx);
    const float difference = length(herePixels - therePixels);
    if (!isfinite(difference)) return 1.0;
    return 1.0 - smoothstep(0.5, 1.0, difference / (motionReject * (length(herePixels) + 1.0)));
}

float4 SampleCatmullRom(Texture2D<float4> tex, float2 uv, float2 size) {
    const float2 position = uv * size;
    const float2 centre = floor(position - 0.5) + 0.5;
    const float2 f = position - centre;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 texel = 1.0 / size;
    const float2 uv0 = (centre - 1.0) * texel;
    const float2 uv3 = (centre + 2.0) * texel;
    const float2 uv12 = (centre + w2 / max(w12, 1e-4)) * texel;
    float4 result = tex.SampleLevel(gLinear, float2(uv12.x, uv0.y), 0) * (w12.x * w0.y);
    result += tex.SampleLevel(gLinear, float2(uv0.x, uv12.y), 0) * (w0.x * w12.y);
    result += tex.SampleLevel(gLinear, uv12, 0) * (w12.x * w12.y);
    result += tex.SampleLevel(gLinear, float2(uv3.x, uv12.y), 0) * (w3.x * w12.y);
    result += tex.SampleLevel(gLinear, float2(uv12.x, uv3.y), 0) * (w12.x * w3.y);
    const float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return result / max(weight, 1e-4);
}

float4 MovedDetail(uint2 p) {
    const float2 work = WorkSize();
    const float2 uv = (float2(p) + 0.5) / work;
    const float2 raw = MotionAt(int2(p));
    const float2 previousUv = uv + raw / work;
    if (!all(isfinite(previousUv)) || any(previousUv < 0.0) || any(previousUv > 1.0))
        return 0.0;

    float3 mean, sigma;
    ColourBox(p, mean, sigma);

    const float2 position = previousUv * work - 0.5;
    const int2 base = int2(floor(position));
    const float2 f = position - float2(base);
    float3 detailLo = 65504.0, detailHi = -65504.0;
    float valid = 1.0;
    [unroll] for (int i = 0; i < 4; ++i) {
        const int2 corner = int2(i & 1, i >> 1);
        const float2 axis = float2(corner.x != 0 ? f.x : 1.0 - f.x, corner.y != 0 ? f.y : 1.0 - f.y);
        if (axis.x * axis.y < 1e-3) continue;
        const int2 texel = clamp(base + corner, int2(0, 0), int2(work) - 1);
        const float4 d = t1.Load(int3(texel, 0));
        const bool ok = all(isfinite(d.rgb)) && isfinite(d.a);
        valid = ok ? min(valid, d.a) : 0.0;
        detailLo = ok ? min(detailLo, d.rgb) : detailLo;
        detailHi = ok ? max(detailHi, d.rgb) : detailHi;
    }
    if (valid < 0.999) return 0.0;

    const float3 moved = clamp(SampleCatmullRom(t1, previousUv, work).rgb, detailLo, detailHi);
    const float3 saved = ToYCoCg(t2.SampleLevel(gLinear, previousUv, 0).rgb);
    const float3 excess = max(abs(saved - mean) - clipGamma * sigma, 0.0) / sigma;
    const float colourTrust = 1.0 - saturate(max(excess.x, max(excess.y, excess.z)) / max(clipFalloff, 1e-3));
    if (!all(isfinite(moved)) || !all(isfinite(saved))) return 0.0;
    const float trust = saturate(colourTrust * MotionTrust(previousUv, raw));
    return float4(moved, trust);
}

void Capture(uint2 p) {
    const float3 input = t0.Load(int3(p, 0)).rgb;
    const float3 answer = t1.Load(int3(p, 0)).rgb;
    const float3 detail = answer - input;
    const bool valid = all(isfinite(detail)) && all(isfinite(input)) && all(abs(detail) <= 65504.0);
    u0[p] = float4(valid ? detail : 0.0, valid ? 1.0 : 0.0);
    u1[p] = float4(all(isfinite(input)) ? input : 0.0, 0.0);
}

void Estimate(uint2 p) {
    u0[p] = MovedDetail(p);
}

void Fill(uint2 p) {
    const float3 input = t0.Load(int3(p, 0)).rgb;
    const float4 estimate = t1.Load(int3(p, 0));
    const bool estimateOk = all(isfinite(estimate.rgb)) && isfinite(estimate.a);
    const float trust = estimateOk ? saturate(estimate.a) : 0.0;
    const float3 own = estimateOk ? estimate.rgb * trust : 0.0;
    float filled = 0.0;
    float3 fillDetail = 0.0;
    if (trust < 0.999 && fillStrength > 0.0) {
        const float2 work = WorkSize();
        float3 sum = 0.0;
        float weightSum = 0.0;
        [unroll] for (int k = 0; k < 16; ++k) {
            const float radius = lerp(2.0, max(fillRadius, 2.0), sqrt((k + 0.5) / 16.0));
            const float angle = 2.39996323 * k;
            const int2 texel = clamp(int2(p) + int2(round(radius * float2(cos(angle), sin(angle)))),
                int2(0, 0), int2(work) - 1);
            const float4 tap = t1.Load(int3(texel, 0));
            if (!all(isfinite(tap.rgb)) || !isfinite(tap.a) || tap.a <= 0.0) continue;
            const float weight = saturate(tap.a);
            sum += tap.rgb * weight;
            weightSum += weight;
        }
        if (weightSum > 1e-3) {
            fillDetail = sum / weightSum;
            filled = (1.0 - trust) * saturate(fillStrength) * saturate(weightSum / 2.0);
        }
    }
    float3 result = input + own + fillDetail * filled;
    if (!all(isfinite(result))) result = input;
    u0[p] = float4(result, 1.0);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint2 p = id.xy;
    if (p.x >= workWidth || p.y >= workHeight) return;
    if (mode == 0) Capture(p);
    else if (mode == 1) Estimate(p);
    else Fill(p);
}
)";

constexpr char COLOR_DOWNSAMPLE_HLSL[] = R"(
Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    float2 MotionScale;
    float2 Padding;
};

float Sinc(float x) {
    if (abs(x) < 1e-5) return 1.0;
    x *= 3.14159265358979323846;
    return sin(x) / x;
}

float Lanczos2(float x) {
    return abs(x) < 2.0 ? Sinc(x) * Sinc(x * 0.5) : 0.0;
}

[numthreads(8, 8, 1)]
void DownsampleColorVertical(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= SourceExtent.x || tid.y >= TargetExtent.y) return;
    float scale = float(TargetExtent.y) / float(SourceExtent.y);
    float position = (float(tid.y) + 0.5) / scale - 0.5;
    float support = 2.0 / scale;
    int first = int(ceil(position - support));
    int last = int(floor(position + support));
    float4 total = 0.0;
    float totalWeight = 0.0;
    [loop]
    for (int y = first; y <= last; ++y) {
        float weight = Lanczos2((float(y) - position) * scale);
        total += InputColor.Load(int3(tid.x,
            clamp(y, 0, int(SourceExtent.y) - 1), 0)) * weight;
        totalWeight += weight;
    }
    OutputColor[tid.xy] = total / (abs(totalWeight) > 1e-6 ? totalWeight : 1.0);
}

[numthreads(8, 8, 1)]
void DownsampleColorHorizontal(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    float scale = float(TargetExtent.x) / float(SourceExtent.x);
    float position = (float(tid.x) + 0.5) / scale - 0.5;
    float support = 2.0 / scale;
    int first = int(ceil(position - support));
    int last = int(floor(position + support));
    float4 total = 0.0;
    float totalWeight = 0.0;
    [loop]
    for (int x = first; x <= last; ++x) {
        float weight = Lanczos2((float(x) - position) * scale);
        total += InputColor.Load(int3(
            clamp(x, 0, int(SourceExtent.x) - 1), tid.y, 0)) * weight;
        totalWeight += weight;
    }
    OutputColor[tid.xy] = total / (abs(totalWeight) > 1e-6 ? totalWeight : 1.0);
}
)";

constexpr char GUIDANCE_DOWNSAMPLE_HLSL[] = R"(
Texture2D<float2> InputMotion : register(t0);
Texture2D<float> InputDepth : register(t1);
Texture2D<float> InputConfidence : register(t2);
RWTexture2D<float2> OutputMotion : register(u0);
RWTexture2D<float> OutputDepth : register(u1);
RWTexture2D<float> OutputConfidence : register(u2);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    float2 MotionScale;
    float2 Padding;
};

[numthreads(8, 8, 1)]
void DownsampleGuidance(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    float2 sourceStart = float2(tid.xy) * float2(SourceExtent) / float2(TargetExtent);
    float2 sourceEnd = float2(tid.xy + 1) * float2(SourceExtent) / float2(TargetExtent);
    int2 first = int2(floor(sourceStart));
    int2 last = int2(ceil(sourceEnd));
    float2 motionTotal = 0.0;
    float2 weightedMotionTotal = 0.0;
    float confidenceTotal = 0.0;
    float totalWeight = 0.0;
    float closestDepth = 0.0;
    [loop]
    for (int y = first.y; y < last.y; ++y) {
        float weightY = max(0.0, min(sourceEnd.y, float(y + 1)) - max(sourceStart.y, float(y)));
        [loop]
        for (int x = first.x; x < last.x; ++x) {
            float weightX = max(0.0, min(sourceEnd.x, float(x + 1)) - max(sourceStart.x, float(x)));
            float weight = weightX * weightY;
            int2 sourcePixel = clamp(int2(x, y), int2(0, 0), int2(SourceExtent) - 1);
            float2 motion = InputMotion.Load(int3(sourcePixel, 0));
            float confidence = InputConfidence.Load(int3(sourcePixel, 0));
            motionTotal += motion * weight;
            weightedMotionTotal += motion * confidence * weight;
            confidenceTotal += confidence * weight;
            totalWeight += weight;
            closestDepth = max(closestDepth, InputDepth.Load(int3(sourcePixel, 0)));
        }
    }
    float2 motion = confidenceTotal > 1e-6 ?
        weightedMotionTotal / confidenceTotal : motionTotal / max(totalWeight, 1e-6);
    OutputMotion[tid.xy] = motion * MotionScale;
    OutputDepth[tid.xy] = closestDepth;
    OutputConfidence[tid.xy] = confidenceTotal / max(totalWeight, 1e-6);
}
)";

constexpr char RESIDUAL_PREPARE_HLSL[] = R"(
Texture2D<float4> ReducedColor : register(t0);
Texture2D<float4> ReducedDenoised : register(t1);
RWTexture2D<float4> Residual : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    float2 MotionScale;
    float2 Padding;
};

[numthreads(8, 8, 1)]
void PrepareResidual(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    float3 original = ReducedColor.Load(int3(tid.xy, 0)).rgb;
    float3 denoised = ReducedDenoised.Load(int3(tid.xy, 0)).rgb;
    Residual[tid.xy] = float4(denoised - original, 0.0);
}
)";

constexpr char RESIDUAL_HORIZONTAL_HLSL[] = R"(
Texture2D<float4> Residual : register(t0);
RWTexture2D<float4> HorizontalResidual : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    float2 MotionScale;
    float2 Padding;
};

float CatmullRom(float x) {
    x = abs(x);
    if (x < 1.0) return ((1.5 * x - 2.5) * x) * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

[numthreads(8, 8, 1)]
void UpsampleResidualHorizontal(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= SourceExtent.x || tid.y >= TargetExtent.y) return;
    float reducedPosition = (float(tid.x) + 0.5) *
        float(TargetExtent.x) / float(SourceExtent.x) - 0.5;
    int center = int(floor(reducedPosition));
    float3 result = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (int x = -1; x <= 2; ++x) {
        float weight = CatmullRom(reducedPosition - float(center + x));
        int sampleX = clamp(center + x, 0, int(TargetExtent.x) - 1);
        result += Residual.Load(int3(sampleX, tid.y, 0)).rgb * weight;
        totalWeight += weight;
    }
    HorizontalResidual[tid.xy] = float4(
        result / (abs(totalWeight) > 1e-6 ? totalWeight : 1.0), 0.0);
}
)";

constexpr char RESIDUAL_VERTICAL_COMPOSITE_HLSL[] = R"(
Texture2D<float4> OriginalColor : register(t0);
Texture2D<float4> HorizontalResidual : register(t1);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    float2 MotionScale;
    float2 Padding;
};

float CatmullRom(float x) {
    x = abs(x);
    if (x < 1.0) return ((1.5 * x - 2.5) * x) * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

[numthreads(8, 8, 1)]
void CompositeResidualVertical(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SourceExtent)) return;
    float4 original = OriginalColor.Load(int3(tid.xy, 0));
    float reducedPosition = (float(tid.y) + 0.5) *
        float(TargetExtent.y) / float(SourceExtent.y) - 0.5;
    int center = int(floor(reducedPosition));
    float3 residual = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (int y = -1; y <= 2; ++y) {
        float weight = CatmullRom(reducedPosition - float(center + y));
        int sampleY = clamp(center + y, 0, int(TargetExtent.y) - 1);
        residual += HorizontalResidual.Load(int3(tid.x, sampleY, 0)).rgb * weight;
        totalWeight += weight;
    }
    residual /= abs(totalWeight) > 1e-6 ? totalWeight : 1.0;
    OutputColor[tid.xy] = float4(saturate(original.rgb + residual), original.a);
}
)";

struct ResampleConstants {
	uint32_t sourceWidth = 0;
	uint32_t sourceHeight = 0;
	uint32_t targetWidth = 0;
	uint32_t targetHeight = 0;
	float motionScaleX = 1.0f;
	float motionScaleY = 1.0f;
	float padding0 = 0.0f;
	float padding1 = 0.0f;
};
static_assert(sizeof(ResampleConstants) == 32);

bool NGXSucceeded(NVSDK_NGX_Result result) noexcept {
	return NVSDK_NGX_SUCCEED(result);
}

struct TimingSummary {
	double average = 0.0;
	double p95 = 0.0;
	double p99 = 0.0;
	double maximum = 0.0;
	size_t count = 0;
};

struct TimingWindow {
	static constexpr size_t CAPACITY = 120;
	std::array<double, CAPACITY> values{};
	size_t count = 0;
	size_t next = 0;

	void Add(double value) noexcept {
		values[next] = value;
		next = (next + 1) % CAPACITY;
		count = std::min(count + 1, CAPACITY);
	}

	TimingSummary Summarize() const noexcept {
		TimingSummary result{ .count = count };
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

template <typename T>
T GetExport(HMODULE module, const char* name) noexcept {
	return reinterpret_cast<T>(GetProcAddress(module, name));
}

struct CudaComputeCapability {
	int major = 0;
	int minor = 0;
	bool valid = false;
};

CudaComputeCapability QueryCudaComputeCapability(LUID adapterLuid) noexcept {
	using CuInitFn = int(__stdcall*)(unsigned int);
	using CuDeviceGetCountFn = int(__stdcall*)(int*);
	using CuDeviceGetFn = int(__stdcall*)(int*, int);
	using CuDeviceGetLuidFn = int(__stdcall*)(char*, unsigned int*, int);
	using CuDeviceGetAttributeFn = int(__stdcall*)(int*, int, int);

	CudaComputeCapability result;
	HMODULE module = LoadLibraryExW(
		L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!module) return result;
	auto unload = wil::scope_exit([&]() { FreeLibrary(module); });
	auto get = [module](const char* name) {
		return GetProcAddress(module, name);
	};
	const auto cuInit = reinterpret_cast<CuInitFn>(get("cuInit"));
	const auto cuDeviceGetCount = reinterpret_cast<CuDeviceGetCountFn>(
		get("cuDeviceGetCount"));
	const auto cuDeviceGet = reinterpret_cast<CuDeviceGetFn>(get("cuDeviceGet"));
	const auto cuDeviceGetLuid = reinterpret_cast<CuDeviceGetLuidFn>(
		get("cuDeviceGetLuid"));
	const auto cuDeviceGetAttribute = reinterpret_cast<CuDeviceGetAttributeFn>(
		get("cuDeviceGetAttribute"));
	if (!cuInit || !cuDeviceGetCount || !cuDeviceGet || !cuDeviceGetLuid ||
		!cuDeviceGetAttribute || cuInit(0) != 0) {
		return result;
	}

	int count = 0;
	if (cuDeviceGetCount(&count) != 0) return result;
	for (int ordinal = 0; ordinal < count; ++ordinal) {
		int device = 0;
		char luid[sizeof(LUID)]{};
		unsigned int nodeMask = 0;
		if (cuDeviceGet(&device, ordinal) != 0 ||
			cuDeviceGetLuid(luid, &nodeMask, device) != 0 ||
			std::memcmp(luid, &adapterLuid, sizeof(LUID)) != 0) {
			continue;
		}
		// CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR/MINOR.
		if (cuDeviceGetAttribute(&result.major, 75, device) == 0 &&
			cuDeviceGetAttribute(&result.minor, 76, device) == 0) {
			result.valid = true;
		}
		break;
	}
	return result;
}

struct DLSSNRRuntimeSelection {
	std::filesystem::path path;
	DLSSNRRuntimeKind kind = DLSSNRRuntimeKind::Unknown;
	CudaComputeCapability cuda;
	std::wstring adapterName;
	uint32_t deviceId = 0;
	bool forced = false;
};

const char* RuntimeKindName(DLSSNRRuntimeKind kind) noexcept {
	switch (kind) {
	case DLSSNRRuntimeKind::ShortFuseFp16: return "310.8.SF-v2 FP16 (RTX 20)";
	case DLSSNRRuntimeKind::Rtx30Patched: return "310.8.0 RTX30 patch";
	case DLSSNRRuntimeKind::Rtx40Patched: return "310.8.0 RTX40 patch";
	case DLSSNRRuntimeKind::Rtx50Original: return "310.8.0 original (RTX 50)";
	case DLSSNRRuntimeKind::Custom: return "custom override";
	case DLSSNRRuntimeKind::LegacyRoot: return "legacy root fallback";
	default: return "unknown";
	}
}

std::filesystem::path RuntimePathForKind(
	const std::filesystem::path& applicationDirectory,
	DLSSNRRuntimeKind kind
) {
	std::filesystem::path directory = applicationDirectory / L"DLSSNR-Runtimes";
	switch (kind) {
	case DLSSNRRuntimeKind::ShortFuseFp16:
		directory /= L"RTX20";
		if (std::filesystem::is_regular_file(directory / L"nvngx_dlssnr.dll")) {
			return directory / L"nvngx_dlssnr.dll";
		}
		return applicationDirectory / L"DLSSNR-Runtimes" /
			L"RTX20-30-SF-v2" / L"nvngx_dlssnr.dll";
	case DLSSNRRuntimeKind::Rtx30Patched:
		directory /= L"RTX30";
		if (std::filesystem::is_regular_file(directory / L"nvngx_dlssnr.dll")) {
			return directory / L"nvngx_dlssnr.dll";
		}
		return applicationDirectory / L"DLSSNR-Runtimes" /
			L"RTX20-30-SF-v2" / L"nvngx_dlssnr.dll";
	case DLSSNRRuntimeKind::Rtx40Patched:
		directory /= L"RTX40";
		break;
	case DLSSNRRuntimeKind::Rtx50Original:
		directory /= L"RTX50";
		break;
	default:
		return applicationDirectory / L"nvngx_dlssnr.dll";
	}
	return directory / L"nvngx_dlssnr.dll";
}

DLSSNRRuntimeKind ParseRuntimeOverride(const std::wstring& value) noexcept {
	if (_wcsicmp(value.c_str(), L"sf-v2") == 0 ||
		_wcsicmp(value.c_str(), L"rtx20-30") == 0 ||
		_wcsicmp(value.c_str(), L"rtx20") == 0) {
		return DLSSNRRuntimeKind::ShortFuseFp16;
	}
	if (_wcsicmp(value.c_str(), L"rtx30") == 0) {
		return DLSSNRRuntimeKind::Rtx30Patched;
	}
	if (_wcsicmp(value.c_str(), L"rtx40") == 0) {
		return DLSSNRRuntimeKind::Rtx40Patched;
	}
	if (_wcsicmp(value.c_str(), L"rtx50") == 0 ||
		_wcsicmp(value.c_str(), L"original") == 0) {
		return DLSSNRRuntimeKind::Rtx50Original;
	}
	return DLSSNRRuntimeKind::Unknown;
}

DLSSNRRuntimeKind DetectRuntimeKind(
	std::wstring_view adapterName,
	const CudaComputeCapability& cuda
) noexcept {
	if (adapterName.find(L"RTX 20") != std::wstring_view::npos) {
		return DLSSNRRuntimeKind::ShortFuseFp16;
	}
	if (adapterName.find(L"RTX 30") != std::wstring_view::npos) {
		return DLSSNRRuntimeKind::Rtx30Patched;
	}
	if (adapterName.find(L"RTX 40") != std::wstring_view::npos) {
		return DLSSNRRuntimeKind::Rtx40Patched;
	}
	if (adapterName.find(L"RTX 50") != std::wstring_view::npos) {
		return DLSSNRRuntimeKind::Rtx50Original;
	}
	if (!cuda.valid) return DLSSNRRuntimeKind::Unknown;
	if (cuda.major == 7) {
		return DLSSNRRuntimeKind::ShortFuseFp16;
	}
	if (cuda.major == 8 && cuda.minor <= 6) {
		return DLSSNRRuntimeKind::Rtx30Patched;
	}
	if (cuda.major == 8 && cuda.minor == 9) {
		return DLSSNRRuntimeKind::Rtx40Patched;
	}
	if (cuda.major >= 12) {
		return DLSSNRRuntimeKind::Rtx50Original;
	}
	return DLSSNRRuntimeKind::Unknown;
}

DLSSNRRuntimeSelection SelectDLSSNRRuntime(
	const std::filesystem::path& applicationDirectory,
	IDXGIAdapter4* adapter
) noexcept {
	DLSSNRRuntimeSelection result;
	DXGI_ADAPTER_DESC3 description{};
	if (adapter && SUCCEEDED(adapter->GetDesc3(&description))) {
		result.adapterName = description.Description;
		result.deviceId = description.DeviceId;
		if (description.VendorId == 0x10de) {
			// GeForce names identify the 20/30/40/50 generation without touching
			// the CUDA driver during renderer initialization. Query CUDA only for
			// professional/OEM names that do not carry a GeForce generation token.
			result.kind = DetectRuntimeKind(result.adapterName, result.cuda);
			if (result.kind == DLSSNRRuntimeKind::Unknown) {
				result.cuda = QueryCudaComputeCapability(description.AdapterLuid);
				result.kind = DetectRuntimeKind(result.adapterName, result.cuda);
			}
		}
	}

	std::wstring runtimeOverride(32768, L'\0');
	const DWORD overrideLength = GetEnvironmentVariableW(
		L"MAGPIE_DLSSNR_RUNTIME", runtimeOverride.data(),
		static_cast<DWORD>(runtimeOverride.size()));
	if (overrideLength && overrideLength < runtimeOverride.size()) {
		runtimeOverride.resize(overrideLength);
		if (_wcsicmp(runtimeOverride.c_str(), L"auto") != 0) {
			result.forced = true;
			const DLSSNRRuntimeKind forcedKind = ParseRuntimeOverride(runtimeOverride);
			if (forcedKind != DLSSNRRuntimeKind::Unknown) {
				result.kind = forcedKind;
				result.path = RuntimePathForKind(applicationDirectory, result.kind);
			} else {
				result.kind = DLSSNRRuntimeKind::Custom;
				result.path = std::filesystem::path(runtimeOverride);
				if (result.path.is_relative()) result.path = applicationDirectory / result.path;
				std::error_code error;
				if (std::filesystem::is_directory(result.path, error)) {
					result.path /= L"nvngx_dlssnr.dll";
				}
			}
		}
	}

	if (result.path.empty()) {
		if (result.kind != DLSSNRRuntimeKind::Unknown) {
			result.path = RuntimePathForKind(applicationDirectory, result.kind);
		} else {
			result.kind = DLSSNRRuntimeKind::LegacyRoot;
			result.path = applicationDirectory / L"nvngx_dlssnr.dll";
		}
	}

	std::error_code error;
	if (!result.forced && !std::filesystem::is_regular_file(result.path, error)) {
		const std::filesystem::path legacyPath =
			applicationDirectory / L"nvngx_dlssnr.dll";
		error.clear();
		if (std::filesystem::is_regular_file(legacyPath, error)) {
			Logger::Get().Warn(fmt::format(
				"Selected DLSSNR runtime is missing; using legacy root DLL instead: {}",
				StrHelper::UTF16ToUTF8(result.path.wstring())));
			result.kind = DLSSNRRuntimeKind::LegacyRoot;
			result.path = legacyPath;
		}
	}
	return result;
}

}

struct DLSSNRFilter::Impl {
	static constexpr uint32_t COMMAND_SLOT_COUNT = 4;
	struct CommandSlot {
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> commandList;
		uint64_t completionValue = 0;
		uint32_t timestampQuery = 0;
		FrameGuidanceFrameId timestampFrameId = 0;
		bool timestampPending = false;
	};

	using SnippetInitExtFn = NVSDK_NGX_Result(NVSDK_CONV*)(
		unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
		const NVSDK_NGX_Parameter*);
	using CreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(
		ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
		NVSDK_NGX_Handle**);
	using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(
		ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
		const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
	using ReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
	using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
	using GetModuleFileNameWFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

	~Impl();

	ID3D11Device5* device11 = nullptr;
	ID3D11DeviceContext4* context11 = nullptr;
	winrt::com_ptr<ID3D12Device> device12;
	winrt::com_ptr<ID3D12CommandQueue> queue12;
	winrt::com_ptr<ID3D12CommandAllocator> allocator12;
	winrt::com_ptr<ID3D12GraphicsCommandList> commandList12;
	std::array<CommandSlot, COMMAND_SLOT_COUNT> commandSlots;
	uint32_t nextCommandSlot = 0;
	winrt::com_ptr<ID3D11Texture2D> sharedInput11;
	winrt::com_ptr<ID3D11Texture2D> sharedOutput11;
	winrt::com_ptr<ID3D11ShaderResourceView> inputSrv11;
	ID3D11Texture2D* inputTexture11 = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> sharedInputSrv11;
	winrt::com_ptr<ID3D11ShaderResourceView> sharedOutputSrv11;
	winrt::com_ptr<ID3D11UnorderedAccessView> sharedInputUav11;
	winrt::com_ptr<ID3D11ComputeShader> colorConvertShader11;
	winrt::com_ptr<ID3D11ComputeShader> passFeedbackShader11;
	winrt::com_ptr<ID3D11Buffer> passFeedbackCb11;
	winrt::com_ptr<ID3D11Texture2D> passFeedbackScratch11;
	winrt::com_ptr<ID3D11UnorderedAccessView> passFeedbackScratchUav11;
	winrt::com_ptr<ID3D11Texture2D> passProxy11;
	winrt::com_ptr<ID3D11ComputeShader> detailReprojectShader11;
	winrt::com_ptr<ID3D11Buffer> detailCb11;
	winrt::com_ptr<ID3D11SamplerState> detailSampler11;
	winrt::com_ptr<ID3D11Texture2D> detailStore11;
	winrt::com_ptr<ID3D11ShaderResourceView> detailStoreSrv11;
	winrt::com_ptr<ID3D11UnorderedAccessView> detailStoreUav11;
	winrt::com_ptr<ID3D11Texture2D> detailColor11;
	winrt::com_ptr<ID3D11ShaderResourceView> detailColorSrv11;
	winrt::com_ptr<ID3D11UnorderedAccessView> detailColorUav11;
	winrt::com_ptr<ID3D11Texture2D> detailEstimate11;
	winrt::com_ptr<ID3D11ShaderResourceView> detailEstimateSrv11;
	winrt::com_ptr<ID3D11UnorderedAccessView> detailEstimateUav11;
	ID3D11Texture2D* detailAnswerRead = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> detailAnswerReadSrv11;
	winrt::com_ptr<ID3D11Texture2D> detailAnswer11;
	winrt::com_ptr<ID3D11ShaderResourceView> detailAnswerSrv11;
	winrt::com_ptr<ID3D11Texture2D> detailInput11;
	winrt::com_ptr<ID3D11ShaderResourceView> detailInputSrv11;
	ID3D11Texture2D* detailCurrentTexture = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> detailCurrentSrv11;
	ID3D11Texture2D* detailMotionTexture = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> detailMotionSrv11;
	ID3D11Texture2D* detailConfidenceTexture = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> detailConfidenceSrv11;
	ID3D11Texture2D* detailOutputTexture = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> detailOutputUav11;
	bool detailValid = false;
	bool detailReuseNext = false;
	bool detailReuseFailed = false;
	bool deviceRemoved = false;
	winrt::com_ptr<ID3D11ComputeShader> colorDownsampleVerticalShader11;
	winrt::com_ptr<ID3D11ComputeShader> colorDownsampleHorizontalShader11;
	winrt::com_ptr<ID3D11ComputeShader> guidanceDownsampleShader11;
	winrt::com_ptr<ID3D11ComputeShader> residualPrepareShader11;
	winrt::com_ptr<ID3D11ComputeShader> residualHorizontalShader11;
	winrt::com_ptr<ID3D11ComputeShader> residualVerticalCompositeShader11;
	winrt::com_ptr<ID3D11Buffer> resampleConstants11;
	winrt::com_ptr<ID3D11Texture2D> reducedMotion11;
	winrt::com_ptr<ID3D11Texture2D> reducedDepth11;
	winrt::com_ptr<ID3D11Texture2D> reducedConfidence11;
	winrt::com_ptr<ID3D11UnorderedAccessView> reducedMotionUav11;
	winrt::com_ptr<ID3D11UnorderedAccessView> reducedDepthUav11;
	winrt::com_ptr<ID3D11UnorderedAccessView> reducedConfidenceUav11;
	winrt::com_ptr<ID3D11ShaderResourceView> guidanceMotionSrv11;
	winrt::com_ptr<ID3D11ShaderResourceView> guidanceDepthSrv11;
	winrt::com_ptr<ID3D11ShaderResourceView> guidanceConfidenceSrv11;
	ID3D11Texture2D* guidanceMotion11 = nullptr;
	ID3D11Texture2D* guidanceDepth11 = nullptr;
	ID3D11Texture2D* guidanceConfidence11 = nullptr;
	winrt::com_ptr<ID3D11Texture2D> resampleIntermediate11;
	winrt::com_ptr<ID3D11ShaderResourceView> resampleIntermediateSrv11;
	winrt::com_ptr<ID3D11UnorderedAccessView> resampleIntermediateUav11;
	winrt::com_ptr<ID3D11Texture2D> residual11;
	winrt::com_ptr<ID3D11ShaderResourceView> residualSrv11;
	winrt::com_ptr<ID3D11UnorderedAccessView> residualUav11;
	winrt::com_ptr<ID3D11Texture2D> compositeOutput11;
	winrt::com_ptr<ID3D11UnorderedAccessView> compositeOutputUav11;
	winrt::com_ptr<ID3D12Resource> sharedInput12;
	winrt::com_ptr<ID3D12Resource> sharedOutput12;
	std::unique_ptr<FrameGuidanceD3D12Interop> guidanceInterop;
	winrt::com_ptr<ID3D11Fence> fence11;
	winrt::com_ptr<ID3D12Fence> fence12;
	wil::unique_event_nothrow fenceEvent;
	winrt::com_ptr<ID3D12QueryHeap> timestampQueryHeap;
	winrt::com_ptr<ID3D12Resource> timestampReadback;
	uint64_t timestampFrequency = 0;
	NVSDK_NGX_Parameter* parameters = nullptr;
	NVSDK_NGX_Handle* feature = nullptr;
	static constexpr uint32_t kMaxPasses = 4;
	std::array<NVSDK_NGX_Handle*, kMaxPasses> passFeatures{};
	bool extraPassRejected = false;
	int renderPreset = 0;
	HMODULE snippetModule = nullptr;
	SnippetInitExtFn snippetInitExt = nullptr;
	CreateFeatureFn snippetCreateFeature = nullptr;
	EvaluateFeatureFn snippetEvaluateFeature = nullptr;
	ReleaseFeatureFn snippetReleaseFeature = nullptr;
	ShutdownFn snippetShutdown = nullptr;
	void** snippetGetModuleFileNameIatSlot = nullptr;
	uint64_t fenceValue = 0;
	uint64_t evaluateCount = 0;
	uint64_t evaluateSuccessCount = 0;
	uint64_t evaluateFailureCount = 0;
	TimingWindow slotWaitTimings;
	TimingWindow inputPrepareTimings;
	TimingWindow guidancePrepareTimings;
	TimingWindow evaluateCpuTimings;
	TimingWindow submitTimings;
	TimingWindow evaluateGpuTimings;
	FrameGuidanceFrameId lastGuidanceResetFrameId =
		std::numeric_limits<FrameGuidanceFrameId>::max();
	uint32_t sourceWidth = 0;
	uint32_t sourceHeight = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	bool convertInputToRgba = false;
	bool useResolutionScaling = false;
	bool coreInitialized = false;
	bool snippetInitialized = false;
	bool snippetCallerHookInstalled = false;
	bool useSignedSnippet = false;
	bool resetHistory = true;
	bool disabled = false;
};

namespace {

std::atomic<void*> g_snippetCallerHookOwner = nullptr;
std::atomic<HMODULE> g_snippetCallerModule = nullptr;
std::atomic<DLSSNRFilter::Impl::GetModuleFileNameWFn>
	g_snippetOriginalGetModuleFileNameW = nullptr;

LONG CaptureNgxException(DWORD code, DWORD* sehCode) noexcept {
	*sehCode = code;
	return EXCEPTION_EXECUTE_HANDLER;
}

NVSDK_NGX_Result CallCoreInitSafely(
	const char* projectId,
	const char* engineVersion,
	const wchar_t* applicationDataPath,
	ID3D12Device* device,
	const NVSDK_NGX_FeatureCommonInfo* featureInfo,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		return NVSDK_NGX_D3D12_Init_with_ProjectID(
			projectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, engineVersion,
			applicationDataPath, device, featureInfo, NVSDK_NGX_Version_API);
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

NVSDK_NGX_Result CallAllocateParametersSafely(
	NVSDK_NGX_Parameter** parameters,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		return NVSDK_NGX_D3D12_AllocateParameters(parameters);
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

NVSDK_NGX_Result CallCreateFeatureSafely(
	DLSSNRFilter::Impl::CreateFeatureFn function,
	ID3D12GraphicsCommandList* commandList,
	NVSDK_NGX_Feature featureId,
	NVSDK_NGX_Parameter* parameters,
	NVSDK_NGX_Handle** feature,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		return function(commandList, featureId, parameters, feature);
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

NVSDK_NGX_Result CallEvaluateFeatureSafely(
	DLSSNRFilter::Impl::EvaluateFeatureFn function,
	ID3D12GraphicsCommandList* commandList,
	const NVSDK_NGX_Handle* feature,
	const NVSDK_NGX_Parameter* parameters,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		return function(commandList, feature, parameters, nullptr);
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

NVSDK_NGX_Result CallReleaseFeatureSafely(
	DLSSNRFilter::Impl::ReleaseFeatureFn function,
	NVSDK_NGX_Handle* feature,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		return function(feature);
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

NVSDK_NGX_Result CallDestroyParametersSafely(
	NVSDK_NGX_Parameter* parameters,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		return NVSDK_NGX_D3D12_DestroyParameters(parameters);
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

NVSDK_NGX_Result CallShutdownSafely(
	DLSSNRFilter::Impl::ShutdownFn function,
	ID3D12Device* device,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		return function(device);
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

NVSDK_NGX_Result CallSnippetInitSafely(
	DLSSNRFilter::Impl::SnippetInitExtFn function,
	const wchar_t* applicationDataPath,
	ID3D12Device* device,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		return function(
			DLSSNR_SIGNED_SNIPPET_APPLICATION_ID, applicationDataPath,
			device, NVSDK_NGX_Version_API, nullptr);
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

DWORD WINAPI SnippetGetModuleFileNameW(
	HMODULE module,
	LPWSTR filename,
	DWORD size
) noexcept {
	if (module == g_snippetCallerModule.load(std::memory_order_acquire)) {
		constexpr wchar_t AUTHORIZED_CALLER[] = L"nvngx.dll";
		constexpr DWORD AUTHORIZED_CALLER_LENGTH = ARRAYSIZE(AUTHORIZED_CALLER) - 1;
		if (!filename || !size) {
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return 0;
		}
		if (size <= AUTHORIZED_CALLER_LENGTH) {
			if (size > 1) {
				std::memcpy(filename, AUTHORIZED_CALLER, (size - 1) * sizeof(wchar_t));
			}
			filename[size - 1] = L'\0';
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return size;
		}
		std::memcpy(filename, AUTHORIZED_CALLER, sizeof(AUTHORIZED_CALLER));
		return AUTHORIZED_CALLER_LENGTH;
	}

	const auto original =
		g_snippetOriginalGetModuleFileNameW.load(std::memory_order_acquire);
	if (original) return original(module, filename, size);
	SetLastError(ERROR_INVALID_FUNCTION);
	return 0;
}

template <typename T>
void* FunctionAddress(T function) noexcept {
	void* result = nullptr;
	static_assert(sizeof(function) == sizeof(result));
	std::memcpy(&result, &function, sizeof(result));
	return result;
}

void** FindImportedFunctionSlot(HMODULE module, const char* functionName) noexcept {
	if (!module || !functionName) return nullptr;
	auto* base = reinterpret_cast<std::byte*>(module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE ||
		nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		return nullptr;
	}

	const auto& directory =
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!directory.VirtualAddress || !directory.Size ||
		directory.VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
		directory.Size > nt->OptionalHeader.SizeOfImage ||
		directory.VirtualAddress > nt->OptionalHeader.SizeOfImage - directory.Size) {
		return nullptr;
	}

	auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
		base + directory.VirtualAddress);
	const auto* descriptorEnd = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
		base + directory.VirtualAddress + directory.Size);
	for (; descriptor < descriptorEnd && descriptor->Name; ++descriptor) {
		if (descriptor->Name >= nt->OptionalHeader.SizeOfImage) continue;
		const char* libraryName = reinterpret_cast<const char*>(base + descriptor->Name);
		if (_stricmp(libraryName, "KERNEL32.dll") != 0 &&
			_stricmp(libraryName, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0 &&
			_stricmp(libraryName, "api-ms-win-core-libraryloader-l1-1-0.dll") != 0) {
			continue;
		}
		if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) continue;
		auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
			base + descriptor->OriginalFirstThunk);
		auto* addressThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
			base + descriptor->FirstThunk);
		for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addressThunk) {
			if (IMAGE_SNAP_BY_ORDINAL64(nameThunk->u1.Ordinal)) continue;
			const uint32_t nameRva = static_cast<uint32_t>(nameThunk->u1.AddressOfData);
			if (nameRva >= nt->OptionalHeader.SizeOfImage) return nullptr;
			const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + nameRva);
			if (std::strcmp(reinterpret_cast<const char*>(import->Name), functionName) == 0) {
				return reinterpret_cast<void**>(&addressThunk->u1.Function);
			}
		}
	}
	return nullptr;
}

bool InstallSnippetCallerCompatibility(DLSSNRFilter::Impl& impl) noexcept {
	impl.snippetGetModuleFileNameIatSlot =
		FindImportedFunctionSlot(impl.snippetModule, "GetModuleFileNameW");
	if (!impl.snippetGetModuleFileNameIatSlot) {
		Logger::Get().Error(
			"DLSSNR signed snippet has no GetModuleFileNameW import");
		return false;
	}

	void* expectedOwner = nullptr;
	if (!g_snippetCallerHookOwner.compare_exchange_strong(
		expectedOwner, &impl, std::memory_order_acq_rel)) {
		Logger::Get().Error(
			"DLSSNR signed snippet caller compatibility is already owned by another session");
		return false;
	}

	void* hookAddress = FunctionAddress(&SnippetGetModuleFileNameW);
	HMODULE callerModule = nullptr;
	if (!GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(hookAddress), &callerModule)) {
		g_snippetCallerHookOwner.store(nullptr, std::memory_order_release);
		Logger::Get().Win32Error(
			"Resolve DLSSNR signed snippet caller module failed");
		return false;
	}

	DWORD oldProtection = 0;
	if (!VirtualProtect(
		impl.snippetGetModuleFileNameIatSlot, sizeof(void*),
		PAGE_READWRITE, &oldProtection)) {
		g_snippetCallerHookOwner.store(nullptr, std::memory_order_release);
		Logger::Get().Win32Error(
			"Make DLSSNR signed snippet IAT writable failed");
		return false;
	}

	g_snippetCallerModule.store(callerModule, std::memory_order_release);
	void* original = InterlockedExchangePointer(
		reinterpret_cast<void* volatile*>(impl.snippetGetModuleFileNameIatSlot),
		hookAddress);
	DLSSNRFilter::Impl::GetModuleFileNameWFn originalFunction = nullptr;
	std::memcpy(&originalFunction, &original, sizeof(original));
	g_snippetOriginalGetModuleFileNameW.store(
		originalFunction, std::memory_order_release);
	impl.snippetCallerHookInstalled = true;

	DWORD ignoredProtection = 0;
	if (!VirtualProtect(
		impl.snippetGetModuleFileNameIatSlot, sizeof(void*),
		oldProtection, &ignoredProtection)) {
		Logger::Get().Win32Error(
			"Restore DLSSNR signed snippet IAT protection failed");
	}
	FlushInstructionCache(
		GetCurrentProcess(), impl.snippetGetModuleFileNameIatSlot, sizeof(void*));
	if (!originalFunction) {
		Logger::Get().Error(
			"DLSSNR signed snippet GetModuleFileNameW import was null");
		return false;
	}
	return true;
}

bool RestoreSnippetCallerCompatibility(DLSSNRFilter::Impl& impl) noexcept {
	if (!impl.snippetCallerHookInstalled) return true;
	const auto original =
		g_snippetOriginalGetModuleFileNameW.load(std::memory_order_acquire);
	DWORD oldProtection = 0;
	if (!impl.snippetGetModuleFileNameIatSlot || !VirtualProtect(
			impl.snippetGetModuleFileNameIatSlot, sizeof(void*),
			PAGE_READWRITE, &oldProtection)) {
		Logger::Get().Win32Error(
			"Restore DLSSNR signed snippet caller IAT failed");
		return false;
	}

	InterlockedExchangePointer(
		reinterpret_cast<void* volatile*>(impl.snippetGetModuleFileNameIatSlot),
		FunctionAddress(original));
	DWORD ignoredProtection = 0;
	VirtualProtect(
		impl.snippetGetModuleFileNameIatSlot, sizeof(void*),
		oldProtection, &ignoredProtection);
	FlushInstructionCache(
		GetCurrentProcess(), impl.snippetGetModuleFileNameIatSlot, sizeof(void*));
	impl.snippetCallerHookInstalled = false;
	impl.snippetGetModuleFileNameIatSlot = nullptr;
	g_snippetOriginalGetModuleFileNameW.store(nullptr, std::memory_order_release);
	g_snippetCallerModule.store(nullptr, std::memory_order_release);
	void* expectedOwner = &impl;
	g_snippetCallerHookOwner.compare_exchange_strong(
		expectedOwner, nullptr, std::memory_order_acq_rel);
	return true;
}

}

static bool WaitForFence(DLSSNRFilter::Impl& impl, uint64_t value) noexcept {
	if (!value || impl.fence12->GetCompletedValue() >= value) {
		return true;
	}
	if (!impl.fenceEvent) {
		Logger::Get().Error("DLSSNR fence event is unavailable");
		return false;
	}
	ResetEvent(impl.fenceEvent.get());
	const HRESULT hr = impl.fence12->SetEventOnCompletion(
		value, impl.fenceEvent.get());
	if (FAILED(hr)) {
		Logger::Get().ComError("Set DLSSNR fence event failed", hr);
		return false;
	}
	impl.fenceEvent.wait();
	return true;
}

static bool WaitForQueue(DLSSNRFilter::Impl& impl) noexcept {
	const uint64_t value = ++impl.fenceValue;
	HRESULT hr = impl.queue12->Signal(impl.fence12.get(), value);
	if (FAILED(hr)) {
		Logger::Get().ComError("Signal DLSSNR D3D12 fence failed", hr);
		return false;
	}
	return WaitForFence(impl, value);
}

static bool IsGpuDeviceLost(HRESULT hr) noexcept {
	return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG ||
		hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

static void NoteDlssnrDeviceLost(DLSSNRFilter::Impl& impl, HRESULT hr) noexcept {
	if (impl.deviceRemoved) return;
	impl.deviceRemoved = true;
	impl.disabled = true;
	impl.detailValid = false;
	impl.detailReuseNext = false;
	impl.detailReuseFailed = false;
	Logger::Get().ComError(
		"DLSSNR stopped because the GPU device was removed. Quit Magpie completely before scaling again",
		hr);
}

static bool DlssnrDeviceIsRemoved(DLSSNRFilter::Impl& impl) noexcept {
	if (impl.deviceRemoved) return true;
	if (!impl.device12) return false;
	const HRESULT removed = impl.device12->GetDeviceRemovedReason();
	if (SUCCEEDED(removed)) return false;
	NoteDlssnrDeviceLost(impl, removed);
	return true;
}

static void CollectGpuTiming(
	DLSSNRFilter::Impl& impl,
	DLSSNRFilter::Impl::CommandSlot& slot
) noexcept {
	if (impl.deviceRemoved || !slot.timestampPending || !impl.timestampReadback ||
		!impl.timestampFrequency) return;
	const size_t offset = size_t(slot.timestampQuery) * sizeof(uint64_t);
	const D3D12_RANGE readRange{ offset, offset + sizeof(uint64_t) * 2 };
	void* mapped = nullptr;
	const HRESULT hr = impl.timestampReadback->Map(0, &readRange, &mapped);
	if (FAILED(hr) || !mapped) {
		if (IsGpuDeviceLost(hr)) {
			NoteDlssnrDeviceLost(impl, hr);
		} else {
			Logger::Get().ComError("Read DLSSNR GPU timestamp failed", hr);
		}
		slot.timestampPending = false;
		return;
	}
	const auto* timestamps = reinterpret_cast<const uint64_t*>(
		static_cast<const uint8_t*>(mapped) + offset);
	if (timestamps[1] >= timestamps[0]) {
		const double gpuMs = double(timestamps[1] - timestamps[0]) * 1000.0 /
			double(impl.timestampFrequency);
		impl.evaluateGpuTimings.Add(gpuMs);
		FrameGuidancePerformance::PublishDlssnrGpuTiming(gpuMs);
	}
	const D3D12_RANGE writtenRange{ 0, 0 };
	impl.timestampReadback->Unmap(0, &writtenRange);
	slot.timestampPending = false;
}

static std::string FormatTimingSummary(
	std::string_view name,
	const TimingSummary& value
) {
	return fmt::format(
		"{}[n={} avg={:.3f} p95={:.3f} p99={:.3f} max={:.3f}]",
		name, value.count, value.average, value.p95, value.p99, value.maximum);
}

DLSSNRFilter::Impl::~Impl() {
	if (queue12 && fence12) {
		WaitForQueue(*this);
	}
	{
		const auto function = useSignedSnippet ? snippetReleaseFeature :
			static_cast<ReleaseFeatureFn>(&NVSDK_NGX_D3D12_ReleaseFeature);
		auto releaseOne = [&](NVSDK_NGX_Handle*& handle) {
			if (!handle || !function) return;
			DWORD sehCode = 0;
			const NVSDK_NGX_Result result =
				CallReleaseFeatureSafely(function, handle, &sehCode);
			if (sehCode) {
				Logger::Get().Warn(fmt::format(
					"DLSSNR ReleaseFeature raised SEH {:#x}", sehCode));
			} else if (!NGXSucceeded(result)) {
				Logger::Get().Warn(fmt::format(
					"DLSSNR ReleaseFeature failed ({:#x})", (uint32_t)result));
			}
			handle = nullptr;
		};
		for (NVSDK_NGX_Handle*& handle : passFeatures) {
			if (feature == handle) feature = nullptr;
			releaseOne(handle);
		}
		releaseOne(feature);
	}
	if (parameters) {
		DWORD sehCode = 0;
		const NVSDK_NGX_Result result =
			CallDestroyParametersSafely(parameters, &sehCode);
		if (sehCode) {
			Logger::Get().Warn(fmt::format(
				"DLSSNR DestroyParameters raised SEH {:#x}", sehCode));
		} else if (!NGXSucceeded(result)) {
			Logger::Get().Warn(fmt::format(
				"DLSSNR DestroyParameters failed ({:#x})", (uint32_t)result));
		}
		parameters = nullptr;
	}
	if (snippetInitialized && snippetShutdown && device12) {
		DWORD sehCode = 0;
		const NVSDK_NGX_Result result =
			CallShutdownSafely(snippetShutdown, device12.get(), &sehCode);
		if (sehCode) {
			Logger::Get().Warn(fmt::format(
				"DLSSNR signed snippet Shutdown1 raised SEH {:#x}", sehCode));
		} else if (!NGXSucceeded(result)) {
			Logger::Get().Warn(fmt::format(
				"DLSSNR signed snippet Shutdown1 failed ({:#x})", (uint32_t)result));
		}
		snippetInitialized = false;
	}
	DLSSNRNvapi::Restore(snippetModule);
	const bool callerCompatibilityRestored =
		RestoreSnippetCallerCompatibility(*this);
	if (snippetModule) {
		if (callerCompatibilityRestored) {
			if (!FreeLibrary(snippetModule)) {
				Logger::Get().Win32Error(
					"Release DLSSNR signed snippet DLL failed");
			}
		} else {
			Logger::Get().Warn(
				"DLSSNR signed snippet DLL retained because caller IAT restoration failed");
		}
		snippetModule = nullptr;
	}
	if (coreInitialized && device12) {
		DWORD sehCode = 0;
		const NVSDK_NGX_Result result = CallShutdownSafely(
			static_cast<ShutdownFn>(&NVSDK_NGX_D3D12_Shutdown1),
			device12.get(), &sehCode);
		if (sehCode) {
			Logger::Get().Warn(fmt::format(
				"DLSSNR parameter Core Shutdown1 raised SEH {:#x}", sehCode));
		} else if (!NGXSucceeded(result)) {
			Logger::Get().Warn(fmt::format(
				"DLSSNR parameter Core Shutdown1 failed ({:#x})", (uint32_t)result));
		}
		coreInitialized = false;
	}
}

static bool CreateSharedTexture(
	DLSSNRFilter::Impl& impl,
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
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED |
		D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	HRESULT hr = impl.device11->CreateTexture2D(&desc, nullptr, texture11.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSNR shared D3D11 texture failed", hr);
		return false;
	}

	winrt::com_ptr<IDXGIResource1> dxgiResource;
	hr = texture11->QueryInterface(IID_PPV_ARGS(dxgiResource.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Query DLSSNR shared IDXGIResource1 failed", hr);
		return false;
	}
	HANDLE rawHandle = nullptr;
	hr = dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawHandle);
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSNR texture shared handle failed", hr);
		return false;
	}
	wil::unique_handle handle(rawHandle);
	hr = impl.device12->OpenSharedHandle(handle.get(), IID_PPV_ARGS(texture12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Open DLSSNR texture in D3D12 failed", hr);
		return false;
	}
	return true;
}

static bool UpdateInputShaderResource(
	DLSSNRFilter::Impl& impl,
	ID3D11Texture2D* input
) noexcept;

static bool CreateComputeShader(
	DLSSNRFilter::Impl& impl,
	std::string_view source,
	const char* entryPoint,
	const char* sourceName,
	winrt::com_ptr<ID3D11ComputeShader>& shader
) noexcept;

static bool CreateResolutionScalingResources(
	DLSSNRFilter::Impl& impl,
	ID3D11Texture2D* input,
	const D3D11_TEXTURE2D_DESC& outputDesc
) noexcept;

static bool CreateGuidanceResampleResources(DLSSNRFilter::Impl& impl) noexcept;

static bool UpdateGuidanceResources(
	DLSSNRFilter::Impl& impl,
	const FrameGuidanceView& view,
	FrameGuidanceFrameId frameId
) noexcept {
	return impl.guidanceInterop && impl.guidanceInterop->Update(
		view, frameId, { impl.width, impl.height });
}

static void SetSubrect(
	NVSDK_NGX_Parameter* parameters,
	const ResourceParameters& resource,
	FrameGuidanceRegion region
) {
	parameters->Set(resource.baseX, region.x);
	parameters->Set(resource.baseY, region.y);
	parameters->Set(resource.width, region.width);
	parameters->Set(resource.height, region.height);
}

static NVSDK_NGX_Result NVSDK_CONV SetScalingRatioCallback(
	NVSDK_NGX_Parameter* parameters
) noexcept {
	__try {
		if (!parameters) return NVSDK_NGX_Result_FAIL_InvalidParameter;
		parameters->Set(PARAM_SCALING_RATIO, 1.0f);
		return NVSDK_NGX_Result_Success;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

static void SetCreateParametersUnsafe(
	DLSSNRFilter::Impl& impl,
	const DLSSNRSettings& settings
) {
	impl.parameters->Set(PARAM_WIDTH, impl.width);
	impl.parameters->Set(PARAM_HEIGHT, impl.height);
	impl.parameters->Set(PARAM_INPUT_WIDTH, impl.width);
	impl.parameters->Set(PARAM_INPUT_HEIGHT, impl.height);
	impl.parameters->Set(PARAM_OUTPUT_WIDTH, impl.width);
	impl.parameters->Set(PARAM_OUTPUT_HEIGHT, impl.height);
	impl.parameters->Set(PARAM_OUTPUT_DOT_WIDTH, impl.width);
	impl.parameters->Set(PARAM_OUTPUT_DOT_HEIGHT, impl.height);
	impl.parameters->Set(PARAM_UPSCALING, 0u);
	impl.parameters->Set(PARAM_SCALE, 1.0f);
	impl.parameters->Set(PARAM_SCALING_RATIO, 1.0f);
	impl.parameters->Set(
		PARAM_SCALING_RATIO_CALLBACK,
		FunctionAddress(&SetScalingRatioCallback));
	impl.parameters->Set(PARAM_PRESET, static_cast<unsigned>(settings.renderPreset));
	impl.parameters->Set(PARAM_STYLE, settings.style);
	impl.parameters->Set(PARAM_INTENSITY, settings.intensity);
	impl.parameters->Set(PARAM_LOCAL_TONE, settings.localToneStrength);
	impl.parameters->Set(PARAM_LOCAL_STRUCTURE, settings.localStructureStrength);
	impl.parameters->Set("DLSSNR.SkinStructureStrength", settings.skinStructureStrength);
	impl.parameters->Set(PARAM_AUTO_MASK, settings.useAutoMask ? 1 : 0);
	impl.parameters->Set(NVSDK_NGX_Parameter_Width, impl.width);
	impl.parameters->Set(NVSDK_NGX_Parameter_Height, impl.height);
	impl.parameters->Set(
		NVSDK_NGX_Parameter_PerfQualityValue,
		static_cast<int>(NVSDK_NGX_PerfQuality_Value_Balanced));
	impl.parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
	impl.parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
}

static bool SetCreateParametersSafely(
	DLSSNRFilter::Impl& impl,
	const DLSSNRSettings& settings,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		SetCreateParametersUnsafe(impl, settings);
		return true;
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return false;
	}
}

static bool InitializeSignedSnippet(
	DLSSNRFilter::Impl& impl,
	const std::filesystem::path& applicationDirectory,
	const std::filesystem::path& dllPath
) noexcept {
	if (!impl.snippetModule) {
		impl.snippetModule = LoadLibraryExW(
			dllPath.c_str(), nullptr,
			LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	}
	if (!impl.snippetModule) {
		Logger::Get().Win32Error(fmt::format(
			"Load selected nvngx_dlssnr.dll failed: {}",
			StrHelper::UTF16ToUTF8(dllPath.wstring())));
		return false;
	}

	impl.snippetInitExt = GetExport<DLSSNRFilter::Impl::SnippetInitExtFn>(
		impl.snippetModule, "NVSDK_NGX_D3D12_Init_Ext");
	impl.snippetCreateFeature = GetExport<DLSSNRFilter::Impl::CreateFeatureFn>(
		impl.snippetModule, "NVSDK_NGX_D3D12_CreateFeature");
	impl.snippetEvaluateFeature = GetExport<DLSSNRFilter::Impl::EvaluateFeatureFn>(
		impl.snippetModule, "NVSDK_NGX_D3D12_EvaluateFeature");
	impl.snippetReleaseFeature = GetExport<DLSSNRFilter::Impl::ReleaseFeatureFn>(
		impl.snippetModule, "NVSDK_NGX_D3D12_ReleaseFeature");
	impl.snippetShutdown = GetExport<DLSSNRFilter::Impl::ShutdownFn>(
		impl.snippetModule, "NVSDK_NGX_D3D12_Shutdown1");
	if (!impl.snippetInitExt || !impl.snippetCreateFeature ||
		!impl.snippetEvaluateFeature || !impl.snippetReleaseFeature ||
		!impl.snippetShutdown) {
		Logger::Get().Error("DLSSNR signed snippet exports are incomplete");
		return false;
	}
	if (!InstallSnippetCallerCompatibility(impl)) return false;
	DLSSNRNvapi::Install(impl.snippetModule);

	DWORD sehCode = 0;
	const NVSDK_NGX_Result result = CallSnippetInitSafely(
		impl.snippetInitExt, applicationDirectory.c_str(),
		impl.device12.get(), &sehCode);
	if (sehCode) {
		Logger::Get().Error(fmt::format(
			"DLSSNR signed snippet Init_Ext raised SEH {:#x}", sehCode));
		return false;
	}
	if (!NGXSucceeded(result)) {
		Logger::Get().Error(fmt::format(
			"DLSSNR signed snippet Init_Ext failed ({:#x})", (uint32_t)result));
		return false;
	}
	impl.snippetInitialized = true;
	impl.useSignedSnippet = true;
	return true;
}

static void SetEvaluateParametersUnsafe(
	DLSSNRFilter::Impl& impl,
	const DLSSNRSettings& settings,
	const FrameGuidanceView& guidance,
	bool guidanceReset
) {
	impl.parameters->Set(PARAM_COLOR, impl.sharedInput12.get());
	impl.parameters->Set(PARAM_OUTPUT, impl.sharedOutput12.get());
	impl.parameters->Set(PARAM_MVEC, impl.guidanceInterop->Motion());
	impl.parameters->Set(PARAM_DEPTH, impl.guidanceInterop->Depth());
	const FrameGuidanceRegion full = FrameGuidanceRegion::Full(
		{ impl.width, impl.height });
	SetSubrect(impl.parameters, RESOURCE_PARAMETERS[0], full);
	SetSubrect(impl.parameters, RESOURCE_PARAMETERS[1], full);
	SetSubrect(impl.parameters, RESOURCE_PARAMETERS[2],
		guidance.motion.metadata.validRegion);
	SetSubrect(impl.parameters, RESOURCE_PARAMETERS[3],
		guidance.depth.metadata.validRegion);
	// Motion is already in source pixels. Scale 1 keeps that unit.
	impl.parameters->Set(PARAM_MVEC_SCALE_X, 1.0f);
	impl.parameters->Set(PARAM_MVEC_SCALE_Y, 1.0f);
	impl.parameters->Set(PARAM_DEPTH_INVERTED, 1);
	impl.parameters->Set(PARAM_INDICATOR_INVERT_X, 0);
	impl.parameters->Set(PARAM_INDICATOR_INVERT_Y, 0);
	impl.parameters->Set(PARAM_ENABLED, 1);
	impl.parameters->Set(
		PARAM_RESET, impl.resetHistory || guidanceReset ? 1 : 0);
	impl.parameters->Set(PARAM_STYLE, settings.style);
	impl.parameters->Set(PARAM_INTENSITY, settings.intensity);
	impl.parameters->Set(PARAM_LOCAL_TONE, settings.localToneStrength);
	impl.parameters->Set(PARAM_LOCAL_STRUCTURE, settings.localStructureStrength);
	impl.parameters->Set("DLSSNR.SkinStructureStrength", settings.skinStructureStrength);
	impl.parameters->Set(PARAM_PRESET, static_cast<unsigned>(settings.renderPreset));
	impl.parameters->Set(PARAM_AUTO_MASK, settings.useAutoMask ? 1 : 0);
	impl.parameters->Set(PARAM_UI_CORRECTION, 0);
}

static bool SetEvaluateParametersSafely(
	DLSSNRFilter::Impl& impl,
	const DLSSNRSettings& settings,
	const FrameGuidanceView& guidance,
	bool guidanceReset,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		SetEvaluateParametersUnsafe(impl, settings, guidance, guidanceReset);
		return true;
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return false;
	}
}

static bool PrepareInput(
	DLSSNRFilter::Impl& impl,
	ID3D11Texture2D* input
) noexcept {
	if (impl.useResolutionScaling) {
		if (!UpdateInputShaderResource(impl, input)) return false;
		const ResampleConstants constants{
			.sourceWidth = impl.sourceWidth,
			.sourceHeight = impl.sourceHeight,
			.targetWidth = impl.width,
			.targetHeight = impl.height,
			.motionScaleX = float(impl.width) / float(impl.sourceWidth),
			.motionScaleY = float(impl.height) / float(impl.sourceHeight)
		};
		impl.context11->UpdateSubresource(
			impl.resampleConstants11.get(), 0, nullptr, &constants, 0, 0);
		ID3D11ShaderResourceView* srv = impl.inputSrv11.get();
		ID3D11UnorderedAccessView* uav = impl.resampleIntermediateUav11.get();
		ID3D11Buffer* constantBuffer = impl.resampleConstants11.get();
		impl.context11->CSSetShader(
			impl.colorDownsampleVerticalShader11.get(), nullptr, 0);
		impl.context11->CSSetShaderResources(0, 1, &srv);
		impl.context11->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		impl.context11->CSSetConstantBuffers(0, 1, &constantBuffer);
		impl.context11->Dispatch(
			(impl.sourceWidth + 7) / 8, (impl.height + 7) / 8, 1);
		ID3D11ShaderResourceView* nullSrv = nullptr;
		ID3D11UnorderedAccessView* nullUav = nullptr;
		ID3D11Buffer* nullBuffer = nullptr;
		impl.context11->CSSetShaderResources(0, 1, &nullSrv);
		impl.context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		srv = impl.resampleIntermediateSrv11.get();
		uav = impl.sharedInputUav11.get();
		impl.context11->CSSetShader(
			impl.colorDownsampleHorizontalShader11.get(), nullptr, 0);
		impl.context11->CSSetShaderResources(0, 1, &srv);
		impl.context11->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		impl.context11->Dispatch(
			(impl.width + 7) / 8, (impl.height + 7) / 8, 1);
		impl.context11->CSSetShaderResources(0, 1, &nullSrv);
		impl.context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		impl.context11->CSSetConstantBuffers(0, 1, &nullBuffer);
		impl.context11->CSSetShader(nullptr, nullptr, 0);
		return true;
	}
	D3D11_TEXTURE2D_DESC inputDesc{};
	input->GetDesc(&inputDesc);
	// The renderer's captured frame can be BGRA, while every DLSSNR output is
	// RGBA.  Repeated passes feed that RGBA output back as input, so they must
	// bypass the BGRA conversion shader and copy the previous result directly.
	if (inputDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
		impl.context11->CopyResource(impl.sharedInput11.get(), input);
		return true;
	}
	if (inputDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
		!impl.convertInputToRgba || !impl.inputSrv11) {
		Logger::Get().Error(fmt::format(
			"Prepare DLSSNR input encountered unsupported format {}",
			static_cast<uint32_t>(inputDesc.Format)));
		return false;
	}
	ID3D11ShaderResourceView* srv = impl.inputSrv11.get();
	ID3D11UnorderedAccessView* uav = impl.sharedInputUav11.get();
	impl.context11->CSSetShader(impl.colorConvertShader11.get(), nullptr, 0);
	impl.context11->CSSetShaderResources(0, 1, &srv);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	impl.context11->Dispatch((impl.width + 7) / 8, (impl.height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrv = nullptr;
	ID3D11UnorderedAccessView* nullUav = nullptr;
	impl.context11->CSSetShaderResources(0, 1, &nullSrv);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	impl.context11->CSSetShader(nullptr, nullptr, 0);
	return true;
}

static bool UpdateGuidanceShaderResources(
	DLSSNRFilter::Impl& impl,
	const FrameGuidanceView& guidance
) noexcept {
	if (impl.guidanceMotion11 == guidance.motion.texture &&
		impl.guidanceDepth11 == guidance.depth.texture &&
		impl.guidanceConfidence11 == guidance.confidence.texture &&
		impl.guidanceMotionSrv11 && impl.guidanceDepthSrv11 &&
		impl.guidanceConfidenceSrv11) {
		return true;
	}
	winrt::com_ptr<ID3D11ShaderResourceView> motionSrv;
	winrt::com_ptr<ID3D11ShaderResourceView> depthSrv;
	winrt::com_ptr<ID3D11ShaderResourceView> confidenceSrv;
	HRESULT hr = impl.device11->CreateShaderResourceView(
		guidance.motion.texture, nullptr, motionSrv.put());
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateShaderResourceView(
			guidance.depth.texture, nullptr, depthSrv.put());
	}
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateShaderResourceView(
			guidance.confidence.texture, nullptr, confidenceSrv.put());
	}
	if (FAILED(hr)) {
		Logger::Get().ComError(
			"Create DLSSNR Frame Guidance downsample SRVs failed", hr);
		return false;
	}
	impl.guidanceMotionSrv11 = std::move(motionSrv);
	impl.guidanceDepthSrv11 = std::move(depthSrv);
	impl.guidanceConfidenceSrv11 = std::move(confidenceSrv);
	impl.guidanceMotion11 = guidance.motion.texture;
	impl.guidanceDepth11 = guidance.depth.texture;
	impl.guidanceConfidence11 = guidance.confidence.texture;
	return true;
}

static bool PrepareReducedGuidance(
	DLSSNRFilter::Impl& impl,
	const FrameGuidanceView& guidance,
	FrameGuidanceExtent sourceExtent
) noexcept {
	if (!sourceExtent.IsValid() || !UpdateGuidanceShaderResources(impl, guidance)) {
		return false;
	}
	const ResampleConstants constants{
		.sourceWidth = sourceExtent.width,
		.sourceHeight = sourceExtent.height,
		.targetWidth = impl.width,
		.targetHeight = impl.height,
		.motionScaleX = float(impl.width) / float(sourceExtent.width),
		.motionScaleY = float(impl.height) / float(sourceExtent.height)
	};
	impl.context11->UpdateSubresource(
		impl.resampleConstants11.get(), 0, nullptr, &constants, 0, 0);
	ID3D11ShaderResourceView* srvs[]{
		impl.guidanceMotionSrv11.get(),
		impl.guidanceDepthSrv11.get(),
		impl.guidanceConfidenceSrv11.get()
	};
	ID3D11UnorderedAccessView* uavs[]{
		impl.reducedMotionUav11.get(),
		impl.reducedDepthUav11.get(),
		impl.reducedConfidenceUav11.get()
	};
	ID3D11Buffer* constantBuffer = impl.resampleConstants11.get();
	impl.context11->CSSetShader(
		impl.guidanceDownsampleShader11.get(), nullptr, 0);
	impl.context11->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
	impl.context11->CSSetUnorderedAccessViews(
		0, ARRAYSIZE(uavs), uavs, nullptr);
	impl.context11->CSSetConstantBuffers(0, 1, &constantBuffer);
	impl.context11->Dispatch(
		(impl.width + 7) / 8, (impl.height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrvs[ARRAYSIZE(srvs)]{};
	ID3D11UnorderedAccessView* nullUavs[ARRAYSIZE(uavs)]{};
	ID3D11Buffer* nullBuffer = nullptr;
	impl.context11->CSSetShaderResources(
		0, ARRAYSIZE(nullSrvs), nullSrvs);
	impl.context11->CSSetUnorderedAccessViews(
		0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
	impl.context11->CSSetConstantBuffers(0, 1, &nullBuffer);
	impl.context11->CSSetShader(nullptr, nullptr, 0);
	return true;
}

static FrameGuidanceRegion ScaleGuidanceRegion(
	FrameGuidanceRegion region,
	FrameGuidanceExtent source,
	FrameGuidanceExtent target
) noexcept {
	const uint64_t sourceRight = uint64_t(region.x) + region.width;
	const uint64_t sourceBottom = uint64_t(region.y) + region.height;
	const uint32_t left = static_cast<uint32_t>(
		uint64_t(region.x) * target.width / source.width);
	const uint32_t top = static_cast<uint32_t>(
		uint64_t(region.y) * target.height / source.height);
	const uint32_t right = static_cast<uint32_t>(std::min<uint64_t>(
		target.width,
		(sourceRight * target.width + source.width - 1) / source.width));
	const uint32_t bottom = static_cast<uint32_t>(std::min<uint64_t>(
		target.height,
		(sourceBottom * target.height + source.height - 1) / source.height));
	return { left, top, right - left, bottom - top };
}

static FrameGuidanceView MakeReducedGuidance(
	DLSSNRFilter::Impl& impl,
	const FrameGuidanceView& source,
	FrameGuidanceExtent sourceExtent
) noexcept {
	const FrameGuidanceExtent targetExtent{ impl.width, impl.height };
	auto reducedMetadata = [&](const FrameGuidanceMetadata& metadata) {
		FrameGuidanceMetadata result = metadata;
		result.sourceExtent = targetExtent;
		result.validRegion = ScaleGuidanceRegion(
			metadata.validRegion, sourceExtent, targetExtent);
		result.sync = {};
		return result;
	};
	FrameGuidanceView result = source;
	result.motion = {
		.texture = impl.reducedMotion11.get(),
		.format = DXGI_FORMAT_R16G16_FLOAT,
		.metadata = reducedMetadata(source.motion.metadata)
	};
	result.depth = {
		.texture = impl.reducedDepth11.get(),
		.format = DXGI_FORMAT_R32_FLOAT,
		.metadata = reducedMetadata(source.depth.metadata)
	};
	result.confidence = {
		.texture = impl.reducedConfidence11.get(),
		.format = DXGI_FORMAT_R8_UNORM,
		.metadata = reducedMetadata(source.confidence.metadata)
	};
	result.rawDepth = {};
	result.depthResidual = {};
	return result;
}

static bool CompositeResidual(
	DLSSNRFilter::Impl& impl,
	ID3D11Texture2D* output,
	ID3D11ShaderResourceView* reducedDenoised
) noexcept {
	const ResampleConstants constants{
		.sourceWidth = impl.sourceWidth,
		.sourceHeight = impl.sourceHeight,
		.targetWidth = impl.width,
		.targetHeight = impl.height,
		.motionScaleX = float(impl.width) / float(impl.sourceWidth),
		.motionScaleY = float(impl.height) / float(impl.sourceHeight)
	};
	impl.context11->UpdateSubresource(
		impl.resampleConstants11.get(), 0, nullptr, &constants, 0, 0);
	ID3D11ShaderResourceView* prepareSrvs[]{
		impl.sharedInputSrv11.get(), reducedDenoised
	};
	ID3D11UnorderedAccessView* prepareUav = impl.residualUav11.get();
	ID3D11Buffer* constantBuffer = impl.resampleConstants11.get();
	impl.context11->CSSetShader(
		impl.residualPrepareShader11.get(), nullptr, 0);
	impl.context11->CSSetShaderResources(
		0, ARRAYSIZE(prepareSrvs), prepareSrvs);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &prepareUav, nullptr);
	impl.context11->CSSetConstantBuffers(0, 1, &constantBuffer);
	impl.context11->Dispatch(
		(impl.width + 7) / 8, (impl.height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullPrepareSrvs[ARRAYSIZE(prepareSrvs)]{};
	ID3D11UnorderedAccessView* nullUav = nullptr;
	impl.context11->CSSetShaderResources(
		0, ARRAYSIZE(nullPrepareSrvs), nullPrepareSrvs);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);

	ID3D11ShaderResourceView* horizontalSrv = impl.residualSrv11.get();
	ID3D11UnorderedAccessView* horizontalUav =
		impl.resampleIntermediateUav11.get();
	impl.context11->CSSetShader(
		impl.residualHorizontalShader11.get(), nullptr, 0);
	impl.context11->CSSetShaderResources(0, 1, &horizontalSrv);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &horizontalUav, nullptr);
	impl.context11->Dispatch(
		(impl.sourceWidth + 7) / 8, (impl.height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrv = nullptr;
	impl.context11->CSSetShaderResources(0, 1, &nullSrv);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);

	ID3D11ShaderResourceView* verticalSrvs[]{
		impl.inputSrv11.get(), impl.resampleIntermediateSrv11.get()
	};
	ID3D11UnorderedAccessView* compositeUav = impl.compositeOutputUav11.get();
	impl.context11->CSSetShader(
		impl.residualVerticalCompositeShader11.get(), nullptr, 0);
	impl.context11->CSSetShaderResources(
		0, ARRAYSIZE(verticalSrvs), verticalSrvs);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &compositeUav, nullptr);
	impl.context11->Dispatch(
		(impl.sourceWidth + 7) / 8, (impl.sourceHeight + 7) / 8, 1);
	ID3D11ShaderResourceView* nullVerticalSrvs[ARRAYSIZE(verticalSrvs)]{};
	ID3D11Buffer* nullBuffer = nullptr;
	impl.context11->CSSetShaderResources(
		0, ARRAYSIZE(nullVerticalSrvs), nullVerticalSrvs);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	impl.context11->CSSetConstantBuffers(0, 1, &nullBuffer);
	impl.context11->CSSetShader(nullptr, nullptr, 0);
	impl.context11->CopyResource(output, impl.compositeOutput11.get());
	return true;
}

DLSSNRFilter::DLSSNRFilter() = default;
DLSSNRFilter::~DLSSNRFilter() = default;

FrameGuidanceRequirements
DLSSNRFilter::GetFrameGuidanceRequirements() const noexcept {
	if (!_impl || _impl->disabled) return {};
	FrameGuidanceRequirements result{ .zero = true };
	switch (_settings.guidanceMode) {
	case 0:
		result.motion = true;
		result.depth = true;
		break;
	case 1:
		break;
	case 2:
		result.motion = true;
		break;
	case 3:
		result.depth = true;
		break;
	default:
		break;
	}
	result.depthInferenceInterval = _settings.depthInferenceInterval;
	return result;
}

bool DLSSNRFilter::Initialize(
	DeviceResources& resources,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output,
	const DLSSNRSettings& settings
) noexcept {
	g_dlssnrRuntimeState.store(
		static_cast<int32_t>(DLSSNRRuntimeState::Pending), std::memory_order_relaxed);
	struct InitStatusGuard {
		bool completed = false;
		~InitStatusGuard() {
			if (!completed) {
				g_dlssnrRuntimeState.store(
					static_cast<int32_t>(DLSSNRRuntimeState::Failed), std::memory_order_relaxed);
			}
		}
	} initStatus;
	_settings = settings;
	DLSSNRNvapi::SetReuseInterval(_settings.vitReuseInterval);
	DLSSNRNvapi::NoteHybridRequest(_settings.modelPrecision != 0);
	_settings.processingResolutionPercent = std::clamp(
		_settings.processingResolutionPercent, 25u, 100u);
	_impl.reset();
	FrameGuidancePerformance::ResetDlssnrGpuTiming();
	auto impl = std::make_unique<Impl>();
	impl->device11 = resources.GetD3DDevice();
	impl->context11 = resources.GetD3DDC();

	D3D11_TEXTURE2D_DESC inputDesc{};
	D3D11_TEXTURE2D_DESC outputDesc{};
	input->GetDesc(&inputDesc);
	output->GetDesc(&outputDesc);
	if (inputDesc.Width != outputDesc.Width || inputDesc.Height != outputDesc.Height) {
		Logger::Get().Error(fmt::format(
			"DLSSNR requires same-resolution input/output: {}x{} -> {}x{}",
			inputDesc.Width, inputDesc.Height, outputDesc.Width, outputDesc.Height));
		return false;
	}
	const bool supportedInput = inputDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
		inputDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
	if (!supportedInput || outputDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
		Logger::Get().Error(fmt::format(
			"DLSSNR SDR path unsupported formats: input={}, output={}",
			(uint32_t)inputDesc.Format, (uint32_t)outputDesc.Format));
		return false;
	}
	impl->sourceWidth = inputDesc.Width;
	impl->sourceHeight = inputDesc.Height;
	impl->useResolutionScaling = _settings.processingResolutionPercent < 100;
	auto alignWork = [](uint32_t size, uint32_t native) {
		if (size >= native) return native;
		const uint32_t aligned = std::max(16u, (size + 8u) / 16u * 16u);
		return std::min(aligned, native);
	};
	impl->width = impl->useResolutionScaling ? alignWork(std::max(
		1u, static_cast<uint32_t>(std::lround(
			double(inputDesc.Width) *
			double(_settings.processingResolutionPercent) / 100.0))), inputDesc.Width)
		: inputDesc.Width;
	impl->height = impl->useResolutionScaling ? alignWork(std::max(
		1u, static_cast<uint32_t>(std::lround(
			double(inputDesc.Height) *
			double(_settings.processingResolutionPercent) / 100.0))), inputDesc.Height)
		: inputDesc.Height;
	impl->convertInputToRgba = inputDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;

	HRESULT hr = D3D12CreateDevice(
		resources.GetGraphicsAdapter(), D3D_FEATURE_LEVEL_11_0,
		IID_PPV_ARGS(impl->device12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSNR D3D12 device failed", hr);
		if (hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DEVICE_REMOVED ||
			hr == DXGI_ERROR_DEVICE_RESET) {
			Logger::Get().Error(
				"The GPU is still hung from the last scaling session. Quit Magpie completely, then start it again.");
		}
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
		Logger::Get().ComError("Create DLSSNR D3D12 command objects failed", hr);
		return false;
	}

	D3D11_TEXTURE2D_DESC sharedDesc = outputDesc;
	sharedDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sharedDesc.Width = impl->width;
	sharedDesc.Height = impl->height;
	if (!CreateSharedTexture(*impl, sharedDesc, true,
		impl->sharedInput11, impl->sharedInput12) ||
		!CreateSharedTexture(*impl, sharedDesc, true,
			impl->sharedOutput11, impl->sharedOutput12)) {
		return false;
	}
	if (impl->useResolutionScaling) {
		if (!CreateResolutionScalingResources(*impl, input, outputDesc)) {
			return false;
		}
	} else if (!CreateGuidanceResampleResources(*impl)) {
		return false;
	}
	if (!impl->useResolutionScaling && impl->convertInputToRgba) {
		hr = impl->device11->CreateShaderResourceView(
			input, nullptr, impl->inputSrv11.put());
		if (SUCCEEDED(hr)) {
			hr = impl->device11->CreateUnorderedAccessView(
				impl->sharedInput11.get(), nullptr, impl->sharedInputUav11.put());
		}
		winrt::com_ptr<ID3DBlob> shaderBlob;
		if (SUCCEEDED(hr) && !DirectXHelper::CompileComputeShader(
			COLOR_CONVERT_HLSL, "ConvertToRgba", shaderBlob.put(),
			"DLSSNRColorConvert")) {
			hr = E_FAIL;
		}
		if (SUCCEEDED(hr)) {
			hr = impl->device11->CreateComputeShader(
				shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
				impl->colorConvertShader11.put());
		}
		if (FAILED(hr)) {
			Logger::Get().ComError("Create DLSSNR BGRA conversion resources failed", hr);
			return false;
		}
	}

	hr = impl->device11->CreateFence(
		0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(impl->fence11.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSNR shared fence failed", hr);
		return false;
	}
	HANDLE rawFence = nullptr;
	hr = impl->fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawFence);
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSNR fence shared handle failed", hr);
		return false;
	}
	wil::unique_handle fenceHandle(rawFence);
	hr = impl->device12->OpenSharedHandle(
		fenceHandle.get(), IID_PPV_ARGS(impl->fence12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Open DLSSNR shared fence in D3D12 failed", hr);
		return false;
	}
	hr = impl->fenceEvent.create();
	if (FAILED(hr)) {
		Logger::Get().ComError("Create reusable DLSSNR fence event failed", hr);
		return false;
	}

	D3D12_QUERY_HEAP_DESC queryDesc{};
	queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	queryDesc.Count = Impl::COMMAND_SLOT_COUNT * 2;
	hr = impl->device12->CreateQueryHeap(
		&queryDesc, IID_PPV_ARGS(impl->timestampQueryHeap.put()));
	D3D12_HEAP_PROPERTIES readbackHeap{};
	readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
	readbackHeap.CreationNodeMask = 1;
	readbackHeap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC readbackDesc{};
	readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	readbackDesc.Width = uint64_t(queryDesc.Count) * sizeof(uint64_t);
	readbackDesc.Height = 1;
	readbackDesc.DepthOrArraySize = 1;
	readbackDesc.MipLevels = 1;
	readbackDesc.SampleDesc.Count = 1;
	readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (SUCCEEDED(hr)) {
		hr = impl->device12->CreateCommittedResource(
			&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			IID_PPV_ARGS(impl->timestampReadback.put()));
	}
	if (SUCCEEDED(hr)) {
		hr = impl->queue12->GetTimestampFrequency(&impl->timestampFrequency);
	}
	if (FAILED(hr) || !impl->timestampFrequency) {
		Logger::Get().Warn(fmt::format(
			"DLSSNR GPU timestamp telemetry unavailable ({:#x})",
			static_cast<uint32_t>(hr)));
		impl->timestampQueryHeap = nullptr;
		impl->timestampReadback = nullptr;
		impl->timestampFrequency = 0;
	}

	const std::filesystem::path applicationDirectory =
		Win32Helper::GetExePath().parent_path();
	const DLSSNRRuntimeSelection runtimeSelection = SelectDLSSNRRuntime(
		applicationDirectory, resources.GetGraphicsAdapter());
	g_dlssnrRuntimeKind.store(
		static_cast<int32_t>(runtimeSelection.kind), std::memory_order_relaxed);
	g_dlssnrCudaComputeMajor.store(
		runtimeSelection.cuda.major, std::memory_order_relaxed);
	g_dlssnrCudaComputeMinor.store(
		runtimeSelection.cuda.minor, std::memory_order_relaxed);
	LogDlssnrStatus(fmt::format(
		"DLSSNR runtime selection: adapter=\"{}\" device={:#x} cuda={}.{} "
		"mode={} runtime=\"{}\" path=\"{}\"",
		StrHelper::UTF16ToUTF8(runtimeSelection.adapterName),
		runtimeSelection.deviceId,
		runtimeSelection.cuda.major, runtimeSelection.cuda.minor,
		runtimeSelection.forced ? "forced" : "auto",
		RuntimeKindName(runtimeSelection.kind),
		StrHelper::UTF16ToUTF8(runtimeSelection.path.wstring())));

	// Preload the selected module before NGX Core initialization. Windows resolves
	// already-loaded modules by base name, so this prevents Core from binding a
	// different nvngx_dlssnr.dll from the application root.
	impl->snippetModule = LoadLibraryExW(
		runtimeSelection.path.c_str(), nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (impl->snippetModule) {
		DLSSNRNvapi::Install(impl->snippetModule);
	}
	if (!impl->snippetModule) {
		Logger::Get().Win32Error(fmt::format(
			"Preload selected nvngx_dlssnr.dll failed: {}",
			StrHelper::UTF16ToUTF8(runtimeSelection.path.wstring())));
		return false;
	}

	const std::wstring featurePath = runtimeSelection.path.parent_path().wstring();
	const std::wstring applicationFeaturePath = applicationDirectory.wstring();
	std::array<const wchar_t*, 2> featurePaths{
		featurePath.c_str(), applicationFeaturePath.c_str()
	};
	NVSDK_NGX_FeatureCommonInfo featureInfo{};
	featureInfo.PathListInfo.Path = featurePaths.data();
	featureInfo.PathListInfo.Length =
		runtimeSelection.path.parent_path() == applicationDirectory ? 1 : 2;
	DWORD sehCode = 0;
	NVSDK_NGX_Result result = CallCoreInitSafely(
		"7c134ab9-9677-4af5-a2b2-bca943350861",
		"Magpie-DLSSNR-2",
		applicationDirectory.c_str(),
		impl->device12.get(),
		&featureInfo,
		&sehCode);
	if (sehCode) {
		Logger::Get().Error(fmt::format(
			"DLSSNR parameter Core Init raised SEH {:#x}", sehCode));
		return false;
	}
	if (!NGXSucceeded(result)) {
		Logger::Get().Error(fmt::format(
			"DLSSNR parameter Core Init failed ({:#x})", (uint32_t)result));
		return false;
	}
	impl->coreInitialized = true;

	if constexpr (!ENABLE_CORE_FEATURE18_DIAGNOSTIC) {
		if (!InitializeSignedSnippet(
			*impl, applicationDirectory, runtimeSelection.path)) return false;
	}

	sehCode = 0;
	result = CallAllocateParametersSafely(&impl->parameters, &sehCode);
	if (sehCode) {
		Logger::Get().Error(fmt::format(
			"DLSSNR AllocateParameters raised SEH {:#x}", sehCode));
		return false;
	}
	if (!NGXSucceeded(result) || !impl->parameters) {
		Logger::Get().Error(fmt::format(
			"DLSSNR AllocateParameters failed ({:#x})", (uint32_t)result));
		return false;
	}
	sehCode = 0;
	impl->renderPreset = _settings.renderPreset;
	if (!SetCreateParametersSafely(*impl, _settings, &sehCode)) {
		Logger::Get().Error(fmt::format(
			"DLSSNR creation parameter setup raised SEH {:#x}", sehCode));
		return false;
	}

	const Impl::CreateFeatureFn createFeature =
		ENABLE_CORE_FEATURE18_DIAGNOSTIC ?
		static_cast<Impl::CreateFeatureFn>(&NVSDK_NGX_D3D12_CreateFeature) :
		impl->snippetCreateFeature;
	sehCode = 0;
	result = CallCreateFeatureSafely(
		createFeature, impl->commandList12.get(), FEATURE_DLSSNR,
		impl->parameters, &impl->feature, &sehCode);
	if (sehCode) {
		Logger::Get().Error(fmt::format(
			"DLSSNR {} CreateFeature raised SEH {:#x}",
			ENABLE_CORE_FEATURE18_DIAGNOSTIC ? "Core diagnostic" : "signed snippet",
			sehCode));
		return false;
	}
	if (!NGXSucceeded(result) || !impl->feature) {
		Logger::Get().Error(fmt::format(
			"DLSSNR {} Feature 18 creation failed ({:#x})",
			ENABLE_CORE_FEATURE18_DIAGNOSTIC ? "Core diagnostic" : "signed snippet",
			(uint32_t)result));
		return false;
	}
	impl->passFeatures[0] = impl->feature;

	hr = impl->commandList12->Close();
	if (FAILED(hr)) {
		Logger::Get().ComError("Close DLSSNR initialization command list failed", hr);
		return false;
	}
	ID3D12CommandList* lists[]{ impl->commandList12.get() };
	impl->queue12->ExecuteCommandLists(1, lists);
	if (!WaitForQueue(*impl)) {
		return false;
	}
	for (uint32_t i = 0; i < impl->commandSlots.size(); ++i) {
		Impl::CommandSlot& slot = impl->commandSlots[i];
		slot.timestampQuery = i * 2;
		hr = impl->device12->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.allocator.put()));
		if (SUCCEEDED(hr)) {
			hr = impl->device12->CreateCommandList(
				0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.get(), nullptr,
				IID_PPV_ARGS(slot.commandList.put()));
		}
		if (SUCCEEDED(hr)) hr = slot.commandList->Close();
		if (FAILED(hr)) {
			Logger::Get().ComError("Create DLSSNR command ring failed", hr);
			return false;
		}
	}
	impl->guidanceInterop = std::make_unique<FrameGuidanceD3D12Interop>();
	if (!impl->guidanceInterop->Initialize(
		impl->device12.get(), impl->fence12.get())) {
		return false;
	}

	LogDlssnrStatus(fmt::format(
		"DLSSNR STATUS: Feature=18 created=true path={} size={}x{} preset=default(1) "
		"processingResolution={}% internalSize={}x{} style={} intensity={} localTone={} "
		"localStructure={} guidanceMode={} autoMask={} "
		"depthInterval={} passes={} historyMode={} disabled=false",
		ENABLE_CORE_FEATURE18_DIAGNOSTIC ? "core-diagnostic" : "signed-snippet",
		impl->sourceWidth, impl->sourceHeight,
		_settings.processingResolutionPercent, impl->width, impl->height, _settings.style,
		_settings.intensity, _settings.localToneStrength,
		_settings.localStructureStrength, _settings.guidanceMode,
		_settings.useAutoMask,
		_settings.depthInferenceInterval,
		_settings.passes,
		_settings.historyMode));
	_impl = std::move(impl);
	initStatus.completed = true;
	g_dlssnrRuntimeState.store(
		static_cast<int32_t>(DLSSNRRuntimeState::FeatureCreated), std::memory_order_relaxed);
	return true;
}

bool DLSSNRFilter::Resize(
	DeviceResources& resources,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output
) noexcept {
	return Initialize(resources, input, output, _settings);
}

static FrameGuidanceView SelectGuidance(
	const NativeEffectDrawContext& context,
	const DLSSNRSettings& settings,
	FrameGuidanceExtent extent
) noexcept {
	FrameGuidanceView selected = context.frameGuidance;
	const FrameGuidanceView& zero = context.zeroFrameGuidance;
	if (!selected.IsValidFor(context.frameId, extent)) selected = zero;
	switch (settings.guidanceMode) {
	case 1:
		selected = zero;
		break;
	case 2:
		selected.depth = zero.depth;
		selected.rawDepth = {};
		selected.depthResidual = {};
		break;
	case 3:
		selected.motion = zero.motion;
		selected.confidence = zero.confidence;
		break;
	default:
		break;
	}
	selected.requiresHistoryReset =
		selected.depth.metadata.requiresHistoryReset ||
		selected.motion.metadata.requiresHistoryReset ||
		selected.confidence.metadata.requiresHistoryReset;
	return selected.IsValidFor(context.frameId, extent) ? selected : zero;
}

DLSSNRSettings DLSSNRFilter::ResolvePassSettings(uint32_t pass) const noexcept {
	DLSSNRSettings resolved = _settings;
	if (pass == 0 || pass > 3) return resolved;
	const DLSSNRSettings::PassTune& tune = _settings.laterPass[pass - 1];
	if (tune.style >= 0) resolved.style = tune.style;
	if (tune.intensity >= 0.0f) resolved.intensity = tune.intensity;
	resolved.localToneStrength = tune.localToneStrength >= 0.0f ?
		tune.localToneStrength : _settings.localToneStrength;
	if (tune.localStructureStrength >= 0.0f) {
		resolved.localStructureStrength = tune.localStructureStrength;
	}
	if (tune.skinStructureStrength >= 0.0f) {
		resolved.skinStructureStrength = tune.skinStructureStrength;
	}
	if (tune.renderPreset >= 0) resolved.renderPreset = tune.renderPreset;
	return resolved;
}

static bool EnsurePassFeedbackResources(
	DLSSNRFilter::Impl& impl,
	ID3D11Texture2D* output
) noexcept {
	if (!impl.passFeedbackShader11) {
		if (!CreateComputeShader(
			impl, PASS_FEEDBACK_HLSL, "BlendPassFeedback",
			"DLSSNRPassFeedback", impl.passFeedbackShader11)) {
			return false;
		}
	}
	if (!impl.passFeedbackCb11) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = 16;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		const HRESULT hr = impl.device11->CreateBuffer(
			&desc, nullptr, impl.passFeedbackCb11.put());
		if (FAILED(hr)) return false;
	}
	D3D11_TEXTURE2D_DESC outputDesc{};
	output->GetDesc(&outputDesc);
	auto matches = [&](ID3D11Texture2D* texture) {
		if (!texture) return false;
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		return desc.Width == outputDesc.Width && desc.Height == outputDesc.Height &&
			desc.Format == outputDesc.Format;
	};
	if (!matches(impl.passFeedbackScratch11.get()) || !matches(impl.passProxy11.get())) {
		impl.passFeedbackScratch11 = nullptr;
		impl.passFeedbackScratchUav11 = nullptr;
		impl.passProxy11 = nullptr;
		D3D11_TEXTURE2D_DESC desc = outputDesc;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		HRESULT hr = impl.device11->CreateTexture2D(
			&desc, nullptr, impl.passFeedbackScratch11.put());
		if (SUCCEEDED(hr)) {
			hr = impl.device11->CreateUnorderedAccessView(
				impl.passFeedbackScratch11.get(), nullptr,
				impl.passFeedbackScratchUav11.put());
		}
		if (SUCCEEDED(hr)) {
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			hr = impl.device11->CreateTexture2D(
				&desc, nullptr, impl.passProxy11.put());
		}
		if (FAILED(hr)) return false;
	}
	return impl.passFeedbackScratchUav11 && impl.passProxy11;
}

static bool ApplyPassFeedback(
	DLSSNRFilter::Impl& impl,
	ID3D11Texture2D* proxy,
	ID3D11Texture2D* answer,
	float feedback
) noexcept {
	if (!proxy || !answer || proxy == answer) return false;
	if (!EnsurePassFeedbackResources(impl, answer)) return false;
	winrt::com_ptr<ID3D11ShaderResourceView> proxySrv;
	winrt::com_ptr<ID3D11ShaderResourceView> answerSrv;
	HRESULT hr = impl.device11->CreateShaderResourceView(proxy, nullptr, proxySrv.put());
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateShaderResourceView(answer, nullptr, answerSrv.put());
	}
	if (FAILED(hr)) return false;
	const float constants[4]{ feedback, 0, 0, 0 };
	impl.context11->UpdateSubresource(
		impl.passFeedbackCb11.get(), 0, nullptr, constants, 0, 0);
	ID3D11ShaderResourceView* srvs[]{ proxySrv.get(), answerSrv.get() };
	ID3D11UnorderedAccessView* uav = impl.passFeedbackScratchUav11.get();
	ID3D11Buffer* buffer = impl.passFeedbackCb11.get();
	impl.context11->CSSetShader(impl.passFeedbackShader11.get(), nullptr, 0);
	impl.context11->CSSetShaderResources(0, 2, srvs);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	impl.context11->CSSetConstantBuffers(0, 1, &buffer);
	D3D11_TEXTURE2D_DESC desc{};
	answer->GetDesc(&desc);
	impl.context11->Dispatch((desc.Width + 7) / 8, (desc.Height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrvs[]{ nullptr, nullptr };
	ID3D11UnorderedAccessView* nullUav = nullptr;
	ID3D11Buffer* nullBuffer = nullptr;
	impl.context11->CSSetShaderResources(0, 2, nullSrvs);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	impl.context11->CSSetConstantBuffers(0, 1, &nullBuffer);
	impl.context11->CSSetShader(nullptr, nullptr, 0);
	impl.context11->CopyResource(answer, impl.passFeedbackScratch11.get());
	return true;
}

bool DLSSNRFilter::_EnsurePassFeature(uint32_t pass) noexcept {
	if (!_impl || pass >= Impl::kMaxPasses) return false;
	Impl& impl = *_impl;
	if (impl.passFeatures[pass]) return true;
	if (pass == 0 || impl.extraPassRejected || !impl.parameters) return false;
	if (!WaitForQueue(impl)) return false;

	HRESULT hr = impl.allocator12->Reset();
	if (SUCCEEDED(hr)) {
		hr = impl.commandList12->Reset(impl.allocator12.get(), nullptr);
	}
	if (FAILED(hr)) {
		Logger::Get().ComError("DLSSNR extra pass command list reset failed", hr);
		impl.extraPassRejected = true;
		return false;
	}

	DWORD sehCode = 0;
	if (!SetCreateParametersSafely(impl, ResolvePassSettings(pass), &sehCode)) {
		impl.commandList12->Close();
		Logger::Get().Warn(fmt::format(
			"DLSSNR pass {} create parameters raised SEH {:#x}; keeping earlier passes",
			pass + 1, sehCode));
		impl.extraPassRejected = true;
		return false;
	}

	const Impl::CreateFeatureFn createFeature = ENABLE_CORE_FEATURE18_DIAGNOSTIC ?
		static_cast<Impl::CreateFeatureFn>(&NVSDK_NGX_D3D12_CreateFeature) :
		impl.snippetCreateFeature;
	sehCode = 0;
	const NVSDK_NGX_Result result = CallCreateFeatureSafely(
		createFeature, impl.commandList12.get(), FEATURE_DLSSNR,
		impl.parameters, &impl.passFeatures[pass], &sehCode);
	hr = impl.commandList12->Close();
	if (SUCCEEDED(hr)) {
		ID3D12CommandList* lists[]{ impl.commandList12.get() };
		impl.queue12->ExecuteCommandLists(1, lists);
		WaitForQueue(impl);
	}
	if (sehCode || FAILED(hr) || !NGXSucceeded(result) || !impl.passFeatures[pass]) {
		impl.passFeatures[pass] = nullptr;
		impl.extraPassRejected = true;
		Logger::Get().Warn(fmt::format(
			"DLSSNR pass {} feature was not created ({:#x}, seh={:#x}); keeping earlier passes",
			pass + 1, (uint32_t)result, sehCode));
		return false;
	}
	Logger::Get().Info(fmt::format("DLSSNR pass {} has its own feature", pass + 1));
	return true;
}

bool DLSSNRFilter::_DrawOnce(
	const NativeEffectDrawContext& context,
	NVSDK_NGX_Handle* feature,
	bool forceHistoryReset
) noexcept {
	if (!_impl || !feature || !_impl->parameters) {
		return false;
	}
	Impl& impl = *_impl;
	ID3D11Texture2D* input = context.input;
	ID3D11Texture2D* output = context.output;
	if (DlssnrDeviceIsRemoved(impl)) {
		if (input != output) {
			impl.context11->CopyResource(output, input);
		}
		return true;
	}
	auto fail = [&](std::string_view stage) noexcept {
		impl.disabled = true;
		g_dlssnrEvaluateFailureCount.fetch_add(1, std::memory_order_relaxed);
		g_dlssnrRuntimeState.store(
			static_cast<int32_t>(DLSSNRRuntimeState::Failed), std::memory_order_relaxed);
		LogDlssnrStatus(fmt::format(
			"DLSSNR STATUS: Feature=18 frameId={} stage={} result=internal-failure "
			"disabled=true fallback=pass-through-next-frame",
			context.frameId, stage), true);
		return false;
	};
	Impl::CommandSlot& commandSlot =
		impl.commandSlots[impl.nextCommandSlot++ % Impl::COMMAND_SLOT_COUNT];
	const auto slotWaitStart = std::chrono::steady_clock::now();
	if (commandSlot.completionValue &&
		!WaitForFence(impl, commandSlot.completionValue)) {
		if (DlssnrDeviceIsRemoved(impl)) {
			if (input != output) {
				impl.context11->CopyResource(output, input);
			}
			return true;
		}
		return fail("command-slot-wait");
	}
	const double slotWaitMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - slotWaitStart).count();
	CollectGpuTiming(impl, commandSlot);
	const auto inputPrepareStart = std::chrono::steady_clock::now();
	if (!PrepareInput(impl, input)) return fail("prepare-input");
	const double inputPrepareMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - inputPrepareStart).count();
	if (impl.disabled) {
		if (impl.useResolutionScaling) {
			return CompositeResidual(
				impl, output, impl.sharedInputSrv11.get());
		}
		impl.context11->CopyResource(output, impl.sharedInput11.get());
		return true;
	}
	const auto guidancePrepareStart = std::chrono::steady_clock::now();
	// A guidance miss used to set disabled for the rest of the session, so the
	// filter passed the color through and looked like it was doing nothing.
	auto passColor = [&](std::string_view stage) noexcept {
		static uint32_t logged = 0;
		if (logged < 3) {
			++logged;
			Logger::Get().Warn(fmt::format(
				"DLSSNR guidance skipped ({}); the color filter keeps running",
				stage));
		}
		if (!impl.useResolutionScaling) {
			impl.context11->CopyResource(output, impl.sharedInput11.get());
			return true;
		}
		return false;
	};
	const auto guidanceExtentOf = [](const FrameGuidanceView& view) noexcept {
		return view.motion.metadata.sourceExtent.width ?
			view.motion.metadata.sourceExtent :
			view.depth.metadata.sourceExtent;
	};
	const FrameGuidanceView& produced = context.frameGuidance;
	const FrameGuidanceExtent producedExtent = guidanceExtentOf(produced);
	const FrameGuidanceExtent selectExtent =
		produced.IsValidFor(context.frameId, producedExtent) ? producedExtent :
		guidanceExtentOf(context.zeroFrameGuidance);
	const FrameGuidanceView guidance = SelectGuidance(
		context, _settings, selectExtent);
	const FrameGuidanceExtent guidanceExtent = guidanceExtentOf(guidance);
	if (!guidanceExtent.IsValid() ||
		!impl.guidanceInterop->WaitForProducer(impl.context11, guidance)) {
		return passColor("producer-wait");
	}
	FrameGuidanceView reducedGuidance;
	const FrameGuidanceView* evaluateGuidance = &guidance;
	const FrameGuidanceExtent colorExtent{ impl.width, impl.height };
	const bool resampleGuidance = impl.useResolutionScaling ||
		guidanceExtent.width != colorExtent.width ||
		guidanceExtent.height != colorExtent.height;
	if (resampleGuidance) {
		if (!impl.guidanceDownsampleShader11 &&
			!CreateGuidanceResampleResources(impl)) {
			return passColor("guidance-resample");
		}
		if (!PrepareReducedGuidance(impl, guidance, guidanceExtent)) {
			return passColor("guidance-resample");
		}
		reducedGuidance = MakeReducedGuidance(impl, guidance, guidanceExtent);
		evaluateGuidance = &reducedGuidance;
		static bool loggedResample = false;
		if (!loggedResample) {
			loggedResample = true;
			Logger::Get().Info(fmt::format(
				"DLSSNR resampling frame guidance from {}x{} to {}x{}",
				guidanceExtent.width, guidanceExtent.height,
				colorExtent.width, colorExtent.height));
		}
	}
	if (!UpdateGuidanceResources(impl, *evaluateGuidance, context.frameId)) {
		return passColor("guidance-interop");
	}
	const double guidancePrepareMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - guidancePrepareStart).count();
	const uint64_t inputReady = ++impl.fenceValue;
	HRESULT hr = impl.context11->Signal(impl.fence11.get(), inputReady);
	if (FAILED(hr)) {
		return fail("d3d11-input-signal");
	}
	impl.context11->Flush();
	hr = impl.queue12->Wait(impl.fence12.get(), inputReady);
	if (FAILED(hr)) {
		return fail("d3d12-input-wait");
	}
	hr = commandSlot.allocator->Reset();
	if (SUCCEEDED(hr)) {
		hr = commandSlot.commandList->Reset(commandSlot.allocator.get(), nullptr);
	}
	if (FAILED(hr)) {
		return fail("command-list-reset");
	}
	ID3D12GraphicsCommandList* commandList = commandSlot.commandList.get();

	D3D12_RESOURCE_BARRIER barriers[2]{};
	barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[0].Transition = {
		impl.sharedInput12.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
	};
	barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[1].Transition = {
		impl.sharedOutput12.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS
	};
	commandList->ResourceBarrier(ARRAYSIZE(barriers), barriers);
	impl.guidanceInterop->Transition(
		commandList, D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	const bool guidanceReset = forceHistoryReset ||
		(evaluateGuidance->requiresHistoryReset &&
			impl.lastGuidanceResetFrameId != context.frameId);
	DWORD sehCode = 0;
	if (!SetEvaluateParametersSafely(
		impl, _activeSettings ? *_activeSettings : _settings,
		*evaluateGuidance, guidanceReset, &sehCode)) {
		commandList->Close();
		Logger::Get().Error(fmt::format(
			"DLSSNR evaluation parameter setup raised SEH {:#x}", sehCode));
		return fail("ngx-parameters-seh");
	}
	if (impl.timestampQueryHeap) {
		commandList->EndQuery(
			impl.timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP,
			commandSlot.timestampQuery);
	}
	const auto evaluateStart = std::chrono::steady_clock::now();
	const Impl::EvaluateFeatureFn evaluateFeature = impl.useSignedSnippet ?
		impl.snippetEvaluateFeature :
		static_cast<Impl::EvaluateFeatureFn>(&NVSDK_NGX_D3D12_EvaluateFeature);
	DLSSNRNvapi::SetReuseInterval(_settings.vitReuseInterval);
	DLSSNRNvapi::BeginEvaluate(feature, forceHistoryReset, static_cast<long long>(context.frameId), commandList);
	const NVSDK_NGX_Result result = CallEvaluateFeatureSafely(
		evaluateFeature, commandList, feature,
		impl.parameters, &sehCode);
	DLSSNRNvapi::EndEvaluate(commandList);
	const double evaluateCpuMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - evaluateStart).count();
	if (impl.timestampQueryHeap) {
		commandList->EndQuery(
			impl.timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP,
			commandSlot.timestampQuery + 1);
		commandList->ResolveQueryData(
			impl.timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP,
			commandSlot.timestampQuery, 2, impl.timestampReadback.get(),
			uint64_t(commandSlot.timestampQuery) * sizeof(uint64_t));
	}
	++impl.evaluateCount;
	const bool evaluateSucceeded = !sehCode && NGXSucceeded(result);
	if (evaluateSucceeded) {
		++impl.evaluateSuccessCount;
		g_dlssnrEvaluateSuccessCount.fetch_add(1, std::memory_order_relaxed);
		g_dlssnrRuntimeState.store(
			static_cast<int32_t>(DLSSNRRuntimeState::Evaluating), std::memory_order_relaxed);
	} else {
		++impl.evaluateFailureCount;
		g_dlssnrEvaluateFailureCount.fetch_add(1, std::memory_order_relaxed);
		g_dlssnrRuntimeState.store(
			static_cast<int32_t>(DLSSNRRuntimeState::Failed), std::memory_order_relaxed);
		impl.disabled = true;
		if (sehCode) {
			Logger::Get().Error(fmt::format(
				"DLSSNR EvaluateFeature raised SEH {:#x}", sehCode));
		}
	}
	if (!evaluateSucceeded || impl.evaluateCount <= 8 ||
		impl.evaluateCount % 120 == 0) {
		LogDlssnrStatus(fmt::format(
			"DLSSNR STATUS: Feature=18 frameId={} evaluateCount={} result={:#x} "
			"success={} failures={} guidanceMode={} path={} disabled={}",
			context.frameId, impl.evaluateCount, static_cast<uint32_t>(result),
			impl.evaluateSuccessCount, impl.evaluateFailureCount,
			_settings.guidanceMode,
			impl.useSignedSnippet ? "signed-snippet" : "core-diagnostic",
			impl.disabled), !evaluateSucceeded);
	}
	const auto submitStart = std::chrono::steady_clock::now();
	for (D3D12_RESOURCE_BARRIER& barrier : barriers) {
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
	}
	impl.guidanceInterop->Transition(
		commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_COMMON);
	commandList->ResourceBarrier(ARRAYSIZE(barriers), barriers);
	hr = commandList->Close();
	if (FAILED(hr)) {
		return fail("command-list-close");
	}
	ID3D12CommandList* lists[]{ commandList };
	impl.queue12->ExecuteCommandLists(1, lists);
	commandSlot.timestampPending = impl.timestampQueryHeap != nullptr;
	commandSlot.timestampFrameId = context.frameId;
	const uint64_t outputReady = ++impl.fenceValue;
	hr = impl.queue12->Signal(impl.fence12.get(), outputReady);
	commandSlot.completionValue = outputReady;
	impl.guidanceInterop->MarkSubmitted(outputReady);
	if (SUCCEEDED(hr)) {
		hr = impl.context11->Wait(impl.fence11.get(), outputReady);
	}
	if (FAILED(hr)) {
		return fail("output-signal-wait");
	}
	if (impl.useResolutionScaling) {
		if (!CompositeResidual(
			impl, output, impl.disabled ? impl.sharedInputSrv11.get() :
				impl.sharedOutputSrv11.get())) {
			return fail("residual-composite");
		}
	} else {
		impl.context11->CopyResource(
			output, impl.disabled ? impl.sharedInput11.get() : impl.sharedOutput11.get());
		static bool loggedCopy = false;
		if (!loggedCopy) {
			loggedCopy = true;
			Logger::Get().Info("DLSSNR copies model output the same way as the original fork");
		}
	}
	const double submitMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - submitStart).count();
	impl.slotWaitTimings.Add(slotWaitMs);
	impl.inputPrepareTimings.Add(inputPrepareMs);
	impl.guidancePrepareTimings.Add(guidancePrepareMs);
	impl.evaluateCpuTimings.Add(evaluateCpuMs);
	impl.submitTimings.Add(submitMs);
	if (guidanceReset) {
		impl.lastGuidanceResetFrameId = context.frameId;
	}
	impl.resetHistory = false;
	if (impl.evaluateCount <= 8 || impl.evaluateCount % 120 == 0) {
		if (impl.evaluateCount <= 8) {
			Logger::Get().Info(fmt::format(
				"DLSSNR timing: frameId={} evaluateCount={} slotWait={:.3f} ms "
				"inputPrepare={:.3f} ms guidancePrepare={:.3f} ms "
				"evaluateCPU={:.3f} ms submit={:.3f} ms",
				context.frameId, impl.evaluateCount, slotWaitMs, inputPrepareMs,
				guidancePrepareMs, evaluateCpuMs, submitMs));
		} else {
			Logger::Get().Info(fmt::format(
				"DLSSNR timing 120-frame window: frameId={} evaluateCount={} {} {} {} {} {} {}",
				context.frameId, impl.evaluateCount,
				FormatTimingSummary("slotWait", impl.slotWaitTimings.Summarize()),
				FormatTimingSummary("inputPrepare", impl.inputPrepareTimings.Summarize()),
				FormatTimingSummary("guidancePrepare", impl.guidancePrepareTimings.Summarize()),
				FormatTimingSummary("evaluateCPU", impl.evaluateCpuTimings.Summarize()),
				FormatTimingSummary("submit", impl.submitTimings.Summarize()),
				FormatTimingSummary("evaluateGPU", impl.evaluateGpuTimings.Summarize())));
		}
	}
	return true;
}

static bool CreateComputeShader(
	DLSSNRFilter::Impl& impl,
	std::string_view source,
	const char* entryPoint,
	const char* sourceName,
	winrt::com_ptr<ID3D11ComputeShader>& shader
) noexcept {
	winrt::com_ptr<ID3DBlob> shaderBlob;
	if (!DirectXHelper::CompileComputeShader(
		source, entryPoint, shaderBlob.put(), sourceName)) {
		return false;
	}
	const HRESULT hr = impl.device11->CreateComputeShader(
		shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
		shader.put());
	if (FAILED(hr)) {
		Logger::Get().ComError(fmt::format(
			"Create {} compute shader failed", sourceName), hr);
		return false;
	}
	return true;
}

static bool UpdateInputShaderResource(
	DLSSNRFilter::Impl& impl,
	ID3D11Texture2D* input
) noexcept {
	if (impl.inputTexture11 == input && impl.inputSrv11) return true;
	winrt::com_ptr<ID3D11ShaderResourceView> srv;
	const HRESULT hr = impl.device11->CreateShaderResourceView(
		input, nullptr, srv.put());
	if (FAILED(hr)) {
		Logger::Get().ComError(
			"Create DLSSNR processing-resolution input SRV failed", hr);
		return false;
	}
	impl.inputSrv11 = std::move(srv);
	impl.inputTexture11 = input;
	return true;
}

static bool CreateGuidanceResampleResources(DLSSNRFilter::Impl& impl) noexcept {
	if (impl.guidanceDownsampleShader11 && impl.reducedMotion11 &&
		impl.resampleConstants11) {
		return true;
	}
	constexpr UINT GUIDANCE_BIND_FLAGS =
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	constexpr UINT GUIDANCE_MISC_FLAGS =
		D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	if (!impl.reducedMotion11) {
		impl.reducedMotion11 = DirectXHelper::CreateTexture2D(
			impl.device11, DXGI_FORMAT_R16G16_FLOAT, impl.width, impl.height,
			GUIDANCE_BIND_FLAGS, D3D11_USAGE_DEFAULT, GUIDANCE_MISC_FLAGS);
		impl.reducedDepth11 = DirectXHelper::CreateTexture2D(
			impl.device11, DXGI_FORMAT_R32_FLOAT, impl.width, impl.height,
			GUIDANCE_BIND_FLAGS, D3D11_USAGE_DEFAULT, GUIDANCE_MISC_FLAGS);
		impl.reducedConfidence11 = DirectXHelper::CreateTexture2D(
			impl.device11, DXGI_FORMAT_R8_UNORM, impl.width, impl.height,
			GUIDANCE_BIND_FLAGS, D3D11_USAGE_DEFAULT, GUIDANCE_MISC_FLAGS);
	}
	if (!impl.reducedMotion11 || !impl.reducedDepth11 || !impl.reducedConfidence11) {
		Logger::Get().Error("Create DLSSNR guidance resample textures failed");
		return false;
	}
	HRESULT hr = S_OK;
	if (!impl.reducedMotionUav11) {
		hr = impl.device11->CreateUnorderedAccessView(
			impl.reducedMotion11.get(), nullptr, impl.reducedMotionUav11.put());
		if (SUCCEEDED(hr)) {
			hr = impl.device11->CreateUnorderedAccessView(
				impl.reducedDepth11.get(), nullptr, impl.reducedDepthUav11.put());
		}
		if (SUCCEEDED(hr)) {
			hr = impl.device11->CreateUnorderedAccessView(
				impl.reducedConfidence11.get(), nullptr,
				impl.reducedConfidenceUav11.put());
		}
	}
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSNR guidance resample UAVs failed", hr);
		return false;
	}
	if (!impl.resampleConstants11) {
		D3D11_BUFFER_DESC constantsDesc{};
		constantsDesc.ByteWidth = sizeof(ResampleConstants);
		constantsDesc.Usage = D3D11_USAGE_DEFAULT;
		constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		hr = impl.device11->CreateBuffer(
			&constantsDesc, nullptr, impl.resampleConstants11.put());
		if (FAILED(hr)) {
			Logger::Get().ComError(
				"Create DLSSNR guidance resample constants failed", hr);
			return false;
		}
	}
	return impl.guidanceDownsampleShader11 || CreateComputeShader(
		impl, GUIDANCE_DOWNSAMPLE_HLSL, "DownsampleGuidance",
		"DLSSNRGuidanceDownsample", impl.guidanceDownsampleShader11);
}

static bool CreateResolutionScalingResources(
	DLSSNRFilter::Impl& impl,
	ID3D11Texture2D* input,
	const D3D11_TEXTURE2D_DESC& outputDesc
) noexcept {
	if (!UpdateInputShaderResource(impl, input)) return false;
	HRESULT hr = impl.device11->CreateShaderResourceView(
		impl.sharedInput11.get(), nullptr, impl.sharedInputSrv11.put());
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateShaderResourceView(
			impl.sharedOutput11.get(), nullptr, impl.sharedOutputSrv11.put());
	}
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateUnorderedAccessView(
			impl.sharedInput11.get(), nullptr, impl.sharedInputUav11.put());
	}
	if (FAILED(hr)) {
		Logger::Get().ComError(
			"Create DLSSNR processing-resolution color views failed", hr);
		return false;
	}

	D3D11_TEXTURE2D_DESC compositeDesc = outputDesc;
	compositeDesc.Usage = D3D11_USAGE_DEFAULT;
	compositeDesc.CPUAccessFlags = 0;
	compositeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
		D3D11_BIND_UNORDERED_ACCESS;
	compositeDesc.MiscFlags = 0;
	hr = impl.device11->CreateTexture2D(
		&compositeDesc, nullptr, impl.compositeOutput11.put());
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateUnorderedAccessView(
			impl.compositeOutput11.get(), nullptr,
			impl.compositeOutputUav11.put());
	}
	if (FAILED(hr)) {
		Logger::Get().ComError(
			"Create DLSSNR processing-resolution composite failed", hr);
		return false;
	}

	impl.resampleIntermediate11 = DirectXHelper::CreateTexture2D(
		impl.device11, DXGI_FORMAT_R16G16B16A16_FLOAT,
		impl.sourceWidth, impl.height,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	impl.residual11 = DirectXHelper::CreateTexture2D(
		impl.device11, DXGI_FORMAT_R16G16B16A16_FLOAT,
		impl.width, impl.height,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	if (!impl.resampleIntermediate11 || !impl.residual11) {
		Logger::Get().Error(
			"Create DLSSNR processing-resolution residual textures failed");
		return false;
	}
	hr = impl.device11->CreateShaderResourceView(
		impl.resampleIntermediate11.get(), nullptr,
		impl.resampleIntermediateSrv11.put());
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateUnorderedAccessView(
			impl.resampleIntermediate11.get(), nullptr,
			impl.resampleIntermediateUav11.put());
	}
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateShaderResourceView(
			impl.residual11.get(), nullptr, impl.residualSrv11.put());
	}
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateUnorderedAccessView(
			impl.residual11.get(), nullptr, impl.residualUav11.put());
	}
	if (FAILED(hr)) {
		Logger::Get().ComError(
			"Create DLSSNR processing-resolution residual views failed", hr);
		return false;
	}

	constexpr UINT GUIDANCE_BIND_FLAGS =
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	constexpr UINT GUIDANCE_MISC_FLAGS =
		D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	impl.reducedMotion11 = DirectXHelper::CreateTexture2D(
		impl.device11, DXGI_FORMAT_R16G16_FLOAT, impl.width, impl.height,
		GUIDANCE_BIND_FLAGS, D3D11_USAGE_DEFAULT, GUIDANCE_MISC_FLAGS);
	impl.reducedDepth11 = DirectXHelper::CreateTexture2D(
		impl.device11, DXGI_FORMAT_R32_FLOAT, impl.width, impl.height,
		GUIDANCE_BIND_FLAGS, D3D11_USAGE_DEFAULT, GUIDANCE_MISC_FLAGS);
	impl.reducedConfidence11 = DirectXHelper::CreateTexture2D(
		impl.device11, DXGI_FORMAT_R8_UNORM, impl.width, impl.height,
		GUIDANCE_BIND_FLAGS, D3D11_USAGE_DEFAULT, GUIDANCE_MISC_FLAGS);
	if (!impl.reducedMotion11 || !impl.reducedDepth11 ||
		!impl.reducedConfidence11) {
		Logger::Get().Error(
			"Create DLSSNR reduced Frame Guidance textures failed");
		return false;
	}
	hr = impl.device11->CreateUnorderedAccessView(
		impl.reducedMotion11.get(), nullptr, impl.reducedMotionUav11.put());
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateUnorderedAccessView(
			impl.reducedDepth11.get(), nullptr, impl.reducedDepthUav11.put());
	}
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateUnorderedAccessView(
			impl.reducedConfidence11.get(), nullptr,
			impl.reducedConfidenceUav11.put());
	}
	if (FAILED(hr)) {
		Logger::Get().ComError(
			"Create DLSSNR reduced Frame Guidance UAVs failed", hr);
		return false;
	}

	D3D11_BUFFER_DESC constantsDesc{};
	constantsDesc.ByteWidth = sizeof(ResampleConstants);
	constantsDesc.Usage = D3D11_USAGE_DEFAULT;
	constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	hr = impl.device11->CreateBuffer(
		&constantsDesc, nullptr, impl.resampleConstants11.put());
	if (FAILED(hr)) {
		Logger::Get().ComError(
			"Create DLSSNR processing-resolution constants failed", hr);
		return false;
	}

	return CreateComputeShader(
			impl, COLOR_DOWNSAMPLE_HLSL, "DownsampleColorVertical",
			"DLSSNRColorDownsampleVertical", impl.colorDownsampleVerticalShader11) &&
		CreateComputeShader(
			impl, COLOR_DOWNSAMPLE_HLSL, "DownsampleColorHorizontal",
			"DLSSNRColorDownsampleHorizontal", impl.colorDownsampleHorizontalShader11) &&
		CreateComputeShader(
			impl, GUIDANCE_DOWNSAMPLE_HLSL, "DownsampleGuidance",
			"DLSSNRGuidanceDownsample", impl.guidanceDownsampleShader11) &&
		CreateComputeShader(
			impl, RESIDUAL_PREPARE_HLSL, "PrepareResidual",
			"DLSSNRResidualPrepare", impl.residualPrepareShader11) &&
		CreateComputeShader(
			impl, RESIDUAL_HORIZONTAL_HLSL, "UpsampleResidualHorizontal",
			"DLSSNRResidualHorizontal", impl.residualHorizontalShader11) &&
		CreateComputeShader(
			impl, RESIDUAL_VERTICAL_COMPOSITE_HLSL,
			"CompositeResidualVertical", "DLSSNRResidualVerticalComposite",
			impl.residualVerticalCompositeShader11);
}


static void UnbindDetailReuse(DLSSNRFilter::Impl& impl) noexcept {
	ID3D11ShaderResourceView* nullSrvs[]{ nullptr, nullptr, nullptr, nullptr, nullptr };
	ID3D11UnorderedAccessView* nullUavs[]{ nullptr, nullptr };
	impl.context11->CSSetShaderResources(0, 5, nullSrvs);
	impl.context11->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
	impl.context11->CSSetShader(nullptr, nullptr, 0);
}

static bool EnsureDetailSrv(
	ID3D11Device* device,
	ID3D11Texture2D* texture,
	ID3D11Texture2D*& cached,
	winrt::com_ptr<ID3D11ShaderResourceView>& srv
) noexcept {
	if (!texture) return false;
	if (cached == texture && srv) return true;
	winrt::com_ptr<ID3D11ShaderResourceView> created;
	const HRESULT hr = device->CreateShaderResourceView(
		texture, nullptr, created.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSNR detail reuse SRV failed", hr);
		return false;
	}
	srv = std::move(created);
	cached = texture;
	return true;
}

static bool EnsureDetailUav(
	ID3D11Device* device,
	ID3D11Texture2D* texture,
	ID3D11Texture2D*& cached,
	winrt::com_ptr<ID3D11UnorderedAccessView>& uav
) noexcept {
	if (!texture) return false;
	if (cached == texture && uav) return true;
	winrt::com_ptr<ID3D11UnorderedAccessView> created;
	const HRESULT hr = device->CreateUnorderedAccessView(
		texture, nullptr, created.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSNR detail reuse UAV failed", hr);
		return false;
	}
	uav = std::move(created);
	cached = texture;
	return true;
}

struct DetailReuseCB {
	uint32_t mode;
	uint32_t workWidth;
	uint32_t workHeight;
	float clipGamma;
	float clipFalloff;
	float sigmaFloor;
	float motionReject;
	float fillStrength;
	float fillRadius;
	float pad0;
	float pad1;
	float pad2;
};
static_assert(sizeof(DetailReuseCB) == 48);

static bool EnsureFloatTarget(
	DLSSNRFilter::Impl& impl,
	uint32_t width,
	uint32_t height,
	winrt::com_ptr<ID3D11Texture2D>& texture,
	winrt::com_ptr<ID3D11ShaderResourceView>& srv,
	winrt::com_ptr<ID3D11UnorderedAccessView>& uav
) noexcept {
	D3D11_TEXTURE2D_DESC have{};
	if (texture) texture->GetDesc(&have);
	if (texture && srv && uav && have.Width == width && have.Height == height &&
		have.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
		return true;
	}
	texture = DirectXHelper::CreateTexture2D(
		impl.device11, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	srv = nullptr;
	uav = nullptr;
	if (!texture) return false;
	HRESULT hr = impl.device11->CreateShaderResourceView(texture.get(), nullptr, srv.put());
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateUnorderedAccessView(texture.get(), nullptr, uav.put());
	}
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSNR detail reuse float target failed", hr);
		texture = nullptr;
		return false;
	}
	return true;
}

static bool EnsureDetailReusePipeline(DLSSNRFilter::Impl& impl) noexcept {
	if (!impl.detailReprojectShader11 && !CreateComputeShader(
		impl, DETAIL_REPROJECT_HLSL, "CSMain",
		"DLSSNRDetailReproject", impl.detailReprojectShader11)) {
		impl.detailReuseFailed = true;
		Logger::Get().Error(
			"DLSSNR detail reuse shader failed. Later frames run the denoiser.");
		return false;
	}
	if (!impl.detailSampler11) {
		D3D11_SAMPLER_DESC desc{};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		const HRESULT hr = impl.device11->CreateSamplerState(&desc, impl.detailSampler11.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("Create DLSSNR detail reuse sampler failed", hr);
			return false;
		}
	}
	if (!impl.detailCb11) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = sizeof(DetailReuseCB);
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		const HRESULT hr = impl.device11->CreateBuffer(&desc, nullptr, impl.detailCb11.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("Create DLSSNR detail reuse constants failed", hr);
			return false;
		}
	}
	return true;
}

static bool DispatchDetailReuse(
	DLSSNRFilter::Impl& impl,
	const DetailReuseCB& constants,
	std::initializer_list<ID3D11ShaderResourceView*> srvs,
	std::initializer_list<ID3D11UnorderedAccessView*> uavs
) noexcept {
	impl.context11->UpdateSubresource(impl.detailCb11.get(), 0, nullptr, &constants, 0, 0);
	ID3D11ShaderResourceView* srvSlots[4]{};
	ID3D11UnorderedAccessView* uavSlots[2]{};
	uint32_t i = 0;
	for (ID3D11ShaderResourceView* srv : srvs) {
		if (i < 4) srvSlots[i++] = srv;
	}
	i = 0;
	for (ID3D11UnorderedAccessView* uav : uavs) {
		if (i < 2) uavSlots[i++] = uav;
	}
	ID3D11Buffer* cb = impl.detailCb11.get();
	ID3D11SamplerState* sampler = impl.detailSampler11.get();
	impl.context11->CSSetShader(impl.detailReprojectShader11.get(), nullptr, 0);
	impl.context11->CSSetConstantBuffers(0, 1, &cb);
	impl.context11->CSSetSamplers(0, 1, &sampler);
	impl.context11->CSSetShaderResources(0, 4, srvSlots);
	impl.context11->CSSetUnorderedAccessViews(0, 2, uavSlots, nullptr);
	impl.context11->Dispatch(
		(constants.workWidth + 7) / 8, (constants.workHeight + 7) / 8, 1);
	UnbindDetailReuse(impl);
	ID3D11Buffer* nullCb = nullptr;
	ID3D11SamplerState* nullSampler = nullptr;
	impl.context11->CSSetConstantBuffers(0, 1, &nullCb);
	impl.context11->CSSetSamplers(0, 1, &nullSampler);
	return true;
}

static DetailReuseCB MakeDetailReuseConstants(uint32_t mode, uint32_t width, uint32_t height) noexcept {
	const float areaScale = std::clamp(
		std::sqrt(float(width) * float(height) / (1920.0f * 1080.0f)), 0.5f, 2.0f);
	return DetailReuseCB{
		.mode = mode,
		.workWidth = width,
		.workHeight = height,
		.clipGamma = 1.25f,
		.clipFalloff = 1.0f,
		.sigmaFloor = 0.01f,
		.motionReject = 0.5f,
		.fillStrength = 1.0f,
		.fillRadius = 24.0f * areaScale
	};
}

static bool CaptureNrHistory(
	DLSSNRFilter::Impl& impl,
	ID3D11Texture2D* input,
	ID3D11Texture2D* answer
) noexcept {
	if (impl.detailReuseFailed || !input || !answer) return false;
	D3D11_TEXTURE2D_DESC desc{};
	input->GetDesc(&desc);
	if (!EnsureDetailReusePipeline(impl) ||
		!EnsureFloatTarget(impl, desc.Width, desc.Height, impl.detailStore11, impl.detailStoreSrv11, impl.detailStoreUav11) ||
		!EnsureFloatTarget(impl, desc.Width, desc.Height, impl.detailColor11, impl.detailColorSrv11, impl.detailColorUav11) ||
		!EnsureDetailSrv(impl.device11, input, impl.detailCurrentTexture, impl.detailCurrentSrv11) ||
		!EnsureDetailSrv(impl.device11, answer, impl.detailAnswerRead, impl.detailAnswerReadSrv11)) {
		return false;
	}
	UnbindDetailReuse(impl);
	return DispatchDetailReuse(
		impl, MakeDetailReuseConstants(0, desc.Width, desc.Height),
		{ impl.detailCurrentSrv11.get(), impl.detailAnswerReadSrv11.get(),
			impl.detailCurrentSrv11.get(), impl.detailCurrentSrv11.get() },
		{ impl.detailStoreUav11.get(), impl.detailColorUav11.get() });
}

static bool ReprojectNrDetail(
	DLSSNRFilter::Impl& impl,
	ID3D11Texture2D* current,
	ID3D11Texture2D* motion,
	ID3D11Texture2D* output
) noexcept {
	if (impl.detailReuseFailed || !current || !motion || !output || current == output ||
		!impl.detailStoreSrv11 || !impl.detailColorSrv11) {
		return false;
	}
	D3D11_TEXTURE2D_DESC desc{};
	current->GetDesc(&desc);
	if (!EnsureDetailReusePipeline(impl) ||
		!EnsureFloatTarget(
			impl, desc.Width, desc.Height, impl.detailEstimate11,
			impl.detailEstimateSrv11, impl.detailEstimateUav11) ||
		!EnsureDetailSrv(impl.device11, current, impl.detailCurrentTexture, impl.detailCurrentSrv11) ||
		!EnsureDetailSrv(impl.device11, motion, impl.detailMotionTexture, impl.detailMotionSrv11) ||
		!EnsureDetailUav(impl.device11, output, impl.detailOutputTexture, impl.detailOutputUav11)) {
		return false;
	}
	if (!DispatchDetailReuse(
		impl, MakeDetailReuseConstants(1, desc.Width, desc.Height),
		{ impl.detailCurrentSrv11.get(), impl.detailStoreSrv11.get(),
			impl.detailColorSrv11.get(), impl.detailMotionSrv11.get() },
		{ impl.detailEstimateUav11.get(), nullptr })) {
		return false;
	}
	return DispatchDetailReuse(
		impl, MakeDetailReuseConstants(2, desc.Width, desc.Height),
		{ impl.detailCurrentSrv11.get(), impl.detailEstimateSrv11.get(),
			impl.detailCurrentSrv11.get(), impl.detailMotionSrv11.get() },
		{ impl.detailOutputUav11.get(), nullptr });
}

bool DLSSNRFilter::Draw(const NativeEffectDrawContext& context) noexcept {
	const bool hasUsableMotion = _settings.guidanceMode != 1 &&
		_settings.guidanceMode != 3 &&
		context.frameGuidance.motion.metadata.valid &&
		!context.frameGuidance.motion.metadata.isZero;
	const bool forceHistoryReset = _settings.historyMode == 1 ||
		(_settings.historyMode == 0 && !hasUsableMotion);
	if (!_impl || !_impl->passFeatures[0]) return false;
	if (DlssnrDeviceIsRemoved(*_impl)) {
		if (context.input != context.output) {
			_impl->context11->CopyResource(context.output, context.input);
		}
		return true;
	}
	if (_settings.detailReuse && _impl->detailReuseNext && _impl->detailValid &&
		ReprojectNrDetail(
			*_impl, context.input,
			context.frameGuidance.motion.texture,
			context.output)) {
		static bool loggedReuse = false;
		if (!loggedReuse) {
			loggedReuse = true;
			Logger::Get().Info(
				"DLSSNR detail reuse moves the saved detail with a soft colour trust and fills the gaps.");
		}
		_impl->detailReuseNext = false;
		return true;
	}
	const DLSSNRSettings firstPass = ResolvePassSettings(0);
	_activeSettings = &firstPass;
	const bool firstOk = _DrawOnce(
		context, _impl->passFeatures[0], forceHistoryReset);
	_activeSettings = nullptr;
	if (!firstOk) return false;
	const uint32_t passCount = std::min(_settings.passes, DLSSNRFilter::Impl::kMaxPasses);
	ID3D11Texture2D* proxy = context.input;
	for (uint32_t pass = 1; pass < passCount; ++pass) {
		if (!_EnsurePassFeature(pass)) break;
		if (_settings.passFeedback < 0.999f) {
			if (!ApplyPassFeedback(*_impl, proxy, context.output, _settings.passFeedback)) {
				return false;
			}
			if (pass + 1 < passCount) {
				_impl->context11->CopyResource(
					_impl->passProxy11.get(), context.output);
				proxy = _impl->passProxy11.get();
			}
		}
		const DLSSNRSettings passSettings = ResolvePassSettings(pass);
		_activeSettings = &passSettings;
		const NativeEffectDrawContext repeated{
			.input = context.output,
			.output = context.output,
			.frameId = context.frameId,
			.frameGuidance = context.frameGuidance,
			.zeroFrameGuidance = context.zeroFrameGuidance
		};
		const bool passOk = _DrawOnce(
			repeated, _impl->passFeatures[pass], forceHistoryReset);
		_activeSettings = nullptr;
		if (!passOk) return false;
	}
	if (_settings.detailReuse && !_impl->detailReuseFailed &&
		CaptureNrHistory(*_impl, context.input, context.output)) {
		_impl->detailValid = true;
		_impl->detailReuseNext = true;
	} else {
		_impl->detailValid = false;
		_impl->detailReuseNext = false;
	}
	return true;
}

}

#else

namespace Magpie {

struct DLSSNRFilter::Impl {};
DLSSNRFilter::DLSSNRFilter() = default;
DLSSNRFilter::~DLSSNRFilter() = default;
FrameGuidanceRequirements
DLSSNRFilter::GetFrameGuidanceRequirements() const noexcept { return {}; }
bool DLSSNRFilter::Initialize(
	DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*,
	const DLSSNRSettings&) noexcept {
	g_dlssnrRuntimeState.store(
		static_cast<int32_t>(DLSSNRRuntimeState::Failed), std::memory_order_relaxed);
	Logger::Get().Error("DLSSNR support is disabled at build time");
	return false;
}
bool DLSSNRFilter::Resize(
	DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*) noexcept {
	return false;
}
bool DLSSNRFilter::Draw(const NativeEffectDrawContext&) noexcept {
	return false;
}

}

#endif
