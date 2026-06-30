#pragma once
#if WITH_EDITOR

#include <string>

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObjectId

// Changes a staticMesh's material preset. The material/textures load at Init and
// there is no live setter, so this respawns the runtime object (same id) with the
// new preset. Undoable.
class SetMaterialCommand : public EditorCommand
{
public:
    SetMaterialCommand(EditorObjectId id, std::string material);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;

private:
    bool Apply(EditorContext& ctx, const std::string& material);

    EditorObjectId id_;
    std::string newMaterial_;
    std::string oldMaterial_;
    bool captured_ = false;
};

#endif // WITH_EDITOR
