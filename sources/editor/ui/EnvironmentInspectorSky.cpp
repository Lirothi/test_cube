#include "editor/ui/EnvironmentInspector.h"
#if WITH_EDITOR

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "core/JsonField.h" // R9.6: jsonfield::Value -- value()'s conversions, no throw outside the loader
#include "app/scene/Scene.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/scene/EnvironmentRuntime.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/commands/EditEnvironmentCommand.h"
#include "editor/commands/EditorCommandStack.h"
#include "rendering/lighting/DirectionalLight.h"
#include "imgui.h"

// R10.3: the Inspector's sky -- the atmosphere, the volumetric clouds, the skybox (EnvironmentInspector.h).

void EnvironmentInspector::DrawSkyAtmosphere()
{
    // B6.2. Having this object AT ALL is what makes the level procedural -- the same shape
    // as UE, where a SkyAtmosphere is an actor you add rather than a flag on the sky. Delete
    // it and the level goes back to its cubemap. `enabled` is here so it can be switched off
    // without losing the tuning, exactly as the other look sections have it.
    checkB("Enabled", "enabled", true);
    InspectorHelp(
        "Runs the Hillaire atmosphere -- transmittance, multi-scattering, SkyView and "
        "distant-light LUTs -- from the sun's direction and the parameters below, instead "
        "of reading the level's cubemap.\n\n"
        "The cubemap is still used while this is off, and remains the fallback the fog and "
        "the water read before the procedural capture exists, so switching costs no reload. "
        "A procedural level STILL NEEDS a Skybox texture: the atmosphere is drawn by the Skybox "
        "object, which the level loader creates only when one is named -- without it the sky "
        "renders black.\n\n"
        "--set=sky.mode overrides this for a headless run.");

    dragF("Sky Luminance Scale", "luminanceScale", 2.13f, 0.01f, 0.0f, 10.0f, "%.3f");
    InspectorHelp(
        "UE SkyAndAerialPerspectiveLuminanceFactor: multiplies the sun's illuminance on the way "
        "INTO every atmosphere table, so it brightens the sky, the aerial-perspective haze on "
        "geometry AND the ambient the sky's probes deliver -- all of them together, relative to "
        "the direct sun. It is NOT sky-only (the row below is), and it is never the camera's "
        "exposure. 1 = physics.");
    dragF("Sky-only Luminance", "skyLuminanceFactor", 1.0f, 0.01f, 0.0f, 10.0f, "%.3f");
    InspectorHelp(
        "The PICTURE of the sky only: the pixels of sky you see, multiplied when they are drawn. "
        "Nothing the sky lights moves with it -- not the ambient on the sand, not the reflections, "
        "not the haze on geometry, not the sun disc. (UE's SkyLuminanceFactor also feeds their "
        "sky-light capture; measured here, that made this row and the one above the same knob, "
        "so ours does not.)");

    ImGui::SeparatorText("What the atmosphere feeds");
    checkB("Environment lighting", "environmentLighting", false);
    InspectorHelp("Rebuilds the reflection cube and the sky lighting from this atmosphere "
                  "whenever the sun or the parameters change. OFF leaves both on the "
                  "cubemap's, so a procedural sky would light the scene with a different "
                  "sky than the one you can see.");
    checkB("Aerial perspective", "aerialPerspective", false);
    InspectorHelp(
        "The atmosphere's own camera volume, 32 x 32 x 16 over 96 km, applied to distant "
        "OPAQUE geometry. It is not the height fog: that is Post Process > Exponential "
        "Height Fog, and it is a separate layer the fog is composed over "
        "(SkyAtmosphereCommon.ush:148-151).");
    dragF("Aerial Start Depth (m)", "aerialStartDepthMetres", 100.0f, 1.0f, 0.0f, 5000.0f, "%.0f");
    InspectorHelp(
        "UE AerialPerspectiveStartDepth (their 0.1 km): view depth before which the volume adds "
        "nothing. Its own knob since the part-B review -- it used to ride on the height fog's "
        "volumetric distance, a subsystem that can be off while this is on.");
    dragF("Aerial View Distance Scale", "aerialViewDistanceScale", 1.0f, 0.1f, 0.01f, 100.0f, "%.2f");
    InspectorHelp(
        "UE AerialPerspectiveViewDistanceScale: how many metres of Earth's air one metre of view "
        "distance counts as. 1 = Earth, and Earth's air over a few hundred metres is invisible "
        "(measured 0.8 % on the far water, nothing on the island). 5-20 gives an island the haze "
        "of kilometres -- this is the knob that makes aerial perspective READ, not the start depth.");

    ImGui::SeparatorText("Medium");
    dragF("Rayleigh Scale", "rayleighScale", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
    InspectorHelp("Multiplies Earth's Rayleigh coefficients, so 1.0 is Earth. It is what "
                  "makes the sky blue and the horizon red; more of it deepens both.");
    dragF("Mie Scale", "mieScale", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
    InspectorHelp("Multiplies Earth's Mie scattering AND absorption together -- haze and "
                  "aerosol. It is what puts the white glare around the sun.");
    dragF("Mie Anisotropy", "mieAnisotropy", 0.8f, 0.005f, 0.0f, 0.999f, "%.3f");
    InspectorHelp("How forward-peaked the Mie phase is -- UE's MieAnisotropy, same range "
                  "and default. 0 scatters uniformly; closer to 1 throws light forward and "
                  "puts a HALO around the sun.\n\n"
                  "This is the knob for the bright dome that sits over the horizon at a low "
                  "sun, and it is not the same as Mie Scale: that one changes how much haze "
                  "there is, which takes the sunset's warmth with it. This changes only how "
                  "tightly that haze aims the light at you.");
    dragF("Ozone Scale", "ozoneScale", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
    InspectorHelp("Multiplies Earth's ozone absorption, which is what keeps a twilight sky "
                  "blue overhead instead of grey.");
    {
        const auto tint3 = [&](const char* label, const char* key) {
            const Math::float3 v = JsonFloat3(tgt(), key, Math::float3(1.0f, 1.0f, 1.0f));
            float ov[3] = { v.x, v.y, v.z };
            const bool edited = ImGui::DragFloat3(label, ov, 0.005f, 0.0f, 20.0f, "%.3f");
            beginContinuousEdit(edited);
            if (edited) { tgt()[key] = { ov[0], ov[1], ov[2] }; }
            trackContinuousEdit(edited);
        };
        tint3("Rayleigh Tint", "rayleighTint");
        tint3("Mie Tint", "mieTint");
        tint3("Ozone Tint", "ozoneTint");
    }
    InspectorHelp("The medium's colours, per channel, over the scales above: UE's RayleighScattering / "
                  "MieScattering / OtherAbsorption colours against their defaults, so a UE sky ports exactly. "
                  "(1, 1, 1) = Earth's colour. Ultra Dynamic Sky's: Rayleigh (0.962, 0.996, 1) at scale 1.208, Mie "
                  "(0.802, 0.880, 1) at 3.503 -- a bluish aerosol --, ozone (2.596, 1, 2.109) at 1.063.");
    dragF("Mie Absorption Scale", "mieAbsorptionScale", -1.0f, 0.01f, -1.0f, 10.0f, "%.2f");
    InspectorHelp("Multiplies Earth's Mie absorption on its own (UE MieAbsorptionScale / 0.000444). -1 = the "
                  "Mie Scale's, which moves scattering and absorption together. Ultra Dynamic Sky's: 1 (Earth's "
                  "absorption under 3.5x the scattering).");
    dragF("Height Fog Contribution", "heightFogContribution", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
    InspectorHelp("UE's SkyAtmosphere HeightFogContribution (default 1): the sky's light IN THE HEIGHT FOG -- "
                  "the fog's colour from the distant sky light, and the ground-illuminance half of its sun "
                  "lobe. Not the sky. A dense horizontal fog of the sky's AVERAGE colour reads darker than the "
                  "horizon sky by day; Ultra Dynamic Sky sets 2 (and less at sunset: Time of Day > Ultra "
                  "Dynamic Sky fog colour multiplies this). --set=sky.heightFogContribution.");
    dragF("Ground Albedo", "groundAlbedo", 0.402f, 0.01f, 0.0f, 1.0f, "%.3f");
    InspectorHelp("The planet's reflectance, as it enters multi-scattering. UE's default is "
                  "FColor(170) converted from sRGB, which is 0.402 linear -- not 170/255.");
    dragF("Multi-scattering Factor", "multiScatteringFactor", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
    InspectorHelp("Weight on the multi-scattering LUT's contribution. 0 leaves single "
                  "scattering alone, which reads as a much darker sky away from the sun.");
}

void EnvironmentInspector::DrawVolumetricCloud()
{
    // Plan part C. Having this object is what gives the level clouds, as adding UE's
    // VolumetricCloud actor is; it needs the Sky Atmosphere object (the clouds are lit by its
    // distant light and hazed by its aerial perspective) and does nothing on a cubemap sky.
    // Every default below is VolumetricCloudSettings.h's (shown when the level leaves a key out).
    checkB("Enabled", "enabled", true);
    InspectorHelp(
        "A layer of volumetric clouds, traced at half resolution one pixel of each 2x2 block a "
        "frame and rebuilt at full resolution over four frames, with their shadow on the island "
        "and in the fog. Ultra Dynamic Sky's density scheme on textures this engine generates. "
        "Needs the Sky Atmosphere object: on a cubemap sky this stays off and the session log "
        "says so.\n\n"
        "--set=cloud.enabled overrides this for a headless run; every row below is "
        "--set=cloud.<key> too.");

    ImGui::SeparatorText("Clouds");
    dragF("Coverage", "coverage", 0.5f, 0.005f, 0.0f, 1.0f, "%.3f");
    InspectorHelp("How much of the sky is cloud: 0 clear .. 1 closed (Ultra Dynamic Sky's Cloud Density "
                  "/ 3). It also sets how TALL the clouds grow -- light cover is low fair-weather puffs, "
                  "heavy cover fills the layer. The single most visible knob.");
    dragF("Cloud Type", "cloudType", 0.0f, 0.01f, -1.0f, 1.0f, "%.2f");
    InspectorHelp("Flatter (towards -1) or taller (towards +1) clouds at the same coverage: shifts which "
                  "column of the height profile Coverage reads.");
    dragF("Macro Variation", "macroVariation", 0.0f, 0.005f, 0.0f, 1.0f, "%.3f");
    InspectorHelp("Moves Coverage up and down by this much over Macro Tile, so the sky has cloudy "
                  "regions and clear ones instead of an even scatter (and the distance is not one "
                  "continuous bank). Clouds in the cloudy regions are taller too.");
    dragF("Macro Tile (km)", "macroTileKm", 60.0f, 0.5f, 1.0f, 1000.0f, "%.0f");
    InspectorHelp("Size of Macro Variation's cloudy and clear regions.");
    dragF("Overcast From", "overcastFrom", 0.85f, 0.005f, 0.0f, 0.99f, "%.3f");
    InspectorHelp("The Coverage where the clouds start closing into one overcast deck: from here to 1 "
                  "the erosion smooths out, Macro Variation fades and the gaps fill, so Coverage 1 is a "
                  "solid sheet -- and over the last of it the sun leaves the ground (no sunlight, no sun "
                  "shadows, no glints on the water). The clouds' tops stay in the sun.");
    dragF("Overcast Evenness", "overcastEvenness", 0.7f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("How evenly the closed deck covers the sky: 0 keeps the cloud field's thick and thin "
                  "patches and the lumpy floor under the overcast, 1 is one uniform grey sheet with a "
                  "flat base. Acts only as the cover closes (from Overcast From to 1).");
    dragF("Formation Tile (km)", "weatherTileKm", 12.0f, 0.1f, 1.0f, 1000.0f, "%.1f");
    InspectorHelp("The scale of the cloud field: the size of the formation volume on the ground, i.e. "
                  "how far apart the cloud groups sit and how big they are. Ultra Dynamic Sky's is 12 km.");
    dragF("Wind Speed (km/h)", "windKmH", 20.0f, 0.5f, 0.0f, 500.0f, "%.0f");
    InspectorHelp("How fast the whole field drifts along the level's wind heading (the Wind "
                  "object), on the shared clock --wind-freeze pins.");
    dragF("Formation Change Speed", "formationChangeSpeed", 1.0f, 0.01f, 0.0f, 100.0f, "%.2f");
    InspectorHelp("How fast the cloud shapes evolve relative to their drift, as Ultra Dynamic Sky's: 1 "
                  "runs the volume's whole time loop while the clouds travel 16 formation tiles -- at "
                  "30 km/h over a 12 km tile a cloud lives 15-30 minutes. No wind, no change.");
    {
        int seed = static_cast<int>(jsonfield::Value(tgt(), "seed", 0u));
        if (ImGui::DragInt("Seed", &seed, 1.0f, 0, 100000))
        {
            nlohmann::json after = withField("seed", static_cast<unsigned>(std::max(seed, 0)));
            executeChange(std::move(after), historyLabel);
        }
    }
    InspectorHelp("Regenerates every cloud texture from another seed: a different sky, the same weather.");

    // The shape knobs are FINE TUNING: their defaults are Ultra Dynamic Sky's values or fits to
    // its look (VolumetricCloudSettings.h), and a level that leaves them alone looks like that.
    // Folded, so the knobs above are what the panel shows.
    if (ImGui::TreeNodeEx("Fine tuning (shape)", ImGuiTreeNodeFlags_SpanAvailWidth))
    {
        InspectorHelp("The density model's shape knobs. Their defaults reproduce Ultra Dynamic Sky's look; "
                      "change them only to move away from it.");
        dragF("Formation Roughness", "formationRoughness", 0.85f, 0.005f, 0.3f, 0.9f, "%.3f");
        InspectorHelp("The formation field's fBm gain. Lower = smooth, peaked formations: tall, round "
                      "clouds in big groups; higher = flatter, more ragged ones. Regenerates the 32 MB "
                      "volume (a few ms).");
        dragF("Floor Variation Clear", "floorVariationClear", 0.08f, 0.005f, 0.0f, 2.0f, "%.3f");
        InspectorHelp("How much the cloud bases undulate at light cover (UDS Floor Variation Clear): 0 = "
                      "every base on the layer floor, higher lifts some of them.");
        dragF("Floor Variation Cloudy", "floorVariationCloudy", 0.64f, 0.005f, 0.0f, 2.0f, "%.3f");
        InspectorHelp("The same at heavy cover (Coverage above 0.5): broken, ragged undersides.");
        dragF("Erosion Strength", "erosionStrength", 1.2f, 0.01f, 0.0f, 4.0f, "%.2f");
        InspectorHelp("UDS 3D Erosion: how deep the cells volume carves the formations -- the cauliflower. "
                      "0 leaves smooth blobs.");
        dragF("Erosion Power", "erosionPower", 3.0f, 0.05f, 0.25f, 8.0f, "%.2f");
        InspectorHelp("UDS Erosion Power: higher = only the cells' brightest parts carve (rounder bulges "
                      "with sharp gaps), lower = everything erodes a little (softer, fuzzier).");
        dragF("Erosion Tile (km)", "erosionTileKm", 1.2f, 0.01f, 0.1f, 20.0f, "%.2f");
        InspectorHelp("World size of the cells volume (vertically 0.58 of it): the size of the bulges.");
        dragF("Fine Detail Strength", "highFrequencyStrength", 0.24f, 0.005f, 0.0f, 2.0f, "%.3f");
        InspectorHelp("UDS High Frequency Noise: the fine, wispy detail on edges near the camera.");
        dragF("Fine Detail Distance (km)", "highFrequencyDistanceKm", 1.5f, 0.05f, 0.1f, 20.0f, "%.2f");
        InspectorHelp("The fine detail fades out by this distance (it would only shimmer further away).");

        ImGui::SeparatorText("Height profile");
        InspectorHelp("The generated profile LUT: per coverage (column) and height, how far up a formation "
                      "reaches, how dense it is and how much it erodes. Each change regenerates it (cheap). "
                      "--cloud-profile=<png> replaces it for the session.");
        dragF("Profile Height", "profileHeight", 1.0f, 0.01f, 0.2f, 2.0f, "%.2f");
        InspectorHelp("How tall the formations grow at every coverage (1 = heavy cover reaches the top of "
                      "the layer).");
        dragF("Dome", "profileDome", 0.60f, 0.005f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("How fast a formation narrows with height: 0 = flat-topped slabs, 1 = steep domes "
                      "where only the dense core reaches up (towers). Heavy cover flattens it on its own.");
        dragF("Top Softness", "profileTopSoftness", 0.27f, 0.005f, 0.01f, 1.0f, "%.3f");
        InspectorHelp("The fraction of a formation's height its top closes over: low = hard tops.");
        dragF("Base Softness", "profileBaseSoftness", 0.29f, 0.005f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("The height over which the underside gains density: low = crisp flat bases, high = "
                      "soft, hazy ones.");
        dragF("Small Cloud Density", "profileSmallDensity", 0.09f, 0.005f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("Density at no coverage, i.e. of the smallest clouds (applied cubed): low = thin, wispy "
                      "puffs; high = solid little cumulus.");
        dragF("Body Density", "profileBodyDensity", 0.65f, 0.005f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("Density at full coverage (cubed: 0.65 = 0.27 of Extinction); a developed cumulus "
                      "field at coverage 0.45 is 80 % of the way there. Higher = darker cores.");
        dragF("Base Erosion", "profileBaseErosion", 0.20f, 0.005f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("Erosion at the cloud floor: low = flat, firm undersides; high = ragged ones. Heavy cover "
                      "(above ~0.6) never erodes its floor: an overcast deck's underside is smooth.");
        dragF("Erosion Rise", "profileErosionRise", 0.53f, 0.005f, 0.05f, 1.0f, "%.3f");
        InspectorHelp("How far up (a fraction of the formation's top) the erosion reaches full strength: "
                      "low = cauliflower right above the base.");
        dragF("Heavy Cover Erosion Cut", "profileHeavyErosionCut", 0.35f, 0.005f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("How much less heavy cover erodes: smoother overcast.");
        dragF("Heavy Cover Thinning", "profileHeavyThinning", 0.66f, 0.005f, 0.0f, 1.0f, "%.3f");
        InspectorHelp("How much cover above ~0.4 thins with height: a dense base under a softer top.");
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Layer");
    dragF("Bottom Altitude (km)", "layerBottomKm", 0.6f, 0.05f, 0.1f, 20.0f, "%.2f");
    InspectorHelp("Height of the cloud bases above sea level. Trade-wind cumulus over a tropical sea "
                  "sits at ~0.6 km; UE's default 5 km is for a big continental sky.");
    dragF("Layer Height (km)", "layerHeightKm", 0.7f, 0.05f, 0.1f, 20.0f, "%.2f");
    InspectorHelp("Thickness of the layer: the tallest clouds (heavy cover) fill it, light cover uses the "
                  "bottom fifth.");

    ImGui::SeparatorText("Medium");
    dragF("Extinction (1/m)", "extinctionScale", 10.0f, 0.05f, 0.0f, 20.0f, "%.2f");
    InspectorHelp("How dense the cloud is at the height profile's density 1, per metre. The profile's "
                  "density is applied cubed (a developed cloud's ~0.5 is 1/8 of this), so it sits ~8x a "
                  "real cumulus' 0.04-0.1; Ultra Dynamic Sky runs 10. Lower reads as haze, higher as a "
                  "hard-edged wall.");
    dragF("Albedo", "albedo", 0.95f, 0.005f, 0.0f, 1.0f, "%.3f");
    InspectorHelp("Single-scattering albedo of the droplets -- how much of the light a "
                  "particle scatters rather than absorbs. Water is close to 1; lower darkens "
                  "the shadowed sides.");

    ImGui::SeparatorText("Lighting");
    dragF("Phase G", "phaseG", 0.85f, 0.005f, -0.99f, 0.99f, "%.3f");
    InspectorHelp("Henyey-Greenstein anisotropy of the main lobe, POSITIVE forward: the silver lining "
                  "when the sun is behind the cloud. The same sign as UE's material node -- a UE or "
                  "Ultra Dynamic Sky value is entered as is.");
    dragF("Phase G2", "phaseG2", 0.4f, 0.005f, -0.99f, 0.99f, "%.3f");
    InspectorHelp("A second, broader lobe blended in by Phase Blend: a gentler forward glow, or "
                  "(negative) some light thrown back towards the sun-facing side.");
    dragF("Phase Blend", "phaseBlend", 0.65f, 0.005f, 0.0f, 1.0f, "%.3f");
    InspectorHelp("How much of the second lobe is mixed in. 0 = the main lobe alone.");
    dragF("Multi-scatter Contribution", "msContribution", 0.85f, 0.005f, 0.0f, 1.0f, "%.3f");
    InspectorHelp("UE MultiScatteringContribution: how much light each further scattering "
                  "octave adds. Brightens the thick, self-shadowed middle of a cloud.");
    dragF("Multi-scatter Occlusion", "msOcclusion", 0.5f, 0.005f, 0.0f, 1.0f, "%.3f");
    InspectorHelp("UE MultiScatteringOcclusion: how much less each octave is shadowed. "
                  "Lower lets light deeper into the cloud.");
    dragF("Multi-scatter Eccentricity", "msEccentricity", 0.4f, 0.005f, 0.0f, 1.0f, "%.3f");
    InspectorHelp("UE MultiScatteringEclipse: how much each octave keeps the phase's "
                  "direction. Lower makes the extra light more even.");
    {
        int octaves = static_cast<int>(jsonfield::Value(tgt(), "msOctaves", 2u));
        if (ImGui::SliderInt("Multi-scatter Octaves", &octaves, 0, 2))
        {
            nlohmann::json after = withField("msOctaves", static_cast<unsigned>(std::clamp(octaves, 0, 2)));
            executeChange(std::move(after), historyLabel);
        }
    }
    InspectorHelp("UE MultiScatteringApproximationOctaveCount: extra scattering octaves beyond the "
                  "first bounce. Each is shadowed less (Occlusion squared), so the second carries the "
                  "sun's own colour into the base of a dense cloud -- a lighter, greyer underside. "
                  "Ultra Dynamic Sky runs 2.");
    dragF("Sky Light Bottom Occlusion", "skyLightBottomOcclusion", 0.0f, 0.005f, 0.0f, 1.0f, "%.3f");
    InspectorHelp("UE SkyLightCloudBottomOcclusion: how much of the sky's light the bottom of the layer "
                  "loses. 0 lights the underside like the top (Ultra Dynamic Sky), 1 leaves it only the "
                  "sun.");
    dragF("Sky Light Intensity", "skyLightIntensity", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
    InspectorHelp("Scales the blue sky's light on the clouds -- the light their shadowed sides get. "
                  "Higher = lighter AND bluer shading (weaker shadows), lower = darker, greyer "
                  "undersides. 1 = the atmosphere as computed. Under a closing overcast it reaches the "
                  "inside only through the deck.");
    dragF("Light Through Cloud", "diffuseLight", 0.25f, 0.01f, 0.0f, 10.0f, "%.2f");
    InspectorHelp("Sunlight diffused down through a cloud's depth -- what lights the base of a thick "
                  "cloud and an overcast deck from below (thick clouds get darker bases than thin ones). "
                  "0.25 = Ultra Dynamic Sky's cool slate overcast and contrasty bases; 1 = the physical "
                  "estimate (a lighter, warmer deck, paler bases); 0 = none, the scattering octaves alone "
                  "(UE's model): a closed deck is then lit by the blue sky only and reads as a blue sky.");
    dragF("Reflection Horizon Fade (deg)", "reflectionHorizonFadeDeg", 5.0f, 0.1f, 0.0f, 45.0f, "%.1f");
    InspectorHelp("The clouds in the sky's captured picture -- what the WATER reflects (and the probes and "
                  "the fog's sky) -- fade out below this elevation, so the sea along the horizon reflects "
                  "the clear haze instead of blotches of the far cloud band. Not under a closing deck, "
                  "where the horizon is cloud. 0 = keep them all. The clouds on screen are not touched.");

    ImGui::SeparatorText("Tracing");
    {
        int samples = static_cast<int>(jsonfield::Value(tgt(), "viewSampleCountMax", 128u));
        if (ImGui::DragInt("View Samples (max)", &samples, 1.0f, 4, 768))
        {
            nlohmann::json after = withField("viewSampleCountMax", static_cast<unsigned>(std::clamp(samples, 4, 768)));
            executeChange(std::move(after), historyLabel);
        }
    }
    InspectorHelp("Samples along a view ray as long as Sample Distance; the step is that distance "
                  "divided by this, constant for shorter rays (3 km / 128 = 23 m). This is where the "
                  "cost is: 0.43 ms at 128, 0.71 at 256 (2560 x 1440, DLSS Balanced).");
    {
        int samples = static_cast<int>(jsonfield::Value(tgt(), "viewSampleCountMin", 2u));
        if (ImGui::DragInt("View Samples (min)", &samples, 1.0f, 1, 768))
        {
            nlohmann::json after = withField("viewSampleCountMin", static_cast<unsigned>(std::clamp(samples, 1, 768)));
            executeChange(std::move(after), historyLabel);
        }
    }
    InspectorHelp("UE SampleMinCount: no ray takes fewer samples, however short its path through the layer "
                  "-- the step shrinks instead. Ultra Dynamic Sky sets 135 (UE 2). It is what removes the "
                  "\"snow\" beside the sun: wisps thinner than a step, caught by a ray now and then. On "
                  "wind_test (2026-10-14, a ~1.7 km path = ~72 steps at 23 m): specks in the hole by the sun "
                  "5.5 -> 0 a frame in trace mode 0, 15.5 -> 2.4 in mode 1, for +0.12 / +0.19 ms of GPU frame.");
    dragF("Sample Distance (km)", "distanceToSampleCountMaxKm", 3.0f, 0.05f, 0.1f, 1000.0f, "%.2f");
    InspectorHelp("UE DistanceToSampleMaxCount: the ray length View Samples are spread over (UE 15 km). "
                  "Shorter = finer steps, the same count: sharper edges through a thin layer.");
    {
        int samples = static_cast<int>(jsonfield::Value(tgt(), "shadowSampleCount", 4u));
        if (ImGui::DragInt("Sun Shadow Samples", &samples, 1.0f, 1, 80))
        {
            nlohmann::json after = withField("shadowSampleCount", static_cast<unsigned>(std::clamp(samples, 1, 80)));
            executeChange(std::move(after), historyLabel);
        }
    }
    InspectorHelp("Samples of the march towards the sun taken at EVERY lit view sample -- the "
                  "self-shadowing that gives a cloud its dark belly. Multiplies the cost.");
    dragF("Sun Shadow Distance (km)", "shadowTracingDistanceKm", 0.156f, 0.005f, 0.1f, 100.0f, "%.3f");
    InspectorHelp("UE ShadowTracingDistance: how far towards the sun the self-shadow march looks (UE 15 km, "
                  "Ultra Dynamic Sky 0.156). Longer = darker, deeper self-shadow.");
    dragF("Stop Transmittance", "stopTracingTransmittance", 0.005f, 0.0005f, 0.0f, 0.5f, "%.4f");
    InspectorHelp("UE StopTracingTransmittanceThreshold: the ray stops once this little light "
                  "would get through. Higher is cheaper and lets less light punch through.");
    dragF("Tracing Max Distance (km)", "tracingMaxDistanceKm", 50.0f, 0.5f, 1.0f, 1000.0f, "%.0f");
    InspectorHelp("UE TracingMaxDistance: how far past the layer's entry point a ray is followed.");
    dragF("Tracing Start Max (km)", "tracingStartMaxDistanceKm", 350.0f, 1.0f, 1.0f, 10000.0f, "%.0f");
    InspectorHelp("UE TracingStartMaxDistance: a ray whose layer entry is further than this is "
                  "not traced at all.");
    checkB("Temporal", "temporal", true);
    InspectorHelp("Blend each pixel's new sample into its history (reprojected by the cloud's own depth). "
                  "OFF: every pixel shows its latest sample alone -- the trace's noise, unfiltered.");
    dragF("History Weight", "historyWeight", 0.9f, 0.005f, 0.0f, 0.99f, "%.2f");
    InspectorHelp("How much of the history survives each frame: 0.9 is about a ten-frame history, "
                  "lower follows a fast sun or wind more closely at the price of noise.");

    ImGui::SeparatorText("Shadow map");
    checkB("Cloud Shadows", "shadowMap", true);
    InspectorHelp("UE r.VolumetricCloud.ShadowMap: an orthographic map from the sun over the "
                  "ground under the camera, multiplied into the sun wherever it lights the "
                  "scene -- the island, the volumetric fog. This is the cloud's shadow "
                  "crawling over the sand, and it works even when no cloud is in frame.");
    dragF("Shadow Extent (km)", "shadowExtentKm", 20.0f, 0.5f, 1.0f, 500.0f, "%.0f");
    InspectorHelp("Half-width of the map on the ground. 512 texels across twice this: 20 km "
                  "is 78 m a texel. UE default 150 km (585 m a texel) is for a continent.");
    dragF("Shadow Strength", "shadowStrength", 1.0f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("UE CloudShadowStrength: scales the optical depth the map reports. 1 = the "
                  "cloud's own density.");
    dragF("Ocean Body Shadow", "oceanBodyShadow", 0.35f, 0.01f, 0.0f, 1.0f, "%.2f");
    InspectorHelp("How much of the cloud shadow the WATER BODY takes; the sun glints and the foam "
                  "always lose the sun. 0 = the water's own light ignores the cloud, 1 = it darkens "
                  "with the sun like the land. Under broken cloud the bright cloud sides keep the "
                  "sea lit far more than the shadow map alone says.");
    dragF("Shadow Horizon Fade (deg)", "shadowHorizonFadeDeg", 6.0f, 0.1f, 0.0f, 45.0f, "%.1f");
    InspectorHelp("The cloud shadow on the ground and the water fades out as the sun (or the moon) goes "
                  "down: full from this height, none below 1 degree (the volumetric fog keeps all of it). "
                  "A light at the horizon throws a cloud's shadow tens of km, and every small turn of it "
                  "moves the shadow by up to a kilometre: under it the glitter path flickered. "
                  "0 = the shadow at any height (UE).");
    dragF("Shadow Snap (km)", "shadowSnapKm", 2.0f, 0.1f, 0.01f, 100.0f, "%.2f");
    InspectorHelp("UE ShadowMap.SnapLength: the map's centre moves in steps of this, so the "
                  "shadow does not shimmer as the camera walks.");
    dragF("Shadow Depth Bias (km)", "shadowDepthBiasKm", 0.0f, 0.01f, -5.0f, 5.0f, "%.2f");
    InspectorHelp("UE CloudShadowDepthBias: pushes the recorded cloud front along the light.");
    {
        int samples = static_cast<int>(jsonfield::Value(tgt(), "shadowMapSampleCount", 16u));
        if (ImGui::DragInt("Shadow Map Samples", &samples, 1.0f, 4, 128))
        {
            nlohmann::json after = withField("shadowMapSampleCount", static_cast<unsigned>(std::clamp(samples, 4, 128)));
            executeChange(std::move(after), historyLabel);
        }
    }
    InspectorHelp("Samples per map texel through the layer (UE base 16, doubled towards the horizon).");
}

void EnvironmentInspector::DrawSkybox()
{
    // BOTH ROWS BELOW MEET IN ONE LINE, `Skybox::GetExposure()`:
    //     return HasEnvironment() ? 1.0f : exposure_ * PhysicalScale();
    // `exposure_` is Intensity, `PhysicalScale()` is Sky Illuminance / the cube's own
    // measured up-illuminance. When the procedural atmosphere supplies the environment --
    // `sky.mode` on AND `environmentLighting` on, the exact gate in
    // `SkyAtmosphere::BuildEnvironment` -- their product is discarded and the whole sky,
    // its ambient fill, its reflections and the fog's sky term run at a hard 1.0.
    //
    // Asking the Skybox itself rather than re-deriving the condition here: this is the same
    // object `GetExposure()` branches on, so the greying cannot drift from the behaviour.
    // Measured before shipping the grey: `--set=light.skyIlluminanceLux` 12000 vs 40000 on
    // a procedural level moved the water by 0.007 % of pixels against a 0.007 % same-build
    // noise floor, and the frame median by 0.05 of a code.
    const Skybox* liveSky = ctx.scene.GetSkybox();
    const bool skyInert = liveSky != nullptr && liveSky->HasEnvironment();
    if (skyInert)
    {
        ImGui::TextDisabled("Procedural sky owns the environment -- the two rows below do nothing.");
        InspectorHelp(
            "The Sky Atmosphere object is on with Environment Lighting enabled, so it "
            "supplies this level's environment cubes and `Skybox::GetExposure()` returns a "
            "hard 1.0. Intensity and Sky Illuminance are multiplied into that value and "
            "nowhere else, so neither changes anything -- including the drawn sky.\n\n"
            "What to use instead: Sky Atmosphere > Sky Luminance Scale for how bright the "
            "sky reads, and the directional light's Sky Fill Intensity for how hard it "
            "lights the scene.\n\n"
            "Turn Environment Lighting off (or delete the Sky Atmosphere object) and these "
            "two come back, because the level is then lit by the cubemap again.");
    }
    ImGui::BeginDisabled(skyInert);
    dragF("Intensity", "intensity", 1.0f, 0.01f, 0.0f, 8.0f, "%.3f");
    InspectorHelp(
        "How bright this level's sky is, on the engine's linear scale. 1 = the cubemap's "
        "own radiance, untouched.\n\n"
        "It exists because HDRI libraries are not calibrated to our scale. Measured on "
        "citrus_orchard_puresky_4k: the sky's median luminance is 0.657, and the default "
        "manual exposure multiplier is 1.44, which puts 48.6% of the sky above 1.0 BEFORE "
        "the tone curve runs. No curve can undo that -- it needs less light, not a "
        "different shoulder. Around 0.45 brings that sky back under the knee.\n\n"
        "Auto exposure used to hide this by metering it away. That is exactly why it must "
        "not be the only thing holding the image together: turn adaptation off and the "
        "scene should still be photographable.");

    // P16.3b. Logarithmic for the same reason the sun's row is: the useful range runs from
    // a few hundred lux to twenty thousand.
    dragF("Sky Illuminance (lux)", "illuminanceLux", 0.0f, 1.0f, 0.0f, 50000.0f,
          "%.0f", ImGuiSliderFlags_Logarithmic);
    InspectorHelp(
        "How much light this sky puts on a HORIZONTAL surface, in lux. It is the sky's half "
        "of the same physical scale the sun's illuminance uses, and it is what puts the two "
        "on one ruler.\n\n"
        "12000 clear sky with the sun about 30 degrees up, 20000 clear sky with the sun "
        "overhead, 10000-20000 heavy overcast (where the sky IS the light), 2000 deep "
        "twilight.\n\n"
        "0 = NOT AUTHORED, and then nothing changes: the sky keeps the level's old "
        "behaviour, scaled only by Intensity above. That is what keeps an unconverted "
        "level rendering exactly what it renders today.\n\n"
        "The scale that realises the number is DERIVED, never authored -- the engine reads "
        "this sky's own irradiance cube and divides -- so the same 12000 means the same "
        "thing on a cube that came out of the importer bright and one that came out dim. "
        "Check the Session log window's [ibl] line for the measured value and the factor it produced.\n\n"
        "Needs the F7 IBL siblings (_spec/_diffuse). A sky imported without them has "
        "nothing to measure and this row does nothing.");

    // P16.6b -- WHAT TO PUT IN IT. Switching skyboxes should not mean guessing a
    // four-figure number, and the answer does not come from the sky asset: it comes from
    // WHERE THE SUN IS. The clear-day model is the one the levels were converted with, so
    // the recommendation and the content agree by construction.
    {
        const Math::float3 sunDir =
            ctx.scene.GetDirectionalLight().GetDirection().Normalized();
        const float sinE = std::clamp(-sunDir.y, 0.02f, 1.0f);
        const float sunPerp = 128000.0f * std::exp(-0.21f / sinE);
        const float f = 0.32f - 0.18f * sinE;
        const float recommended = f / (1.0f - f) * sunPerp * sinE;
        const float elevDeg = std::asin(sinE) * 180.0f / 3.14159265f;
        ImGui::TextDisabled("Sun is %.0f deg up -> a clear sky delivers about %.0f lx",
                            elevDeg, recommended);
        ImGui::SameLine();
        if (ImGui::SmallButton("Use it"))
        {
            nlohmann::json after = withField("illuminanceLux", recommended);
            executeChange(std::move(after), historyLabel);
        }
        InspectorHelp(
            "The number depends on the SUN, not on which photograph you picked: a clear sky "
            "at 30 degrees delivers about 12000 lx, at the horizon a tenth of that.\n\n"
            "AN OVERCAST SKY IS THE OTHER CASE ENTIRELY -- there the sky IS the light, so "
            "use 10000-20000 AND take the directional sun down to near zero. Pasting a "
            "bright sun over a photograph of an overcast evening is what makes the ground "
            "come out brighter than the sky above it.\n\n"
            "The import log says which kind you have: 'sky sun REMOVED ... the sun was N%%' "
            "for a clear sky, 'NOT REMOVED ... no sun to take out' for an overcast one.");
    }
    // The Texture Asset row below stays LIVE even when the two above are greyed: a
    // procedural level still needs a cubemap named here, because `JsonLevel` only creates
    // the Skybox object -- the thing that draws the atmosphere -- when one is.
    ImGui::EndDisabled();
    const std::string current = jsonfield::Value(tgt(), "texture", std::string());
    const std::string currentLabel = current.empty()
        ? std::string("(none)")
        : std::filesystem::path(current).filename().string();

    if (ImGui::BeginCombo("Texture Asset", currentLabel.c_str()))
    {
        int visibleTextures = 0;
        for (const EditorAssetRecord& rec : registry.Assets())
        {
            if (rec.id.type != EditorAssetType::Texture ||
                !rec.texture.valid ||
                rec.texture.kind != EditorTextureKind::TextureCube)
            {
                continue;
            }
            // F7 ships SERVICE cubes beside every sky -- the GGX-prefiltered `_spec` and
            // the cosine-convolved `_diffuse`. They are cubemaps, so they pass the filter
            // above, but choosing one as the level's sky is never what anyone wants: the
            // first is a blurred stack indexed by roughness, the second is a 32^2 blob.
            // Hide them; the runtime finds them from the display cube's name by itself.
            if (EndsWithSuffix(rec.id.key, "_spec.dds") ||
                EndsWithSuffix(rec.id.key, "_diffuse.dds"))
            {
                continue;
            }

            const bool selected = rec.id.key == current;
            std::string label = rec.id.key;
            if (!rec.texture.format.empty())
            {
                label += "  [";
                label += rec.texture.format;
                label += "]";
            }
            ++visibleTextures;
            if (ImGui::Selectable(label.c_str(), selected) && !selected)
            {
                nlohmann::json after = withField("texture", NormalizePath(rec.id.key));
                executeChange(std::move(after), "Set Skybox Texture");
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        if (visibleTextures == 0)
        {
            ImGui::TextDisabled("No cubemap textures found.");
        }
        ImGui::EndCombo();
    }

    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", jsonfield::Value(tgt(), "texture", std::string()).c_str());
    const float applyButtonWidth = ImGui::CalcTextSize("Apply Texture").x +
        ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(std::max(120.0f,
        ImGui::GetContentRegionAvail().x - applyButtonWidth - ImGui::GetStyle().ItemSpacing.x));
    const bool textureChanged = ImGui::InputText("Texture", buf, sizeof(buf));
    beginContinuousEdit(textureChanged);
    if (textureChanged)
    {
        tgt()["texture"] = NormalizePath(buf);
        ctx.document.SetDirty(true);
    }

    const bool textureCommitted = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    const bool applyTexture = ImGui::Button("Apply Texture");
    if (textureCommitted && activeEditObject.value == env.id.value)
    {
        commandStack.Execute(ctx, std::make_unique<EditEnvironmentCommand>(
            env.id,
            propertiesBeforeEdit,
            props,
            "Set Skybox Texture"));
        activeEditObject = EditorObjectId{};
    }
    else if (applyTexture)
    {
        EnvironmentRuntime::Apply(ctx, env);
    }
}

#endif // WITH_EDITOR
