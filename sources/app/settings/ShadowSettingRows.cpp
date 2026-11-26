#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/scene/SceneRenderConfig.h"
#include "rendering/shadows/ShadowSettings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// --set / --sweep / MCP set_settings names shadow.*, csm.*, sdsm.*, contact.* (the sun's and the lamps' shadows)
// (architecture plan R2.2). Each row is the branch App.cpp's ApplySweepValue had, its clamp included, and the reader
// ReadSweepValue had (R2.2 gave every simple name one first, so --settings-dump compared the move); a row without a
// reader had none.
namespace settings
{
namespace
{
const SettingDesc kShadowRows[] = {
    // Contact shadows (S12) -- shadow-mode independent, so they are not under vsm.*
    TC_SETTING("contact.enabled", Bool, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { render::contact::g_enabled_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = (render::contact::g_enabled_game) ? 1.0f : 0.0f; return true; }),
    TC_SETTING("contact.length", Float, SettingPersistent,
               [](SettingContext&, float value) { render::contact::g_length_game = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::contact::g_length_game); return true; }),
    TC_SETTING("contact.lengthInWorldSpace", Bool, SettingPersistent,
               [](SettingContext&, float value) { render::contact::g_lengthInWorldSpace_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::contact::g_lengthInWorldSpace_game); return true; }),
    TC_SETTING("contact.normalOffset", Float, SettingPersistent,
               [](SettingContext&, float value) { render::contact::g_normalOffsetFrac_game = std::clamp(value, 0.0f, 2.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::contact::g_normalOffsetFrac_game); return true; }),
    TC_SETTING("contact.grazingFade", Float, SettingPersistent,
               [](SettingContext&, float value) { render::contact::g_grazingFadeNdotL_game = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::contact::g_grazingFadeNdotL_game); return true; }),
    TC_SETTING("contact.minDistance", Float, SettingPersistent,
               [](SettingContext&, float value) { render::contact::g_minDistanceM_game = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::contact::g_minDistanceM_game); return true; }),
    TC_SETTING("contact.maxDistance", Float, SettingPersistent,
               [](SettingContext&, float value) { render::contact::g_maxDistanceM_game = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::contact::g_maxDistanceM_game); return true; }),
    TC_SETTING("contact.fadeBand", Float, SettingPersistent,
               [](SettingContext&, float value) { render::contact::g_fadeBandM_game = std::max(0.1f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::contact::g_fadeBandM_game); return true; }),
    TC_SETTING("contact.thickness", Float, SettingPersistent,
               [](SettingContext&, float value) { render::contact::g_maxThicknessFrac_game = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::contact::g_maxThicknessFrac_game); return true; }),
    TC_SETTING("contact.temporal", Bool, SettingPersistent,
               [](SettingContext&, float value) { render::contact::g_temporalDither_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::contact::g_temporalDither_game); return true; }),
    TC_SETTING("contact.localMode", Float, SettingPersistent,
               [](SettingContext&, float value) { render::contact::g_localMode_game = (std::uint32_t)std::clamp(value, 0.0f, 3.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::contact::g_localMode_game); return true; }),
    TC_SETTING("contact.intensity", Float, SettingPersistent,
               [](SettingContext&, float value) { render::contact::g_intensity_game = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::contact::g_intensity_game); return true; }),
    TC_SETTING("contact.steps", Float, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { render::contact::g_steps_game = (std::uint32_t)std::clamp(value, 1.0f, 32.0f); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(render::contact::g_steps_game)); return true; }),
    // S8 A/B lever: 1 = soft ramp + Gather tent (default), 0 = the legacy 3x3 SampleCmp box.
    TC_SETTING("csm.filterMode", Float, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext& c, float value) { c.scene.CascadeConfig().filterMode = (uint32_t)std::clamp((int)value, 0, 2); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(c.scene.CascadeConfig().filterMode)); return true; }),
    // The three filter knobs beside the kernel selector, so a softness A/B is a command line.
    // Artist units here, exactly as the sliders show them: the *7+1 mapping to shader units
    // happens once, on the CPU, where UE do it too.
    TC_SETTING("csm.sharpen", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.scene.CascadeConfig().shadowFilterSharpen = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CascadeConfig().shadowFilterSharpen); return true; }),
    TC_SETTING("csm.receiverBias", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.scene.CascadeConfig().csmReceiverBias = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CascadeConfig().csmReceiverBias); return true; }),
    TC_SETTING("csm.overBlurCorrect", Bool, SettingPersistent,
               [](SettingContext& c, float value) { c.scene.CascadeConfig().pcfOverBlurCorrection = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CascadeConfig().pcfOverBlurCorrection); return true; }),
    // S6: UE's three legacy-CSM bias cvars.
    TC_SETTING("csm.slopeScale", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.scene.CascadeConfig().slopeScale = std::max(0.0f, value); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CascadeConfig().slopeScale); return true; }),
    TC_SETTING("csm.maxSlope", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.scene.CascadeConfig().maxSlope = std::max(0.0f, value); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CascadeConfig().maxSlope); return true; }),
    TC_SETTING("csm.depthBias", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.scene.CascadeConfig().depthBiasInTexels = std::max(0.0f, value); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CascadeConfig().depthBiasInTexels); return true; }),
    TC_SETTING("csm.normalBias", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.scene.CascadeConfig().normalBiasInTexels = std::max(0.0f, value); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CascadeConfig().normalBiasInTexels); return true; }),
    // Shadow distance, headless. S10's terminator lives at this plane, and on a small level
    // it is otherwise off-screen -- pulling it in is how the fade gets tested at all.
    TC_SETTING("csm.maxDistance", Float, SettingTransient,
               [](SettingContext& ctx, float value)
               {
                   CascadeShadowConfig& c = ctx.scene.CascadeConfig();
                   const float f = std::max(1.0f, value);
                   const float scale = f / std::max(1e-3f, c.maxDistance);
                   c.maxDistance = f;
                   // Scale the authored splits with it, or BuildSplitScheme clamps them all to the new
                   // far and every cascade collapses onto the same plane.
                   for (float& d : c.sliceDistances) { d *= scale; }
                   return true;
               },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.CascadeConfig().maxDistance); return true; }),
    // S10. UE clamp CascadeTransitionFraction to 0.3; the same cap applies to both.
    TC_SETTING("csm.blendFraction", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.scene.CascadeConfig().blendFraction = std::clamp(value, 0.0f, 0.3f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CascadeConfig().blendFraction); return true; }),
    TC_SETTING("csm.distanceFade", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.scene.CascadeConfig().distanceFadeFraction = std::clamp(value, 0.0f, 0.3f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CascadeConfig().distanceFadeFraction); return true; }),
    // S11 [r.Shadow.CSMScissorOptim]: scissor each cascade's depth pass to the camera's view cone.
    TC_SETTING("csm.scissorOptim", Bool, SettingPersistent,
               [](SettingContext& c, float value) { c.scene.CascadeConfig().scissorOptim = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CascadeConfig().scissorOptim); return true; }),
    TC_SETTING("csm.scissorPad", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.scene.CascadeConfig().scissorPadTexels = std::clamp(value, 0.0f, 64.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CascadeConfig().scissorPadTexels); return true; }),
    // S14 [UE ShadowBoundsAccurate]: cull cascade casters against the slice extruded toward the sun.
    TC_SETTING("csm.accurateCull", Bool, SettingPersistent,
               [](SettingContext& c, float value) { c.scene.CascadeConfig().accurateCasterCull = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CascadeConfig().accurateCasterCull); return true; }),
    // occlusion plan S5b
    TC_SETTING("csm.hzbCull", Bool, SettingPersistent,
               [](SettingContext& c, float value) { c.scene.CascadeConfig().hzbCull = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CascadeConfig().hzbCull); return true; }),
    // The atlas edge, shared by Legacy and SDSM. A process global (it outlives a level switch
    // and is a RESOURCE size, not per-scene tuning); applied at GPU idle by
    // Scene::ReconcileShadowMode, so setting it here is safe at any time.
    TC_SETTING("csm.atlasRes", Float, SettingPersistent | SettingScalable | SettingCostIdleGpu,
               [](SettingContext&, float value) { render::g_csmAtlasRes_game = static_cast<unsigned>(std::clamp(value, static_cast<float>(render::kCsmAtlasResMin), static_cast<float>(render::kCsmAtlasResMax))); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(render::g_csmAtlasRes_game)); return true; }),
    // S15 (SDSM). Process globals, so no `scene` -- they must survive a level switch and be
    // settable before any Scene exists, exactly like the mode switch itself.
    TC_SETTING("sdsm.partitions", Float, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { render::sdsm::g_partitions_game = (std::uint32_t)std::clamp(value, 1.0f, (float)render::sdsm::kMaxPartitions); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(render::sdsm::g_partitions_game)); return true; }),
    TC_SETTING("sdsm.borderTexels", Float, SettingPersistent,
               [](SettingContext&, float value) { render::sdsm::g_borderTexels_game = std::clamp(value, 0.0f, 32.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::sdsm::g_borderTexels_game); return true; }),
    TC_SETTING("sdsm.dilation", Float, SettingPersistent,
               [](SettingContext&, float value) { render::sdsm::g_dilation_game = std::clamp(value, 0.0f, 0.45f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::sdsm::g_dilation_game); return true; }),
    TC_SETTING("sdsm.minScaleOverSphere", Float, SettingPersistent,
               [](SettingContext&, float value) { render::sdsm::g_minScaleOverSphere_game = std::clamp(value, 0.001f, 1.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::sdsm::g_minScaleOverSphere_game); return true; }),
    TC_SETTING("sdsm.zMargin", Float, SettingPersistent,
               [](SettingContext&, float value) { render::sdsm::g_zMargin_game = std::clamp(value, 0.0f, 1000.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::sdsm::g_zMargin_game); return true; }),
    TC_SETTING("sdsm.stabilityFrames", Float, SettingPersistent,
               [](SettingContext&, float value) { render::sdsm::g_stabilityFrames_game = std::clamp(value, 0.0f, 240.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::sdsm::g_stabilityFrames_game); return true; }),
    TC_SETTING("sdsm.cacheFrames", Int, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { render::sdsm::g_cacheFrames_game = static_cast<std::uint32_t>(std::clamp(value, 1.0f, 32.0f)); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(render::sdsm::g_cacheFrames_game)); return true; }),
    TC_SETTING("sdsm.cacheDilation", Float, SettingPersistent,
               [](SettingContext&, float value) { render::sdsm::g_cacheDilation_game = std::clamp(value, 0.0f, 0.5f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::sdsm::g_cacheDilation_game); return true; }),
    TC_SETTING("sdsm.texelSnap", Bool, SettingPersistent,
               [](SettingContext&, float value) { render::sdsm::g_texelSnap_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::sdsm::g_texelSnap_game); return true; }),
    TC_SETTING("sdsm.sizeHysteresis", Float, SettingPersistent,
               [](SettingContext&, float value) { render::sdsm::g_sizeHysteresis_game = std::clamp(value, 0.0f, 0.5f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::sdsm::g_sizeHysteresis_game); return true; }),
    TC_SETTING("sdsm.evsm", Bool, SettingPersistent,
               [](SettingContext&, float value) { render::sdsm::g_evsm_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::sdsm::g_evsm_game); return true; }),
    TC_SETTING("sdsm.evsmPos", Float, SettingPersistent,
               [](SettingContext&, float value) { render::sdsm::g_evsmPos_game = std::clamp(value, 1.0f, render::sdsm::kEvsmMaxExponent); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::sdsm::g_evsmPos_game); return true; }),
    TC_SETTING("sdsm.evsmNeg", Float, SettingPersistent,
               [](SettingContext&, float value) { render::sdsm::g_evsmNeg_game = std::clamp(value, 1.0f, render::sdsm::kEvsmMaxExponent); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::sdsm::g_evsmNeg_game); return true; }),
    TC_SETTING("sdsm.blur", Float, SettingPersistent,
               [](SettingContext&, float value) { render::sdsm::g_evsmBlur_game = std::clamp(value, 0.0f, 0.25f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::sdsm::g_evsmBlur_game); return true; }),
    TC_SETTING("sdsm.blurMaxTexels", Float, SettingPersistent,
               [](SettingContext&, float value) { render::sdsm::g_evsmBlurMaxTexels_game = std::clamp(value, 1.0f, 64.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::sdsm::g_evsmBlurMaxTexels_game); return true; }),
    // The Ctrl-key toggle, headless. Two draw paths write the SAME atlas (GPU-driven indirect
    // vs the CPU object walk) and S6 has to bias both identically -- that is only checkable
    // if a capture can select the path.
    // The sun's shadow machinery, 0 Legacy CSM / 1 VSM / 2 SDSM -- the developer window's combo (Ctrl+V)
    // writes the same global; Scene::ReconcileShadowMode applies it at its GPU-idle point.
    TC_SETTING("shadow.mode", Int, SettingPersistent | SettingScalable | SettingCostIdleGpu,
               [](SettingContext&, float value) { render::g_shadowMode_game = static_cast<render::ShadowMode>(std::clamp(static_cast<int>(value), 0, 2)); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(static_cast<std::uint32_t>(render::g_shadowMode_game))); return true; }),
    TC_SETTING("shadow.indirect", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_indirectShadowsEnabled_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_indirectShadowsEnabled_game); return true; }),
    // Local-light shadow plan S1: 1 = UE's spot / point biases, 0 = the old ones (the A/B).
    TC_SETTING("shadow.localBiasUe", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowBiasUe_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowBiasUe_game); return true; }),
    // Plan S2: 1 = UE's spot / point filters, 0 = the old 3x3 boxes.
    TC_SETTING("shadow.localFilterUe", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowFilterUe_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowFilterUe_game); return true; }),
    // Plan S3: 1 = UE's shadow fade by on-screen resolution, 0 = every shadowed light at full strength.
    TC_SETTING("shadow.localFadeUe", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowFadeUe_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowFadeUe_game); return true; }),
    // Plan S4: 1 = UE's LINEAR spot depth (SV_Depth) and its 1 mm near, 0 = the perspective depth and 0.1 m.
    TC_SETTING("shadow.spotLinearDepth", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_spotShadowLinearDepth_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_spotShadowLinearDepth_game); return true; }),
    // ...the point cube faces' linear depth, and UE's 1 cm near for them (0 = the old max(0.2, 2 % of the radius)).
    TC_SETTING("shadow.pointLinearDepth", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_pointShadowLinearDepth_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_pointShadowLinearDepth_game); return true; }),
    TC_SETTING("shadow.pointNearUe", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_pointShadowNearUe_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_pointShadowNearUe_game); return true; }),
    // Peter-panning hunt: a multiplier on the atlas-mode local lights' UE shadowBias.
    // The peter-panning fix: the TEXEL bias (VSM's local model on the atlases) and its two sizes in shadow texels.
    TC_SETTING("shadow.localBiasTexel", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowBiasTexel_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowBiasTexel_game); return true; }),
    TC_SETTING("shadow.localNormalTexels", Float, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowNormalTexels_game = std::clamp(value, 0.0f, 8.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowNormalTexels_game); return true; }),
    TC_SETTING("shadow.localPushTexels", Float, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowPushTexels_game = std::clamp(value, 0.0f, 8.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowPushTexels_game); return true; }),
    // S6a
    TC_SETTING("shadow.localCompact", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowCompact_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowCompact_game); return true; }),
    // S7
    TC_SETTING("shadow.localCache", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowCache_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowCache_game); return true; }),
    // S7
    TC_SETTING("shadow.localCacheWind", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowCacheWind_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowCacheWind_game); return true; }),
    // The lamps' shadows sway only within this many metres of the camera (< 0 = everywhere).
    TC_SETTING("shadow.localWindRange", Float, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { render::g_localShadowWindRange_game = std::min(value, 100000.0f); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(render::g_localShadowWindRange_game); return true; }),
    // S7
    TC_SETTING("shadow.localDirectRead", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowDirectRead_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowDirectRead_game); return true; }),
    // Plan S9e: UE's caster culling for the local views -- the light's bounds, the light + camera hull, the radius
    // threshold (a share of the distance to the camera, 0 = off).
    TC_SETTING("shadow.localBoundsCull", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowBoundsCull_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowBoundsCull_game); return true; }),
    TC_SETTING("shadow.localHullCull", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowHullCull_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowHullCull_game); return true; }),
    TC_SETTING("shadow.localRadiusThreshold", Float, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowRadiusThreshold_game = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowRadiusThreshold_game); return true; }),
    TC_SETTING("shadow.cullValidate", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_shadowCullRevalidate_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_shadowCullRevalidate_game); return true; }),
    TC_SETTING("shadow.localViewsPerList", Float, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowViewsPerList_game = static_cast<unsigned>(std::clamp(value, 1.0f, 128.0f)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowViewsPerList_game); return true; }),
    TC_SETTING("shadow.localCacheMaxPages", Float, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowCacheMaxPages_game = static_cast<unsigned>(std::clamp(value, 1.0f, static_cast<float>(render::kLocalShadowCacheMaxPagesLimit))); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowCacheMaxPages_game); return true; }),
    // Plan S4: UE r.Shadow.MaxResolution for the spot / point atlases (Scene::ReconcileShadowMode reallocates).
    TC_SETTING("shadow.localMaxRes", Float, SettingPersistent | SettingScalable | SettingCostIdleGpu,
               [](SettingContext&, float value) { render::g_localShadowMaxRes_game = static_cast<unsigned>(std::clamp(value, static_cast<float>(render::kLocalShadowMaxResMin), static_cast<float>(render::kLocalShadowMaxResMax))); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(render::g_localShadowMaxRes_game)); return true; }),
    // Plan S9: the most atlas pages each local-shadow pool may grow to (ReconcileShadowMode).
    TC_SETTING("shadow.localMaxPages", Float, SettingTransient,
               [](SettingContext&, float value) { render::g_localShadowMaxPages_game = static_cast<unsigned>(std::clamp(value, 1.0f, static_cast<float>(render::kLocalShadowMaxPagesLimit))); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_localShadowMaxPages_game); return true; }),
    // The Ctrl-G toggle, headless: GI casters folded into the indirect cull. OFF is what lets the
    // one-shot GPU-vs-CPU cull validator run (its CPU reference covers static casters only), so
    // a headless gate can read its PASS/MISMATCH verdict from the session log.
    TC_SETTING("shadow.giIndirect", Bool, SettingPersistent,
               [](SettingContext&, float value) { render::g_giIndirectShadowsEnabled_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_giIndirectShadowsEnabled_game); return true; }),
};
TC_SETTINGS(kShadowRows);
}
}
