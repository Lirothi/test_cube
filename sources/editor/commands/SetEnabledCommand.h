#pragma once
#if WITH_EDITOR

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObjectId

// Sets an object's enabled flag on the document and its runtime visibility
// (when it has a live editor-owned runtime). Undo restores the state the object
// had before, which is not the opposite of the target when nothing changed.
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
    bool oldEnabled_ = false;
    bool captured_ = false;
};

#endif // WITH_EDITOR
