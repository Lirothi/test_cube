#pragma once
#if WITH_EDITOR

#include <string>

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h"

// A before/after snapshot of one DOCUMENT object's `properties`, undoable.
//
// EditEnvironmentCommand has done this for environment entities since the beginning;
// ordinary objects had nothing equivalent, so every property they own could only be changed
// by a command written for that one property -- SetMeshAsset, SetMaterial, SetParticlePreset
// -- and anything without such a command could not be edited at all. Dragging a point of a
// spline zone is the first thing to need it, and it will not be the last: `properties` is
// the level's own JSON, and this is the general way to put something back the way it was.
//
// Deliberately NOT a transform edit: TransformObjectCommand owns position, rotation and
// scale, and having two commands that can both move an object would make undo depend on
// which one ran.
class EditObjectPropertiesCommand : public EditorCommand
{
public:
    EditObjectPropertiesCommand(
        EditorObjectId id,
        nlohmann::json beforeProperties,
        nlohmann::json afterProperties,
        std::string historyLabel = "Edit Object");

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override { return historyLabel_; }

private:
    bool Apply(EditorContext& ctx, const nlohmann::json& properties);

    EditorObjectId id_;
    nlohmann::json beforeProperties_;
    nlohmann::json afterProperties_;
    std::string historyLabel_;
};

#endif // WITH_EDITOR
