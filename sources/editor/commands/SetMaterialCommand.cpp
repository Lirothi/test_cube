#include "editor/commands/SetMaterialCommand.h"
#if WITH_EDITOR

#include <memory>
#include <utility>

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "editor/EditorContext.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/renderables/RenderableObjectBase.h"

SetMaterialCommand::SetMaterialCommand(EditorObjectId id, std::string material)
    : id_(id)
    , newMaterial_(std::move(material))
{
}

bool SetMaterialCommand::Apply(EditorContext& ctx, const std::string& material)
{
    EditorObject* obj = ctx.document.Find(id_);
    if (!obj)
    {
        return false;
    }

    obj->properties["material"] = material;
    ctx.document.SetDirty(true);

    // Respawn only if this object has a live editor-owned runtime.
    if (ctx.scene.FindEditorObject(id_.value) != nullptr)
    {
        const nlohmann::json json = EditorSceneDocument::ObjectToJson(*obj);
        std::unique_ptr<RenderableObjectBase> runtime = SceneObjectFactory::CreateStaticMeshFromJson(json);
        if (runtime)
        {
            ctx.renderer.WaitForPreviousFrame();
            ctx.scene.RemoveEditorObject(id_.value);
            UploadBatch uploads;
            if (uploads.Begin(&ctx.renderer))
            {
                ctx.scene.AddInitializedEditorObject(ctx.renderer, uploads, id_.value, std::move(runtime));
                uploads.SubmitAndWait(&ctx.renderer);
            }
        }
    }
    return true;
}

bool SetMaterialCommand::Execute(EditorContext& ctx)
{
    if (!captured_)
    {
        const EditorObject* obj = ctx.document.Find(id_);
        if (!obj)
        {
            return false;
        }
        oldMaterial_ = obj->properties.value("material", std::string());
        captured_ = true;
    }
    return Apply(ctx, newMaterial_);
}

void SetMaterialCommand::Undo(EditorContext& ctx)
{
    Apply(ctx, oldMaterial_);
}

#endif // WITH_EDITOR
