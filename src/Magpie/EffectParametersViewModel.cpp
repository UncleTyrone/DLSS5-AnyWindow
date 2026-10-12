#include "pch.h"
#include "EffectParametersViewModel.h"
#if __has_include("ScalingModeBoolParameter.g.cpp")
#include "ScalingModeBoolParameter.g.cpp"
#endif
#if __has_include("ScalingModeFloatParameter.g.cpp")
#include "ScalingModeFloatParameter.g.cpp"
#endif
#if __has_include("EffectParametersViewModel.g.cpp")
#include "EffectParametersViewModel.g.cpp"
#endif
#include "StrHelper.h"
#include "AppSettings.h"
#include "ScalingModesService.h"
#include "ScalingMode.h"
#include "EffectsService.h"
#include "EffectDesc.h"
#include "App.h"

using namespace Magpie;

namespace winrt::Magpie::implementation {

// 限制保存频率
// 1 秒内没有新的调用才执行保存
static fire_and_forget LazySaveAppSettings() {
	using namespace std::chrono;

	static steady_clock::time_point lastInvokeTime;
	static bool sleeping = false;

	lastInvokeTime = steady_clock::now();

	if (sleeping) {
		co_return;
	}

	sleeping = true;
	
	co_await 1s;

	while (true) {
		int duration = (int)duration_cast<milliseconds>(steady_clock::now() - lastInvokeTime).count();
		if (duration >= 999) {
			break;
		}

		// 如果与 lastInvokeTime 相差不到 1s，则继续等待
		co_await milliseconds(1000 - duration);
	}

	// 回到主线程
	co_await App::Get().Dispatcher();

	sleeping = false;
	AppSettings::Get().SaveAsync();
}

EffectParametersViewModel::EffectParametersViewModel(uint32_t scalingModeIdx, uint32_t effectIdx)
	: _scalingModeIdx(scalingModeIdx), _effectIdx(effectIdx)
{
	ScalingMode& scalingMode = ScalingModesService::Get().GetScalingMode(_scalingModeIdx);
	_effectInfo = EffectsService::Get().GetEffect(scalingMode.effects[_effectIdx].name);

	phmap::flat_hash_map<std::wstring, float>& params = _Data();

	std::vector<IInspectable> boolParams;
	std::vector<IInspectable> floatParams;
	std::vector<IInspectable> filterBoolParams;
	std::vector<IInspectable> filterFloatParams;
	std::vector<IInspectable> pass1Params;
	std::vector<IInspectable> pass2Params;
	std::vector<IInspectable> pass3Params;
	std::vector<IInspectable> pass4Params;
	std::vector<IInspectable> guidanceParams;
	const bool groupDlssnr = _effectInfo->name.find(L"DLSSNR") != std::wstring::npos;
	for (uint32_t i = 0, size = (uint32_t)_effectInfo->params.size(); i < size; ++i) {
		const EffectParameterDesc& param = _effectInfo->params[i];
		std::vector<IInspectable>* floatDestination = &floatParams;
		std::vector<IInspectable>* boolDestination = &boolParams;
		if (groupDlssnr) {
			if (param.name.rfind("pass2", 0) == 0) {
				floatDestination = &pass2Params;
			} else if (param.name.rfind("pass3", 0) == 0) {
				floatDestination = &pass3Params;
			} else if (param.name.rfind("pass4", 0) == 0) {
				floatDestination = &pass4Params;
			} else if (param.name == "style" || param.name == "intensity" ||
				param.name == "localToneStrength" || param.name == "localStructureStrength" ||
				param.name == "skinStructureStrength" || param.name == "renderPreset") {
				floatDestination = &pass1Params;
			} else if (param.name == "guidanceMode" || param.name == "depthInferenceInterval" ||
				param.name == "historyMode") {
				floatDestination = &guidanceParams;
			} else if (param.name == "processingResolutionPercent" || param.name == "passes" ||
				param.name == "passFeedback" || param.name == "vitReuseInterval") {
				floatDestination = &filterFloatParams;
			} else if (param.name == "useAutoMask" || param.name == "detailReuse" ||
				param.name == "modelPrecision") {
				boolDestination = &filterBoolParams;
			}
		}

		std::optional<float> paramValue;
		{
			auto it = params.find(StrHelper::UTF8ToUTF16(param.name));
			if (it != params.end()) {
				paramValue = it->second;
			}
		}
		
		if (param.constant.index() == 0) {
			const EffectConstant<float>& constant = std::get<0>(param.constant);
			auto floatParamItem = make_self<ScalingModeFloatParameter>(
				i,
				hstring(StrHelper::UTF8ToUTF16(param.label.empty() ? param.name : param.label)),
				paramValue.has_value() ? *paramValue : constant.defaultValue,
				constant.minValue,
				constant.maxValue,
				constant.step
			);
			floatParamItem->PropertyChanged({ this, &EffectParametersViewModel::_ScalingModeFloatParameter_PropertyChanged });
			floatDestination->push_back(*floatParamItem);
		} else {
			const EffectConstant<int>& constant = std::get<1>(param.constant);
			if (constant.minValue == 0 && constant.maxValue == 1 && constant.step == 1) {
				auto boolParamItem = make_self<ScalingModeBoolParameter>(
					i,
					hstring(StrHelper::UTF8ToUTF16(param.label.empty() ? param.name : param.label)),
					paramValue.has_value() ? std::abs(*paramValue) > FLOAT_EPSILON<float> : (bool)constant.defaultValue
				);
				boolParamItem->PropertyChanged({ this, &EffectParametersViewModel::_ScalingModeBoolParameter_PropertyChanged });
				boolDestination->push_back(*boolParamItem);
			} else {
				auto floatParamItem = make_self<ScalingModeFloatParameter>(
					i,
					hstring(StrHelper::UTF8ToUTF16(param.label.empty() ? param.name : param.label)),
					paramValue.has_value() ? *paramValue : (float)constant.defaultValue,
					(float)constant.minValue,
					(float)constant.maxValue,
					(float)constant.step
				);
				floatParamItem->PropertyChanged({ this, &EffectParametersViewModel::_ScalingModeFloatParameter_PropertyChanged });
				floatDestination->push_back(*floatParamItem);
			}
		}
	}
	if (!boolParams.empty()) {
		_boolParams = single_threaded_vector(std::move(boolParams));
	}
	if (!floatParams.empty()) {
		_floatParams = single_threaded_vector(std::move(floatParams));
	}
	if (!filterBoolParams.empty() || !filterFloatParams.empty()) {
		_filterBoolParams = single_threaded_vector(std::move(filterBoolParams));
		_filterFloatParams = single_threaded_vector(std::move(filterFloatParams));
	}
	if (!pass1Params.empty()) {
		_pass1Params = single_threaded_vector(std::move(pass1Params));
	}
	if (!pass2Params.empty()) {
		_pass2Params = single_threaded_vector(std::move(pass2Params));
	}
	if (!pass3Params.empty()) {
		_pass3Params = single_threaded_vector(std::move(pass3Params));
	}
	if (!pass4Params.empty()) {
		_pass4Params = single_threaded_vector(std::move(pass4Params));
	}
	if (!guidanceParams.empty()) {
		_guidanceParams = single_threaded_vector(std::move(guidanceParams));
	}
	_ApplyDlssFgParameterState();
}

// 应确保被删除后依然处于合法的状态，调用任何方法都不会崩溃，见 ScalingModeItem::_IsRemoved
bool EffectParametersViewModel::_IsRemoved() const noexcept {
	return _scalingModeIdx == std::numeric_limits<uint32_t>::max() ||
		_effectIdx == std::numeric_limits<uint32_t>::max();
}

void EffectParametersViewModel::_ScalingModeBoolParameter_PropertyChanged(
	IInspectable const& sender,
	PropertyChangedEventArgs const& args
) {
	if (_IsRemoved() || args.PropertyName() != L"Value") {
		return;
	}

	ScalingModeBoolParameter* boolParamImpl =
		get_self<ScalingModeBoolParameter>(sender.try_as<Magpie::ScalingModeBoolParameter>());
	const std::string& effectName = _effectInfo->params[boolParamImpl->Index()].name;
	_Data()[StrHelper::UTF8ToUTF16(effectName)] = (float)boolParamImpl->Value();

	if (effectName == "dynamicMfg" || effectName == "adaTemporalFix") {
		_ApplyDlssFgParameterState();
	}

	LazySaveAppSettings();
}

void EffectParametersViewModel::_ScalingModeFloatParameter_PropertyChanged(
	IInspectable const& sender,
	PropertyChangedEventArgs const& args
) {
	if (_IsRemoved() || args.PropertyName() != L"Value") {
		return;
	}

	ScalingModeFloatParameter* floatParamImpl =
		get_self<ScalingModeFloatParameter>(sender.try_as<Magpie::ScalingModeFloatParameter>());
	const std::string& effectName = _effectInfo->params[floatParamImpl->Index()].name;
	_Data()[StrHelper::UTF8ToUTF16(effectName)] = (float)floatParamImpl->Value();

	if (effectName == "dynamicMfg" || effectName == "adaTemporalFix") {
		_ApplyDlssFgParameterState();
	}

	LazySaveAppSettings();
}

struct GpuFrameGenerationSupport {
	bool ada = false;
	bool blackwell = false;
};

static GpuFrameGenerationSupport QueryGpuFrameGenerationSupport() noexcept {
	static GpuFrameGenerationSupport cached{};
	static bool ready = false;
	if (ready) {
		return cached;
	}
	ready = true;
	const HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!cuda) {
		return cached;
	}
	using CuInit = int(__stdcall*)(unsigned);
	using CuGet = int(__stdcall*)(int*, int);
	using CuAttr = int(__stdcall*)(int*, int, int);
	const auto cuInit = reinterpret_cast<CuInit>(GetProcAddress(cuda, "cuInit"));
	const auto cuDeviceGet = reinterpret_cast<CuGet>(GetProcAddress(cuda, "cuDeviceGet"));
	const auto cuAttr = reinterpret_cast<CuAttr>(GetProcAddress(cuda, "cuDeviceGetAttribute"));
	int device = 0;
	int major = 0;
	int minor = 0;
	if (cuInit && cuDeviceGet && cuAttr &&
		cuInit(0) == 0 && cuDeviceGet(&device, 0) == 0 &&
		cuAttr(&major, 75, device) == 0 && cuAttr(&minor, 76, device) == 0) {
		cached.ada = major == 8 && minor == 9;
		cached.blackwell = major >= 10;
	}
	FreeLibrary(cuda);
	return cached;
}

