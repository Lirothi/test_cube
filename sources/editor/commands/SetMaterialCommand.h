#pragma once
#if WITH_EDITOR

#include <string>

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObjectId

// Gives a staticMesh ONE material on every slot (SetMaterialSlotCommand changes one slot).
// The material/textures load at Init and there is no live setter, so this respawns the
// runtime object (same id) with the new preset. Undoable: Undo puts back exactly the keys
// the object carried -- an ABSENT key stays absent, so a mesh-asset default it inherited is
// inherited again rather than frozen into an override.
class SetMaterialCommand : public EditorCommand
{
public:
    SetMaterialCommand(EditorObjectId id, std::string material);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override { return "Set Material"; }

private:
    // One property as the object carries it: absent, or present with this exact value.
    struct Key
    {
        bool present = false;
        nlohmann::json value;
    };

    bool Apply(EditorContext& ctx, const Key& material, const Key& materials);

    EditorObjectId id_;
    std::string material_;
    Key oldMaterial_;
    Key oldMaterials_;
    Key newMaterial_;
    Key newMaterials_;
    bool captured_ = false;
};

#endif // WITH_EDITOR
