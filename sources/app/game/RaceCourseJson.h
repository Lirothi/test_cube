#pragma once

#include "app/game/RaceCourse.h"
#include "third_party/json/json.hpp"

namespace game
{
    // What a level object says about the race, from its level JSON or the document's properties (beside RaceCourse.h and
    // not in it: the Scene holds a RaceCourse and has no business with JSON). `position` is the object's.
    inline void ReadRaceKeys(const nlohmann::json& object, const Math::float3& position, RaceCourse& out)
    {
        const auto gate = object.find(kRaceGateKey);
        if (gate != object.end() && gate->is_number_integer() && gate->get<int>() > 0)
        {
            const auto track = object.find(kRaceTrackKey);
            out.marks.push_back(RaceMark{ position, gate->get<int>(), track != object.end() && track->is_string() ? track->get<std::string>() : std::string() });
        }
        const auto bot = object.find(kRaceBotKey);
        if (bot != object.end() && bot->is_boolean() && bot->get<bool>())
        {
            out.bots.push_back(position);
        }
    }
}
