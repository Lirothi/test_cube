#include "editor/ui/EnvironmentInspector.h"
#if WITH_EDITOR

#include <algorithm>
#include <string>

#include "rendering/core/PhotographicSettingsJson.h" // E1: the curve plot parses as the runtime does
#include "core/JsonField.h" // R9.6: jsonfield::Value -- value()'s conversions, no throw outside the loader
#include "app/scene/Scene.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/commands/EditEnvironmentCommand.h"
#include "imgui.h"

// R10.3: the Inspector's photographic camera -- exposure and the colour pipeline, a look group each, drawn as
// their own object or a section of the folded Post Process (EnvironmentInspector.h).

void EnvironmentInspector::DrawCameraExposure()
{
    // P1: dormant. Everything here round-trips through the level and reaches
    // Scene::SetCameraExposure, but nothing reads it yet -- the metering passes are P2.
    checkB("Enabled", "enabled", false);
    InspectorHelp("Master switch for the whole photographic camera. While OFF the linear "
                  "exposure multiplier is exactly 1.0 AND no metering work is scheduled, so "
                  "the frame is bit-for-bit what it was before any of this existed - off is "
                  "a true no-op, not a neutral value being applied.");
    if (!jsonfield::Value(tgt(), "enabled", false))
    {
        ImGui::TextDisabled("Dormant: exposure multiplier is exactly 1.0.");
    }
    checkB("Auto Exposure", "autoExposure", true);
    InspectorHelp("ON meters the scene each frame and adapts towards what it finds. OFF "
                  "holds Manual EV100 exactly, which is what every A/B in this project uses "
                  "- a feature that changes average luminance moves the meter, and then "
                  "every pixel in the frame shifts including ones the feature never "
                  "touched.");

    const bool automatic = jsonfield::Value(tgt(), "autoExposure", true);
    if (automatic)
    {
        ImGui::SeparatorText("Target");
        dragF("Compensation (EV)", "compensationEv", 0.0f, 0.05f, -8.0f, 8.0f, "%.2f");
        InspectorHelp("Offsets whatever the meter decides. This is the artistic knob - "
                      "negative darkens the whole image, positive lifts it.\n\n"
                      "P16.13: AUTO MODE ONLY. Manual mode has its own field, so a trim you "
                      "dial here cannot follow you into a fixed-exposure shot and back.");
        ImGui::SeparatorText("Range (clamps on the metered result)");
        dragF("Min EV100", "minEv100", -6.0f, 0.1f, -16.0f, 20.0f, "%.2f");
        dragF("Max EV100", "maxEv100", 16.0f, 0.1f, -16.0f, 20.0f, "%.2f");
        InspectorHelp("A safety net on the ADAPTED value, not a look control. The default "
                      "span is 22 stops, which clamps essentially nothing - it is \"off\", "
                      "not \"tuned\". Narrowing it is how a scene stops the camera opening "
                      "all the way up in a dark frame.\n\n"
                      "It is deliberately NOT the way to keep different lighting conditions "
                      "apart: percentile metering is scale-invariant, so a correctly "
                      "metered night and a correctly metered noon land on the same target, "
                      "and clamping the result flattens rather than fixes that. The "
                      "compensation CURVE below is the real answer.");

        // Time-of-day plan E1.
        ImGui::SeparatorText("Compensation curve (by the light)");
        {
            // The rows are the document's own pairs, in its order; the runtime sorts them by EV
            // (PhotographicSettingsJson::ApplyOverrides), so a row dragged past its neighbour
            // still evaluates in order.
            // A COPY, read once: releasing a row's drag commits an EditEnvironmentCommand that
            // replaces the object's properties under this loop, and an iterator into them dangled --
            // the next row dereferenced it (nlohmann invalid_iterator, owner 2026-10-13). Writes go
            // through a fresh lookup each time, checked.
            const auto curveIt = tgt().find("exposureCurve");
            const bool hasCurve = curveIt != tgt().end() && curveIt->is_array();
            const nlohmann::json curveNow = hasCurve ? *curveIt : nlohmann::json::array();
            const std::size_t count = curveNow.size();
            const auto pointOf = [](const nlohmann::json& p, float& ev, float& comp)
            {
                const bool ok = p.is_array() && p.size() >= 2 && p[0].is_number() && p[1].is_number();
                ev = ok ? p[0].get<float>() : 0.0f;
                comp = ok ? p[1].get<float>() : 0.0f;
            };
            std::size_t removeAt = count;
            ImGui::PushID("exposureCurve");
            for (std::size_t i = 0; i < count; ++i)
            {
                float v[2] = { 0.0f, 0.0f };
                pointOf(curveNow[i], v[0], v[1]);
                ImGui::PushID(static_cast<int>(i));
                const bool changed = ImGui::DragFloat2("EV100 -> EV", v, 0.05f, -16.0f, 24.0f, "%.2f");
                beginContinuousEdit(changed);
                if (changed)
                {
                    const auto live = tgt().find("exposureCurve");
                    if (live != tgt().end() && live->is_array() && i < live->size())
                    {
                        (*live)[i] = nlohmann::json::array({ v[0], std::clamp(v[1], -10.0f, 10.0f) });
                    }
                }
                trackContinuousEdit(changed);
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) { removeAt = i; }
                ImGui::PopID();
            }
            if (count == 0u)
            {
                ImGui::TextDisabled("None: the meter's middle grey at any light.");
            }
            else
            {
                // The curve as the solve will run it -- parsed (sorted, clamped) by the same
                // ApplyOverrides and evaluated by the CPU twin of the shader's ExposureCurve.
                render::CameraExposureSettings parsed;
                render::PhotographicSettingsJson::ApplyOverrides(tgt(), parsed);
                constexpr int kSamples = 57;
                constexpr float kLoEv = -12.0f, kHiEv = 16.0f; // the night's section included (under ~0)
                float samples[kSamples];
                float lo = -1.0f, hi = 0.0f; // always show 0 and a stop below it
                for (int s = 0; s < kSamples; ++s)
                {
                    const float ev = kLoEv + (kHiEv - kLoEv) * static_cast<float>(s) / (kSamples - 1);
                    samples[s] = render::EvaluateExposureCurve(parsed, ev);
                    lo = std::min(lo, samples[s]);
                    hi = std::max(hi, samples[s]);
                }
                ImGui::PlotLines("##curvePlot", samples, kSamples, 0, "EV100 -12 .. 16", lo - 0.5f, hi + 0.5f,
                                 ImVec2(ImGui::GetContentRegionAvail().x, 50.0f));
            }
            if (removeAt < count)
            {
                nlohmann::json curve = curveNow;
                curve.erase(removeAt);
                executeChange(withField("exposureCurve", std::move(curve)), historyLabel);
            }
            else if (count < render::CameraExposureSettings::kExposureCurveMaxPoints &&
                     ImGui::SmallButton("Add point"))
            {
                // Two stops of light past the last point at its compensation: a flat start.
                nlohmann::json curve = curveNow;
                float ev = 0.0f, comp = 0.0f;
                if (!curve.empty()) { pointOf(curve.back(), ev, comp); ev += 2.0f; }
                curve.push_back(nlohmann::json::array({ ev, comp }));
                executeChange(withField("exposureCurve", std::move(curve)), historyLabel);
            }
            ImGui::PopID();

            ImGui::PushID("curvePresets");
            if (ImGui::SmallButton("None"))
            {
                executeChange(withField("exposureCurve", nlohmann::json::array()), historyLabel);
            }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("No curve: every level renders as it did before E1."); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Eye"))
            {
                render::CameraExposureSettings eye;
                render::SetEyeExposureCurve(eye);
                nlohmann::json curve = nlohmann::json::array();
                for (std::uint32_t i = 0; i < eye.exposureCurvePoints; ++i)
                {
                    curve.push_back(nlohmann::json::array({ eye.exposureCurveEv[i], eye.exposureCurveComp[i] }));
                }
                executeChange(withField("exposureCurve", std::move(curve)), historyLabel);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("What a level with a time of day wants: a day untouched, the end of "
                                  "twilight 1.3 stops down, a full-moon night about 3.6, a moonless one 5 "
                                  "(with Min EV100 low enough for the eye to reach it, about -11).");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("UDS"))
            {
                render::CameraExposureSettings uds;
                render::SetUdsExposureCurve(uds);
                nlohmann::json curve = nlohmann::json::array();
                for (std::uint32_t i = 0; i < uds.exposureCurvePoints; ++i)
                {
                    curve.push_back(nlohmann::json::array({ uds.exposureCurveEv[i], uds.exposureCurveComp[i] }));
                }
                // UDS's Exposure Bias is 0: the curve and the compensation together, one undo entry.
                nlohmann::json after = withField("exposureCurve", std::move(curve));
                after["compensationEv"] = 0.0f;
                executeChange(std::move(after), historyLabel);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Ultra Dynamic Sky's curve one to one (its Exposure_Compensation_Curve's five cubic "
                                  "keys, +14.55 EV for its 5-lux sun, eight points within 0.015 EV) and its "
                                  "Exposure Bias 0 -- for UDS's moon (Time of Day > Night Brightness ~11400: the "
                                  "moon 1/33 of the sun). Below EV 5.3 a flat -1.5: the steady night camera follows "
                                  "the night's light in full. With the physical moon (Night Brightness 1) it leaves "
                                  "the night far too dark -- that one wants Eye.");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("UDS Steady Night"))
            {
                render::CameraExposureSettings steady;
                render::SetUdsSteadyNightExposureCurve(steady);
                nlohmann::json curve = nlohmann::json::array();
                for (std::uint32_t i = 0; i < steady.exposureCurvePoints; ++i)
                {
                    curve.push_back(nlohmann::json::array({ steady.exposureCurveEv[i], steady.exposureCurveComp[i] }));
                }
                executeChange(withField("exposureCurve", std::move(curve)), historyLabel);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("UDS's curve with a night that holds: the same from EV 10.55 up (the day, the "
                                  "bright twilight), steeper below, so at night the camera follows only 40 %% "
                                  "of the scene's darkening (UDS: 80 %%). Among dark palms the sky through the "
                                  "leaves no longer lights up: the camera opened 1.3 stops there, now 0.6. "
                                  "The open night comes out ~0.6 stops darker -- Time of Day > Night "
                                  "Exposure to taste. Under 0 the same night section as UDS: a quarter or "
                                  "a low moon darker than the full one, not exposed up to a dim day.");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("UE"))
            {
                render::CameraExposureSettings ue;
                render::SetUeExposureCurve(ue);
                nlohmann::json curve = nlohmann::json::array();
                for (std::uint32_t i = 0; i < ue.exposureCurvePoints; ++i)
                {
                    curve.push_back(nlohmann::json::array({ ue.exposureCurveEv[i], ue.exposureCurveComp[i] }));
                }
                executeChange(withField("exposureCurve", std::move(curve)), historyLabel);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Unreal's: no compensation curve, one constant Exposure Compensation at every "
                                  "EV -- +0.5, what the owner's UE wind_test PPV carries (UE's project default "
                                  "is 1.0). The night is followed in full like a day; for UE's whole picture "
                                  "also set Time of Day > Night Camera EV100 to 0 (UE has no steady night).");
            }
            ImGui::PopID();
        }
        InspectorHelp("Extra compensation, in EV, by how much light the eye is adapted to -- the "
                      "scene's EV100 in this renderer's numbers (log2 cd/m2 + 3; Live below shows it "
                      "as Eye): wind_test's beach meters 15 at noon, 2.5 at the end of civil twilight, "
                      "-3.5 under a full moon. NEGATIVE DARKENS, like the "
                      "Compensation above, and adds to it.\n\n"
                      "Why: percentile metering lands any correctly lit scene on the same middle "
                      "grey, so under auto exposure a moonlit beach reads as a dim day. The eye "
                      "adapts only part of the way and a night stays dark; this curve is the rest "
                      "(UE's Exposure Compensation Curve). Piecewise linear through up to eight "
                      "points, flat past its ends. It follows the eye's SMOOTHED adaptation, so a "
                      "dark room darkens the picture as the eye adapts rather than on one frame. "
                      "Min/Max EV100 still bound the camera. Night vision reads the eye's "
                      "adaptation, not the darkened camera. The SLOPE is the adaptation's strength: where the "
                      "compensation rises s stops a stop, the camera follows (1 - s) of the scene's change.\n\n"
                      "THE NIGHT'S SECTION: with a time of day and its Night Camera EV100, the open night's "
                      "camera reads this curve at the night's PHYSICAL light (middle grey under the moon, the "
                      "twilight and the stars, before Night Brightness): -3.3 a full moon at 45 degrees (the "
                      "Night Camera EV100 itself), -6.9 a quarter moon 22 degrees up, -11 no moon. Points there "
                      "set how much darker a darker night looks; the day's EVs (a dim room meters 4-8) are "
                      "not touched by them. Flat there = the camera follows the night's light in full. "
                      "--set=exposure.curve:<0 none|1 eye|2 uds|3 uds steady night>");
        ImGui::SeparatorText("Metering (which pixels set the exposure)");
        dragF("Low Percentile", "lowPercentile", 0.15f, 0.005f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("The histogram window the meter averages between. Widening it makes "
                      "exposure react to more of the frame; narrowing it makes the meter "
                      "pickier and steadier.");
        InspectorHelp("Measured on the canonical views: 0.15 is what the scene was "
                      "flown at and preferred; about 0.30 lands nearest the reference "
                      "photograph if a little more contrast is wanted. Raise it toward 0.5 "
                      "ONLY if shaded interiors read over-exposed and the weight mask below "
                      "is already doing its job - the two solve different problems and "
                      "stacking them over-corrects.");
        dragF("High Percentile", "highPercentile", 0.80f, 0.005f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("The top of that window. Lowering it makes the meter ignore more of "
                      "the bright end, which is the blunt version of what the mask below "
                      "does selectively: it discards bright samples EVERYWHERE, including "
                      "ones that are the subject.");
        ImGui::SeparatorText("Weight mask (centre-weighted metering)");
        dragF("Meter Mask Strength", "meterMaskStrength", 0.7f, 0.01f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("How strongly the frame edges are discounted. 0 meters the whole "
                      "frame evenly, which lets a bright sky at the top drag the whole "
                      "image dark.");
        dragF("Meter Mask Inner", "meterMaskInnerRadius", 0.35f, 0.01f, 0.0f, 2.0f, "%.3f");
        dragF("Meter Mask Outer", "meterMaskOuterRadius", 1.0f, 0.01f, 0.0f, 2.0f, "%.3f");
        InspectorHelp("Where the falloff runs, as fractions of the HALF-DIAGONAL: 1.0 is "
                      "the frame corner. Inside the inner radius a sample counts fully, "
                      "past the outer one it counts least. The shader floors the weight at "
                      "0.05 (as UE does), so the edges always contribute a little - a "
                      "subject filling the border must still be metered, not dropped.");
        dragF("Meter Mask Sky Bias", "meterMaskSkyBias", 0.6f, 0.01f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("Extra discount applied to the upper part of the frame, on top of the "
                      "radial mask. This is the sky-specific version of the same problem.");
        ImGui::SeparatorText("Adaptation (how fast it moves)");
        dragF("Speed Up (stops/s)", "speedUp", 3.0f, 0.05f, 0.0f, 20.0f, "%.2f");
        InspectorHelp("Rate towards a BRIGHTER target (stepping into sun). Faster than "
                      "the down rate on purpose - that asymmetry is how a real eye behaves.");
        dragF("Speed Down (stops/s)", "speedDown", 1.0f, 0.05f, 0.0f, 20.0f, "%.2f");
        InspectorHelp("Rate towards a DARKER target (stepping into shade).");
        dragF("Ease-in Distance", "adaptationStartDistance", 1.5f, 0.05f, 0.05f, 20.0f, "%.2f");
        InspectorHelp("Distance from the target, IN STOPS, where adaptation stops running "
                      "at a constant rate and starts easing in. Far away it holds the "
                      "stops/second above, so a big transition is time-bounded; inside this "
                      "distance the last fraction of a stop decelerates instead of arriving "
                      "at full speed and stopping dead. UE's "
                      "r.EyeAdaptation.ExponentialTransitionDistance, same default of 1.5.");
        dragF("Black Bucket Influence", "blackBucketInfluence", 1.0f, 0.01f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("Weight of the DARKEST histogram bucket. 1 counts it normally. Lower "
                      "it when a scene has large regions of pure black - letterboxing, an "
                      "unlit interior - which would otherwise drag the meter down and open "
                      "the camera on nothing. UE call it "
                      "r.EyeAdaptation.BlackHistogramBucketInfluence.");
    }
    else
    {
        // P16.6: the camera, as three settings a photographer knows. EV100 is DERIVED and
        // is shown read-only below -- there is no second control for the same number.
        dragF("Aperture f/", "apertureFStop", 16.0f, 0.05f, 1.0f, 32.0f, "%.1f");
        InspectorHelp(
            "The iris, as an f-number. Every whole stop is a factor of two in light: "
            "f/1.4, 2, 2.8, 4, 5.6, 8, 11, 16, 22.");
        dragF("Shutter (s)", "shutterSpeedSec", 1.0f / 125.0f, 1.0f, 1.0f / 8000.0f, 30.0f,
              "%.5f", ImGuiSliderFlags_Logarithmic);
        InspectorHelp(
            "Exposure time in SECONDS: 1/125 is 0.008, 1/500 is 0.002. It does not blur "
            "anything here -- the renderer has no shutter -- it only meters light.");
        dragF("ISO", "isoSensitivity", 100.0f, 1.0f, 25.0f, 6400.0f, "%.0f",
              ImGuiSliderFlags_Logarithmic);
        InspectorHelp("Sensor sensitivity. 100 is the reference; doubling it is one stop.");
        dragF("Exposure Compensation (EV)", "manualCompensationEv", 0.0f, 0.02f, -5.0f, 5.0f,
              "%+.2f");
        InspectorHelp(
            "A quick +/- on top of the three settings above. POSITIVE BRIGHTENS.\n\n"
            "It is the trim you reach for while looking at the frame, so you do not have to "
            "re-solve the shutter in your head to make a scene half a stop warmer.\n\n"
            "P16.13: THIS IS MANUAL MODE'S OWN FIELD (`manualCompensationEv`). Auto mode "
            "has a separate one, because they are different jobs -- auto offsets what the "
            "METER decided, this offsets a number you solved by hand -- and while they "
            "shared a field, trimming one mode silently re-trimmed the other and back "
            "again. Switching modes no longer carries a trim across.");
        InspectorHelp("Held exactly, with no metering at all.\n\n"
                      "THE DEFAULTS ARE SUNNY-16 -- f/16, 1/125, ISO 100, EV 14.97 -- and since P16 the lights are in real lux, so an outdoor scene is correctly exposed with nobody typing anything.\n\n"
                      "EV100 = log2(N^2/t) - log2(ISO/100). It is shown below, read-only: it is derived from these three and having it as a fourth control would be two names for one number.\n\n"
                      "A level authored before P16.6 carried a hand-solved `manualEv100`; it is migrated by holding the shutter and ISO and solving the aperture, so the frame does not move.");
    }

    // Plan section 6.2 wants both representations visible, because EV is the authored
    // quantity but the linear multiplier is what a shader bug would show up in.
    ImGui::Separator();
    // P16.13: in manual the readout is the EV that ACTUALLY RUNS, i.e. the solved camera EV
    // minus the manual trim -- the same arithmetic exposure_solve_cs does. Showing the
    // untrimmed value would have this row disagree with the image as soon as the trim moved.
    const float shownEv = automatic
        ? JsonFloat(tgt(), "compensationEv", 0.0f)
        : (render::Ev100FromCamera(JsonFloat(tgt(), "apertureFStop", 16.0f),
                                   JsonFloat(tgt(), "shutterSpeedSec", 1.0f / 125.0f),
                                   JsonFloat(tgt(), "isoSensitivity", 100.0f)) -
           JsonFloat(tgt(), "manualCompensationEv", 0.0f));
    const float multiplier = jsonfield::Value(tgt(), "enabled", false)
        ? render::ExposureMultiplierFromEv100(shownEv)
        : render::kIdentityExposureMultiplier;
    ImGui::TextDisabled(automatic
        ? "Compensation %.2f EV -> x%.5f"
        : "Manual %.2f EV100 -> x%.5f",
        shownEv, multiplier);

    ImGui::SeparatorText("Local exposure");
    dragF("Local Highlights", "localHighlightContrast", 1.0f, 0.01f, 0.1f, 2.0f, "%.3f");
    InspectorHelp("Contrast scale for everything brighter than middle grey, judged by the "
                  "BLURRED neighbourhood rather than the pixel. 1 = off.\n\n"
                  "Below 1 COMPRESSES -- bright regions come down while their detail stays "
                  "(measured on sun_glint: 0.55 cut clipping 65x for a 2% median move). "
                  "Above 1 EXPANDS, which on wind_test is the more useful direction: this "
                  "scene's problem is too little range, not too much. Either way it is the "
                  "one thing a global exposure cannot do.");
    dragF("Local Shadows", "localShadowContrast", 1.0f, 0.01f, 0.1f, 2.0f, "%.3f");
    InspectorHelp("Same for everything darker than middle grey. Below 1 lifts shaded "
                  "regions without touching the lit ones; above 1 deepens them, and being "
                  "per-neighbourhood it deepens WITHOUT crushing the lit side.");
    dragF("Local Detail", "localDetailStrength", 1.0f, 0.01f, 0.0f, 3.0f, "%.3f");
    InspectorHelp("How much per-pixel detail survives the scaling. 1 = all of it, which is "
                  "the point: scaling the blurred base while passing detail through "
                  "untouched is what keeps micro-contrast.");
    dragF("Local HL Threshold", "localHighlightThreshold", 0.0f, 0.05f, 0.0f, 8.0f, "%.2f");
    InspectorHelp("Stops ABOVE middle grey before the highlight scaling starts. 0 applies "
                  "it everywhere brighter than grey.");
    dragF("Local SH Threshold", "localShadowThreshold", 0.0f, 0.05f, 0.0f, 8.0f, "%.2f");
    InspectorHelp("Stops away from middle grey before the effect starts, so mid-tones -- "
                  "usually the subject -- are left alone.");
    dragF("Local Blur Blend", "localBlurredBlend", 0.6f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("What the operator judges a neighbourhood by (UE Blurred Luminance "
                  "Blend, default 0.6). 0 = the bilateral grid only: each pixel's "
                  "neighbourhood is the nearby pixels about AS BRIGHT as itself, so a "
                  "bright sky does not lift the base of the water just under the horizon "
                  "(the dark band). 1 = a plain blur only, which bleeds across every "
                  "high-contrast edge -- what shipped before the grid.");
    dragF("Local Blur Size %", "localBlurredKernelPercent", 50.0f, 0.5f, 0.0f, 100.0f, "%.1f %%");
    InspectorHelp("The width of that blur as a percent of the view (UE Blurred Luminance "
                  "Kernel Size Percent, default 50). Narrow makes the operator more local; "
                  "it also lets one tiny, very bright source -- the sun's disc -- lift the "
                  "neighbourhood round itself, and the highlight compression then draws a "
                  "dark ring round it (seen at the ~2 % this used to be).");
    {
        // Mirrors the dev window's local-exposure presets exactly, including "Expand",
        // which is the one measured against docs/ref/ref_wind_test.png.
        struct LocalPreset { const char* name; float hl, sh, detail, hlT, shT; const char* tip; };
        static const LocalPreset kLocal[] = {
            { "Off", 1.00f, 1.00f, 1.00f, 0.00f, 0.00f,
              "Neutral -- a true no-op, the shader skips the block entirely." },
            { "Gentle", 0.85f, 0.90f, 1.00f, 0.50f, 0.50f,
              "Mild COMPRESSION. Reach for it when a frame clips, e.g. into the sun over water." },
            { "Strong", 0.65f, 0.75f, 1.10f, 0.25f, 0.25f,
              "Heavy compression. Check the horizon for halos." },
            { "Expand", 1.35f, 1.35f, 1.00f, 0.00f, 0.00f,
              "The measured match to the reference: p99/p02 spread 23.6x -> 41.4x on the "
              "overview view with clipping still at 0.000%. 1.5 on both reaches 52.9x "
              "against the reference's 53.6x. Median drops a little -- about +0.1 EV back." },
        };
        ImGui::PushID("localPresets");
        for (int i = 0; i < static_cast<int>(std::size(kLocal)); ++i)
        {
            const LocalPreset& preset = kLocal[i];
            if (i != 0) { ImGui::SameLine(); }
            if (ImGui::SmallButton(preset.name))
            {
                nlohmann::json after = withField("localHighlightContrast", preset.hl);
                after["localShadowContrast"] = preset.sh;
                after["localDetailStrength"] = preset.detail;
                after["localHighlightThreshold"] = preset.hlT;
                after["localShadowThreshold"] = preset.shT;
                executeChange(std::move(after), historyLabel);
            }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", preset.tip); }
        }
        ImGui::PopID();
    }

    // Time-of-day plan E2.
    ImGui::SeparatorText("Night vision");
    dragF("Night Vision", "nightVision", 1.0f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("The eye's rods taking over in the dark (the Purkinje shift): a cold cast falls "
                  "over the scene and its colours weaken -- why a moonlit night looks COLD although "
                  "moonlight is warmer than sunlight. How much follows the light by itself: none "
                  "above ~3 cd/m2 (a day and its shadows are untouched), full below ~0.005, per "
                  "pixel against what the eye is adapted to, so the moon's disc and a lamp keep "
                  "their colour. 1 = that estimate, 0 = a camera's colour vision at any light. "
                  "Colour only -- how DARK the night reads is the compensation curve's. "
                  "--set=exposure.nightVision:<0..1>");
    {
        const render::CameraExposureSettings defaults{};
        const Math::float3 t = JsonFloat3(tgt(), "nightTint",
            Math::float3(defaults.nightTint[0], defaults.nightTint[1], defaults.nightTint[2]));
        float tv[3] = { t.x, t.y, t.z };
        const bool changed = ImGui::ColorEdit3("Night Tint", tv);
        beginContinuousEdit(changed);
        if (changed) { tgt()["nightTint"] = { tv[0], tv[1], tv[2] }; }
        trackContinuousEdit(changed);
        InspectorHelp("The cold cast of night vision. Only its HUE matters -- it is applied at unit "
                      "luminance, so it never brightens or darkens. The default is the rods' "
                      "blue-cyan, mild: the cold LIGHT itself is the moon's (Time of Day > Moon > "
                      "Light Colour). White = no cast.");
        dragF("Night Desaturation", "nightDesaturation", defaults.nightDesaturation, 0.01f, 0.0f, 1.0f, "%.2f");
        InspectorHelp("How much colour the rods take at full night vision. 0 = the cold tint alone, "
                      "every hue kept (UDS's look: green palms stay green under blue light); 1 = "
                      "the rods' colourless grey, the physiology taken literally -- which renders a "
                      "moonlit night black and white. --set=exposure.nightDesaturation:<0..1>");
    }

    // Live metering state and the histogram, the same readouts the developer window shows.
    // They belong here too: the percentile and mask sliders above are unreadable without
    // seeing the distribution they are clipping.
    // Game thread plan G8b: the hand-over's copy -- read with no wait for the render thread's frame.
    const Renderer::ExposureView& metering = ctx.renderer.ExposureForGame();
    const ExposureMetering::Readback& readback = metering.readback;
    ImGui::SeparatorText("Live");
    if (readback.valid)
    {
        ImGui::Text("Adapted %+.3f EV100 (x%.5f)", readback.adaptedEv100,
            render::ExposureMultiplierFromEv100(readback.adaptedEv100));
        ImGui::Text("Target  %+.3f EV100", readback.targetEv100);
        if (automatic)
        {
            // E1: the curve's share is the solve's whole compensation less the static trim.
            ImGui::TextDisabled("Eye %+.3f EV100, curve %+.2f EV",
                readback.eyeEv100 + JsonFloat(tgt(), "compensationEv", 0.0f),
                readback.compensationEv - JsonFloat(tgt(), "compensationEv", 0.0f));
        }
        ImGui::TextDisabled("Metered low %.5f  high %.5f (scene linear)",
            readback.lowLuminance, readback.highLuminance);
        const float gap = readback.targetEv100 - readback.adaptedEv100;
        ImGui::TextDisabled(fabsf(gap) < 0.01f ? "settled" : "adapting (%+.2f EV to go)", gap);
    }
    else
    {
        ImGui::TextDisabled("waiting for the first readback (enable the camera above)");
    }
    if (ImGui::SmallButton("Reset adaptation"))
    {
        ctx.renderer.Exposure().RequestReset();
    }
    InspectorHelp("Snaps the adapted value to the target instantly -- the same thing that "
                  "happens on level load, resize and camera cuts.");

    {
        static float bins[ExposureMetering::kHistogramBins] = {};
        UINT total = 0;
        if (metering.histogramValid && metering.histogramTotal > 0)
        {
            std::copy(metering.histogram.begin(), metering.histogram.end(), bins);
            total = metering.histogramTotal;
            constexpr int kBinCount = static_cast<int>(ExposureMetering::kHistogramBins);
            const ImVec2 size(ImGui::GetContentRegionAvail().x, 70.0f);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::PlotHistogram("##inspectorHistogram", bins, kBinCount, 0, nullptr,
                0.0f, 1.0f, size);

            // Markers follow the CUMULATIVE distribution: placing them at
            // lowPercentile * binCount would simply misreport where the window sits.
            float sum = 0.0f;
            for (int i = 0; i < kBinCount; ++i) { sum += bins[i]; }
            const float invSum = sum > 0.0f ? 1.0f / sum : 0.0f;
            const float lowPct = JsonFloat(tgt(), "lowPercentile", 0.15f);
            const float highPct = JsonFloat(tgt(), "highPercentile", 0.80f);
            int loBin = 0;
            int hiBin = kBinCount - 1;
            bool haveLo = false;
            float cumulative = 0.0f;
            for (int i = 0; i < kBinCount; ++i)
            {
                cumulative += bins[i] * invSum;
                if (!haveLo && cumulative >= lowPct) { loBin = i; haveLo = true; }
                if (cumulative >= highPct) { hiBin = i; break; }
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float binW = size.x / static_cast<float>(kBinCount);
            const float x0 = origin.x + binW * static_cast<float>(loBin);
            const float x1 = origin.x + binW * static_cast<float>(hiBin + 1);
            dl->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1, origin.y + size.y),
                IM_COL32(90, 170, 255, 40));
            ImGui::TextDisabled("blue = the window the percentiles actually meter");
        }
        else
        {
            ImGui::TextDisabled("histogram appears once metering has run");
        }
    }
}

void EnvironmentInspector::DrawColorPipeline()
{
    // P3C. Same set the dev window exposes, so a look tuned live can be written into the
    // level here rather than only through the clipboard button.
    const std::string curve = jsonfield::Value(tgt(), "toneCurve", std::string("legacy"));
    int curveIndex = (curve == "agx") ? 1 : ((curve == "filmic" || curve == "film") ? 2 : 0);
    {
        const char* kCurveNames[] = { "Legacy (ACES fit)", "AgX", "Filmic (Unreal)" };
        const bool changed = ImGui::Combo("Tone Curve", &curveIndex, kCurveNames, 3);
        beginContinuousEdit(changed);
        if (changed)
        {
            tgt()["toneCurve"] = (curveIndex == 1) ? "agx" : (curveIndex == 2 ? "filmic" : "legacy");
        }
        trackContinuousEdit(changed);
    }

    dragF("Vignette", "vignetteIntensity", 0.4f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("UE's natural vignette (VignetteIntensity, default 0.4): the cosine-fourth law, on the "
                  "scene and the bloom before the grade. At 0.4 the top edge's centre keeps 86 %, a corner 57 %. "
                  "0 = none.");

    ImGui::SeparatorText("Colour grade (applies to every curve)");
    dragF("Grade Saturation", "gradeSaturation", 1.30f, 0.01f, 0.0f, 4.0f, "%.3f");
    InspectorHelp("Chroma around luma, applied in linear BEFORE the curve -- the same place "
                  "Unreal bakes it into its LUT. 1 = unchanged, 0 = greyscale.");
    dragF("Grade Contrast", "gradeContrast", 1.15f, 0.01f, 0.1f, 4.0f, "%.3f");
    InspectorHelp("Pivoted on middle grey (0.18), so raising it deepens shadows and lifts "
                  "highlights while midtones stay put. This is the control that STRETCHES "
                  "the histogram -- exposure can only slide it.");
    dragF("Grade Gamma", "gradeGamma", 1.10f, 0.01f, 0.1f, 4.0f, "%.3f");
    InspectorHelp("Midtone weighting. Above 1 opens midtones without moving black or white "
                  "as much as gain would.");
    dragF("Grade Gain", "gradeGain", 1.0f, 0.01f, 0.0f, 4.0f, "%.3f");
    InspectorHelp("Plain multiplier. Overlaps with exposure compensation -- prefer the "
                  "camera's compensation for overall brightness or the two will fight.");
    dragF("Grade Offset", "gradeOffset", 0.0f, 0.001f, -1.0f, 1.0f, "%.4f");
    InspectorHelp("Plain lift. Small positive values give the faded, milky-black film look; "
                  "negative crushes the black point.");

    // Same preset set as the developer window, written through the command stack so a
    // preset is one undo step rather than five stray field edits.
    {
        struct GradePreset { const char* name; float sat, con, gam, gain, off; const char* tip; };
        static const GradePreset kPresets[] = {
            { "Neutral", 1.00f, 1.00f, 1.00f, 1.00f, 0.000f,
              "No grading. Bit-identical to the ungraded image." },
            { "Vivid", 1.40f, 1.25f, 1.00f, 1.00f, 0.000f,
              "The measured match to the reference photograph. Set compensation to suit the "
              "CURVE: about -0.4 EV on Legacy, 0.0 on Filmic." },
            { "Punchy", 1.20f, 1.45f, 1.00f, 1.00f, 0.000f,
              "Contrast-forward rather than colour-forward." },
            { "Filmic", 1.10f, 1.05f, 1.00f, 1.00f, 0.015f,
              "The faded look -- the offset lifts the black point off zero." },
            { "Warm sand", 1.30f, 1.15f, 1.10f, 1.00f, 0.000f,
              "The default. Vivid with the midtones opened up so lit sand and foliage keep "
              "detail. Pairs with -0.15 EV on the Filmic curve." },
            { "Flat", 0.85f, 0.90f, 1.00f, 1.00f, 0.000f,
              "Deliberately washed out, for judging LIGHTING rather than look -- shading "
              "errors stop hiding behind the grade." },
        };
        ImGui::PushID("gradePresets");
        for (int i = 0; i < static_cast<int>(std::size(kPresets)); ++i)
        {
            const GradePreset& preset = kPresets[i];
            if (i != 0) { ImGui::SameLine(); }
            if (ImGui::SmallButton(preset.name))
            {
                nlohmann::json after = withField("gradeSaturation", preset.sat);
                after["gradeContrast"] = preset.con;
                after["gradeGamma"] = preset.gam;
                after["gradeGain"] = preset.gain;
                after["gradeOffset"] = preset.off;
                executeChange(std::move(after), historyLabel);
            }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", preset.tip); }
        }
        ImGui::PopID();
    }

    if (curveIndex == 2)
    {
        ImGui::SeparatorText("Film curve (Unreal's controls)");
        dragF("Film Slope", "filmSlope", 0.88f, 0.005f, 0.1f, 2.0f, "%.3f");
        InspectorHelp("Steepness of the straight middle section, i.e. the curve's overall "
                      "contrast. Unreal's default is 0.88.");
        dragF("Film Toe", "filmToe", 0.55f, 0.005f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("How much the shadows roll off. Higher keeps shadow detail and lifts "
                      "the black end; lower crushes toward black sooner. Default 0.55.");
        dragF("Film Shoulder", "filmShoulder", 0.26f, 0.005f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("How much the highlights roll off. LOWER reaches white sooner, which "
                      "is the knob to use when the image reads short of the reference at "
                      "the top end. Default 0.26.");
        dragF("Film Black Clip", "filmBlackClip", 0.0f, 0.005f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("How far below zero the toe may reach before clipping. Default 0.");
        dragF("Film White Clip", "filmWhiteClip", 0.04f, 0.005f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("How far above one the shoulder may reach. Default 0.04.");
        if (ImGui::SmallButton("Unreal defaults"))
        {
            nlohmann::json after = withField("filmSlope", 0.88f);
            after["filmToe"] = 0.55f;
            after["filmShoulder"] = 0.26f;
            after["filmBlackClip"] = 0.0f;
            after["filmWhiteClip"] = 0.04f;
            executeChange(std::move(after), historyLabel);
        }
        checkB("UE colour pipeline (AP1)", "ueGamut", true);
        InspectorHelp("Unreal's whole pipeline around this curve: the grade and the curve in ACEScg (AP1) with "
                      "the ACES glow and red modifier, Expand Gamut, Blue Correction, and the grade after the "
                      "local exposure and the bloom, where UE's LUT sits. Off = the older linear-sRGB path "
                      "(paler blue skies than UE's at the same numbers).");
        dragF("Expand Gamut", "expandGamut", 1.0f, 0.01f, 0.0f, 4.0f, "%.2f");
        InspectorHelp("UE's ExpandGamut (default 1): bright saturated colour pushed toward a wider gamut. "
                      "UE colour pipeline only.");
        dragF("Blue Correction", "blueCorrection", 0.6f, 0.01f, 0.0f, 1.0f, "%.2f");
        InspectorHelp("UE's BlueCorrection (default 0.6): keeps bright saturated blues from shifting toward "
                      "purple. UE colour pipeline only.");
    }
    else if (curveIndex == 1)
    {
        ImGui::SeparatorText("AgX look");
        dragF("AgX Slope", "agxSlope", 1.0f, 0.01f, 0.0f, 4.0f, "%.3f");
        dragF("AgX Power", "agxPower", 1.0f, 0.01f, 0.1f, 4.0f, "%.3f");
        InspectorHelp("Contrast. NOTE it acts on the [0,1] log-encoded value, so raising it "
                      "DARKENS -- unlike the grade contrast above, which pivots on middle "
                      "grey. That is why the AgX 'punchy' look crushes the image.");
        dragF("AgX Saturation", "agxSaturation", 1.0f, 0.01f, 0.0f, 4.0f, "%.3f");
        ImGui::PushID("agxLook");
        if (ImGui::SmallButton("Neutral"))
        {
            nlohmann::json after = withField("agxSlope", 1.0f); after["agxPower"] = 1.0f; after["agxSaturation"] = 1.0f;
            executeChange(std::move(after), historyLabel);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Punchy"))
        {
            nlohmann::json after = withField("agxSlope", 1.0f); after["agxPower"] = 1.35f; after["agxSaturation"] = 1.4f;
            executeChange(std::move(after), historyLabel);
        }
        ImGui::PopID();
    }
}

#endif // WITH_EDITOR
