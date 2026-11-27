// R10.4: Developer Controls > Reflections (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"
#include "app/ui/DeveloperTabs.h" // GRAPHICS_CONTROL

#include <algorithm>
#include <cstdint>
#include <string>

#include "app/GraphicsSettings.h"
#include "app/scene/Scene.h"
#include "imgui.h"
#include "rendering/core/Renderer.h"
#include "rendering/shadows/ShadowSettings.h"
#include "ocean/OceanSimulation.h"

namespace
{
    constexpr const char* kSsrTechniqueLabels[] = {
        "Log March",
        "UE SSR (HZB march)",
    };

    constexpr const char* kUeSsrQualityLabels[] = {
        "Custom",
        "UE Low (8x1)",
        "UE Medium (16x1)",
        "UE High (8x4)",
        "UE Epic (12x12)",
    };
}

void DeveloperWindow::DrawReflectionsTab(Renderer& renderer, Scene& scene, SceneRenderSettings& settings,
    GraphicsSettingsManager& graphicsSettings)
{
    ImGui::SeparatorText("Source");
    // Reflection source (S8): None / Sky only / SSR / RT. RT is greyed out on
    // non-RT hardware (and the renderer falls back to SSR anyway).
    const bool rtSupported = renderer.IsRaytracingSupported();
    const char* srcLabels[] = { "None", "Sky only", "SSR", "RT" };
    constexpr int srcCount = static_cast<int>(ReflectionSource::Count);
    int curSrc = static_cast<int>(settings.reflectionSource);
    if (curSrc < 0 || curSrc >= srcCount) { curSrc = static_cast<int>(ReflectionSource::SSR); }
    GRAPHICS_CONTROL(ReflectionSource, "reflectionSource", [&]()
    {
        bool changed = false;
        if (ImGui::BeginCombo("Reflections [F5]", srcLabels[curSrc]))
        {
            for (int i = 0; i < srcCount; ++i)
            {
                const bool isRT = (i == static_cast<int>(ReflectionSource::RT));
                ImGui::BeginDisabled(isRT && !rtSupported);
                if (ImGui::Selectable(srcLabels[i], curSrc == i))
                {
                    settings.reflectionSource = static_cast<ReflectionSource>(i);
                    changed = true;
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        return changed;
    }());
    if (!rtSupported)
    {
        ImGui::TextDisabled("RT requires hardware ray tracing (unavailable).");
    }

    float ssrScale = renderer.GetReflectionTextureScale().x;
    if (GRAPHICS_CONTROL(ReflectionResolution, "reflectionResolution",
        ImGui::SliderFloat("Reflection resolution", &ssrScale,
                           0.25f, 1.0f, "%.2f")))
    {
        renderer.SetReflectionTextureScale(ssrScale);
    }
    DevHelp("Both reflection resolutions are fractions of the RENDER resolution -- what goes into "
            "DLSS, the display with DLSS off -- so a DLSS mode changes their cost with the rest "
            "of the frame. 1.0 = one trace per rendered pixel; more could not add detail.");
    ImGui::Text("Reflection target: %ux%u (render %ux%u)", renderer.GetReflectionTextureWidth(),
                renderer.GetReflectionTextureHeight(), renderer.GetRenderWidth(), renderer.GetRenderHeight());
    // S15b: glass off-screen reflections render into a glass G-buffer + glassReflection at
    // this same reflection resolution and follow the reflection source below.
    // Inspect them via Texture inspector [F4] -> "Glass Refl Normal/Depth" + "Glass Reflection".
    ImGui::TextDisabled("Glass: traced in SSR/RT, cubemap in Sky only, disabled in None.");
    float oceanReflectionScale = renderer.GetOceanReflectionTextureScale().x;
    if (GRAPHICS_CONTROL(OceanReflectionResolution, "oceanReflectionResolution",
        ImGui::SliderFloat("Ocean reflection resolution",
            &oceanReflectionScale, 0.25f, 1.0f, "%.2f")))
    {
        renderer.SetOceanReflectionTextureScale(oceanReflectionScale);
    }
    ImGui::Text("Ocean reflection target: %ux%u", renderer.GetOceanReflectionTextureWidth(), renderer.GetOceanReflectionTextureHeight());
    GRAPHICS_CONTROL(WaterReflectionSurfaceNormal, "waterReflectionSurfaceNormal",
        ImGui::Checkbox("Water reflection: trace the surface normal (UE)",
                        &settings.waterReflectionSurfaceNormal));
    DevHelp("Off (shipped): the water is traced as a flat mirror from the wave's own point and "
            "the wave shape comes back in the composite, which reads that mirror at the old planar "
            "UV offset. On: UE's way -- every pixel casts its ray about its own reflection normal "
            "and reads its own result. Measured 2026-10-12 on the lagoon: with all 4 cascades in "
            "the normal it is pixel grit; with 2 it gives clean wavy reflections. Pair it with "
            "the cascades below at 2 and ocean reflection resolution 1.0 (+0.009 ms at DLSS "
            "Balanced) or 0.75 (today's cost); 0.5 shows 2x2 texels without DLSS.");
    int waterNormalCascades = static_cast<int>(settings.waterReflectionNormalCascades);
    if (GRAPHICS_CONTROL(WaterReflectionNormalCascades, "waterReflectionNormalCascades",
        ImGui::SliderInt("Water reflection normal cascades", &waterNormalCascades, 1, 4)))
    {
        settings.waterReflectionNormalCascades = static_cast<std::uint32_t>(waterNormalCascades);
    }
    DevHelp("How many FFT cascades, largest first, the water's reflection normal takes -- the "
            "normal the trace above casts about (and, with it off, what the UV offset reads). "
            "The default preset's patches are 600 / 56 / 5.3 / 0.5 m; the last one is already "
            "faded out ~20 m from the camera, so 3 looks like 4 on far water. 4 = the full "
            "shading normal; 2 = the swell only; 1 = an almost calm mirror. The sky in the "
            "reflection and the sun glints keep the full normal either way.");
    GRAPHICS_CONTROL(WaterReflectionNormalFilter, "waterReflectionNormalFilter",
        ImGui::SliderFloat("Water reflection normal filter (mips)", &settings.waterReflectionNormalFilter,
                           0.0f, 4.0f, "%.2f"));
    DevHelp("How many mips coarser than the shading normal the water reflection's DIRECTION is "
            "taken -- the sky's and the screen-space ray's (the surf-sim surface). A mip is the "
            "pixel's footprint, so 2 keeps every ripple wider than ~4 pixels and drops the ones a "
            "single mirror sample per pixel can only alias; their slope variance goes to the sky's "
            "blur. The Fresnel weight, the sun glints, the foam and the body keep the full normal. "
            "0 = the shading normal (the default). 2-3 turn a still frame's salt-and-pepper "
            "reflection into coherent ripples and show the swell's sky on sun-lit water; the boil "
            "in motion stays. ~+0.008 ms above 0.");
    GRAPHICS_CONTROL(WaterReflectionPrefilter, "waterReflectionPrefilter",
        ImGui::SliderFloat("Water reflection prefilter (px)", &settings.waterReflectionPrefilter,
                           0.0f, 3.0f, "%.2f"));
    DevHelp("A pixel reconstruction filter on the water's reflection (the screen-space hit, and the "
            "sky where the ray missed): a Gaussian of this many render pixels across the screen's "
            "columns, half along them, over the neighbouring water. Each water pixel casts one mirror "
            "ray from ripples finer than the pixel, so near the shore neighbouring rays land on the "
            "beach, a trunk, the sky between the crowns -- runs of pixels without DLSS, stretched "
            "smears under it (DLSS cannot follow ripples). 1 (default) = clean ripples; 0 = off.");
    GRAPHICS_CONTROL(WaterReflectionTemporal, "waterReflectionTemporal",
        ImGui::Checkbox("Water reflection temporal resolve (without DLSS)", &settings.waterReflectionTemporal));
    DevHelp("The water reflection's OWN temporal resolve, separate from the one below. It runs "
            "only while DLSS is off -- UE's rule (IsSSRTemporalPassRequired): with a temporal "
            "upscaler the two accumulators in a row turn the trace's grain into crawling "
            "blotches. Without DLSS the raw trace flickers and the resolve calms it. Off by "
            "default: it judges stillness by the water SURFACE's motion, ~0 with a still "
            "camera, so any still inertia stretches the history while the waves change the "
            "reflection every frame -- it smears.");
    if (settings.waterReflectionTemporal && renderer.IsDlssActive() &&
        !settings.waterReflectionTemporalUnderDlss)
    {
        ImGui::TextDisabled("  idle: DLSS accumulates the frame");
    }
    ImGui::BeginDisabled(!settings.waterReflectionTemporal);
    {
        GRAPHICS_CONTROL_WIDTH(WaterReflectionTemporalBlend, "waterReflectionTemporalBlend", 98.0f,
            ImGui::SliderFloat("Water temporal blend", &settings.waterReflectionTemporalBlend,
                               0.02f, 1.0f, "%.3f"));
        DevHelp("Weight of the CURRENT frame in the water reflection's resolve (1 = no "
                "accumulation). Same meaning as the deferred one's below, its own value.");
        GRAPHICS_CONTROL_WIDTH(WaterReflectionTemporalStillInertia,
            "waterReflectionTemporalStillInertia", 98.0f,
            ImGui::SliderFloat("Water temporal still inertia",
                               &settings.waterReflectionTemporalStillInertia, 0.0f, 2.0f, "%.2f"));
        DevHelp("Extra inertia for 'still' pixels -- but under water 'still' is the SURFACE's "
                "motion, so a still camera over moving waves gets the relaxed clamp and the "
                "long history: that is the smear. 0 (default) = the hard clamp and the plain "
                "blend above.");
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Filtering");
    GRAPHICS_CONTROL(ReflectionTemporal, "reflectionTemporal",
        ImGui::Checkbox("Reflection temporal resolve", &settings.ssrTemporal));
    DevHelp("Accumulates the reflection over time instead of showing each frame raw. "
            "Applies to BOTH sources: an SSR march is violently sensitive to its "
            "jittered start (measured 7.9x less frame-to-frame movement), and RT at "
            "half reflection res boils the same way once reflected foliage is "
            "subpixel -- 1 sharp ray/px under DLSS jitter. Unreal never display "
            "either unfiltered. Perf: ~0.014 ms. Off = the tracer's raw output, "
            "which is what you want when judging a tracer rather than the picture.");
    ImGui::BeginDisabled(!settings.ssrTemporal);
    {
        GRAPHICS_CONTROL_WIDTH(ReflectionTemporalBlend, "reflectionTemporalBlend", 98.0f,
            ImGui::SliderFloat("Temporal blend", &settings.ssrTemporalBlendWeight,
                               0.02f, 1.0f, "%.3f"));
        DevHelp("Weight of the CURRENT frame. UE use 1/8 = 0.125 for their SSR TAA "
                "config. Lower = longer history, steadier but slower to react; 1 = no "
                "accumulation at all.");
        GRAPHICS_CONTROL_WIDTH(ReflectionTemporalStillInertia,
            "reflectionTemporalStillInertia", 98.0f,
            ImGui::SliderFloat("Temporal still inertia",
                               &settings.ssrTemporalClampExpand, 0.0f, 2.0f, "%.2f"));
        DevHelp("Extra inertia for STILL pixels, on top of the blend above. At zero "
                "motion this knob both RELAXES the neighbourhood clamp (the measured "
                "limiter of still-camera boil) and divides the frame weight by up to "
                "(1 + 4x this). Measured on the bronze bench: 0 = baseline 0.46, "
                "0.5 = 0.37, 2.0 = 0.20 frame-to-frame boil. Any motion restores the "
                "hard clamp and full blend, so response while moving is unchanged. "
                "Cost at high values: the reflection of something moving in a STILL "
                "mirror (a swaying palm) can trail slightly. 0 = off.");
    }
    ImGui::EndDisabled();
    // S16: glossy reflections — blur radius scales with surface roughness (0 = sharp mirror).
    GRAPHICS_CONTROL(ReflectionGlossyScale, "reflectionGlossyScale",
        ImGui::SliderFloat("Glossy blur", &settings.reflectionGlossyScale,
                           0.0f, 24.0f, "%.1f"));
    DevHelp("SSR reflections: reflection pixels of blur added at roughness 1 (0 = sharp). RT "
            "reflections do not use it: each pixel's mirror reflection is spread over its GGX "
            "lobe as seen on the screen -- the hit distance against the surface's, the roughness, "
            "the projection (reflection_glossy_cs.hlsl). Headless: --set=reflections.glossyBlur:<px>; "
            "--set=reflections.lobe:0 puts this blur back on RT too (the A/B).");

    ImGui::SeparatorText("SSR");
    int ssrTechnique = static_cast<int>(settings.ssrTechnique);
    const int ssrTechniqueCount = static_cast<int>(SsrTechnique::Count);
    if (ssrTechnique < 0 || ssrTechnique >= ssrTechniqueCount)
    {
        ssrTechnique = 0;
    }
    if (GRAPHICS_CONTROL(SsrTechnique, "ssrTechnique",
        ImGui::Combo("SSR technique", &ssrTechnique,
                     kSsrTechniqueLabels, ssrTechniqueCount)))
    {
        settings.ssrTechnique = static_cast<SsrTechnique>(ssrTechnique);
    }
    DevHelp("How the reflection ray searches for its hit. Log March takes 128 growing "
            "view-space steps against full depth. UE SSR follows SSRTReflections.usf: "
            "8/12/16 fixed screen-space steps issued in Batch4 against the FURTHEST HZB "
            "at a mostly fixed mip, with roughness-aware High/Epic ray counts. It is "
            "usually cheaper but deliberately approximate. Only affects SSR, not RT.");

    if (settings.ssrTechnique == SsrTechnique::UeHzb)
    {
        UeSsrSettings& ue = settings.ssrUe;
        int preset = static_cast<int>(ue.preset);
        if (preset < 0 || preset >= static_cast<int>(UeSsrQualityPreset::Count))
        {
            preset = 0;
        }
        if (GRAPHICS_CONTROL(UeSsrQuality, "ueSsrQuality",
            ImGui::Combo("UE SSR quality", &preset, kUeSsrQualityLabels,
                         static_cast<int>(UeSsrQualityPreset::Count))))
        {
            ApplyUeSsrQualityPreset(ue, static_cast<UeSsrQualityPreset>(preset));
        }
        DevHelp("The actual SSRTReflections.usf presets. High/Epic use 4/12 GGX rays "
                "only at roughness >= 0.1; smoother surfaces collapse the whole budget "
                "to one 24-step mirror ray, exactly as UE do.");

        if (ImGui::TreeNode("UE SSR advanced"))
        {
            int steps = static_cast<int>(ue.numSteps);
            if (GRAPHICS_CONTROL(UeSsrSteps, "ueSsrSteps",
                                 ImGui::SliderInt("UE steps / ray", &steps, 4, 64)))
            {
                ue.numSteps = static_cast<uint32_t>(std::min(64, (steps + 3) & ~3));
                ue.preset = UeSsrQualityPreset::Custom;
            }
            DevHelp("Rounded up to a multiple of four because UE issue depth requests "
                    "in Batch4. More steps fill thin/far silhouettes and narrow the "
                    "per-step depth interval; cost is linear per ray.");

            int rays = static_cast<int>(ue.numRays);
            if (GRAPHICS_CONTROL(UeSsrRays, "ueSsrRays",
                                 ImGui::SliderInt("UE rays / pixel", &rays, 1, 12)))
            {
                ue.numRays = static_cast<uint32_t>(rays);
                ue.preset = UeSsrQualityPreset::Custom;
            }
            DevHelp("Used only with glossy rays. Cost is rays x steps; 12x12 is UE Epic "
                    "and is intentionally expensive on rough reflectors.");

            if (GRAPHICS_CONTROL(UeSsrGlossyRays, "ueSsrGlossyRays",
                ImGui::Checkbox("UE roughness GGX rays", &ue.glossyRays)))
            {
                ue.preset = UeSsrQualityPreset::Custom;
            }
            DevHelp("OFF traces one geometric mirror ray. ON importance-samples the "
                    "reflector roughness when Rays > 1; roughness < 0.1 still collapses "
                    "to UE's one 24-step mirror ray.");

            GRAPHICS_CONTROL(UeSsrUseSurfaceRoughness, "ueSsrUseSurfaceRoughness",
                ImGui::Checkbox("UE use surface roughness", &ue.useSurfaceRoughness));
            DevHelp("Normally reads packed roughness from GB0, matching UE. Disable to "
                    "force one value below -- useful for proving which quality branch "
                    "is active without editing a material.");
            ImGui::BeginDisabled(ue.useSurfaceRoughness);
            GRAPHICS_CONTROL(UeSsrRoughnessOverride, "ueSsrRoughnessOverride",
                ImGui::SliderFloat("UE roughness override", &ue.roughnessOverride,
                                   0.0f, 1.0f, "%.2f"));
            ImGui::EndDisabled();

            GRAPHICS_CONTROL(UeSsrIntensity, "ueSsrIntensity",
                ImGui::SliderFloat("UE intensity", &ue.intensity,
                                   0.0f, 1.0f, "%.2f"));
            DevHelp("SSRParams.r: ScreenSpaceReflectionIntensity / 100. UE stock 1.0. "
                    "Multiplies the whole SSR output after the roughness fade.");

            GRAPHICS_CONTROL(UeSsrMaxRoughness, "ueSsrMaxRoughness",
                ImGui::SliderFloat("UE max roughness", &ue.maxRoughness,
                                   0.01f, 1.0f, "%.2f"));
            DevHelp("ScreenSpaceReflectionMaxRoughness, UE stock 0.6: full SSR up to "
                    "half this roughness, faded to nothing at it. Below the High tier "
                    "UE double the fade slope, and so does this port.");
            ImGui::TreePop();
        }
    }

    ImGui::SeparatorText("RT");
    ImGui::BeginDisabled(!rtSupported);
    {
        static const char* kAlphaModes[] = { "Off (solid cards)",
                                             "First hit (holes)",
                                             "Full (exact)" };
        int alphaMode = static_cast<int>(std::min(settings.rtAlphaMode, 2u));
        if (GRAPHICS_CONTROL_WIDTH(RtAlphaMode, "rtAlphaMode", 126.0f,
                             ImGui::Combo("RT foliage alpha", &alphaMode, kAlphaModes, 3)))
        {
            settings.rtAlphaMode = static_cast<uint32_t>(alphaMode);
        }
        DevHelp("The REFLECTION rays' alpha test. Off: FORCE_OPAQUE traversal, leaves "
                "reflect as solid cards -- cheapest. First hit: same cheap traversal, "
                "but the committed hit is alpha-tested with the albedo sample shading "
                "fetches anyway; a transparent texel becomes a MISS, so crowns get "
                "their holes -- the holes show the sky fallback, not what is really "
                "behind. Costs the same as Off. Full: exact per-candidate testing "
                "during traversal -- the expensive one. Every other RT ray (the sky "
                "visibility, the GI, the shadow rays of what a reflection hits) sees "
                "the geometry, a leaf card solid, whatever this says. "
                "Headless: --set=rt.alphaMode:0/1/2.");
    }
    ImGui::BeginDisabled(settings.rtAlphaMode == 0u);
    GRAPHICS_CONTROL_WIDTH(RtAlphaMissKeep, "rtAlphaMissKeep", 98.0f,
        ImGui::SliderFloat("RT foliage fill", &settings.rtAlphaMissKeep,
                           0.0f, 1.0f, "%.2f"));
    DevHelp("Stochastic coverage inflation for the RT alpha test. One sharp ray per "
            "pixel at reflection res undersamples thin fronds, so the reflected crown "
            "reads smaller than the real one. On a failed alpha test the hit is still "
            "kept with this probability, re-rolled per pixel per frame -- the temporal "
            "resolve averages the dither back into density. 0 = honest cutouts, "
            "1 = the old solid cards. In FIRST-HIT mode it matters more: the "
            "single-layer test discards the crown's own depth, so raise it (0.3-0.5) "
            "to buy the density back. Headless: --set=rt.alphaMissKeep:<v>.");
    ImGui::EndDisabled();
    GRAPHICS_CONTROL(RtWindBlas, "rtWindBlas",
        ImGui::Checkbox("RT wind sway", &settings.rtWindBlas));
    DevHelp("Wind-deform the nearest casters' BLASes every frame so foliage sway "
            "reaches RT reflections. Off = reflections show the rest pose. "
            "Cost scales with the animated set (~0.3 ms for 24 palms). Headless: "
            "--set=rt.windBlas.");
    ImGui::BeginDisabled(!settings.rtWindBlas);
    GRAPHICS_CONTROL_WIDTH(RtWindBlasRadius, "rtWindBlasRadius", 98.0f,
        ImGui::SliderFloat("RT wind radius", &settings.rtWindBlasRadius,
                           0.0f, 100.0f, "%.0f m"));
    DevHelp("Casters inside this radius sway in RT (nearest-first, 24-slot cap); "
            "beyond it the shared rest-pose BLAS stands. Headless: "
            "--set=rt.windRadius.");
    ImGui::EndDisabled();
    ImGui::Checkbox("RT debug view -> Reflection target [F6]", &settings.rtDebugView);
    ImGui::EndDisabled();
    if (rtSupported && settings.rtDebugView)
    {
        ImGui::TextDisabled("Open the texture inspector [F4] and select 'Reflection'.");
    }

    ImGui::SeparatorText("Sun specular");
    // Analytic sun specular boost on metals: spec lobe *= (1 + metal*coef). 0 = physical.
    GRAPHICS_CONTROL(SunMetalSpecInfluence, "sunMetalSpecInfluence",
        ImGui::SliderFloat("Sun spec on metal", &settings.sunMetalSpecInfluence,
                           0.0f, 16.0f, "%.1f"));

    // The sun's angular size (disc AND the specular lobe floor) is the level's sun's:
    // Inspector > Directional Light > Sun Disc, UE's LightSourceAngle.

    ImGui::Separator();
    if (ImGui::Button("Reset reflections to defaults"))
    {
        graphicsSettings.ResetReflections(renderer, scene, settings);
    }
}

#endif // !TC_SHIPPING
