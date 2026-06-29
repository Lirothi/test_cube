#pragma once
#if WITH_EDITOR

#include <memory>
#include <vector>

#include "editor/commands/EditorCommand.h"

struct EditorContext;

// Linear undo/redo stack. Executing a new command clears the redo history.
class EditorCommandStack
{
public:
    // Executes the command; on success pushes it onto the undo stack and clears
    // redo. Returns the command's Execute() result. A failed command is dropped.
    bool Execute(EditorContext& ctx, std::unique_ptr<EditorCommand> command);

    void Undo(EditorContext& ctx);
    void Redo(EditorContext& ctx);

    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }

    void Clear();

private:
    std::vector<std::unique_ptr<EditorCommand>> undo_;
    std::vector<std::unique_ptr<EditorCommand>> redo_;
};

#endif // WITH_EDITOR
