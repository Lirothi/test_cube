#pragma once
#if WITH_EDITOR

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObjectId

// Toggles an object's enabled flag on the document and its runtime visibility
// (when it has a live editor-owned runtime). Undoable.
class SetEnabledCommand : public EditorCommand
{
public:
    SetEnabledCommand(EditorObjectId id, bool enabled);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override
    {
        return enabled_ ? "Enable Object" : "Disable Object";
    }

private:
    void Apply(EditorContext& ctx, bool enabled);

    EditorObjectId id_;
    bool enabled_;
};

#endif // WITH_EDITOR
