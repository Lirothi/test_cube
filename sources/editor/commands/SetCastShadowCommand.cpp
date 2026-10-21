#include "editor/commands/SetCastShadowCommand.h"
#if WITH_EDITOR

#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/RenderableObjectBase.h"

SetCastShadowCommand::SetCastShadowCommand(EditorObjectId id, bool castShadow)
    : id_(id)
    , castShadow_(castShadow)
{
}

bool SetCastShadowCommand::DocumentCastsShadow(const EditorObject& obj)
{
    const auto it = obj.properties.find("castShadow");
    return !(it != obj.properties.end() && it->is_boolean() && !it->get<bool>());
}

void SetCastShadowCommand::Apply(EditorContext& ctx, EditorObjectId id, bool castShadow)
{
    if (EditorObject* obj = ctx.document.Find(id))
    {
        if (castShadow) { obj->properties.erase("castShadow"); }
        else { obj->properties["castShadow"] = false; }
    }
    if (RenderableObjectBase* base = ctx.scene.FindEditorObject(id.value))
    {
        if (RenderableObject* object = base->AsRenderableObject())
        {
            const bool changed = object->CastsShadow() != castShadow;
            object->SetCastsShadow(castShadow);
            // The caster set changed: the GPU caster tables, the mega buffer, the shared shadow queue and the shadow
            // caches rebuild once after this command (or the composite it is in) -- SetEnabledCommand's road.
            if (changed && object->IsVisible())
            {
                ctx.scene.NotifyEditorShadowCasterVisibilityChanged();
            }
        }
    }
    ctx.document.SetDirty(true);
}

bool SetCastShadowCommand::Execute(EditorContext& ctx)
{
    const EditorObject* obj = ctx.document.Find(id_);
    if (!obj) { return false; }
    before_ = DocumentCastsShadow(*obj);
    Apply(ctx, id_, castShadow_);
    return true;
}

void SetCastShadowCommand::Undo(EditorContext& ctx)
{
    Apply(ctx, id_, before_);
}

#endif // WITH_EDITOR
