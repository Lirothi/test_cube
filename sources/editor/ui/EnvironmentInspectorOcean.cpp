#include "editor/ui/EnvironmentInspector.h"
#if WITH_EDITOR

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "core/JsonField.h" // R9.6: jsonfield::Value -- value()'s conversions, no throw outside the loader
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/scene/EnvironmentRuntime.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/commands/EditEnvironmentCommand.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/ui/EditorDragDrop.h"
#include "app/Systems.h"
#include "ocean/OceanRenderConfigJson.h"
#include "ocean/OceanRenderable.h" // ocean::g_shoreRunup_game gates the Shore and surf section
#include "ocean/OceanSimulation.h"
#include "ocean/OceanWakeKnobs.h"
#include "imgui.h"

// R10.3: the Inspector's ocean -- simulation, rendering, shore and surf, wake, swell (EnvironmentInspector.h).

void EnvironmentInspector::DrawOcean()
{
    OceanSimulation* ocean = Systems::GetOceanSimulation();
    if (!ocean)
    {
        ImGui::TextDisabled("No ocean in this level.");
        ImGui::TextDisabled("Use the Ocean menu to create one.");
    }
    else
    {
        // Preset (reference): changing it reloads the sim live.
        const std::string curPreset = jsonfield::Value(tgt(), "preset", std::string());
        const std::string curLabel = std::filesystem::path(curPreset).filename().string();
        if (ImGui::BeginCombo("Preset", curLabel.empty() ? "(none)" : curLabel.c_str()))
        {
            for (const std::string& pr : EnvironmentRuntime::OceanPresets())
            {
                const bool sel = (pr == curPreset);
                const std::string label = std::filesystem::path(pr).filename().string();
                if (ImGui::Selectable(label.c_str(), sel) && !sel)
                {
                    nlohmann::json after = withField("preset", pr);
                    executeChange(std::move(after), "Set Ocean Preset");
                }
            }
            ImGui::EndCombo();
        }

        OceanRenderConfig render = ocean->GetRenderConfig();
        const auto renderIt = tgt().find("render");
        if (renderIt != tgt().end() && renderIt->is_object())
        {
            OceanRenderConfigJson::ApplyOverrides(*renderIt, render);
        }

        const auto storeRender = [&]()
        {
            tgt()["render"] = OceanRenderConfigJson::ToJson(render);
        };
        const auto beginRenderContinuousEdit = [&]()
        {
            if (ImGui::IsItemActivated())
            {
                activeEditObject = env.id;
                propertiesBeforeEdit = props;
            }
        };
        const auto finishRenderContinuousEdit = [&](bool changed)
        {
            if (changed)
            {
                EnvironmentRuntime::Apply(ctx, env);
                ctx.document.SetDirty(true);
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                nlohmann::json before =
                    activeEditObject.value == env.id.value ?
                        std::move(propertiesBeforeEdit) :
                        props;
                commandStack.Execute(ctx, std::make_unique<EditEnvironmentCommand>(
                    env.id,
                    std::move(before),
                    props,
                    historyLabel));
                activeEditObject = EditorObjectId{};
            }
        };
        const auto renderDrag = [&](const char* label,
            float& value,
            float speed,
            float minimum,
            float maximum,
            const char* format = "%.3f")
        {
            const bool changed = ImGui::DragFloat(
                label, &value, speed, minimum, maximum, format);
            beginRenderContinuousEdit();
            if (changed)
            {
                value = std::clamp(value, minimum, maximum);
                storeRender();
            }
            finishRenderContinuousEdit(changed);
        };
        const auto renderColor = [&](const char* label, Math::float4& color)
        {
            float values[3] = { color.x, color.y, color.z };
            const bool changed = ImGui::ColorEdit3(
                label,
                values,
                ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            beginRenderContinuousEdit();
            if (changed)
            {
                color = Math::float4(values[0], values[1], values[2], color.w);
                storeRender();
            }
            finishRenderContinuousEdit(changed);
        };
        const auto renderVector4 = [&](const char* label,
            Math::float4& value,
            float speed,
            float minimum,
            float maximum)
        {
            float values[4] = { value.x, value.y, value.z, value.w };
            const bool changed = ImGui::DragFloat4(
                label, values, speed, minimum, maximum, "%.3f");
            beginRenderContinuousEdit();
            if (changed)
            {
                value = Math::float4(
                    std::clamp(values[0], minimum, maximum),
                    std::clamp(values[1], minimum, maximum),
                    std::clamp(values[2], minimum, maximum),
                    std::clamp(values[3], minimum, maximum));
                storeRender();
            }
            finishRenderContinuousEdit(changed);
        };

        ImGui::TextDisabled("Render settings are stored in this level and override the preset.");

        if (ImGui::CollapsingHeader("Surface color", ImGuiTreeNodeFlags_DefaultOpen))
        {
            renderColor("Deep Scatter Tint", render.deepScatterColor);
            renderColor("Subsurface Tint", render.sssColor);
            renderColor("Diffuse Tint", render.diffuseColor);
            renderDrag("Low-Sun Body Light", render.lowSunBodyLight, 0.005f, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How much of the sun still lights the water body with the sun on the horizon.\n"
                                  "Below 12 degrees the sun's share fades by what actually gets into the water;\n"
                                  "this is the floor of that fade. 1 = no fade. Above 12 degrees: no effect.");
        }

        if (ImGui::CollapsingHeader("Specular and reflection", ImGuiTreeNodeFlags_DefaultOpen))
        {
            renderDrag("Specular Strength", render.specularStrength, 0.01f, 0.0f, 10.0f);
            renderDrag("Roughness Scale", render.roughnessScale, 0.005f, 0.0f, 1.0f);
            renderDrag("Roughness Distance", render.roughnessDistance, 1.0f, 1.0f, 5000.0f, "%.1f");
            renderDrag("Reflection Normal Flattening", render.reflectionNormalStrength, 0.005f, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pulls the reflection normal toward +Y (1 = a flat mirror) for BOTH the sky\n"
                                  "sample and the screen-space trace. Higher values bring back reflections of a\n"
                                  "distant shore at grazing angles, but flatten the sky's ripples.");
            renderDrag("Sky Reflection Horizon Pull", render.reflectionSkyHorizonPull, 0.005f, 0.05f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Compresses the SKY reflection ray toward the horizon (1 = off).\n"
                                  "A clear-sky HDRI is much darker at the zenith, and a wave's facets\n"
                                  "swing the reflected ray between horizon and zenith - at grazing angles\n"
                                  "Fresnel passes that contrast through at full strength, so it reads as\n"
                                  "hard dark streaks along the crests. Lower values keep the water in the\n"
                                  "bright band near the horizon, which is also where real water reflects\n"
                                  "from at these angles. ~0.4-0.6 is the useful range. Affects the ocean's\n"
                                  "environment sample only: the screen-space reflection, land IBL and sky untouched.");
            renderDrag("Horizon Fog Strength", render.horizonFogStrength, 0.005f, 0.01f, 5.0f);
            renderDrag("Horizon Fog Distance Scale", render.horizonFogDistanceScale, 0.01f, 0.0f, 20.0f);
            renderDrag("Cascade Fade Scale", render.cascadeFadeScale, 0.1f, 0.0f, 1000.0f);
            renderDrag("Minimum Mesh Scale", render.minMeshScale, 0.1f, 0.001f, 1000.0f);
            renderDrag("Detail Normal Mip Bias", render.detailNormalMipBias, 0.05f, -4.0f, 4.0f);
            renderDrag("Macro Normal Mip Bias (DLSS)", render.macroNormalMipBiasDlss, 0.05f, -4.0f, 4.0f);
            renderDrag("Macro Normal Mip Bias (Native)", render.macroNormalMipBiasNative, 0.05f, -4.0f, 4.0f);
        }

        if (ImGui::CollapsingHeader("Refraction and volume"))
        {
            renderDrag("Surface Refraction", render.surfaceRefractionStrength, 0.005f, 0.0f, 5.0f);
            renderDrag("Underwater Refraction", render.underwaterRefractionStrength, 0.005f, 0.0f, 5.0f);
            renderDrag("Absorption Depth", render.absorptionDepthScale, 0.1f, 1.0f, 1000.0f);
            renderDrag("Fog Density", render.fogDensity, 0.005f, 0.0f, 10.0f);
        }

        if (ImGui::CollapsingHeader("Subsurface scattering"))
        {
            renderDrag("Sun Scatter", render.sunScatterStrength, 0.01f, 0.0f, 10.0f);
            renderDrag("Sky Scatter", render.skyScatterStrength, 0.01f, 0.0f, 10.0f);
            renderDrag("Scatter Spread", render.scatterSpread, 0.005f, 0.001f, 2.0f);
            renderDrag("View Alignment", render.viewAlignmentStrength, 0.005f, 0.0f, 1.0f);
            renderDrag("Height Bias", render.sssHeightBias, 0.01f, -10.0f, 10.0f);
            renderDrag("Distance Fade", render.sssFadeDistance, 0.1f, 0.0f, 1000.0f);
        }

        if (ImGui::CollapsingHeader("Wind shading"))
        {
            renderDrag("Wind Speed", render.windSpeed, 0.1f, 0.0f, 100.0f);
            renderDrag("Waves Scale", render.wavesScale, 0.01f, 0.0f, 10.0f);
            renderDrag("Wind Alignment", render.windAlignment, 0.005f, 0.0f, 1.0f);
            renderDrag("UV Warp Strength", render.windUvWarpStrength, 0.005f, 0.0f, 5.0f);
        }

        if (ImGui::CollapsingHeader("Boat wake", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // ocean/OceanWake: every boat's hull pressure moving over deep water; only while a boat is in the world.
            bool wakeEnabled = render.wakeEnabled;
            if (ImGui::Checkbox("Boat Wake Enabled", &wakeEnabled))
            {
                OceanRenderConfig afterRender = render;
                afterRender.wakeEnabled = wakeEnabled;
                nlohmann::json after = withField("render", OceanRenderConfigJson::ToJson(afterRender));
                executeChange(std::move(after), "Set Ocean Boat Wake Enabled");
            }
            const auto wakeTip = [](const char* text)
            {
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", text);
                }
            };
            renderDrag("Wake Strength", render.wakeStrength, 0.01f, 0.0f, 4.0f);
            wakeTip("The hull's pressure: 1 = the head of water its draft stands for (physics).");
            renderDrag("Wake Height", render.wakeDisplacement, 0.01f, 0.0f, 2.0f);
            wakeTip("The field's height in the water's vertices.");
            renderDrag("Wake Slope", render.wakeNormal, 0.01f, 0.0f, 3.0f);
            wakeTip("The field's slope in the water's normal: 1 = its true slope.");
            renderDrag("Wake Damping (1/s)", render.wakeDamping, 0.02f, 0.0f, 20.0f);
            wakeTip("At the field's shortest wave (2 texels), falling as k^2 to the longer ones.");
            renderDrag("Wake Breaking (1/s)", render.wakeBreaking, 0.05f, 0.0f, 20.0f);
            wakeTip("A long wave in the shallows (depth under a twentieth of its length) runs down at up to this rate: "
                    "the surf zone a beach takes it in (Crest's Attenuation In Shallows, 2/s). 0 = none: the "
                    "waterline reflects it whole.");
            renderDrag("Wake Texel (m)", render.wakeTexel, 0.01f, 0.25f, 2.0f);
            wakeTip("512 texels make the window round the camera: 0.5 m = 256 m. A change restarts the field.");
            // The foam and the bow wave's arms: one table for both windows, the level's file and `--set`
            // (ocean/OceanWakeKnobs.h).
            for (const ocean::WakeKnob& knob : ocean::WakeKnobs())
            {
                if (knob.section)
                {
                    ImGui::SeparatorText(knob.section);
                }
                renderDrag(knob.label, render.*knob.field, knob.speed, knob.min, knob.max);
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s\n\n--set ocean.%s:<%g .. %g>", knob.hint, knob.key, knob.min, knob.max);
                }
            }
        }

        if (ImGui::CollapsingHeader("Boat spray", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // vfx/VesselSpray: lit drops off a planing hull's chines, every boat's; the white water along its sides.
            bool sprayEnabled = render.sprayEnabled;
            if (ImGui::Checkbox("Boat Spray Enabled", &sprayEnabled))
            {
                OceanRenderConfig afterRender = render;
                afterRender.sprayEnabled = sprayEnabled;
                nlohmann::json after = withField("render", OceanRenderConfigJson::ToJson(afterRender));
                executeChange(std::move(after), "Set Ocean Boat Spray Enabled");
            }
            const auto sprayTip = [](const char* text)
            {
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", text);
                }
            };
            renderDrag("Spray Amount", render.sprayRate, 0.01f, 0.0f, 1.0f);
            sprayTip("A share of what the presets can keep alive, at full speed (from 12 m/s; none under 3).");
            renderDrag("Spray Mist", render.sprayMist, 0.01f, 0.0f, 4.0f);
            sprayTip("The mist's share of the amount: soft puffs that hang and drift on the wind.");
            renderDrag("Spray Outward", render.sprayOut, 0.005f, 0.0f, 2.0f);
            sprayTip("A drop's speed outward, a share of the hull's (the earth's frame).");
            renderDrag("Spray Upward", render.sprayUp, 0.005f, 0.0f, 2.0f);
            sprayTip("A drop's speed upward, a share of the hull's: how high the sheet flies.");
            renderDrag("Spray Forward", render.sprayForward, 0.005f, -1.0f, 1.0f);
            sprayTip("A drop's speed along the hull's way, a share of the hull's.");
            renderDrag("Spray Slam", render.spraySlam, 0.01f, 0.0f, 4.0f);
            sprayTip("The burst a slam into a wave throws (1 = the presets' numbers, 0 = none).");
            renderColor("Spray Colour", render.sprayColor);
            sprayTip("The drops' albedo under the sun and the sky's light (white water: a light grey, neutral).");
            renderDrag("Spray Foam Light", render.sprayFoamLight, 0.01f, 0.0f, 1.0f);
            sprayTip("1 = the drops are lit as a level sheet of the water's foam, the same white as the trail; 0 = as "
                     "spheres: their sides take a low sun, the sea under them takes their sky.");
            renderDrag("Spray Opacity", render.sprayOpacity, 0.01f, 0.0f, 3.0f);
            sprayTip("Times the drops' alpha (each at most 1): higher = less of the water shows through, whiter spray.");
            renderDrag("Spray Soft Fade", render.spraySoftFade, 0.01f, 0.0f, 2.0f);
            sprayTip("Metres over which a drop fades out before it meets the water's surface (the mist over 2.5x it): no hard line where a sprite crosses the sea. 0 = cut by the water.");
            renderDrag("Spray Forward Scatter", render.sprayForwardScatter, 0.005f, 0.0f, 1.0f);
            sprayTip("The sun's share in the forward lobe: spray against the sun glows, the rest of it is diffuse. Higher = darker spray with the sun behind the camera, brighter against it.");
        }

        if (ImGui::CollapsingHeader("Shore and surf", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // surf sim injection: mode-independent (docs/ocean_surf_sim_plan.md).
            {
                bool surfSimEnabled = render.surfSimEnabled;
                if (ImGui::Checkbox("Surf Sim Enabled", &surfSimEnabled))
                {
                    OceanRenderConfig afterRender = render;
                    afterRender.surfSimEnabled = surfSimEnabled;
                    nlohmann::json after = withField("render", OceanRenderConfigJson::ToJson(afterRender));
                    executeChange(std::move(after), "Set Ocean Surf Sim Enabled");
                }
                // Tooltip helper: annotates the LAST widget (the drag above it).
                auto tip = [](const char* text)
                {
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s", text);
                    }
                };
                // --- Spawner: where and what is born ---
                renderDrag("Surf Spawn Distance", render.surfSimSpawnDistance, 0.25f, 5.0f, 200.0f);
                tip("Metres seaward of the waterline where a wave segment is born.\n"
                    "Farther = longer travel, more refraction, more decay on the way in.");
                renderDrag("Surf Segment Length", render.surfSimSegmentLength, 0.25f, 4.0f, 120.0f);
                tip("Along-shore length of each spawned wavefront (m).\n"
                    "30 = local roller, 100+ = a long wall across the beach.");
                renderDrag("Surf Spawn Interval", render.surfSimSpawnInterval, 0.02f, 0.25f, 30.0f);
                tip("Seconds between spawns at FULL wind. The wave frequency lever.");
                renderDrag("Surf Wind Coupling", render.surfSimWindCoupling, 0.005f, 0.0f, 1.0f);
                tip("How much wind drives the spawner. 1 = calm is silent, amplitude and\n"
                    "cadence scale with wind; 0 = waves always come as authored.");
                renderDrag("Surf Min Spawn Depth (m)", render.surfSimMinSpawnDepth, 0.02f, 0.0f, 10.0f);
                tip("Bottom depth required under the WHOLE segment to be born.\n"
                    "Kills births in lagoons, on shoal banks and in channels.");
                // --- Wave shape: peak width and height IN THE SIM (the cap follows) ---
                renderDrag("Surf Wave Amplitude", render.surfSimWaveAmplitude, 0.005f, 0.0f, 2.0f);
                tip("Injected height budget (m). It is the time-INTEGRAL of the forcing,\n"
                    "not the visible peak - the actual crest is lower and depends on\n"
                    "Wave Sigma / Spawn Duration.");
                renderDrag("Surf Wave Sigma (m)", render.surfSimWaveSigma, 0.02f, 1.5f, 12.0f);
                tip("Across-shore half-width of the injected hump - THE wavelength lever.\n"
                    "Smaller = narrower, sharper, higher wave (same amplitude budget in\n"
                    "less width) and a narrower foam cap. Below ~1.5 m the 1 m sim grid\n"
                    "aliases.");
                renderDrag("Surf Spawn Duration (s)", render.surfSimSpawnDuration, 0.01f, 0.3f, 3.0f);
                tip("Seconds the hump inflates. Shorter = more compact, TALLER packet\n"
                    "(less smearing while it departs); the amplitude integral is kept.");
                renderDrag("Surf Bore Floor (m)", render.surfSimCelerityFloor, 0.005f, 0.1f, 1.0f);
                tip("Minimum depth used for wave speed (c = sqrt(g*depth)).\n"
                    "High = the front marches reliably to the waterline.\n"
                    "Low = waves slow and COMPRESS on the shallows (shorter and steeper\n"
                    "near shore) but spend longer dying in the strip.");
                renderDrag("Surf Wave Damping (1/s)", render.surfSimWaveDamping, 0.001f, 0.0f, 0.4f);
                tip("Open-water settle rate. Lower = waves survive the trip fuller;\n"
                    "higher = calmer field between waves.");
                // --- Breaking and foam ---
                renderDrag("Surf Breaker Gamma", render.surfSimBreakerGamma, 0.005f, 0.4f, 1.2f);
                tip("Surf breaker index H/d: the wave foams where its height exceeds\n"
                    "gamma * depth (McCowan ~0.78). Lower = breaks earlier and farther\n"
                    "out = wider foam band.");
                renderDrag("Surf Break Onset", render.surfSimBreakOnset, 0.005f, 0.1f, 0.9f);
                tip("Fraction of the breaker criterion where foam STARTS ramping in.\n"
                    "Lower = foam appears on the approach = wider cap;\n"
                    "higher = only right at the break = narrow cap.");
                renderDrag("Surf Deposit Strength", render.surfSimDepositStrength, 0.01f, 0.0f, 5.0f);
                tip("Peak foam a breaking crest stamps into the field\n"
                    "(max() semantics, like the FFT whitecaps).");
                renderDrag("Surf Foam Fade Time (s)", render.surfSimFoamFadeTime, 0.01f, 0.2f, 30.0f);
                tip("Seconds a full-strength foam stamp takes to dissolve\n"
                    "(linear decay behind the crest). The tail length lever.");
                // --- Foam look ---
                renderDrag("Surf Foam Coverage Multiplier", render.surfSimFoamCoverageMultiplier, 0.005f, 0.0f, 2.0f);
                tip("Multiplies final surf-sim foam coverage after breakup and window fade.\n"
                    "0 = hidden, 1 = unchanged, above 1 = stronger (clamped to full coverage).\n"
                    "Does not change foam generation, lifetime, contact foam or FFT whitecaps.");
                renderDrag("Surf Front Breakup", render.surfSimFrontBreakup, 0.005f, 0.0f, 1.0f);
                tip("Tear of the FRESH foam (values near the stamp peak). 0 = solid cap.");
                renderDrag("Surf Tail Breakup", render.surfSimTailBreakup, 0.005f, 0.0f, 2.0f);
                tip("Tear of the decayed tail. Above 1 the threshold outgrows the\n"
                    "pattern and even mid-fresh foam shreds.");
                renderDrag("Surf Cap Width", render.surfSimCapWidth, 0.005f, 0.25f, 4.0f);
                tip("Bends the value-age curve of the tear: > 1 keeps foam on the FRONT\n"
                    "amount longer = wider dense zone; < 1 hands it to the tail sooner\n"
                    "= narrower.");
                renderDrag("Surf Tear Scale (m)", render.surfSimTearScale, 0.05f, 1.0f, 50.0f);
                tip("Patch size of the drifting tear pattern (metres).");
                // --- Surface coupling ---
                renderDrag("Surf Wave Displacement", render.surfSimDisplacement, 0.005f, 0.0f, 2.0f);
                tip("How much of the sim wave height rides the water surface as vertex\n"
                    "displacement. 0 = foam only, the surface stays flat.");
                renderDrag("Surf Run Inland (m)", render.surfSimRunInland, 0.05f, 0.0f, 20.0f);
                tip("Metres past the SDF waterline the wave may live - the VISIBLE water\n"
                    "edge sits inland of the shore map's zero.");
            }
            // Mirrors OceanControlsWindow: the two surface variants read different
            // settings, so only the live ones are shown per mode.
            if (!ocean::g_shoreRunup_game)
            {
                ImGui::TextDisabled(
                    "Legacy ocean surface (relaunch with --ocean-runup-shore for the modern stack).");
                renderDrag("Vertical Damp Strength", render.shoreLegacyVerticalDampStrength, 0.005f, 0.0f, 1.0f);
                renderDrag("XZ Damp Strength", render.shoreLegacyXzDampStrength, 0.005f, 0.0f, 1.0f);
                renderDrag("Damp Fade Depth", render.shoreLegacyDampFadeDepth, 0.05f, 0.01f, 50.0f);
                // Shared with the modern surface: the same fields drive its normal fade.
                renderDrag("Normal Fade Depth", render.shoreNormalFadeDepth, 0.01f, 0.01f, 10.0f);
                renderVector4("Normal Cascade Minimums", render.shoreNormalMinWeights, 0.005f, 0.0f, 1.0f);
                renderDrag(
                    "Contact Foam Strength",
                    render.shoreLegacyContactFoamStrength,
                    0.002f,
                    0.0f,
                    1.0f);
                ImGui::SeparatorText("Contact Foam Tail");
                renderDrag("Tail Texture Scale", render.shoreLegacyTailTextureScale, 0.005f, 0.001f, 10.0f);
                renderDrag("Tail Depth", render.shoreLegacyTailDepth, 0.002f, 0.0f, 5.0f);
                renderDrag("Tail Scroll Speed", render.shoreLegacyTailScrollSpeed, 0.005f, 0.0f, 10.0f);
                renderDrag("Tail De-Tile", render.shoreLegacyTailDetile, 0.005f, 0.0f, 1.0f);
                renderDrag("Tail Contrast", render.shoreLegacyTailContrast, 0.005f, 0.0f, 4.0f);
                renderDrag("Tail Brightness Bias", render.shoreLegacyTailBias, 0.002f, -1.0f, 1.0f);
                renderDrag("Tail Edge Fade", render.shoreLegacyTailEdgeFade, 0.002f, 0.001f, 2.0f);
                ImGui::SeparatorText("Foam Dissipation");
                renderDrag("Dissipation Scale", render.shoreLegacyDissipationScale, 0.25f, 1.0f, 200.0f);
                renderDrag("Dissipation Speed", render.shoreLegacyDissipationSpeed, 0.005f, 0.0f, 5.0f);
                renderDrag("Dissipation Amount", render.shoreLegacyDissipationAmount, 0.002f, 0.0f, 1.0f);
                renderDrag("Dissipation Contrast", render.shoreLegacyDissipationContrast, 0.01f, 0.1f, 8.0f);
                renderDrag("Wind Thinning", render.shoreLegacyWindThinning, 0.002f, 0.0f, 1.0f);
                // Shared fields: the same soft-edge and shore-albedo knobs the modern surface uses.
                renderDrag("Refraction Soft Edge Distance", render.shoreEdgeSoftDepth, 0.001f, 0.0f, 0.25f);
                renderDrag("Albedo Scale", render.shoreContactFoamAlbedoScale, 0.01f, 0.001f, 10.0f);
                renderDrag("Albedo Scroll Speed", render.shoreContactFoamAlbedoScrollSpeed, 0.01f, 0.0f, 10.0f);

                if (ImGui::TreeNodeEx(
                    "Wet Sand",
                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
                {
                    renderDrag(
                        "Deposit Depth",
                        render.shoreWetnessDepositDepth,
                        0.005f,
                        0.001f,
                        2.0f);
                    renderDrag(
                        "Wet Time (s)",
                        render.shoreWetnessWetTime,
                        0.02f,
                        0.05f,
                        30.0f);
                    renderDrag(
                        "Dry Time (s)",
                        render.shoreWetnessDryTime,
                        0.1f,
                        0.05f,
                        300.0f);
                    renderDrag(
                        "Darkening",
                        render.shoreWetnessDarkening,
                        0.005f,
                        0.0f,
                        1.0f);
                    renderDrag(
                        "Film Reflection",
                        render.shoreWetnessReflectionStrength,
                        0.005f,
                        0.0f,
                        2.0f);
                    renderDrag(
                        "Water Edge Offset (m)",
                        render.shoreWetnessEdgeOffset,
                        0.01f,
                        0.0f,
                        20.0f);
                    renderDrag(
                        "Max Slope (deg)",
                        render.shoreWetnessMaxSlopeDegrees,
                        0.25f,
                        0.0f,
                        89.0f);
                    ImGui::SeparatorText("Distant Height Fallback");
                    renderDrag(
                        "Above Water (m)",
                        render.shoreWetnessFallbackAboveWater,
                        0.01f,
                        0.0f,
                        20.0f);
                    renderDrag(
                        "Below Water (m)",
                        render.shoreWetnessFallbackBelowWater,
                        0.02f,
                        0.0f,
                        50.0f);
                    renderDrag(
                        "Fade Start",
                        render.shoreWetnessFallbackFadeStartPercent,
                        0.25f,
                        0.0f,
                        100.0f,
                        "%.0f %%");
                    renderDrag(
                        "Breakup Strength",
                        render.shoreWetnessFallbackBreakupStrength,
                        0.005f,
                        0.0f,
                        1.0f);
                    renderDrag(
                        "Breakup Scale (m)",
                        render.shoreWetnessFallbackBreakupScale,
                        0.1f,
                        0.1f,
                        500.0f,
                        "%.1f");
                    ImGui::TreePop();
                }
            }
            else
            {
            renderDrag("Vertical Fade Depth", render.shoreVerticalFadeDepth, 0.01f, 0.01f, 10.0f);
            renderDrag("Shallow XZ Strength", render.shoreHorizontalMin, 0.005f, 0.0f, 1.0f);
            renderDrag("XZ Restore Depth", render.shoreHorizontalFadeDepth, 0.01f, 0.01f, 10.0f);
            renderDrag("Normal Fade Depth", render.shoreNormalFadeDepth, 0.01f, 0.01f, 10.0f);
            renderVector4("Normal Cascade Minimums", render.shoreNormalMinWeights, 0.005f, 0.0f, 1.0f);
            renderDrag("Run-up Depth", render.shoreRunupDepth, 0.01f, 0.01f, 10.0f);
            renderDrag("Run-up Strength", render.shoreRunupStrength, 0.01f, 0.0f, 10.0f);
            renderDrag("Run-up Max Wave", render.shoreRunupMaxWave, 0.01f, 0.0f, 10.0f);
            renderDrag("Run-up Slope Fade Start", render.shoreRunupSlopeStartDegrees, 0.25f, 0.0f, 89.0f);
            renderDrag("Run-up Slope Cutoff", render.shoreRunupSlopeEndDegrees, 0.25f, 0.0f, 89.0f);
            renderDrag("Swash Amplitude", render.shoreSwashAmplitude, 0.005f, 0.0f, 1.0f);
            renderDrag("Run-up Slope Smoothing", render.shoreRunupSlopeSmoothing, 0.05f, 0.5f, 8.0f);
            renderDrag("Bottom Clearance", render.shoreBottomClearance, 0.005f, 0.0f, 1.0f);
            renderDrag("Refraction Soft Edge Distance", render.shoreEdgeSoftDepth, 0.001f, 0.0f, 0.25f);
            renderDrag(
                "Geometry Edge Refraction Fade",
                render.shoreGeometryEdgeRefractionFadeDepth,
                0.005f,
                0.0f,
                2.0f);
            renderDrag(
                "Geometry Wave Fade Distance",
                render.shoreGeometryFadeDistance,
                1.0f,
                1.0f,
                2000.0f,
                "%.1f");

            if (ImGui::TreeNodeEx(
                "Wet Sand",
                ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
            {
                renderDrag(
                    "Deposit Depth",
                    render.shoreWetnessDepositDepth,
                    0.005f,
                    0.001f,
                    2.0f);
                renderDrag(
                    "Wet Time (s)",
                    render.shoreWetnessWetTime,
                    0.02f,
                    0.05f,
                    30.0f);
                renderDrag(
                    "Dry Time (s)",
                    render.shoreWetnessDryTime,
                    0.1f,
                    0.05f,
                    300.0f);
                renderDrag(
                    "Darkening",
                    render.shoreWetnessDarkening,
                    0.005f,
                    0.0f,
                    1.0f);
                renderDrag(
                    "Film Reflection",
                    render.shoreWetnessReflectionStrength,
                    0.005f,
                    0.0f,
                    2.0f);
                renderDrag(
                    "Max Slope (deg)",
                    render.shoreWetnessMaxSlopeDegrees,
                    0.25f,
                    0.0f,
                    89.0f);
                ImGui::SeparatorText("Distant Height Fallback");
                renderDrag(
                    "Above Water (m)",
                    render.shoreWetnessFallbackAboveWater,
                    0.01f,
                    0.0f,
                    20.0f);
                renderDrag(
                    "Below Water (m)",
                    render.shoreWetnessFallbackBelowWater,
                    0.02f,
                    0.0f,
                    50.0f);
                renderDrag(
                    "Fade Start",
                    render.shoreWetnessFallbackFadeStartPercent,
                    0.25f,
                    0.0f,
                    100.0f,
                    "%.0f %%");
                renderDrag(
                    "Breakup Strength",
                    render.shoreWetnessFallbackBreakupStrength,
                    0.005f,
                    0.0f,
                    1.0f);
                renderDrag(
                    "Breakup Scale (m)",
                    render.shoreWetnessFallbackBreakupScale,
                    0.1f,
                    0.1f,
                    500.0f,
                    "%.1f");
                ImGui::TreePop();
            }

            if (ImGui::TreeNodeEx(
                "Contact Foam",
                ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
            {
                ImGui::SeparatorText("Coverage");
                renderDrag("Main Width", render.shoreContactFoamMainWidth, 0.002f, 0.0f, 1.0f);
                renderDrag(
                    "Main Breakup Length",
                    render.shoreContactFoamBreakupLength,
                    0.001f,
                    0.0f,
                    1.0f);
                renderDrag(
                    "Breakup Length Variation",
                    render.shoreContactFoamBreakupLengthVariation,
                    0.002f,
                    0.0f,
                    1.0f);
                renderDrag(
                    "Breakup Variation Scale",
                    render.shoreContactFoamBreakupVariationScale,
                    0.002f,
                    0.001f,
                    1.0f);
                renderDrag("Opacity", render.shoreContactFoamOpacity, 0.005f, 0.0f, 1.0f);

                ImGui::SeparatorText("Wind Response");
                renderDrag(
                    "Calm Amount",
                    render.shoreContactFoamCalmAmount,
                    0.005f,
                    0.0f,
                    1.0f);
                renderDrag(
                    "Full At Wind Force",
                    render.shoreContactFoamFullWindForce,
                    0.005f,
                    0.01f,
                    1.0f);

                ImGui::SeparatorText("Breakup Pattern");
                renderDrag("Pattern Scale", render.shoreContactFoamPatternScale, 0.005f, 0.001f, 2.0f);
                renderDrag("Pattern Density", render.shoreContactFoamPatternDensity, 0.005f, 0.0f, 1.0f);
                renderDrag("Pattern Scroll Speed", render.shoreContactFoamPatternScrollSpeed, 0.01f, 0.0f, 10.0f);

                ImGui::SeparatorText("Signed Depth Warp");
                renderDrag(
                    "Depth Warp Scale",
                    render.shoreContactFoamDepthWarpScale,
                    0.002f,
                    0.001f,
                    2.0f);
                renderDrag(
                    "Depth Warp Strength",
                    render.shoreContactFoamDepthWarpStrength,
                    0.002f,
                    0.0f,
                    0.5f);
                renderDrag(
                    "Depth Warp Range",
                    render.shoreContactFoamDepthWarpRange,
                    0.005f,
                    0.0f,
                    2.0f);

                ImGui::SeparatorText("Appearance");
                renderDrag("Albedo Scale", render.shoreContactFoamAlbedoScale, 0.01f, 0.001f, 10.0f);
                renderDrag("Albedo Scroll Speed", render.shoreContactFoamAlbedoScrollSpeed, 0.01f, 0.0f, 10.0f);
                renderDrag(
                    "Normal Strength",
                    render.shoreContactFoamNormalStrength,
                    0.005f,
                    0.0f,
                    1.0f);
                ImGui::TreePop();
            }
            } // modern shore stack (ocean::g_shoreRunup_game)
        }

        if (ImGui::CollapsingHeader("Foam"))
        {
            renderColor("Foam Tint", render.foamTint);
            renderDrag("Foam Normal Strength", render.foamNormalStrength, 0.005f, 0.0f, 1.0f);
            renderDrag("Underwater Foam Parallax", render.underwaterFoamParallax, 0.01f, 0.0f, 10.0f);
        }

        if (ImGui::CollapsingHeader("Caustics"))
        {
            bool causticsEnabled = render.causticsEnabled;
            if (ImGui::Checkbox("Caustics Enabled", &causticsEnabled))
            {
                OceanRenderConfig afterRender = render;
                afterRender.causticsEnabled = causticsEnabled;
                nlohmann::json after = withField("render", OceanRenderConfigJson::ToJson(afterRender));
                executeChange(std::move(after), "Set Ocean Caustics Enabled");
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Sun caustics on everything below the water line. Applied in the\n"
                    "deferred lighting pass, so they follow the sun shadow.");
            }
            const auto selectCausticsTexture = [&](const std::string& path)
            {
                OceanRenderConfig afterRender = render;
                afterRender.causticsTexture = NormalizePath(path);
                nlohmann::json after = withField("render", OceanRenderConfigJson::ToJson(afterRender));
                executeChange(std::move(after), "Set Ocean Caustics Texture");
                render = afterRender;
            };
            if (ImGui::BeginCombo("Flipbook Asset", render.causticsTexture.c_str()))
            {
                for (const EditorAssetRecord& rec : registry.Assets())
                {
                    if (rec.id.type != EditorAssetType::Texture || !rec.texture.valid ||
                        rec.texture.kind != EditorTextureKind::Texture2D)
                        continue;
                    const bool selected = rec.id.key == render.causticsTexture;
                    if (ImGui::Selectable(rec.id.key.c_str(), selected) && !selected)
                        selectCausticsTexture(rec.id.key);
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(EditorDragDrop::kAssetPayloadType))
                {
                    EditorAssetId id;
                    if (EditorDragDrop::DecodeAssetPayload(payload, id))
                    {
                        const auto* rec = registry.FindById(id);
                        if (rec && rec->id.type == EditorAssetType::Texture && rec->texture.valid &&
                            rec->texture.kind == EditorTextureKind::Texture2D)
                            selectCausticsTexture(rec->id.key);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            char causticsPath[1024];
            std::snprintf(causticsPath, sizeof(causticsPath), "%s", render.causticsTexture.c_str());
            const bool pathChanged = ImGui::InputText("Flipbook Path", causticsPath, sizeof(causticsPath));
            beginRenderContinuousEdit();
            if (pathChanged)
            {
                render.causticsTexture = NormalizePath(causticsPath);
                storeRender();
                ctx.document.SetDirty(true);
            }
            // Commit once focus leaves the field, rather than uploading on each keystroke.
            finishRenderContinuousEdit(false);
            const auto causticsInt = [&](const char* label, int& value, int minimum, int maximum)
            {
                const bool edited = ImGui::InputInt(label, &value);
                beginRenderContinuousEdit();
                if (edited)
                {
                    value = std::clamp(value, minimum, maximum);
                    render.causticsFrameCount = std::clamp(render.causticsFrameCount, 0,
                        render.causticsColumns * render.causticsRows);
                    storeRender();
                }
                finishRenderContinuousEdit(edited);
            };
            causticsInt("Columns", render.causticsColumns, 1, 1024);
            causticsInt("Rows", render.causticsRows, 1, 1024);
            causticsInt("Frame Count (0 = all)", render.causticsFrameCount, 0,
                render.causticsColumns * render.causticsRows);
            ImGui::TextDisabled("Frames: left to right, then top to bottom.");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The image width/height must divide evenly by Columns/Rows.\n"
                                  "Both bundled caustics atlases use 4 columns and 4 rows.\n"
                                  "Texture selection keeps Dark Bias and the other lighting controls.");
            renderDrag("Intensity", render.causticsIntensity, 0.01f, 0.0f, 6.0f);
            renderDrag("Tile Size (m)", render.causticsScale, 0.05f, 0.25f, 60.0f);
            renderDrag("Speed (frames/s)", render.causticsSpeed, 0.1f, 0.0f, 60.0f);
            renderDrag("Depth Fade (m)", render.causticsDepthFade, 0.1f, 0.1f, 120.0f);
            renderDrag("Surface Fade (m)", render.causticsSurfaceFade, 0.01f, 0.0f, 5.0f);
            renderDrag("Up-Facing Gate", render.causticsUpFacing, 0.005f, 0.0f, 1.0f);
            renderDrag("Dark Bias", render.causticsBias, 0.005f, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Pattern value that means \"no gain\". The generator prints the value\n"
                    "unfocused sunlight encodes to; at that setting the dark cells stay\n"
                    "neutral and only the cords brighten.");
            }
            renderDrag("Dispersion (texels)", render.causticsDispersion, 0.01f, 0.0f, 8.0f);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Chromatic split of the cords. 0 is monochrome and 3x cheaper.");
            }
            renderDrag("De-Tile Layer", render.causticsLayerBlend, 0.005f, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Second layer at another scale, min-combined, to hide the tiling\n"
                    "period. 0 halves the texture taps.");
            }
            renderColor("Caustics Tint", render.causticsTint);
        }

        if (ImGui::CollapsingHeader("Absorption gradient"))
        {
            bool curvedGradient = render.absorptionGradientType >= 0.5f;
            if (ImGui::Checkbox("Curved Interpolation", &curvedGradient))
            {
                OceanRenderConfig afterRender = render;
                afterRender.absorptionGradientType = curvedGradient ? 1.0f : 0.0f;
                nlohmann::json after = withField("render", OceanRenderConfigJson::ToJson(afterRender));
                executeChange(std::move(after), "Set Ocean Absorption Gradient");
            }

            for (size_t index = 0; index < render.absorptionColors.size(); ++index)
            {
                ImGui::PushID(static_cast<int>(index));
                Math::float4& key = render.absorptionColors[index];
                renderColor("Color", key);
                renderDrag("Position", key.w, 0.005f, 0.0f, 1.0f);
                ImGui::Separator();
                ImGui::PopID();
            }

            if (render.absorptionColors.size() < 8u && ImGui::Button("Add Key"))
            {
                OceanRenderConfig afterRender = render;
                const Math::float4 last = afterRender.absorptionColors.empty()
                    ? Math::float4(1.0f, 1.0f, 1.0f, 1.0f)
                    : afterRender.absorptionColors.back();
                afterRender.absorptionColors.push_back(last);
                nlohmann::json after = withField("render", OceanRenderConfigJson::ToJson(afterRender));
                executeChange(std::move(after), "Add Ocean Absorption Key");
            }
            if (render.absorptionColors.size() > 1u)
            {
                ImGui::SameLine();
                if (ImGui::Button("Remove Key"))
                {
                    OceanRenderConfig afterRender = render;
                    afterRender.absorptionColors.pop_back();
                    nlohmann::json after = withField("render", OceanRenderConfigJson::ToJson(afterRender));
                    executeChange(std::move(after), "Remove Ocean Absorption Key");
                }
            }
        }

        ImGui::SeparatorText("Simulation overrides");
        float windForce = JsonFloat(tgt(), "windForce", ocean->GetWindForce01());
        const bool windForceChanged =
            ImGui::SliderFloat("Wind Force", &windForce, 0.0f, 1.0f);
        beginContinuousEdit(windForceChanged);
        if (windForceChanged) { tgt()["windForce"] = windForce; }
        trackContinuousEdit(windForceChanged);

        float windDirection = JsonFloat(
            tgt(),
            "windDirectionDeg",
            ocean->GetLocalWindDirectionDegrees());
        const bool windDirectionChanged = ImGui::DragFloat(
            "Wind Direction",
            &windDirection,
            0.5f,
            -360.0f,
            360.0f,
            "%.1f deg");
        beginContinuousEdit(windDirectionChanged);
        if (windDirectionChanged) { tgt()["windDirectionDeg"] = windDirection; }
        trackContinuousEdit(windDirectionChanged);

        float swellDirection = JsonFloat(
            tgt(),
            "swellDirectionDeg",
            ocean->GetSwellDirectionDegrees());
        const bool swellDirectionChanged = ImGui::DragFloat(
            "Swell Direction",
            &swellDirection,
            0.5f,
            -360.0f,
            360.0f,
            "%.1f deg");
        beginContinuousEdit(swellDirectionChanged);
        if (swellDirectionChanged) { tgt()["swellDirectionDeg"] = swellDirection; }
        trackContinuousEdit(swellDirectionChanged);
    }
}

#endif // WITH_EDITOR
