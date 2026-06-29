#include "editor/commands/EditorCommandStack.h"
#if WITH_EDITOR

#include <utility>

bool EditorCommandStack::Execute(EditorContext& ctx, std::unique_ptr<EditorCommand> command)
{
    if (!command)
    {
        return false;
    }
    if (!command->Execute(ctx))
    {
        return false; // a command that fails to apply is not recorded
    }
    undo_.push_back(std::move(command));
    redo_.clear();
    return true;
}

void EditorCommandStack::Undo(EditorContext& ctx)
{
    if (undo_.empty())
    {
        return;
    }
    std::unique_ptr<EditorCommand> command = std::move(undo_.back());
    undo_.pop_back();
    command->Undo(ctx);
    redo_.push_back(std::move(command));
}

void EditorCommandStack::Redo(EditorContext& ctx)
{
    if (redo_.empty())
    {
        return;
    }
    std::unique_ptr<EditorCommand> command = std::move(redo_.back());
    redo_.pop_back();
    command->Execute(ctx);
    undo_.push_back(std::move(command));
}

void EditorCommandStack::Clear()
{
    undo_.clear();
    redo_.clear();
}

#endif // WITH_EDITOR
