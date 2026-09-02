#pragma once

#include <windows.h>

namespace Magpie {

constexpr DWORD DLSS5_STATS_MAGIC = 0x35534C44; // "DLS5"

enum DLSS5FeatureState : LONG {
	DLSS5FeatureFailed = -1,
	DLSS5FeaturePending = 0,
	DLSS5FeatureCreated = 1,
	DLSS5FeatureEvaluating = 2
};

struct DLSS5StatsShared {
	DWORD magic = DLSS5_STATS_MAGIC;
	DWORD version = 2;
	volatile LONG fps = 0;
	volatile LONG featureState = DLSS5FeaturePending;
	volatile LONG evaluateSuccessCount = 0;
	volatile LONG evaluateFailureCount = 0;
	volatile LONG configuredPasses = 1;
};

}
