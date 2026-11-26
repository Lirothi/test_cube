#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "rendering/lighting/GtaoSettings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// --set / --sweep / MCP set_settings names gtao.* (ground-truth ambient occlusion) (architecture plan R2.2). Each row
// is the branch App.cpp's ApplySweepValue had, its clamp included, and the reader ReadSweepValue had (R2.2 gave every
// simple name one first, so --settings-dump compared the move); a row without a reader had none.
namespace settings
{
namespace
{
const SettingDesc kGtaoRows[] = {
    // P6B: the AO pass and its knobs, so a sweep can measure it before it is wired into
    // lighting. `gtao.enabled` is the A/B switch the plan requires before it becomes default.
    TC_SETTING("gtao.enabled", Bool, SettingLevelOwned | SettingScalable | SettingCostNextFrame,
               [](SettingContext& c, float value) { c.scene.GtaoRef().enabled = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = (c.scene.GtaoRef().enabled) ? 1.0f : 0.0f; return true; }),
    TC_SETTING("gtao.worldRadius", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().worldRadius = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().worldRadius); return true; }),
    TC_SETTING("gtao.thickness", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().thickness = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().thickness); return true; }),
    TC_SETTING("gtao.intensity", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().intensity = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().intensity); return true; }),
    TC_SETTING("gtao.numAngles", Float, SettingLevelOwned | SettingScalable | SettingCostNextFrame,
               [](SettingContext& c, float value) { c.scene.GtaoRef().numAngles = (uint32_t)std::max(1.0f, value); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(c.scene.GtaoRef().numAngles)); return true; }),
    TC_SETTING("gtao.numSteps", Float, SettingLevelOwned | SettingScalable | SettingCostNextFrame,
               [](SettingContext& c, float value) { c.scene.GtaoRef().numSteps = (uint32_t)std::max(1.0f, value); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(c.scene.GtaoRef().numSteps)); return true; }),
    // Items 3-5: one key per filter stage, so a sweep can isolate the denoise from the
    // temporal from the upsample instead of measuring the whole chain at once.
    TC_SETTING("gtao.denoise", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().denoise = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().denoise); return true; }),
    TC_SETTING("gtao.temporal", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().temporal = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().temporal); return true; }),
    TC_SETTING("gtao.filterRadius", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().filterRadius = (uint32_t)std::max(0.0f, value); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().filterRadius); return true; }),
    TC_SETTING("gtao.filterPlaneTolerance", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().filterPlaneTolerance = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().filterPlaneTolerance); return true; }),
    TC_SETTING("gtao.temporalBlendWeight", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().temporalBlendWeight = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().temporalBlendWeight); return true; }),
    TC_SETTING("gtao.temporalClampRange", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().temporalClampRange = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().temporalClampRange); return true; }),
    TC_SETTING("gtao.useGBufferNormal", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().useGBufferNormal = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().useGBufferNormal); return true; }),
    TC_SETTING("gtao.useHzb", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().useHzb = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().useHzb); return true; }),
    TC_SETTING("gtao.hzbMipBias", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().hzbMipBias = (uint32_t)std::max(0.0f, value); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().hzbMipBias); return true; }),
    // P16.4. `skyRadius` at or below `worldRadius` switches the second horizon walk off, which
    // is the exact-no-op baseline for the A/B.
    TC_SETTING("gtao.skyRadius", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().skyRadius = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().skyRadius); return true; }),
    TC_SETTING("gtao.skyMipBias", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().skyMipBias = (uint32_t)std::max(0.0f, value); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().skyMipBias); return true; }),
    // Mid-range intensity; 0 = the sky walk's compute path is off entirely (exact no-op).
    TC_SETTING("gtao.skyIntensity", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().skyIntensity = std::max(0.0f, value); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().skyIntensity); return true; }),
    TC_SETTING("gtao.strength", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().strength = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().strength); return true; }),
    TC_SETTING("gtao.upsampleTolerance", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.GtaoRef().upsampleTolerance = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.GtaoRef().upsampleTolerance); return true; }),
};
TC_SETTINGS(kGtaoRows);
}
}
