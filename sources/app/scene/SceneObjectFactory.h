#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// nlohmann/json — single header. The factory functions are JSON-driven so engine
// code (and later the editor) can share mesh creation without depending on
// editor types.
#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

class RenderableObjectBase;
class StaticMesh;
class Scene;

// Shared creation of the data-driven mesh object types (staticMesh,
// transparentMesh) used by both level loading and (later) the editor. Demo- or
// generator-specific types (RotatingObject, instancedModels, ocean) live in
// SceneObjectRegistry creators.
namespace SceneObjectFactory
{
    // J1: expand a mesh-asset reference. If `objectJson` has a `"mesh"` key (path to a
    // models/<name>.mesh.json), fold that asset's render defaults UNDER the object's own keys
    // (object wins; `geometry`->`model`; `spawnScale` skipped as a spawn-time hint) and return the
    // effective JSON. Returns the input unchanged when there is no `mesh` key or the file is
    // unreadable. Public so alternate creators (RotatingObject) resolve the reference too.
    struct MeshAssetScanCache;
    nlohmann::json ResolveMeshAsset(const nlohmann::json& objectJson,
                                    MeshAssetScanCache* cache = nullptr);

    // Apply the shared staticMesh JSON fields to an already-created StaticMesh
    // (or subclass): rotationDeg, texOffsScale, normalStrength, useMR,
    // metalRough, renderLayer.
    void ApplyStaticMeshJsonProperties(StaticMesh& mesh, const nlohmann::json& objectJson);

    // Create a plain StaticMesh from a level/editor object JSON entry: model,
    // material, inputLayout, shader, position, scale, plus the shared properties
    // above.
    std::unique_ptr<RenderableObjectBase> CreateStaticMeshFromJson(const nlohmann::json& objectJson);

    // Level-error detection: return every missing-asset problem for a mesh object (empty = healthy).
    // Checks the geometry (mesh.json + geometry file), each named material preset
    // (data/materials/<name>.json; "auto"/glTF-embedded skipped), and that preset's albedo/mr/normal
    // texture files exist (honoring H2 .dds-sibling resolution). Pure/CPU, no GPU or editor deps.
    // Memo for a WHOLE scan. Each call opens and parses the object's mesh.json and every material
    // .json it names, then stats each texture. A level whose objects share assets — any foliage
    // level — otherwise re-reads the same handful of files once per object: measured at 926 ms for
    // a single pass over wind_test, which is a visible hitch every time the editor rescans.
    // Pass one cache for the whole loop; nullptr keeps the old standalone behaviour.
    struct MeshAssetScanCache
    {
        std::unordered_map<std::string, bool> fileExists;
        std::unordered_map<std::string, std::vector<std::string>> materialErrors; // by preset name
        std::unordered_map<std::string, nlohmann::json> meshAssets;               // by mesh.json path
    };

    std::vector<std::string> MeshAssetErrors(const nlohmann::json& objectJson,
                                             MeshAssetScanCache* cache = nullptr);

    // Create a TransparentStaticMesh from a level/editor object JSON entry.
    std::unique_ptr<RenderableObjectBase> CreateTransparentMeshFromJson(Scene& scene, const nlohmann::json& objectJson);

    // E3: create a GPU particle emitter from a level/editor object JSON entry. Resolves the
    // EmitterDesc from preset/overrides/inline (vfx::ResolveEmitterDesc) and applies the
    // position/rotation/scale transform. Shared by level loading and editor spawn.
    std::unique_ptr<RenderableObjectBase> CreateParticleEmitterFromJson(const nlohmann::json& objectJson);
}
