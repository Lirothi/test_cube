#pragma once
#if WITH_EDITOR

#include <string>

#include "third_party/json/json.hpp"

class AssetRegistry;
struct EditorContext;

// Part I2 — Material Editor window. Opened by double-clicking a `data/materials/<name>.json` in the
// content browser (or the "Edit Material" context item). Edits the schema-v2 fields — albedo / mr /
// normal / emissive texture pickers, tint, metal/rough, normal strength, texOffsScale tiling,
// alphaTest / alphaCutoff / twoSided, emissive color/strength, normalIsRG, and the I0
// feature-shader override — and writes them back to the file (round-trip, unknown keys preserved).
//
// Save is one safe live-apply mechanism (the spec's two tiers collapse into a respawn): write file
// -> re-register the preset + evict its cached MaterialData -> respawn every placed object whose
// EFFECTIVE material (after mesh-asset + slot resolution) is this one. Respawn rebuilds textures,
// params AND the per-slot PSO from the new definition, so scalar edits and PSO-affecting toggles
// (alphaTest/twoSided/normalIsRG) all show immediately without descriptor surgery.
class MaterialEditorPanel
{
public:
    // Load a material for editing. `materialName` = the file stem (how levels/slots reference it);
    // `filePath` = the data/materials/<name>.json on disk. Resets any prior edit state.
    void Open(const std::string& materialName, const std::string& filePath);

    // Draw the window body (inside the editor's lambda panel). `open` backs the close button; `ctx`
    // gives the scene/document/renderer so Save can live-apply to placed instances.
    void Draw(EditorContext& ctx, AssetRegistry& registry, bool* open);

    const std::string& CurrentName() const { return name_; }

private:
    void Save(EditorContext& ctx, AssetRegistry& registry);
    // Respawn every placed static mesh whose effective material is this one. Returns the count.
    int  ApplyToScene(EditorContext& ctx) const;

    std::string    name_;   // material name (= file stem)
    std::string    path_;   // data/materials/<name>.json
    nlohmann::json doc_;    // parsed document — the round-trip base (unknown keys preserved)
    bool           loaded_ = false;
    std::string    status_;
};

#endif // WITH_EDITOR
