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
    ui::ImGuiWindowMaximizeState windowMaximize_;
    bool showBorder_ = true;
};
