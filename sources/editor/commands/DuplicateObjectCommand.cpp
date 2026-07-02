#include "editor/commands/DuplicateObjectCommand.h"
#if WITH_EDITOR

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectRegistry.h"
#include "editor/EditorContext.h"
#include "editor/scene/EnvironmentRuntime.h"
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
        if (const EditorObject* source = ctx.document.Find(sourceId_))
        {
            // A document object (mesh): recreated below via the object registry.
            if (source->type == "ocean")
            {
                return false;
            }
            object_ = *source;
            object_.id = ctx.document.AllocateId();
            object_.name = DuplicateName(source->name);
            isEnvironment_ = false;
            built_ = true;
        }
        else
        {
            // Not a document object: only environment point/spot lights can be
            // duplicated. Camera, skybox, directional light and ocean are
            // singletons with no meaningful copy.
            bool foundLight = false;
            for (const EditorObject& env : ctx.document.Environment())
            {
                if (env.id.value != sourceId_.value)
                {
                    continue;
                }
                if (env.type != "pointLight" && env.type != "spotLight")
                {
                    return false;
                }
                object_ = env;
                object_.id = ctx.document.AllocateId();
                object_.name = DuplicateName(env.name);
                isEnvironment_ = true;
                built_ = true;
                foundLight = true;
                break;
            }
            if (!foundLight)
            {
                return false;
            }
        }
    }

    if (isEnvironment_)
    {
        // Lights are not runtime scene objects; the duplicate is a new environment
        // entity that RebuildLights folds into the LightManager (CPU-only, no GPU
        // upload). The whole-scene idle keeps it consistent with the object path.
        previousSelection_ = ctx.selectedObject;
        ctx.renderer.WaitForPreviousFrame();
        ctx.document.Environment().push_back(object_);
        EnvironmentRuntime::RebuildLights(ctx);
        ctx.selectedObject = object_.id;
        ctx.document.SetDirty(true);
        return true;
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
    if (isEnvironment_)
    {
        std::vector<EditorObject>& environment = ctx.document.Environment();
        for (auto it = environment.begin(); it != environment.end(); ++it)
        {
            if (it->id.value == object_.id.value)
            {
                environment.erase(it);
                break;
            }
        }
        EnvironmentRuntime::RebuildLights(ctx);
    }
    else
    {
        ctx.scene.RemoveEditorObject(object_.id.value);
        ctx.document.Remove(object_.id);
    }
    ctx.selectedObject = previousSelection_;
    ctx.document.SetDirty(true);
}

#endif // WITH_EDITOR
