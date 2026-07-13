#include "editor/commands/CompositeCommand.h"
#if WITH_EDITOR

#include <utility>

#include "editor/EditorContext.h"

CompositeCommand::CompositeCommand(std::string historyLabel)
    : historyLabel_(std::move(historyLabel))
{
}

void CompositeCommand::Add(std::unique_ptr<EditorCommand> command)
{
    if (command)
    {
        commands_.push_back(std::move(command));
    }
}

bool CompositeCommand::Execute(EditorContext& ctx)
{
    if (commands_.empty())
    {
        return false;
    }

    const bool dirtyBefore = ctx.document.IsDirty();
    std::size_t executed = 0;
    for (; executed < commands_.size(); ++executed)
    {
        if (!commands_[executed]->Execute(ctx))
        {
            while (executed > 0)
            {
                --executed;
                commands_[executed]->Undo(ctx);
            }
            ctx.document.SetDirty(dirtyBefore);
            return false;
        }
    }
    return true;
}

void CompositeCommand::Undo(EditorContext& ctx)
{
    for (auto it = commands_.rbegin(); it != commands_.rend(); ++it)
    {
        (*it)->Undo(ctx);
    }
}

#endif // WITH_EDITOR
