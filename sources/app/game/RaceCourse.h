#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "core/math/Math.h"

// A race course on the water, as a level says it: its MARKS are ordinary level objects (buoys) that carry a gate's number,
// "raceGate": N -- two marks with the same number are that gate's posts, a boat passes between them, the gates are run in
// the order of their numbers and the last leads back to the first (gate 1 is the start and the finish). A boat that
// carries "raceBot": true is helmed by a bot in Play (game::RaceBot). No object of its own: move a buoy and the course
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
        std::vector<Math::float3> bots; // where the boats a bot helms stand in the level
        bool Empty() const { return marks.empty() && bots.empty(); }
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
