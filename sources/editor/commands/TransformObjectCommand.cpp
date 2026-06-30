#include "editor/commands/TransformObjectCommand.h"
#if WITH_EDITOR

#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "rendering/renderables/RenderableObject.h"

TransformObjectCommand::TransformObjectCommand(EditorObjectId id, const EditorTransform& before, const EditorTransform& after)
    : id_(id)
    , before_(before)
    , after_(after)
{
}

void TransformObjectCommand::ApplyTransform(EditorContext& ctx, EditorObjectId id, const EditorTransform& t)
{
    if (EditorObject* obj = ctx.document.Find(id))
    {
        obj->transform = t;
    }

    // Runtime objects derived from RenderableObject get the live transform. Demo
    // objects loaded at level load have no editor-owned runtime (SceneObjectId 0),
    // so FindEditorObject returns null and only the document is updated.
    RenderableObjectBase* runtime = ctx.scene.FindEditorObject(id.value);
    if (RenderableObject* ro = runtime ? runtime->AsRenderableObject() : nullptr)
    {
        ro->SetPosition(t.position);
        ro->SetRotationEulerDeg(t.rotationDeg);
        ro->SetScale(t.scale);
    }

    ctx.document.SetDirty(true);
}

bool TransformObjectCommand::Execute(EditorContext& ctx)
{
    ApplyTransform(ctx, id_, after_);
    return true;
}

void TransformObjectCommand::Undo(EditorContext& ctx)
{
    ApplyTransform(ctx, id_, before_);
}

#endif // WITH_EDITOR
