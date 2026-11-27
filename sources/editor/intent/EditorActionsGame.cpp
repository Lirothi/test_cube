#include "editor/intent/EditorActionBuilders.h"
#if WITH_EDITOR

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "app/game/RaceCourse.h" // game::TrackDisplayName (came through Scene.h before R6.2b)
#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "editor/commands/CreateDocumentObjectCommand.h"
#include "editor/scene/EditorZone.h"
#include "editor/scene/EditorPlayerStart.h"
#include "editor/commands/SetBuoyantCommand.h"
#include "editor/commands/SetRaceKeyCommand.h"
#include "editor/commands/SetVesselTuneCommand.h"
#include "editor/commands/SetPhysicsCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "ocean/OceanRenderable.h"

// R10.3: the editor's actions -- what the game reads off an object: buoyancy, race gates and bots, vessels and boat cards, physics, the Player Start.
// Cut out of EditorActionRegistry.cpp; the table that names them is still there (EditorActionBuilders.h).

namespace editoractions
{
    // ------------------------------------------------------------------ setBuoyant

    std::unique_ptr<EditorCommand> BuildSetBuoyant(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        const bool buoyant = BoolOr(intent.params, "enabled", true);
        std::vector<std::unique_ptr<EditorCommand>> commands;
        std::size_t skipped = 0;
        for (const EditorObjectId id : targets)
        {
            const EditorObject* obj = actionCtx.editor.document.Find(id);
            if (!obj || obj->type != "staticMesh")
            {
                ++skipped; // lights, zones, the ocean itself: nothing with a hull
                continue;
            }
            commands.push_back(std::make_unique<SetBuoyantCommand>(id, buoyant));
        }
        if (commands.empty())
        {
            outStatus = "Nothing that can float: buoyancy is for meshes";
            return nullptr;
        }
        const std::size_t count = commands.size();
        outStatus = (buoyant ? "Floating " : "Stopped floating ") + CountedObjects(count);
        if (skipped > 0) { outStatus += " (" + CountedObjects(skipped) + " skipped: not meshes)"; }
        if (buoyant && !actionCtx.editor.scene.FindOceanRenderable())
        {
            outStatus += "; this level has no ocean, so nothing moves until it gets one";
        }
        return FoldIntoOneEntry(std::move(commands),
            std::string(buoyant ? "Float " : "Stop Floating ") + std::to_string(count) + " Objects");
    }

    // ------------------------------------------------------------------ setRaceGate, setRaceBot

    std::unique_ptr<EditorCommand> BuildSetRaceGate(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        const int gate = std::max(static_cast<int>(std::lround(NumberOr(intent.params, "gate", 0.0f))), 0);
        const bool hasTrack = intent.params.contains("track") && intent.params["track"].is_string();
        const std::string track = hasTrack ? intent.params["track"].get<std::string>() : std::string();
        std::vector<std::unique_ptr<EditorCommand>> commands;
        for (const EditorObjectId id : targets)
        {
            const EditorObject* obj = actionCtx.editor.document.Find(id);
            if (!obj || !SetRaceKeyCommand::AppliesTo(*obj)) { continue; }
            commands.push_back(SetRaceKeyCommand::Gate(id, gate));
            // Out of the race: its course goes with its number. Else the course named, when one is.
            if (gate == 0 || hasTrack) { commands.push_back(SetRaceKeyCommand::Track(id, gate == 0 ? std::string() : track)); }
        }
        if (commands.empty())
        {
            outStatus = "Nothing to mark: a race gate's mark is a mesh (a buoy)";
            return nullptr;
        }
        const std::size_t count = gate == 0 || hasTrack ? commands.size() / 2 : commands.size();
        outStatus = gate > 0 ? "Gate " + std::to_string(gate) + (hasTrack ? " of course '" + game::TrackDisplayName(track) + "'" : std::string()) + ": " +
                                   CountedObjects(count) + (count == 2 ? "" : " (a gate is TWO marks)")
                             : "No longer marks of the race: " + CountedObjects(count);
        return FoldIntoOneEntry(std::move(commands), gate > 0 ? "Race Gate " + std::to_string(gate) : "Clear Race Gate");
    }

    std::unique_ptr<EditorCommand> BuildSetRaceBot(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        const bool bot = BoolOr(intent.params, "enabled", true);
        std::vector<std::unique_ptr<EditorCommand>> commands;
        std::size_t skipped = 0;
        for (const EditorObjectId id : targets)
        {
            const EditorObject* obj = actionCtx.editor.document.Find(id);
            if (!obj || !SetRaceKeyCommand::AppliesTo(*obj) || !SetPhysicsCommand::VesselAsset(actionCtx.editor, id))
            {
                ++skipped;
                continue;
            }
            commands.push_back(SetRaceKeyCommand::Bot(id, bot));
        }
        if (commands.empty())
        {
            outStatus = "No boat among them: a race's bots helm vessels (a mesh whose asset has a \"vessel\" block)";
            return nullptr;
        }
        const std::size_t count = commands.size();
        outStatus = (bot ? "The kind of " : "Not the kind of ") + CountedObjects(count) + (bot ? " races as a bot (a race makes its own copies)" : " for the bots");
        if (skipped > 0) { outStatus += " (" + CountedObjects(skipped) + " skipped: not boats)"; }
        return FoldIntoOneEntry(std::move(commands), bot ? "Races As A Bot" : "Races Not As A Bot");
    }

