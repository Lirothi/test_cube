#include "editor/commands/SpawnMeshCommand.h"
#if WITH_EDITOR

#include <memory>
#include <utility>

#include "app/camera/Camera.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/renderables/RenderableObjectBase.h"

namespace
{
    // A position 5 units in front of the camera, as a JSON [x, y, z] array.
    nlohmann::json SpawnPositionJson(const Scene& scene)
    {
        const Math::float3& camPos = scene.CameraRef().GetPosition();
        const Math::float3& camDir = scene.CameraRef().GetDirection();
        return nlohmann::json::array({
            camPos.x + camDir.x * 5.0f,
            camPos.y + camDir.y * 5.0f,
            camPos.z + camDir.z * 5.0f });
    }
}

SpawnMeshCommand::SpawnMeshCommand(Kind kind, std::string modelPath, std::string staticMaterial)
    : kind_(kind)
    , modelPath_(std::move(modelPath))
    , staticMaterial_(std::move(staticMaterial))
{
}

bool SpawnMeshCommand::Execute(EditorContext& ctx)
{
    // Build the object's JSON + document mirror once; reuse them on redo so the
    // restored object is identical (same id, same params).
    if (!built_)
    {
        nlohmann::json o = nlohmann::json::object();
        o["model"] = modelPath_;
        o["position"] = SpawnPositionJson(ctx.scene);
        o["scale"] = nlohmann::json::array({ 1.0f, 1.0f, 1.0f });

        if (kind_ == Kind::TransparentMesh)
        {
            o["type"] = "transparentMesh";
            // Default glass params: copy from an existing transparentMesh in the
            // document if one exists (keeping our own model).
            for (const EditorObject& existing : ctx.document.Objects())
            {
                if (existing.type == "transparentMesh")
                {
                    for (auto it = existing.properties.begin(); it != existing.properties.end(); ++it)
                    {
                        if (it.key() != "model")
                        {
                            o[it.key()] = it.value();
                        }
                    }
                    break;
                }
            }
        }
        else
        {
            o["type"] = "staticMesh";
            o["material"] = staticMaterial_;
            o["shader"] = "shaders/gbuffer.hlsl";
            o["inputLayout"] = "PosNormTanUV";
        }

        objectJson_ = std::move(o);
        object_ = EditorSceneDocument::ObjectFromJson(ctx.document.AllocateId(), objectJson_);
        built_ = true;
    }

    previousSelection_ = ctx.selectedObject;

    // Mirror the object in the document.
    ctx.document.Add(object_);

    // Build the runtime renderable from the same JSON via the shared factory.
    std::unique_ptr<RenderableObjectBase> runtime =
        (kind_ == Kind::TransparentMesh)
            ? SceneObjectFactory::CreateTransparentMeshFromJson(ctx.scene, objectJson_)
            : SceneObjectFactory::CreateStaticMeshFromJson(objectJson_);
    if (!runtime)
    {
        ctx.document.Remove(object_.id);
        return false;
    }

    // Mutate the live scene with a full GPU sync around the change. Safe here:
    // this runs in the editor draw/Tick window, before scene render recording.
    ctx.renderer.WaitForPreviousFrame();
    UploadBatch uploads;
    if (!uploads.Begin(&ctx.renderer))
    {
        ctx.document.Remove(object_.id);
        return false;
    }
    const bool added = ctx.scene.AddInitializedEditorObject(
        ctx.renderer, uploads, object_.id.value, std::move(runtime));
    uploads.SubmitAndWait(&ctx.renderer);

    if (!added)
    {
        ctx.document.Remove(object_.id);
        return false;
    }

    ctx.selectedObject = object_.id;
    ctx.document.SetDirty(true);
    return true;
}

void SpawnMeshCommand::Undo(EditorContext& ctx)
{
    ctx.renderer.WaitForPreviousFrame();
    ctx.scene.RemoveEditorObject(object_.id.value);
    ctx.document.Remove(object_.id);
    ctx.selectedObject = previousSelection_;
    ctx.document.SetDirty(true);
}

#endif // WITH_EDITOR
