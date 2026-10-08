#pragma once
#include "BackendDescriptorStore.h"
#include <atomic>
#include "CursorDrawer.h"
#include "DeviceResources.h"
#include "EffectDrawer.h"
#include "EffectsProfiler.h"
#include "FrameGuidanceService.h"
#include "OverlayDrawer.h"
#include "PresenterBase.h"
#include "StepTimer.h"

namespace Magpie {

class FrameSourceBase;

class Renderer {
public:
	Renderer() noexcept;
	~Renderer() noexcept;

	Renderer(const Renderer&) = delete;
	Renderer(Renderer&&) = delete;

	ScalingError Initialize(HWND hwndAttach, OverlayOptions& overlayOptions) noexcept;

	bool Render(bool force = false, bool waitForGpu = false) noexcept;

	bool OnResize() noexcept;

	void OnEndResize() noexcept;

	void OnMove() noexcept;

	void SwitchToolbarState() noexcept;

	const RECT& SrcRect() const noexcept;

	// 屏幕坐标而不是窗口局部坐标
	const RECT& DestRect() const noexcept {
		return _destRect;
	}

	const FrameSourceBase& FrameSource() const noexcept {
		return *_frameSource;
	}

	uint32_t FPS() const noexcept {
		const uint32_t presentedFps = _overlayFps.load(std::memory_order_relaxed);
		return presentedFps ? presentedFps : _stepTimer.FPS();
	}

	void OnCursorVisibilityChanged(bool isVisible, bool onDestory);

	void OnSourceFocusChanged() noexcept;

	void MessageHandler(UINT msg, WPARAM wParam, LPARAM lParam) noexcept;

	const std::vector<const EffectDesc*>& ActiveEffectDescs() const noexcept {
		return _activeEffectDescs;
	}

	void StartProfile() noexcept;

	void StopProfile() noexcept;

	bool IsCursorOnOverlayCaptionArea() const noexcept {
		return _overlayDrawer.IsCursorOnCaptionArea();
	}

	winrt::fire_and_forget TakeScreenshot(
		uint32_t effectIdx,
		uint32_t passIdx = std::numeric_limits<uint32_t>::max(),
		uint32_t outputIdx = std::numeric_limits<uint32_t>::max()
	) noexcept;

private:
	bool _AcquireFrontendSharedTexture() noexcept;
	void _ReleaseFrontendSharedTexture() noexcept;
	bool _OpenFrontendSharedTextures(HANDLE sharedHandle) noexcept;
	bool _ConsumeDoubleBufferedFrame() noexcept;
	void _FrontendRender(bool waitForGpu = false) noexcept;

	void _BackendThreadProc() noexcept;

	HANDLE _InitBackend() noexcept;

	bool _InitFrameSource() noexcept;

	ID3D11Texture2D* _BuildEffects() noexcept;

	void _UpdateActiveEffectDescs() noexcept;

	bool _ShouldAppendBicubic(ID3D11Texture2D* outTexture) noexcept;

	bool _AppendBicubic(ID3D11Texture2D** inOutTexture) noexcept;

	ID3D11Texture2D* _ResizeEffects() noexcept;

	void _UpdateDestRect() noexcept;

	HANDLE _CreateSharedTexture(ID3D11Texture2D* effectsOutput) noexcept;

	void _BackendRender(
		ID3D11Texture2D* effectsOutput,
		bool isNewCaptureFrame
	) noexcept;

	uint32_t _NextDLSSFrameGenerationCount() noexcept;

	bool _PublishBackendTexture(ID3D11Texture2D* texture, bool synchronous) noexcept;

	void _BeginDLSSFgGpuTiming() noexcept;
	void _MarkDLSSFgGpuTiming(uint32_t idx) noexcept;
	void _EndDLSSFgGpuTiming(bool valid) noexcept;

