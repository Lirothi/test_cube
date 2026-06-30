#include "editor/commands/SetEnabledCommand.h"
#if WITH_EDITOR

#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "rendering/renderables/RenderableObjectBase.h"

SetEnabledCommand::SetEnabledCommand(EditorObjectId id, bool enabled)
    : id_(id)
    , enabled_(enabled)
{
}

void SetEnabledCommand::Apply(EditorContext& ctx, bool enabled)
{
    if (EditorObject* obj = ctx.document.Find(id_))
    {
        obj->enabled = enabled;
    }

    // Runtime visibility (skipped during bucketization when hidden).
    if (RenderableObjectBase* base = ctx.scene.FindEditorObject(id_.value))
    {
        base->SetVisible(enabled);
    }

    ctx.document.SetDirty(true);
}

bool SetEnabledCommand::Execute(EditorContext& ctx)
{
    Apply(ctx, enabled_);
    return true;
}

void SetEnabledCommand::Undo(EditorContext& ctx)
{
    Apply(ctx, !enabled_);
}

#endif // WITH_EDITOR
