#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/math/Math.h"
#include "ocean/OceanSimulationInputs.h"
#include "ocean/OceanSimulationSettings.h"
#include "core/threading/Knob.h" // game thread plan G5: knobs with a game twin

struct OceanRenderConfig
{
    Math::float4 deepScatterColor = Math::float4(0.0f, 0.012745098f, 0.04019608f, 1.0f);
    Math::float4 sssColor = Math::float4(0.13333334f, 0.9411765f, 0.6039216f, 1.0f);
    Math::float4 diffuseColor = Math::float4(0.0f, 0.025490196f, 0.02745098f, 1.0f);
    Math::float4 foamTint = Math::float4(1.0f, 1.0f, 1.0f, 1.0f);

    float specularStrength = 1.1f;
    // How much of the UNRESOLVED slope variance (Bruneton, `roughnessScale`) widens the SUN's glint (OceanSunSpecular).
    // 0 = the glitter path alone: each resolved wave facet mirrors the disc (UE's sphere light), the path made of
    // sparkles as in photographs; 1 = the old glint, smeared by that variance through GGX's long tails into a broad
    // smooth patch (the sun's ~1e9 cd/m2 makes the tails visible tens of degrees out). The sky's reflection keeps its
    // blur either way -- it is the honest reflection. Owner, 2026-11-03: "we only need the path -- or a knob to fade it";
    // then, at 0: "fix the water's specular back, bring the path back" -- 1 (the path as it was) is the default.
    float sunGlintSmear = 1.0f;
    float roughnessScale = 0.7f;
    float roughnessDistance = 150.0f;
    float horizonFogStrength = 0.55f;

    float surfaceRefractionStrength = 0.35f;
    float underwaterRefractionStrength = 0.75f;
    float absorptionDepthScale = 10.0f;
    float fogDensity = 0.1f;

    float sunScatterStrength = 0.35f;
    float skyScatterStrength = 0.35f;
    float scatterSpread = 0.2f;
    float viewAlignmentStrength = 0.8f;

    float sssHeightBias = 0.0f;
    float sssFadeDistance = 6.0f;
    float horizonFogDistanceScale = 2.5f;
    // How far the water's reflection normal is pulled toward +Y (1 = a flat mirror). ONE normal for both
    // halves of the reflection: the sky sample and, since the ocean G-buffer plan's S3, the screen-space
    // trace (GB1). Owner's call (2026-10-10): one knob, 0.1. Measured on wind_test's lagoon: higher
    // values bring back the far beach's reflection (0.5 ~ the old plane trace, 1 a crisp mirror) but
    // also flatten the sky's ripples.
    float reflectionNormalStrength = 0.1f;
    // Horizon pull for the SKY reflection ray only (skyParams.w -> OceanSkyReflectDir). 1 = off,
    // which is the shipped default so an existing level looks byte-identical; below 1 compresses
    // the ray's Y so the water samples the band near the horizon instead of swinging into the
    // zenith. It exists because a clear-sky HDRI's dark zenith, sampled across a wave's facets,
    // reads as hard dark streaks along the crests — and IblClampToSharp's one-sided guard cancels
    // the prefilter blur exactly there (full reasoning in ocean_surface_surf_sim.hlsli). ~0.4-0.6 is
    // the useful range; the screen-space reflection and the land IBL are untouched.
    float reflectionSkyHorizonPull = 1.0f;
    // How much of the sun still lights the water BODY (deep scatter, subsurface glow, diffuse) when
    // the sun sits on the horizon -- the floor of SunBodyEntry (ocean_surface_common.hlsli), which
    // fades the sun's share below 12 degrees by what actually gets into the water. 0 = the physical
    // fade alone (at 2 degrees the body keeps 4 % of the sun: a dark sea, which the owner found
    // "черезчур погасил"), 1 = off (the old full-strength green glow at sunset). Nothing changes for
    // a sun at 12 degrees or higher, whatever this says. 0.1 from a 2-degree sweep of 0-0.35: the
    // body keeps a teal tint without glowing; by 0.2 the near waves are green again.
    float lowSunBodyLight = 0.1f;
    float cascadeFadeScale = 20.0f;
    float minMeshScale = 15.0f;
    float detailNormalMipBias = 0.0f;
    float macroNormalMipBiasDlss = 0.0f;
    float macroNormalMipBiasNative = 1.0f;

