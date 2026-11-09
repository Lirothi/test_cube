#include "editor/commands/SetBuoyantCommand.h"
#if WITH_EDITOR

#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "editor/commands/SetPhysicsCommand.h" // VesselAsset: a vessel floats by default
#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/RenderableObjectBase.h"

SetBuoyantCommand::SetBuoyantCommand(EditorObjectId id, bool buoyant)
    : id_(id)
    , buoyant_(buoyant)
{
}

bool SetBuoyantCommand::DocumentBuoyant(const EditorObject& obj, bool vesselAsset)
{
    const auto it = obj.properties.find("buoyant");
    return it != obj.properties.end() && it->is_boolean() ? it->get<bool>() : vesselAsset;
}

void SetBuoyantCommand::Apply(EditorContext& ctx, EditorObjectId id, bool buoyant)
{
    if (EditorObject* obj = ctx.document.Find(id))
    {
        // Written only where it differs from the asset's default: a vessel floats unless it says false.
        const bool assetDefault = SetPhysicsCommand::VesselAsset(ctx, id);
        if (buoyant == assetDefault) { obj->properties.erase("buoyant"); }
        else { obj->properties["buoyant"] = buoyant; }
    }
    if (RenderableObjectBase* base = ctx.scene.FindEditorObject(id.value))
    {
        if (RenderableObject* object = base->AsRenderableObject())
        {
            object->SetBuoyant(buoyant);
        }
    }
    ctx.document.SetDirty(true);
}

bool SetBuoyantCommand::Execute(EditorContext& ctx)
{
    const EditorObject* obj = ctx.document.Find(id_);
    if (!obj) { return false; }
    before_ = DocumentBuoyant(*obj, SetPhysicsCommand::VesselAsset(ctx, id_));
    Apply(ctx, id_, buoyant_);
    return true;
}

void SetBuoyantCommand::Undo(EditorContext& ctx)
{
    Apply(ctx, id_, before_);
}

#endif // WITH_EDITOR
