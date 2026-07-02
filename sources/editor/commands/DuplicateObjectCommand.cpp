#include "editor/commands/DuplicateObjectCommand.h"
#if WITH_EDITOR

#include <memory>
#include <string>
#include <utility>

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectRegistry.h"
#include "editor/EditorContext.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/renderables/RenderableObjectBase.h"

namespace
{
    std::string DuplicateName(const std::string& name)
    {
        return name.empty() ? std::string("Copy") : name + " Copy";
    }
}

DuplicateObjectCommand::DuplicateObjectCommand(EditorObjectId sourceId)
    : sourceId_(sourceId)
{
}

bool DuplicateObjectCommand::Execute(EditorContext& ctx)
{
    if (!built_)
    {
        const EditorObject* source = ctx.document.Find(sourceId_);
        if (!source || source->type == "ocean")
        {
            return false;
        }

        object_ = *source;
        object_.id = ctx.document.AllocateId();
        object_.name = DuplicateName(source->name);
        built_ = true;
    }

    const nlohmann::json objectJson = EditorSceneDocument::ObjectToJson(object_);
    SceneObjectRegistry objectRegistry = SceneObjectRegistry::CreateWithBuiltins();
    if (!objectRegistry.Has(object_.type))
    {
        return false;
    }

    SceneObjectRegistry::CreationContext creationCtx{ ctx.scene };
    SceneObjectRegistry::ObjectList runtimeObjects = objectRegistry.Create(object_.type, creationCtx, objectJson);
    if (runtimeObjects.empty())
    {
        return false;
    }

    previousSelection_ = ctx.selectedObject;
    ctx.document.Add(object_);

    ctx.renderer.WaitForPreviousFrame();
    UploadBatch uploads;
    if (!uploads.Begin(&ctx.renderer))
    {
        ctx.document.Remove(object_.id);
        return false;
    }

    bool addedAny = false;
    for (std::unique_ptr<RenderableObjectBase>& runtime : runtimeObjects)
    {
        if (!runtime)
        {
            continue;
        }

        runtime->SetVisible(object_.enabled);
        if (!ctx.scene.AddInitializedEditorObject(
            ctx.renderer, uploads, object_.id.value, std::move(runtime)))
        {
            ctx.scene.RemoveEditorObject(object_.id.value);
            ctx.document.Remove(object_.id);
            return false;
        }
        addedAny = true;
    }

    if (!addedAny)
    {
        ctx.document.Remove(object_.id);
        return false;
    }

    uploads.SubmitAndWait(&ctx.renderer);
    ctx.selectedObject = object_.id;
    ctx.document.SetDirty(true);
    return true;
}

void DuplicateObjectCommand::Undo(EditorContext& ctx)
{
    ctx.renderer.WaitForPreviousFrame();
    ctx.scene.RemoveEditorObject(object_.id.value);
    ctx.document.Remove(object_.id);
    ctx.selectedObject = previousSelection_;
    ctx.document.SetDirty(true);
}

#endif // WITH_EDITOR
