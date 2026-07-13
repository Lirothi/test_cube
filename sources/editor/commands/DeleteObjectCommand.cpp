#include "editor/commands/DeleteObjectCommand.h"
#if WITH_EDITOR

#include <algorithm>
#include <cassert>
#include <memory>
#include <utility>
#include <vector>

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectRegistry.h"
#include "editor/EditorContext.h"
#include "editor/scene/EnvironmentRuntime.h"
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
        if (ctx.document.IndexOf(id_, objectIndex_))
        {
            const EditorObject* obj = ctx.document.Find(id_);
            if (!obj || obj->type == "ocean")
            {
                return false;
            }
            object_ = *obj;
            isEnvironment_ = false;
        }
        else
        {
            const std::vector<EditorObject>& environment = ctx.document.Environment();
            bool found = false;
            for (std::size_t i = 0; i < environment.size(); ++i)
            {
                if (environment[i].id.value != id_.value)
                {
                    continue;
                }
                if (environment[i].type != "pointLight" && environment[i].type != "spotLight")
                {
                    return false;
                }
                objectIndex_ = i;
                object_ = environment[i];
                isEnvironment_ = true;
                found = true;
                break;
            }
            if (!found)
            {
                return false;
            }
        }
        captured_ = true;
    }

    previousSelection_ = ctx.selection;

    if (isEnvironment_)
    {
        ctx.renderer.WaitForPreviousFrame();
        std::vector<EditorObject>& environment = ctx.document.Environment();
        for (auto it = environment.begin(); it != environment.end(); ++it)
        {
            if (it->id.value == id_.value)
            {
                environment.erase(it);
                break;
            }
        }
        EnvironmentRuntime::RebuildLights(ctx);
        ctx.selection.Remove(id_);
        ctx.document.SetDirty(true);
        return true;
    }

    ctx.renderer.WaitForPreviousFrame();
    runtimeRemoved_ = ctx.scene.RemoveEditorObject(id_.value);
    ctx.document.Remove(id_);

    ctx.selection.Remove(id_);
    ctx.document.SetDirty(true);
    return true;
}

void DeleteObjectCommand::Undo(EditorContext& ctx)
{
    if (isEnvironment_)
    {
        std::vector<EditorObject>& environment = ctx.document.Environment();
        const std::size_t index = std::min(objectIndex_, environment.size());
        environment.insert(environment.begin() + static_cast<std::ptrdiff_t>(index), object_);
        EnvironmentRuntime::RebuildLights(ctx);
        ctx.selection = previousSelection_;
        ctx.document.SetDirty(true);
        return;
    }

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

    ctx.selection = previousSelection_;
    ctx.document.SetDirty(true);
}

#endif // WITH_EDITOR
