#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "rendering/scene/SceneFrameData.h"
#include "rendering/wind/WindState.h"
#include "vfx/ParticleTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// --set / --sweep / MCP set_settings names particles.* and wind.* (particles, the wind's clock) (architecture plan
// R2.2). Each row is the branch App.cpp's ApplySweepValue had, its clamp included, and the reader ReadSweepValue had
// (R2.2 gave every simple name one first, so --settings-dump compared the move); a row without a reader had none.
namespace settings
{
namespace
{
const SettingDesc kVfxRows[] = {
    // --wind-freeze for a RUNNING process (MCP set_settings): 1 holds the current wind + ocean pose,
    // 0 lets the clock run again. Two looks compared in one editor then see the same waves.
    TC_SETTING("wind.freeze", Bool, SettingTransient,
               [](SettingContext&, float value) { vfx::g_windFreeze_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vfx::g_windFreeze_game); return true; }),
    // ...and frozen at THIS clock (seconds), --wind-freeze=<s> for a running process: two arms frozen with
    // wind.freeze:1 at different moments hold different poses, and the VSM pages the receivers request with them.
    TC_SETTING("wind.freezeAt", Float, SettingTransient,
               [](SettingContext&, float value) { vfx::g_windFreeze_game = true; vfx::g_windFrozenTime_game = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(vfx::g_windFrozenTime_game); return true; }),
    TC_SETTING("particles.spawnScale", Float, SettingTransient | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { vfx::g_spawnScale = std::clamp(value, 0.0f, 4.0f); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(vfx::g_spawnScale); return true; }),
    // The boats' spray (vfx/VesselSpray): the ocean preset's knobs (OceanRenderConfig::spray*, the Ocean window and the
    // Inspector's "Boat spray") -- on/off, how much (a share of what the presets can keep alive), the shares of the
    // hull's speed a drop leaves with, the drops' albedo and forward lobe, the white water along the hulls' sides.
    // A lit particle's air: the fog's colour as before 2026-11-09 (the A/B), 0 = the sky's distant light.
    TC_SETTING("particles.airFromFog", Bool, SettingTransient,
               [](SettingContext&, float value) { vfx::g_litAirFromFog_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vfx::g_litAirFromFog_game); return true; }),
    TC_SETTING("particles.collideHulls", Bool, SettingTransient,
               [](SettingContext&, float value) { vfx::g_collideHulls_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vfx::g_collideHulls_game); return true; }),
    // a sprite's pixels over a hull
    TC_SETTING("particles.clipHulls", Bool, SettingTransient,
               [](SettingContext&, float value) { vfx::g_clipHulls_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vfx::g_clipHulls_game); return true; }),
    // soft fade against the water too
    TC_SETTING("particles.softWater", Bool, SettingTransient | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { g_particleSoftWater_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = (g_particleSoftWater_game) ? 1.0f : 0.0f; return true; }),
};
TC_SETTINGS(kVfxRows);
}
}
