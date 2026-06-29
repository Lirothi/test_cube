#pragma once
#if WITH_EDITOR

#include <string>

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObject, EditorObjectId, nlohmann::json

// Spawns a mesh asset into the live scene as an initialized runtime object,
// mirrored by an EditorObject in the document. Undo removes both; redo restores
// the same object with the same id. The object's JSON and id are built once on
// the first Execute and reused on redo so the object is identical.
class SpawnMeshCommand : public EditorCommand
{
public:
    enum class Kind { StaticMesh, TransparentMesh };

    SpawnMeshCommand(Kind kind, std::string modelPath, std::string staticMaterial);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;

private:
    Kind kind_;
    std::string modelPath_;
    std::string staticMaterial_;

    bool built_ = false;
    nlohmann::json objectJson_;   // factory-shape JSON, built once
    EditorObject object_;         // document mirror, built once (stable id)
    EditorObjectId previousSelection_{};
};

#endif // WITH_EDITOR
