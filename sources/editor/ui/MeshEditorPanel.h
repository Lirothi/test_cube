#pragma once
#if WITH_EDITOR

#include <cstdint>
#include <string>
#include <vector>

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
    // Load a `.mesh.json` for editing (content-browser EditMesh action). Resets prior state.
    void Open(const std::string& meshAssetPath);

    // Draw the window body (inside the editor's lambda panel). `open` backs the close button.
    // `ctx` gives access to the scene document so Save can live-apply to placed instances.
    void Draw(EditorContext& ctx, AssetRegistry& registry, bool* open);

    const std::string& CurrentPath() const { return path_; }

private:
    void Save(EditorContext& ctx, AssetRegistry& registry);
    // Live-apply: respawn every placed object that references this mesh asset (and doesn't override
    // the changed key) so the edit shows immediately. Returns the number of instances updated.
    int  ApplyToScene(EditorContext& ctx) const;

    std::string   path_;             // the .mesh.json path currently being edited
    nlohmann::json doc_;             // parsed document (the round-trip base — unknown keys preserved)
    bool          loaded_ = false;
    std::vector<std::string> slots_; // one material preset per submesh (auto-sized to the geometry)
    std::vector<uint32_t> recomputeNormalSlots_; // submesh slots that discard authored normals
    std::string   status_;
};

#endif // WITH_EDITOR
