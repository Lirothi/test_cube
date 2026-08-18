#pragma once

#include <d3d12.h>

#include "imgui.h"
#include "ui/ImGuiWindowUtils.h"

class Renderer;

class TextureDebugViewer
{
public:
    bool IsOpen() const { return open_; }
    void SetOpen(bool open) { open_ = open; }
    void Draw(Renderer& renderer, ID3D12Resource* oceanShoreDepth);

    // Set when the user asks to see the current target in the fullscreen view instead. -1 = nothing
    // requested. The developer window polls this, because only it owns the render settings.
    // Values match SceneRenderSettings::debugTexTarget.
    int TakeFullscreenRequest() { const int r = fullscreenRequest_; fullscreenRequest_ = -1; return r; }
    int RequestedMip() const { return mip_; }

    enum class Target : int
    {
        Scene,
        Light,
        Tonemap,
        Fxaa,
        DlssOutput,
        SceneOpaque,
        Reflection,
        ReflectionScratch,
        OceanReflection,
        GlassReflNormal,
        GlassReflDepth,
        GlassReflection,
        GBuffer0,
        GBuffer1,
        ShadingModel,
        GBuffer2,
        GBufferAux,
        Velocity,
        Depth,
        DepthCopy,
        ShadowAtlas,
        OceanShoreDepth,
        Gtao,
        GtaoFiltered,
        GtaoHistory,
        GtaoUpsampled,
        Hzb,
        HzbClosest,
        Count
    };

    enum class ChannelMode : int
    {
        Auto,
        Raw,
        Red,
        Green,
        Blue,
        Alpha,
        Count
    };

private:
    bool open_ = false;
    Target target_ = Target::Scene;
    ChannelMode channelMode_ = ChannelMode::Auto;
    // P6C: the depth pyramid is the first target with a mip chain.
    int mip_ = 0;
    // Brightness applied by OUR preview shader, not by ImGui -- see debug_preview_cs.hlsl. The
    // first version of this used ImGui's image tint, which packs to 8 bits and silently saturated.
    float gain_ = 1.0f;
    bool stretch_ = false;
    int fullscreenRequest_ = -1;
    ui::ImGuiWindowMaximizeState windowMaximize_;
    bool showBorder_ = true;
};
