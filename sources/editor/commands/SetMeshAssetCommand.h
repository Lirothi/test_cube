#pragma once
#if WITH_EDITOR

#include <string>

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h"

// Replaces a staticMesh's .mesh.json reference. The command respawns the live
// runtime so geometry and mesh-owned defaults update immediately, and retains
// explicit instance overrides on objects that already referenced a mesh asset.
class SetMeshAssetCommand : public EditorCommand
{
public:
    SetMeshAssetCommand(EditorObjectId id, std::string meshAssetPath);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override { return "Set Mesh Asset"; }

private:
    bool Apply(EditorContext& ctx, const nlohmann::json& properties);

    EditorObjectId id_;
    std::string newMeshAssetPath_;
    nlohmann::json oldProperties_;
    nlohmann::json newProperties_;
    bool captured_ = false;
};

#endif // WITH_EDITOR
