#include "rendering/debug/TextureDebugViewer.h"
#include "rendering/core/RenderConstants.h" // P6B: kGtaoFormat for the AO row

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "core/profiling/ProfilerScopes.h"
#include "rendering/core/Renderer.h"

namespace
{
    struct TargetView
    {
        TextureDebugViewer::Target target;
        const char* name;
        const char* group;
        const char* description;
        ID3D12Resource* resource;
        DXGI_FORMAT srvFormat;
    };

    const char* FormatName(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
        case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
        case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
        case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
        default: return "Other";
        }
    }

    uint32_t BitsPerPixel(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8_UNORM: return 8;
        case DXGI_FORMAT_R8G8_UNORM: return 16;
        case DXGI_FORMAT_R16_UNORM: return 16;
        case DXGI_FORMAT_R16G16_FLOAT: return 32;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            return 32;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return 64;
        default:
            return 0;
        }
    }

    bool IsSingleChannelFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            return true;
        default:
            return false;
        }
    }

    D3D12_SHADER_COMPONENT_MAPPING Component(uint32_t index)
    {
        switch (index)
        {
        case 0: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
        case 1: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1;
        case 2: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2;
        case 3: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3;
        default: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
        }
    }

    UINT ComponentMapping(TextureDebugViewer::ChannelMode mode, DXGI_FORMAT format)
    {
        if (mode == TextureDebugViewer::ChannelMode::Auto)
        {
            if (IsSingleChannelFormat(format))
            {
                return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                    D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
            }
            return D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        }

        if (mode == TextureDebugViewer::ChannelMode::Raw)
        {
            return D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        }

        uint32_t component = 0;
        switch (mode)
        {
        case TextureDebugViewer::ChannelMode::Green: component = 1; break;
        case TextureDebugViewer::ChannelMode::Blue: component = 2; break;
        case TextureDebugViewer::ChannelMode::Alpha: component = 3; break;
        default: component = 0; break;
        }

        const D3D12_SHADER_COMPONENT_MAPPING src = Component(component);
        return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
            src, src, src, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC MakeSrvDesc(const TargetView& view,
        TextureDebugViewer::ChannelMode channelMode, UINT mip = 0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = view.srvFormat;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = ComponentMapping(channelMode, view.srvFormat);
        // A one-level view of the CHOSEN mip, so the preview shows that level's own texels rather
        // than the whole chain filtered down to the widget size.
        desc.Texture2D.MostDetailedMip = mip;
        desc.Texture2D.MipLevels = 1;
        desc.Texture2D.PlaneSlice = 0;
        desc.Texture2D.ResourceMinLODClamp = 0.0f;
        return desc;
    }

    const char* ChannelModeLabel(TextureDebugViewer::ChannelMode mode)
    {
        switch (mode)
        {
        case TextureDebugViewer::ChannelMode::Auto: return "Auto";
        case TextureDebugViewer::ChannelMode::Raw: return "Raw";
        case TextureDebugViewer::ChannelMode::Red: return "Red";
        case TextureDebugViewer::ChannelMode::Green: return "Green";
        case TextureDebugViewer::ChannelMode::Blue: return "Blue";
        case TextureDebugViewer::ChannelMode::Alpha: return "Alpha";
        default: return "Unknown";
        }
    }

    const char* TargetLabel(TextureDebugViewer::Target target)
    {
        switch (target)
        {
        case TextureDebugViewer::Target::Scene: return "Scene HDR";
        case TextureDebugViewer::Target::Light: return "Lighting";
        case TextureDebugViewer::Target::Tonemap: return "Tonemap";
        case TextureDebugViewer::Target::Fxaa: return "FXAA";
        case TextureDebugViewer::Target::DlssOutput: return "DLSS Output";
        case TextureDebugViewer::Target::SceneOpaque: return "Scene Opaque";
        case TextureDebugViewer::Target::Reflection: return "Reflection";
        case TextureDebugViewer::Target::ReflectionScratch: return "Reflection Scratch";
        case TextureDebugViewer::Target::OceanReflection: return "Ocean Reflection";
        case TextureDebugViewer::Target::GlassReflNormal: return "Glass Refl Normal";
        case TextureDebugViewer::Target::GlassReflDepth: return "Glass Refl Depth";
        case TextureDebugViewer::Target::GlassReflection: return "Glass Reflection";
        case TextureDebugViewer::Target::Gtao: return "GTAO";
        case TextureDebugViewer::Target::GtaoFiltered: return "GTAO denoised";
        case TextureDebugViewer::Target::GtaoHistory: return "GTAO temporal";
        case TextureDebugViewer::Target::GtaoUpsampled: return "GTAO combined";
        case TextureDebugViewer::Target::Hzb: return "HZB furthest (AO)";
        case TextureDebugViewer::Target::HzbClosest: return "HZB closest (SSR)";
        case TextureDebugViewer::Target::GBuffer0: return "GBuffer 0";
        case TextureDebugViewer::Target::GBuffer1: return "GBuffer 1";
        case TextureDebugViewer::Target::ShadingModel: return "Shading Model ID";
        case TextureDebugViewer::Target::GBuffer2: return "GBuffer 2";
        case TextureDebugViewer::Target::GBufferAux: return "GBuffer Aux";
        case TextureDebugViewer::Target::Velocity: return "Velocity";
        case TextureDebugViewer::Target::Depth: return "Depth";
        case TextureDebugViewer::Target::DepthCopy: return "Depth Copy";
        case TextureDebugViewer::Target::ShadowAtlas: return "Shadow Atlas";
        case TextureDebugViewer::Target::OceanShoreDepth: return "Ocean Shore Depth";
        default: return "Unknown";
        }
    }

    TargetView MakeTarget(TextureDebugViewer::Target target,
        const char* group,
        const char* description,
        ID3D12Resource* resource,
        DXGI_FORMAT srvFormat)
    {
        return TargetView{ target, TargetLabel(target), group, description, resource, srvFormat };
    }

    std::array<TargetView, static_cast<size_t>(TextureDebugViewer::Target::Count)> BuildTargets(Renderer& renderer,
        ID3D12Resource* oceanShoreDepth)
    {
        const auto& D = renderer.GetDeferredForFrame();
        return {
            MakeTarget(TextureDebugViewer::Target::Scene, "Color", "HDR scene color before tonemap.", D.scene.Get(), renderer.GetSceneColorFormat()),
            MakeTarget(TextureDebugViewer::Target::Light, "Color", "Lighting accumulation target.", D.light.Get(), renderer.GetLightTargetFormat()),
            MakeTarget(TextureDebugViewer::Target::Tonemap, "Post", "Tonemap output at display resolution.", D.tonemap.Get(), renderer.GetBackbufferResourceFormat()),
            MakeTarget(TextureDebugViewer::Target::Fxaa, "Post", "FXAA output at display resolution.", D.fxaa.Get(), renderer.GetBackbufferResourceFormat()),
            MakeTarget(TextureDebugViewer::Target::DlssOutput, "Post", "DLSS upscaled output.", D.dlssOutput.Get(), renderer.GetSceneColorFormat()),
            MakeTarget(TextureDebugViewer::Target::SceneOpaque, "Color", "Opaque scene copy used by refraction.", D.sceneOpaque.Get(), renderer.GetSceneColorFormat()),
            MakeTarget(TextureDebugViewer::Target::Reflection, "Reflection", "Main reflection result consumed by compose.", D.reflection.Get(), renderer.GetReflectionFormat()),
            MakeTarget(TextureDebugViewer::Target::ReflectionScratch, "Reflection", "Scratch target used by reflection filtering and RT denoise.", D.reflectionScratch.Get(), renderer.GetReflectionScratchFormat()),
            MakeTarget(TextureDebugViewer::Target::OceanReflection, "Reflection", "Premultiplied ocean reflection generated before transparent ocean rendering.", D.oceanReflection.Get(), renderer.GetReflectionFormat()),
            MakeTarget(TextureDebugViewer::Target::GlassReflNormal, "Reflection", "Glass G-buffer: front-face world normal (S15b prepass input).", D.glassReflNormal.Get(), renderer.GetGBuffer1Format()),
            MakeTarget(TextureDebugViewer::Target::GlassReflDepth, "Reflection", "Glass G-buffer: front-face depth (S15b reflection ray origin).", D.glassReflDepth.Get(), renderer.GetDepthSrvFormat()),
            MakeTarget(TextureDebugViewer::Target::GlassReflection, "Reflection", "Off-screen glass reflection (RT or SSR) sampled by the forward glass pass.", D.glassReflection.Get(), renderer.GetReflectionFormat()),
            MakeTarget(TextureDebugViewer::Target::Gtao, "Lighting", "P6B/P16.4 screen-space ambient occlusion, RAW, half render resolution. TWO CHANNELS: Red = contact scale (gtao.worldRadius), Green = sky scale (gtao.skyRadius) - use the channel selector, because in Auto this reads as yellow where both are open and green where only the wide walk found occlusion. 1 = unoccluded. Expected to be noisy: this is the estimate before any filtering. Blank/white everywhere means the pass is disabled (SceneRenderSettings::gtao.enabled); the two channels being identical means Sky Radius is at or below World Radius, which is the OFF state.", D.gtao.Get(), render::kGtaoFormat),
            MakeTarget(TextureDebugViewer::Target::GtaoFiltered, "Lighting", "P6B AO after the bilateral denoise (half res), both channels. Compare against RAW: the per-pixel grain should be gone while the contact darkening stays put. Stale when gtao.denoise is off.", D.gtaoFiltered.Get(), render::kGtaoFormat),
            MakeTarget(TextureDebugViewer::Target::GtaoHistory, "Lighting", "P6B AO after temporal accumulation (half res), both channels; also next frame's history. Under a moving camera the disocclusion band behind an occluder falls back to the un-accumulated estimate. Stale when gtao.temporal is off.", D.gtaoHistory.Get(), render::kGtaoFormat),
            MakeTarget(TextureDebugViewer::Target::GtaoUpsampled, "Lighting", "P6B AO at render resolution after the edge-aware upsample - the target lighting and compose consume, taking min(Red, Green) per pixel. Silhouettes should be crisp: a soft dark fringe against the sky means the depth-aware weights are not firing.", D.gtaoUpsampled.Get(), render::kGtaoFormat),
            MakeTarget(TextureDebugViewer::Target::Hzb, "Depth", "P6C hierarchical depth pyramid, FURTHEST variant: each level is the 2x2 MIN of the previous, which under reversed-Z is the furthest surface in the tile. That is what a horizon search wants -- a coarse tile then UNDER-estimates occlusion, so it cannot invent contact shadows. GTAO reads this one. Mip 0 is half the render resolution; pick the level with the Mip control. Built right after the G-buffer, so it holds OPAQUE depth only and will legitimately disagree with \"Depth\" wherever the transparent pass wrote water or glass. Reversed-Z device depth sits within a hair of 0, so raise Gain or switch on Depth stretch or this reads as a black rectangle.", D.hzb.Get(), render::kHzbFormat),
            MakeTarget(TextureDebugViewer::Target::HzbClosest, "Depth", "P6C step 6: the same pyramid reduced with MAX device Z = the NEAREST surface in each tile. A ray march wants this one, so that a long step cannot tunnel through a surface the tile really contains -- the exact opposite of what the AO search wants, which is why there are two. ONLY BUILT while the HiZ SSR technique is the active reflection source; on any other frame this shows whatever was last written, not live data. Same black-by-nature caveat: use Gain or Depth stretch.", D.hzbClosest.Get(), render::kHzbFormat),
            MakeTarget(TextureDebugViewer::Target::GBuffer0, "GBuffer", "Albedo RGB, metalness A.", D.gb0.Get(), renderer.GetGBuffer0Format()),
            MakeTarget(TextureDebugViewer::Target::GBuffer1, "GBuffer", "Encoded normal RGB; alpha is unused.", D.gb1.Get(), renderer.GetGBuffer1Format()),
            MakeTarget(TextureDebugViewer::Target::ShadingModel, "GBuffer", "Four-bit shading-model ID scale from GBAux.b: black=0 Default Lit, 1/15 gray=1 Two-Sided Foliage, 2/15 gray=2 Terrain, 3..15 are reserved.", D.gbAux.Get(), renderer.GetGBufferAuxFormat()),
            MakeTarget(TextureDebugViewer::Target::GBuffer2, "GBuffer", "DefaultLit emissive; Two-Sided Foliage subsurface/transmission payload.", D.gb2.Get(), renderer.GetGBuffer2Format()),
            MakeTarget(TextureDebugViewer::Target::GBufferAux, "GBuffer", "Auxiliary material payload: AO R, indirect specular scale G, shading model B, reserved A.", D.gbAux.Get(), renderer.GetGBufferAuxFormat()),
            MakeTarget(TextureDebugViewer::Target::Velocity, "GBuffer", "Motion vectors.", D.gbVelocity.Get(), renderer.GetGBufferVelocityFormat()),
            MakeTarget(TextureDebugViewer::Target::Depth, "Depth", "Main deferred depth SRV.", D.depth.Get(), renderer.GetDepthSrvFormat()),
            MakeTarget(TextureDebugViewer::Target::DepthCopy, "Depth", "Depth copy before transparent pass.", D.depthCopy.Get(), renderer.GetDepthSrvFormat()),
            MakeTarget(TextureDebugViewer::Target::ShadowAtlas, "Shadows", "Directional cascade shadow atlas.", D.shadow, DXGI_FORMAT_R16_UNORM),
            MakeTarget(TextureDebugViewer::Target::OceanShoreDepth, "Ocean", "Orthographic terrain depth used by ocean shore blending.", oceanShoreDepth, DXGI_FORMAT_R16_UNORM),
        };
    }

    const TargetView* FindTarget(const std::array<TargetView, static_cast<size_t>(TextureDebugViewer::Target::Count)>& targets,
        TextureDebugViewer::Target target)
    {
        for (const TargetView& view : targets)
        {
            if (view.target == target)
            {
                return &view;
            }
        }
        return nullptr;
    }

    ImVec2 FitAspect(ImVec2 available, uint32_t width, uint32_t height)
    {
        available.x = std::max(available.x, 1.0f);
        available.y = std::max(available.y, 1.0f);
        const float aspect = static_cast<float>(std::max(width, 1u)) / static_cast<float>(std::max(height, 1u));
        ImVec2 size = available;
        if (size.x / size.y > aspect)
        {
            size.x = size.y * aspect;
        }
        else
        {
            size.y = size.x / aspect;
        }
        return size;
    }
}

