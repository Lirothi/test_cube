#pragma once
#if WITH_EDITOR

#include <cstddef>

struct EditorContext;
class EditorCommandStack;

// Displays the command timeline. Selecting a row restores the document/runtime
// state immediately after that command; selecting Initial State undoes all
// currently applied commands.
class CommandHistoryPanel
{
public:
    void Draw(EditorContext& ctx, EditorCommandStack& commandStack, bool* open);

private:
    std::size_t lastAppliedCount_ = static_cast<std::size_t>(-1);
    bool jumpFailed_ = false;
};

#endif // WITH_EDITOR
