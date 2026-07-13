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

    // Runtime visibility (skipped during bucketization when hidden). Visibility
    // also changes the indirect/VSM caster set. Mark it stale so the command
    // stack rebuilds the mega-buffer once after this command or a composite.
    if (RenderableObjectBase* base = ctx.scene.FindEditorObject(id_.value))
    {
        const bool visibilityChanged = base->IsVisible() != enabled;
        base->SetVisible(enabled);
        if (visibilityChanged && base->CastsShadow())
        {
            ctx.scene.NotifyEditorShadowCasterVisibilityChanged();
        }
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
