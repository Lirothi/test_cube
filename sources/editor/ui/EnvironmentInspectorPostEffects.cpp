#include "editor/ui/EnvironmentInspector.h"
#if WITH_EDITOR

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "core/JsonField.h" // R9.6: jsonfield::Value -- value()'s conversions, no throw outside the loader
#include "app/scene/Scene.h"
#include "core/math/Math.h"
#include "imgui.h"

// R10.3: the Inspector's ambient occlusion, exponential height fog and bloom, a look group each, drawn as their
// own object or a section of the folded Post Process (EnvironmentInspector.h).

void EnvironmentInspector::DrawGtao()
{
    // P6B. Every tooltip states the PERFORMANCE consequence as well as the look one, because
    // three of these knobs are linear in GPU cost and that is not guessable from the name.
    {
        bool gtaoEnabled = jsonfield::Value(tgt(), "enabled", false);
        const bool changed = ImGui::Checkbox("Enabled", &gtaoEnabled);
        beginContinuousEdit(changed);
        if (changed) { tgt()["enabled"] = gtaoEnabled; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("Ground-truth screen-space ambient occlusion. The whole chain costs about "
                  "0.12 ms at the defaults (2 directions x 6 steps, half render resolution). "
                  "Off schedules no pass at all, rather than a pass that writes 1.");

    ImGui::SeparatorText("Look");
    dragF("Strength", "strength", 1.0f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("How much of the DYNAMIC occlusion reaches the image. 0 is an exact no-op: "
                  "material AO from the G-buffer still applies, so this cannot switch off "
                  "what F9 already did. FREE - the pass runs either way.");
    dragF("World Radius", "worldRadius", 0.75f, 0.01f, 0.05f, 8.0f, "%.2f m");
    InspectorHelp("Occlusion reach in METRES, so contacts do not grow and shrink as the "
                  "camera moves. FREE in GPU cost. Note it loses authority below about 1 m: "
                  "the search radius is floored at Steps pixels, so past a modest distance it "
                  "stops being a world radius at all.");
    dragF("Sky Radius", "skyRadius", 25.0f, 0.2f, 0.0f, 60.0f, "%.1f m");
    InspectorHelp("P16.4. A SECOND, much wider occlusion radius that damps the SKY FILL only "
                  "- whether this patch of ground is under a canopy or inside a doorway. "
                  "World Radius above cannot answer that: it is a contact radius, and with "
                  "one radius the sky reaches under a palm crown as freely as it reaches "
                  "open sand. At or below World Radius this is OFF and the pass is bit-for-"
                  "bit what it was. COSTS a second horizon walk, so roughly doubles the raw "
                  "GTAO pass when on.");
    dragF("Sky Intensity", "skyIntensity", 1.0f, 0.02f, 0.0f, 3.0f, "%.2f");
    InspectorHelp("The MID-RANGE channel's own strength, scaling its darkening exponent "
                  "independently of the contact channel (1 = the shared-Intensity "
                  "behaviour). At 0 the second walk's COMPUTE PATH is off entirely - the "
                  "same exact no-op as Sky Radius at the bottom, kept as its own switch.");
    {
        int skyMip = static_cast<int>(jsonfield::Value(tgt(), "skyMipBias", 2u));
        const bool changed = ImGui::SliderInt("Sky Mip Bias", &skyMip, 0, 5);
        beginContinuousEdit(changed);
        if (changed) { tgt()["skyMipBias"] = static_cast<std::uint32_t>(skyMip < 0 ? 0 : skyMip); }
        trackContinuousEdit(changed);
    }
    InspectorHelp("Depth-pyramid level the WIDE walk starts from (the contact walk keeps its "
                  "own bias below). Its taps are tens of pixels apart, so a mip-0 fetch lands "
                  "nowhere near the previous one; a coarser level caches better and "
                  "aggregates, which is what you want at a scale where one texel of leaf does "
                  "not decide whether the ground is sheltered. Higher is CHEAPER. Inert while "
                  "Sky Radius is off or Use HZB is unchecked.");
    dragF("Intensity", "intensity", 1.0f, 0.01f, 0.1f, 4.0f, "%.2f");
    InspectorHelp("Exponent on the occlusion term - above 1 deepens, below 1 lifts. FREE.");
    dragF("Thickness", "thickness", 0.6f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("How solid an occluder is assumed to be behind its silhouette. 1 = fully "
                  "solid, so foliage casts a slab behind every leaf; low values keep leaves "
                  "thin. UE equivalent works out to 0.75. FREE.");

    ImGui::SeparatorText("Quality (these cost GPU time)");
    {
        int angles = static_cast<int>(jsonfield::Value(tgt(), "numAngles", 2u));
        const bool changed = ImGui::SliderInt("Directions", &angles, 1, 8);
        beginContinuousEdit(changed);
        if (changed) { tgt()["numAngles"] = static_cast<std::uint32_t>(angles < 1 ? 1 : angles); }
        trackContinuousEdit(changed);
    }
    InspectorHelp("Screen directions searched per pixel. Cost is LINEAR in this - doubling it "
                  "roughly doubles the raw pass. 2 is the UE default (r.GTAO.NumAngles) and "
                  "leans on the temporal stage to average the rest over time.");
    {
        int steps = static_cast<int>(jsonfield::Value(tgt(), "numSteps", 6u));
        const bool changed = ImGui::SliderInt("Steps", &steps, 2, 16);
        beginContinuousEdit(changed);
        if (changed) { tgt()["numSteps"] = static_cast<std::uint32_t>(steps < 1 ? 1 : steps); }
        trackContinuousEdit(changed);
    }
    InspectorHelp("Taps along each direction. Also LINEAR in cost, and it doubles as the floor "
                  "on the pixel radius - so raising it widens the reach at distance as a side "
                  "effect.");
    dragF("Fade Start", "fadeStart", 60.0f, 0.5f, 0.0f, 500.0f, "%.0f m");
    InspectorHelp("Distance where AO begins fading out; beyond it the estimate is mostly noise "
                  "anyway. Pulling both fades IN is the cheapest quality trade here - faded "
                  "pixels still run the pass, but stop contributing artefacts.");
    dragF("Fade End", "fadeEnd", 120.0f, 0.5f, 0.0f, 1000.0f, "%.0f m");
    InspectorHelp("Distance where AO is gone entirely.");

    ImGui::SeparatorText("Filtering");
    {
        bool denoise = jsonfield::Value(tgt(), "denoise", true);
        const bool changed = ImGui::Checkbox("Denoise", &denoise);
        beginContinuousEdit(changed);
        if (changed) { tgt()["denoise"] = denoise; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("Bilateral 5x5 across depth and normal, about 0.017 ms. Removes roughly 40% "
                  "of the raw noise; off is immediately visible as per-pixel grain.");
    {
        bool temporal = jsonfield::Value(tgt(), "temporal", true);
        const bool changed = ImGui::Checkbox("Temporal", &temporal);
        beginContinuousEdit(changed);
        if (changed) { tgt()["temporal"] = temporal; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("Accumulates over frames, which is what makes a 2x6-tap estimate usable at "
                  "all. About 0.012 ms plus one extra half-res target. Off is noisier than any "
                  "setting above can compensate for.");
    dragF("Temporal Blend", "temporalBlendWeight", 0.1f, 0.005f, 0.02f, 1.0f, "%.3f");
    InspectorHelp("Weight of the CURRENT frame. 0.1 (the UE default) is roughly a 10-frame "
                  "history. Higher is more responsive and noisier. FREE.");
    dragF("Temporal Clamp", "temporalClampRange", 0.35f, 0.01f, 0.05f, 1.0f, "%.2f");
    InspectorHelp("How far history may deviate before it is clamped back, with a still camera. "
                  "Below about 0.2 the history stops accumulating on this engine (DLSS jitter "
                  "widens the per-frame spread past the window); above about 0.5 the clamp "
                  "stops protecting against ghosting. FREE.");
    dragF("Filter Plane Tolerance", "filterPlaneTolerance", 0.05f, 0.005f, 0.005f, 1.0f, "%.3f m");
    InspectorHelp("Metres a neighbour may sit off the fitted plane before the denoise drops it. "
                  "Too small leaves grazing floors noisy, too large blurs away the contacts. "
                  "FREE.");
    dragF("Upsample Tolerance", "upsampleTolerance", 0.02f, 0.002f, 0.002f, 1.0f, "%.3f");
    InspectorHelp("Depth tolerance of the edge-aware upsample, as a FRACTION of the pixel "
                  "depth. Large values degrade it to plain bilinear, which spreads occlusion "
                  "past silhouettes. FREE.");

    ImGui::SeparatorText("Advanced");
    {
        bool gbufferNormal = jsonfield::Value(tgt(), "useGBufferNormal", false);
        const bool changed = ImGui::Checkbox("Normal from G-buffer", &gbufferNormal);
        beginContinuousEdit(changed);
        if (changed) { tgt()["useGBufferNormal"] = gbufferNormal; }
        trackContinuousEdit(changed);
    }
    {
        bool useHzb = jsonfield::Value(tgt(), "useHzb", true);
        const bool changed = ImGui::Checkbox("Use depth pyramid (HZB)", &useHzb);
        beginContinuousEdit(changed);
        if (changed) { tgt()["useHzb"] = useHzb; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("Walks the horizon search over the hierarchical depth buffer, reading a "
                  "coarser mip the further a step reaches. Measured 22% faster on the AO pass "
                  "AND slightly less noisy. It under-estimates occlusion a little by design: "
                  "the pyramid keeps the FURTHEST depth in each tile, so a coarse level "
                  "cannot invent contacts.");
    InspectorHelp("OFF matches UE (r.GTAO.UseNormals = 0) and is almost certainly what you "
                  "want: the horizon search walks the DEPTH buffer, so the integral has to be "
                  "given the geometric normal. ON feeds it the normal-mapped one instead, "
                  "which reads as occlusion wherever the two disagree - on detail-mapped sand "
                  "that measured AO 0.35 on a fully open dune. Kept only for comparison.");
}

void EnvironmentInspector::DrawHeightFog()
{
    // P7. The model is transcribed from UE's HeightFogCommon.ush, so each tooltip names the
    // UE parameter it corresponds to -- and, where a UE default does NOT carry over, says
    // why. That distinction is the whole reason these numbers are not simply theirs.
    {
        bool fogEnabled = jsonfield::Value(tgt(), "enabled", false);
        const bool changed = ImGui::Checkbox("Enabled", &fogEnabled);
        beginContinuousEdit(changed);
        if (changed) { tgt()["enabled"] = fogEnabled; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("Global analytic height fog, applied to opaque geometry in compose AND to "
                  "the ocean surface, which share one packed parameter set so the water and "
                  "the land cannot end up in different weather. Off by default: it is a real "
                  "image change and earns its default with an explicit A/B. Off costs "
                  "nothing - the shader skips the whole block on a zero density.");

    ImGui::SeparatorText("Medium");
    dragF("Density", "density", 0.002f, 0.00005f, 0.0f, 0.05f, "%.5f");
    InspectorHelp("UE's FogDensity per METRE (UE's is per 10 m: they divide by 1000 per "
                  "centimetre), so UE's number / 10 = this one: UE's default 0.02 is 0.002 "
                  "here. Visibility at the reference height ~ 8.1 / Density metres (0.002: "
                  "4 km of mist; a clear day at sea, 15-30 km, is ~0.0003-0.0005).");
    dragF("Height Falloff", "heightFalloff", 0.02f, 0.0005f, 0.0f, 0.2f, "%.4f");
    InspectorHelp("UE's FogHeightFalloff per METRE, base 2: UE's number / 10 = this one (UE's "
                  "default 0.2 is 0.02 here). How fast density thins with altitude: it halves "
                  "every 1 / falloff metres; 0 makes it a uniform distance fog. This term is "
                  "what keeps a high camera from getting a flat screen-space wash - looking "
                  "down from altitude, most of the ray is already in thin air.\n\n"
                  "IT ALSO SETS HOW THIN THE LAYER IS, and a thin layer seen from above "
                  "collapses into a BRIGHT LINE ON THE HORIZON: 0.058 is a 17 m layer, and "
                  "from 80 m up the whole sea sits at transmittance ~0.9 while only the ray "
                  "aimed exactly at the horizon accumulates depth (measured at 82 m: a "
                  "25.7/255 step across the horizon row at 0.058, 1.6 at 0.030). A marine "
                  "haze is ~1 km thick: ~0.0015.");
    dragF("Reference Height", "referenceHeight", 0.0f, 0.5f, -100.0f, 500.0f, "%.1f m");
    InspectorHelp("World Y at which Density is exactly the authored value. Sea level here. "
                  "UE call this the fog actor's own height.");
    dragF("Start Distance", "startDistance", 25.0f, 0.5f, 0.0f, 500.0f, "%.0f m");
    InspectorHelp("UE's StartDistance: fog-free air in front of the camera, so near contrast "
                  "survives. Their default is 0; this project starts at 25 m because the "
                  "canonical views put the beach within a few tens of metres.");
    dragF("Max Opacity", "maxOpacity", 0.9f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("UE's FogMaxOpacity, and note what it actually does: it is a FLOOR on "
                  "transmittance, clipping the far end of the curve, not a scale on "
                  "coverage that would bend the whole curve. 0 makes the fog a no-op while "
                  "still enabled, which is a free A/B lever.\n\n"
                  "THE CEILING IS RELEASED AGAIN DEEP INTO THE FOG, and without that the "
                  "whole range below 1 was unusable: the sky is never fogged - it IS fog of "
                  "infinite depth - so it ignores the ceiling, while the water at the "
                  "horizon kept (1 - maxOpacity) of its own colour and the two could not "
                  "meet. Measured at 25 m, the luminance step across the horizon row at "
                  "0.70 was 9.9/255 and is now 2.65, which is what 1.00 gives. The ceiling "
                  "still does its job where it means something - at 25 m, 0.70 against 1.00 "
                  "moves 21% of the frame, and that difference sits in the FAR and middle "
                  "thirds, not on the horizon line.");

    ImGui::SeparatorText("Fog colour (procedural sky)");
    {
        const Math::float3 scale = JsonFloat3(tgt(), "skyAmbientContribution", Math::float3(1.0f, 1.0f, 1.0f));
        float sv[3] = { scale.x, scale.y, scale.z };
        const bool changed = ImGui::ColorEdit3("Sky Atmosphere Ambient Contribution", sv,
                                               ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
        beginContinuousEdit(changed);
        if (changed) { tgt()["skyAmbientContribution"] = { sv[0], sv[1], sv[2] }; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("UE's SkyAtmosphereAmbientContributionColorScale (default white): the fog's colour is the "
                  "atmosphere's distant sky light times this -- ONE colour for every direction, on the ground "
                  "and on the sky, so the air veils the horizon as in UE (HeightFogCommon.ush:189-195). 0 leaves "
                  "only the Inscattering Luminance below. Procedural sky only: the HDRI sky colours the fog with "
                  "the sky along the view (UE's inscattering cubemap). --set=fog.skyAmbient (a gray), "
                  "fog.skyAlongView:1 = the old colour, the A/B.");
    {
        const Math::float3 lum = JsonFloat3(tgt(), "inscatteringLuminance", Math::float3(0.0f, 0.0f, 0.0f));
        float lv[3] = { lum.x, lum.y, lum.z };
        const bool changed = ImGui::DragFloat3("Inscattering Luminance (cd/m2)", lv, 10.0f, 0.0f, 100000.0f, "%.0f");
        beginContinuousEdit(changed);
        if (changed) { tgt()["inscatteringLuminance"] = { std::max(lv[0], 0.0f), std::max(lv[1], 0.0f), std::max(lv[2], 0.0f) }; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("UE's FogInscatteringLuminance (default black): an authored fog colour added to the sky "
                  "ambient above, in physical units -- a daylight sky is thousands of cd/m2. "
                  "--set=fog.inscatterLuminance (a gray).");

    ImGui::SeparatorText("Sun in-scattering");
    dragF("Sun Scatter", "sunScatterStrength", 0.35f, 0.01f, 0.0f, 3.0f, "%.2f");
    InspectorHelp("Weight of the forward-scattered sun added on top of the sky colour, so "
                  "looking into the sun warms the haze and looking away from it does not. "
                  "Judge it on wind_test's sun_glint view - no other canonical view has a "
                  "sun field to read it against.");
    {
        const Math::float3 c = JsonFloat3(tgt(), "sunScatterColor", Math::float3(1.0f, 1.0f, 1.0f));
        float cv[3] = { c.x, c.y, c.z };
        const bool changed = ImGui::ColorEdit3("Sun Scatter Colour", cv, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
        beginContinuousEdit(changed);
        if (changed) { tgt()["sunScatterColor"] = { std::max(cv[0], 0.0f), std::max(cv[1], 0.0f), std::max(cv[2], 0.0f) }; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("UE's DirectionalInscatteringLuminance is a colour: Sun Scatter is its grey, this its tint "
                  "(white = the grey alone). UE multiplies it by the sun's luminance in space, so it carries no "
                  "sunset of its own -- Time of Day > Ultra Dynamic Sky fog colour reddens it with the sun. "
                  "--set=fog.sunScatterR / G / B.");
    dragF("Sun Scatter Tightness", "sunScatterExponent", 4.0f, 0.25f, 1.0f, 64.0f, "%.0f");
    InspectorHelp("UE's DirectionalInscatteringExponent, and their default of 4 IS used "
                  "here: it is dimensionless, so unlike density it transfers unchanged. "
                  "Higher = a tighter glow around the sun.");
    dragF("Sun Scatter Start", "sunScatterStartDistance", 100.0f, 1.0f, 0.0f, 1000.0f, "%.0f m");
    InspectorHelp("UE's DirectionalInscatteringStartDistance: the sun lobe builds up on its "
                  "OWN line integral past this distance, so it does not tint the near "
                  "field. Theirs is 10000 in centimetres. As in UE the lobe is added beside "
                  "the fog colour, not scaled by the fog's coverage a second time, and it "
                  "reaches the sky pixels too (the sky is fogged), so no seam forms at the "
                  "horizon.");

    ImGui::SeparatorText("Volumetric fog");
    checkB("Volumetric (froxel) fog", "volumetric", false);
    InspectorHelp("UE's VolumetricFog on top of the analytic model: inside the volume distance the "
                  "air is a lit, sun-shadowed volume (light shafts through the palms), beyond it the "
                  "analytic fog continues. --set=fog.volumetric:1");
    dragF("Volume Distance", "volumetricDistance", 300.0f, 1.0f, 10.0f, 2000.0f, "%.0f m");
    dragF("Volume Start", "volumetricStartDistance", 0.0f, 0.1f, 0.0f, 500.0f, "%.1f m");
    InspectorHelp("UE's VolumetricFogStartDistance (default 0): the volume's own near plane. Not the "
                  "analytic Start Distance above -- the analytic fog is already kept out of the volume's "
                  "range. --set=fog.volumetricStart");
    dragF("Volume Fade-in", "volumetricNearFadeInDistance", 0.0f, 0.1f, 0.0f, 100.0f, "%.1f m");
    InspectorHelp("UE's VolumetricFogNearFadeInDistance (default 0): the volume fades in over this "
                  "much of every ray from its start. --set=fog.volumetricFadeIn");
    dragF("Albedo", "albedo", 1.0f, 0.01f, 0.0f, 1.0f, "%.2f");
    dragF("Extinction Scale", "extinctionScale", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
    dragF("Phase G", "phaseG", 0.2f, 0.01f, -0.99f, 0.99f, "%.2f");
    InspectorHelp("Henyey-Greenstein anisotropy (UE VolumetricFogScatteringDistribution, their "
                  "default 0.2): towards 1 the shafts brighten when looking into the sun.");
    dragF("Volume Sun Intensity", "sunScatter", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f"); // the sun INSIDE the volume; "Sun Scatter" above is the analytic lobe beyond it
    dragF("Volume Sky Intensity", "skyScatter", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
    dragF("History Weight", "historyWeight", 0.9f, 0.01f, 0.0f, 0.99f, "%.2f");
    checkB("Temporal reprojection", "temporal", true);
    checkB("Jitter (with the history)", "jitter", true);
    dragF("Samples Per Cell", "samplesPerCell", 2.0f, 0.05f, 1.0f, 4.0f, "%.0f");
    InspectorHelp("Jittered samples per froxel every frame. 1 = UE (one sample + TAA); 2 = antithetic pair, "
                  "removes the flicker at a spot tip; 3-4 = more, at the pass cost.");
    checkB("Conservative depth (skip cells behind geometry)", "conservativeDepth", true);
    checkB("Local lights in the volume", "localLights", true);
    dragF("Local Light Intensity", "localLightScatter", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
    dragF("Local Cone Soft Fading", "localSoftFading", 1.0f, 0.05f, 0.0f, 4.0f, "%.2f froxels");
    InspectorHelp("UE r.VolumetricFog.LightSoftFading: a spot cone edge fades over this many froxels, "
                  "which is what removes the stepped, flickering edge. 0 = hard edge (UE default).");
    dragF("Local Distance Bias", "localDistanceBias", 1.0f, 0.05f, 0.0f, 4.0f, "%.2f");

    ImGui::SeparatorText("Sky sampling (HDRI sky)");
    dragF("Sky Blur", "skyBlur", 0.5f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("How blurred the sky is where it is read as the fog's COLOUR, expressed as "
                  "a roughness. 0 samples it sharp, and then the fog is literally a picture "
                  "of what stands behind the surface - cloud edges and the sunset band print "
                  "themselves onto the palms in front of them. Only the lightly-fogged end "
                  "is blurred: a fully fogged pixel always converges on the sharp sky, "
                  "because the sky IS fog of infinite depth and anything else seams at the "
                  "horizon. FREE - it is a mip choice, not an extra sample.");
    dragF("Non-Directional Dist", "nonDirectionalDistance", 0.0f, 0.5f, 0.0f, 10000.0f, "%.0f m");
    dragF("Fully Directional Dist", "fullyDirectionalDistance", 0.0f, 5.0f, 0.0f, 100000.0f, "%.0f m");
    InspectorHelp("Where the sky sample stops being a direction and becomes an average. "
                  "Nearer than the first distance the fog is coloured by the WHOLE SPHERE; "
                  "past the second it is the sky in the direction you are looking, and "
                  "between them it crossfades. BOTH ZERO = off, which is how this ships. "
                  "UE run it at 10 m / 1000 m, but they MULTIPLY the cubemap into an "
                  "authored fog colour while we use the sample as the colour itself, so "
                  "their safe near-field average turns into milk-white water here. Raise "
                  "them only with a before/after in hand.");

    ImGui::TextDisabled("Fog colour comes from the SKY along the view ray - the same shape as");
    ImGui::TextDisabled("UE's InscatteringColorCubemap, generated from the sky instead");
    ImGui::TextDisabled("of authored. That is what removes the horizon seam by construction.");
    // The debug views deliberately live only in the dev window: they are a viewing mode,
    // not level data, and putting them on the object would invite them into the save.
    ImGui::TextDisabled("Debug views (transmittance / in-scattering) are in the F1 window,");
    ImGui::TextDisabled("Render tab - they are a viewing mode, not level data.");
    ImGui::TextDisabled("Fix exposure when comparing: auto-exposure reacts to fog and");
    ImGui::TextDisabled("shifts the WHOLE frame, sky included.");
}

void EnvironmentInspector::DrawBloom()
{
    // P8. Every tooltip says where the number is measured, because the two that matter --
    // threshold and intensity -- are in units that are easy to assume wrongly.
    {
        bool bloomEnabled = jsonfield::Value(tgt(), "enabled", false);
        const bool changed = ImGui::Checkbox("Enabled", &bloomEnabled);
        beginContinuousEdit(changed);
        if (changed) { tgt()["enabled"] = bloomEnabled; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("HDR bloom composited into the image BEFORE the tone curve, in either of UE's "
                  "two forms (Method). Off schedules no pass at all rather than a pass that "
                  "adds zero, so off genuinely costs nothing.");

    ImGui::SeparatorText("Method");
    {
        int method = static_cast<int>(jsonfield::Value(tgt(), "method", 0u));
        const char* kMethods[] = { "Standard (pyramid)", "Convolution (FFT)" };
        const bool changed = ImGui::Combo("Method", &method, kMethods, 2);
        beginContinuousEdit(changed);
        if (changed) { tgt()["method"] = static_cast<std::uint32_t>(method < 0 ? 0 : method); }
        trackContinuousEdit(changed);
    }
    InspectorHelp("STANDARD is UE's Gaussian bloom on a mip pyramid: cheap, symmetric, and "
                  "structurally incapable of a streak -- a stack of Gaussians has no shape "
                  "to give.\n\n"
                  "CONVOLUTION is UE's FFT bloom: the whole scene convolved with a kernel "
                  "IMAGE. Halo, starburst and rays all come out of that ONE kernel. It costs "
                  "more and runs at Resolution % of the display.\n\n"
                  "Only the knobs of the chosen method are shown.");

    dragF("Intensity", "intensity", 0.675f, 0.0005f, 0.0f, 64.0f, "%.4f",
          ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
    InspectorHelp("UE's BloomIntensity, default 0.675. 0 is an exact no-op and skips the "
                  "chain. It also scales the lens ghosts (UE: LensFlareIntensity * "
                  "BloomIntensity). Logarithmic: the useful range spans orders of magnitude.\n\n"
                  "STANDARD: the weight of UE's six Gaussians, Bloom{n}Tint * Intensity / 6, "
                  "laid on the pyramid's levels by their sizes.\n\n"
                  "CONVOLUTION: UE's ScatterDispersionIntensity, the multiplier on the "
                  "fraction of the kernel's energy that SCATTERS. That fraction is a "
                  "property of the kernel IMAGE; every kernel in textures/ scatters what UE's "
                  "DefaultBloomKernel does (~1.75 % at the default size -- "
                  "tools/bloom_kernel_delta.py), so 0.1-1 is the range for all of them. The "
                  "light is PARTITIONED, not added: the scene is dimmed by exactly what the "
                  "flare takes.");

    if (jsonfield::Value(tgt(), "method", 0u) == 0u)
    {
    ImGui::SeparatorText("Standard (pyramid) only");
    dragF("Threshold", "threshold", -1.0f, 0.01f, -1.0f, 16.0f, "%.2f");
    InspectorHelp("UE's FFT bloom has no threshold, and neither does the Convolution here "
                  "(its filter is the bright-pixel gain).\n\n"
                  "Luminance at which bloom starts, in the units the viewer sees -- UE's "
                  "BloomThreshold. NEGATIVE DISABLES IT, and that is UE's own default "
                  "(-1 in Scene.cpp), documented there as \"all pixels affect bloom "
                  "equally (physically correct)\". A lens scatters light from EVERYTHING "
                  "in front of it, not only from whatever passes a brightness test.\n\n"
                  "It also decides how bloom answers to EXPOSURE. With a threshold, raising "
                  "exposure both scales the pixels already over the line and pushes new "
                  "ones over it, so bloom grows faster than the image does. At -1 the "
                  "response is exactly linear: twice the exposure, twice the bloom.");
    dragF("Soft Knee", "softKnee", 0.5f, 0.01f, 0.05f, 4.0f, "%.2f");
    InspectorHelp("Slope of the ramp above the threshold; UE hardwire 0.5, which is the "
                  "default here. Lower = a longer shoulder, so highlights ease into the "
                  "bloom instead of switching on at a fixed brightness.");
    {
        bool firefly = jsonfield::Value(tgt(), "fireflyClamp", false);
        const bool changed = ImGui::Checkbox("Firefly Clamp", &firefly);
        beginContinuousEdit(changed);
        if (changed) { tgt()["fireflyClamp"] = firefly; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("OURS, off by default -- UE's bloom downsample has none. A Karis average on "
                  "the FIRST downsample only: each tap is weighted by 1/(1+luma), so one "
                  "blown-out texel is averaged DOWN instead of surviving the whole pyramid. "
                  "Turn it on if moving sun glints on water pump the entire frame.");
    dragF("Radius", "radius", 1.0f, 0.01f, 0.0f, 4.0f, "%.2f");
    InspectorHelp("Tap spacing of the tent upsample, in DESTINATION texels, so it means the "
                  "same thing at every level and at every resolution. It spreads the same "
                  "energy wider rather than adding any: widen it and the halo gets larger "
                  "and fainter, it does not get brighter.");
    }

    if (jsonfield::Value(tgt(), "method", 0u) == 1u)
    {
        ImGui::SeparatorText("Kernel (convolution only)");
        // P8C-2r: WHICH image. Everything downstream -- placement, the core clamp, the
        // centre/scatter split -- is derived from the pixels, so this one pick changes the
        // entire character of the glare with nothing else to retune. The list is whatever
        // square FP16 DDS sits in textures/; it is scanned once and cached, because an
        // inspector that touches the filesystem every frame is a stutter waiting to happen.
        {
            static std::vector<std::string> kernels;
            static bool scanned = false;
            if (!scanned)
            {
                scanned = true;
                std::error_code ec;
                for (const auto& e : std::filesystem::directory_iterator("textures", ec))
                {
                    if (ec) { break; }
                    if (!e.is_regular_file()) { continue; }
                    const std::filesystem::path& fp = e.path();
                    std::string ext = fp.extension().string();
                    for (char& c : ext) { c = static_cast<char>(::tolower(c)); }
                    if (ext == ".dds") { kernels.push_back("textures/" + fp.filename().string()); }
                }
                std::sort(kernels.begin(), kernels.end());
            }
            const std::string cur = jsonfield::Value(tgt(), "convKernel", std::string("textures/BloomKernelStar.dds"));
            int sel = -1;
            std::vector<const char*> items;
            items.reserve(kernels.size());
            for (size_t i = 0; i < kernels.size(); ++i)
            {
                items.push_back(kernels[i].c_str());
                if (kernels[i] == cur) { sel = static_cast<int>(i); }
            }
            if (!items.empty())
            {
                const bool changed = ImGui::Combo("Kernel Image", &sel, items.data(),
                                                  static_cast<int>(items.size()));
                beginContinuousEdit(changed);
                if (changed && sel >= 0) { tgt()["convKernel"] = kernels[static_cast<size_t>(sel)]; }
                trackContinuousEdit(changed);
            }
        }
        InspectorHelp("The kernel photograph itself. Anything square and FP16 in textures/ "
                      "works: BloomKernelStar is the DERIVED one (nine-bladed diffraction, "
                      "18 rays, measured 7.6-12.3x ray contrast), DefaultBloomKernel is "
                      "UE's original photograph (2.6-4.2x -- their weak rays are the "
                      "arithmetic consequence of their broad glow, not a defect). "
                      "Switching reloads the image and re-surveys it; nothing else needs "
                      "retuning, though the centre/scatter fractions differ per image so "
                      "Intensity will not mean quite the same thing across a swap.");
        // P8C-2: the kernel is UE's photographed DefaultBloomKernel. The star, its rays,
        // the halo and the rainbow dispersion are IN the image -- the aperture generator
        // and its shape controls (kernel radius, spokes, chroma) are retired.
        dragF("Kernel Size", "convSize", 1.0f, 0.01f, 0.02f, 1.0f, "%.2f");
        InspectorHelp("The kernel photograph's width as a fraction of the viewport -- "
                      "UE's BloomConvolutionSize, same units and same default of 1.0. "
                      "This is the ONE size control: the visible glare is the kernel "
                      "image, scaled.");
        {
            float ktint[3] = { 1.0f, 1.0f, 1.0f };
            if (tgt().contains("convKernelTint") && tgt()["convKernelTint"].is_array() &&
                tgt()["convKernelTint"].size() == 3)
            {
                for (int i = 0; i < 3; ++i)
                {
                    ktint[i] = jsonfield::As(tgt()["convKernelTint"][i], ktint[i]);
                }
            }
            const bool changed = ImGui::ColorEdit3("Kernel Tint", ktint,
                ImGuiColorEditFlags_Float);
            beginContinuousEdit(changed);
            if (changed) { tgt()["convKernelTint"] = { ktint[0], ktint[1], ktint[2] }; }
            trackContinuousEdit(changed);
        }
        InspectorHelp("How strongly each channel SCATTERS (ours; UE's ScatterDispersion is "
                      "one number): a bluish tint gives a bluish flare, and the light a "
                      "channel does not scatter stays in the scene, so the frame's colour "
                      "balance is kept. It cannot sit in the kernel image: the transform "
                      "normalises each channel by its own sum, as UE's does.");

        ImGui::SeparatorText("Bright-pixel gain (UE's prefilter)");
        dragF("Gain Slope", "convPreFilterMult", 15.0f, 0.05f, 0.0f, 64.0f, "%.2f");
        InspectorHelp("UE's BloomConvolutionPreFilterMult (default 15), and the ONLY "
                      "filtering their FFT bloom has -- there is no threshold anywhere in "
                      "that path. Nothing is cut: a pixel below Min passes untouched, a "
                      "pixel above Min has its luminance remapped with this slope and "
                      "clamped at Max. 0 = off (under UE's own gate 0 would clamp every "
                      "pixel above Min to Min).");
        dragF("Gain Min", "convPreFilterMin", 7.0f, 0.05f, 0.0f, 1000.0f, "%.2f",
              ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
        InspectorHelp("Where the boost starts (UE default 7), in the PRE-EXPOSED units the "
                      "scene is stored in -- UE's FilterPixel runs on the pre-exposed scene "
                      "colour too. Below it, pixels are left exactly as they are. Off when "
                      "Min >= Max (UE's gate).");
        dragF("Gain Max", "convPreFilterMax", 15000.0f, 10.0f, 0.0f, 65504.0f, "%.1f",
              ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
        InspectorHelp("The CEILING the boosted luminance is clamped to (UE default 15000, "
                      "pre-exposed) -- what any single pixel can contribute, however bright "
                      "it gets. The physical sun disc sits far above everything else in "
                      "the frame; this is what bounds its flare.");
        dragF("Resolution %", "convPercent", 50.0f, 0.5f, 10.0f, 50.0f, "%.0f %%");
        InspectorHelp("Resolution of the convolution as a percent of the display -- UE's "
                      "r.Bloom.ScreenPercentage (their default is 100; 50 is this "
                      "engine's grid ceiling). The first P8C ran at 12.5, and a 1-2 texel "
                      "diffraction ray upscaled 4x per axis is a dashed line of squares "
                      "-- the 'ragged crown'. Lowering this buys the transform cost back.");
        checkB("Before the upscaler", "convBeforeUpscale", true);
        InspectorHelp("Convolve the scene BEFORE DLSS. DLSS takes a glint of a few pixels "
                      "for aliasing and flattens it -- a chrome sphere's sun glint kept about "
                      "a tenth of its energy, so the kernel's star never formed. No effect "
                      "without DLSS.");
        dragF("Temporal Weight", "convTemporalWeight", 0.1f, 0.005f, 0.01f, 1.0f, "%.3f");
        InspectorHelp("With Before the upscaler: the bloom's light (the convolution, the ghosts, "
                      "the streak) is accumulated over frames, and this is the new frame's share. "
                      "The scene before DLSS is jittered, so a glint smaller than a pixel lands in "
                      "it one frame and misses the next -- its star flickered. UE's bloom reads "
                      "TSR's output, which does this averaging. Lower = steadier, and a moving "
                      "glint's flare lags more. 1 = off.");
        dragF("Sun Kernel Scale", "convSunKernelScale", 1.0f, 0.005f, 0.0f, 10.0f, "%.3f",
              ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
        InspectorHelp("ART, not physics: the sun disc's own share of the kernel's light (its "
                      "star, halo and rays). The disc carries thousands of times a glint's "
                      "light, so with one Intensity the sun's star dominates and a glint's "
                      "barely shows -- raise Intensity for everything and pull the sun back "
                      "here. The sun is the sky within Sun Radius of it (with the ghosts), "
                      "applied after Boost. 1 = the sun like any other source.");
    }

    // P8C-2l: THE FLARES ARE NOT PART OF EITHER BLOOM METHOD. They read the HDR image
    // and add into the same bloom target the pyramid and the convolution both write, so
    // they live outside the method's own block -- and outside its `if`, or half of them
    // would vanish from the UI the moment someone chose Standard.
    ImGui::SeparatorText("Anamorphic Streak (any bloom method)");
    // P8C-2h/l: an anisotropic PYRAMID of its own (KinoStreak's structure), reading the
    // HDR image and adding into the bloom target -- it belongs to neither bloom method
    // and runs with both.
    dragF("Anamorphic Intensity", "convAnamorphicIntensity", 0.0f, 0.02f, 0.0f, 8.0f, "%.2f");
    InspectorHelp("Direct brightness of the streak. 0 = off (the passes do not run).");
    dragF("Anamorphic Threshold", "convAnamorphicThreshold", 1.5f, 0.05f, 0.1f, 10000.0f, "%.2f",
          ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
    InspectorHelp("The streak's OWN threshold, and since P8C-2h the ONLY narrowing "
                  "control -- the vertical erosion that used to sit below was deleted "
                  "with the cascade (a min-filter cannot taper at a frame edge). In the "
                  "PRE-EXPOSED units the scene is stored in, as the ghost threshold and "
                  "UE's LensFlareThreshold. Higher values take only the CORE of a "
                  "source, so a bright sky round the sun stops throwing a screen-tall "
                  "band; the physical sun disc clears any sensible value by thousands.");
    dragF("Anamorphic Length", "convAnamorphicLength", 0.28f, 0.005f, 0.01f, 1.0f, "%.3f");
    InspectorHelp("How far the band reaches, as a fraction of the screen width -- "
                  "the VISIBLE extent, so 0.1 draws a band about a tenth of the "
                  "screen long (plus the source's own width, which no filter can "
                  "shorten). It was authored as a 1/e until P8C-2e, and a 1/e lies by "
                  "3.4x. It is realised by WEIGHTING PYRAMID LEVELS -- the level whose "
                  "reach matches the number, with a tent across its neighbours for the "
                  "fraction -- and verified in numpy against the exact tap pattern: "
                  "asked 128/256/512/768 px delivered 92/204/420/788, i.e. 0.72-1.03x. "
                  "The floor is level 0's own reach, about 30 px; the ceiling is the "
                  "deepest level's, about 3200 px, so the whole slider is live.");

    dragF("Anamorphic Width", "convAnamorphicWidth", 3.0f, 0.1f, 0.5f, 30.0f, "%.1f px");
    InspectorHelp("The band's final soft width: a small vertical Gaussian applied at "
                  "composite, in display pixels. Works on the already-blurred band, "
                  "so it can be narrow without aliasing.");
    dragF("Anamorphic Chroma", "convAnamorphicChroma", 0.5f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("Spreads the per-channel 1/e lengths: blue runs ~45% farther and "
                  "red ~30% shorter at 1.0, so the tail shifts white -> blue along "
                  "its length -- the look of real cylindrical-element coatings.");
    {
        float tint[3] = { 1.0f, 1.0f, 1.0f };
        if (tgt().contains("convAnamorphicTint") &&
            tgt()["convAnamorphicTint"].is_array() &&
            tgt()["convAnamorphicTint"].size() == 3)
        {
            for (int i = 0; i < 3; ++i)
            {
                tint[i] = jsonfield::As(tgt()["convAnamorphicTint"][i], tint[i]);
            }
        }
        const bool changed = ImGui::ColorEdit3("Anamorphic Tint", tint,
            ImGuiColorEditFlags_Float);
        beginContinuousEdit(changed);
        if (changed) { tgt()["convAnamorphicTint"] = { tint[0], tint[1], tint[2] }; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("A plain colour multiplier on the whole band, on top of the "
                  "chroma's own gradient.");

    ImGui::SeparatorText("Lens Ghosts (any bloom method)");
    {
        int ghosts = static_cast<int>(jsonfield::Value(tgt(), "convGhosts", 0u));
        const bool changed = ImGui::SliderInt("Ghost Count", &ghosts, 0, 8);
        beginContinuousEdit(changed);
        if (changed) { tgt()["convGhosts"] = static_cast<std::uint32_t>(ghosts < 0 ? 0 : ghosts); }
        trackContinuousEdit(changed);
    }
    InspectorHelp("P8C-2: UE's actual mechanism, both halves. A bokeh SCATTER splats "
                  "one iris sprite per bright pixel of the thresholded scene -- the "
                  "output is the real defocused image of the real sources -- and the "
                  "composite lays N copies of that image scaled about the screen "
                  "centre (UE's LensFlareTints table: two on the source's side, the "
                  "rest mirrored through the centre). No sun position and no sprite "
                  "atlas exist anywhere: two suns give two chains for free.");
    {
        int blades = static_cast<int>(jsonfield::Value(tgt(), "convBlades", 6u));
        const bool changed = ImGui::SliderInt("Blades", &blades, 0, 12);
        beginContinuousEdit(changed);
        if (changed) { tgt()["convBlades"] = static_cast<std::uint32_t>(blades < 0 ? 0 : blades); }
        trackContinuousEdit(changed);
    }
    InspectorHelp("Number of iris blades in the BOKEH SPRITE the scatter splats -- 0 "
                  "is a round bokeh. It shapes the ghosts only: the bloom kernel is a "
                  "photograph and carries its own star.\n\n"
                  "MEASURED, so you know what to expect. Against a 1/255 noise floor, "
                  "3 blades vs 8 moves the frame by 8/255 and 0 (round) vs 8 by 2/255 "
                  "-- an octagon IS very nearly a circle, so the low counts are where "
                  "this control lives. It also needs Ghost Size to be LARGER than the "
                  "source: a ghost is source-convolved-with-sprite, and the bigger of "
                  "the two wins the shape. Blade ROTATION used to sit here and was "
                  "removed in P8C-2d for measuring 4/255 against that same floor -- "
                  "superposition over an extended source washes an orientation out "
                  "entirely.");
    dragF("Ghost Size", "convGhostBokeh", 3.0f, 0.05f, 0.0f, 32.0f, "%.2f %%");
    InspectorHelp("Bokeh sprite radius as a percent of frame width -- UE's "
                  "LensFlareBokehSize, same units and same default of 3. Bigger "
                  "copies are correspondingly dimmer: same light over more area.\n\n"
                  "IT IS ALSO THE SHAPE CONTROL, which is not obvious. A ghost is the "
                  "source CONVOLVED with this sprite, so whichever of the two is "
                  "bigger wins: with the sprite smaller than the source, every ghost "
                  "reproduces the SOURCE's outline -- at 0.5% the sun's ragged corona "
                  "and the water's glitter path came out as recognisable mirrored "
                  "smears in the sky. At UE's 3 the sprite dominates and the chain "
                  "reads as bokeh discs again.");
    dragF("Ghost Intensity", "convGhostIntensity", 1.0f, 0.001f, 0.0f, 1000.0f, "%.4f",
          ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
    InspectorHelp("UE's LensFlareIntensity (default 1); the chain gets it times the "
                  "bloom Intensity, as theirs does. The colour comes from the sources "
                  "themselves, so a dimmer sun throws dimmer ghosts and a sky with "
                  "nothing above the threshold throws none at all.");
    dragF("Ghost Threshold", "convGhostThreshold", 8.0f, 0.05f, 0.0f, 65504.0f, "%.2f",
          ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
    InspectorHelp("UE's LensFlareThreshold (default 8): a BINARY gate on R+G+B of the "
                  "pre-exposed scene, as theirs. Ghosts are images of SOURCES -- too low "
                  "and sunlit foliage becomes one (upside-down palms in the sky, "
                  "observed). The physical sun disc clears it by thousands, so its ghost "
                  "is the disc defocused through the iris. Raising it also cuts the "
                  "scatter's cost directly: collapsed quads rasterize nothing.");
    dragF("Sun Ghost Scale", "convSunGhostScale", 1.0f, 0.005f, 0.0f, 10.0f, "%.3f",
          ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
    InspectorHelp("ART, not physics: the sun disc's own share of the ghosts. The disc outshines "
                  "a glint by thousands, so its chain is what you see while a glint's barely "
                  "shows -- raise Ghost Intensity for everything and pull the sun back here. "
                  "Found by direction (see Sun Radius). 1 = the sun like any other source.");
    dragF("Sun Radius (deg)", "convSunSplitRadiusDeg", 8.0f, 0.05f, 0.1f, 45.0f, "%.1f");
    InspectorHelp("What Sun Kernel Scale and Sun Ghost Scale call the sun: the SKY within this "
                  "angle of it (fully to half of it, fading out to all of it). The disc alone is "
                  "not enough -- the bright sky round it makes as much of its halo and ghosts. "
                  "Geometry never counts, so a glint next to the sun on screen (the sea under "
                  "a low sun) keeps the common share.");

    ImGui::SeparatorText("Sun glare");
    dragF("Sun Glare", "sunGlareIntensity", 0.0065f, 0.0002f, 0.0f, 1.0f, "%.4f",
          ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
    InspectorHelp("A corona around the sun that the convolution cannot give: an analytic "
                  "halo about the sun's direction, added to the bloom. 0 = off.\n\n"
                  "The SHARE OF THE SUN DISC'S LIGHT it carries: the disc's brightness "
                  "times its solid angle, spread over the halo -- so the radii change its "
                  "size, not its total. Its colour and strength are the "
                  "frame's own pixels over the disc, so a palm in front of the sun, a "
                  "cloud over it or a sunset dims and tints it by itself. Works with "
                  "either bloom method.");
    dragF("Glare Radius", "sunGlareRadiusDeg", 2.0f, 0.05f, 0.05f, 30.0f, "%.2f deg");
    InspectorHelp("The bright core's size, in degrees from the sun's centre (its "
                  "e-folding angle). Degrees, not pixels: the same at any resolution "
                  "or field of view.");
    dragF("Glare Veil", "sunGlareVeil", 0.15f, 0.005f, 0.0f, 2.0f, "%.3f");
    InspectorHelp("Weight of the WIDE, faint veil under the core -- the haze of light "
                  "a real lens or eye spreads far across the frame. 0 = core only.");
    dragF("Veil Radius", "sunGlareVeilRadiusDeg", 12.0f, 0.1f, 0.1f, 90.0f, "%.1f deg");
    InspectorHelp("Angle at which the veil has fallen to half.");
    dragF("Low-Sun Below", "sunGlareLowSunDeg", 12.0f, 0.1f, 0.0f, 90.0f, "%.1f deg");
    InspectorHelp("Sun elevation below which the glare starts to ease down (0 = never). The "
                  "exposure opens up as the sun sets, so a glare sized for noon washes a "
                  "sunset frame white.");
    dragF("Low-Sun Floor", "sunGlareLowSunFloor", 0.2f, 0.005f, 0.0f, 1.0f, "%.3f");
    InspectorHelp("What is left of the glare with the sun on the horizon, as a fraction of "
                  "Sun Glare (1 = no fade).");
    dragF("Glare Smoothing", "sunGlareSmoothingSec", 0.1f, 0.005f, 0.0f, 2.0f, "%.3f s");
    InspectorHelp("How slowly the glare AND the corona follow what shows of the sun disc: the time "
                  "constant of a temporal blend of the disc's metering. Leaves crossing the disc "
                  "made both dance; 0 = every frame as it is.");

    ImGui::SeparatorText("Sun corona");
    dragF("Corona", "sunRaysIntensity", 0.0104f, 0.0002f, 0.0f, 2.0f, "%.4f",
          ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
    InspectorHelp("The glare structure around the sun, drawn at full resolution in the tone "
                  "curve pass after Ritschel's \"Temporal Glare\": dense bright bundles with "
                  "dim gaps, fine needles inside them, warm tips, a faint rainbow halo ring. "
                  "0 = off.\n\n"
                  "Scaled by the sun disc's ENERGY (its solid angle over the corona's "
                  "nominal area), so it follows the disc's light, not its brightness per "
                  "pixel. Colour and strength are the frame's own pixels over the sun "
                  "disc, so anything in front of the sun takes the corona with it.");
    dragF("Partial Sun", "sunRaysVisibleExponent", 0.5f, 0.005f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("How much corona a PARTLY hidden sun keeps: the clear disc's corona x (the share of "
                  "the disc that shows)^this. 1 = physical, the rays follow the light that gets "
                  "through -- a sun 80 % behind leaves keeps a fifth of them and they are gone; "
                  "0.5 keeps 45 % there and 22 % at a twentieth of the disc, like a photograph "
                  "of a sun through fronds; 0 = any sliver of sun throws the full corona. A clear "
                  "sun is the same at every value. The share is measured on the scene before the "
                  "upscaler (DLSS rings on the disc).");
    {
        static std::vector<std::string> coronaImages;
        static bool coronaScanned = false;
        if (!coronaScanned)
        {
            coronaScanned = true;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator("textures", ec))
            {
                if (ec) { break; }
                if (!e.is_regular_file()) { continue; }
                std::string name = e.path().filename().string();
                std::string lower = name;
                for (char& ch : lower) { ch = static_cast<char>(::tolower(ch)); }
                const bool image = lower.size() > 4 &&
                    (lower.ends_with(".png") || lower.ends_with(".dds") || lower.ends_with(".jpg"));
                if (image && (lower.find("corona") != std::string::npos || lower.find("glare") != std::string::npos))
                {
                    coronaImages.push_back("textures/" + name);
                }
            }
            std::sort(coronaImages.begin(), coronaImages.end());
        }
        const std::string cur = jsonfield::Value(tgt(), "sunRaysTexture", std::string());
        std::vector<const char*> items{ "(procedural)" };
        int sel = cur.empty() ? 0 : -1;
        for (size_t i = 0; i < coronaImages.size(); ++i)
        {
            items.push_back(coronaImages[i].c_str());
            if (coronaImages[i] == cur) { sel = static_cast<int>(i) + 1; }
        }
        const bool changed = ImGui::Combo("Corona Image", &sel, items.data(), static_cast<int>(items.size()));
        beginContinuousEdit(changed);
        if (changed && sel >= 0) { tgt()["sunRaysTexture"] = sel == 0 ? std::string() : coronaImages[static_cast<size_t>(sel) - 1]; }
        trackContinuousEdit(changed);
    }
    InspectorHelp("Draw the corona from a painted image (grayscale on black, for additive "
                  "use; any textures/*corona* or *glare* file) or procedurally. The image is "
                  "one texture fetch a pixel; the procedural one has the knobs below.");
    const bool coronaFromImage = !jsonfield::Value(tgt(), "sunRaysTexture", std::string()).empty();
    if (coronaFromImage)
    {
        dragF("Corona Size", "sunRaysTextureSizeDeg", 15.0f, 0.1f, 0.5f, 90.0f, "%.1f deg");
        InspectorHelp("The image's half-width on the sky, in degrees: its core should sit on "
                      "the sun disc.");
        dragF("Corona Rotation", "sunRaysRotationDeg", 0.0f, 0.5f, -180.0f, 180.0f, "%.1f deg");
    }
    else
    {
    {
        int bundles = static_cast<int>(jsonfield::Value(tgt(), "sunRaysBundles", 12u));
        const bool changed = ImGui::SliderInt("Bundles", &bundles, 1, 64);
        beginContinuousEdit(changed);
        if (changed) { tgt()["sunRaysBundles"] = static_cast<std::uint32_t>(bundles < 1 ? 1 : bundles); }
        trackContinuousEdit(changed);
    }
    InspectorHelp("How many bright wedges around the circle, each at a random angle and "
                  "brightness; the gaps between them stay dim. A bright bundle also reaches "
                  "further.");
    dragF("Bundle Width", "sunRaysBundleWidthDeg", 5.0f, 0.05f, 0.1f, 90.0f, "%.1f deg");
    InspectorHelp("A bundle's width as an ANGLE around the sun (full width at half maximum), "
                  "so it opens out with distance like a wedge. At most about the spacing "
                  "between bundles (360 / Bundles): wider than that they are one glow.");
    dragF("Bundle Strength", "sunRaysBundleStrength", 0.5f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("How much the bundles rule. 0 = needles evenly round the circle and all "
                  "alike; 1 = every needle gathered into a bundle, dark between them. It "
                  "brightens and gathers together, so the corona's light stays the same "
                  "(measured within 20 % from 0 to 1). On a small corona a high strength "
                  "packs a bundle's needles into one beam.");
    {
        int needles = static_cast<int>(jsonfield::Value(tgt(), "sunRaysCount", 100u));
        const bool changed = ImGui::SliderInt("Needles", &needles, 16, 1000);
        beginContinuousEdit(changed);
        if (changed) { tgt()["sunRaysCount"] = static_cast<std::uint32_t>(needles < 16 ? 16 : needles); }
        trackContinuousEdit(changed);
    }
    InspectorHelp("How many needles around the circle, lit by the bundles they cross; each has "
                  "its own brightness and length, and one in ten reaches about twice as far.\n\n"
                  "Only so many fit side by side: at a distance d from the sun the circle "
                  "holds 2 pi d / width of them. Closer in than that they merge into glow "
                  "-- so a higher count shows as more LONG needles, with the glow reaching "
                  "further out, not as more needles at the core.");
    dragF("Needle Width", "sunRaysWidthDeg", 0.15f, 0.001f, 0.005f, 2.0f, "%.3f deg");
    InspectorHelp("A needle's thickness on the sky (full width at half maximum), the same "
                  "all along it. 0.045 deg is about a pixel at 1440p with a 90-degree field "
                  "of view; a needle is never drawn thinner than a pixel -- below that it "
                  "gets fainter instead. Wider needles also merge into glow sooner.");
    dragF("Corona Length", "sunRaysLengthDeg", 2.5f, 0.05f, 0.1f, 30.0f, "%.2f deg");
    InspectorHelp("Where a needle has fallen to half, in degrees on the sky. The fall-off is a "
                  "power law: a dense core with a long faint tail.");
    dragF("Halo", "sunRaysHalo", 0.08f, 0.005f, 0.0f, 1.0f, "%.3f");
    InspectorHelp("The lenticular halo: a faint rainbow ring, red outside. 0 = none.");
    dragF("Halo Radius", "sunRaysHaloRadiusDeg", 3.5f, 0.05f, 0.1f, 30.0f, "%.2f deg");
    dragF("Star Spikes", "sunRaysSpikes", 0.0f, 0.01f, 0.0f, 3.0f, "%.2f");
    InspectorHelp("An extra REGULAR star on top (eyelashes, an aperture's spikes). 0 = none.");
    {
        int spikes = static_cast<int>(jsonfield::Value(tgt(), "sunRaysSpikeCount", 6u));
        const bool changed = ImGui::SliderInt("Star Spike Count", &spikes, 2, 32);
        beginContinuousEdit(changed);
        if (changed) { tgt()["sunRaysSpikeCount"] = static_cast<std::uint32_t>(spikes < 2 ? 2 : spikes); }
        trackContinuousEdit(changed);
    }
    dragF("Star Spike Width", "sunRaysSpikeWidthDeg", 0.1f, 0.001f, 0.005f, 2.0f, "%.3f deg");
    InspectorHelp("A star spike's thickness on the sky, as Needle Width.");
    dragF("Star Spike Length", "sunRaysSpikeLengthDeg", 10.0f, 0.05f, 0.1f, 60.0f, "%.1f deg");
    InspectorHelp("Where a star spike has fallen to half, in degrees on the sky; a tenth at "
                  "three lengths, gone by three and a half.");
    dragF("Corona Rotation", "sunRaysRotationDeg", 0.0f, 0.5f, -180.0f, 180.0f, "%.1f deg");
    InspectorHelp("Turns the whole pattern on screen.");
    }

    ImGui::TextDisabled("Runs after the upscaler on the image the tone curve reads,");
    ImGui::TextDisabled("so the pyramid is sized off the DISPLAY resolution and does");
    ImGui::TextDisabled("not change shape with the DLSS quality mode.");
    ImGui::TextDisabled("Fix exposure when comparing: bloom moves average luminance");
    ImGui::TextDisabled("and auto-exposure will chase it.");
    ImGui::TextDisabled("Streak and ghosts run with EITHER bloom method: they read the");
    ImGui::TextDisabled("HDR image and add into the same target both methods write.");
}

#endif // WITH_EDITOR