    // The LEGACY surface's authored shore knobs. Ignored by the modern run-up surface.
    // Contact foam strength = foamParams2.y in the June-22 shader, which hardcoded 0.1.
    float shoreLegacyContactFoamStrength = 0.1f;
    // Nearshore damping, split from the June hardcode `saturate(depth * 0.15)` on the whole
    // displacement: separate vertical / XZ strengths (1 = fully still at the waterline) and the
    // water depth where the damping starts biting. Defaults reproduce the original curve.
    float shoreLegacyVerticalDampStrength = 1.0f;
    float shoreLegacyXzDampStrength = 1.0f;
    float shoreLegacyDampFadeDepth = 6.67f;
    // The legacy contact-foam TAIL (the ContactFoam.dds strip): texture scale in tiles per metre,
    // the depth-difference reach it survives to, its wind-drift speed, and the second-octave
    // de-tiling mix. Edge fade reuses shoreEdgeSoftDepth; albedo reuses the shore foam albedo
    // scale/scroll. Defaults (1 / 0.2 / 0 / 0) reproduce the June behaviour.
    float shoreLegacyTailTextureScale = 1.0f;
    float shoreLegacyTailDepth = 0.2f;
    float shoreLegacyTailScrollSpeed = 0.0f;
    float shoreLegacyTailDetile = 0.0f;
    float shoreLegacyTailEdgeFade = 0.1f;
    // Remap of the tail texel before the coverage math: contrast around mid-grey and a brightness
    // bias. They shape the dissipation length (how far bright tongues outrun dark ones): a texel
    // of brightness t survives to TailDepth / (1 - t), so pulling the top of the distribution
    // down gives the tail a finite dissipation depth. Defaults (1, 0) = identity = June.
    float shoreLegacyTailContrast = 1.0f;
    float shoreLegacyTailBias = 0.0f;
    // Foam dissipation (docs/ocean_shore_foam_breakup_plan.md, variant A): a slow large-scale
    // spatio-temporal field that squeezes the tail's depth threshold so patches of the shoreline
    // band decay and regrow in place instead of standing as a constant-width rim. Amount 0 = OFF
    // = exact identity.
    float shoreLegacyDissipationScale = 25.0f;
    float shoreLegacyDissipationSpeed = 0.5f;
    float shoreLegacyDissipationAmount = 0.0f;
    float shoreLegacyDissipationContrast = 2.0f;
    // Variant C of the same plan (variant B, the surf phase, was rejected - see the doc): wind
    // thinning scales the tail threshold by the shared contact-foam wind amount, so the band
    // starves at dead calm (0 = OFF).
    float shoreLegacyWindThinning = 0.0f;
    // Nearshore surf simulation (docs/ocean_surf_sim_plan.md). OFF = zero cost: no compute
    // dispatches recorded and no sim resources allocated (the detachability contract).
    bool surfSimEnabled = false;
    // S2 spawner knobs: segments of disturbance born seaward of the waterline, oriented along
    // the shore. Wind coupling scales BOTH the amplitude and the spawn cadence, so a dead calm
    // starves the surf (the plan's invariant 3).
    float surfSimSpawnDistance = 40.0f;  // metres seaward of the waterline
    float surfSimSegmentLength = 30.0f;  // metres along the shore
    // The TIME INTEGRAL of the injection, not the peak: the packet spreads while it inflates,
    // so the crest that actually arrives is a fraction of this. Tuned against the breaker
    // trigger, not against textbook wave heights.
    float surfSimWaveAmplitude = 0.8f;   // metres injected per segment at full wind
    float surfSimSpawnInterval = 3.0f;   // seconds between spawns at full wind
    float surfSimWindCoupling = 1.0f;    // 0 = ignore wind, 1 = calm silences the spawner
    // S3 breaking + foam: deposit where a wave exceeds the surf breaker index (H/d), torn by
    // the shared breakup pattern, dissipating exponentially.
    // Gamma below the textbook 0.78: the linear wave equation has no shoaling (Green's law), so
    // heights do NOT grow over the shelf the way real waves do — the trigger must meet the sim's
    // waves where they actually are.
    float surfSimDepositStrength = 1.0f; // peak foam a breaking crest stamps (max() semantics)
    float surfSimBreakerGamma = 0.5f;    // H/d breaker index (sim-tuned; McCowan is 0.78)
    float surfSimFoamFadeTime = 2.5f;    // seconds a full-strength foam stamp takes to dissolve
    float surfSimFoamCoverageMultiplier = 1.0f; // final visible coverage, after breakup and window fade
    float surfSimFrontBreakup = 0.5f;    // 0..1 tear of the fresh leading front
    float surfSimTailBreakup = 0.7f;     // 0..1 tear of the decaying tail
    float surfSimTearScale = 7.0f;       // metres, patch scale of the tear pattern
    float surfSimDisplacement = 1.0f;    // 0..2, sim wave height -> vertex displacement scale
    float surfSimRunInland = 2.0f;       // metres past the SDF waterline the wave may live
    float surfSimMinSpawnDepth = 1.2f;   // metres of bottom required under a disturber to be born
    float surfSimCapWidth = 1.0f;        // dense-zone width: >1 wider, <1 narrower (age-curve power)
    float surfSimWaveSigma = 4.0f;       // m, across-shore half-width of the injected hump (wavelength lever)
    float surfSimSpawnDuration = 1.0f;   // s the hump inflates (shorter = more compact packet)
    float surfSimCelerityFloor = 0.4f;   // m, min depth for wave speed - bore march vs shore compression
    float surfSimBreakOnset = 0.5f;      // fraction of the breaker criterion where foam starts (cap width lever)
    float surfSimWaveDamping = 0.08f;    // 1/s open-water settle
    // A boat's wake (ocean/OceanWake): the field of each hull's pressure moving over deep water. Runs only while a boat is in
    // the world; OFF = no dispatch, nothing read.
    bool wakeEnabled = true;
    float wakeStrength = 1.0f;     // the hull's pressure: 1 = the head of water its draft stands for (physics)
    float wakeDisplacement = 1.0f; // the field's height in the surface's vertices (0..2)
    float wakeNormal = 1.0f;       // the field's slope in the surface's normal (1 = the true slope, 0..3)
    float wakeDamping = 1.5f;      // 1/s at the field's shortest wave, as k^2 to the longer ones
    float wakeBreaking = 2.0f;     // 1/s (Crest's rate): a long wave in the shallows, d < wavelength / 20, runs down at it
                                   // as d goes to 0 -- the surf zone a beach takes it in; 0 = none, the waterline
                                   // reflects it whole (0..20)
    float wakeTexel = 0.5f;        // m a texel; 512 make the window round the camera (0.25..2)
    float wakeFoam = 1.0f;         // the wake's foam on the surface (0 = none, 0..2)
    float wakeFoamFade = 0.1f;     // 1/s the wake's foam lace runs down at (0.02..5)
    float wakeFoamFadeDense = 1.5f; // 1/s per unit of foam on top: dense white water runs down fast (0..10)
    float wakeWashFoam = 4.0f;     // the white water a moving hull leaves, a metre of head change under it (0..20)
    float wakeCrestFoam = 1.0f;    // Crest's Wave Foam Strength on the wake's steep crests (0..5)
    float wakeCrestCoverage = 0.55f; // Crest's Wave Foam Coverage: det J under it breaks (0.55 = the Stokes limit, 0..1)
    float wakeChurn = 1.0f;        // the churned white water's relief: lumps and grey cavities (0 = a smooth sheet, 0..1)
    float wakeFoamTile = 3.0f;     // m of water a tile of the wake's foam texture covers: the lace's scale (its largest
                                   // cells a fifth of it); drawn without a repeat (0.2..16)
    // The breaking bow wave: the wake's two foam arms (OceanWake's fronts), from the bow's shoulders outward.
    float wakeBowFoam = 1.0f;      // their foam: 1 = a fresh front of a planing hull leaves white water (0 = no arms, 0..4)
    float wakeBowSpread = 1.0f;    // how fast they run outward: 1 = the diverging wave's own speed, the arms on its crest (0.25..4)
    float wakeBowLife = 2.5f;      // s: a front's breaking runs down e-fold in this -- the arms' white length (0.2..10)
    float wakeBowLace = 0.15f;     // the share of a breaker's foam the water keeps once it has passed: the lace between the
                                   // arms and the trail (0 = clear water there, 1 = a solid wedge, 0..1)
    float wakeBowThrow = 0.09f;    // s of the hull's speed the chine's sheet carries a front off the waterline at its birth:
                                   // the arms' shoulder (1.4 m at 16 m/s; 0 = they start on the hull, 0..0.3)
    float wakeBowWidth = 0.9f;     // m: the breaker band's half width where it leaves the hull (0.3..4)
    float wakeBowWiden = 0.25f;    // m/s the half width grows as the front runs on (0..2)
    float wakeBowScallop = 0.2f;   // the outer edge's raggedness: a leaving point's speed is 1 +- this x a short random walk (0..1)
    float wakeBowSpeedFull = 12.0f; // m/s of the hull at which the arms are whole (they start at 3; 4..30)
    // The wake foam's look on the surface (ocean_surface_surf_sim.hlsli WakeFoamCoverage) and in its field.
    float wakeFoamSpread = 0.3f;   // m2/s the laid foam spreads sideways at (a trail 2-3 m wider after 10 s; 0..1)
    float wakeAeration = 0.4f;     // the bubbles under the foam: the light turquoise scar's strength (0..1)
    float wakeFoamStretch = 3.5f;  // the lace is drawn out this many times along the way it was laid (1 = round, 1..8)
    float wakeFoamSoftness = 0.14f; // the lace's edge between foam and water, in the gaussian's units (0.02..1)
    float wakeFoamBoil = 0.06f;    // m/s the pattern's patches slide against each other (0 = frozen, 0..0.5)
    float wakeFoamTileVary = 0.15f; // each patch of the pattern (a hexagon of its tiling) is drawn at the tile x (1 +- this) (0..0.4: past it a patch would show its tile twice)
    float wakeFoamCap = 0.93f;     // the most of the water the densest foam covers: under 1 solid white water keeps holes (0.5..1)
    float wakeChurnSize = 4.0f;    // m of water a tile of the churn's relief covers, its own scale (at least wakeFoamTile; 0.5..16)
    // A boat's spray (vfx/VesselSpray): lit drops off a planing hull's chines, every boat's.
    bool sprayEnabled = true;
    float sprayRate = 0.7f;        // of what the presets can keep alive (0..1)
    float sprayMist = 1.0f;        // the mist's share of that (0..4)
    float sprayOut = 0.4f;         // a drop's speed outward, a share of the hull's (earth frame, 0..2)
    float sprayUp = 0.2f;          // and upward (0..2)
    float sprayForward = 0.1f;     // and along the hull's way (-1..1)
    float spraySlam = 1.0f;        // the burst a slam into a wave throws (1 = the presets' numbers, 0 = none, 0..4)
    Math::float4 sprayColor = Math::float4(0.95f, 0.95f, 0.95f, 1.0f); // the drops' albedo (x foamTint), under the sun and the sky
    float sprayFoamLight = 1.0f;   // lit as a level sheet of the water's foam (1) or as spheres facing every way (0) (0..1)
    float sprayForwardScatter = 0.25f; // the sun's share in the forward lobe (glows against the sun); the rest is diffuse (0..1)
    float sprayOpacity = 1.0f;     // times the presets' alpha keys (each key at most 1): the water shows through less (0..3)
    float spraySoftFade = 0.4f;    // m: a drop fades out over this much before the water's surface (the mist over 2.5x it) (0..2)