void EffectParametersViewModel::_ApplyDlssFgParameterState() noexcept {
	if (_IsRemoved() || !_effectInfo) {
		return;
	}

	ScalingModeFloatParameter* dynamicMfg = nullptr;
	ScalingModeFloatParameter* adaTemporalFix = nullptr;
	ScalingModeFloatParameter* multiplier = nullptr;
	ScalingModeFloatParameter* dynamicTargetFps = nullptr;
	auto consider = [&](IInspectable const& item) {
		if (auto number = item.try_as<Magpie::ScalingModeFloatParameter>()) {
			ScalingModeFloatParameter* impl = get_self<ScalingModeFloatParameter>(number);
			const std::string& name = _effectInfo->params[impl->Index()].name;
			if (name == "dynamicMfg") {
				dynamicMfg = impl;
			} else if (name == "adaTemporalFix") {
				adaTemporalFix = impl;
			} else if (name == "multiplier") {
				multiplier = impl;
			} else if (name == "dynamicTargetFps") {
				dynamicTargetFps = impl;
			}
		}
	};
	if (_boolParams) {
		for (auto&& item : _boolParams) {
			consider(item);
		}
	}
	if (_floatParams) {
		for (auto&& item : _floatParams) {
			consider(item);
		}
	}

	const GpuFrameGenerationSupport gpu = QueryGpuFrameGenerationSupport();
	if (adaTemporalFix) {
		adaTemporalFix->IsEnabled(gpu.ada);
		if (!gpu.ada && adaTemporalFix->Value() >= 0.5) {
			adaTemporalFix->Value(0.0);
		} else if (gpu.ada && !_Data().contains(L"adaTemporalFix")) {
			adaTemporalFix->Value(1.0);
		}
	}

	// RTX 50 already generates multiple frames. RTX 40 needs the Ada temporal
	// fix before the same dynamic multiplier is safe to turn on.
	const bool adaFixOn = adaTemporalFix && adaTemporalFix->Value() >= 0.5;
	const bool adaMultiFrame = gpu.ada && adaFixOn;
	const bool multiFrameAllowed = gpu.blackwell || adaMultiFrame;
	// 5x and 6x are native on RTX 50. The Ada unlock accepts them only while
	// its temporal fix is on. Every other GPU stops at 4x.
	const double maxMultiplier = (gpu.blackwell || adaMultiFrame) ? 6.0 : 4.0;
	if (multiplier) {
		multiplier->Maximum(maxMultiplier);
		if (multiplier->Value() > maxMultiplier) {
			multiplier->Value(maxMultiplier);
		}
	}
	if (dynamicMfg) {
		dynamicMfg->IsEnabled(multiFrameAllowed);
		if (!multiFrameAllowed && dynamicMfg->Value() >= 0.5) {
			dynamicMfg->Value(0.0);
		}
	}
	const bool multiFrameOn = multiFrameAllowed && dynamicMfg && dynamicMfg->Value() >= 0.5;
	if (multiplier) {
		multiplier->IsEnabled(!multiFrameOn);
	}
	if (dynamicTargetFps) {
		dynamicTargetFps->IsEnabled(multiFrameOn);
	}
}

phmap::flat_hash_map<std::wstring, float>& EffectParametersViewModel::_Data() const {
	ScalingMode& scalingMode = ScalingModesService::Get().GetScalingMode(_scalingModeIdx);
	return scalingMode.effects[_effectIdx].parameters;
}

hstring ScalingModeFloatParameter::ValueText() const noexcept {
	return App::DoubleFormatter().FormatDouble(_value);
}

}
