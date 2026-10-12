#include "pch.h"
#include "AdaptivePresenter.h"
#include "DeviceResources.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "Win32Helper.h"
#include <d3dcompiler.h>

namespace Magpie {

bool AdaptivePresenter::_Initialize(HWND hwndAttach) noexcept {
	if (ScalingWindow::Get().Options().IsDirectFlipDisabled()) {
		// 禁用 DirectFlip 时始终使用 DirectComposition 呈现
		if (!_ResizeDCompVisual(hwndAttach)) {
			Logger::Get().Error("_ResizeDCompVisual 失败");
			return false;
		}

		_isDCompPresenting = true;
		return true;
	}

	const uint32_t bufferCount = _CalcBufferCount();
	bool useDlssFgSwapChain = false;
	for (const EffectOption& effect : ScalingWindow::Get().Options().effects) {
		if (effect.name == "DLSSFG\\DLSS_FrameGeneration") {
			useDlssFgSwapChain = true;
			break;
		}
	}

	const SIZE rendererSize = Win32Helper::GetSizeOfRect(ScalingWindow::Get().RendererRect());
	// Direct flip scans 8-bit sRGB out as linear light on an HDR panel. scRGB and
	// HDR10 presents did the same on this driver. Hand the 8-bit image to DWM.
	const bool hdrOutput = useDlssFgSwapChain && _IsAttachedOutputHdr(hwndAttach);
	const bool dwmSwapChain = hdrOutput && _EnsureHdrBlitPipeline() &&
		_TryCreateDwmSwapChain(hwndAttach, bufferCount, rendererSize);
	DXGI_SWAP_CHAIN_DESC1 sd{
		.Width = (UINT)rendererSize.cx,
		.Height = (UINT)rendererSize.cy,
		.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
		.SampleDesc = {
			.Count = 1
		},
		.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
		.BufferCount = bufferCount,
#ifdef _DEBUG
		// 我们应确保两种渲染方式可以无缝切换，DXGI_SCALING_NONE 使错误更容易观察到
		.Scaling = DXGI_SCALING_NONE,
#else
		// 如果两种渲染方式无法无缝切换，DXGI_SCALING_STRETCH 使视觉变化尽可能小
		.Scaling = DXGI_SCALING_STRETCH,
#endif
		// FLIP_DISCARD + Present(0) drops unread interpolations, so a capture of
		// scanout only sees the latest real frame. FLIP_SEQUENTIAL keeps each
		// generated frame until scanout. The queue stays one frame (latency 1).
		.SwapEffect = useDlssFgSwapChain
			? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
			: DXGI_SWAP_EFFECT_FLIP_DISCARD,
		.AlphaMode = DXGI_ALPHA_MODE_IGNORE,
		// 只要显卡支持始终启用 DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING 以支持可变刷新率
		.Flags = UINT((_deviceResources->IsTearingSupported() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)
		| DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
	};

	ID3D11Device5* d3dDevice = _deviceResources->GetD3DDevice();
	winrt::com_ptr<IDXGISwapChain1> dxgiSwapChain;
	HRESULT hr = S_OK;
	if (!dwmSwapChain) {
		hr = _deviceResources->GetDXGIFactory()->CreateSwapChainForHwnd(
			d3dDevice,
			hwndAttach,
			&sd,
			nullptr,
			nullptr,
			dxgiSwapChain.put()
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("创建交换链失败", hr);
			return false;
		}

		_dxgiSwapChain = dxgiSwapChain.try_as<IDXGISwapChain4>();
		if (!_dxgiSwapChain) {
			Logger::Get().Error("获取 IDXGISwapChain2 失败");
			return false;
		}
	}

	uint32_t maximumFrameLatency = bufferCount - 1;
	if (useDlssFgSwapChain) {
		maximumFrameLatency = 1;
		if (!dwmSwapChain) {
			_useTearingPresent = _deviceResources->IsTearingSupported();
		}
	}
	// DLSSFG submits multiple output frames for each captured frame. Restrict
	// its swap-chain queue to one frame so presentation cannot add another
	// multi-frame latency queue; other effects retain the original behavior.
	if (hdrOutput && !dwmSwapChain) {
		_useTearingPresent = false;
		UINT colorSpaceSupport = 0;
		if (SUCCEEDED(_dxgiSwapChain->CheckColorSpaceSupport(
			DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &colorSpaceSupport)) &&
			(colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
			_dxgiSwapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
		}
		Logger::Get().Info("DLSSFG HDR display is presenting 8-bit without tearing");
	}

	_dxgiSwapChain->SetMaximumFrameLatency(maximumFrameLatency);
	if (maximumFrameLatency == 1) {
		Logger::Get().Info(fmt::format(
			"DLSSFG swap-chain maximum frame latency: 1, tearingPresent={}, flipSequential=true",
			_useTearingPresent));
	}

	_frameLatencyWaitableObject.reset(_dxgiSwapChain->GetFrameLatencyWaitableObject());
	if (!_frameLatencyWaitableObject) {
		Logger::Get().Error("GetFrameLatencyWaitableObject 失败");
		return false;
	}

	hr = _deviceResources->GetDXGIFactory()->MakeWindowAssociation(
		hwndAttach, DXGI_MWA_NO_ALT_ENTER);
	if (FAILED(hr)) {
		Logger::Get().ComError("MakeWindowAssociation 失败", hr);
	}

	hr = _dxgiSwapChain->GetBuffer(0, IID_PPV_ARGS(_backBuffer.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("获取后缓冲区失败", hr);
		return false;
	}

	hr = d3dDevice->CreateRenderTargetView(_backBuffer.get(), nullptr, _backBufferRtv.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateRenderTargetView 失败", hr);
		return false;
	}

	return true;
}

bool AdaptivePresenter::BeginFrame(
	winrt::com_ptr<ID3D11Texture2D>& frameTex,
	winrt::com_ptr<ID3D11RenderTargetView>& frameRtv,
	POINT& drawOffset
) noexcept {
	if (_isDCompPresenting) {
		HRESULT hr = _dcompSurface->BeginDraw(nullptr, IID_PPV_ARGS(&frameTex), &drawOffset);
		if (FAILED(hr)) {
			Logger::Get().ComError("BeginDraw 失败", hr);
			return false;
		}

		hr = _deviceResources->GetD3DDevice()->CreateRenderTargetView(
			frameTex.get(), nullptr, frameRtv.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateRenderTargetView 失败", hr);
			return false;
		}
	} else {
		drawOffset = {};

		if (!_isframeLatencyWaited) {
			_frameLatencyWaitableObject.wait(1000);
			_isframeLatencyWaited = true;
		}

		frameTex = _hdrScanout ? _composeTex : _backBuffer;
		frameRtv = _hdrScanout ? _composeRtv : _backBufferRtv;
	}
	
	return true;
}

void AdaptivePresenter::EndFrame(bool waitForGpu) noexcept {
	if (_isDCompPresenting) {
		_dcompSurface->EndDraw();
	} else if (_hdrScanout) {
		_BlitSdrToHdr();
	}

	if (waitForGpu || _isResized) {
		_isResized = false;

		// 下面两个调用用于减少调整窗口尺寸时的边缘闪烁。
		// 
		// 我们希望 DWM 绘制新的窗口框架时刚好合成新帧，但这不是我们能控制的，尤其是混合架构
		// 下需要在显卡间传输帧数据，无法预测 Present/Commit 后多久 DWM 能收到。我们只能尽
		// 可能为 DWM 合成新帧预留时间，这包括两个步骤：
		// 
		// 1. 首先等待渲染完成，确保新帧对 DWM 随时可用。
		// 2. 然后在新一轮合成开始时提交，这让 DWM 有更多时间合成新帧。
		// 
		// 目前看来除非像 UWP 一般有 DWM 协助，否则彻底摆脱闪烁是不可能的。
		// 
		// https://github.com/Blinue/Magpie/pull/1071#issuecomment-2718314731 讨论了 UWP
		// 调整尺寸的方法，测试表明可以彻底解决闪烁问题。不过它使用了很不稳定的私有接口，没有
		// 实用价值。

		// 等待渲染完成
		_WaitForGpu();

		// 等待 DWM 开始合成新一帧
		Win32Helper::WaitForDwmComposition();
	}

	if (_isDCompPresenting) {
		_dcompDevice->Commit();
	} else {
		UINT presentFlags = _useTearingPresent ? DXGI_PRESENT_ALLOW_TEARING : 0;
		HRESULT presentHr = _dxgiSwapChain->Present(0, presentFlags);
		if (presentHr == DXGI_ERROR_INVALID_CALL && presentFlags != 0) {
			_useTearingPresent = false;
			Logger::Get().Warn(
				"DLSSFG DXGI_PRESENT_ALLOW_TEARING rejected; falling back to Present(0, 0)");
			presentHr = _dxgiSwapChain->Present(0, 0);
		}
		_isframeLatencyWaited = false;
		if (_compositionSwapChain) {
			_dcompDevice->Commit();
		}

		// 丢弃渲染目标的内容
		_deviceResources->GetD3DDC()->DiscardView(_backBufferRtv.get());

		if (_isSwitchingToSwapChain) {
			_isSwitchingToSwapChain = false;

			// 等待交换链呈现新帧
			_WaitForGpu();
			Win32Helper::WaitForDwmComposition();

			// A composition swap chain is only visible through its visual.
			_dcompVisual->SetContent(_compositionSwapChain ? _dxgiSwapChain.get() : nullptr);
			_dcompDevice->Commit();
		}
	}
}

bool AdaptivePresenter::OnResize() noexcept {
	_isResized = true;

	if (ScalingWindow::Get().IsResizingOrMoving() || !_dxgiSwapChain) {
		// 切换到 DirectComposition 呈现，失败则回落到交换链
		_isDCompPresenting = _ResizeDCompVisual();
		if (_isDCompPresenting) {
			return true;
		}

		Logger::Get().Error("_ResizeDCompVisual 失败");

		// 禁用 DirectFlip 时不存在交换链
		if (!_dxgiSwapChain) {
			return false;
		}
	}

	if (!_ResizeSwapChain()) {
		Logger::Get().Error("_ResizeSwapChain 失败");
		return false;
	}
	
	return true;
}

void AdaptivePresenter::OnEndResize(bool& shouldRedraw) noexcept {
	if (!_isDCompPresenting || !_dxgiSwapChain) {
		shouldRedraw = false;
		return;
	}

	shouldRedraw = true;

	_ResizeSwapChain();
	_isDCompPresenting = false;
	// 交换链呈现新帧后再清除 DirectCompostion 内容，确保无缝切换
	_isSwitchingToSwapChain = true;
}

bool AdaptivePresenter::_ResizeSwapChain() noexcept {
	assert(_dxgiSwapChain);

	if (!_isframeLatencyWaited) {
		_frameLatencyWaitableObject.wait(1000);
		_isframeLatencyWaited = true;
	}

	_backBuffer = nullptr;
	_backBufferRtv = nullptr;

	const RECT& swapChainRect = ScalingWindow::Get().RendererRect();
	const SIZE swapChainSize = Win32Helper::GetSizeOfRect(swapChainRect);
	HRESULT hr = _dxgiSwapChain->ResizeBuffers(
		0,
		(UINT)swapChainSize.cx,
		(UINT)swapChainSize.cy,
		DXGI_FORMAT_UNKNOWN,
		UINT((_deviceResources->IsTearingSupported() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)
		| DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("ResizeBuffers 失败", hr);
		return false;
	}

	hr = _dxgiSwapChain->GetBuffer(0, IID_PPV_ARGS(_backBuffer.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("获取后缓冲区失败", hr);
		return false;
	}

	hr = _deviceResources->GetD3DDevice()->CreateRenderTargetView(
		_backBuffer.get(), nullptr, _backBufferRtv.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateRenderTargetView 失败", hr);
		return false;
	}

	if (_hdrScanout && !_EnsureComposeTarget((UINT)swapChainSize.cx, (UINT)swapChainSize.cy)) {
		Logger::Get().Error("_EnsureComposeTarget 失败");
		return false;
	}

	return true;
}

bool AdaptivePresenter::_ResizeDCompVisual(HWND hwndAttach) noexcept {
	const SIZE rendererSize = Win32Helper::GetSizeOfRect(ScalingWindow::Get().RendererRect());

	if (_dcompSurface) {
		// 使用 IDCompositionVirtualSurface 而不是 IDCompositionSurface 的原因是
		// IDCompositionDevice2::CreateSurface 有时相当慢，最坏情况下要几十毫秒。
		HRESULT hr = _dcompSurface->Resize((UINT)rendererSize.cx, (UINT)rendererSize.cy);
		if (FAILED(hr)) {
			Logger::Get().ComError("Resize 失败", hr);
			return false;
		}
	} else {
		// 初始化 DirectComposition
		HRESULT hr = DCompositionCreateDevice3(
			_deviceResources->GetD3DDevice(), IID_PPV_ARGS(&_dcompDevice));
		if (FAILED(hr)) {
			Logger::Get().ComError("DCompositionCreateDevice3 失败", hr);
			return false;
		}

		if (!hwndAttach) {
			// 没有禁用 DirectFlip 时才会在调整大小时初始化，因此必定存在交换链
			hr = _dxgiSwapChain->GetHwnd(&hwndAttach);
			if (FAILED(hr)) {
				Logger::Get().ComError("GetHwnd 失败", hr);
				return false;
			}
		}

		hr = _dcompDevice->CreateTargetForHwnd(hwndAttach, TRUE, _dcompTarget.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateTargetForHwnd 失败", hr);
			return false;
		}

		hr = _dcompDevice->CreateVisual(_dcompVisual.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateVisual 失败", hr);
			return false;
		}

		hr = _dcompTarget->SetRoot(_dcompVisual.get());
		if (FAILED(hr)) {
			Logger::Get().ComError("SetRoot 失败", hr);
			return false;
		}

		hr = _dcompDevice->CreateVirtualSurface(
			(UINT)rendererSize.cx,
			(UINT)rendererSize.cy,
			DXGI_FORMAT_R8G8B8A8_UNORM,
			DXGI_ALPHA_MODE_IGNORE,
			_dcompSurface.put()
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateVirtualSurface 失败", hr);
			return false;
		}
	}

	HRESULT hr = _dcompVisual->SetContent(_dcompSurface.get());
	if (FAILED(hr)) {
		Logger::Get().ComError("SetContent 失败", hr);
		return false;
	}

	return true;
}

bool AdaptivePresenter::_TryCreateDwmSwapChain(HWND hwnd, uint32_t bufferCount, SIZE size) noexcept {
	DXGI_SWAP_CHAIN_DESC1 sd{
		.Width = (UINT)size.cx,
		.Height = (UINT)size.cy,
		.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
		.SampleDesc = { .Count = 1 },
		.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
		.BufferCount = bufferCount,
		.Scaling = DXGI_SCALING_STRETCH,
		.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
		.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED,
		.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
	};

	winrt::com_ptr<IDXGISwapChain1> swapChain;
	HRESULT hr = _deviceResources->GetDXGIFactory()->CreateSwapChainForComposition(
		_deviceResources->GetD3DDevice(), &sd, nullptr, swapChain.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateSwapChainForComposition failed", hr);
		return false;
	}
	_dxgiSwapChain = swapChain.try_as<IDXGISwapChain4>();
	if (!_dxgiSwapChain) {
		Logger::Get().Error("获取 IDXGISwapChain4 失败");
		return false;
	}

	UINT colorSpaceSupport = 0;
	if (SUCCEEDED(_dxgiSwapChain->CheckColorSpaceSupport(
		DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &colorSpaceSupport)) &&
		(colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
		hr = _dxgiSwapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
		if (FAILED(hr)) {
			Logger::Get().ComError("SetColorSpace1 sRGB failed", hr);
		}
	}

	auto fail = [this]() {
		_dcompVisual = nullptr;
		_dcompTarget = nullptr;
		_dcompDevice = nullptr;
		_dxgiSwapChain = nullptr;
		_compositionSwapChain = false;
		_hdrScanout = false;
		return false;
	};

	hr = DCompositionCreateDevice3(
		_deviceResources->GetD3DDevice(), IID_PPV_ARGS(_dcompDevice.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("DCompositionCreateDevice3 failed", hr);
		return fail();
	}
	hr = _dcompDevice->CreateTargetForHwnd(hwnd, TRUE, _dcompTarget.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateTargetForHwnd failed", hr);
		return fail();
	}
	hr = _dcompDevice->CreateVisual(_dcompVisual.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateVisual failed", hr);
		return fail();
	}
	hr = _dcompVisual->SetContent(_dxgiSwapChain.get());
	if (FAILED(hr)) {
		Logger::Get().ComError("SetContent failed", hr);
		return fail();
	}
	hr = _dcompTarget->SetRoot(_dcompVisual.get());
	if (FAILED(hr)) {
		Logger::Get().ComError("SetRoot failed", hr);
		return fail();
	}
	hr = _dcompDevice->Commit();
	if (FAILED(hr)) {
		Logger::Get().ComError("Commit failed", hr);
		return fail();
	}
	if (!_EnsureComposeTarget(sd.Width, sd.Height)) {
		return fail();
	}

	_compositionSwapChain = true;
	_hdrScanout = true;
	_useTearingPresent = false;
	Logger::Get().Info("DLSSFG HDR display is 8-bit sRGB through DWM");
	return true;
}

bool AdaptivePresenter::_IsAttachedOutputHdr(HWND hwnd) noexcept {
	const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	winrt::com_ptr<IDXGIAdapter1> adapter;
	for (UINT adapterIndex = 0;
		SUCCEEDED(_deviceResources->GetDXGIFactory()->EnumAdapters1(adapterIndex, adapter.put()));
		++adapterIndex, adapter = nullptr) {
		winrt::com_ptr<IDXGIOutput> output;
		for (UINT outputIndex = 0;
			SUCCEEDED(adapter->EnumOutputs(outputIndex, output.put()));
			++outputIndex, output = nullptr) {
			DXGI_OUTPUT_DESC desc{};
			if (FAILED(output->GetDesc(&desc)) || desc.Monitor != monitor) {
				continue;
			}
			winrt::com_ptr<IDXGIOutput6> output6 = output.try_as<IDXGIOutput6>();
			DXGI_OUTPUT_DESC1 desc1{};
			return output6 && SUCCEEDED(output6->GetDesc1(&desc1)) &&
				desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
		}
	}
	return false;
}

float AdaptivePresenter::_QuerySdrWhiteScale(HWND hwnd) noexcept {
	const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXW info{ sizeof(info) };
	if (!GetMonitorInfoW(monitor, &info)) {
		return 1.0f;
	}

	UINT32 pathCount = 0;
	UINT32 modeCount = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
		return 1.0f;
	}
	std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
	if (QueryDisplayConfig(
		QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
		return 1.0f;
	}
	paths.resize(pathCount);

	for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
		source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		source.header.size = sizeof(source);
		source.header.adapterId = path.sourceInfo.adapterId;
		source.header.id = path.sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
			continue;
		}
		if (CompareStringOrdinal(source.viewGdiDeviceName, -1, info.szDevice, -1, TRUE) != CSTR_EQUAL) {
			continue;
		}

		DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
		white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
		white.header.size = sizeof(white);
		white.header.adapterId = path.targetInfo.adapterId;
		white.header.id = path.targetInfo.id;
		if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS || white.SDRWhiteLevel == 0) {
			return 1.0f;
		}
		return white.SDRWhiteLevel / 1000.0f;
	}
	return 1.0f;
}

bool AdaptivePresenter::_EnsureHdrBlitPipeline() noexcept {
	if (_hdrPs) {
		return true;
	}

	static constexpr char kHlsl[] = R"(
struct VsOut {
	float4 pos : SV_Position;
};

VsOut VSMain(uint id : SV_VertexID) {
	VsOut o;
	o.pos = float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0);
	return o;
}

Texture2D<float4> source : register(t0);

float4 PSMain(float4 pos : SV_Position) : SV_Target {
	// Composition requires premultiplied alpha. Captured alpha is often 0,
	// which would blend the picture with the HDR desktop and wash it out.
	return float4(source.Load(int3(pos.xy, 0)).rgb, 1);
}
)";

	auto compile = [](const char* target, const char* entry, ID3DBlob** blob) {
		winrt::com_ptr<ID3DBlob> errors;
		const HRESULT hr = D3DCompile(
			kHlsl, sizeof(kHlsl) - 1, "HdrSdrBlit.hlsl", nullptr, nullptr,
			entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, errors.put());
		if (FAILED(hr)) {
			Logger::Get().ComError(
				errors ? (const char*)errors->GetBufferPointer() : "D3DCompile failed", hr);
			return false;
		}
		return true;
	};

	winrt::com_ptr<ID3DBlob> vsBlob;
	winrt::com_ptr<ID3DBlob> psBlob;
	if (!compile("vs_5_0", "VSMain", vsBlob.put()) || !compile("ps_5_0", "PSMain", psBlob.put())) {
		return false;
	}

	ID3D11Device* device = _deviceResources->GetD3DDevice();
	HRESULT hr = device->CreateVertexShader(
		vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, _hdrVs.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateVertexShader failed", hr);
		return false;
	}
	hr = device->CreatePixelShader(
		psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, _hdrPs.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreatePixelShader failed", hr);
		return false;
	}

	D3D11_BUFFER_DESC cbDesc{
		.ByteWidth = 16,
		.Usage = D3D11_USAGE_DYNAMIC,
		.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
		.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE
	};
	hr = device->CreateBuffer(&cbDesc, nullptr, _hdrCb.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateBuffer failed", hr);
		return false;
	}

	D3D11_RASTERIZER_DESC rasterDesc{
		.FillMode = D3D11_FILL_SOLID,
		.CullMode = D3D11_CULL_NONE,
		.DepthClipEnable = TRUE
	};
	hr = device->CreateRasterizerState(&rasterDesc, _hdrRasterizer.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateRasterizerState failed", hr);
		return false;
	}
	return true;
}

bool AdaptivePresenter::_EnsureComposeTarget(UINT width, UINT height) noexcept {
	if (_composeTex) {
		D3D11_TEXTURE2D_DESC existing{};
		_composeTex->GetDesc(&existing);
		if (existing.Width == width && existing.Height == height) {
			return true;
		}
	}

	_composeSrv = nullptr;
	_composeRtv = nullptr;
	_composeTex = nullptr;

	D3D11_TEXTURE2D_DESC desc{
		.Width = width,
		.Height = height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE
	};
	ID3D11Device* device = _deviceResources->GetD3DDevice();
	HRESULT hr = device->CreateTexture2D(&desc, nullptr, _composeTex.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateTexture2D compose target failed", hr);
		return false;
	}
	hr = device->CreateRenderTargetView(_composeTex.get(), nullptr, _composeRtv.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateRenderTargetView compose target failed", hr);
		return false;
	}
	hr = device->CreateShaderResourceView(_composeTex.get(), nullptr, _composeSrv.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateShaderResourceView compose target failed", hr);
		return false;
	}
	return true;
}

void AdaptivePresenter::_BlitSdrToHdr() noexcept {
	if (!_composeSrv || !_hdrPs || !_backBufferRtv) {
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	if (_sdrWhiteQueryTime.time_since_epoch().count() == 0 ||
		now - _sdrWhiteQueryTime >= std::chrono::milliseconds(500)) {
		_sdrWhiteQueryTime = now;
		HWND hwnd = nullptr;
		if (FAILED(_dxgiSwapChain->GetHwnd(&hwnd))) {
			hwnd = nullptr;
		}
		const float scale = _QuerySdrWhiteScale(hwnd);
		if (scale > 0.0f) {
			_sdrWhiteScale = scale;
		}
	}

	ID3D11DeviceContext4* dc = _deviceResources->GetD3DDC();
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (SUCCEEDED(dc->Map(_hdrCb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		*static_cast<float*>(mapped.pData) = _sdrWhiteScale * 80.0f;
		dc->Unmap(_hdrCb.get(), 0);
	}

	D3D11_TEXTURE2D_DESC desc{};
	_backBuffer->GetDesc(&desc);
	const D3D11_VIEWPORT viewport{
		.Width = float(desc.Width),
		.Height = float(desc.Height),
		.MaxDepth = 1.0f
	};

	// The cursor leaves premultiplied blending bound (dest *= srcAlpha, then
	// add). This pass writes alpha 1, so that blend adds every present onto the
	// FLIP_SEQUENTIAL float buffer until the image burns out.
	dc->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	dc->OMSetRenderTargets(0, nullptr, nullptr);
	ID3D11RenderTargetView* rtv = _backBufferRtv.get();
	dc->OMSetRenderTargets(1, &rtv, nullptr);
	constexpr float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	dc->ClearRenderTargetView(rtv, black);
	dc->RSSetViewports(1, &viewport);
	dc->RSSetState(_hdrRasterizer.get());
	dc->IASetInputLayout(nullptr);
	dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	dc->VSSetShader(_hdrVs.get(), nullptr, 0);
	dc->PSSetShader(_hdrPs.get(), nullptr, 0);
	ID3D11Buffer* cb = _hdrCb.get();
	dc->PSSetConstantBuffers(0, 1, &cb);
	ID3D11ShaderResourceView* srv = _composeSrv.get();
	dc->PSSetShaderResources(0, 1, &srv);
	dc->Draw(3, 0);

	ID3D11ShaderResourceView* empty = nullptr;
	dc->PSSetShaderResources(0, 1, &empty);
	dc->OMSetRenderTargets(0, nullptr, nullptr);
}

}
