#pragma once
#if WITH_EDITOR

#include <cstddef>
#include <memory>
#include <vector>

#include "editor/commands/EditorCommand.h"

struct EditorContext;

// Linear command timeline. appliedCount_ is the current state cursor: commands
// before it are applied, commands at or after it are available for redo.
// Executing a new command after rewinding discards that forward branch.
class EditorCommandStack
{
public:
    // Executes the command; on success pushes it onto the undo stack and clears
    // redo. Returns the command's Execute() result. A failed command is dropped.
    bool Execute(EditorContext& ctx, std::unique_ptr<EditorCommand> command);

    void Undo(EditorContext& ctx);
    void Redo(EditorContext& ctx);
    bool MoveTo(EditorContext& ctx, std::size_t appliedCommandCount);

    bool CanUndo() const { return appliedCount_ > 0; }
    bool CanRedo() const { return appliedCount_ < history_.size(); }

    std::size_t HistorySize() const { return history_.size(); }
    std::size_t AppliedCount() const { return appliedCount_; }
    const EditorCommand* HistoryEntry(std::size_t index) const;

    void Clear();

private:
    std::vector<std::unique_ptr<EditorCommand>> history_;
    std::size_t appliedCount_ = 0;
};

#endif // WITH_EDITOR
