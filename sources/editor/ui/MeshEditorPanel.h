#pragma once
#if WITH_EDITOR

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "editor/ui/MeshEditorPreviewScene.h"
#include "materials/TextureCube.h"
#include "rendering/meshes/MeshManager.h" // BinaryInfo: per-LOD chunk triangle counts
#include "third_party/json/json.hpp"

class AssetRegistry;
struct EditorContext;

// Part J — dedicated Mesh Editor window. Opened by double-clicking a `models/<name>.mesh.json`
// asset in the content browser. Edits the mesh asset's render defaults — per-slot materials and
// normal generation, renderLayer, spawnScale, texOffsScale tiling — and writes them back while
// preserving unknown keys. Shader is intentionally NOT editable here: it is a MATERIAL concern
// (Part I0), never a mesh one.
class MeshEditorPanel
{
public:
    using OpenMaterialHandler = std::function<void(
        const std::string& materialName, const std::string& materialPath)>;

    struct PersistentState
    {
        float previewPaneRatio = 0.55f;
        MeshEditorPreviewLight previewLight;
    };

    // Load a `.mesh.json` for editing (content-browser EditMesh action). Resets prior state.
    void Open(const std::string& meshAssetPath);

    // Draw the window body (inside the editor's lambda panel). `open` backs the close button.
    // `ctx` gives access to the scene document so Save can live-apply to placed instances.
    void Draw(EditorContext& ctx, AssetRegistry& registry, bool* open,
        const OpenMaterialHandler& openMaterial);

    const std::string& CurrentPath() const { return path_; }
    PersistentState GetPersistentState() const { return { previewPaneRatio_, previewLight_ }; }
    void SetPersistentState(const PersistentState& state);

private:
    void Save(EditorContext& ctx, AssetRegistry& registry);

    // Per-slot wind foliage weight, stored in the document as "windFoliage": [w0, w1, ...].
    // Reading a slot with no entry yet reports the same default the runtime uses (the slot's
    // alpha-mask flag is not visible here, so 0 = woody, and the runtime fallback still applies
    // while the array is absent). Writing grows the array to the slot count.
    float WindFoliageForSlot(size_t slot) const;
    void SetWindFoliageForSlot(size_t slot, float value);
    // Live-apply: respawn every placed object that references this mesh asset (and doesn't override
    // the changed key) so the edit shows immediately. Returns the number of instances updated.
    int  ApplyToScene(EditorContext& ctx) const;

    std::string   path_;             // the .mesh.json path currently being edited
    nlohmann::json doc_;             // parsed document (the round-trip base — unknown keys preserved)
    bool          loaded_ = false;
    std::vector<std::string> slots_; // one material preset per submesh (auto-sized to the geometry)
    std::vector<uint32_t> recomputeNormalSlots_; // submesh slots that discard authored normals
    // Material slot whose controls were hovered LAST frame (-1 = none); the preview tints it so it
    // is obvious which part of the model a control drives. One frame stale by construction: the
    // preview pane is drawn before the settings pane that detects the hover.
    int           hoveredSlot_ = -1;
    // Chunked meshes (mesh.json "chunkGrid"): their submeshes are spatial TILES, not material
    // slots, so the material list collapses to one and the tiles get their own list instead.
    // chunkGrid_ 0/1 = not chunked. hoveredChunk_ drives the preview's per-ORDINAL highlight,
    // which is the only way to single out one tile (they all share material slot 0).
    int           chunkGrid_ = 0;
    int           hoveredChunk_ = -1;
    MeshManager::BinaryInfo binInfo_{}; // per-LOD, per-submesh triangle counts; empty if not baked
    std::string   status_;
    MeshEditorPreviewScene previewScene_;
    MeshEditorPreviewCamera previewCamera_;
    MeshEditorPreviewLight previewLight_;
    EditorPreviewMode previewMode_ = EditorPreviewMode::Lit;
    std::uint32_t previewLod_ = 0;
    float previewPaneRatio_ = 0.55f;

    // The sky the preview is lit by, per previewLight_.sky. The level's own sky is borrowed
    // from the scene; a picked cubemap is loaded here once per choice and kept until another
    // is picked. Returns null for "none" or a cube that would not load (previewSkyError_ says).
    const TextureCube* ResolvePreviewSky(EditorContext& ctx, float& outExposure);
    void DrawPreviewSkyControls(AssetRegistry& registry);
    std::unique_ptr<TextureCube> previewSkyCube_;
    // A picked cube's split-sum derivatives, found beside it by name exactly as the level's
    // Skybox finds its own (`<stem>_spec.dds`, `<stem>_diffuse.dds`). Both or neither.
    std::unique_ptr<TextureCube> previewSkySpec_;
    std::unique_ptr<TextureCube> previewSkyIrradiance_;
    float previewSkyUpIlluminance_ = 0.0f;   // cube units, from `_diffuse.dds`; 0 = uncalibrated
    std::string previewSkyCubePath_;
    std::string previewSkyError_;

    // previewLight_.levelLighting: the level's sun, sky, exposure and tone curve, in the renderer's
    // own units. False = the preview's legacy look (and `out` is untouched).
    bool BuildPreviewLighting(EditorContext& ctx, EditorPreviewRenderer::PhysicalLighting& out);
    // What BuildPreviewLighting took from the level last frame, for the panel to say so.
    struct LevelLightingReadout
    {
        bool built = false;
        float sunLux = 0.0f;           // luminance of the sun's illuminance, before the tint/scale
        float ev100 = 0.0f;
        bool metered = false;          // false = the level's camera exposure is off (x1)
        bool skyIbl = false;           // split-sum sky (else the flat ambient fills)
        float skyScale = 1.0f;         // what the sky's cube values are multiplied by
        bool skyCalibrated = true;     // false = a picked cube with no `_diffuse.dds` beside it
    };
    LevelLightingReadout levelReadout_;
};

#endif // WITH_EDITOR
