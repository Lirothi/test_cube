#include "editor/commands/SetLampSwitchCommand.h"
#if WITH_EDITOR

#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "rendering/renderables/GBufferRenderable.h"
#include "rendering/renderables/RenderableObjectBase.h"

SetLampSwitchCommand::SetLampSwitchCommand(EditorObjectId id, LampSwitch lampSwitch)
    : id_(id)
    , lampSwitch_(lampSwitch)
{
}

void SetLampSwitchCommand::Apply(EditorContext& ctx, EditorObjectId id, LampSwitch lampSwitch)
{
    if (EditorObject* obj = ctx.document.Find(id))
    {
        if (lampSwitch != LampSwitch::Always) { obj->properties["onAt"] = LampSwitchName(lampSwitch); }
        else { obj->properties.erase("onAt"); }
    }
    if (RenderableObjectBase* base = ctx.scene.FindEditorObject(id.value))
    {
        if (GBufferRenderable* object = base->AsGBufferRenderable())
        {
            object->SetLampSwitch(lampSwitch);
        }
    }
    ctx.document.SetDirty(true);
}

bool SetLampSwitchCommand::Execute(EditorContext& ctx)
{
    const EditorObject* obj = ctx.document.Find(id_);
    if (!obj) { return false; }
    const auto it = obj->properties.find("onAt");
    before_ = it != obj->properties.end() && it->is_string() ? LampSwitchFromName(it->get<std::string>()) : LampSwitch::Always;
    Apply(ctx, id_, lampSwitch_);
    return true;
}

void SetLampSwitchCommand::Undo(EditorContext& ctx)
{
    Apply(ctx, id_, before_);
}

#endif // WITH_EDITOR
