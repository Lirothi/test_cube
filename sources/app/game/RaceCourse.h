#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "core/math/Math.h"

// A race course on the water, as a level says it: its MARKS are ordinary level objects (buoys) that carry a gate's number,
// "raceGate": N -- two marks with the same number are that gate's posts, a boat passes between them, the gates are run in
// the order of their numbers and the last leads back to the first (gate 1 is the start and the finish). A boat that
// carries "raceBot": true names a kind of boat the bots race in: a race spawns its own copies (game::Race), the boat itself
// stays at the shore for the player. No object of its own: move a buoy and the course
// moves. A build with the editor reads the keys off the document when Play starts (EditorController::FindRaceCourse),
// one without it off the level's file (JsonLevel -> Scene::LevelRaceCourse).
// A level may hold more than one course: a mark's "raceTrack": "<name>" says whose it is (absent = the level's first, the
// unnamed one), and each course numbers its gates from 1 and has its own start zone (2026-11-11, owner: "make one more
// course, harder than this one").
namespace game
{
    inline constexpr const char* kRaceGateKey = "raceGate";
    inline constexpr const char* kRaceTrackKey = "raceTrack";
    inline constexpr const char* kRaceBotKey = "raceBot";

    struct RaceMark
    {
        Math::float3 position;
        int gate = 0;      // 1..
        std::string track; // "" = the level's first course
    };

    struct RaceCourse
    {
        std::vector<RaceMark> marks;
        // The kinds of boat the bots race in: the assets (mesh.json) of the level's boats that carry "raceBot", each once, in the
        // level's order. A race spawns its own copies on the grid (game::Race); the boats at the shore stay the player's.
        std::vector<std::string> fleet;
        std::string level;              // the level's file: whose bot memories (game::BotMemory) its races read and write
        bool Empty() const { return marks.empty() && fleet.empty(); }
        // Its courses' names: the unnamed one first, then the others by name.
        std::vector<std::string> TrackNames() const
        {
            std::vector<std::string> names;
            for (const RaceMark& mark : marks)
            {
                if (std::find(names.begin(), names.end(), mark.track) == names.end()) { names.push_back(mark.track); }
            }
            std::sort(names.begin(), names.end());
            return names;
        }
    };
    // How a course is called on screen: its name, the unnamed one "classic".
    inline std::string TrackDisplayName(const std::string& track) { return track.empty() ? std::string("classic") : track; }
}
