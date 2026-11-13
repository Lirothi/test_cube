#pragma once
#if WITH_EDITOR

#include <string>
#include <vector>

#include "core/math/Math.h"
#include "editor/scene/EditorSceneDocument.h"

struct EditorContext;
class EditorCommandStack;

// The level's race courses as the editor shows them (owner, 2026-11-13: "make the tracks entities in the editor -- in the
// outliner, a section in the inspector, the gates edited in the viewport"). A course is still its marks -- level objects
// with "raceGate" (and "raceTrack", app/game/RaceCourse.h) -- so nothing new is saved: a track and a gate are views over
// their marks, selecting one selects its marks and editing one edits them (each edit one undo entry).
namespace editorrace
{
    struct Gate
    {
        int number = 0;
        std::vector<EditorObjectId> marks; // two = a gate; one or three = a gate the course leaves out
    };

    struct Track
    {
        std::string name;        // "" = the level's first ("classic")
        std::vector<Gate> gates; // by number
    };

    // Every mark of the document, by course (the unnamed first, then by name, as RaceCourse::TrackNames) and gate number.
    std::vector<Track> Collect(const EditorSceneDocument& document);

    // The selection is exactly the two marks of one gate: their course, number and ids (a = the first selected).
    bool SelectedGate(const EditorContext& ctx, std::string* track, int* gate, EditorObjectId* a, EditorObjectId* b);
    // The selection is every mark of one course and nothing else.
    bool SelectedTrack(const EditorContext& ctx, std::string* track);

    // A gate as one thing: the middle of its marks, the unit line from mark a to mark b (xz), its width (between the marks'
    // middles) and that line's bearing (degrees, atan2(x, z) -- the same sense as a transform's yaw).
    struct Frame
    {
        Math::float3 center{ 0.0f, 0.0f, 0.0f };
        Math::float2 across{ 1.0f, 0.0f };
        float width = 0.0f;
        float bearingDeg = 0.0f;
    };
    Frame FrameOf(const EditorTransform& a, const EditorTransform& b);
    Frame FrameWith(const Frame& frame, float bearingDeg); // the same gate turned to that bearing
    // The marks' transforms for the gate moved from `before` to `after`: placed on its line at its width, their heights and
    // yaws carried with it.
    void Place(const Frame& before, const Frame& after, EditorTransform& a, EditorTransform& b);

    // Edits, each one undo entry.
    void MoveGate(EditorContext& ctx, EditorCommandStack& history, EditorObjectId a, EditorObjectId b, const Frame& after, const char* label);
    // A new gate after `gate` (between it and the next one, or the course's first): copies of its marks, the gates after it
    // renumbered one on. The new gate is selected.
    void InsertGateAfter(EditorContext& ctx, EditorCommandStack& history, const std::string& track, int gate);
    // The gate's marks deleted and the gates after it renumbered one back.
    void DeleteGate(EditorContext& ctx, EditorCommandStack& history, const std::string& track, int gate);
    // Every mark of the course to another course name ("" = the level's first).
    void RenameTrack(EditorContext& ctx, EditorCommandStack& history, const std::string& track, const std::string& name);
    // The course run the other way round: gate 1 stays the start, gate n (2..count) becomes count + 2 - n.
    void ReverseTrack(EditorContext& ctx, EditorCommandStack& history, const std::string& track);

    // m along the middles of a course's gates, round to its first again.
    float TrackLength(const EditorSceneDocument& document, const Track& track);

    // The marks of the gate `mark` belongs to (itself first), or empty when it is no mark.
    std::vector<EditorObjectId> GateMarksOf(const EditorSceneDocument& document, EditorObjectId mark);
    // Every mark of the course, by gate.
    std::vector<EditorObjectId> TrackMarks(const Track& track);

    // ---- a bot race on a course (owner, 2026-11-13: "set how many bots and of which level race, and hang the camera on any
    // of them") ----
    // A place on the grid: a kind of boat (its mesh.json) a copy of which is made for the race, and its bot's level.
    struct BotRacePick
    {
        std::string asset;
        int difficulty = 2; // game::BotDifficulty: 0 easy, 1 medium, 2 hard
    };
    // The kinds of boat the bots race in: the assets of the level's enabled boats that carry "raceBot", each once, in the
    // document's order (RaceCourse::fleet). The boats themselves stay at the shore for the player.
    std::vector<std::string> RaceFleet(const EditorSceneDocument& document);
    // "models/police_boat/police_boat.mesh.json" -> "police_boat"
    std::string KindName(const std::string& asset);
    // The race into game::RaceGridRef, for the session that starts next: the course, its laps, the places (1 to 4, each a kind of
    // the fleet) with their levels, the chase camera on place `watch` (0-based; -1 = none). False and why when the course has no
    // three whole gates or a kind is not the fleet's. Nothing in the document changes.
    bool RequestBotRace(const EditorSceneDocument& document, const std::string& track, int laps, const std::vector<BotRacePick>& boats,
                        int watch, std::string* why);
    // The Inspector's "Race the bots" asked for it this frame: EditorController starts Play on it.
    bool TakeBotRacePlay();

    // The Inspector's section for a selected gate (its place, width and turn, insert / delete, the gates either side) or a
    // selected course (its name, length, direction, its gates); for a single mark, buttons that select its gate or course.
    // True when it drew a gate or a course.
    bool DrawInspector(EditorContext& ctx, EditorCommandStack& history);
}

#endif // WITH_EDITOR
