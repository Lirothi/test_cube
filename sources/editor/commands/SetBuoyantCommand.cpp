#include "editor/commands/SetBuoyantCommand.h"
#if WITH_EDITOR

#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/RenderableObjectBase.h"

SetBuoyantCommand::SetBuoyantCommand(EditorObjectId id, bool buoyant)
    : id_(id)
    , buoyant_(buoyant)
{
}

void SetBuoyantCommand::Apply(EditorContext& ctx, EditorObjectId id, bool buoyant)
{
    if (EditorObject* obj = ctx.document.Find(id))
    {
        if (buoyant) { obj->properties["buoyant"] = true; }
        else { obj->properties.erase("buoyant"); }
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
    const auto it = obj->properties.find("buoyant");
    before_ = it != obj->properties.end() && it->is_boolean() && it->get<bool>();
    Apply(ctx, id_, buoyant_);
    return true;
}

void SetBuoyantCommand::Undo(EditorContext& ctx)
{
    Apply(ctx, id_, before_);
}

#endif // WITH_EDITOR
