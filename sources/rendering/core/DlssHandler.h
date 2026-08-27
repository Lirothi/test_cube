#pragma once

#include <d3d12.h>
#include <algorithm>
#include <cstdint>
#include <wrl/client.h>

#include "streamline/include/sl.h"
#include "streamline/include/sl_core_types.h"
#include "streamline/include/sl_dlss.h"

#include "core/math/Math.h"

class Camera;
class Renderer;

class DlssHandler {
public:
    explicit DlssHandler(Renderer& renderer);

    void Shutdown();

    void OnStreamlineInitialized(sl::Result initResult);
    void OnBeginFrame();
    void OnDisplaySizeChanged();
    void OnRenderResolutionScaleChanged();
    void UpdateSettings();
    void AllocateResourcesIfNeeded();
    void UpdateCameraData(const Camera& camera);
    bool Evaluate(ID3D12GraphicsCommandList* cl);
    void RefreshRenderResolution();

    Math::float2 GetCurrentJitterPixels() const { return jitterPixels_; }

    // Debug: hold the sub-pixel jitter at zero. Every render-resolution target is re-sampled at a
    // different sub-pixel offset each frame, which is exactly what makes a half-res intermediate
    // (GTAO, SSR) shimmer when you sit and stare at it in the texture inspector. Pausing freezes
    // the sample grid so the target can actually be read.
    //
    // Both sides are held together — the projection AND the jitterOffset handed to DLSS — because
    // zeroing only one would tell DLSS to reconstruct against an offset the frame was not rendered
    // with. DLSS itself NEEDS the jitter, so with this on its output loses its anti-aliasing and
    // goes crawly; that is the honest cost of the switch and it is stated in the UI.
    void SetJitterPaused(bool paused) { jitterPaused_ = paused; }
    bool IsJitterPaused() const { return jitterPaused_; }

    bool IsActive() const;
    void SetActive(bool active);
    bool IsAvailable() const { return available_; }
    bool ShouldUseUpscaledOutput() const { return IsActive() && outputValid_; }
    void InvalidateOutput() { outputValid_ = false; }

    // DLSS-split: will Evaluate() do its work this frame? Asked BEFORE recording, by the
    // Main_DLSS pass builder, so that Main_Tonemap can be recorded CONCURRENTLY with the evaluate
    // instead of waiting to be told what happened inside it.
    //
    // Everything it reads is settled before the graph is built: `active_` is a UI/mode setting,
    // `frameToken_` comes from OnBeginFrame, and the four tagged targets belong to this frame's
    // Deferred set. The one thing it CANNOT foresee is Streamline itself failing mid-record —
    // four of Evaluate's exits are `sl*` calls. That is what `evaluateFailed_` is for: a failure
    // is remembered, so the frame AFTER it predicts false and the pipeline is back on the
    // scene-colour path. The frame that failed shows the previous DLSS output (the tonemap reads
    // the target it was promised); one stale frame on a should-never-happen path is the price of
    // recording the two passes in parallel, and it is deliberate.
    bool WillEvaluate() const;

private:
    bool EvaluateInternal(ID3D12GraphicsCommandList* cl);
    void ClearResourceTags();
    void HandleAllocationFailure();
    void ResetJitterSequence();
    Math::float2 GenerateJitterSample();
    void EnsureExposureResources(ID3D12GraphicsCommandList* cl);

private:
    Renderer& renderer_;
    bool available_ = false;
    bool active_ = false;
    bool resourcesAllocated_ = false;
    bool resetPending_ = true;
    bool outputValid_ = false;
    // DLSS-split: frames left to skip after an evaluate we PREDICTED would run came back false.
    // Ticked down in OnBeginFrame; while non-zero the prediction says no and the tonemap takes the
    // scene-colour path. A COUNTER rather than a sticky bool: the pass that could clear a bool
    // only exists on frames the prediction said yes, so a bool would latch DLSS off for good.
    static constexpr std::uint32_t kEvaluateBackoffFrames = 30;
    std::uint32_t skipEvaluateFrames_ = 0;
    sl::ViewportHandle viewport_{ 1 };
    sl::DLSSOptions options_{};
    sl::Constants constants_{};
    sl::FrameToken* frameToken_ = nullptr;
    UINT dlssRenderWidth_ = 0;
    UINT dlssRenderHeight_ = 0;
    Math::float2 jitterPixels_ = Math::float2(0.0f, 0.0f);
    bool jitterPaused_ = false;
    uint32_t haltonIndex_ = 0;
    static constexpr uint32_t kHaltonSequenceLength_ = 1024;

    // 1x1 R32F exposure texture tagged as kBufferTypeExposure. Without it NGX forces
    // auto-exposure ON regardless of useAutoExposure (confirmed via the NGX debug HUD).
    Microsoft::WRL::ComPtr<ID3D12Resource> exposureTex_;
    Microsoft::WRL::ComPtr<ID3D12Resource> exposureUpload_;
    bool exposureUploaded_ = false;
};
