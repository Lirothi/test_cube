#pragma once
#if WITH_EDITOR

#include <string>

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObjectId

// Replaces a particle emitter's base preset. Particle buffer, material, and sprite setup are
// created at Init, so apply the document change by respawning the runtime object with its same
// editor id. The prior full property set is retained for undo.
class SetParticlePresetCommand final : public EditorCommand
{
public:
    SetParticlePresetCommand(EditorObjectId id, std::string preset);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override { return "Assign Particle Preset"; }

private:
    bool Apply(EditorContext& ctx, const nlohmann::json& properties);

    EditorObjectId id_;
    std::string newPreset_;
    nlohmann::json oldProperties_;
    nlohmann::json newProperties_;
    bool captured_ = false;
};

#endif // WITH_EDITOR
