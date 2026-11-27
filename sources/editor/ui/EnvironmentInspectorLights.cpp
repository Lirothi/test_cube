#include "editor/ui/EnvironmentInspector.h"
#if WITH_EDITOR

#include <algorithm>
#include <string>

#include "core/JsonField.h" // R9.6: jsonfield::Value -- value()'s conversions, no throw outside the loader
#include "core/math/Math.h"
#include "rendering/shadows/ShadowSettings.h" // local-light plan S8: which bias the shadow fields drive
#include "imgui.h"

// R10.3: the Inspector's point and spot lights (EnvironmentInspector.h).

void EnvironmentInspector::DrawPointLight()
{
    colorEdit();
    dragF("Luminous Flux (lm)", "luminousFluxLm", 1000.0f, 1.0f, 0.0f, 200000.0f,
          "%.2f", ImGuiSliderFlags_Logarithmic); // hundredths: a candle is 0.5-4 lm
    dragF("Volumetric Intensity", "volumetricIntensity", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
    InspectorHelp("This light's weight in the volumetric fog (UE VolumetricScatteringIntensity): 0 = lights "
                  "surfaces only, no cone / halo in the air. Multiplied by the fog's Local Light Intensity.");
    InspectorHelp(
        "Luminous flux, in LUMENS -- the number on a light bulb's box. 800 = a 60 W "
        "incandescent, 1600 = 100 W, 3000-5000 a shop fitting, 10000-50000 a "
        "floodlight.\n\n"
        "It is converted to intensity as flux/(4*pi) and falls off as inverse-square, so "
        "what reaches a surface is LUX -- the same unit the sun's illuminance is in. That "
        "is what lets a lamp and daylight be compared instead of guessed at.\n\n"
        "The cone angle does NOT change a spot light's brightness: the same lumens give "
        "the same peak intensity as a point light would. Physically a reflector does "
        "concentrate flux, but tying brightness to the cone makes the two controls fight.\n\n"
        "IT REPLACED `intensity`, which had no unit and did not fall off physically -- the "
        "old curve was (1 - d/r)^2, a window, so a light's brightness moved whenever the "
        "radius was dragged. A level authored before this is migrated by matching the old "
        "brightness at HALF the range; nearer than that it is now brighter and further it "
        "falls off faster, which is the correction, not a regression.");
    dragF("Day Brightness", "dayBrightness", 1.0f, 1.0f, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    InspectorHelp("This light's own multiplier BY DAY, on its lumens: 1 = as authored, 0 = dark by day. Full "
                  "while the sun is up, it fades out in stops as the night comes in (the sun -6 -> -12 "
                  "degrees) and is gone at night. On an always-on lamp it multiplies with Time of Day's lamp "
                  "Day Brightness, which weighs every such lamp against the day.");
    dragF("Radius", "radius", 1.0f, 0.05f, 0.0f, 1000.0f);
    dragF3("Position", "position", Math::float3(0.0f, 0.0f, 0.0f), 0.05f);
    checkB("Cast Shadows", "shadowsEnabled", false);
    dragF("Shadow Bias", "shadowBias", 0.5f, 0.01f, 0.0f, 10.0f, "%.2f");
    dragF("Shadow Slope Bias", "shadowSlopeBias", 0.5f, 0.01f, 0.0f, 10.0f, "%.2f");
    InspectorHelp("UE's Shadow Bias / Shadow Slope Bias (0.5 each). With the texel bias (the default) a "
                  "surface looks its shadow up moved off itself by this light's own texels: 3 along its "
                  "geometric normal x Shadow Bias / 0.5, and 0.5 toward the light x Slope Bias / 0.5 x the "
                  "slope (1 / N.L, up to 6). Larger = no acne on rough or grazing ground, but the shadow "
                  "lifts off its caster's foot. With shadow.localBiasTexel:0 they are UE's own: the lookup "
                  "moves toward the light by 0.02 x 512/Res x 2 x Shadow Bias of the distance, times "
                  "1 + 3 x Slope Bias x (1 - N.L).");
    dragF("Source Radius", "sourceRadius", 0.0f, 0.002f, 0.0f, 10.0f, "%.3f");
    InspectorHelp("UE's Source Radius, in metres: how big the light is for VSM's rays, so how fast its penumbra widens with the "
                  "distance from the caster (a candle ~0.01, a bulb ~0.03). 0, UE's default, is a hard edge the rays' dither softens "
                  "by a pixel. Read by VSM alone: no other shadow mode has rays, and the shading has no area light.");
    dragF("Shadow Sharpen", "shadowSharpen", 0.0f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("UE's Shadow Sharpen: narrows the filtered penumbra -- the 29-tap disc's result is "
                  "stretched about its middle by 7 x this + 1. 0 = UE's soft edge.");
    onAtCombo();

    bool flickerEnabled = tgt().contains("flicker") && tgt()["flicker"].is_object();
    if (ImGui::Checkbox("Flicker", &flickerEnabled))
    {
        nlohmann::json after = props;
        if (flickerEnabled)
        {
            after["flicker"] = {
                { "amplitude", 0.35f },
                { "frequencyHz", 7.0f },
                { "seed", 3 }
            };
        }
        else
        {
            after.erase("flicker");
        }
        executeChange(std::move(after), "Toggle Point Light Flicker");
    }

    if (flickerEnabled)
    {
        ImGui::SeparatorText("Flicker");
        auto dragFlicker = [&](const char* label, const char* key,
            float def, float speed, float lo, float hi)
        {
            float value = JsonFloat(tgt()["flicker"], key, def);
            const bool changed = ImGui::DragFloat(label, &value, speed, lo, hi);
            beginContinuousEdit(changed);
            if (changed) { tgt()["flicker"][key] = value; }
            trackContinuousEdit(changed);
        };
        dragFlicker("Amplitude", "amplitude", 0.35f, 0.01f, 0.0f, 1.0f);
        dragFlicker("Frequency (Hz)", "frequencyHz", 7.0f, 0.1f, 0.0f, 60.0f);

        int seed = jsonfield::Value(tgt()["flicker"], "seed", 3);
        const bool seedChanged = ImGui::DragInt("Seed", &seed, 1.0f, 0, 1000000);
        beginContinuousEdit(seedChanged);
        if (seedChanged) { tgt()["flicker"]["seed"] = std::max(seed, 0); }
        trackContinuousEdit(seedChanged);
    }
}

void EnvironmentInspector::DrawSpotLight()
{
    colorEdit();
    dragF("Luminous Flux (lm)", "luminousFluxLm", 1000.0f, 1.0f, 0.0f, 200000.0f,
          "%.2f", ImGuiSliderFlags_Logarithmic); // hundredths: a candle is 0.5-4 lm
    dragF("Volumetric Intensity", "volumetricIntensity", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
    InspectorHelp("This light's weight in the volumetric fog (UE VolumetricScatteringIntensity): 0 = lights "
                  "surfaces only, no cone / halo in the air. Multiplied by the fog's Local Light Intensity.");
    InspectorHelp(
        "Luminous flux, in LUMENS -- the number on a light bulb's box. 800 = a 60 W "
        "incandescent, 1600 = 100 W, 3000-5000 a shop fitting, 10000-50000 a "
        "floodlight.\n\n"
        "It is converted to intensity as flux/(4*pi) and falls off as inverse-square, so "
        "what reaches a surface is LUX -- the same unit the sun's illuminance is in. That "
        "is what lets a lamp and daylight be compared instead of guessed at.\n\n"
        "The cone angle does NOT change a spot light's brightness: the same lumens give "
        "the same peak intensity as a point light would. Physically a reflector does "
        "concentrate flux, but tying brightness to the cone makes the two controls fight.\n\n"
        "IT REPLACED `intensity`, which had no unit and did not fall off physically -- the "
        "old curve was (1 - d/r)^2, a window, so a light's brightness moved whenever the "
        "radius was dragged. A level authored before this is migrated by matching the old "
        "brightness at HALF the range; nearer than that it is now brighter and further it "
        "falls off faster, which is the correction, not a regression.");
    dragF("Day Brightness", "dayBrightness", 1.0f, 1.0f, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    InspectorHelp("This light's own multiplier BY DAY, on its lumens: 1 = as authored, 0 = dark by day. Full "
                  "while the sun is up, it fades out in stops as the night comes in (the sun -6 -> -12 "
                  "degrees) and is gone at night. On an always-on lamp it multiplies with Time of Day's lamp "
                  "Day Brightness, which weighs every such lamp against the day.");
    dragF("Range", "range", 10.0f, 0.1f, 0.0f, 10000.0f);
    dragF("Inner Angle (deg)", "innerAngleDeg", 15.0f, 0.2f, 0.0f, 89.0f);
    dragF("Outer Angle (deg)", "outerAngleDeg", 25.0f, 0.2f, 0.0f, 89.0f);
    dragF3("Position", "position", Math::float3(0.0f, 0.0f, 0.0f), 0.05f);
    dragF3("Direction", "direction", Math::float3(0.0f, -1.0f, 0.0f), 0.01f);
    checkB("Cast Shadows", "shadowsEnabled", false);
    onAtCombo();
    dragF("Shadow Bias", "shadowBias", 0.5f, 0.01f, 0.0f, 10.0f, "%.2f");
    dragF("Shadow Slope Bias", "shadowSlopeBias", 0.5f, 0.01f, 0.0f, 10.0f, "%.2f");
    InspectorHelp("UE's Shadow Bias / Shadow Slope Bias (0.5 each). With the texel bias (the default) a "
                  "surface looks its shadow up moved off itself by this light's own texels: 3 along its "
                  "geometric normal x Shadow Bias / 0.5, and 0.5 toward the light x Slope Bias / 0.5 x the "
                  "slope (1 / N.L, up to 6). Larger = no acne, but the shadow lifts off its caster's foot. "
                  "With shadow.localBiasTexel:0 they are UE's own: the map is written pushed away from the "
                  "light by 15.36/Res metres x 2 x Shadow Bias, plus that x 3 x Slope Bias on steep "
                  "surfaces.");
    dragF("Source Radius", "sourceRadius", 0.0f, 0.002f, 0.0f, 10.0f, "%.3f");
    InspectorHelp("UE's Source Radius, in metres: how big the light is for VSM's rays, so how fast its penumbra widens with the "
                  "distance from the caster (a candle ~0.01, a bulb ~0.03). 0, UE's default, is a hard edge the rays' dither softens "
                  "by a pixel. Read by VSM alone: no other shadow mode has rays, and the shading has no area light.");
    dragF("Shadow Sharpen", "shadowSharpen", 0.0f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("UE's Shadow Sharpen: narrows the filtered penumbra -- the 6x6 tent's result is stretched "
                  "about its middle by 7 x this + 1. 0 = UE's soft edge.");
    // Plan S8: read only by UE's / the old bias -- the texel bias moves the receiver by texels, so with it on the
    // field is shown but moves nothing, and says so.
    ImGui::BeginDisabled(render::LocalShadowTexelBias());
    dragF("Shadow Normal Bias", "shadowNormalBias", 0.05f, 0.001f, 0.0f, 10.0f, "%.5f");
    ImGui::EndDisabled();
    InspectorHelp("Ours, on top of UE's bias: the receiver is looked up this many metres off its surface, "
                  "along the normal. Only with shadow.localBiasTexel:0 -- the texel bias (the default) "
                  "pushes by this light's texels instead (Shadow Bias above).");
    // Plan S4: absent = automatic, shown as the near the light then uses (render::SpotShadowNear).
    dragF("Shadow Near (m)", "shadowNear", render::SpotShadowNear(0.0f), 0.001f, render::kSpotShadowMinNear,
          5.0f, "%.3f");
    InspectorHelp("The shadow's near plane: a caster nearer the light than this is not in its map (a lamp's "
                  "own bulb or frame inside it does not shadow the light -- or switch that mesh's Cast Shadow "
                  "off). Unset, it is UE's 1 mm on the atlases' linear depth and 0.1 m under VSM / "
                  "shadow.spotLinearDepth:0, whose perspective depth loses precision far away when it is lower.");
}

#endif // WITH_EDITOR
