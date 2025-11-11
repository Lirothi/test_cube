#pragma once

#include <d3d12.h>
#include <algorithm>
#include <cstdint>

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

    bool IsActive() const;
    void SetActive(bool active);
    bool IsAvailable() const { return available_; }
    bool ShouldUseUpscaledOutput() const { return IsActive() && outputValid_; }
    void InvalidateOutput() { outputValid_ = false; }

private:
    void HandleAllocationFailure();
    void ResetJitterSequence();
    Math::float2 GenerateJitterSample();

private:
    Renderer& renderer_;
    bool available_ = false;
    bool active_ = false;
    bool resourcesAllocated_ = false;
    bool resetPending_ = true;
    bool outputValid_ = false;
    sl::ViewportHandle viewport_{ 1 };
    sl::DLSSOptions options_{};
    sl::Constants constants_{};
    sl::FrameToken* frameToken_ = nullptr;
    UINT dlssRenderWidth_ = 0;
    UINT dlssRenderHeight_ = 0;
    Math::float2 jitterPixels_ = Math::float2(0.0f, 0.0f);
    uint32_t haltonIndex_ = 0;
    static constexpr uint32_t kHaltonSequenceLength_ = 1024;
};
