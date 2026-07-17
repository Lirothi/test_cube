#pragma once

#include <memory>

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
    nlohmann::json ResolveMeshAsset(const nlohmann::json& objectJson);

    // Apply the shared staticMesh JSON fields to an already-created StaticMesh
    // (or subclass): rotationDeg, texOffsScale, normalStrength, useMR,
    // metalRough, renderLayer.
    void ApplyStaticMeshJsonProperties(StaticMesh& mesh, const nlohmann::json& objectJson);

    // Create a plain StaticMesh from a level/editor object JSON entry: model,
    // material, inputLayout, shader, position, scale, plus the shared properties
    // above.
    std::unique_ptr<RenderableObjectBase> CreateStaticMeshFromJson(const nlohmann::json& objectJson);

    // Create a TransparentStaticMesh from a level/editor object JSON entry.
    std::unique_ptr<RenderableObjectBase> CreateTransparentMeshFromJson(Scene& scene, const nlohmann::json& objectJson);

    // E3: create a GPU particle emitter from a level/editor object JSON entry. Resolves the
    // EmitterDesc from preset/overrides/inline (vfx::ResolveEmitterDesc) and applies the
    // position/rotation/scale transform. Shared by level loading and editor spawn.
    std::unique_ptr<RenderableObjectBase> CreateParticleEmitterFromJson(const nlohmann::json& objectJson);
}