    float shoreVerticalFadeDepth = 1.25f;
    float shoreHorizontalMin = 0.65f;
    float shoreHorizontalFadeDepth = 2.0f;
    float shoreNormalFadeDepth = 2.0f;
    Math::float4 shoreNormalMinWeights = Math::float4(0.25f, 0.45f, 0.75f, 1.0f);
    float shoreRunupDepth = 2.0f;
    float shoreRunupStrength = 1.5f;
    float shoreRunupMaxWave = 0.8f;
    float shoreRunupSlopeStartDegrees = 25.0f;
    float shoreRunupSlopeEndDegrees = 55.0f;
    // Swash: the horizontal shuttle of the splash-zone water with the arriving wave — a material
    // motion of the vertices (the wet edge rides along), not a mask. A LOW-WIND device: the
    // excursion peaks in the light-to-mid wind band (ramping in above the calm threshold, fading
    // out quadratically toward full wind), because a big sea's own run-up push already moves the
    // shoreline and stacking the shuttle on top overdrives the waves, while below the calm
    // threshold there is no surf at all. This slider scales it 0..1.
    float shoreSwashAmplitude = 0.85f;
    // Baseline (in shore-map texels, ~1 m each) of the centred difference that reads the beach
    // slope for the run-up gate. THE tooth-count control: a short baseline reads texel noise and
    // cuts many small teeth; a long one reads the beach's real shape and leaves a few wide bays.
    float shoreRunupSlopeSmoothing = 1.5f;
    float shoreBottomClearance = 0.05f;
    float shoreEdgeSoftDepth = 0.03f;
    float shoreGeometryEdgeRefractionFadeDepth = 0.1f;
    float shoreGeometryFadeDistance = 150.0f;
    float shoreContactFoamMainWidth = 0.1225f;
    float shoreContactFoamBreakupLength = 0.05f;
    float shoreContactFoamBreakupLengthVariation = 0.05f;
    float shoreContactFoamBreakupVariationScale = 0.04f;
    float shoreContactFoamDepthWarpStrength = 0.03f;
    float shoreContactFoamDepthWarpRange = 0.25f;
    float shoreContactFoamDepthWarpScale = 0.098f;
    float shoreContactFoamPatternScrollSpeed = 0.2f;
    float shoreContactFoamAlbedoScale = 0.15f;
    float shoreContactFoamAlbedoScrollSpeed = 0.2f;
    float shoreContactFoamOpacity = 0.8f;
    float shoreContactFoamCalmAmount = 0.1f;
    float shoreContactFoamFullWindForce = 0.6f;
    float shoreContactFoamNormalStrength = 1.0f;
    float shoreContactFoamPatternScale = 0.28f;
    float shoreContactFoamPatternDensity = 0.55f;

