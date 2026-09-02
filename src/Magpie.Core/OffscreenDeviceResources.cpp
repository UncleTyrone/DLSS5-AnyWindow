#include "pch.h"
#include "DeviceResources.h"
#include "Logger.h"

namespace Magpie {

bool DeviceResources::InitializeOffscreen() noexcept {
	HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(_dxgiFactory.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create offscreen DXGI factory failed", hr);
		return false;
	}

	for (UINT index = 0;; ++index) {
		winrt::com_ptr<IDXGIAdapter1> adapter;
		if (FAILED(_dxgiFactory->EnumAdapters1(index, adapter.put()))) break;
		DXGI_ADAPTER_DESC1 desc{};
		if (FAILED(adapter->GetDesc1(&desc)) ||
			(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
			desc.VendorId != 0x10de) {
			continue;
		}

		constexpr D3D_FEATURE_LEVEL levels[]{
			D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
		};
		winrt::com_ptr<ID3D11Device> device;
		winrt::com_ptr<ID3D11DeviceContext> context;
		D3D_FEATURE_LEVEL selectedLevel{};
		hr = D3D11CreateDevice(
			adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED,
			levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
			device.put(), &selectedLevel, context.put());
		if (FAILED(hr)) continue;

		_d3dDevice = device.try_as<ID3D11Device5>();
		_d3dDC = context.try_as<ID3D11DeviceContext4>();
		_graphicsAdapter = adapter.try_as<IDXGIAdapter4>();
		if (!_d3dDevice || !_d3dDC || !_graphicsAdapter) {
			_d3dDevice = nullptr;
			_d3dDC = nullptr;
			_graphicsAdapter = nullptr;
			continue;
		}

		D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT precision{};
		if (SUCCEEDED(device->CheckFeatureSupport(
			D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT,
			&precision, sizeof(precision)))) {
			_isFP16Supported =
				(precision.AllOtherShaderStagesMinPrecision &
				 D3D11_SHADER_MIN_PRECISION_16_BIT) != 0;
		}
		Logger::Get().Info(fmt::format(
			"Created offscreen D3D device on NVIDIA adapter {} vendor={:#x} device={:#x}",
			index, desc.VendorId, desc.DeviceId));
		return true;
	}

	Logger::Get().Error(
		"No NVIDIA hardware FL11 adapter is available for offscreen DLSS processing");
	return false;
}

}
