#include "editor/commands/DeleteObjectCommand.h"
#if WITH_EDITOR

#include <cassert>
#include <memory>
#include <utility>

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectRegistry.h"
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
        if (!ctx.document.IndexOf(id_, objectIndex_))
        {
            return false;
        }

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
    ctx.document.Insert(object_, objectIndex_);

    // Recreate the runtime object only if Execute actually removed one.
    if (runtimeRemoved_)
    {
        const nlohmann::json o = EditorSceneDocument::ObjectToJson(object_);
        SceneObjectRegistry objectRegistry = SceneObjectRegistry::CreateWithBuiltins();
        SceneObjectRegistry::CreationContext creationCtx{ ctx.scene };
        SceneObjectRegistry::ObjectList runtimeObjects =
            objectRegistry.Create(object_.type, creationCtx, o);
        if (!runtimeObjects.empty())
        {
            ctx.renderer.WaitForPreviousFrame();
            UploadBatch uploads;
            if (uploads.Begin(&ctx.renderer))
            {
                bool addedAny = false;
                for (std::unique_ptr<RenderableObjectBase>& runtime : runtimeObjects)
                {
                    if (!runtime)
                    {
                        continue;
                    }

                    runtime->SetVisible(object_.enabled);
                    addedAny |= ctx.scene.AddInitializedEditorObject(
                        ctx.renderer, uploads, object_.id.value, std::move(runtime));
                }

                if (addedAny)
                {
                    uploads.SubmitAndWait(&ctx.renderer);

#ifndef NDEBUG
                    if (object_.type == "staticMesh" &&
                        o.value("rotateSpeedDeg", 0.0f) != 0.0f)
                    {
                        const RenderableObjectBase* restored =
                            ctx.scene.FindEditorObject(object_.id.value);
                        assert(restored && restored->IsDynamicCaster() &&
                            "Undo must restore the rotating static-mesh runtime type");
                    }
#endif
                }
            }
        }
    }

    ctx.selectedObject = previousSelection_;
    ctx.document.SetDirty(true);
}

#endif // WITH_EDITOR