    // Wet sand is a persistent world-space history stamped by the visible ordinary ocean sheet.
    // It is independent of contact foam and SurfSim: it approaches the stamp over Wet Time; when
    // the sheet retreats, the terrain remains dark/specular for Dry Time and then returns dry.
    float shoreWetnessDepositDepth = 0.35f;
    float shoreWetnessWetTime = 0.75f;
    float shoreWetnessDryTime = 18.0f;
    float shoreWetnessDarkening = 0.35f;
    float shoreWetnessReflectionStrength = 0.35f;
    // Legacy water fades visually before its geometry ends. Keep the wet stamp this many metres
    // seaward from the authored SDF waterline so it follows the visible edge instead of leading it.
    float shoreWetnessEdgeOffset = 0.5f;
    // Reject the XZ-projected history on steep terrain and vertical walls.
    float shoreWetnessMaxSlopeDegrees = 65.0f;
    // Outside the camera-centred wetness history, keep distant terrain wet by height relative to
    // the still ocean plane. The two extents are independent because a beach usually needs only a
    // thin wet rim above sea level but a deeper submerged band below it. Fade Start is a percentage
    // of either selected extent: the result stays fully wet before it, then falls linearly to zero
    // exactly at Above/Below. Setting both extents to zero disables the fallback.
    float shoreWetnessFallbackAboveWater = 0.35f;
    float shoreWetnessFallbackBelowWater = 1.5f;
    float shoreWetnessFallbackFadeStartPercent = 75.0f;
    // World-anchored XZ breakup of the ABOVE-water outer edge. Strength is the maximum fraction
    // removed from Above Water (never added beyond it); Scale is metres per broad noise cell.
    float shoreWetnessFallbackBreakupStrength = 0.35f;
    float shoreWetnessFallbackBreakupScale = 8.0f;

