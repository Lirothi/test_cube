#pragma once
#if WITH_EDITOR

#include <string>

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObject, EditorObjectId, nlohmann::json

// Spawns a factory-built mesh object into the live scene as an initialized
// runtime object, mirrored by an EditorObject in the document. Undo removes both;
// redo restores the same object with the same id.
class SpawnMeshCommand : public EditorCommand
{
public:
    explicit SpawnMeshCommand(nlohmann::json objectJson);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;

private:
    bool built_ = false;
    nlohmann::json objectJson_;   // factory-shape JSON
    EditorObject object_;         // document mirror, built once (stable id)
    EditorObjectId previousSelection_{};
};

#endif // WITH_EDITOR
