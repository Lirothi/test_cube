#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/math/Math.h"
#include "physics/PhysicsWorld.h"

// Where the player starts (UE's APlayerStart), and the table of what a level may say about the player. A level object with no renderable behind it, like freeCameraStart:
// {"type": "playerStart", "position": [...], "rotationDeg": [pitch, yaw, 0], "walkSpeed": 5, ...}. The position is the FEET. docs/play_mode_plan.md.
namespace game
{
    inline constexpr const char* kPlayerStartType = "playerStart";

    // One number of physics::PlayerSettings. ONE table: the level's keys, the Inspector, --set player.<key>, Developer Controls.
    struct PlayerKey
    {
        const char* key;
        const char* label;
        float physics::PlayerSettings::* member;
        float speed; // of the drag
        float lo;
        float hi;
        const char* format;
    };
    const std::vector<PlayerKey>& PlayerKeys();
    const PlayerKey* FindPlayerKey(std::string_view key);

    struct PlayerStart
    {
        Math::float3 feet;
        float yawRad = 0.0f;
        float pitchRad = 0.0f;
        std::vector<std::pair<const PlayerKey*, float>> keys; // what this level says over the defaults
    };

    void ApplyPlayerKeys(const PlayerStart& start, physics::PlayerSettings& settings);
}
