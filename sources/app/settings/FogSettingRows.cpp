#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/lighting/HeightFogSettings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// --set / --sweep / MCP set_settings names fog.* (the exponential height fog and its froxel volume) (architecture plan
// R2.2). Each row is the branch App.cpp's ApplySweepValue had, its clamp included, and the reader ReadSweepValue had
// (R2.2 gave every simple name one first, so --settings-dump compared the move); a row without a reader had none.
namespace settings
{
namespace
{
const SettingDesc kFogRows[] = {
    // Volumetric fog cell size (8 / 16 / 32 render pixels); the renderer picks the change up at the next frame.
    // the renderer rounds to a power of two
    TC_SETTING("fog.gridPixels", Float, SettingPersistent | SettingScalable | SettingCostTargets,
               [](SettingContext&, float value) { render::g_fogGridPixels_game = static_cast<unsigned>(std::clamp(value, 4.0f, 64.0f)); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(render::g_fogGridPixels_game)); return true; }),
    TC_SETTING("fog.gridZ", Float, SettingPersistent | SettingScalable | SettingCostTargets,
               [](SettingContext&, float value) { render::g_fogGridZ_game = static_cast<unsigned>(std::clamp(value, 16.0f, 128.0f)); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(render::g_fogGridZ_game)); return true; }),
    // Project-wide like fog.gridPixels (the level's fog keys are the medium): UE's GridCenterOffsetFromDepthBuffer.
    TC_SETTING("fog.depthOffset", Float, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.fogDepthOffset = std::clamp(value, -1.0f, 4.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.fogDepthOffset); return true; }),
    TC_SETTING("fog.depthThreshold", Float, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.fogDepthThreshold = std::clamp(value, 0.0f, 128.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.fogDepthThreshold); return true; }),
    TC_SETTING("fog.sunRtFallback", Bool, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.fogSunRtFallback = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.fogSunRtFallback); return true; }),
    // P7 exponential height fog -- NOT aerial perspective, which is `sky.aerialPerspective`
    // and belongs to the sky atmosphere. `fog.enabled` is the gate; the rest are the model's own
    // parameters, so a sweep can find defaults without a rebuild.
    TC_SETTING("fog.enabled", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().enabled = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().enabled); return true; }),
    TC_SETTING("fog.density", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().density = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().density); return true; }),
    TC_SETTING("fog.debugView", Int, SettingTransient,
               [](SettingContext&, float value) { g_fogDebugView_game = static_cast<uint32_t>(std::max(0.0f, value)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(g_fogDebugView_game); return true; }),
    TC_SETTING("fog.heightFalloff", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().heightFalloff = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().heightFalloff); return true; }),
    TC_SETTING("fog.referenceHeight", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().referenceHeight = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().referenceHeight); return true; }),
    TC_SETTING("fog.startDistance", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().startDistance = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().startDistance); return true; }),
    TC_SETTING("fog.maxOpacity", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().maxOpacity = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().maxOpacity); return true; }),
    TC_SETTING("fog.sunScatter", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().sunScatterStrength = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().sunScatterStrength); return true; }),
    // UE's DirectionalInscatteringLuminance colour (the tint of fog.sunScatter), per channel.
    TC_SETTING("fog.sunScatterR", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().sunScatterColor.x = std::max(value, 0.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().sunScatterColor.x); return true; }),
    TC_SETTING("fog.sunScatterG", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().sunScatterColor.y = std::max(value, 0.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().sunScatterColor.y); return true; }),
    TC_SETTING("fog.sunScatterB", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().sunScatterColor.z = std::max(value, 0.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().sunScatterColor.z); return true; }),
    TC_SETTING("fog.sunScatterExp", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().sunScatterExponent = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().sunScatterExponent); return true; }),
    TC_SETTING("fog.sunScatterStart", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().sunScatterStartDistance = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().sunScatterStartDistance); return true; }),
    TC_SETTING("fog.skyBlur", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().skyBlur = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().skyBlur); return true; }),
    // UE's fog colour (procedural sky): SkyAtmosphereAmbientContributionColorScale and FogInscatteringLuminance as
    // grays; fog.skyAlongView:1 = the colour before 2026-10-31 (the sky along the view), transient, the A/B.
    TC_SETTING("fog.skyAmbient", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().skyAmbientContribution = float3(std::max(value, 0.0f)); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().skyAmbientContribution.x); return true; }),
    TC_SETTING("fog.inscatterLuminance", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().inscatteringLuminance = float3(std::max(value, 0.0f)); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().inscatteringLuminance.x); return true; }),
    TC_SETTING("fog.skyAlongView", Bool, SettingTransient,
               [](SettingContext&, float value) { g_fogSkyAlongView_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(g_fogSkyAlongView_game); return true; }),
    // fog.inSkyCapture:0 = the height fog out of the sky's capture (the water's sky reflection, the sky fill), the A/B.
    TC_SETTING("fog.inSkyCapture", Bool, SettingTransient,
               [](SettingContext&, float value) { g_fogInSkyCapture_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(g_fogInSkyCapture_game); return true; }),
    TC_SETTING("fog.nonDirectionalDistance", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().nonDirectionalDistance = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().nonDirectionalDistance); return true; }),
    TC_SETTING("fog.fullyDirectionalDistance", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().fullyDirectionalDistance = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().fullyDirectionalDistance); return true; }),
    // Volumetric fog (docs/volumetric_fog_sky_clouds_ssgi_plan.md, part A).
    TC_SETTING("fog.volumetric", Bool, SettingLevelOwned | SettingScalable | SettingCostNextFrame,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().volumetric = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = (c.scene.HeightFogRef().volumetric) ? 1.0f : 0.0f; return true; }),
    TC_SETTING("fog.volumetricDistance", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().volumetricDistance = std::max(1.0f, value); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().volumetricDistance); return true; }),
    TC_SETTING("fog.volumetricStart", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().volumetricStartDistance = std::max(0.0f, value); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().volumetricStartDistance); return true; }),
    TC_SETTING("fog.volumetricFadeIn", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().volumetricNearFadeInDistance = std::max(0.0f, value); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().volumetricNearFadeInDistance); return true; }),
    TC_SETTING("fog.albedo", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().albedo = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().albedo); return true; }),
    TC_SETTING("fog.extinctionScale", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().extinctionScale = std::max(0.0f, value); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().extinctionScale); return true; }),
    TC_SETTING("fog.phaseG", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().phaseG = std::clamp(value, -0.99f, 0.99f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().phaseG); return true; }),
    TC_SETTING("fog.sunVolScatter", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().sunScatter = std::max(0.0f, value); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().sunScatter); return true; }),
    TC_SETTING("fog.skyVolScatter", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().skyScatter = std::max(0.0f, value); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().skyScatter); return true; }),
    TC_SETTING("fog.historyWeight", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().historyWeight = std::clamp(value, 0.0f, 0.99f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().historyWeight); return true; }),
    TC_SETTING("fog.temporal", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().temporal = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().temporal); return true; }),
    TC_SETTING("fog.jitter", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().jitter = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().jitter); return true; }),
    TC_SETTING("fog.samplesPerCell", Int, SettingLevelOwned | SettingScalable | SettingCostNextFrame,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().samplesPerCell = std::clamp(static_cast<int>(value), 1, 4); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.HeightFogRef().samplesPerCell); return true; }),
    TC_SETTING("fog.conservativeDepth", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().conservativeDepth = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().conservativeDepth); return true; }),
    TC_SETTING("fog.localLights", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().localLights = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().localLights); return true; }),
    TC_SETTING("fog.localLightScatter", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().localLightScatter = std::max(value, 0.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().localLightScatter); return true; }),
    TC_SETTING("fog.localSoftFading", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().localSoftFading = std::max(value, 0.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().localSoftFading); return true; }),
    TC_SETTING("fog.localDistanceBias", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.HeightFogRef().localDistanceBias = std::max(value, 0.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.HeightFogRef().localDistanceBias); return true; }),
};
TC_SETTINGS(kFogRows);
}
}
