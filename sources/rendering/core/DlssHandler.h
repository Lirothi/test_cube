#pragma once

#include <d3d12.h>
#include <algorithm>
#include <cstdint>

#include "streamline/include/sl.h"
#include "streamline/include/sl_core_types.h"
#include "streamline/include/sl_dlss.h"

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

    bool IsActive() const;
    void SetActive(bool active);
    bool IsAvailable() const { return available_; }
    bool ShouldUseUpscaledOutput() const { return IsActive() && outputValid_; }
    void InvalidateOutput() { outputValid_ = false; }

private:
    void HandleAllocationFailure();

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
};
