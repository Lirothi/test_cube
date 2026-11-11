#pragma once
#if WITH_EDITOR

#include <memory>
#include <string>

#include <vector>

#include "app/game/VesselStats.h"             // VesselCard, SolveCard
#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObject, EditorObjectId
#include "physics/CollisionJson.h"            // VesselKey

// A placed boat's own numbers (physics::VesselGroupPerCopy: how it races -- the engine, the hull, the helm, the turn, the
// sea): the level object's "vessel" block, which SceneObjectFactory::ResolveMeshAsset lays key by key over its asset's
// mesh.json "vessel". A key is written only while it differs from the asset's, so a copy tuned and put back saves
// byte-identical. The runtime object takes the numbers at once and the physics world rebuilds its body from them
// (RenderableObject::SetVesselSettings): the card in Play, the helm and a race bot all read them from there. Undo restores
// the block the object had.
class SetVesselTuneCommand : public EditorCommand
{
public:
    // One number of this copy, clamped to the key's range; at its asset's own value the key is taken away.
    static std::unique_ptr<SetVesselTuneCommand> Key(EditorObjectId id, const physics::VesselKey& key, float value);
    // One key back to its asset's; `key` null = every key a copy may say.
    static std::unique_ptr<SetVesselTuneCommand> Reset(EditorObjectId id, const physics::VesselKey* key);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override { return label_; }
    // Would it change this object? A command that changes nothing fails (no history entry), and one such in a
    // CompositeCommand fails the whole: a builder over a selection keeps only those that do.
    bool Changes(const EditorObject& obj) const;
    EditorObjectId Target() const { return id_; }

    // Is it a copy of a boat, by the document (its asset's mesh.json read)? What setVessel takes; the Inspector asks the
    // runtime object instead (SetPhysicsCommand::VesselAsset), which reads no file a frame.
    static bool IsVessel(const EditorObject& obj);
    // The object's own block; null = none.
    static const nlohmann::json* DocumentTune(const EditorObject& obj);
    // Has this copy its own value of the key?
    static bool HasOwn(const EditorObject& obj, const physics::VesselKey& key);
    // What its asset says (the object's own numbers left out), and what it does (its own over the asset's).
    static physics::VesselSettings AssetSettings(const EditorObject& obj);
    static physics::VesselSettings EffectiveSettings(const EditorObject& obj);

    // The card's casual edit (Inspector > Boat, MCP setBoatCard): the bars named in `which` (kCard* bits) as `want` says,
    // the others held where the boat reads them now; game::SolveCard finds the numbers, and every one that moves is a
    // Key command. Empty when nothing moves or the boat has no hull in the physics world (not a boat now: `reached` 0s).
    enum : unsigned { kCardTopSpeed = 1, kCardToPlane = 2, kCardCircle = 4, kCardSeaKeeping = 8 };
    struct HullShape
    {
        float halfLength = 0.0f;
        float rudderLever = 0.0f;
        float draft = 0.0f; // m, as the copy floats (the settings' times its scale)
    };
    // The physics world's hull of the placed copy (its body is built in the editor too). False = none.
    static bool ShapeOf(EditorContext& ctx, EditorObjectId id, HullShape& out);
    static std::vector<std::unique_ptr<SetVesselTuneCommand>> Card(EditorContext& ctx, EditorObjectId id, const game::VesselCard& want, unsigned which,
                                                                    game::VesselCard* reached = nullptr);

private:
    enum class Kind : unsigned char { Set, ResetKey, ResetAll };
    SetVesselTuneCommand(EditorObjectId id, Kind kind, const physics::VesselKey* key, float value, std::string label);
    static void Apply(EditorContext& ctx, EditorObjectId id, const nlohmann::json& block);
    nlohmann::json After(const EditorObject& obj) const; // its block once this is done; null = none

    EditorObjectId id_;
    Kind kind_;
    const physics::VesselKey* key_ = nullptr;
    float value_ = 0.0f;
    nlohmann::json before_; // null = no block
    std::string label_;
};

#endif // WITH_EDITOR
