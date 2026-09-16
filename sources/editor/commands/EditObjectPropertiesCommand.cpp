#include "editor/commands/EditObjectPropertiesCommand.h"
#if WITH_EDITOR

#include <utility>

#include "editor/EditorContext.h"

EditObjectPropertiesCommand::EditObjectPropertiesCommand(
    EditorObjectId id,
    nlohmann::json beforeProperties,
    nlohmann::json afterProperties,
    std::string historyLabel)
    : id_(id)
    , beforeProperties_(std::move(beforeProperties))
    , afterProperties_(std::move(afterProperties))
    , historyLabel_(std::move(historyLabel))
{
}

bool EditObjectPropertiesCommand::Apply(EditorContext& ctx, const nlohmann::json& properties)
{
    EditorObject* object = ctx.document.Find(id_);
    if (!object)
    {
        return false;
    }
    object->properties = properties;
    ctx.document.SetDirty(true);
    return true;
}

bool EditObjectPropertiesCommand::Execute(EditorContext& ctx)
{
    // An edit that changes nothing is not an edit: letting it through would put an entry on
    // the undo stack that Ctrl+Z appears to ignore.
    if (beforeProperties_ == afterProperties_)
    {
        return false;
    }
    return Apply(ctx, afterProperties_);
}

void EditObjectPropertiesCommand::Undo(EditorContext& ctx)
{
    Apply(ctx, beforeProperties_);
}

#endif // WITH_EDITOR
