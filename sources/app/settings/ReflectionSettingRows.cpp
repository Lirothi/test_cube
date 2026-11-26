#include "app/Systems.h"
#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "rendering/core/Renderer.h"
#include "rendering/lighting/SsrSettings.h"
#include "rendering/scene/SceneFrameData.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// --set / --sweep / MCP set_settings names ssr.* and reflections.* (screen-space and RT reflections) (architecture plan
// R2.2). Each row is the branch App.cpp's ApplySweepValue had, its clamp included, and the reader ReadSweepValue had
// (R2.2 gave every simple name one first, so --settings-dump compared the move); a row without a reader had none.
namespace settings
{
namespace
{
const SettingDesc kReflectionRows[] = {
    // Reflection trace resolution as a fraction of the RENDER resolution, the one DLSS takes
    // (graphics_settings reflections.resolution; the RT and SSR traces run at it), for a 4K sweep.
    TC_SETTING("reflections.resolution", Float, SettingTransient | SettingScalable | SettingCostTargets,
               [](SettingContext&, float value)
               {
                   const float scale = std::clamp(value, 0.25f, 1.0f);
                   Systems::GetRenderer().SetReflectionTextureScale(Math::float2(scale, scale));
                   return true;
               },
               [](SettingContext&, float& out) { out = static_cast<float>(Systems::GetRenderer().GetReflectionTextureScale().x); return true; }),
    // S16's roughness-driven reflection blur (Reflections "Glossy blur", graphics_settings `glossyBlur`):
    // reflection-resolution pixels added to the blur at roughness 1.
    TC_SETTING("reflections.glossyBlur", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.reflectionGlossyScale = std::clamp(value, 0.0f, 24.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.reflectionGlossyScale); return true; }),
    // The RT reflection's GGX-lobe glossy filter; 0 = the separable blur above, the A/B.
    TC_SETTING("reflections.lobe", Bool, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.reflectionLobeBlur = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.reflectionLobeBlur); return true; }),
    TC_SETTING("reflections.hitReprojection", Bool, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.reflectionHitReprojection = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.reflectionHitReprojection); return true; }),
    TC_SETTING("reflections.geometricNormal", Bool, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.reflectionGeometricNormal = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.reflectionGeometricNormal); return true; }),
    // P6C/P13: which screen-space search the reflection ray uses. 0 = log march (fixed growing
    // steps against full depth), 1 = UE SSRT fixed-step Batch4 march against the furthest HZB.
    // The A/B switch for the SSR retrofit, and the only way to compare both inside one binary.
    // Only has an effect when render.reflectionSource is 2 (SSR).
    // Despite the prefix this now gates the temporal resolve for BOTH reflection sources —
    // the RT reflection at half res boils under DLSS jitter just like the SSR buffer did.
    TC_SETTING("ssr.temporal", Bool, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.ssrTemporal = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.ssrTemporal); return true; }),
    TC_SETTING("ssr.waterTemporal", Bool, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.waterReflectionTemporal = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.waterReflectionTemporal); return true; }),
    TC_SETTING("ssr.waterTemporalUnderDlss", Bool, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.waterReflectionTemporalUnderDlss = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.waterReflectionTemporalUnderDlss); return true; }),
    TC_SETTING("ssr.waterTemporalBlend", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.waterReflectionTemporalBlend = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.waterReflectionTemporalBlend); return true; }),
    TC_SETTING("ssr.waterTemporalInertia", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.waterReflectionTemporalStillInertia = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.waterReflectionTemporalStillInertia); return true; }),
    // EXPERIMENT (2026-10-12): the water traced from its own normal and read at the pixel (UE's way).
    TC_SETTING("ssr.waterSurfaceNormal", Bool, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.waterReflectionSurfaceNormal = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.waterReflectionSurfaceNormal); return true; }),
    // FFT cascades in the water's reflection normal (4 = all, 2 = the swell only).
    TC_SETTING("ssr.waterNormalCascades", Int, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext& c, float value) { c.renderSettings.waterReflectionNormalCascades = static_cast<uint32_t>(std::clamp(static_cast<int>(value + 0.5f), 1, 4)); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(c.renderSettings.waterReflectionNormalCascades)); return true; }),
    TC_SETTING("reflections.oceanResolution", Float, SettingTransient | SettingScalable | SettingCostTargets,
               [](SettingContext&, float value)
               {
                   const float scale = std::clamp(value, 0.25f, 1.0f);
                   Systems::GetRenderer().SetOceanReflectionTextureScale(Math::float2(scale, scale));
                   return true;
               },
               [](SettingContext&, float& out) { out = static_cast<float>(Systems::GetRenderer().GetOceanReflectionTextureScale().x); return true; }),
    // The whole water-reflection variant in ONE value, so a single --sweep compares them:
    // 0 = shipped (flat mirror + UV break-up, 0.5 of render, resolve off), 1 = UE (surface normal,
    // full render res, resolve on with inertia 0), 2 = UE without the resolve, 3 = UE at 0.5 res.
    TC_SETTING("ssr.waterExperiment", Int, SettingTransient,
               [](SettingContext& c, float value)
               {
                   const int variant = static_cast<int>(value + 0.5f);
                   c.renderSettings.waterReflectionSurfaceNormal = variant != 0;
                   c.renderSettings.waterReflectionTemporal = variant == 1 || variant == 3;
                   c.renderSettings.waterReflectionTemporalBlend = 0.125f;
                   c.renderSettings.waterReflectionTemporalStillInertia = 0.0f;
                   const float scale = (variant == 1 || variant == 2) ? 1.0f : 0.5f;
                   Systems::GetRenderer().SetOceanReflectionTextureScale(Math::float2(scale, scale));
                   return true;
               },
               nullptr),
    TC_SETTING("ssr.temporalBlend", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.ssrTemporalBlendWeight = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.ssrTemporalBlendWeight); return true; }),
    TC_SETTING("ssr.temporalClampExpand", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.ssrTemporalClampExpand = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.ssrTemporalClampExpand); return true; }),
    TC_SETTING("ssr.ueQuality", Int, SettingTransient | SettingScalable | SettingCostNextFrame,
               [](SettingContext& c, float value)
               {
                   const uint32_t q = static_cast<uint32_t>(std::max(0.0f, value));
                   ApplyUeSsrQualityPreset(c.renderSettings.ssrUe,
                       q < static_cast<uint32_t>(UeSsrQualityPreset::Count)
                           ? static_cast<UeSsrQualityPreset>(q) : UeSsrQualityPreset::Epic);
                   return true;
               },
               [](SettingContext& c, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.renderSettings.ssrUe.preset))); return true; }),
    TC_SETTING("ssr.ueSteps", Int, SettingTransient,
               [](SettingContext& c, float value)
               {
                   c.renderSettings.ssrUe.numSteps = static_cast<uint32_t>(std::max(4.0f, value));
                   c.renderSettings.ssrUe.preset = UeSsrQualityPreset::Custom;
                   return true;
               },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.ssrUe.numSteps); return true; }),
    TC_SETTING("ssr.ueRays", Int, SettingTransient,
               [](SettingContext& c, float value)
               {
                   c.renderSettings.ssrUe.numRays = static_cast<uint32_t>(std::max(1.0f, value));
                   c.renderSettings.ssrUe.preset = UeSsrQualityPreset::Custom;
                   return true;
               },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.ssrUe.numRays); return true; }),
    TC_SETTING("ssr.ueGlossy", Bool, SettingTransient,
               [](SettingContext& c, float value)
               {
                   c.renderSettings.ssrUe.glossyRays = value != 0.0f;
                   c.renderSettings.ssrUe.preset = UeSsrQualityPreset::Custom;
                   return true;
               },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.ssrUe.glossyRays); return true; }),
    TC_SETTING("ssr.ueUseSurfaceRoughness", Bool, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.ssrUe.useSurfaceRoughness = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.ssrUe.useSurfaceRoughness); return true; }),
    TC_SETTING("ssr.ueRoughnessOverride", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.ssrUe.roughnessOverride = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.ssrUe.roughnessOverride); return true; }),
    TC_SETTING("ssr.ueIntensity", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.ssrUe.intensity = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.ssrUe.intensity); return true; }),
    TC_SETTING("ssr.ueMaxRoughness", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.ssrUe.maxRoughness = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.ssrUe.maxRoughness); return true; }),
    TC_SETTING("ssr.technique", Int, SettingTransient,
               [](SettingContext& c, float value)
               {
                   const uint32_t t = (uint32_t)std::max(0.0f, value);
                   c.renderSettings.ssrTechnique = t < (uint32_t)SsrTechnique::Count
                       ? (SsrTechnique)t : SsrTechnique::LogMarch;
                   return true;
               },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.ssrTechnique); return true; }),
};
TC_SETTINGS(kReflectionRows);
}
}
