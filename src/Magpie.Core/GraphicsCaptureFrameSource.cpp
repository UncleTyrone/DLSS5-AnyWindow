#include "pch.h"
#include "GraphicsCaptureFrameSource.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include <dwmapi.h>
#include <d3dcompiler.h>
#include <Windows.Graphics.DirectX.Direct3D11.interop.h>

namespace winrt {
using namespace Windows::Graphics;
using namespace Windows::Graphics::Capture;
using namespace Windows::Graphics::DirectX;
using namespace Windows::Graphics::DirectX::Direct3D11;
}

namespace Magpie {

static bool IsMonitorHdr(const DeviceResources& resources, HWND hwnd) noexcept {
	const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	winrt::com_ptr<IDXGIAdapter1> adapter;
	for (UINT adapterIndex = 0;
		SUCCEEDED(resources.GetDXGIFactory()->EnumAdapters1(adapterIndex, adapter.put()));
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

static float QuerySdrWhiteScale(HWND hwnd) noexcept {
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

bool GraphicsCaptureFrameSource::_Initialize() noexcept {
	ID3D11Device5* d3dDevice = _deviceResources->GetD3DDevice();

	HRESULT hr;

	winrt::com_ptr<IGraphicsCaptureItemInterop> interop;
	try {
		if (!winrt::GraphicsCaptureSession::IsSupported()) {
			Logger::Get().Error("当前不支持 WinRT 捕获");
			return false;
		}

		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		d3dDevice->QueryInterface<IDXGIDevice>(dxgiDevice.put());

		hr = CreateDirect3D11DeviceFromDXGIDevice(
			dxgiDevice.get(),
			reinterpret_cast<::IInspectable**>(winrt::put_abi(_wrappedD3DDevice))
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("创建 IDirect3DDevice 失败", hr);
			return false;
		}

		// 从窗口句柄获取 GraphicsCaptureItem
		interop = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
		if (!interop) {
			Logger::Get().Error("获取 IGraphicsCaptureItemInterop 失败");
			return false;
		}
	} catch (const winrt::hresult_error& e) {
		Logger::Get().Error(StrHelper::Concat("初始化 WinRT 失败: ", StrHelper::UTF16ToUTF8(e.message())));
		return false;
	}

	if (!_CaptureWindow(interop.get())) {
		Logger::Get().Error("窗口捕获失败");
		return false;
	}

	_output = DirectXHelper::CreateTexture2D(
		d3dDevice,
		DXGI_FORMAT_B8G8R8A8_UNORM,
		_frameBox.right - _frameBox.left,
		_frameBox.bottom - _frameBox.top,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET
	);
	if (!_output) {
		Logger::Get().Error("创建纹理失败");
		return false;
	}
	hr = d3dDevice->CreateRenderTargetView(_output.get(), nullptr, _outputRtv.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateRenderTargetView capture output failed", hr);
		return false;
	}

	Logger::Get().Info("GraphicsCaptureFrameSource 初始化完成");
	return true;
}

bool GraphicsCaptureFrameSource::Start() noexcept {
	_DisableRoundCornerInWin11();
	return _StartCapture();
}

FrameSourceState GraphicsCaptureFrameSource::_Update() noexcept {
	if (!_captureSession) {
		return FrameSourceState::Waiting;
	}

	winrt::Direct3D11CaptureFrame frame = _captureFramePool.TryGetNextFrame();
	if (!frame) {
		// 因为已通过 FrameArrived 注册回调，所以每当有新帧时会有新消息到达
		return FrameSourceState::Waiting;
	}

	++_sourceFrameCounts.arrived;
	// 取最新帧，帧率较低时可以有效降低延迟
	while (true) {
		if (winrt::Direct3D11CaptureFrame nextFrame = _captureFramePool.TryGetNextFrame()) {
			frame = std::move(nextFrame);
			++_sourceFrameCounts.arrived;
		} else {
			break;
		}
	}

	// 从帧获取 IDXGISurface
	winrt::IDirect3DSurface d3dSurface = frame.Surface();

	winrt::com_ptr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> dxgiInterfaceAccess(
		d3dSurface.try_as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>()
	);

	winrt::com_ptr<ID3D11Texture2D> withFrame;
	HRESULT hr = dxgiInterfaceAccess->GetInterface(IID_PPV_ARGS(&withFrame));
	if (FAILED(hr)) {
		Logger::Get().ComError("从获取 IDirect3DSurface 获取 ID3D11Texture2D 失败", hr);
		return FrameSourceState::Error;
	}

	D3D11_TEXTURE2D_DESC sourceDesc{};
	withFrame->GetDesc(&sourceDesc);
	if (_hdrScRgbCapture && sourceDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
		_ConvertScRgbToSrgb(withFrame.get());
	} else {
		_deviceResources->GetD3DDC()->CopySubresourceRegion(
			_output.get(), 0, 0, 0, 0, withFrame.get(), 0, &_frameBox);
	}

	return FrameSourceState::NewFrame;
}

void GraphicsCaptureFrameSource::OnCursorVisibilityChanged(bool isVisible, bool onDestory) noexcept {
	// 显示光标时必须重启捕获
	if (isVisible) {
		_StopCapture();
		
		if (onDestory) {
			// FIXME: 这里尝试修复拖动窗口时光标不显示的问题，但有些环境下不起作用
			SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, 0);
		} else {
			_StartCapture();
		}
	}
}

static bool CalcWindowCapturedFrameBounds(HWND hWnd, RECT& rect) noexcept {
	// Graphics Capture 的捕获区域没有文档记录，这里的计算是我实验了多种窗口后得出的，
	// 高度依赖实现细节，未来可能会失效。
	// Win10 和 Win11 24H2 开始捕获区域为 extended frame bounds；Win11 24H2 前
	// DwmGetWindowAttribute 对最大化的窗口返回值和 Win10 不同，可能是 OS 的 bug，
	// 应进一步处理。
	const auto& srcTracker = ScalingWindow::Get().SrcTracker();
	rect = srcTracker.WindowFrameRect();
	
	if (!srcTracker.IsZoomed() ||
		Win32Helper::GetOSVersion().IsWin10() ||
		Win32Helper::GetOSVersion().Is24H2OrNewer())
	{
		return true;
	}

	// 如果窗口禁用了非客户区域绘制则捕获区域为 extended frame bounds
	BOOL hasBorder = TRUE;
	HRESULT hr = DwmGetWindowAttribute(hWnd, DWMWA_NCRENDERING_ENABLED, &hasBorder, sizeof(hasBorder));
	if (FAILED(hr)) {
		Logger::Get().ComError("DwmGetWindowAttribute 失败", hr);
		return false;
	}

	if (!hasBorder) {
		return true;
	}

	RECT clientRect;
	if (!Win32Helper::GetClientScreenRect(hWnd, clientRect)) {
		Logger::Get().Error("GetClientScreenRect 失败");
		return false;
	}

	// 有些窗口最大化后有部分客户区在屏幕外，如 UWP 和资源管理器，它们的捕获区域
	// 是整个客户区。否则捕获区域不会超出屏幕
	HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ .cbSize = sizeof(mi) };
	if (!GetMonitorInfo(hMon, &mi)) {
		Logger::Get().Win32Error("GetMonitorInfo 失败");
		return false;
	}

	if (clientRect.top < mi.rcWork.top) {
		rect = clientRect;
	} else {
		Win32Helper::IntersectRect(rect, rect, mi.rcWork);
	}

	return true;
}

// 部分使用 Kirikiri 引擎的游戏有着这样的架构: 游戏窗口并非顶级窗口，而是被一个零尺寸
// 的窗口所有。此时 Alt+Tab 列表中的窗口和任务栏图标实际上是所有者窗口，这会导致 WGC
// 捕获失败。我们特殊处理这类窗口。
static bool IsKirikiriWindow(HWND hwndSrc) noexcept {
	const HWND hwndOwner = GetWindowOwner(hwndSrc);
	if (!hwndOwner) {
		return false;
	}

	RECT ownerRect;
	if (!GetWindowRect(hwndOwner, &ownerRect)) {
		Logger::Get().Win32Error("GetWindowRect 失败");
		return false;
	}

	// 所有者窗口尺寸为零，而且是顶级窗口
	return ownerRect.left == ownerRect.right && ownerRect.top == ownerRect.bottom &&
		!GetWindowOwner(hwndOwner);
}

bool GraphicsCaptureFrameSource::_CaptureWindow(IGraphicsCaptureItemInterop* interop) noexcept {
	const SrcTracker& srcTracker = ScalingWindow::Get().SrcTracker();
	const HWND hwndSrc = srcTracker.Handle();
	const RECT& srcRect = srcTracker.SrcRect();

	RECT frameBounds;
	if (!CalcWindowCapturedFrameBounds(hwndSrc, frameBounds)) {
		Logger::Get().Error("CalcWindowCapturedFrameBounds 失败");
		return false;
	}

	if (srcRect.left < frameBounds.left || srcRect.top < frameBounds.top) {
		Logger::Get().Error("裁剪边框错误");
		return false;
	}

	// 在源窗口存在 DPI 缩放时有时会有一像素的偏移（取决于窗口在屏幕上的位置）
	// 可能是 DwmGetWindowAttribute 的 bug
	_frameBox = {
		UINT(srcRect.left - frameBounds.left),
		UINT(srcRect.top - frameBounds.top),
		0,
		UINT(srcRect.right - frameBounds.left),
		UINT(srcRect.bottom - frameBounds.top),
		1
	};

	const DWORD srcExStyle = GetWindowExStyle(hwndSrc);
	// WS_EX_APPWINDOW 样式使窗口始终在 Alt+Tab 列表中显示
	if (srcExStyle & WS_EX_APPWINDOW) {
		return _TryCreateGraphicsCaptureItem(interop);
	}

	const bool isSrcKirikiri = IsKirikiriWindow(hwndSrc);
	if (isSrcKirikiri) {
		Logger::Get().Info("源窗口有零尺寸的所有者窗口");
	} else {
		// 第一次尝试捕获。Kirikiri 窗口必定失败，无需尝试
		if (_TryCreateGraphicsCaptureItem(interop)) {
			return true;
		}
	}

	// 添加 WS_EX_APPWINDOW 样式
	if (!SetWindowLongPtr(hwndSrc, GWL_EXSTYLE, srcExStyle | WS_EX_APPWINDOW)) {
		Logger::Get().Win32Error("SetWindowLongPtr 失败");
		return false;
	}

	Logger::Get().Info("已改变源窗口样式");
	_isSrcStyleChanged = true;

	// Kirikiri 窗口改变样式后所有者窗口和游戏窗口将同时出现在 Alt+Tab 列表和任务栏中。
	// 虽然所有窗口都会如此，但 Kirikiri 的特殊之处在于两个窗口的图标和标题相同，为了不
	// 引起困惑应隐藏所有者窗口的图标。
	if (isSrcKirikiri) {
		_taskbarList = winrt::try_create_instance<ITaskbarList>(CLSID_TaskbarList);
		if (_taskbarList) {
			HRESULT hr = _taskbarList->HrInit();
			if (SUCCEEDED(hr)) {
				// 修正任务栏图标
				_taskbarList->DeleteTab(GetWindowOwner(hwndSrc));
				_taskbarList->AddTab(hwndSrc);

				// 修正 Alt+Tab 切换顺序
				if (GetForegroundWindow() == hwndSrc) {
					SetForegroundWindow(GetDesktopWindow());
					SetForegroundWindow(hwndSrc);
				}
			} else {
				Logger::Get().ComError("ITaskbarList::HrInit 失败", hr);
				_taskbarList = nullptr;
			}
		} else {
			Logger::Get().Error("创建 ITaskbarList 失败");
		}
	}

	// 再次尝试捕获
	if (_TryCreateGraphicsCaptureItem(interop)) {
		return true;
	} else {
		if (_isSrcStyleChanged) {
			// 恢复源窗口样式
			SetWindowLongPtr(hwndSrc, GWL_EXSTYLE, srcExStyle);
		}
		return false;
	}
}

bool GraphicsCaptureFrameSource::_TryCreateGraphicsCaptureItem(IGraphicsCaptureItemInterop* interop) noexcept {
	try {
		HRESULT hr = interop->CreateForWindow(
			ScalingWindow::Get().SrcTracker().Handle(),
			winrt::guid_of<winrt::GraphicsCaptureItem>(),
			winrt::put_abi(_captureItem)
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("创建 GraphicsCaptureItem 失败", hr);
			return false;
		}
	} catch (const winrt::hresult_error& e) {
		Logger::Get().Info(StrHelper::Concat("源窗口无法使用窗口捕获: ", StrHelper::UTF16ToUTF8(e.message())));
		return false;
	}

	return true;
}

bool GraphicsCaptureFrameSource::_StartCapture() noexcept {
	if (_captureSession) {
		return true;
	}

	try {
		// HDR desktops store SDR windows as scRGB. 8-bit capture clamps that
		// and the picture comes back lifted. Float keeps the real values.
		winrt::DirectXPixelFormat captureFormat =
			winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
		const HWND srcHwnd = ScalingWindow::Get().SrcTracker().Handle();
		if (IsMonitorHdr(*_deviceResources, srcHwnd) && _EnsureScRgbConversion()) {
			captureFormat = winrt::DirectXPixelFormat::R16G16B16A16Float;
			_hdrScRgbCapture = true;
			_invSdrWhiteScale = 1.0f / QuerySdrWhiteScale(srcHwnd);
			Logger::Get().Info(fmt::format(
				"Graphics Capture HDR: scRGB float, SDR white {:.0f} nits, converting to sRGB",
				80.0f / _invSdrWhiteScale));
		}
		auto createPool = [&](winrt::DirectXPixelFormat format) {
			return winrt::Direct3D11CaptureFramePool::Create(
				_wrappedD3DDevice,
				format,
				4,
				{ (int)_frameBox.right, (int)_frameBox.bottom }
			);
		};
		try {
			_captureFramePool = createPool(captureFormat);
		} catch (const winrt::hresult_error& e) {
			if (!_hdrScRgbCapture) {
				throw;
			}
			Logger::Get().Info(StrHelper::Concat(
				"scRGB capture unavailable, using 8-bit: ",
				StrHelper::UTF16ToUTF8(e.message())));
			_hdrScRgbCapture = false;
			_captureFramePool = createPool(winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized);
		}

		// 注册回调是为了确保每当有新的帧时会向当前线程发送消息，回调中什么也不做
		_captureFramePool.FrameArrived([](const auto&, const auto&) {});

		_captureSession = _captureFramePool.CreateCaptureSession(_captureItem);

		// 禁止捕获光标。从 Win10 v2004 开始支持
		if (winrt::ApiInformation::IsPropertyPresent(
			winrt::name_of<winrt::GraphicsCaptureSession>(),
			L"IsCursorCaptureEnabled"
		)) {
			_captureSession.IsCursorCaptureEnabled(false);
		}

		// 不显示黄色边框，Win32 应用中无需请求权限。从 Win11 开始支持
		if (winrt::ApiInformation::IsPropertyPresent(
			winrt::name_of<winrt::GraphicsCaptureSession>(),
			L"IsBorderRequired"
		)) {
			_captureSession.IsBorderRequired(false);
		}

		// Win11 24H2 中必须设置 MinUpdateInterval 才能使捕获帧率超过 60FPS
		if (winrt::ApiInformation::IsPropertyPresent(
			winrt::name_of<winrt::GraphicsCaptureSession>(),
			L"MinUpdateInterval"
		)) {
			_captureSession.MinUpdateInterval(1ms);
		}

		_captureSession.StartCapture();
	} catch (const winrt::hresult_error& e) {
		Logger::Get().Info(StrHelper::Concat("Graphics Capture 失败: ", StrHelper::UTF16ToUTF8(e.message())));
		return false;
	}

	return true;
}

bool GraphicsCaptureFrameSource::_EnsureScRgbConversion() noexcept {
	if (_scRgbPs) {
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

cbuffer Params : register(b0) {
	uint offsetX;
	uint offsetY;
	float invWhiteScale;
	float pad;
};

Texture2D<float4> source : register(t0);

float3 LinearToSrgb(float3 c) {
	c = saturate(c);
	float3 low = c * 12.92;
	float3 high = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
	return lerp(high, low, step(c, 0.0031308));
}

float4 PSMain(float4 pos : SV_Position) : SV_Target {
	float3 scrgb = source.Load(int3(int(pos.x) + int(offsetX), int(pos.y) + int(offsetY), 0)).rgb;
	return float4(LinearToSrgb(max(scrgb, 0) * invWhiteScale), 1);
}
)";

	auto compile = [](const char* target, const char* entry, ID3DBlob** blob) {
		winrt::com_ptr<ID3DBlob> errors;
		const HRESULT hr = D3DCompile(
			kHlsl, sizeof(kHlsl) - 1, "ScRgbToSrgb.hlsl", nullptr, nullptr,
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
		vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, _scRgbVs.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateVertexShader failed", hr);
		return false;
	}
	hr = device->CreatePixelShader(
		psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, _scRgbPs.put());
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
	hr = device->CreateBuffer(&cbDesc, nullptr, _scRgbCb.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateBuffer failed", hr);
		return false;
	}

	D3D11_RASTERIZER_DESC rasterDesc{
		.FillMode = D3D11_FILL_SOLID,
		.CullMode = D3D11_CULL_NONE,
		.DepthClipEnable = TRUE
	};
	hr = device->CreateRasterizerState(&rasterDesc, _scRgbRasterizer.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateRasterizerState failed", hr);
		return false;
	}
	return true;
}

void GraphicsCaptureFrameSource::_ConvertScRgbToSrgb(ID3D11Texture2D* source) noexcept {
	if (!_scRgbPs || !_outputRtv || !source) {
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	if (_sdrWhiteQueryTime.time_since_epoch().count() == 0 ||
		now - _sdrWhiteQueryTime >= std::chrono::milliseconds(500)) {
		_sdrWhiteQueryTime = now;
		const float scale = QuerySdrWhiteScale(ScalingWindow::Get().SrcTracker().Handle());
		if (scale > 0.0f) {
			_invSdrWhiteScale = 1.0f / scale;
		}
	}

	ID3D11Device* device = _deviceResources->GetD3DDevice();
	winrt::com_ptr<ID3D11ShaderResourceView> srv;
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{
		.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipLevels = 1 }
	};
	if (FAILED(device->CreateShaderResourceView(source, &srvDesc, srv.put()))) {
		return;
	}

	struct Params {
		UINT offsetX;
		UINT offsetY;
		float invWhiteScale;
		float pad;
	} params{
		.offsetX = _frameBox.left,
		.offsetY = _frameBox.top,
		.invWhiteScale = _invSdrWhiteScale
	};

	ID3D11DeviceContext4* dc = _deviceResources->GetD3DDC();
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (SUCCEEDED(dc->Map(_scRgbCb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		*static_cast<Params*>(mapped.pData) = params;
		dc->Unmap(_scRgbCb.get(), 0);
	}

	const D3D11_VIEWPORT viewport{
		.Width = float(_frameBox.right - _frameBox.left),
		.Height = float(_frameBox.bottom - _frameBox.top),
		.MaxDepth = 1.0f
	};
	dc->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	ID3D11RenderTargetView* rtv = _outputRtv.get();
	dc->OMSetRenderTargets(1, &rtv, nullptr);
	dc->RSSetViewports(1, &viewport);
	dc->RSSetState(_scRgbRasterizer.get());
	dc->IASetInputLayout(nullptr);
	dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	dc->VSSetShader(_scRgbVs.get(), nullptr, 0);
	dc->PSSetShader(_scRgbPs.get(), nullptr, 0);
	ID3D11Buffer* cb = _scRgbCb.get();
	dc->PSSetConstantBuffers(0, 1, &cb);
	ID3D11ShaderResourceView* bound = srv.get();
	dc->PSSetShaderResources(0, 1, &bound);
	dc->Draw(3, 0);

	ID3D11ShaderResourceView* empty = nullptr;
	dc->PSSetShaderResources(0, 1, &empty);
	dc->OMSetRenderTargets(0, nullptr, nullptr);
}

void GraphicsCaptureFrameSource::_StopCapture() noexcept {
	if (_captureSession) {
		_captureSession.Close();
		_captureSession = nullptr;
	}
	if (_captureFramePool) {
		_captureFramePool.Close();
		_captureFramePool = nullptr;
	}
}

GraphicsCaptureFrameSource::~GraphicsCaptureFrameSource() {
	_StopCapture();

	const HWND hwndSrc = ScalingWindow::Get().SrcTracker().Handle();

	// 还原源窗口样式
	if (_isSrcStyleChanged) {
		const DWORD srcExStyle = GetWindowExStyle(hwndSrc);
		SetWindowLongPtr(hwndSrc, GWL_EXSTYLE, srcExStyle & ~WS_EX_APPWINDOW);
	}

	// 还原 Kirikiri 窗口
	if (_taskbarList) {
		_taskbarList->DeleteTab(hwndSrc);
		_taskbarList->AddTab(GetWindowOwner(hwndSrc));

		// 修正任务栏焦点窗口和 Alt+Tab 切换顺序
		if (GetForegroundWindow() == hwndSrc) {
			SetForegroundWindow(GetDesktopWindow());
			SetForegroundWindow(hwndSrc);
		}
	}
}

}