	bool _InitializeDLSSFrameGenerator(
		ID3D11Texture2D* input,
		const struct DLSSFrameGenerationSettings& settings
	) noexcept;
	void _HandleDLSSFrameGenerationFailure(ID3D11Texture2D* input) noexcept;
	void _DisableDLSSFrameGenerationForSession() noexcept;

	bool _UpdateDynamicConstants() const noexcept;

	winrt::IAsyncAction _UpdateNextScreenshotNum(const wchar_t* imgFormat) noexcept;

	winrt::IAsyncOperation<bool> _TakeScreenshotImpl(
		uint32_t effectIdx,
		uint32_t passIdx,
		uint32_t outputIdx
	) noexcept;

	static LRESULT CALLBACK _LowLevelKeyboardHook(int nCode, WPARAM wParam, LPARAM lParam);

	// 只能由前台线程访问
	DeviceResources _frontendResources;
	std::unique_ptr<PresenterBase> _presenter;
	
	CursorDrawer _cursorDrawer;
	OverlayDrawer _overlayDrawer;

	winrt::com_ptr<ID3D11Texture2D> _frontendSharedTexture;
	winrt::com_ptr<IDXGIKeyedMutex> _frontendSharedTextureMutex;
	// Second handoff slot used by DLSSFG for even publish sequence numbers.
	winrt::com_ptr<ID3D11Texture2D> _frontendSharedTextureAlt;
	winrt::com_ptr<IDXGIKeyedMutex> _frontendSharedTextureMutexAlt;
	// DLSSFG copies the shared texture here before the frame-latency wait so
	// the backend can publish the next frame while this one waits for scanout.
	winrt::com_ptr<ID3D11Texture2D> _frontendStagingTexture;
	uint64_t _lastAccessMutexKey = 0;
	bool _isDLSSFrameGenerationActive = false;
	RECT _destRect{};
	
	std::thread _backendThread;

	wil::unique_hhook _hKeyboardHook;
	
	// 只能由后台线程访问
	DeviceResources _backendResources;
	Magpie::BackendDescriptorStore _backendDescriptorStore;
	std::unique_ptr<FrameSourceBase> _frameSource;
	FrameGuidanceService _frameGuidanceService;
	FrameGuidanceFrameId _capturedFrameId = 0;
	std::vector<EffectDrawer> _effectDrawers;
	std::vector<std::unique_ptr<class NativeEffectBackend>> _nativeEffectBackends;
	std::unique_ptr<class DLSSFrameGenerator> _dlssFrameGenerator;
	uint32_t _dlssFgConsecutiveFailures = 0;
	uint32_t _dlssFgRecoveryAttempts = 0;
	float _dlssFgRefreshHz = 0.0f;
	std::chrono::steady_clock::time_point _dlssFgRateWindowStart{};
	uint32_t _dlssFgRateWindowFrames = 0;
	double _dlssFgSourceFps = 0.0;
	// Generated frames requested per real frame; -1 until initialized.
	double _dlssFgGenerated = -1.0;
	// Frames of present credit, refilled at refresh-1 Hz.
	double _dlssFgPresentBudget = 2.0;
	std::chrono::steady_clock::time_point _dlssFgBudgetTime{};

	StepTimer _stepTimer;
	EffectsProfiler _effectsProfiler;

	winrt::com_ptr<ID3D11Fence> _d3dFence;
	uint64_t _fenceValue = 0;
	wil::unique_event_nothrow _fenceEvent;

	winrt::com_ptr<ID3D11Texture2D> _backendSharedTexture;
	winrt::com_ptr<IDXGIKeyedMutex> _backendSharedTextureMutex;
	winrt::com_ptr<ID3D11Texture2D> _backendSharedTextureAlt;
	winrt::com_ptr<IDXGIKeyedMutex> _backendSharedTextureMutexAlt;

	winrt::com_ptr<ID3D11Buffer> _dynamicCB;

	uint32_t _screenshotNum = 0;

