#include "editor/commands/SetMaterialSlotCommand.h"
#if WITH_EDITOR

#include <algorithm>
#include <memory>
#include <utility>

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "editor/EditorContext.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/renderables/GBufferRenderable.h"
#include "rendering/renderables/RenderableObjectBase.h"

SetMaterialSlotCommand::SetMaterialSlotCommand(EditorObjectId id, int slot, std::string material)
    : id_(id)
    , slot_(slot)
    , newMaterial_(std::move(material))
{
}

void SetMaterialSlotCommand::Respawn(EditorContext& ctx, EditorObjectId id) const
{
    if (ctx.scene.FindEditorObject(id.value) == nullptr)
    {
        return; // no live runtime (e.g. object disabled) — the document change is enough
    }
    const EditorObject* obj = ctx.document.Find(id);
    if (!obj)
    {
        return;
    }
    const nlohmann::json json = EditorSceneDocument::ObjectToJson(*obj);
    std::unique_ptr<RenderableObjectBase> runtime = SceneObjectFactory::CreateStaticMeshFromJson(json);
    if (!runtime)
    {
        return;
    }
    ctx.renderer.WaitForPreviousFrame();
    ctx.scene.RemoveEditorObject(id.value);
    UploadBatch uploads;
    if (uploads.Begin(&ctx.renderer))
    {
        ctx.scene.AddInitializedEditorObject(ctx.renderer, uploads, id.value, std::move(runtime));
        uploads.SubmitAndWait(&ctx.renderer);
    }
}

bool SetMaterialSlotCommand::Apply(EditorContext& ctx, const std::string& material)
{
    EditorObject* obj = ctx.document.Find(id_);
    if (!obj || slot_ < 0)
    {
        return false;
    }

    // How many slots does this object have? Prefer the live runtime (glTF asset => one slot per
    // submesh); fall back to any existing array or just the edited slot.
    size_t slotCount = static_cast<size_t>(slot_) + 1;
    if (RenderableObjectBase* runtime = ctx.scene.FindEditorObject(id_.value))
    {
        if (GBufferRenderable* gb = runtime->AsGBufferRenderable())
        {
            slotCount = std::max(slotCount, gb->SlotCount());
        }
    }
    if (obj->properties.contains("materials") && obj->properties["materials"].is_array())
    {
        slotCount = std::max(slotCount, obj->properties["materials"].size());
    }

    // Synthesize the array from the current effective state: slot 0 inherits the scalar
    // "material" (or "auto"), the rest default to "auto" (pull from the glTF).
    const std::string slot0Default = obj->properties.value("material", std::string("auto"));
    nlohmann::json materials = nlohmann::json::array();
    for (size_t i = 0; i < slotCount; ++i)
    {
        if (obj->properties.contains("materials") &&
            obj->properties["materials"].is_array() &&
            i < obj->properties["materials"].size() &&
            obj->properties["materials"][i].is_string())
        {
            materials.push_back(obj->properties["materials"][i]);
        }
        else
        {
            materials.push_back(i == 0 ? slot0Default : std::string("auto"));
        }
    }
    materials[static_cast<size_t>(slot_)] = material;

    obj->properties["materials"] = std::move(materials);
    ctx.document.SetDirty(true);
    Respawn(ctx, id_);
    return true;
}

bool SetMaterialSlotCommand::Execute(EditorContext& ctx)
{
    if (!captured_)
    {
        const EditorObject* obj = ctx.document.Find(id_);
        if (!obj)
        {
            return false;
        }
        oldHadMaterials_ = obj->properties.contains("materials");
        oldMaterials_ = oldHadMaterials_ ? obj->properties["materials"] : nlohmann::json();
        captured_ = true;
    }
    return Apply(ctx, newMaterial_);
}

void SetMaterialSlotCommand::Undo(EditorContext& ctx)
{
    EditorObject* obj = ctx.document.Find(id_);
    if (!obj)
    {
        return;
    }
    if (oldHadMaterials_)
    {
        obj->properties["materials"] = oldMaterials_;
    }
    else
    {
        obj->properties.erase("materials");
    }
    ctx.document.SetDirty(true);
    Respawn(ctx, id_);
}

#endif // WITH_EDITOR