    // ------------------------------------------------------------------ setVessel

    // A boat's own number (the Inspector's Boat section): {key, value}; {key, reset: true} puts the asset's back, {reset: true}
    // every one. SetVesselTuneCommand, one undo entry for every boat matched.
    std::unique_ptr<EditorCommand> BuildSetVessel(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        const std::string name = StringOr(intent.params, "key", "");
        const physics::VesselKey* key = name.empty() ? nullptr : physics::FindVesselKey(name);
        if (!name.empty() && (!key || !physics::VesselGroupPerCopy(key->group)))
        {
            outStatus = "'" + name + "' is no number a boat may say for itself:";
            for (const physics::VesselKey& entry : physics::VesselKeys())
            {
                if (physics::VesselGroupPerCopy(entry.group)) { outStatus += std::string(" ") + entry.key; }
            }
            return nullptr;
        }
        const bool reset = BoolOr(intent.params, "reset", false);
        const auto valueIt = intent.params.find("value");
        const bool hasValue = valueIt != intent.params.end() && valueIt->is_number();
        if (!reset && (!key || !hasValue))
        {
            outStatus = "setVessel takes a key and its value, or reset: true (with a key that one goes back to the asset's, without one every one)";
            return nullptr;
        }
        const float asked = hasValue ? valueIt->get<float>() : 0.0f;
        std::vector<std::unique_ptr<EditorCommand>> commands;
        std::size_t skipped = 0;
        std::size_t unchanged = 0;
        for (const EditorObjectId id : targets)
        {
            const EditorObject* obj = actionCtx.editor.document.Find(id);
            if (!obj || !SetRaceKeyCommand::AppliesTo(*obj) || !SetVesselTuneCommand::IsVessel(*obj))
            {
                ++skipped;
                continue;
            }
            std::unique_ptr<SetVesselTuneCommand> command = reset ? SetVesselTuneCommand::Reset(id, key) : SetVesselTuneCommand::Key(id, *key, asked);
            if (!command->Changes(*obj))
            {
                ++unchanged;
                continue;
            }
            commands.push_back(std::move(command));
        }
        if (commands.empty())
        {
            outStatus = unchanged > 0 ? "Nothing changes: " + CountedObjects(unchanged) + (reset ? " already as the asset's" : " already at that number")
                                      : "No boat among them: setVessel tunes a copy of a vessel (a mesh whose asset has a \"vessel\" block)";
            return nullptr;
        }
        const std::size_t count = commands.size();
        if (reset) { outStatus = std::string(key ? key->key : "every number") + " as the asset's on " + CountedObjects(count); }
        else
        {
            const float value = std::clamp(asked, key->lo, key->hi);
            char text[96];
            std::snprintf(text, sizeof(text), "%s = %g", key->key, static_cast<double>(value));
            outStatus = std::string(text) + " on " + CountedObjects(count);
            if (value != asked) { outStatus += " (clamped to its range " + std::to_string(key->lo) + " .. " + std::to_string(key->hi) + ")"; }
        }
        if (unchanged > 0) { outStatus += ", " + CountedObjects(unchanged) + " unchanged"; }
        if (skipped > 0) { outStatus += " (" + CountedObjects(skipped) + " skipped: not boats)"; }
        return FoldIntoOneEntry(std::move(commands), reset ? "Boat: As The Asset's" : std::string("Boat: ") + key->label);
    }