	// 可由所有线程访问
	// With _doubleBufferedHandoff this is the newest published sequence number
	// and only the backend writes it; slot = sequence & 1, keys alternate 0/1.
	std::atomic<uint64_t> _sharedTextureMutexKey = 0;
	// Set before the backend thread starts; read-only afterwards.
	bool _doubleBufferedHandoff = false;
	std::atomic<bool> _synchronousFramePresentationEnabled = false;
	// Backend key of the newest shared texture the frontend has copied.
	std::atomic<uint64_t> _frontendConsumedKey = 0;
	wil::unique_event_nothrow _frontendConsumedEvent;
	// Backend key posted to the frontend that it may not have copied yet.
	uint64_t _pendingFrontendKey = 0;
	float _frameRateFilterTarget = 0.0f;
	std::chrono::nanoseconds _synchronousPresentInterval{};
	std::chrono::steady_clock::time_point _dlssFgDiagnosticsStart{};
	uint32_t _dlssFgCapturedFrameCount = 0;
	uint32_t _dlssFgPresentedFrameCount = 0;
	std::chrono::nanoseconds _dlssFgGpuWait{};
	std::chrono::nanoseconds _dlssFgFrontendWait{};
	std::chrono::nanoseconds _dlssFgUpdateTime{};
	std::chrono::nanoseconds _dlssFgIdleTime{};
	std::chrono::steady_clock::time_point _dlssFgLastRealFrame{};
	std::chrono::nanoseconds _dlssFgMaxRealGap{};
	// Timestamps: before capture update, after update, after guidance,
	// after effects, after DLSSG input submit, after DLSSG output wait,
	// after generated-frame AcquireSync, after frame generation,
	// after real-frame AcquireSync, after real-frame publish.
	static constexpr uint32_t DLSSFG_GPU_STAMPS = 10;
	struct DLSSFgGpuTimingSlot {
		winrt::com_ptr<ID3D11Query> disjoint;
		std::array<winrt::com_ptr<ID3D11Query>, DLSSFG_GPU_STAMPS> stamps;
		bool pending = false;
		bool valid = false;
	};
	std::array<DLSSFgGpuTimingSlot, 8> _dlssFgGpuSlots;
	uint32_t _dlssFgGpuSlotIdx = 0;
	bool _dlssFgGpuSlotOpen = false;
	std::array<double, DLSSFG_GPU_STAMPS> _dlssFgGpuStageMs{};
	uint32_t _dlssFgGpuSamples = 0;
	uint32_t _dlssFgGpuIssued = 0;
	// Stamp to mark after the next backend AcquireSync, or 0 for none.
	uint32_t _dlssFgAcquireStamp = 0;
	std::array<std::chrono::steady_clock::time_point, DLSSFG_GPU_STAMPS> _dlssFgCpuMarks{};
	std::array<double, DLSSFG_GPU_STAMPS> _dlssFgCpuStageMs{};
	uint32_t _dlssFgCpuSamples = 0;
	std::chrono::steady_clock::time_point _dlssFgGpuLogStart{};
	std::atomic<int64_t> _frontendLatencyWaitNs{ 0 };
	std::atomic<uint32_t> _frontendRenderCount{ 0 };
	std::atomic<uint32_t> _overlayFps{ 0 };
	bool _isXeSSFrameGenerationActive = false;
	bool _xessFgFrontendSuppressionLogged = false;

	// INVALID_HANDLE_VALUE 表示后端初始化失败
	std::atomic<HANDLE> _sharedTextureHandle{ NULL };
	// 下面四个成员由 _sharedTextureHandle 同步
	HANDLE _sharedTextureHandleAlt = NULL;
	winrt::Windows::System::DispatcherQueue _backendThreadDispatcher{ nullptr };
	ScalingError _backendInitError = ScalingError::NoError;
	std::vector<EffectDesc> _effectDescs;
	// 包含追加的 Bicubic
	std::vector<const EffectDesc*> _activeEffectDescs;
};

}
