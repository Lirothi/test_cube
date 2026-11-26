#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "rendering/meshes/LodSelect.h"
#include "rendering/shadows/VirtualShadowMap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// --set / --sweep / MCP set_settings names vsm.* (virtual shadow maps) (architecture plan R2.2). Each row is the branch
// App.cpp's ApplySweepValue had, its clamp included, and the reader ReadSweepValue had (R2.2 gave every simple name one
// first, so --settings-dump compared the move); a row without a reader had none.
namespace settings
{
namespace
{
const SettingDesc kVsmRows[] = {
    TC_SETTING("vsm.clipmapDepthBias", Float, SettingPersistent,
               [](SettingContext&, float value) { value = std::isfinite(value) ? std::clamp(value, 0.0f, 0.01f) : 0.0002f; vsm::g_clipmapDepthBias_game = value; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_clipmapDepthBias_game); return true; }),
    // Per-level depth-bias shaping (bias(L) = max(base * decay^L, floorTexels), see
    // VsmClipmapShadow) -- headless mirrors of the two dev-window sliders beside the base bias.
    TC_SETTING("vsm.clipmapDepthBiasDecay", Float, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_clipmapDepthBiasDecay_game = std::clamp(value, 0.01f, 1.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_clipmapDepthBiasDecay_game); return true; }),
    TC_SETTING("vsm.clipmapDepthBiasFloor", Float, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_clipmapDepthBiasFloorTexels_game = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_clipmapDepthBiasFloorTexels_game); return true; }),
    TC_SETTING("vsm.clipmapNormalBias", Float, SettingPersistent,
               [](SettingContext&, float value) { value = std::isfinite(value) ? std::clamp(value, 0.0f, 4.0f) : 1.0f; vsm::g_clipmapNormalBias_game = value; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_clipmapNormalBias_game); return true; }),
    TC_SETTING("vsm.clipmapFirstLevel", Int, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_clipmapFirstLevel_game = std::clamp(static_cast<int>(value), 0, 16); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_clipmapFirstLevel_game); return true; }),
    TC_SETTING("vsm.resolutionLodBiasDirectional", Float, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { vsm::g_resolutionLodBiasDirectional_game = std::clamp(value, -4.0f, 4.0f); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(vsm::g_resolutionLodBiasDirectional_game); return true; }),
    TC_SETTING("vsm.clipmapPan", Bool, SettingTransient,
               [](SettingContext&, float value) { vsm::g_clipmapPan_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_clipmapPan_game); return true; }),
    TC_SETTING("vsm.cullBackfacingPixels", Bool, SettingTransient,
               [](SettingContext&, float value) { vsm::g_cullBackfacingPixels_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_cullBackfacingPixels_game); return true; }),
    TC_SETTING("vsm.pageDilation", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_pageDilation_game = std::clamp(value, 0.0f, 0.5f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_pageDilation_game); return true; }),
    TC_SETTING("vsm.markCoarsePages", Int, SettingTransient,
               [](SettingContext&, float value) { vsm::g_markCoarsePages_game = std::clamp(static_cast<int>(value), 0, 3); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_markCoarsePages_game); return true; }),
    TC_SETTING("vsm.coarseCacheDynamic", Bool, SettingTransient,
               [](SettingContext&, float value) { vsm::g_coarseCacheDynamic_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_coarseCacheDynamic_game); return true; }),
    TC_SETTING("vsm.coarsePagePixelsStatic", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_coarsePagePixelsStatic_game = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_coarsePagePixelsStatic_game); return true; }),
    TC_SETTING("vsm.coarsePagePixelsDynamic", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_coarsePagePixelsDynamic_game = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_coarsePagePixelsDynamic_game); return true; }),
    // r.Shadow.Virtual.Clipmap.ZRangeScale: reach up-sun as a multiple of the level radius.
    // 10 reproduces the old `extent * 5`; UE ship 1000.
    TC_SETTING("vsm.clipmapZRangeScale", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_clipmapZRangeScale_game = std::max(1.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_clipmapZRangeScale_game); return true; }),
    // SMRT (docs/vsm_smrt_plan.md). rayCount 0 = the single-tap SampleCmp path -- the A/B arm.
    TC_SETTING("vsm.smrtRayCount", Float, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { vsm::g_smrtRayCount_game = (std::uint32_t)std::clamp(value, 0.0f, (float)vsm::kSmrtMaxRays); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(vsm::g_smrtRayCount_game)); return true; }),
    TC_SETTING("vsm.smrtSamplesPerRay", Float, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { vsm::g_smrtSamplesPerRay_game = (std::uint32_t)std::clamp(value, 1.0f, (float)vsm::kSmrtMaxSamplesPerRay); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(vsm::g_smrtSamplesPerRay_game)); return true; }),
    TC_SETTING("vsm.localSmrtRayCount", Float, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { vsm::g_localSmrtRayCount_game = (std::uint32_t)std::clamp(value, 0.0f, (float)vsm::kSmrtMaxRays); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(vsm::g_localSmrtRayCount_game)); return true; }),
    TC_SETTING("vsm.localSmrtSamplesPerRay", Float, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_localSmrtSamplesPerRay_game = (std::uint32_t)std::clamp(value, 0.0f, (float)vsm::kSmrtMaxSamplesPerRay); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_localSmrtSamplesPerRay_game); return true; }),
    TC_SETTING("vsm.localSmrtTexelDitherScale", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_localSmrtTexelDitherScale_game = std::clamp(value, 0.0f, 8.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_localSmrtTexelDitherScale_game); return true; }),
    TC_SETTING("vsm.localSmrtExtrapolateMaxSlope", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_localSmrtExtrapolateMaxSlope_game = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_localSmrtExtrapolateMaxSlope_game); return true; }),
    TC_SETTING("vsm.localSmrtMaxSlopeBias", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_localSmrtMaxSlopeBias_game = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_localSmrtMaxSlopeBias_game); return true; }),
    TC_SETTING("vsm.localSmrtMaxRayAngle", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_localSmrtMaxRayAngle_game = std::clamp(value, 0.001f, 1.5f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_localSmrtMaxRayAngle_game); return true; }),
    TC_SETTING("vsm.smrtRayLengthScale", Float, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_smrtRayLengthScale_game = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_smrtRayLengthScale_game); return true; }),
    TC_SETTING("vsm.smrtExtrapolateMaxSlope", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_smrtExtrapolateMaxSlope_game = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_smrtExtrapolateMaxSlope_game); return true; }),
    TC_SETTING("vsm.smrtSourceAngleDeg", Float, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_smrtSourceAngleDeg_game = std::clamp(value, 0.0f, 45.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_smrtSourceAngleDeg_game); return true; }),
    TC_SETTING("vsm.smrtTexelDitherScale", Float, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_smrtTexelDitherScale_game = std::clamp(value, 0.0f, 8.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_smrtTexelDitherScale_game); return true; }),
    TC_SETTING("vsm.smrtTemporalDither", Bool, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_smrtTemporalDither_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_smrtTemporalDither_game); return true; }),
    TC_SETTING("vsm.smrtAdaptiveRayCount", Float, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_smrtAdaptiveRayCount_game = (std::uint32_t)std::clamp(value, 0.0f, 16.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_smrtAdaptiveRayCount_game); return true; }),
    TC_SETTING("vsm.smrtScreenRayLength", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_smrtScreenRayLength_game = std::clamp(value, 0.0f, 0.5f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_smrtScreenRayLength_game); return true; }),
    TC_SETTING("vsm.smrtScreenRaySamples", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_smrtScreenRaySamples_game = (std::uint32_t)std::clamp(value, 1.0f, 16.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_smrtScreenRaySamples_game); return true; }),
    // The dev-window "Shadow LOD bias" slider, headless. A change triggers the same GPU-idle
    // caster rebuild the slider does (Scene::ReconcileShadowLodBias polls it) — which is the
    // point: the round-trip perf leak is only reproducible in ONE process
    // (docs/bug_shadow_lod_bias_perf.md §6).
    TC_SETTING("vsm.shadowLodBias", Float, SettingPersistent,
               [](SettingContext&, float value) { value = std::isfinite(value) ? std::clamp(value, -2.0f, 3.0f) : 0.0f; render::g_shadowLodBias_game = (int)value; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_shadowLodBias_game); return true; }),
    // Caster-vs-receiver LOD floor (LodSelect.h). Off = the old per-view-only caster LOD.
    TC_SETTING("vsm.pageCaching", Bool, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_pageCaching_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_pageCaching_game); return true; }),
    TC_SETTING("vsm.windRange", Float, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { vsm::g_windRange_game = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(vsm::g_windRange_game); return true; }),
    // the wind's pages: marked only
    TC_SETTING("vsm.windPageMarks", Bool, SettingTransient,
               [](SettingContext&, float value) { vsm::g_windPageMarks_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_windPageMarks_game); return true; }),
    // the wind's levels in slices
    TC_SETTING("vsm.windSliceTexels", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_windSliceTexels_game = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_windSliceTexels_game); return true; }),
    TC_SETTING("vsm.perInstanceCasterLod", Bool, SettingTransient,
               [](SettingContext&, float value) { vsm::g_perInstanceCasterLod_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_perInstanceCasterLod_game); return true; }),
    // occlusion plan S5b.2
    TC_SETTING("vsm.hzbCull", Bool, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_hzbCull_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_hzbCull_game); return true; }),
    TC_SETTING("vsm.shadowLodBiasNearTier", Bool, SettingPersistent,
               [](SettingContext&, float value) { render::g_shadowLodBiasNearTier_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_shadowLodBiasNearTier_game); return true; }),
    TC_SETTING("vsm.shadowLodTierStride", Float, SettingPersistent,
               [](SettingContext&, float value) { render::g_shadowLodTierStride_game = std::clamp((int)value, 1, 8); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_shadowLodTierStride_game); return true; }),
    // Plan S10b: the VSM local lights' receiver push (the Developer window's VSM section has the same three).
    TC_SETTING("vsm.localTexelBias", Bool, SettingTransient,
               [](SettingContext&, float value) { vsm::g_localTexelBias_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_localTexelBias_game); return true; }),
    TC_SETTING("vsm.localLateralTexels", Float, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_localLateralTexels_game = std::clamp(value, 0.0f, 8.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_localLateralTexels_game); return true; }),
    TC_SETTING("vsm.localDepthPushTexels", Float, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_localDepthPushTexels_game = std::clamp(value, 0.0f, 8.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_localDepthPushTexels_game); return true; }),
    // Plan S10e: UE's dynamic VSM resolution (0 = off).
    TC_SETTING("vsm.dynamicResMaxLodBias", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_dynamicResMaxLodBias_game = std::clamp(value, 0.0f, 4.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_dynamicResMaxLodBias_game); return true; }),
    TC_SETTING("vsm.dynamicResMaxPoolLoad", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_dynamicResMaxPoolLoad_game = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_dynamicResMaxPoolLoad_game); return true; }),
    // Plan S10e: UE's local-light mip by the pixel footprint (0 = by camera distance) and its bias.
    TC_SETTING("vsm.localMipFromPixel", Bool, SettingTransient,
               [](SettingContext&, float value) { vsm::g_localMipFromPixel_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_localMipFromPixel_game); return true; }),
    TC_SETTING("vsm.requestLightCull", Bool, SettingTransient,
               [](SettingContext&, float value) { vsm::g_requestLightCull_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_requestLightCull_game); return true; }),
    TC_SETTING("vsm.waterCasterMask", Int, SettingTransient,
               [](SettingContext&, float value) { vsm::g_waterCasterMask_game = static_cast<int>(value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_waterCasterMask_game); return true; }),
    // S10c
    TC_SETTING("vsm.lightPageRemap", Bool, SettingTransient,
               [](SettingContext&, float value) { vsm::g_lightPageRemap_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_lightPageRemap_game); return true; }),
    TC_SETTING("vsm.maxLightAge", Int, SettingTransient,
               [](SettingContext&, float value) { vsm::g_maxLightAgeFrames_game = static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1000.0f)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_maxLightAgeFrames_game); return true; }),
    TC_SETTING("vsm.lruFrames", Int, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_lruThreshold_game = static_cast<std::uint32_t>(std::clamp(value, 1.0f, static_cast<float>(vsm::kMaxLruFrames))); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_lruThreshold_game); return true; }),
    // Plan S10d: UE r.Shadow.Virtual.DistantLightMode (0 = off, 1 = on) and MaxDistantUpdatePerFrame (< 0 = all).
    // R2.n: the persisted VSM knobs without a row before (clamps as the graphics file's load had them).
    TC_SETTING("vsm.lodRefDistance", Float, SettingPersistent,
               [](SettingContext&, float value)
               {
                   vsm::g_refDist_game = std::isfinite(value) ? std::clamp(value, 1.0f, 40.0f) : vsm::kLodRefDist;
                   return true;
               },
               [](SettingContext&, float& out) { out = vsm::g_refDist_game; return true; }),
    TC_SETTING("vsm.requestDownscale", Int, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_requestDownscale_game = static_cast<std::uint32_t>(std::clamp(value, 1.0f, 8.0f)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_requestDownscale_game); return true; }),
    TC_SETTING("vsm.residentOnly", Bool, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_residentIterOnly_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_residentIterOnly_game); return true; }),
    TC_SETTING("vsm.singleDraw", Bool, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_pageDrawSingle_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_pageDrawSingle_game); return true; }),
    TC_SETTING("vsm.distantLightMode", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_distantLightMode_game = value != 0.0f ? 1 : 0; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_distantLightMode_game); return true; }),
    TC_SETTING("vsm.maxDistantUpdatePerFrame", Int, SettingTransient,
               [](SettingContext&, float value) { vsm::g_maxDistantUpdatesPerFrame_game = static_cast<int>(std::clamp(value, -1.0f, 1000.0f)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_maxDistantUpdatesPerFrame_game); return true; }),
    TC_SETTING("vsm.redrawUnrequestedFrames", Int, SettingTransient,
               [](SettingContext&, float value) { vsm::g_redrawUnrequestedFrames_game = static_cast<std::uint32_t>(std::clamp(value, 0.0f, static_cast<float>(vsm::kMaxLruFrames))); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_redrawUnrequestedFrames_game); return true; }),
    TC_SETTING("vsm.resolutionLodBiasLocal", Float, SettingPersistent,
               [](SettingContext&, float value) { vsm::g_resolutionLodBiasLocal_game = std::clamp(value, -4.0f, 4.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_resolutionLodBiasLocal_game); return true; }),
    // Plan S10e: UE r.Shadow.Virtual.ResolutionLodBiasLocalMoving / r.Shadow.Scene.LightActiveFrameCount.
    TC_SETTING("vsm.resolutionLodBiasLocalMoving", Float, SettingTransient,
               [](SettingContext&, float value) { vsm::g_resolutionLodBiasLocalMoving_game = std::clamp(value, -4.0f, 4.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_resolutionLodBiasLocalMoving_game); return true; }),
    TC_SETTING("vsm.lightActiveFrameCount", Int, SettingTransient,
               [](SettingContext&, float value) { vsm::g_lightActiveFrameCount_game = static_cast<std::uint32_t>(std::clamp(value, 1.0f, 1000.0f)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_lightActiveFrameCount_game); return true; }),
    // Mirror of the VSM page-stats log toggle, so a headless run can capture the resident/request
    // counts (logs/vsm_pages.log) that the dev-window "VSM" tab shows live.
    TC_SETTING("vsm.logPageStats", Bool, SettingTransient,
               [](SettingContext&, float value) { vsm::g_logPageStats_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vsm::g_logPageStats_game); return true; }),
};
TC_SETTINGS(kVsmRows);
}
}
