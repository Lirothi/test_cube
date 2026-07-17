#include "editor/commands/SetMeshAssetCommand.h"
#if WITH_EDITOR

#include <memory>
#include <utility>

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "editor/EditorContext.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/renderables/RenderableObjectBase.h"

namespace
{
    void ClearLegacyMeshFields(nlohmann::json& properties)
    {
        // A legacy staticMesh stores its render setup inline. Once it is converted
        // to a .mesh.json reference, those fields would win during ResolveMeshAsset
        // and prevent the selected asset from supplying its own defaults.
        static constexpr const char* kFields[] = {
            "model", "material", "materials", "shader", "inputLayout", "renderLayer",
            "texOffsScale", "normalStrength", "useMR", "emissiveColor",
            "emissiveStrength", "metalRough"
        };
        for (const char* field : kFields)
        {
            properties.erase(field);
        }
    }
}

SetMeshAssetCommand::SetMeshAssetCommand(EditorObjectId id, std::string meshAssetPath)
    : id_(id)
    , newMeshAssetPath_(std::move(meshAssetPath))
{
}

bool SetMeshAssetCommand::Apply(EditorContext& ctx, const nlohmann::json& properties)
{
    EditorObject* obj = ctx.document.Find(id_);
    if (!obj || obj->type != "staticMesh")
    {
        return false;
    }

    obj->properties = properties;
    ctx.document.SetDirty(true);

    if (ctx.scene.FindEditorObject(id_.value) == nullptr)
    {
        return true;
    }

    const nlohmann::json json = EditorSceneDocument::ObjectToJson(*obj);
    std::unique_ptr<RenderableObjectBase> runtime =
        SceneObjectFactory::CreateStaticMeshFromJson(json);
    if (!runtime)
    {
        return false;
    }

    ctx.renderer.WaitForPreviousFrame();
    ctx.scene.RemoveEditorObject(id_.value);
    UploadBatch uploads;
    if (uploads.Begin(&ctx.renderer))
    {
        ctx.scene.AddInitializedEditorObject(ctx.renderer, uploads, id_.value, std::move(runtime));
        uploads.SubmitAndWait(&ctx.renderer);
    }
    return true;
}

bool SetMeshAssetCommand::Execute(EditorContext& ctx)
{
    if (!captured_)
    {
        const EditorObject* obj = ctx.document.Find(id_);
        if (!obj || obj->type != "staticMesh" || newMeshAssetPath_.empty())
        {
            return false;
        }

        oldProperties_ = obj->properties;
        newProperties_ = oldProperties_;
        const bool alreadyAssetBacked = newProperties_.contains("mesh") &&
            newProperties_["mesh"].is_string();
        if (!alreadyAssetBacked)
        {
            ClearLegacyMeshFields(newProperties_);
        }
        newProperties_["mesh"] = newMeshAssetPath_;
        captured_ = true;
    }
    return Apply(ctx, newProperties_);
}

void SetMeshAssetCommand::Undo(EditorContext& ctx)
{
    Apply(ctx, oldProperties_);
}

#endif // WITH_EDITOR
