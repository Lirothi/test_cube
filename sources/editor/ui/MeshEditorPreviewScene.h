#pragma once
#if WITH_EDITOR

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/math/Math.h"
#include "editor/assets/EditorPreviewMode.h"
#include "editor/assets/EditorPreviewRenderer.h" // PhysicalLighting
#include "imgui.h"
#include "rendering/meshes/LodSelect.h" // render::kLodScreenSizes

class Renderer;
class TextureCube;

struct MeshEditorPreviewCamera
{
    float yaw = 0.674741f;
    float pitch = 0.500180f;
    float zoom = 1.0f;
    float panX = 0.0f;
    float panY = 0.0f;
};

struct MeshEditorPreviewLight
{
    Math::float3 direction{ -0.390360f, -0.780720f, 0.487950f };
    Math::float3 color{ 1.0f, 1.0f, 1.0f };
    float exposure = 1.0f;
    float ambient = 0.1f;
    bool showPosition = false;
    float positionDistance = 1.5f;
    // Which sky the preview REFLECTS (the background too): "" = the open level's own sky,
    // "none" = no sky at all, otherwise a cubemap asset path.
    std::string sky;
    float skyIntensity = 1.0f;
    // Light the preview the way the level lights the scene: its sun in lux, its sky through the
    // split-sum IBL, its tone curve. Only `direction` (and the sky choice above) still apply then;
    // `color`, `exposure` and `ambient` belong to the legacy look, which keeps them for when this
    // is switched off.
    bool levelLighting = true;
    // The camera under level lighting is the preview's OWN: not the viewport's, which stands wherever it stands (a closed room meters 6 stops
    // under the open sun the preview is lit by, and the asset came out white). Auto = a grey card under the preview's light, over or under by
    // the compensation (positive = brighter); else the EV100 as given.
    bool autoExposure = true;
    float exposureCompensationEv = 0.0f;
    float manualEv100 = 14.0f;
};

// A small isolated scene used by the Mesh Editor. Geometry and materials are
// loaded into private caches, then rendered into one persistent target per
// swapchain frame. The targets are submitted before the main frame, so ImGui can
// sample the current orbit camera without a per-drag GPU wait.
class MeshEditorPreviewScene
{
public:
    enum class State
    {
        Loading,
        Ready,
        Failed
    };

    struct View
    {
        State state = State::Loading;
        ImTextureID texture = ImTextureID_Invalid;
        const char* error = nullptr;
        std::uint32_t lodCount = 1;
        std::uint32_t indexCount = 0;
        std::uint32_t triangleCount = 0;
        // Mesh space -> clip for the image in `texture` (the frame it was rendered with, cached
        // or not). Valid when state == Ready.
        Math::mat4 viewProj;
        Math::mat4 viewMatrix;
        Math::mat4 projMatrix;
    };

    MeshEditorPreviewScene();
    ~MeshEditorPreviewScene();

    MeshEditorPreviewScene(const MeshEditorPreviewScene&) = delete;
    MeshEditorPreviewScene& operator=(const MeshEditorPreviewScene&) = delete;

    View Update(Renderer& renderer,
        const std::string& assetKey,
        const std::string& geometry,
        const std::vector<std::string>& materialSlots,
        const std::vector<std::uint32_t>& recomputeNormalSlots,
        std::uint64_t assetRegistryRevision,
        std::uint32_t renderWidth,
        std::uint32_t renderHeight,
        const MeshEditorPreviewCamera& camera,
        const MeshEditorPreviewLight& light,
        EditorPreviewMode mode,
        std::uint32_t lod,
        // Live mesh.json "texOffsScale" (null = use each material's own). Per-frame only: it feeds
        // the draw constants and must never take part in the reload key, or every drag would
        // rebuild the scene.
        const Math::float4* texOffsScaleOverride = nullptr,
        // Material slot to highlight while its Mesh Editor control is hovered (-1 = none).
        int highlightMaterialSlot = -1,
        // Submesh ORDINAL to highlight (-1 = none) — how the chunk list picks out ONE tile,
        // which the slot highlight above cannot do (every tile shares slot 0).
        int highlightSubmeshOrdinal = -1,
        const TextureCube* environment = nullptr,
        float environmentExposure = 1.0f,
        // The level's own lighting (see EditorPreviewRenderer::PhysicalLighting); null = the
        // preview's legacy look.
        const EditorPreviewRenderer::PhysicalLighting* physical = nullptr,
        // Lines over the mesh against its depth (the collision shape). The revision says when they changed: the preview
        // re-renders only when something it draws did. 0 = no lines.
        const EditorPreviewRenderer::PreviewLines* lines = nullptr,
        std::uint64_t linesRevision = 0);

    // Release resources while the renderer is available. Used when switching
    // assets; normal application teardown already idles the GPU first.
    void Reset(Renderer& renderer);

    // Everything, pipelines included, before the device goes (the debug layer's exit report). The next Update builds it again.
    void ReleaseGpuResources(Renderer& renderer);

    // UE's AUTOMATIC screen size of each LOD of the loaded mesh (Mesh::GetLodScreenSizes) -- what an
    // authored mesh.json "lodScreenSize" replaces. Levels the mesh lacks read 0. False while nothing
    // is loaded.
    bool GetLodScreenSizes(float pixelError, float (&out)[render::kLodScreenSizes]) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // WITH_EDITOR