    // The card's bars as the player reads them (Inspector > Boat, the casual edit): any of {topSpeedKn, secondsTo20Kn,
    // circleM, seaKeeping (%)}; the bars not named are held; game::SolveCard finds the numbers.
    std::unique_ptr<EditorCommand> BuildSetBoatCard(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        game::VesselCard want;
        unsigned which = 0;
        const auto read = [&intent, &which](const char* name, unsigned bit, float scale, float& out)
        {
            const auto it = intent.params.find(name);
            if (it == intent.params.end() || !it->is_number()) { return; }
            out = it->get<float>() / scale;
            which |= bit;
        };
        read("topSpeedKn", SetVesselTuneCommand::kCardTopSpeed, 1.94384f, want.topSpeed);
        read("secondsTo20Kn", SetVesselTuneCommand::kCardToPlane, 1.0f, want.toPlane);
        read("circleM", SetVesselTuneCommand::kCardCircle, 1.0f, want.circle);
        read("seaKeeping", SetVesselTuneCommand::kCardSeaKeeping, 100.0f, want.seaKeeping);
        if (which == 0)
        {
            outStatus = "setBoatCard takes at least one of topSpeedKn, secondsTo20Kn, circleM, seaKeeping";
            return nullptr;
        }
        std::vector<std::unique_ptr<EditorCommand>> commands;
        std::size_t notBoats = 0;
        std::string reachedText;
        for (const EditorObjectId id : targets)
        {
            const EditorObject* obj = actionCtx.editor.document.Find(id);
            game::VesselCard reached;
            std::vector<std::unique_ptr<SetVesselTuneCommand>> boat =
                obj && SetRaceKeyCommand::AppliesTo(*obj) ? SetVesselTuneCommand::Card(actionCtx.editor, id, want, which, &reached)
                                                          : std::vector<std::unique_ptr<SetVesselTuneCommand>>{};
            if (reached.topSpeed <= 0.0f)
            {
                ++notBoats;
                continue;
            }
            if (reachedText.empty())
            {
                char text[200];
                std::snprintf(text, sizeof(text), "%s reads %.1f kn, 20 kn in %.1f s, a %.0f m circle, sea keeping %.0f %%", obj->name.c_str(),
                              reached.topSpeed * 1.94384f, reached.toPlane, reached.circle, reached.seaKeeping * 100.0f);
                reachedText = text;
            }
            for (std::unique_ptr<SetVesselTuneCommand>& command : boat)
            {
                if (command->Changes(*obj)) { commands.push_back(std::move(command)); } // one that does not would fail the whole entry
            }
        }
        if (commands.empty())
        {
            outStatus = !reachedText.empty() ? "Nothing changes: " + reachedText
                                             : "No boat among them: the card is a boat's in the physics world (a vessel copy, Body Dynamic and floating)";
            return nullptr;
        }
        outStatus = reachedText + " (" + std::to_string(commands.size()) + " numbers moved)";
        if (notBoats > 0) { outStatus += " (" + CountedObjects(notBoats) + " skipped: not boats now)"; }
        return FoldIntoOneEntry(std::move(commands), "Boat: Card");
    }

    // ------------------------------------------------------------------ setPhysics, keepSimulation

    std::unique_ptr<EditorCommand> BuildSetPhysics(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        physics::Motion motion = physics::Motion::Static;
        const std::string name = StringOr(intent.params, "motion", "static");
        if (!physics::MotionFromName(name, motion))
        {
            outStatus = "motion is none, static or dynamic, not '" + name + "'";
            return nullptr;
        }
        const float mass = NumberOr(intent.params, "mass", 0.0f);
        std::vector<std::unique_ptr<EditorCommand>> commands;
        std::size_t skipped = 0;
        for (const EditorObjectId id : targets)
        {
            const EditorObject* obj = actionCtx.editor.document.Find(id);
            if (!obj || !SetPhysicsCommand::AppliesTo(*obj))
            {
                ++skipped; // lights, zones, water, particles: no body
                continue;
            }
            commands.push_back(std::make_unique<SetPhysicsCommand>(id, motion, mass));
        }
        if (commands.empty())
        {
            outStatus = "Nothing to give a body: physics is for static meshes";
            return nullptr;
        }
        const std::size_t count = commands.size();
        outStatus = "Physics " + name + ": " + CountedObjects(count);
        if (skipped > 0) { outStatus += " (" + CountedObjects(skipped) + " skipped: not meshes)"; }
        return FoldIntoOneEntry(std::move(commands), "Set Physics " + name + " on " + std::to_string(count) + " Objects");
    }

    std::unique_ptr<EditorCommand> BuildKeepSimulation(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent&,
        std::string& outStatus)
    {
        return BuildKeepSimulationCommand(actionCtx.editor, targets.empty() ? nullptr : &targets, outStatus);
    }

    // ------------------------------------------------------------------ placePlayerStart

    // One per level: a second call moves it, as the place a play session starts from is one place.
    std::unique_ptr<EditorCommand> BuildPlacePlayerStart(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        (void)targets;
        EditorContext& ctx = actionCtx.editor;
        EditorObject placed = editorplayerstart::BuildObject(ctx.scene);
        if (intent.params.contains("at")) { placed.transform.position = Vec3Or(intent.params, "at", placed.transform.position); }
        if (intent.params.contains("yawDeg")) { placed.transform.rotationDeg.y = NumberOr(intent.params, "yawDeg", placed.transform.rotationDeg.y); }
        const Math::float3 at = placed.transform.position;
        char where[96];
        std::snprintf(where, sizeof(where), "at %.2f %.2f %.2f, yaw %.1f", at.x, at.y, at.z, placed.transform.rotationDeg.y);
        if (const EditorObject* existing = editorplayerstart::Find(ctx.document))
        {
            outStatus = std::string("Moved the Player Start ") + where;
            return std::make_unique<TransformObjectCommand>(existing->id, existing->transform, placed.transform);
        }
        outStatus = std::string("Placed a Player Start ") + where;
        return std::make_unique<CreateDocumentObjectCommand>(std::move(placed));
    }
}

#endif // WITH_EDITOR
