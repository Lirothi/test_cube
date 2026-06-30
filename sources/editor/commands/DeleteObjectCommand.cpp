#include "editor/commands/DeleteObjectCommand.h"
#if WITH_EDITOR

#include <memory>
#include <utility>

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "editor/EditorContext.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/renderables/RenderableObjectBase.h"

DeleteObjectCommand::DeleteObjectCommand(EditorObjectId id)
    : id_(id)
{
}

bool DeleteObjectCommand::Execute(EditorContext& ctx)
{
    // Capture the object once so Undo (and redo) can restore the same data.
    if (!captured_)
    {
        const EditorObject* obj = ctx.document.Find(id_);
        if (!obj)
        {
            return false;
        }
        object_ = *obj;
        captured_ = true;
    }

    previousSelection_ = ctx.selectedObject;

    ctx.renderer.WaitForPreviousFrame();
    runtimeRemoved_ = ctx.scene.RemoveEditorObject(id_.value);
    ctx.document.Remove(id_);

    if (ctx.selectedObject.value == id_.value)
    {
        ctx.selectedObject = EditorObjectId{};
    }
    ctx.document.SetDirty(true);
    return true;
}

void DeleteObjectCommand::Undo(EditorContext& ctx)
{
    ctx.document.Add(object_);

    // Recreate the runtime object only if Execute actually removed one.
    if (runtimeRemoved_)
    {
        const nlohmann::json o = EditorSceneDocument::ObjectToJson(object_);
        std::unique_ptr<RenderableObjectBase> runtime =
            (object_.type == "transparentMesh")
                ? SceneObjectFactory::CreateTransparentMeshFromJson(ctx.scene, o)
                : SceneObjectFactory::CreateStaticMeshFromJson(o);
        if (runtime)
        {
            ctx.renderer.WaitForPreviousFrame();
            UploadBatch uploads;
            if (uploads.Begin(&ctx.renderer))
            {
                ctx.scene.AddInitializedEditorObject(
                    ctx.renderer, uploads, object_.id.value, std::move(runtime));
                uploads.SubmitAndWait(&ctx.renderer);
            }
        }
    }

    ctx.selectedObject = previousSelection_;
    ctx.document.SetDirty(true);
}

#endif // WITH_EDITOR
