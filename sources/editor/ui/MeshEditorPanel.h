#pragma once
#if WITH_EDITOR

#include <string>
#include <vector>

#include "third_party/json/json.hpp"

class AssetRegistry;

// Part J — dedicated Mesh Editor window. Opened by double-clicking a `models/<name>.mesh.json`
// asset in the content browser. Edits the mesh asset's render defaults — per-slot materials,
// renderLayer, spawnScale, texOffsScale tiling — and writes them back to the file (round-trip
// preserving unknown keys). Shader is intentionally NOT editable here: it is a MATERIAL concern
// (Part I0), never a mesh one.
class MeshEditorPanel
{
public:
    // Load a `.mesh.json` for editing (content-browser EditMesh action). Resets prior state.
    void Open(const std::string& meshAssetPath);

    // Draw the window body (inside the editor's lambda panel). `open` backs the close button.
    void Draw(AssetRegistry& registry, bool* open);

    const std::string& CurrentPath() const { return path_; }

private:
    void Save(AssetRegistry& registry);

    std::string   path_;             // the .mesh.json path currently being edited
    nlohmann::json doc_;             // parsed document (the round-trip base — unknown keys preserved)
    bool          loaded_ = false;
    std::vector<std::string> slots_; // one material preset per submesh (auto-sized to the geometry)
    std::string   status_;
};

#endif // WITH_EDITOR
