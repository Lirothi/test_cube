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

    history_.erase(
        history_.begin() + static_cast<std::ptrdiff_t>(appliedCount_),
        history_.end());
    history_.push_back(std::move(command));
    appliedCount_ = history_.size();
    return true;
}

void EditorCommandStack::Undo(EditorContext& ctx)
{
    if (!CanUndo())
    {
        return;
    }

    history_[appliedCount_ - 1]->Undo(ctx);
    --appliedCount_;
}

void EditorCommandStack::Redo(EditorContext& ctx)
{
    if (!CanRedo())
    {
        return;
    }

    if (history_[appliedCount_]->Execute(ctx))
    {
        ++appliedCount_;
    }
}

bool EditorCommandStack::MoveTo(EditorContext& ctx, std::size_t appliedCommandCount)
{
    if (appliedCommandCount > history_.size())
    {
        return false;
    }

    const std::size_t target = appliedCommandCount;
    while (appliedCount_ > target)
    {
        Undo(ctx);
    }
    while (appliedCount_ < target)
    {
        const std::size_t before = appliedCount_;
        Redo(ctx);
        if (appliedCount_ == before)
        {
            return false;
        }
    }
    return appliedCount_ == target;
}

const EditorCommand* EditorCommandStack::HistoryEntry(std::size_t index) const
{
    return index < history_.size() ? history_[index].get() : nullptr;
}

void EditorCommandStack::Clear()
{
    history_.clear();
    appliedCount_ = 0;
}

#endif // WITH_EDITOR
