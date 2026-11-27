// R10.4: Developer Controls > VSM (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"
#include "app/ui/DeveloperTabs.h" // GRAPHICS_CONTROL

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>

#include "app/GraphicsSettings.h"
#include "app/scene/Scene.h"
#include "imgui.h"
#include "rendering/core/Renderer.h"
#include "rendering/visibility/OcclusionHistory.h" // S3a: vis::g_occlusion_game
#include "rendering/meshes/LodSelect.h"
#include "rendering/shadows/VirtualShadowMap.h"
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/shadows/ShadowGpuData.h" // Tessy N3: the cluster counter
#include "rendering/shadows/TessySettings.h"

void DeveloperWindow::DrawVsmTab(Renderer& renderer, Scene& scene, SceneRenderSettings& settings,
    GraphicsSettingsManager& graphicsSettings)
{
    if (ImGui::Button("Reset VSM quality to defaults"))
    {
        graphicsSettings.ResetVsm(renderer, scene, settings);
    }
    ImGui::TextWrapped("Virtual Shadow Maps (Rung 2, experimental) for local lights "
        "(spot + point + glass). Legacy = CSM directional + spot/point atlas. Tunes "
        "quality vs. cost live (no reallocation). Directional still uses CSM until Step 24.");
    ImGui::Separator();
    DrawShadowModeCombo("vsmShadowMode");

    // Applies to BOTH modes (folds GPU-instanced casters into the indirect cull): ON =
    // GI casts in VSM + via indirect in Legacy; OFF = GI reverts to the Legacy CPU tail.
    GRAPHICS_CONTROL(GiIndirectShadows, "giIndirectShadows",
        ImGui::Checkbox("GPU-instanced casters -> indirect/VSM [Ctrl+G]",
                        &render::g_giIndirectShadowsEnabled_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ON: GPU-instanced objects cast shadows in VSM (and via the indirect path in\n"
                          "Legacy), dropping their CPU RenderShadow tail. OFF: Legacy CPU tail only (no VSM).");


    // (Chunked-terrain LOD selection moved to the "LOD" tab — it is a camera-LOD
    // control, not a shadow one; the caster follows the drawn LOD by construction.)

    ImGui::BeginDisabled(!render::VsmActive());

    // S3.6: these two used to live OUTSIDE this block because they also applied to the
    // Legacy cascades. They no longer do: the Legacy/Rung-0 caster LOD now comes from the
    // RECEIVER (UE's rule), so viewLod_ -- the only thing these knobs feed -- is zeroed for
    // cascades and is not read by the Legacy draw at all. A control that claims a scope it
    // does not have is worse than no control, hence the move and the renames.
    GRAPHICS_CONTROL(ShadowLodBias, "shadowLodBias",
        ImGui::SliderInt("Shadow LOD bias (VSM only)",
                         &render::g_shadowLodBias_game, -2, 3));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ADDITIVE offset on the per-view shadow LOD, VSM ONLY (clipmap levels plus\n"
                          "the local lights, which are pinned at tier 0). Each view picks a base LOD by\n"
                          "its tier (near = fine, far = coarse); this shifts the whole curve. 0 = the\n"
                          "tier curve alone; + = coarser everywhere (cheaper), - = sharper.\n"
                          "Legacy CSM ignores it: its casters draw at their RECEIVER's LOD.\n"
                          "Changing it rebuilds the caster tables at GPU idle (a hitch).\n");

    GRAPHICS_CONTROL(ShadowLodBiasNearTier, "shadowLodBiasNearTier",
        ImGui::Checkbox("Bias the NEAREST tier too (VSM only)",
                        &render::g_shadowLodBiasNearTier_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Default OFF. The bias shifts the TIER curve, but clipmap level 0 and the local\n"
                          "lights (pinned at tier 0) have no distance to hide a coarser caster behind, so\n"
                          "biasing them lands as self-shadow blobs on thin shells. Measured on demo.json's\n"
                          "tent, 5 interleaved samples: dark canvas pixels 2698 OFF vs 3996 ON, ~+0.2 ms.\n"
                          "A change rebuilds the caster tables at GPU idle.\n");

    GRAPHICS_CONTROL(ShadowLodTierStride, "shadowLodTierStride",
        ImGui::SliderInt("Shadow tiers per LOD (VSM only)",
                         &render::g_shadowLodTierStride_game, 1, 8));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How many consecutive shadow-view tiers share one caster mesh LOD.\n"
                          "1 = the aggressive 0,1,2,3 curve; 2 = 0,0,1,1,2,2, which avoids changing caster\n"
                          "geometry at every VSM clip boundary. The bias above is added afterwards.\n"
                          "VSM only -- Legacy CSM has no per-view LOD curve at all.\n"
                          "Changing it rebuilds the caster tables (a hitch).\n");

    GRAPHICS_CONTROL(VsmRefDistance, "vsmRefDistance",
        ImGui::SliderFloat("LOD ref distance", &vsm::g_refDist_game,
                           1.0f, 40.0f, "%.1f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Smaller = coarser pages: fewer/faster + more stable, softer near.\n"
                          "Larger = sharper near but more resident pages + higher render cost.\n"
                          "Local lights' surfaces use it only with the pixel-footprint mip off\n"
                          "(--set vsm.localMipFromPixel:0); glass and the fog always.");

    int ds = static_cast<int>(vsm::g_requestDownscale_game);
    if (GRAPHICS_CONTROL(VsmRequestDownscale, "vsmRequestDownscale",
                         ImGui::SliderInt("Request downscale", &ds, 1, 8)))
        vsm::g_requestDownscale_game = static_cast<std::uint32_t>(ds < 1 ? 1 : ds);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Screen sub-sampling for page discovery.\n"
                          "1 = full res (best coverage, costliest); higher = cheaper, may miss pages.");

    int lru = static_cast<int>(vsm::g_lruThreshold_game);
    if (GRAPHICS_CONTROL(VsmLru, "vsmLru",
                         ImGui::SliderInt("LRU eviction frames", &lru, 1, static_cast<int>(vsm::kMaxLruFrames))))
        vsm::g_lruThreshold_game = static_cast<std::uint32_t>(lru < 1 ? 1 : lru);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Frames a resident page survives unrequested before it is freed (UE 1000).\n"
                          "Before that it is taken back only when the pool has no empty page left,\n"
                          "least recently requested first.");

    GRAPHICS_CONTROL(VsmClipmapFirstLevel, "vsmClipmapFirstLevel",
        ImGui::SliderInt("Clipmap first level", &vsm::g_clipmapFirstLevel_game, 0, 16));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE's r.Shadow.Virtual.Clipmap.FirstLevel, 6. Level L is 2^(L+3) cm across, 16384 texels: the\n"
                          "finest texel a shadow can have. What a pixel asks for is its distance, not this.");
    GRAPHICS_CONTROL(VsmResolutionLodBiasDirectional, "vsmResolutionLodBiasDirectional",
        ImGui::SliderFloat("Resolution LOD bias (sun)", &vsm::g_resolutionLodBiasDirectional_game, -4.0f, 4.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE's r.Shadow.Virtual.ResolutionLodBiasDirectional: 0 at High, -1.5 at Epic. On top of the bias\n"
                          "the render resolution asks for: -1 doubles the shadow's resolution and costs up to 4x the pages.\n"
                          "Now %.2f in all.", vsm::g_clipmapFrameBias);
    // ---- SMRT (docs/vsm_smrt_plan.md) --------------------------------------------
    // Marches a ray from the receiver toward the light through the clipmap instead of
    // taking one biased comparison. Directional clipmap only, so it sits with the
    // clipmap knobs and inside the same VsmActive() gate.
    ImGui::Separator();
    bool smrtOn = vsm::g_smrtRayCount_game > 0u;
    // Remembers the ray count across an off/on cycle, so unticking is not destructive.
    // 7 = UE's RayCountDirectional default, and the right thing to hand someone who
    // just ticked the box. ONE ray is the noisiest configuration this feature has: the
    // per-pixel direction jitter is not averaged and the march point-samples one texel
    // per step with no filter, so a single ray measures twice the reference's
    // salt-and-pepper (10.09 vs 5.37; 7 rays 6.06, 16 rays 5.29).
    static int smrtLastRays = 7;
    if (smrtOn)
    {
        smrtLastRays = static_cast<int>(vsm::g_smrtRayCount_game);
    }
    if (GRAPHICS_CONTROL(VsmSmrtEnabled, "vsmSmrtEnabled",
        ImGui::Checkbox("SMRT ray-marched clipmap", &smrtOn)))
    {
        vsm::g_smrtRayCount_game = smrtOn
            ? static_cast<std::uint32_t>(smrtLastRays)
            : 0u;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("OFF = one SampleCmp per level with a constant depth bias (today's path).\n"
                          "ON  = UE's SMRT: a ray from the receiver toward the light, whose own\n"
                          "depth step per sample IS the tolerance -- so the constant bias below is\n"
                          "not applied at all and its sliders grey out.\n\n"
                          "1 ray is HARD-EDGED by design (no dither, no filter). Softness needs\n"
                          "more rays; measured on wind_test it moves shadow coverage by 0.56pp,\n"
                          "i.e. the shadow stays where it was.");

    // The lamps' own (vsm_smrt_local.hlsli): they do not hang on the sun's switch above.
    int localSmrtRays = static_cast<int>(vsm::g_localSmrtRayCount_game);
    if (GRAPHICS_CONTROL(VsmLocalSmrtRays, "vsmLocalSmrtRays",
        ImGui::SliderInt("Lamps' SMRT rays", &localSmrtRays, 0, static_cast<int>(vsm::kSmrtMaxRays))))
        vsm::g_localSmrtRayCount_game = static_cast<std::uint32_t>(localSmrtRays);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rays per pixel and lamp, UE's r.Shadow.Virtual.SMRT.RayCountLocal (4 on High).\n"
                          "0 = a 3 x 3 PCF of one mip instead. Cost lands in Pass_PointLights / Pass_SpotLights.");
    int localSmrtSteps = static_cast<int>(vsm::g_localSmrtSamplesPerRay_game);
    if (GRAPHICS_CONTROL(VsmLocalSmrtSamples, "vsmLocalSmrtSamples",
        ImGui::SliderInt("Lamps' SMRT samples/ray", &localSmrtSteps, 0, static_cast<int>(vsm::kSmrtMaxSamplesPerRay))))
        vsm::g_localSmrtSamplesPerRay_game = static_cast<std::uint32_t>(localSmrtSteps);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Steps along each ray, UE's SamplesPerRayLocal (4 on High). Only a light with a Source Radius\n"
                          "takes them: without one a ray is one sample at the receiver, moved by the dither.");

    ImGui::BeginDisabled(!smrtOn);
    int smrtRays = static_cast<int>(vsm::g_smrtRayCount_game);
    if (GRAPHICS_CONTROL(VsmSmrtRays, "vsmSmrtRays",
        ImGui::SliderInt("SMRT rays", &smrtRays, 1,
                         static_cast<int>(vsm::kSmrtMaxRays))))
    {
        vsm::g_smrtRayCount_game = static_cast<std::uint32_t>(smrtRays);
        smrtLastRays = smrtRays;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rays per pixel; UE's r.Shadow.Virtual.SMRT.RayCountDirectional default is 7.\n"
                          "Cost lands in Pass_Lighting, not in the shadow passes.\n"
                          "Until Step 3 adds the per-ray dither every ray is identical, so counts\n"
                          "above 1 buy nothing yet and only cost.");

    int smrtSteps = static_cast<int>(vsm::g_smrtSamplesPerRay_game);
    if (GRAPHICS_CONTROL(VsmSmrtSamples, "vsmSmrtSamples",
        ImGui::SliderInt("SMRT samples/ray", &smrtSteps, 1,
                         static_cast<int>(vsm::kSmrtMaxSamplesPerRay))))
        vsm::g_smrtSamplesPerRay_game = static_cast<std::uint32_t>(smrtSteps);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Steps along each ray (UE default 8). Too few and a thin occluder is\n"
                          "stepped over, so its shadow disappears rather than softening.");

    GRAPHICS_CONTROL(VsmSmrtTemporal, "vsmSmrtTemporal",
        ImGui::Checkbox("SMRT temporal dither", &vsm::g_smrtTemporalDither_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rotates the sample set once per frame, so the temporal pass sees a\n"
                          "different set each frame and averages them (UE feed StateFrameIndex\n"
                          "into their blue noise the same way).\n\n"
                          "It makes a SINGLE frame noisier and only pays off through DLSS, so\n"
                          "turn it off when judging a still. Measured with DLSS on: -13.7%% noise\n"
                          "at 1 ray, -2.6%% at 7 -- it matters most where quality is worst.");

    int adaptive = static_cast<int>(vsm::g_smrtAdaptiveRayCount_game);
    if (GRAPHICS_CONTROL(VsmSmrtAdaptive, "vsmSmrtAdaptive",
        ImGui::SliderInt("SMRT adaptive after N rays", &adaptive, 0, 8)))
        vsm::g_smrtAdaptiveRayCount_game = static_cast<std::uint32_t>(adaptive);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("After N rays, a wave whose lanes ALL agree stops early (UE default 1).\n"
                          "Fully lit and fully umbral pixels are most of the screen and need one\n"
                          "or two rays; only penumbrae need the full count. 0 disables it.\n\n"
                          "Measured at 7 rays: Pass_Lighting 0.861 -> 0.425 ms, a 2.0x saving for\n"
                          "0.23%% of pixels changed. Compute only -- glass always shoots them all.");

    GRAPHICS_CONTROL(VsmSmrtSunAngle, "vsmSmrtSunAngle",
        ImGui::SliderFloat("SMRT sun angle (deg)",
            &vsm::g_smrtSourceAngleDeg_game, 0.0f, 8.0f, "%.3f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The light angular SIZE -- what a penumbra is actually made of.\n"
                          "UE default is 0.5357 deg, the real sun disc. At 0 every ray collapses\n"
                          "onto the light axis and the ray count buys nothing at all.\n\n"
                          "Raise it for visibly soft shadows: the penumbra then WIDENS with\n"
                          "distance from the contact point, which no single-tap filter can do.");

    GRAPHICS_CONTROL(VsmSmrtTexelDither, "vsmSmrtTexelDither",
        ImGui::SliderFloat("SMRT texel dither",
            &vsm::g_smrtTexelDitherScale_game, 0.0f, 4.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Jitters each ray start by up to this many shadow texels (UE default 2.0),\n"
                          "trading the resolution staircase for noise the temporal pass absorbs.\n"
                          "0 is a clean off switch. Each offset carries its own receiver-plane\n"
                          "bias; without that the jitter is simply acne.");

    GRAPHICS_CONTROL(VsmSmrtRayLength, "vsmSmrtRayLength",
        ImGui::SliderFloat("SMRT ray length scale",
            &vsm::g_smrtRayLengthScale_game, 0.0f, 4.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Ray length as a multiple of distance-to-camera (UE default 1.5). This is\n"
                          "the knob that REPLACES the depth bias as the thing to tune, and UE's own\n"
                          "warning applies: too high detaches shadows from their contact points\n"
                          "unless samples/ray goes up; too low caps how large a penumbra can get.");
    ImGui::EndDisabled();
    ImGui::Separator();

    // The constant depth bias is DEAD on the SMRT path -- VsmClipmapShadow returns from
    // the march before levelDepthBias is even computed. Greyed rather than left live,
    // because a slider that visibly does nothing is worse than an absent one.
    ImGui::BeginDisabled(smrtOn);
    GRAPHICS_CONTROL(VsmDepthBias, "vsmDepthBias",
        ImGui::SliderFloat("Clipmap depth bias", &vsm::g_clipmapDepthBias_game,
                           0.0f, 0.01f, "%.4f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Directional clipmap NDC depth bias at LEVEL 0 (0.0001 = 1.23 shadow texels).\n"
                          "Constant in texels across levels when decay = 1, so its world size doubles per\n"
                          "level -- raise it with decay < 1 or far thin shadows detach.");
    GRAPHICS_CONTROL(VsmDepthBiasDecay, "vsmDepthBiasDecay",
        ImGui::SliderFloat("Depth bias decay /level",
            &vsm::g_clipmapDepthBiasDecay_game, 0.25f, 1.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("bias(L) = max(depthBias * decay^L, floor). 1.0 = legacy constant-in-texels;\n"
                          "0.5 = constant WORLD-size bias (the near value everywhere). Lets the near bias\n"
                          "rise against acne without peter-panning the far levels.");
    GRAPHICS_CONTROL(VsmDepthBiasFloor, "vsmDepthBiasFloor",
        ImGui::SliderFloat("Depth bias floor (texels)",
            &vsm::g_clipmapDepthBiasFloorTexels_game, 0.0f, 1.5f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lower bound of the decayed bias, in texels of the level actually sampled.\n"
                          "The D32 pool has effectively no quantization floor, so 0 is legal -- the\n"
                          "receiver-plane bias carries the slope; raise this only if residual acne shows.");
    ImGui::EndDisabled(); // constant depth bias group (inert under SMRT)

    // NOT in the disabled group: the normal offset is applied on BOTH paths (SMRT keeps
    // it -- it is already UE's formula and it is what stops the ray starting inside the
    // receiver).
    GRAPHICS_CONTROL(VsmNormalBias, "vsmNormalBias",
        ImGui::SliderFloat("Clipmap normal bias (UE units)",
            &vsm::g_clipmapNormalBias_game, 0.0f, 4.0f, "%.3f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Receiver offset along the normal, UE's r.Shadow.Virtual.NormalBias units\n"
                          "(their default 0.5): scaled by distance-to-camera and the FOV, /1000 on the CPU.\n"
                          "Values well above 0.5 are masking a caster/receiver geometry mismatch\n"
                          "(terrain shadow LOD) -- see docs/terrain_shadow_chunking_plan.md.");

    GRAPHICS_CONTROL(VsmLocalLateralBias, "vsmLocalLateralBias",
        ImGui::SliderFloat("Local lateral bias (texels)",
            &vsm::g_localLateralTexels_game, 0.0f, 4.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Spot + point VSM: offset along the geometric normal, in texels of the mip actually\n"
                          "sampled. 1.5 = the 3x3 PCF's reach (less leaves acne at grazing light).\n"
                          "Higher = less acne but the shadow Peter-pans (lifts off the base).");
    GRAPHICS_CONTROL(VsmLocalDepthPush, "vsmLocalDepthPush",
        ImGui::SliderFloat("Local depth push (texels)",
            &vsm::g_localDepthPushTexels_game, 0.0f, 4.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Spot + point VSM: along-the-light-ray depth push in shadow texels,\n"
                          "slope-scaled by 1/N.L. The main acne knob; barely Peter-pans (depth-only).");

    GRAPHICS_CONTROL(VsmResidentOnly, "vsmResidentOnly",
        ImGui::Checkbox("Resident-only render (faster, may flicker)",
                        &vsm::g_residentIterOnly_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ON: render only pages a 3-frame-old snapshot says are resident (fewer CPU\n"
                          "draws), but shadows blink for ~3 frames when the set changes (motion/churn).\n"
                          "OFF: render the whole pool every frame (correct, ~4x the render CPU).\n"
                          "Ignored while 'Single-draw page render' is on (that path skips nothing).");

    GRAPHICS_CONTROL(VsmSingleDraw, "vsmSingleDraw",
        ImGui::Checkbox("Single-draw page render (dormant)",
                        &vsm::g_pageDrawSingle_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ON: one ExecuteIndirect over every (page, group) arg instead of the 1024-page\n"
                          "CPU loop; the per-page viewport becomes a VS clip remap + SV_ClipDistance page\n"
                          "borders. Removes the resident-snapshot blink at no CPU cost. Needs the mega\n"
                          "buffer. OFF: the per-page loop (A/B, and per-page inspection in PIX).\n"
                          "Measured: CPU 0.182 -> 0.058 ms, GPU neutral, frame time unchanged\n"
                          "(the pass records on a worker, so the CPU saving is off the hot path).");

    // Barrier plan step 7 prereq: this one is BOOT-ONLY (`--vsm-page-compact`).
    // It is the only config flag whose two paths rest a buffer in states that cannot
    // be combined — PageArgCount is UAV when off and INDIRECT_ARGUMENT when on, and
    // UAV is exclusive. Toggling it live would invalidate the canonical table the
    // barrier compile seeds from. The other page-draw flag needs no such treatment:
    // its two states are both reads and are declared as a union.
    ImGui::BeginDisabled();
    ImGui::Checkbox("Compact draw args (boot-only: --vsm-page-compact)", &vsm::g_pageDrawCompact_game);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ON: the setup CS appends only NON-EMPTY (page, group) records and the draw\n"
                          "uses that counter as its count buffer, instead of walking all 1024 x groups\n"
                          "fixed-layout records. Measured ~+0.017 ms GPU here: the walk it removes was\n"
                          "already free, the atomic it adds lands in the setup CS. Kept for group-heavy\n"
                          "scenes (records = pages x groups). No effect while single-draw is off.");

    // Occlusion plan S5b.2: the light-space two-pass HZB occlusion of the clipmap pages.
    GRAPHICS_CONTROL(VsmHzbCull, "vsmHzbCull",
        ImGui::Checkbox("Light-space HZB occlusion (two-pass, clipmap pages)",
                        &vsm::g_hzbCull_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A (caster, page) pair that LAST frame's pool pyramid hid is deferred; pass A\n"
                          "draws the rest; the pyramid is rebuilt from the pages pass A drew; the deferred\n"
                          "pairs are retested and pass B draws the survivors into the same pages. Zero\n"
                          "latency, no holes by construction (Unreal's NonNanite.UseHZB). Needs the\n"
                          "scatter + single-draw page render. OFF by measurement (2026-09-04): cost-neutral\n"
                          "on the wall level, +0.11 ms in the palm grove. --set=vsm.hzbCull:0|1");
    {
        const auto& hs = scene.Vsm().HzbStats();
        ImGui::Text("light hzb cull: %s; pairs deferred %u, drawn in pass B %u, past the list %u (frame N-3)",
                    scene.Vsm().HzbCullThisFrame() ? "ON this frame" : "off", hs[0], hs[1], hs[2]);
    }
    ImGui::TextDisabled("Tessy (cluster geometry): its switches and counters are in the Tessy tab.");
    GRAPHICS_CONTROL(VsmPageCaching, "vsmPageCaching",
        ImGui::Checkbox("Page cache", &vsm::g_pageCaching_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Skips re-rendering pages whose content did not change. Two rules make the\n"
                          "cached content valid: (1) wind past the 'wind range' renders\n"
                          "RIGID, so a swaying grove caches its far levels; (2) any view whose MATRIX\n"
                          "changed re-renders wholesale - a local page id has no scroll offset, so a moved\n"
                          "sun/spot would otherwise serve depth for the wrong world rect (a clipmap level's pages move).\n"
                          "Net effect: parked camera + settled lights cache fully; motion redraws\n"
                          "exactly what it invalidates. Off = every resident page every frame.");
    {
        GRAPHICS_CONTROL_WIDTH(VsmWindRange, "vsmWindRange", 105.0f,
            ImGui::SliderFloat("Wind range (m)", &vsm::g_windRange_game, 0.0f, 512.0f, "%.0f"));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Shadows within this distance of the camera sway; past it the casters draw RIGID, so the\n"
                              "page cache can keep them (a cached page and a fresh render agree exactly). Its levels:\n"
                              "now the first %u. The cost is the swaying casters' pages redrawn every frame.",
                              vsm::ClipmapWindLevels());
    }
    ImGui::Checkbox("Wind redraws only swaying pages", &vsm::g_windPageMarks_game);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ON: of the wind range's levels, only the pages a swaying caster lies on (within the wind\n"
                          "range of the camera, this frame or last) re-render, and none while the wind is frozen and\n"
                          "the camera still -- UE's per-instance WPO invalidation. The same picture: a page with nothing\n"
                          "swaying draws the same depth. OFF = every page of those levels every frame (the A/B).\n"
                          "--set=vsm.windPageMarks:0|1");
    ImGui::SetNextItemWidth(105.0f);
    ImGui::SliderFloat("Wind pose step (texels)", &vsm::g_windSliceTexels_game, 0.0f, 4.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A clipmap level takes the wind's pose (and redraws its swaying pages) only once the fastest\n"
                          "swaying vertex could have moved this many of its texels since the last time -- a bound from\n"
                          "the sway's own formula. Near levels (fine texels) take every frame; a level of 8 cm texels\n"
                          "a few times a second. A shadow lags its tree by at most this many of its own texels.\n"
                          "0 = every level every frame. --set=vsm.windSliceTexels");

    ImGui::Checkbox("Log page stats to debug output", &vsm::g_logPageStats_game);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Periodically print \"[VSM] request ... | resident=...\" to DBWIN. Off by default\n"
                          "(spammy); the live numbers above/below always update regardless.");

    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::TextDisabled("Pool: %u pages (%ux%u). refDist=%.1f downscale=%u lru=%u",
        vsm::kPoolPageCount, vsm::kPoolTexelsX, vsm::kPoolTexelsY,
        vsm::g_refDist_game, vsm::g_requestDownscale_game, vsm::g_lruThreshold_game);

    // --- Live page stats + physical-pool residency grid (kFrameCount-old snapshot). ---
    ImGui::Separator();
    ImGui::TextUnformatted("Pages");
    const VirtualShadowMap::DebugStats& vstats = scene.Vsm().Stats();
    if (!render::VsmActive())
    {
        ImGui::TextDisabled("(enable VSM to sample live page stats)");
    }
    else if (!vstats.valid)
    {
        ImGui::TextDisabled("(sampling\xE2\x80\xA6 move the camera to populate)");
    }
    else
    {
        const float frac = static_cast<float>(vstats.resident) /
                           static_cast<float>(vsm::kPoolPageCount);
        char resLabel[64];
        std::snprintf(resLabel, sizeof(resLabel), "%u / %u resident (%.0f%%)",
            vstats.resident, vsm::kPoolPageCount, frac * 100.0f);
        ImGui::ProgressBar(frac, ImVec2(240.0f, 0.0f), resLabel);
        ImGui::Text("Requested %u  |  new this frame %u", vstats.requested, vstats.newAlloc);
        if (vstats.fail > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.30f, 1.0f),
                "Alloc fails: %u (pool oversubscribed \xE2\x80\x94 raise refDist/downscale or lower LOD)",
                vstats.fail);
        else
            ImGui::TextDisabled("Alloc fails: 0");
        ImGui::Text("Request LOD  L0=%u  L1=%u  L2=%u  L3=%u  L4=%u  L5=%u  L6=%u  L7=%u",
            vstats.perLevel[0], vstats.perLevel[1], vstats.perLevel[2], vstats.perLevel[3],
            vstats.perLevel[4], vstats.perLevel[5], vstats.perLevel[6], vstats.perLevel[7]);

        const std::vector<std::uint32_t>& owners = scene.Vsm().PhysOwnerSnapshot();
        if (owners.size() == vsm::kPoolPageCount)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("Physical pool %ux%u pages \xE2\x80\x94 colour = owning shadow view:",
                vsm::kPoolPagesX, vsm::kPoolPagesY);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const int   axis = static_cast<int>(vsm::kPoolPagesX);
            const int   rows = static_cast<int>(vsm::kPoolPagesY);
            const float cell = 3.0f, gap = 1.0f, pitch = cell + gap;
            for (int py = 0; py < rows; ++py)
            {
                for (int px = 0; px < axis; ++px)
                {
                    const std::uint32_t owner = owners[static_cast<size_t>(py) * axis + px];
                    ImU32 col;
                    if (owner == 0xFFFFFFFFu)
                    {
                        col = IM_COL32(38, 38, 44, 255); // free
                    }
                    else
                    {
                        const std::uint32_t view = vsm::ViewOfPage(owner);
                        const bool clip = vsm::IsClipmapView(view); // directional clipmap
                        const std::uint32_t views = std::max(vsm::MaxVirtualViews(), 1u);
                        const float hue = static_cast<float>(view % views) / static_cast<float>(views);
                        col = ImColor::HSV(hue, clip ? 0.85f : 0.55f, clip ? 1.0f : 0.85f);
                    }
                    const ImVec2 a(origin.x + px * pitch, origin.y + py * pitch);
                    dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), col);
                }
            }
            ImGui::Dummy(ImVec2(axis * pitch, rows * pitch));
            if (ImGui::IsItemHovered())
            {
                const ImVec2 m = ImGui::GetIO().MousePos;
                const int cx = static_cast<int>((m.x - origin.x) / pitch);
                const int cy = static_cast<int>((m.y - origin.y) / pitch);
                if (cx >= 0 && cx < axis && cy >= 0 && cy < rows)
                {
                    const std::uint32_t owner = owners[static_cast<size_t>(cy) * axis + cx];
                    if (owner == 0xFFFFFFFFu)
                        ImGui::SetTooltip("phys #%d (%d,%d): free", cy * axis + cx, cx, cy);
                    else
                    {
                        const std::uint32_t view = vsm::ViewOfPage(owner);
                        // Plan S10b: [prefix spots | prefix faces | clipmap | more spots | more faces].
                        const std::uint32_t moreSpotsEnd = vsm::SpotView(render::ShadowSpotCapacity() - 1u) + 1u;
                        const char* kind = vsm::IsClipmapView(view) ? "directional clipmap"
                            : ((view < vsm::PointFaceView(0u) || (view >= vsm::SpotView(render::kShadowViewVsmSpots) &&
                                                                  view < moreSpotsEnd)) ? "spot" : "point face");
                        ImGui::SetTooltip("phys #%d (%d,%d)\nview %u (%s)",
                            cy * axis + cx, cx, cy, view, kind);
                    }
                }
            }
        }
    }
}

#endif // !TC_SHIPPING
