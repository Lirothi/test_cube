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
}
