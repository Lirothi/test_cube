#include "app/game/PlayerStart.h"

namespace game
{
    const std::vector<PlayerKey>& PlayerKeys()
    {
        using S = physics::PlayerSettings;
        static const std::vector<PlayerKey> keys = {
            { "walkSpeed", "Walk speed (m/s)", &S::walkSpeed, 0.05f, 0.1f, 30.0f, "%.2f" },
            { "sprintSpeed", "Sprint speed (m/s)", &S::sprintSpeed, 0.05f, 0.1f, 30.0f, "%.2f" },
            { "crouchSpeed", "Crouch speed (m/s)", &S::crouchSpeed, 0.05f, 0.1f, 30.0f, "%.2f" },
            { "swimSpeed", "Swim speed (m/s)", &S::swimSpeed, 0.05f, 0.1f, 30.0f, "%.2f" },
            { "acceleration", "Acceleration (m/s2)", &S::acceleration, 0.1f, 0.1f, 200.0f, "%.2f" },
            { "braking", "Braking (m/s2)", &S::braking, 0.1f, 0.0f, 200.0f, "%.2f" },
            { "friction", "Friction (1/s)", &S::friction, 0.05f, 0.0f, 50.0f, "%.2f" },
            { "airControl", "Air control", &S::airControl, 0.005f, 0.0f, 1.0f, "%.3f" },
            { "jumpSpeed", "Jump speed (m/s)", &S::jumpSpeed, 0.02f, 0.0f, 30.0f, "%.2f" },
            { "height", "Height (m)", &S::height, 0.01f, 0.7f, 4.0f, "%.2f" },
            { "crouchHeight", "Crouched height (m)", &S::crouchHeight, 0.01f, 0.7f, 4.0f, "%.2f" },
            { "radius", "Radius (m)", &S::radius, 0.005f, 0.1f, 1.0f, "%.3f" },
            { "eyeHeight", "Eye height (m)", &S::eyeHeight, 0.01f, 0.1f, 4.0f, "%.2f" },
            { "crouchEyeHeight", "Crouched eye height (m)", &S::crouchEyeHeight, 0.01f, 0.1f, 4.0f, "%.2f" },
            { "stepHeight", "Step height (m)", &S::stepHeight, 0.01f, 0.0f, 2.0f, "%.2f" },
            { "maxSlopeDeg", "Steepest slope (deg)", &S::maxSlopeDeg, 0.25f, 0.0f, 89.0f, "%.1f" },
            { "swimDepth", "Swims from a depth of (m)", &S::swimDepth, 0.01f, 0.1f, 10.0f, "%.2f" },
        };
        return keys;
    }

    const PlayerKey* FindPlayerKey(std::string_view key)
    {
        for (const PlayerKey& entry : PlayerKeys())
        {
            if (key == entry.key) { return &entry; }
        }
        return nullptr;
    }

    void ApplyPlayerKeys(const PlayerStart& start, physics::PlayerSettings& settings)
    {
        for (const auto& [key, value] : start.keys) { settings.*(key->member) = value; }
    }
}
