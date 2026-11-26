#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// --set / --sweep / MCP set_settings names physics.* (the physics world, game-side) (architecture plan R2.2). Each row
// is the branch App.cpp's ApplySweepValue had, its clamp included, and the reader ReadSweepValue had (R2.2 gave every
// simple name one first, so --settings-dump compared the move); a row without a reader had none.
namespace settings
{
namespace
{
const SettingDesc kPhysicsRows[] = {
    // The physics world (docs/physics_plan.md): game-side knobs, no render copy.
    TC_SETTING("physics.enabled", Bool, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().enabled = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().enabled); return true; }),
    TC_SETTING("physics.debug", Int, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().debug = static_cast<int>(value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().debug); return true; }),
    TC_SETTING("physics.debugRadius", Float, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().debugRadius = std::max(value, 1.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().debugRadius); return true; }),
    TC_SETTING("physics.stepHz", Float, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().stepHz = std::clamp(value, 10.0f, 480.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().stepHz); return true; }),
    TC_SETTING("physics.maxSteps", Int, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().maxSteps = std::clamp(static_cast<int>(value), 1, 32); return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().maxSteps); return true; }),
    TC_SETTING("physics.threads", Int, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().threads = std::clamp(static_cast<int>(value), 0, 16); return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().threads); return true; }),
    TC_SETTING("physics.async", Bool, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().async = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().async); return true; }),
    TC_SETTING("physics.kickInTick", Bool, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().kickInTick = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().kickInTick); return true; }),
    TC_SETTING("physics.stepDelayMs", Float, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().stepDelayMs = std::max(value, 0.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().stepDelayMs); return true; }),
    TC_SETTING("physics.gravity", Float, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().gravity = value; return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().gravity); return true; }),
    TC_SETTING("physics.autoShapes", Bool, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().autoShapes = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().autoShapes); return true; }),
    TC_SETTING("physics.forceLod", Int, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().forceLod = static_cast<int>(std::clamp(value, -1.0f, 3.0f)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().forceLod); return true; }),
    TC_SETTING("physics.hullTolerance", Float, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().hullTolerance = std::clamp(value, 0.0f, 0.25f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().hullTolerance); return true; }),
    TC_SETTING("physics.killY", Float, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().killY = value; return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().killY); return true; }),
    TC_SETTING("physics.angularDamping", Float, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().angularDamping = std::max(value, 0.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().angularDamping); return true; }),
    TC_SETTING("physics.rollingResistance", Float, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().rollingResistance = std::max(value, 0.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().rollingResistance); return true; }),
    TC_SETTING("physics.mooring", Float, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().mooring = std::clamp(value, 0.0f, 10.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().mooring); return true; }),
    TC_SETTING("physics.mooringTurn", Float, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().mooringTurn = std::clamp(value, 0.0f, 10.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().mooringTurn); return true; }),
    TC_SETTING("physics.mooringSettle", Float, SettingTransient,
               [](SettingContext&, float value) { physics::GameSettings().mooringSettle = std::clamp(value, 0.0f, 20.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(physics::GameSettings().mooringSettle); return true; }),
};
TC_SETTINGS(kPhysicsRows);
}
}
