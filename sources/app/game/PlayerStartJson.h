#pragma once

#include <algorithm>

#include "app/game/PlayerStart.h"
#include "third_party/json/json.hpp"

namespace game
{
    // What the object says about the player, from its level JSON or the document's properties. Beside PlayerStart.h and not in it: the Scene holds a PlayerStart and has no business with JSON.
    inline void ReadPlayerKeys(const nlohmann::json& object, PlayerStart& out)
    {
        out.keys.clear();
        for (const PlayerKey& entry : PlayerKeys())
        {
            const auto it = object.find(entry.key);
            if (it != object.end() && it->is_number())
            {
                out.keys.emplace_back(&entry, std::clamp(it->get<float>(), entry.lo, entry.hi));
            }
        }
    }
}