    float windSpeed = 12.0f;
    float wavesScale = 1.0f;
    float windAlignment = 0.5f;
    float windUvWarpStrength = 0.2f;

    float foamNormalStrength = 0.6f;
    float underwaterFoamParallax = 1.6f;

    // --- Caustics -----------------------------------------------------------------------------
    // Sampled in the deferred lighting pass (lighting_cs.hlsl) for every surface below the water
    // line, so they land on the lagoon floor, the submerged beach and any prop under water, and
    // they inherit the sun shadow for free. Off when there is no ocean in the level.
    bool  causticsEnabled = true;
    std::string causticsTexture = "textures/ocean/caustics_flipbook.dds";
    int causticsColumns = 4;
    int causticsRows = 4;
    int causticsFrameCount = 0; // 0 = use the entire grid, in row-major order
    float causticsIntensity = 2.6f;     // gain added to the direct sun term at a cord
    float causticsScale = 6.0f;         // metres per tile; the pattern is ~16 cells wide, so this
                                        // sets the cell size (6 m -> ~37 cm cells)
    float causticsSpeed = 10.0f;        // flipbook frames per second
    float causticsDepthFade = 14.0f;    // metres below the surface over which it fades to nothing
    float causticsSurfaceFade = 0.4f;   // metres of fade-in right under the surface (kills the waterline seam)
    float causticsUpFacing = 0.7f;      // 0 = ignore the normal, 1 = full N.up gate
    // The flipbook stores ray DENSITY, so there is one physically right value here: whatever
    // unfocused sunlight encodes to (tools/gen_caustics.py prints it). At that setting the dark
    // cells stay neutral and only the cords add light.
    float causticsBias = 0.186f;
    float causticsDispersion = 0.25f;   // chromatic split in texels (0 = monochrome, and 3x cheaper)
    float causticsLayerBlend = 0.6f;    // second de-tiling layer, min-combined (0 = single layer)
    Math::float4 causticsTint = Math::float4(0.85f, 1.0f, 0.95f, 1.0f);

