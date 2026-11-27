#include "editor/ui/EnvironmentInspector.h"
#if WITH_EDITOR

#include <algorithm>
#include <cstdio>
#include <string>

#include "rendering/lighting/TimeOfDaySettings.h"    // the moon's light colour default
#include "core/JsonField.h" // R9.6: jsonfield::Value -- value()'s conversions, no throw outside the loader
#include "app/scene/Scene.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "imgui.h"
#include "imgui_internal.h" // ImGui::MarkItemEdited

// R10.3: the Inspector's time of day -- sun, moon, stars, lamps, the night (EnvironmentInspector.h).

void EnvironmentInspector::DrawTimeOfDay()
{
    // Time-of-day plan T3: the level's light. The clock and the paths are the object's own
    // properties; the sun's light is the `sun` sub-object (the keys of the old Directional
    // Light, which a converted level carries over unchanged), the moon's the `moon` one.
    {
        const TimeOfDay& live = ctx.scene.GetTimeOfDay();
        const float now = live.Settings().timeHours;
        const int minutes = static_cast<int>(now * 60.0f) % (24 * 60);
        const char* lit = live.LitBy() == TimeOfDay::Body::Sun ? "the sun"
                        : live.LitBy() == TimeOfDay::Body::Moon ? "the moon" : "nobody (both down)";
        ImGui::TextDisabled("Now %02d:%02d - sun %.1f deg, moon %.1f deg (phase angle %.0f) - lit by %s",
                            minutes / 60, minutes % 60, live.SunElevationDeg(), live.MoonElevationDeg(),
                            live.MoonPhaseAngleDeg(), lit);
        if (live.HandOverFade() < 1.0f)
        {
            ImGui::TextDisabled("Hand-over: its light at %.0f %%", 100.0f * live.HandOverFade());
        }
    }

    ImGui::SeparatorText("Clock");
    dragF("Time (h)", "time", 12.0f, 0.01f, 0.0f, 24.0f, "%.2f");
    {
        const float t = JsonFloat(tgt(), "time", 12.0f);
        const int m = static_cast<int>(t * 60.0f + 0.5f) % (24 * 60);
        ImGui::SameLine();
        ImGui::TextDisabled("%02d:%02d", m / 60, m % 60);
    }
    InspectorHelp("Local time, 0-24. The sun is placed on its path for this hour, the moon on "
                  "its own; whichever is up lights the scene (the sun while any of its disc is "
                  "above the horizon). --time=HH:MM on the command line, --set=tod.time:<h>.");
    checkB("Animate", "animate", false);
    InspectorHelp("Run the clock. Off, the level is one instant -- what is saved is what you see. "
                  "On, edits that do not touch the time keep the running clock where it is.");
    const bool real = jsonfield::Value(tgt(), "realPositions", false);
    if (!real)
    {
        dragF("Day Length (min)", "dayLengthMin", 30.0f, 0.1f, 0.01f, 10000.0f, "%.1f");
        dragF("Night Length (min)", "nightLengthMin", 15.0f, 0.1f, 0.01f, 10000.0f, "%.1f");
        InspectorHelp("Real minutes from dawn to dusk and from dusk to dawn while animating "
                      "(Ultra Dynamic Sky's split: 30 and 15).");
    }
    else
    {
        dragF("Time Scale", "timeScale", 60.0f, 1.0f, 0.0f, 1.0e6f, "%.0f",
              ImGuiSliderFlags_Logarithmic);
        InspectorHelp("Game seconds per real second while animating: 1 = real time, 60 = an hour "
                      "a minute, 1440 = a day a minute.");
    }

    ImGui::SeparatorText("Where the sun and the moon are");
    checkB("Real Positions", "realPositions", false);
    InspectorHelp("OFF (Simple): the sun rides a tilted circle from Dawn to Dusk, the moon its own "
                  "circle placed by the phase -- art-directable, and how an old level's authored "
                  "sun is reproduced exactly. ON (Real): date, latitude and longitude, the sun from "
                  "NOAA's calculator (as UE's SunPosition), the moon from Schlyter's elements with "
                  "the parallax and its real phase.");
    if (!real)
    {
        dragF("Dawn (h)", "dawn", 6.0f, 0.01f, 0.0f, 24.0f, "%.2f");
        dragF("Dusk (h)", "dusk", 18.0f, 0.01f, 0.0f, 24.0f, "%.2f");
        dragF("Path Yaw (deg)", "pathYawDeg", 180.0f, 0.5f, -360.0f, 360.0f, "%.1f");
        InspectorHelp("The azimuth the path leans toward: the noon sun stands there (180 = south). "
                      "It rises 90 degrees to the left of it and sets 90 degrees to the right.");
        dragF("Path Tilt (deg)", "pathTiltDeg", 30.0f, 0.1f, -89.0f, 89.0f, "%.1f");
        InspectorHelp("The path's tilt from the zenith: the noon sun stands 90 - tilt high.");
        dragF("Height Offset", "heightOffset", 0.0f, 0.005f, -1.0f, 1.0f, "%.3f");
        dragF("Extend Twilight", "extendTwilight", 0.0f, 0.01f, 0.0f, 1.0f, "%.2f");
        InspectorHelp("Flattens the path near the horizon, so dawn and dusk last longer.");
    }
    else
    {
        int date[3] = { 2026, 6, 21 };
        if (const auto it = tgt().find("date"); it != tgt().end() && it->is_array() && it->size() >= 3)
        {
            for (int i = 0; i < 3; ++i) { if ((*it)[i].is_number()) { date[i] = static_cast<int>(std::lround((*it)[i].get<double>())); } }
        }
        const bool changed = ImGui::InputInt3("Date (Y M D)", date);
        beginContinuousEdit(changed);
        if (changed)
        {
            tgt()["date"] = { std::clamp(date[0], 1, 9999), std::clamp(date[1], 1, 12), std::clamp(date[2], 1, 31) };
        }
        trackContinuousEdit(changed);
        dragF("Latitude (deg)", "latitude", -16.5f, 0.05f, -90.0f, 90.0f, "%.2f");
        dragF("Longitude (deg, +E)", "longitude", -151.75f, 0.05f, -180.0f, 180.0f, "%.2f");
        dragF("UTC Offset (h)", "utcOffset", -10.0f, 0.25f, -14.0f, 14.0f, "%.2f");
    }
    dragF("North Yaw (deg)", "northYawDeg", 0.0f, 0.5f, -360.0f, 360.0f, "%.1f");
    InspectorHelp("Which world direction is north, turned clockwise seen from above; 0 = +Z.");

    // Each body's rows in their own ImGui ID scope: the sun and the moon share labels ("Light Source
    // Angle (deg)", "Temperature (K)") and ImGui otherwise treats the two as one widget.
    groupKey = "sun";
    ImGui::PushID("sun");
    ImGui::SeparatorText("Sun");
    scrollAnchor();
    colorEdit();
    checkB("Use Colour Temperature", "useSunTemperature", false);
    if (jsonfield::Value(tgt(), "useSunTemperature", false))
    {
        dragF("Temperature (K)", "sunTemperatureK", 6500.0f, 25.0f, 1000.0f, 15000.0f, "%.0f");
    }
    InspectorHelp(
        "Tints the sun by a black-body temperature, multiplying the colour above. OFF by "
        "default, so the authored colour is used as-is.\n\n"
        "1700 candle, 2700 tungsten, 4000 cool white, 5500 midday sun, 6500 D65 (neutral "
        "here), 7500+ overcast/shade blue.\n\n"
        "The locus is Unreal's, minus its Stefan-Boltzmann brightness term, and the result "
        "is renormalised to unit luminance -- so this changes HUE ONLY.\n\n"
        "Valid 1000-15000K; outside that the fit bends back on itself, so it is clamped.");
    dragF("Sun Illuminance (lux)", "sunIlluminanceLux", 120000.0f, 1.0f, 0.0f, 200000.0f,
          "%.0f", ImGuiSliderFlags_Logarithmic);
    InspectorHelp(
        "How bright the sun is OUTSIDE the atmosphere, in LUX, perpendicular to the beam: the "
        "atmosphere and the cloud deck take their share on the way down, so the same number "
        "gives a full midday and a red, dim sunset. ~127000 is the real solar constant; "
        "100000-120000 is the usual range.\n\n"
        "Physical values need the photographic camera ON (Post Process > Camera Exposure).");
    dragF("Ambient", "ambient", 0.05f, 0.005f, 0.0f, 10.0f);
    InspectorHelp("The flat fallback fill as a fraction of the light's colour -- only on levels whose "
                  "sky has no irradiance derivatives. Shared by the moon.");
    dragF("Sky Fill Intensity", "skyFillIntensity", 1.0f, 0.01f, 0.0f, 4.0f, "%.3f");
    InspectorHelp(
        "How much of the SKY'S OWN measured irradiance reaches diffuse surfaces. 1 = the "
        "irradiance cube at face value, the physical answer. Shared by the moon, so the fill "
        "does not jump when the light changes hands.");
    {
        const Math::float3 g = JsonFloat3(tgt(), "groundAlbedo", Math::float3(0.25f, 0.25f, 0.25f));
        float gv[3] = { g.x, g.y, g.z };
        const bool changed = ImGui::ColorEdit3("Ground Bounce Albedo", gv);
        beginContinuousEdit(changed);
        if (changed) { tgt()["groundAlbedo"] = { gv[0], gv[1], gv[2] }; }
        trackContinuousEdit(changed);
    }
    InspectorHelp(
        "The DIFFUSE REFLECTANCE OF THE GROUND -- the light that comes back up off the floor "
        "and fills everything facing downward. A reflectance, not a light: it follows the sun "
        "(and the moon) and cannot brighten a night. Dry sand 0.4, grass 0.2, water 0.06.");

    ImGui::SeparatorText("Sun: Light Shafts");
    checkB("Light Shafts", "lightShaftsEnabled", true);
    InspectorHelp(
        "Unreal's Light Shaft Bloom on the sun (plan A7): the bright sky near the sun is radially "
        "blurred towards its screen position and added into the scene -- the god rays through the "
        "crowns. Screen-space: runs only while the sun is in front of the camera. About 0.1 ms.");
    dragF("Bloom Scale", "lightShaftBloomScale", 0.3f, 0.01f, 0.0f, 10.0f, "%.2f");
    InspectorHelp("UE BloomScale: the strength of the shafts (Ultra Dynamic Sky 0.3, UE 0.2; ~1 for rays "
                  "you cannot miss). 0 switches them off.");
    dragF("Bloom Threshold", "lightShaftBloomThreshold", 0.75f, 0.01f, 0.0f, 50.0f, "%.2f");
    InspectorHelp("UE BloomThreshold: only pixels brighter than this seed the shafts, on the pre-exposed "
                  "scale (1 = display white). UDS 0.75: the disc and the bright sky round it.");
    dragF("Bloom Max Brightness", "lightShaftBloomMaxBrightness", 0.35f, 0.01f, 0.0f, 1000.0f, "%.2f");
    InspectorHelp("UE BloomMaxBrightness: caps one pixel's part. UDS 0.35 -- the cap makes their look: "
                  "the sky round the sun seeds wide but flat, so the rays run long across the scene and "
                  "the disc does not burst into a star. UE's 100 = a starburst and short rays.");
    {
        const Math::float3 tint = JsonFloat3(tgt(), "lightShaftBloomTint", Math::float3(1.0f, 1.0f, 1.0f));
        float tv[3] = { tint.x, tint.y, tint.z };
        const bool changed = ImGui::ColorEdit3("Bloom Tint", tv);
        beginContinuousEdit(changed);
        if (changed) { tgt()["lightShaftBloomTint"] = { tv[0], tv[1], tv[2] }; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("UE BloomTint: multiplies the shaft colour. White = the sky's own colour.");
    dragF("Occlusion Depth Range (m)", "lightShaftOcclusionDepthRange", 1000.0f, 10.0f, 1.0f, 20000.0f, "%.0f");
    InspectorHelp("UE LightShaftOcclusionDepthRange (their 1000 m): only pixels in the FAR half of this "
                  "range seed the shafts, so nearby lit surfaces do not streak.");
    dragF("Smoothing (s)", "lightShaftSmoothing", 0.15f, 0.005f, 0.0f, 2.0f, "%.3f");
    InspectorHelp("The time constant of a temporal blend of the shafts' mask: 0 = every frame as it is "
                  "(the rays tremble with every leaf); higher = more inertia.");
    dragF("Shaft Length", "lightShaftLength", 1.0f, 0.01f, 0.05f, 2.0f, "%.2f");
    InspectorHelp("1 = Unreal's rays. Over 1 = LONGER: Unreal's fade as 1/distance from the sun, and this "
                  "slows the fade -- 1.5 as 1/sqrt(distance), 2 not at all (Bloom Scale may then want "
                  "lowering). Under 1 = shorter: the blur stops short of the sun, at 0.5 a ray ends about "
                  "3x its gap's distance from it, at 0.3 about 1.7x. The moon's shafts use it too.");
    dragF("Light Source Angle (deg)", "lightSourceAngle", 0.5357f, 0.005f, 0.0f, 10.0f, "%.4f");
    InspectorHelp("UE LightSourceAngle: the sun's angular DIAMETER, 0.5357 for the real sun. It draws "
                  "the disc and floors the width of the specular lobe; the sun sets when the whole "
                  "disc is under the horizon.");
    dragF("Light Source Soft Angle (deg)", "lightSourceSoftAngle", 0.0f, 0.005f, 0.0f, 10.0f, "%.3f");
    InspectorHelp("UE LightSourceSoftAngle (0 by default; Ultra Dynamic Sky's sun 0.5): softens the edge "
                  "of the sun's glint on glossy surfaces -- the roughness is widened by it before the disc "
                  "acts -- with the glint's energy kept. Specular only; the disc and the shadows keep "
                  "Light Source Angle.");

    ImGui::PopID();
    groupKey = "moon";
    ImGui::PushID("moon");
    ImGui::SeparatorText("Moon");
    scrollAnchor();
    checkB("Moon", "enabled", true);
    InspectorHelp("Off, nobody lights the night: the scene is lit by the sky alone.");
    dragF("Full Moon Illuminance (lux)", "illuminanceLux", 0.32f, 0.001f, 0.0f, 1000.0f, "%.3f",
          ImGuiSliderFlags_Logarithmic);
    InspectorHelp("A FULL moon outside the atmosphere at its mean distance: the sun's 127 klx times the "
                  "full moon's 14 magnitudes fainter = 0.32 lux (~0.27 on the ground with the moon "
                  "overhead). The phase law scales it down (a quarter moon is ~0.1 of full); Real "
                  "positions add the distance.");
    dragF("New Moon Brightness", "newMoonBrightness", 0.0f, 0.001f, 0.0f, 1.0f, "%.3f");
    InspectorHelp("The least the moon's LIGHT falls to with the phase, as a share of the full moon's. 0 = "
                  "physics: a new moon sheds nothing and the night is the airglow and the stars. Ultra "
                  "Dynamic Sky's New Moon Light Brightness is 0.015 -- its moonless night stays a dark blue "
                  "sky. The drawn disc follows the bare phase law either way.");
    checkB("Ultra Dynamic Sky moon", "udsMoon", false);
    InspectorHelp("The moon -- its light on the ground and in the sky, its disc -- and the night sky take Night "
                  "Brightness in full from the SUNSET, as Ultra Dynamic Sky's moon shines at 1/33 of its sun from the "
                  "moment the sun's centre is on the horizon (0 at +1.15 degrees, linear); the sun's twilight sky "
                  "takes none (UDS's twilight is its physical sun, brightened by its sky material: Twilight > Ultra "
                  "Dynamic Sky sky material). Off = one night gain coming in with the night (-6 to -12 degrees). The "
                  "lamps and the night camera keep that one either way. --set=tod.udsMoon.");
    // No Temperature row (removed 2026-10-15): every hue it set has its own row below
    // (TimeOfDaySettings, "NO MOON TEMPERATURE").
    {
        const TimeOfDaySettings defaults{};
        const Math::float3 dc = JsonFloat3(tgt(), "discColor",
            Math::float3(defaults.moonDiscColor[0], defaults.moonDiscColor[1], defaults.moonDiscColor[2]));
        float dv[3] = { dc.x, dc.y, dc.z };
        const bool changed = ImGui::ColorEdit3("Disc Colour", dv);
        beginContinuousEdit(changed);
        if (changed) { tgt()["discColor"] = { dv[0], dv[1], dv[2] }; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("The colour the drawn DISC is seen in (its light shafts grow out of it). Only "
                  "its hue counts (unit luminance: the disc stays as bright as its light). White, as Ultra "
                  "Dynamic Sky draws its moon; a touch of blue reads silver against the night.");
    dragF("Glow", "glow", 0.02f, 0.001f, 0.0f, 2.0f, "%.3f");
    InspectorHelp("A soft glow round the disc, as Ultra Dynamic Sky draws one (its Moon Glow): the peak, as a "
                  "fraction of the full moon's mean brightness, fading to nothing at the radius below; it "
                  "follows the phase. The real moon's is the aureole of the haze in front of it. The light "
                  "shafts seed on it -- without it they drew the whole halo themselves. 0 = none.");
    dragF("Glow Radius (deg)", "glowRadiusDeg", 11.6f, 0.1f, 0.1f, 90.0f, "%.1f");
    InspectorHelp("How far the glow reaches from the moon's centre (UDS 11.6).");
    {
        const TimeOfDaySettings defaults{};
        const Math::float3 lc = JsonFloat3(tgt(), "lightColor",
            Math::float3(defaults.moonLightColor[0], defaults.moonLightColor[1], defaults.moonLightColor[2]));
        float lv[3] = { lc.x, lc.y, lc.z };
        const bool changed = ImGui::ColorEdit3("Light Colour", lv);
        beginContinuousEdit(changed);
        if (changed) { tgt()["lightColor"] = { lv[0], lv[1], lv[2] }; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("The colour the moon LIGHTS the scene AND the sky in -- the cold light of a moonlit night, "
                  "the hue of the moonlit sky (with Twilight > Night Rayleigh Tint, as Ultra Dynamic Sky's "
                  "moon lights its atmosphere). Only its hue counts (used at unit luminance; the lux stay "
                  "physical). The default is UDS's Moon Light Colour. White = the physical moonlight, "
                  "which reads warm on a beige beach.");
    dragF("Light Source Angle (deg)", "lightSourceAngle", 0.52f, 0.005f, 0.0f, 20.0f, "%.3f");
    InspectorHelp("The moon's angular DIAMETER: the drawn disc and the light's size (the specular "
                  "floor, the soft shadow). 0.52 is the real moon's mean.");
    dragF("Light Source Soft Angle (deg)", "lightSourceSoftAngle", 0.0f, 0.005f, 0.0f, 10.0f, "%.3f");
    InspectorHelp("UE LightSourceSoftAngle of the moon light (Ultra Dynamic Sky's moon 0.285): softens "
                  "the edge of its glint on glossy surfaces, the energy kept. 0 = Unreal's default.");
    dragF("Disc Adaptation", "discAdaptation", 1.0f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("How the drawn disc is seen. 1 = by the eye, which adapts to it: brought into the "
                  "display's range, its maria and relief visible, its edge and terminator smooth. 0 = "
                  "by a camera under the night's exposure: ten thousand times the white, clipped flat "
                  "and hard-edged. Only the disc -- the light it sheds is physical either way.");
    dragF("Shaft Bloom Scale", "lightShaftBloomScale", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
    InspectorHelp("The moon's light shafts, which take over once the sun's are gone and the moon lights the "
                  "ground: their strength (Unreal BloomScale on the moon's light). 0 = none. The length, "
                  "smoothing and depth range are the Sun's; Sun > Light Shafts switches both.");
    dragF("Shaft Bloom Threshold", "lightShaftBloomThreshold", 0.008f, 0.001f, 0.0f, 50.0f, "%.3f");
    InspectorHelp("Only pixels brighter than this seed them, on the exposed scale: just above the moonlit "
                  "sky, so the disc AND its glow seed (as Ultra Dynamic Sky's 0.15 does on its ~10x "
                  "brighter night). A brighter night exposure may want it higher -- or the whole sky seeds.");
    dragF("Shaft Max Brightness", "lightShaftBloomMaxBrightness", 0.03f, 0.001f, 0.0f, 1000.0f, "%.3f");
    InspectorHelp("Caps one pixel's part -- at the glow's level next to the disc, as UDS's 0.15 is at its "
                  "glow's: the seed is wide and flat, a soft halo and long soft rays. High (100) = the "
                  "disc alone seeds and the blur draws its copies.");
    {
        const TimeOfDaySettings defaults{};
        const Math::float3 st = JsonFloat3(tgt(), "lightShaftBloomTint",
            Math::float3(defaults.moonLightShaftTint[0], defaults.moonLightShaftTint[1], defaults.moonLightShaftTint[2]));
        float sv[3] = { st.x, st.y, st.z };
        const bool changed = ImGui::ColorEdit3("Shaft Tint", sv);
        beginContinuousEdit(changed);
        if (changed) { tgt()["lightShaftBloomTint"] = { sv[0], sv[1], sv[2] }; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("The moon's shafts are its light scattered by the air, so they come in the Light "
                  "Colour above; this multiplies that, in place of the sun's Bloom Tint (Unreal's "
                  "BloomTint on the moon's light; Ultra Dynamic Sky's Moon Light Shaft Bloom Tint).");
    dragF("Glint Phase Falloff", "glintPhaseFalloff", 0.5f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("The moon's glints on the water, faded again by its phase: 0 = physics (the path follows "
                  "the moon's light alone -- a crescent's only ~3 stops dimmer on screen, the night's "
                  "exposure rising), 1 = faded once more by the phase law. 0.5: the full moon's path as "
                  "it is, a quarter's 1.7 stops down, a thin crescent's 3.3.");
    dragF("Earthshine", "earthshine", 1.0f, 0.05f, 0.0f, 100.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    InspectorHelp("The ashen light on the moon's dark side -- sunlight reflected by the Earth. 1 = the "
                  "physical estimate (about 1/8000 of the full disc at new moon, fading as the Earth's "
                  "own phase wanes toward full moon). Seen on a thin crescent.");
    if (!real)
    {
        dragF("Phase", "phase", 0.5f, 0.005f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("0 new, 0.25 first quarter, 0.5 full, 0.75 last quarter. It changes the moon's FACE "
                      "and its light, not its place: the moon rides opposite the sun (turned by Orbit "
                      "Offset) at any phase, as Ultra Dynamic Sky's does. Real positions take both from "
                      "the date.");
        checkB("Phase Advances", "phaseAuto", true);
        InspectorHelp("While the clock runs, the phase advances one cycle per 29.53 days.");
        dragF("Moon Path Yaw (deg)", "pathYawDeg", 180.0f, 0.5f, -360.0f, 360.0f, "%.1f");
        dragF("Moon Path Tilt (deg)", "pathTiltDeg", 35.0f, 0.1f, -89.0f, 89.0f, "%.1f");
        dragF("Orbit Offset (deg)", "orbitOffsetDeg", 0.0f, 0.5f, -360.0f, 360.0f, "%.1f");
        InspectorHelp("Turns the moon along its circle away from opposite the sun.");
    }

    ImGui::PopID();
    // Time-of-day plan N2: the night sky's own layers.
    groupKey = "nightSky";
    ImGui::PushID("nightSky");
    ImGui::SeparatorText("Night sky");
    scrollAnchor();
    {
        const TimeOfDaySettings defaults{};
        checkB("Night Sky", "enabled", defaults.nightSkyEnabled);
        InspectorHelp("What a sky has without sun or moon -- the airglow, the stars, the Milky Way -- "
                      "procedural, physical by default (a moonless sky far from any town). Drawn in "
                      "the sky, in its picture the water and the probes see, and in the clouds' light "
                      "from below; not while the sun stands above 10 degrees, where none of it can show.");
        dragF("Airglow (cd/m2)", "airglowZenith", defaults.airglowZenith, 1.0e-6f, 0.0f, 1.0f, "%.6f",
              ImGuiSliderFlags_Logarithmic);
        InspectorHelp("The night sky's own glow at the zenith: airglow and zodiacal light, ~1.2e-4 cd/m2 "
                      "at a dark site. Brighter toward the horizon (a layer 90 km up seen edge-on, x6) "
                      "until the air takes it back -- which is the warm band low over the sea.");
        const Math::float3 ag = JsonFloat3(tgt(), "airglowColor",
            Math::float3(defaults.airglowColor[0], defaults.airglowColor[1], defaults.airglowColor[2]));
        float av[3] = { ag.x, ag.y, ag.z };
        const bool changed = ImGui::ColorEdit3("Airglow Colour", av);
        beginContinuousEdit(changed);
        if (changed) { tgt()["airglowColor"] = { av[0], av[1], av[2] }; }
        trackContinuousEdit(changed);
        InspectorHelp("Its hue only (used at unit luminance); the oxygen line makes it faintly green.");
        dragF("Stars", "stars", defaults.starsBrightness, 0.01f, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        InspectorHelp("A multiplier on the stars: 1 = the real sky's counts and brightness (~4500 brighter "
                      "than magnitude 6, down to ~9.7). Each a point of exactly its light, so they do not "
                      "grow with the resolution or flicker under DLSS.");
        dragF("Milky Way", "milkyWay", defaults.milkyWayBrightness, 0.01f, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        InspectorHelp("A multiplier on the galaxy's band (1 = physical: about the airglow's brightness, "
                      "which is why it shows only far from towns and to an adapted eye).");
        dragF("Twinkle", "twinkle", defaults.starTwinkle, 0.01f, 0.0f, 1.0f, "%.2f");
        InspectorHelp("Scintillation, 0..1, stronger toward the horizon. On the scene's wind clock: "
                      "--wind-freeze stills it.");
        if (!real)
        {
            dragF("Sidereal Time at Midnight (h)", "siderealAtMidnight", defaults.siderealAtMidnightHours,
                  0.05f, -48.0f, 48.0f, "%.2f");
            InspectorHelp("Which stars stand overhead at midnight in Simple mode (Real takes it from the "
                          "date and longitude). 18 = a southern winter's night, the galaxy's core high.");
            // The section's own latitude (Real mode's field), the only thing Simple mode reads it for.
            groupKey.clear();
            dragF("Latitude (deg)", "latitude", -16.5f, 0.05f, -90.0f, 90.0f, "%.2f");
            InspectorHelp("How high the celestial pole stands -- the stars turn about it. Real positions "
                          "use it for the sun and the moon as well.");
            groupKey = "nightSky";
        }
    }
    ImGui::PopID();

    // The twilight as Ultra Dynamic Sky draws it.
    groupKey = "twilight";
    ImGui::PushID("twilight");
    ImGui::SeparatorText("Twilight");
    scrollAnchor();
    {
        const TimeOfDaySettings defaults{};
        dragF("Sun Leaves Sky From (deg)", "sunSkyStartDeg", defaults.sunSkyStartDeg, 0.05f, -90.0f, 90.0f, "%.2f");
        dragF("Sun Fully In Sky At (deg)", "sunSkyFullDeg", defaults.sunSkyFullDeg, 0.05f, -90.0f, 90.0f, "%.2f");
        InspectorHelp("The sun's light INTO THE SKY -- the glow of the twilight, the haze, the ambient the "
                      "sky sheds, the clouds -- is 0 with the sun at or below the first elevation and full "
                      "from the second, linear between. Ultra Dynamic Sky's: -9.03 and -6.49. Physics keeps "
                      "a glow down to -18, which over a physical moonlit sky draws a bright band low over "
                      "a black sky; UDS's moon is far brighter and its sun is gone. Both -90 = physics. The "
                      "ground's light is not this: the sun lights it only while its disc is up.");
        const Math::float3 rt = JsonFloat3(tgt(), "rayleighNightTint",
            Math::float3(defaults.rayleighNightTint[0], defaults.rayleighNightTint[1], defaults.rayleighNightTint[2]));
        float rv[3] = { rt.x, rt.y, rt.z };
        const bool changed = ImGui::DragFloat3("Night Rayleigh Tint", rv, 0.005f, 0.0f, 10.0f, "%.3f");
        beginContinuousEdit(changed);
        if (changed) { tgt()["rayleighNightTint"] = { rv[0], rv[1], rv[2] }; }
        trackContinuousEdit(changed);
        InspectorHelp("The atmosphere's Rayleigh scattering, per channel, times this while the sun is down: "
                      "UDS's night Rayleigh colour against its day one (1.43, 0.85, 0.61) -- less blue "
                      "scattered, a paler moonlit sky and a less purple twilight. The sun's own reddening "
                      "follows, as in UDS. (1, 1, 1) = none. A multiplier, not a colour: it may exceed 1.");
        dragF("Day Rayleigh From (deg)", "rayleighDayStartDeg", defaults.rayleighDayStartDeg, 0.05f, -90.0f, 90.0f, "%.2f");
        dragF("Day Rayleigh Full At (deg)", "rayleighDayFullDeg", defaults.rayleighDayFullDeg, 0.05f, -90.0f, 90.0f, "%.2f");
        InspectorHelp("Where the night tint gives way to the authored Rayleigh: from the first sun elevation, "
                      "gone by the second, easing out (UDS's blend: -2.07 to 3.68).");
        const auto tint3 = [&](const char* label, const char* key, const float* fallback) {
            const Math::float3 v = JsonFloat3(tgt(), key, Math::float3(fallback[0], fallback[1], fallback[2]));
            float ov[3] = { v.x, v.y, v.z };
            const bool edited = ImGui::DragFloat3(label, ov, 0.01f, 0.0f, 20.0f, "%.2f");
            beginContinuousEdit(edited);
            if (edited) { tgt()[key] = { ov[0], ov[1], ov[2] }; }
            trackContinuousEdit(edited);
        };
        tint3("Night Ozone Tint", "ozoneNightTint", defaults.ozoneNightTint);
        tint3("Sunrise Ozone Tint", "ozoneSunriseTint", defaults.ozoneSunriseTint);
        dragF("Ozone Blend (deg)", "ozoneBlendDeg", defaults.ozoneBlendDeg, 0.01f, 0.001f, 90.0f, "%.2f");
        InspectorHelp("The atmosphere's ozone, per channel, times the night tint with the sun at or below minus "
                      "this many degrees, the sunrise tint at 0, the level's own ozone from plus this up. "
                      "Earth's ozone absorbs green most, so a twilight sky -- lit by light that crossed the "
                      "ozone layer edge-on -- comes out magenta; UDS's absorbs red as hard and its twilight "
                      "is blue. UDS's values: x(6.15, 1.81, 1.43) at night, x(4.62, 1.56, 4.03) at sunrise, "
                      "1.95 degrees. (1, 1, 1) twice = none.");
        dragF("Night Aerosol", "mieNightScale", defaults.mieNightScale, 0.005f, 0.0f, 10.0f, "%.3f");
        InspectorHelp("The atmosphere's aerosol (its Mie scattering, Sky Atmosphere > Mie Scale) times this at "
                      "night -- none with the sun above -6 degrees, all of it from -12. The glow round a low "
                      "moon is that aerosol's aureole: what a camera sees under a dark sky, not what an eye at a "
                      "window does. The day and the sunset's haze stay as they are. 1 = the same air as by day.");
        checkB("Ultra Dynamic Sky sky material", "udsSkyMaterial", defaults.skyUdsMaterial);
        InspectorHelp("The sky as Ultra Dynamic Sky draws it over the atmosphere -- its sky sphere's material, read node by "
                      "node in UE and checked against UE's bare atmosphere: x Overall Intensity x 0.8 (2.3 + 1.8 u), u "
                      "rising toward the sun (by day x0.69 away from it to x1.23 toward it), a light contrast, a blue Night "
                      "Sky Glow from the sunset on and, at night, the same gradient round the moon; Overall Intensity 0.375 "
                      "by day, 1.25 at the twilight's peak (-2.6 degrees), 0.45 at night. The drawn sky AND its capture "
                      "(the probes, the water's sky reflection), as UDS's sky light captures its sphere. Off = the "
                      "atmosphere as it is. --set=tod.skyUdsMaterial.");
        {
            const UdsSkyFrameData& uds = ctx.scene.GetTimeOfDay().UdsSky();
            if (uds.glow[3] > 0.0f)
            {
                ImGui::TextDisabled("Now: Overall Intensity %.3f, glow (%.1f, %.1f, %.1f) cd/m2", uds.params[0],
                                    uds.glow[0], uds.glow[1], uds.glow[2]);
            }
        }
        dragF("Light Hand-over (deg)", "lightHandOverDeg", defaults.lightHandOverDeg, 0.05f, 0.0f, 20.0f, "%.2f");
        InspectorHelp("When the light passes between the moon and the sun (the sun's disc wholly under the "
                      "horizon), the body that has it fades out over this many degrees of the sun's height "
                      "before, and the one taking it fades in over as many after -- one light at a time, "
                      "no jump of the glint on the water. The sun's disc and the sky are not faded. 0 = the "
                      "hard switch.");
    }
    ImGui::PopID();

    // Plan L: when the lamps marked "On At: night" (lights, glowing meshes) switch, and how they come on.
    groupKey = "lamps";
    ImGui::PushID("lamps");
    ImGui::SeparatorText("Lamps");
    scrollAnchor();
    {
        const TimeOfDaySettings defaults{};
        dragF("Switch At Sun (deg)", "sunElevationDeg", defaults.lampSunElevationDeg, 0.05f, -90.0f, 90.0f, "%.2f");
        InspectorHelp("The lamps marked On At: night (lights, and meshes that glow) come on when the sun's centre "
                      "goes under this elevation and go out when it comes back over it. 0 = sunset and "
                      "sunrise, the moment Ultra Dynamic Sky fires its Sunset / Sunrise events; -3 = a little "
                      "into the dusk, as a street's light sensors do.");
        dragF("Random Delay (s)", "randomDelaySec", defaults.lampRandomDelaySec, 0.01f, 0.0f, 600.0f, "%.2f");
        InspectorHelp("Each lamp switches up to this many seconds after the sun crosses -- a smooth noise of where "
                      "it stands, so a street's lamps come on one by one while a light and the glowing bulb "
                      "beside it come on together. UDS's Random Delay: 1. 0 = all at once.");
        dragF("Turn On (s)", "turnOnSec", defaults.lampTurnOnSec, 0.01f, 0.0f, 600.0f, "%.2f");
        dragF("Turn On Flicker", "turnOnFlicker", defaults.lampTurnOnFlicker, 0.01f, 0.0f, 1.0f, "%.2f");
        dragF("Turn On Exponent", "turnOnExponent", defaults.lampTurnOnExponent, 0.01f, 0.01f, 10.0f, "%.2f");
        InspectorHelp("How a lamp comes on: over Turn On seconds, its brightness along t^Exponent, flickering by "
                      "up to Flicker while it warms (UDS's Light Day/Night Toggle: 1.5 s, 0.6, 0.2). It goes "
                      "out at once. A clock set by hand -- the time slider, setTime -- switches every lamp at "
                      "once.");
        dragF("Day Brightness", "dayBrightness", defaults.lampDayBrightness, 1.0f, 0.0f, 100000.0f, "%.0f",
              ImGuiSliderFlags_Logarithmic);
        InspectorHelp("A manual multiplier on the ALWAYS-ON lamps (no On At) by day, as Night Brightness is the "
                      "night's -- by night they take Night Brightness like the night lamps. 1 = physics: a lamp "
                      "is lost in daylight. Ultra Dynamic Sky's sun is 5 lux against our 120000, which is why a "
                      "lamp shows by day in UE; 24000 weighs it as UDS does.");
        ImGui::TextDisabled("Now: %s", ctx.scene.GetNightLamps().Night() ? "night (lamps on)" : "day (lamps off)");
    }
    ImGui::PopID();

    // The height fog's density by the sun's height (timeOfDay.fog), UDS's day / night fog multipliers.
    groupKey = "fog";
    ImGui::PushID("fog");
    ImGui::SeparatorText("Fog By Sun Height");
    scrollAnchor();
    {
        const TimeOfDaySettings defaults{};
        // THE CURVE OVER THE LEVEL'S DAY (owner, 2026-10-31: "a handier UI for the fog curve"): x = the clock,
        // 0-24 h on the level's date and place; y = the fog's density, log (the height fog's Density x the
        // multiplier), visibility at sea level beside it; the day / twilight / night bands and the sun's arc
        // behind. A handle dragged up / down sets its multiplier, left / right the sun elevation it sits at
        // (drawn where the afternoon passes it; the dawn mirrors). Dragged anywhere else: the clock, and the
        // frame follows. One undo entry a drag (the plot is one ImGui item).
        {
            TimeOfDaySettings day = ctx.scene.GetTimeOfDay().Settings();
            day.fogDayDensity = JsonFloat(tgt(), "dayDensity", defaults.fogDayDensity);
            day.fogDuskDensity = JsonFloat(tgt(), "duskDensity", defaults.fogDuskDensity);
            day.fogNightDensity = JsonFloat(tgt(), "nightDensity", defaults.fogNightDensity);
            day.fogDayFullDeg = JsonFloat(tgt(), "dayFullDeg", defaults.fogDayFullDeg);
            day.fogDuskDeg = JsonFloat(tgt(), "duskDeg", defaults.fogDuskDeg);
            day.fogNightFullDeg = JsonFloat(tgt(), "nightFullDeg", defaults.fogNightFullDeg);
            const HeightFogSettings& fog = ctx.scene.GetHeightFog();
            const bool absolute = fog.enabled && fog.density > 0.0f; // else the multiplier alone
            const float base = absolute ? fog.density : 1.0f;
            const float nowHours = std::clamp(day.timeHours, 0.0f, 24.0f); // the live clock (it runs when animated)

            constexpr int kSamples = 97; // every 15 minutes
            float elev[kSamples];
            float lo = 1.0e30f, hi = -1.0e30f;
            int noon = 0;
            for (int i = 0; i < kSamples; ++i)
            {
                elev[i] = SunElevationAtHours(day, 24.0 * i / (kSamples - 1));
                const float d = std::log10(std::max(base * FogDensityScaleAt(day, elev[i]), 1.0e-7f));
                lo = std::min(lo, d);
                hi = std::max(hi, d);
                if (elev[i] > elev[noon]) { noon = i; }
            }
            for (const float m : { day.fogDayDensity, day.fogDuskDensity, day.fogNightDensity })
            {
                const float d = std::log10(std::max(base * m, 1.0e-7f));
                lo = std::min(lo, d);
                hi = std::max(hi, d);
            }
            lo -= 0.25f;
            hi += 0.25f;
            if (hi - lo < 1.0f) { const float mid = 0.5f * (lo + hi); lo = mid - 0.5f; hi = mid + 0.5f; }
            // The range holds still while a handle is dragged, or the plot would rescale under the mouse.
            static int dragWhat = -1; // 0 day, 1 dusk, 2 night, 3 the clock
            static float dragLo = 0.0f, dragHi = 1.0f;
            if (dragWhat >= 0) { lo = dragLo; hi = dragHi; }

            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float width = std::max(ImGui::GetContentRegionAvail().x, 240.0f);
            const float kLeft = ImGui::CalcTextSize(absolute ? "0.0005" : "x0.05").x + 6.0f;
            const float kRight = absolute ? ImGui::CalcTextSize("16 km").x + 8.0f : 8.0f;
            const float kPlotH = 160.0f, kAxisH = 16.0f;
            const float x0 = origin.x + kLeft, x1 = origin.x + width - kRight;
            const float y0 = origin.y + 4.0f, y1 = y0 + kPlotH;
            const auto xOf = [&](float h) { return x0 + (x1 - x0) * h / 24.0f; };
            const auto hourOf = [&](float x) { return std::clamp((x - x0) / (x1 - x0) * 24.0f, 0.0f, 24.0f); };
            const auto yOf = [&](float density) {
                const float d = std::log10(std::max(density, 1.0e-7f));
                return y1 - (y1 - y0) * std::clamp((d - lo) / (hi - lo), 0.0f, 1.0f);
            };
            const auto densityOf = [&](float y) { return std::pow(10.0f, lo + (y1 - y) / (y1 - y0) * (hi - lo)); };
            // Where the afternoon passes an elevation (the handle's place); -1 if the sun never does.
            const auto afternoonHourAt = [&](float deg) {
                for (int i = noon; i + 1 < kSamples; ++i)
                {
                    if (elev[i] >= deg && elev[i + 1] < deg)
                    {
                        const float t = (elev[i] - deg) / std::max(elev[i] - elev[i + 1], 1.0e-6f);
                        return 24.0f * (i + t) / (kSamples - 1);
                    }
                }
                return -1.0f;
            };
            const float handleDeg[3] = { day.fogDayFullDeg, day.fogDuskDeg, day.fogNightFullDeg };
            const float handleMult[3] = { day.fogDayDensity, day.fogDuskDensity, day.fogNightDensity };
            ImVec2 handlePos[3];
            for (int k = 0; k < 3; ++k)
            {
                const float h = afternoonHourAt(handleDeg[k]);
                handlePos[k] = ImVec2(h < 0.0f ? -1.0e6f : xOf(h), yOf(base * handleMult[k]));
            }

            ImGui::InvisibleButton("##fogCurve", ImVec2(width, kPlotH + kAxisH + 8.0f));
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            if (ImGui::IsItemActivated())
            {
                dragWhat = 3;
                float best = 10.0f * 10.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const float dx = mouse.x - handlePos[k].x, dy = mouse.y - handlePos[k].y;
                    if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; dragWhat = k; }
                }
                dragLo = lo;
                dragHi = hi;
            }
            bool changed = false;
            float newMult = 0.0f, newDeg = 0.0f, newHours = nowHours;
            if (ImGui::IsItemActive() && dragWhat >= 0)
            {
                const float h = hourOf(mouse.x);
                if (dragWhat == 3)
                {
                    newHours = std::round(h * 60.0f) / 60.0f;
                    changed = std::fabs(newHours - nowHours) > 1.0e-4f;
                }
                else
                {
                    newMult = std::round(std::clamp(densityOf(mouse.y) / base, 0.0f, 100.0f) * 100.0f) / 100.0f;
                    newDeg = std::round(SunElevationAtHours(day, h) * 2.0f) / 2.0f;
                    // The three stay in order with half a degree between them.
                    if (dragWhat == 0) { newDeg = std::clamp(newDeg, day.fogDuskDeg + 0.5f, 89.0f); }
                    if (dragWhat == 1) { newDeg = std::clamp(newDeg, day.fogNightFullDeg + 0.5f, day.fogDayFullDeg - 0.5f); }
                    if (dragWhat == 2) { newDeg = std::clamp(newDeg, -89.0f, day.fogDuskDeg - 0.5f); }
                    changed = std::fabs(newMult - handleMult[dragWhat]) > 1.0e-4f ||
                              std::fabs(newDeg - handleDeg[dragWhat]) > 1.0e-4f;
                }
            }
            beginContinuousEdit(changed);
            if (changed)
            {
                static const char* const kMultKey[3] = { "dayDensity", "duskDensity", "nightDensity" };
                static const char* const kDegKey[3] = { "dayFullDeg", "duskDeg", "nightFullDeg" };
                if (dragWhat == 3) { props["time"] = newHours; }
                else
                {
                    tgt()[kMultKey[dragWhat]] = newMult;
                    tgt()[kDegKey[dragWhat]] = newDeg;
                }
                ImGui::MarkItemEdited(ImGui::GetItemID());
            }
            trackContinuousEdit(changed);
            if (!ImGui::IsItemActive()) { dragWhat = -1; }
            const bool hovered = ImGui::IsItemHovered();

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(18, 20, 28, 255));
            for (int i = 0; i + 1 < kSamples; ++i)
            {
                const float e = 0.5f * (elev[i] + elev[i + 1]);
                const ImU32 band = e >= day.fogDayFullDeg ? IM_COL32(70, 120, 200, 70)
                                 : e > day.fogNightFullDeg ? IM_COL32(220, 140, 60, 70) : IM_COL32(0, 0, 0, 0);
                dl->AddRectFilled(ImVec2(xOf(24.0f * i / (kSamples - 1)), y0),
                                  ImVec2(xOf(24.0f * (i + 1) / (kSamples - 1)), y1), band);
            }
            const ImU32 gridCol = IM_COL32(255, 255, 255, 28);
            const ImU32 labelCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            char text[48];
            for (int decade = static_cast<int>(std::floor(lo)); decade <= static_cast<int>(std::ceil(hi)); ++decade)
            {
                for (const float step : { 1.0f, 2.0f, 5.0f })
                {
                    const float d = step * std::pow(10.0f, static_cast<float>(decade));
                    const float l = std::log10(d);
                    if (l < lo || l > hi) { continue; }
                    const float y = yOf(d);
                    dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), gridCol);
                    std::snprintf(text, sizeof(text), absolute ? "%g" : "x%g", d);
                    dl->AddText(ImVec2(origin.x, y - 7.0f), labelCol, text);
                    if (absolute)
                    {
                        // Koschmieder at sea level: 3.912 / sigma, sigma = density (ln 2)^2 (height_fog.hlsli).
                        const float km = 3.912f / (0.48045301f * d) / 1000.0f;
                        std::snprintf(text, sizeof(text), km >= 10.0f ? "%.0f km" : "%.1f km", km);
                        dl->AddText(ImVec2(x1 + 4.0f, y - 7.0f), labelCol, text);
                    }
                }
            }
            for (int h = 0; h <= 24; h += 3)
            {
                dl->AddLine(ImVec2(xOf(static_cast<float>(h)), y1), ImVec2(xOf(static_cast<float>(h)), y1 + 3.0f), labelCol);
                std::snprintf(text, sizeof(text), "%d", h);
                dl->AddText(ImVec2(xOf(static_cast<float>(h)) - 4.0f, y1 + 3.0f), labelCol, text);
            }
            // The sun's arc, -30..75 degrees over the plot's height, faint behind the curve.
            ImVec2 arc[kSamples], curve[kSamples];
            for (int i = 0; i < kSamples; ++i)
            {
                const float x = xOf(24.0f * i / (kSamples - 1));
                arc[i] = ImVec2(x, y1 - (y1 - y0) * std::clamp((elev[i] + 30.0f) / 105.0f, 0.0f, 1.0f));
                curve[i] = ImVec2(x, yOf(base * FogDensityScaleAt(day, elev[i])));
            }
            dl->AddPolyline(arc, kSamples, IM_COL32(255, 220, 120, 70), ImDrawFlags_None, 1.0f);
            const float horizonY = y1 - (y1 - y0) * (30.0f / 105.0f);
            dl->AddLine(ImVec2(x0, horizonY), ImVec2(x1, horizonY), IM_COL32(255, 220, 120, 35));
            dl->AddPolyline(curve, kSamples, IM_COL32(120, 190, 255, 255), ImDrawFlags_None, 2.0f);
            dl->AddLine(ImVec2(xOf(nowHours), y0), ImVec2(xOf(nowHours), y1), IM_COL32(255, 200, 60, 230), 1.5f);
            static const char* const kHandleName[3] = { "day", "dusk", "night" };
            ImVec4 placed[3]; // the labels drawn so far (x0, y0, x1, y1): a label that would cover one goes below
            int placedCount = 0;
            for (int k = 0; k < 3; ++k)
            {
                if (handlePos[k].x < x0 - 1.0f) { continue; } // the sun never passes that elevation today
                const bool hot = dragWhat == k;
                dl->AddCircleFilled(handlePos[k], hot ? 6.5f : 5.5f, IM_COL32(235, 240, 250, 255));
                dl->AddCircle(handlePos[k], hot ? 6.5f : 5.5f, IM_COL32(60, 140, 230, 255), 0, 2.0f);
                std::snprintf(text, sizeof(text), "%s x%.2f", kHandleName[k], handleMult[k]);
                const ImVec2 size = ImGui::CalcTextSize(text);
                const float lx = std::clamp(handlePos[k].x - 0.5f * size.x, x0, x1 - size.x);
                float ly = handlePos[k].y - size.y - 8.0f;
                const auto covers = [&](float y) {
                    for (int j = 0; j < placedCount; ++j)
                    {
                        if (lx < placed[j].z && lx + size.x > placed[j].x && y < placed[j].w && y + size.y > placed[j].y) { return true; }
                    }
                    return false;
                };
                if (ly < y0 || covers(ly)) { ly = handlePos[k].y + 8.0f; }
                if (covers(ly)) { ly += size.y + 2.0f; }
                placed[placedCount++] = ImVec4(lx, ly, lx + size.x, ly + size.y);
                dl->AddText(ImVec2(lx, ly), IM_COL32(235, 240, 250, 230), text);
            }
            if (hovered && dragWhat < 0)
            {
                const float h = hourOf(mouse.x);
                const float e = SunElevationAtHours(day, h);
                const float d = base * FogDensityScaleAt(day, e);
                const int minutes = static_cast<int>(std::lround(h * 60.0f));
                if (absolute)
                {
                    ImGui::SetTooltip("%02d:%02d  sun %.1f deg\ndensity %.4f (x%.2f)  visibility ~%.1f km\n"
                                      "drag a handle: up/down = multiplier, left/right = its sun height;\n"
                                      "drag elsewhere = the clock", minutes / 60, minutes % 60, e, d,
                                      d / base, 3.912f / (0.48045301f * d) / 1000.0f);
                }
                else
                {
                    ImGui::SetTooltip("%02d:%02d  sun %.1f deg  x%.2f\n(the height fog is off: the multiplier alone)",
                                      minutes / 60, minutes % 60, e, d);
                }
            }
            const float eNow = SunElevationAtHours(day, nowHours);
            const float dNow = base * FogDensityScaleAt(day, eNow);
            const int nowMinutes = static_cast<int>(std::lround(nowHours * 60.0f));
            if (absolute)
            {
                ImGui::TextDisabled("Now %02d:%02d  sun %.1f deg  density %.4f (x%.2f)  visibility ~%.1f km", nowMinutes / 60,
                                    nowMinutes % 60, eNow, dNow, dNow / base, 3.912f / (0.48045301f * dNow) / 1000.0f);
            }
            else
            {
                ImGui::TextDisabled("Now %02d:%02d  sun %.1f deg  density x%.2f (the height fog is off)", nowMinutes / 60,
                                    nowMinutes % 60, eNow, dNow);
            }
        }
        dragF("Day Density x", "dayDensity", defaults.fogDayDensity, 0.005f, 0.0f, 100.0f, "%.3f");
        dragF("Dusk Density x", "duskDensity", defaults.fogDuskDensity, 0.005f, 0.0f, 100.0f, "%.3f");
        dragF("Night Density x", "nightDensity", defaults.fogNightDensity, 0.005f, 0.0f, 100.0f, "%.3f");
        InspectorHelp("Multipliers on the height fog's Density (Exponential Height Fog), by the sun's height: Day "
                      "with the sun at Day Full and over, Dusk at Dusk, Night at Night Full and under, smoothly "
                      "between; dawn mirrors dusk. Ultra Dynamic Sky's Fog Density Daytime / Nighttime "
                      "Multiplier, with a dusk of its own. A clear afternoon by the sea sees 20-40 km (a density "
                      "~0.0005); the evening's damp air is the sunset haze. Author Density for the dusk and take "
                      "the day down -- e.g. 0.15. The sunset glow itself is the fog's Sun Scatter and the "
                      "volume's Phase G, not this. All 1 = the level's density at every hour.");
        dragF("Day Full At (deg)", "dayFullDeg", defaults.fogDayFullDeg, 0.1f, -90.0f, 90.0f, "%.1f");
        dragF("Dusk At (deg)", "duskDeg", defaults.fogDuskDeg, 0.1f, -90.0f, 90.0f, "%.1f");
        dragF("Night Full At (deg)", "nightFullDeg", defaults.fogNightFullDeg, 0.1f, -90.0f, 90.0f, "%.1f");
        checkB("Ultra Dynamic Sky fog colour", "udsColor", defaults.fogUdsColor);
        InspectorHelp("The fog's COLOUR by the sun's height, as Ultra Dynamic Sky animates it (read off its actor in "
                      "UE every 3 minutes of its day): the sky's light in the fog (Sky Atmosphere > Height Fog "
                      "Contribution) x2 by day, x0.84 just over the horizon, x1.2 at it, ~x2 at night; the fog's Sun "
                      "Scatter tinted from 0 at the horizon through red (0.24, 0.10, 0.02) at 6.5 degrees to (0.79, "
                      "0.64, 0.56) from 43 -- set Sun Scatter 1 and its colour white to get UDS's own; and a blue "
                      "Inscattering Luminance from the sunset on. Each multiplies the level's own value. Off = the "
                      "level's numbers at every hour. --set=tod.fogUdsColor.");
        {
            const TimeOfDay& tod = ctx.scene.GetTimeOfDay();
            const float* t = tod.FogSunScatterTint();
            const float* f = tod.FogInscatterFloor();
            ImGui::TextDisabled("Now: sky x%.2f, sun scatter (%.2f, %.2f, %.2f), floor (%.1f, %.1f, %.1f) cd/m2",
                                tod.FogSkyContribution(), t[0], t[1], t[2], f[0], f[1], f[2]);
        }
    }
    ImGui::PopID();

    groupKey.clear();
    ImGui::SeparatorText("Night");
    dragF("Night Brightness", "nightBrightness", 1.0f, 0.01f, 0.0f, 100000.0f, "%.2f",
          ImGuiSliderFlags_Logarithmic);
    InspectorHelp("A manual multiplier on the night's light: the moon's, the night sky's (airglow, "
                  "stars, the Milky Way) and the night lamps' (lights and glows set On At: night), so a "
                  "lamp keeps its place against the moonlight. 1 = physics: the full moon 1/375000 of the sun. Ultra Dynamic "
                  "Sky's moon is 1/33 of its sun -- about 11400 here -- and that is what makes its dawn a "
                  "moonlit blue sky until the sun is nearly up; pair it with an exposure curve that keeps "
                  "the night darker than the day (Camera Exposure > Compensation curve).");
    dragF("Day Sky Brightness", "daySkyBrightness", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
    InspectorHelp("A manual multiplier on the sky BY DAY: the sky, its haze on distant things, its "
                  "reflections and the light it sheds, together (the sky's luminance scale). It comes in "
                  "with the day sky -- Twilight > Day Rayleigh From / Full At -- so the night and the "
                  "moonlit dawn keep their darkness. 1 = as the atmosphere gives it.");
    dragF("Night Exposure (stops)", "nightExposureEv", 0.0f, 0.05f, -10.0f, 10.0f, "%+.2f");
    InspectorHelp("The night darker or lighter: stops added to the camera's exposure at night -- none "
                  "with the sun above -6 degrees, all of it from -12, smooth between. Only the picture: "
                  "the light, the meter, the exposure curve and the night vision stay as they are. "
                  "Negative darkens; 0 = none.");
    dragF("Night Camera EV100", "nightCameraEv100", 0.0f, 0.05f, -20.0f, 30.0f, "%.2f");
    InspectorHelp("The steady night: the camera held near this EV100 at night (by the same night share), "
                  "so dim candles stay dim instead of being exposed back up. It is the camera for the full "
                  "moon 45 degrees up with Night Exposure and the compensation at 0 (both move it in full). "
                  "Another night -- a quarter moon, no moon, the twilight -- moves it as Camera Exposure > "
                  "Compensation curve says at that night's physical EV (its points under 0: -3.3 is the full "
                  "moon at 45 degrees, -6.9 a quarter moon 22 degrees up, -11 no moon). Taken from the sky, not "
                  "the meter: a lamp or a turn of the view does not move it. 0 = off, the auto exposure as before.");
    dragF("Night Camera Follow", "nightCameraFollow", 0.2f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("How much of the auto exposure's departure from the Night Camera EV100 the camera "
                  "still follows: 0 = held exactly, 1 = the auto exposure. The eye's own adaptation "
                  "(the meter, night vision) is not touched.");
    dragF("Night Camera Open Sky From", "nightCameraOpenFrom", 0.15f, 0.005f, 0.0f, 1.0f, "%.3f");
    dragF("Night Camera Open Sky Full", "nightCameraOpenFull", 0.35f, 0.005f, 0.0f, 1.0f, "%.3f");
    InspectorHelp("Night Exposure and the steady Night Camera are the OPEN night's: they come in by how "
                  "much open sky the frame sees (the sky itself 1, a surface its RT sky visibility), "
                  "none below From, whole from Full, followed at the exposure's adaptation speeds. Full "
                  "at or below From = always whole. get_readouts' openSky / openNight.");
    dragF("Night Camera Indoor Lift (stops)", "nightCameraIndoorLift", 5.0f, 0.05f, 0.0f, 30.0f, "%.2f");
    InspectorHelp("A closed frame at night follows the auto exposure as by day, but opens at most this "
                  "many stops past the Night Camera EV100: a candle-lit room is exposed for its candles, "
                  "an unlit one stays dark. 0 = never brighter than the open night.");
    ImGui::SeparatorText("Cost");
    dragF("Light Update Threshold (deg)", "lightUpdateThresholdDeg", 0.05f, 0.005f, 0.0f, 5.0f, "%.3f");
    InspectorHelp("While the clock runs, the scene light (and with it every shadow) turns only once "
                  "the body has moved this far; the sky follows exactly. 0.05 degrees is 12 s of a "
                  "real sun.");
}

#endif // WITH_EDITOR
