#pragma once
#if WITH_EDITOR

#include <memory>
#include <string>

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObjectId

// The race's two keys on a level object (app/game/RaceCourse.h): "raceGate": N on a mark -- two marks with one number
// are a gate -- and "raceBot": true on a boat a bot helms in Play. The document's alone: nothing at runtime reads them
// until Play starts and builds the course from the document. Written only while set, so a level without a race saves
// byte-identical. Undo restores what the object had.
class SetRaceKeyCommand : public EditorCommand
{
public:
    // `gate` 0 = no mark of the race.
    static std::unique_ptr<SetRaceKeyCommand> Gate(EditorObjectId id, int gate);
    static std::unique_ptr<SetRaceKeyCommand> Bot(EditorObjectId id, bool bot);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override { return label_; }

    // A mark is any mesh; a bot's boat any mesh too (whether it is a vessel is the asset's to say: SetPhysicsCommand::VesselAsset).
    static bool AppliesTo(const EditorObject& obj) { return obj.type == "staticMesh"; }
    static int DocumentGate(const EditorObject& obj);
    static bool DocumentBot(const EditorObject& obj);

private:
    SetRaceKeyCommand(EditorObjectId id, std::string key, nlohmann::json value, std::string label);
    static void Apply(EditorContext& ctx, EditorObjectId id, const std::string& key, const nlohmann::json& value);

    EditorObjectId id_;
    std::string key_;
    nlohmann::json value_;  // null = the key is taken away
    nlohmann::json before_;
    std::string label_;
};

#endif // WITH_EDITOR
