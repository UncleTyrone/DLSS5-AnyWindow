#pragma once
#include "EffectParametersViewModel.g.h"
#include "ScalingModeBoolParameter.g.h"
#include "ScalingModeFloatParameter.g.h"
#include <parallel_hashmap/phmap.h>

namespace Magpie {
struct EffectInfo;
}

namespace winrt::Magpie::implementation {

struct ScalingModeBoolParameter : ScalingModeBoolParameterT<ScalingModeBoolParameter>,
                                  wil::notify_property_changed_base<ScalingModeBoolParameter> {
	ScalingModeBoolParameter(uint32_t index, const hstring& label, bool initValue)
		: _index(index), _label(box_value(label)), _value(initValue) {
	}
	uint32_t Index() const noexcept {
		return _index;
	}

	bool Value() const noexcept {
		return _value;
	}

	void Value(bool value) {
		_value = value;
		RaisePropertyChanged(L"Value");
	}

	IInspectable Label() const noexcept {
		return _label;
	}

	bool IsEnabled() const noexcept {
		return _isEnabled;
	}

	void IsEnabled(bool value) {
		if (_isEnabled == value) {
			return;
		}
		_isEnabled = value;
		RaisePropertyChanged(L"IsEnabled");
	}

private:
	const uint32_t _index;
	IInspectable _label;
	bool _value;
	bool _isEnabled = true;
};

struct ScalingModeFloatParameter : ScalingModeFloatParameterT<ScalingModeFloatParameter>,
                                   wil::notify_property_changed_base<ScalingModeFloatParameter> {
	ScalingModeFloatParameter(uint32_t index, const hstring& label, float initValue, float minimum, float maximum, float step)
		: _index(index), _label(label), _minimum(minimum), _maximum(maximum), _step(step), _value(initValue) {
	}

	uint32_t Index() const noexcept {
		return _index;
	}

	double Value() const noexcept {
		return _value;
	}

	void Value(double value) {
		_value = value;
		RaisePropertyChanged(L"Value");
		RaisePropertyChanged(L"ValueText");
	}

	hstring ValueText() const noexcept;

	hstring Label() const noexcept {
		return _label;
	}

	double Minimum() const noexcept {
		return _minimum;
	}

	double Maximum() const noexcept {
		return _maximum;
	}

	double Step() const noexcept {
		return _step;
	}

	void Maximum(double value) {
		if (_maximum == value) {
			return;
		}
		_maximum = value;
		RaisePropertyChanged(L"Maximum");
	}

	bool IsEnabled() const noexcept {
		return _isEnabled;
	}

	void IsEnabled(bool value) {
		if (_isEnabled == value) {
			return;
		}
		_isEnabled = value;
		RaisePropertyChanged(L"IsEnabled");
		RaisePropertyChanged(L"Opacity");
	}

	double Opacity() const noexcept {
		return _isEnabled ? 1.0 : 0.4;
	}

private:
	const uint32_t _index;
	const hstring _label;
	const double _minimum;
	double _maximum;
	const double _step;
	double _value;
	bool _isEnabled = true;
};

struct EffectParametersViewModel : EffectParametersViewModelT<EffectParametersViewModel> {
	EffectParametersViewModel(uint32_t scalingModeIdx, uint32_t effectIdx);

	uint32_t ScalingModeIdx() const noexcept {
		return _scalingModeIdx;
	}

	void ScalingModeIdx(uint32_t value) noexcept {
		_scalingModeIdx = value;
	}

	uint32_t EffectIdx() const noexcept {
		return _effectIdx;
	}

	void EffectIdx(uint32_t value) noexcept {
		_effectIdx = value;
	}

	IVector<IInspectable> BoolParams() const noexcept {
		return _boolParams;
	}

	IVector<IInspectable> FloatParams() const noexcept {
		return _floatParams;
	}

	bool HasBoolParams() const noexcept {
		return _boolParams != nullptr;
	}

	bool HasFloatParams() const noexcept {
		return _floatParams != nullptr;
	}

	IVector<IInspectable> FilterBoolParams() const noexcept {
		return _filterBoolParams;
	}

	IVector<IInspectable> FilterFloatParams() const noexcept {
		return _filterFloatParams;
	}

	IVector<IInspectable> Pass1Params() const noexcept {
		return _pass1Params;
	}

	IVector<IInspectable> Pass2Params() const noexcept {
		return _pass2Params;
	}

	IVector<IInspectable> Pass3Params() const noexcept {
		return _pass3Params;
	}

	IVector<IInspectable> Pass4Params() const noexcept {
		return _pass4Params;
	}

	IVector<IInspectable> GuidanceParams() const noexcept {
		return _guidanceParams;
	}

	bool HasFilterParams() const noexcept {
		return _filterFloatParams != nullptr || _filterBoolParams != nullptr;
	}

	bool HasPass1Params() const noexcept {
		return _pass1Params != nullptr;
	}

	bool HasPass2Params() const noexcept {
		return _pass2Params != nullptr;
	}

	bool HasPass3Params() const noexcept {
		return _pass3Params != nullptr;
	}

	bool HasPass4Params() const noexcept {
		return _pass4Params != nullptr;
	}

	bool HasGuidanceParams() const noexcept {
		return _guidanceParams != nullptr;
	}

private:
	bool _IsRemoved() const noexcept;

	void _ScalingModeBoolParameter_PropertyChanged(IInspectable const& sender, PropertyChangedEventArgs const& args);

	void _ScalingModeFloatParameter_PropertyChanged(IInspectable const& sender, PropertyChangedEventArgs const& args);

	void _ApplyDlssFgParameterState() noexcept;

	phmap::flat_hash_map<std::wstring, float>& _Data() const;

	IVector<IInspectable> _boolParams{ nullptr };
	IVector<IInspectable> _floatParams{ nullptr };
	IVector<IInspectable> _filterBoolParams{ nullptr };
	IVector<IInspectable> _filterFloatParams{ nullptr };
	IVector<IInspectable> _pass1Params{ nullptr };
	IVector<IInspectable> _pass2Params{ nullptr };
	IVector<IInspectable> _pass3Params{ nullptr };
	IVector<IInspectable> _pass4Params{ nullptr };
	IVector<IInspectable> _guidanceParams{ nullptr };

	uint32_t _scalingModeIdx;
	uint32_t _effectIdx;
	const ::Magpie::EffectInfo* _effectInfo = nullptr;
};

}
