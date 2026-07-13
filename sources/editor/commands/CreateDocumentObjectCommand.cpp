#include "editor/commands/CreateDocumentObjectCommand.h"
#if WITH_EDITOR

#include <utility>

#include "editor/EditorContext.h"

CreateDocumentObjectCommand::CreateDocumentObjectCommand(EditorObject object)
    : object_(std::move(object))
{
}

bool CreateDocumentObjectCommand::Execute(EditorContext& ctx)
{
    if (object_.type.empty())
    {
        return false;
    }

    if (!built_)
    {
        object_.id = ctx.document.AllocateId();
        built_ = true;
    }

    previousSelection_ = ctx.selection;
    ctx.document.Add(object_);
    ctx.selection.Replace(object_.id);
    ctx.document.SetDirty(true);
    return true;
}

void CreateDocumentObjectCommand::Undo(EditorContext& ctx)
{
    ctx.document.Remove(object_.id);
    ctx.selection = previousSelection_;
    ctx.document.SetDirty(true);
}

#endif // WITH_EDITOR
