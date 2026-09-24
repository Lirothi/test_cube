#pragma once
#if WITH_EDITOR

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObjectId

// Floats an object on the ocean, or stops it floating: the document's "buoyant" property -- written
// only while on, so a level that never floated anything saves byte-identical -- and the runtime
// object's flag, which ocean/OceanBuoyancy picks up (or lets go of) on the next frame. Undoable;
// Undo restores what the object had before, not the opposite of the request.
class SetBuoyantCommand : public EditorCommand
{
public:
    SetBuoyantCommand(EditorObjectId id, bool buoyant);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override
    {
        return buoyant_ ? "Enable Buoyancy" : "Disable Buoyancy";
    }

private:
    static void Apply(EditorContext& ctx, EditorObjectId id, bool buoyant);

    EditorObjectId id_;
    bool buoyant_;
    bool before_ = false;
};

#endif // WITH_EDITOR