    float absorptionGradientType = 0.0f;
    std::vector<Math::float4> absorptionColors = {
        Math::float4(0.0f, 0.041025557f, 0.094412796f, 0.0f),
        Math::float4(0.0f, 0.17351386f, 0.43203577f, 0.2f),
        Math::float4(0.16198544f, 0.68352747f, 0.79865986f, 0.66608685f),
        Math::float4(1.0f, 1.0f, 1.0f, 1.0f)
    };
};

struct OceanSimulationConfig
{
    OceanSimulationSettings settings;
    OceanRenderConfig render;

    float localWindDirectionDegrees = 0.0f;
    float swellDirectionDegrees = 0.0f;
    float windForce01 = 0.0f;

    OceanSimulationInputsProvider::InputsProviderMode inputMode = OceanSimulationInputsProvider::InputsProviderMode::Fixed;
    float timeScale = 1.0f;
    float depth = 1000.0f;

    std::shared_ptr<EqualizerPreset> defaultEqualizer = EqualizerPreset::CreateDefault();
    std::shared_ptr<SwellPreset> swellPreset = std::make_shared<SwellPreset>();
    std::shared_ptr<LocalWavesPreset> localPreset = std::make_shared<LocalWavesPreset>();
    std::vector<std::shared_ptr<LocalWavesPreset>> localPresets;
    size_t localPresetIndex = 0;
};

namespace ocean
{
// "--ocean-wind=<0..1>": boot-time override applied AFTER the level's own wind sources (inline
// block and wind entity both), so a headless capture can be taken at a chosen sea state — the
// artifacts this exists to check only show up in a swell. Negative = no override. Diagnostic only;
// never written back to any config.
inline float g_windForceOverride = -1.0f;
inline float g_windForceOverride_game = -1.0f;
inline const bool g_windForceOverride_knob = ::knob::Register(g_windForceOverride_game, g_windForceOverride, "ocean::g_windForceOverride");

// "--ocean-geomfade=<metres>": override shoreGeometryFadeDistance for a headless capture without
// touching the level file. Negative = no override. Diagnostic only.
inline float g_geometryFadeOverride = -1.0f;
inline float g_geometryFadeOverride_game = -1.0f;
inline const bool g_geometryFadeOverride_knob = ::knob::Register(g_geometryFadeOverride_game, g_geometryFadeOverride, "ocean::g_geometryFadeOverride");
}

OceanSimulationConfig CloneOceanSimulationConfig(const OceanSimulationConfig& config);
bool LoadOceanSimulationConfigFromFile(const std::wstring& path, OceanSimulationConfig& outConfig);
bool SaveOceanSimulationConfigToFile(const std::wstring& path, const OceanSimulationConfig& config);
