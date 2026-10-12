#pragma once
#include "PresenterBase.h"
#include <dcomp.h>

namespace Magpie {

// 根据需要在交换链和 DirectComposition 两种呈现方式间切换。交换链可以触发
// DirectFlip/IndependentFlip 以最小化延迟，DirectComposition 在调整尺寸
// 时闪烁更少，这个呈现器旨在结合两者的优势。
class AdaptivePresenter final : public PresenterBase {
protected:
	bool _Initialize(HWND hwndAttach) noexcept override;

public:
	bool BeginFrame(
		winrt::com_ptr<ID3D11Texture2D>& frameTex,
		winrt::com_ptr<ID3D11RenderTargetView>& frameRtv,
		POINT& drawOffset
	) noexcept override;

	void EndFrame(bool waitForGpu = false) noexcept override;

	bool OnResize() noexcept override;

	void OnEndResize(bool& shouldRedraw) noexcept override;

private:
	bool _ResizeSwapChain() noexcept;

	bool _ResizeDCompVisual(HWND hwndAttach = NULL) noexcept;

	bool _IsAttachedOutputHdr(HWND hwnd) noexcept;

	bool _TryCreateDwmSwapChain(HWND hwnd, uint32_t bufferCount, SIZE size) noexcept;

	float _QuerySdrWhiteScale(HWND hwnd) noexcept;

	bool _EnsureHdrBlitPipeline() noexcept;

	bool _EnsureComposeTarget(UINT width, UINT height) noexcept;

	void _BlitSdrToHdr() noexcept;

	winrt::com_ptr<IDXGISwapChain4> _dxgiSwapChain;
	wil::unique_event_nothrow _frameLatencyWaitableObject;
	winrt::com_ptr<ID3D11Texture2D> _backBuffer;
	winrt::com_ptr<ID3D11RenderTargetView> _backBufferRtv;

	// On an HDR monitor the 8-bit image is drawn here, then copied onto a
	// composition swap chain so DWM does the SDR-to-HDR conversion.
	winrt::com_ptr<ID3D11Texture2D> _composeTex;
	winrt::com_ptr<ID3D11RenderTargetView> _composeRtv;
	winrt::com_ptr<ID3D11ShaderResourceView> _composeSrv;
	winrt::com_ptr<ID3D11VertexShader> _hdrVs;
	winrt::com_ptr<ID3D11PixelShader> _hdrPs;
	winrt::com_ptr<ID3D11Buffer> _hdrCb;
	winrt::com_ptr<ID3D11RasterizerState> _hdrRasterizer;
	std::chrono::steady_clock::time_point _sdrWhiteQueryTime{};
	float _sdrWhiteScale = 1.0f;
	bool _hdrScanout = false;
	// Composition swap chain. DWM owns the color conversion; tearing stays off.
	bool _compositionSwapChain = false;

	// 调整大小或禁用 DirectFlip 时使用
	winrt::com_ptr<IDCompositionDesktopDevice> _dcompDevice;
	winrt::com_ptr<IDCompositionTarget> _dcompTarget;
	winrt::com_ptr<IDCompositionVisual2> _dcompVisual;
	winrt::com_ptr<IDCompositionVirtualSurface> _dcompSurface;
	
	bool _isDCompPresenting = false;
	bool _isResized = false;
	bool _isframeLatencyWaited = false;
	bool _isSwitchingToSwapChain = false;
	// DLSSFG presents several frames per capture. Tearing lets those frames
	// scan out instead of being coalesced to the capture rate.
	bool _useTearingPresent = false;
};

}
