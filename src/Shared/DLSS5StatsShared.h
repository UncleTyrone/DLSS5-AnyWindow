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

enum DLSS5RuntimeKind : LONG {
	DLSS5RuntimeUnknown = 0,
	DLSS5RuntimeShortFuseFp16 = 1,
	DLSS5RuntimeRtx40Patched = 2,
	DLSS5RuntimeRtx50Original = 3,
	DLSS5RuntimeCustom = 4,
	DLSS5RuntimeLegacyRoot = 5,
	DLSS5RuntimeRtx30Patched = 6
};

struct DLSS5StatsShared {
	DWORD magic = DLSS5_STATS_MAGIC;
	DWORD version = 3;
	volatile LONG fps = 0;
	volatile LONG featureState = DLSS5FeaturePending;
	volatile LONG evaluateSuccessCount = 0;
	volatile LONG evaluateFailureCount = 0;
	volatile LONG configuredPasses = 1;
	volatile LONG runtimeKind = 0;
	volatile LONG cudaComputeMajor = 0;
	volatile LONG cudaComputeMinor = 0;
};

}
