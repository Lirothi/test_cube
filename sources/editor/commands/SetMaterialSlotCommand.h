#pragma once
#if WITH_EDITOR

#include <string>

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObjectId

// B4: changes ONE material slot of a multi-material staticMesh (glTF assets carry one slot per
// submesh). Writes the object's "materials":[...] array (synthesizing it from the current
// effective state on first touch) and respawns the runtime object (same id) with the new slot,
// mirroring SetMaterialCommand — materials load at Init, there is no live setter. Undoable.
class SetMaterialSlotCommand : public EditorCommand
{
public:
    SetMaterialSlotCommand(EditorObjectId id, int slot, std::string material);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override { return "Set Material Slot"; }

private:
    bool Apply(EditorContext& ctx, const std::string& material);
    void Respawn(EditorContext& ctx, EditorObjectId id) const;

    EditorObjectId id_;
    int slot_ = 0;
    std::string newMaterial_;
    nlohmann::json oldMaterials_; // prior "materials" value (null if the key was absent)
    bool oldHadMaterials_ = false;
    bool captured_ = false;
};

#endif // WITH_EDITOR
