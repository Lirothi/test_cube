#pragma once
#if WITH_EDITOR

#include <string_view>

struct EditorContext;

// Base class for undoable editor actions. Execute performs the action and
// returns false if it could not be applied (the stack then discards it). Undo
// reverses a previously-executed command.
class EditorCommand
{
public:
    virtual ~EditorCommand() = default;
    virtual bool Execute(EditorContext& ctx) = 0;
    virtual void Undo(EditorContext& ctx) = 0;
    virtual std::string_view HistoryLabel() const { return "Command"; }
};

#endif // WITH_EDITOR
