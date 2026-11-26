#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/scene/SceneFrameData.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// --set / --sweep / MCP set_settings names rt.*, glass.*, render.* (the RT scene, glass, the reflection source, pre-
// exposure) (architecture plan R2.2). Each row is the branch App.cpp's ApplySweepValue had, its clamp included, and the
// reader ReadSweepValue had (R2.2 gave every simple name one first, so --settings-dump compared the move); a row
// without a reader had none.
namespace settings
{
namespace
{
const SettingDesc kRenderRows[] = {
    // Where surface reflections come from: 0 None, 1 SkyOnly, 2 SSR, 3 RT. The DEFAULT IS RT,
    // and on RT-capable hardware that means the screen-space path never runs -- so without this
    // key there is no headless way to exercise, measure or gate SSR at all. (Found the hard way
    // in P6C step 6: a "HiZ" capture that had quietly been tracing the TLAS.)
    TC_SETTING("render.reflectionSource", Int, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext& c, float value)
               {
                   const uint32_t r = (uint32_t)std::max(0.0f, value);
                   c.renderSettings.reflectionSource = r < (uint32_t)ReflectionSource::Count
                       ? (ReflectionSource)r : ReflectionSource::RT;
                   return true;
               },
               [](SettingContext& c, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.renderSettings.reflectionSource))); return true; }),
    // Stochastic coverage inflation for the RT foliage alpha test (0 honest .. 1 solid cards).
    TC_SETTING("rt.alphaMissKeep", Float, SettingPersistent,
               [](SettingContext& c, float value) { value = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.15f; c.renderSettings.rtAlphaMissKeep = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.rtAlphaMissKeep); return true; }),
    // RT foliage alpha mode: 0 off (solid cards), 1 first-hit holes, 2 full candidates.
    // rt.alphaTest stays as the legacy on/off alias.
    TC_SETTING("rt.alphaMode", Int, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext& c, float value) { c.renderSettings.rtAlphaMode = static_cast<uint32_t>(std::clamp(value, 0.0f, 2.0f)); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(c.renderSettings.rtAlphaMode)); return true; }),
    TC_SETTING("rt.alphaTest", Float, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.rtAlphaMode = value != 0.0f ? 2u : 0u; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.rtAlphaMode); return true; }),
    // Glass refraction: the glass under the surface in metres (0 = straight through, a few mm = a hollow
    // tumbler, >= the shape's radius = solid -- the 0.5 default).
    TC_SETTING("glass.wall", Float, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.glassWallThickness = std::clamp(value, 0.0f, 0.5f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.glassWallThickness); return true; }),
    // RW: wind-deformed BLASes for near casters (sway in RT reflections) + their radius.
    TC_SETTING("rt.windBlas", Bool, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.rtWindBlas = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.rtWindBlas); return true; }),
    TC_SETTING("rt.meshOpacity", Int, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.rtMeshOpacity = static_cast<uint32_t>(std::clamp(value, 0.0f, 2.0f) + 0.5f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.rtMeshOpacity); return true; }),
    TC_SETTING("rt.cutoutOpacity", Float, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.rtCutoutOpacityOverride = std::min(value, 1.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.rtCutoutOpacityOverride); return true; }),
    TC_SETTING("rt.windRadius", Float, SettingPersistent,
               [](SettingContext& c, float value) { value = std::isfinite(value) ? std::clamp(value, 0.0f, 100.0f) : 40.0f; c.renderSettings.rtWindBlasRadius = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.rtWindBlasRadius); return true; }),
    TC_SETTING("render.preExposure", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_preExposureEnabled_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_preExposureEnabled_game); return true; }),
};
TC_SETTINGS(kRenderRows);
}
}
