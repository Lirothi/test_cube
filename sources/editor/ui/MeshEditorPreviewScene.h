#pragma once
#if WITH_EDITOR

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/math/Math.h"
#include "editor/assets/EditorPreviewMode.h"
#include "imgui.h"

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
        const TextureCube* environment = nullptr,
        float environmentExposure = 1.0f);

    // Release resources while the renderer is available. Used when switching
    // assets; normal application teardown already idles the GPU first.
    void Reset(Renderer& renderer);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // WITH_EDITOR
