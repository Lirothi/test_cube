#include "editor/commands/SpawnMeshCommand.h"
#if WITH_EDITOR

#include <memory>
#include <string>
#include <utility>

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "editor/EditorContext.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/renderables/RenderableObjectBase.h"

namespace
{
    std::unique_ptr<RenderableObjectBase> CreateRuntime(EditorContext& ctx, const nlohmann::json& objectJson)
    {
        const std::string type = objectJson.value("type", std::string());
        if (type == "transparentMesh")
        {
            return SceneObjectFactory::CreateTransparentMeshFromJson(ctx.scene, objectJson);
        }
        if (type == "staticMesh")
        {
            return SceneObjectFactory::CreateStaticMeshFromJson(objectJson);
        }
        return nullptr;
    }
}

SpawnMeshCommand::SpawnMeshCommand(nlohmann::json objectJson)
    : objectJson_(std::move(objectJson))
{
}

bool SpawnMeshCommand::Execute(EditorContext& ctx)
{
    // Build the object's JSON + document mirror once; reuse them on redo so the
    // restored object is identical (same id, same params).
    if (!built_)
    {
        object_ = EditorSceneDocument::ObjectFromJson(ctx.document.AllocateId(), objectJson_);
        built_ = true;
    }

    previousSelection_ = ctx.selection;

    // Mirror the object in the document.
    ctx.document.Add(object_);

    // Build the runtime renderable from the same JSON via the shared factory.
    std::unique_ptr<RenderableObjectBase> runtime = CreateRuntime(ctx, objectJson_);
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

    ctx.selection.Replace(object_.id);
    ctx.document.SetDirty(true);
    return true;
}

void SpawnMeshCommand::Undo(EditorContext& ctx)
{
    ctx.renderer.WaitForPreviousFrame();
    ctx.scene.RemoveEditorObject(object_.id.value);
    ctx.document.Remove(object_.id);
    ctx.selection = previousSelection_;
    ctx.document.SetDirty(true);
}

#endif // WITH_EDITOR
