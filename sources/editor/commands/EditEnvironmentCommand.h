#pragma once
#if WITH_EDITOR

#include <string>

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h"

// Applies a before/after property snapshot to one environment entity and updates
// its type-specific live runtime state. This is shared by inspector edits,
// outliner enable toggles, and viewport light-gizmo gestures.
class EditEnvironmentCommand : public EditorCommand
{
public:
    EditEnvironmentCommand(
        EditorObjectId id,
        nlohmann::json beforeProperties,
        nlohmann::json afterProperties,
        std::string historyLabel = "Edit Environment");

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