void TextureDebugViewer::Draw(Renderer& renderer, ID3D12Resource* oceanShoreDepth)
{
    if (!open_)
    {
        return;
    }
    CPU_SCOPE(ProfilerScopes::kTextureDebugViewerDraw);

    const auto targets = BuildTargets(renderer, oceanShoreDepth);
    const TargetView* selected = FindTarget(targets, target_);
    if (!selected)
    {
        target_ = Target::Scene;
        selected = FindTarget(targets, target_);
    }

    ImGui::SetNextWindowSize(ImVec2(1100.0f, 680.0f), ImGuiCond_FirstUseEver);
    bool open = open_;
    const ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoCollapse |
        (windowMaximize_.maximized ? ImGuiWindowFlags_NoMove : 0);
    if (!ImGui::Begin("Texture Inspector###TextureDebugViewer", &open, windowFlags))
    {
        ImGui::End();
        open_ = open;
        return;
    }
    ui::HandleWindowTitleDoubleClickMaximize(windowMaximize_);

    constexpr float controlsWidth = 400.0f;
    ImGui::BeginChild("TextureDebugControls", ImVec2(controlsWidth, 0.0f), true);

    ImGui::TextUnformatted("Texture");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##TextureDebugRenderTarget", selected ? selected->name : "None", ImGuiComboFlags_HeightLarge))
    {
        const char* previousGroup = "";
        for (const TargetView& view : targets)
        {
            if (std::strcmp(view.group, previousGroup) != 0)
            {
                if (previousGroup[0] != '\0')
                {
                    ImGui::Separator();
                }
                ImGui::TextDisabled("%s", view.group);
                previousGroup = view.group;
            }

            const bool isSelected = view.target == target_;
            if (ImGui::Selectable(view.name, isSelected))
            {
                target_ = view.target;
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    const bool forceShadingModelChannel = target_ == Target::ShadingModel;
    const ChannelMode effectiveChannelMode = forceShadingModelChannel ? ChannelMode::Blue : channelMode_;
    ImGui::BeginDisabled(forceShadingModelChannel);
    if (ImGui::BeginCombo("Channels", ChannelModeLabel(effectiveChannelMode)))
    {
        for (int i = 0; i < static_cast<int>(ChannelMode::Count); ++i)
        {
            const ChannelMode mode = static_cast<ChannelMode>(i);
            const bool isSelected = mode == channelMode_;
            if (ImGui::Selectable(ChannelModeLabel(mode), isSelected))
            {
                channelMode_ = mode;
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    ImGui::Checkbox("Image border", &showBorder_);

    // Mip picker: only meaningful for a target that HAS a chain, which today is the HZB alone.
    const UINT mipCount = (selected && selected->resource)
        ? std::max<UINT>(1u, selected->resource->GetDesc().MipLevels)
        : 1u;
    if (mip_ > static_cast<int>(mipCount) - 1) { mip_ = static_cast<int>(mipCount) - 1; }
    ImGui::BeginDisabled(mipCount <= 1);
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderInt("Mip", &mip_, 0, static_cast<int>(mipCount) - 1);
    ImGui::EndDisabled();

    // Brightness and the depth stretch, both applied by OUR preview shader (see
    // debug_preview_cs.hlsl). They cannot be done with ImGui's image tint: that is packed to 8 bits
    // and saturates at 1.0, so the first version of this slider was bit-identical to doing nothing.
    ImGui::SetNextItemWidth(140.0f);
    ImGui::DragFloat("Gain", &gain_, 0.05f, 0.001f, 100000.0f, "%.3gx",
                     ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    if (ImGui::SmallButton("1x")) { gain_ = 1.0f; }
    ImGui::SameLine();
    ImGui::Checkbox("Depth stretch", &stretch_);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Monotone pow(1/8). Reversed-Z device depth keeps its whole useful range "
                          "within a hair of 0, so a linear preview of a depth target is black. "
                          "Monotone on purpose: min/max relationships survive it.");
    }

    // Only the targets the fullscreen view knows about; its list is a different (shorter) one.
    {
        const int fsTarget =
            (target_ == Target::Gtao) ? 1 :
            (target_ == Target::GtaoFiltered) ? 2 :
            (target_ == Target::GtaoHistory) ? 3 :
            (target_ == Target::GtaoUpsampled) ? 4 :
            (target_ == Target::Hzb) ? 5 :
            (target_ == Target::Depth) ? 6 :
            (target_ == Target::HzbClosest) ? 7 : -1;
        ImGui::BeginDisabled(fsTarget < 0);
        if (ImGui::Button("Show fullscreen"))
        {
            fullscreenRequest_ = fsTarget;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(fsTarget >= 0
            ? "(same target, full window instead of this panel)"
            : "(no fullscreen equivalent for this target)");
    }

    ImGui::Separator();

    ID3D12Resource* resource = selected ? selected->resource : nullptr;
    D3D12_RESOURCE_DESC resourceDesc{};
    if (resource)
    {
        resourceDesc = resource->GetDesc();
        const uint32_t width = static_cast<uint32_t>(std::max<UINT64>(resourceDesc.Width, 1));
        const uint32_t height = std::max<uint32_t>(resourceDesc.Height, 1);
        const uint32_t arraySize = std::max<uint32_t>(resourceDesc.DepthOrArraySize, 1);
        const uint32_t bpp = selected ? BitsPerPixel(selected->srvFormat) : 0;
        const double approxMiB = bpp > 0
            ? (static_cast<double>(width) * height * arraySize * bpp) / (8.0 * 1024.0 * 1024.0)
            : 0.0;

        const UINT shownMip = static_cast<UINT>(std::max(0, mip_));
        ImGui::Text("Size: %ux%u", std::max(1u, width >> shownMip), std::max(1u, height >> shownMip));
        if (mipCount > 1)
        {
            ImGui::Text("Mip %u of %u  (base %ux%u)", shownMip, mipCount - 1, width, height);
        }
        ImGui::Text("Array slices: %u", arraySize);
        ImGui::Text("Aspect: %.3f", static_cast<double>(width) / static_cast<double>(height));
        ImGui::Text("Resource: %s", FormatName(resourceDesc.Format));
        ImGui::Text("SRV: %s", selected ? FormatName(selected->srvFormat) : "None");
        if (bpp > 0)
        {
            ImGui::Text("Approx memory: %.2f MiB", approxMiB);
        }
        else
        {
            ImGui::TextDisabled("Approx memory: unknown");
        }
        ImGui::Text("Mip levels: %u", resourceDesc.MipLevels);
        ImGui::Text("Flags:%s%s%s",
            (resourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? " RTV" : "",
            (resourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ? " DSV" : "",
            (resourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? " UAV" : "");
        ImGui::Text("Resource ptr: 0x%p", static_cast<void*>(resource));
        ImGui::Separator();
        ImGui::TextWrapped("%s", selected ? selected->description : "");
        ImGui::TextDisabled("Preview SRV is transitioned to PixelShaderResource before ImGui rendering.");
    }
    else
    {
        ImGui::TextDisabled("Selected texture is not available.");
    }

    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("TextureDebugPreview", ImVec2(0.0f, 0.0f), true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (resource && selected)
    {
        const uint32_t width = static_cast<uint32_t>(std::max<UINT64>(resourceDesc.Width, 1));
        const uint32_t height = std::max<uint32_t>(resourceDesc.Height, 1);
        const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc =
            MakeSrvDesc(*selected, effectiveChannelMode, static_cast<UINT>(std::max(0, mip_)));

        // Hand the pass what to resample, then display ITS output. The pass runs later this frame
        // (the developer window is built before Scene::Render), so what ImGui ends up sampling is
        // this frame's preview, not a stale one.
        Renderer::DebugPreviewRequest req{};
        req.resource = resource;
        req.srv = srvDesc;
        req.gain = gain_;
        req.stretch = stretch_;
        req.showAlpha = (effectiveChannelMode == TextureDebugViewer::ChannelMode::Raw);
        renderer.RequestDebugPreview(req);

        const auto& targets = renderer.GetDeferredForFrame();
        D3D12_SHADER_RESOURCE_VIEW_DESC previewSrv{};
        previewSrv.Format = render::kDebugPreviewFormat;
        previewSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        previewSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        previewSrv.Texture2D.MipLevels = 1;
        const ImTextureID textureId =
            renderer.CreateImGuiTextureId(targets.debugPreview.Get(), previewSrv);

        ImVec2 available = ImGui::GetContentRegionAvail();
        ImVec2 imageSize = FitAspect(available, width, height);
        const float offsetX = std::max(0.0f, (available.x - imageSize.x) * 0.5f);
        const float offsetY = std::max(0.0f, (available.y - imageSize.y) * 0.5f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offsetY);

        if (textureId != ImTextureID_Invalid)
        {
            const ImVec4 bg = showBorder_ ? ImVec4(0.04f, 0.04f, 0.045f, 1.0f) : ImVec4(0, 0, 0, 0);
            const ImVec4 tint = ImVec4(1, 1, 1, 1);
            ImGui::ImageWithBg(textureId, imageSize, ImVec2(0, 0), ImVec2(1, 1), bg, tint);
        }
        else
        {
            ImGui::TextDisabled("No ImGui texture descriptor available.");
        }
    }
    else
    {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(available.x * 0.35f, available.y * 0.45f));
        ImGui::TextDisabled("No texture selected");
    }

    ImGui::EndChild();

    ImGui::End();
    open_ = open;
}
