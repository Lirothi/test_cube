#pragma once

#include <cstdint>

// Volumetric clouds, part C of docs/volumetric_fog_sky_clouds_ssgi_plan.md. AUTHORED BY THE LEVEL
// (the `volumetricCloud` object): a level with the section has clouds, one without has none -- the
// same shape as UE's VolumetricCloud actor and as our Sky Atmosphere object. The clouds need the
// procedural sky (part B): the distant sky light lights them, the aerial-perspective volume hazes
// them and the atmosphere's units place them; in HDRI mode the pass does not run and says so once.
//
// Units are UE's: the layer in kilometres above the planet, extinction per METRE, distances in km.
// Where a value is UE's default it says so; where it is ours (the density model is not a
// transcription -- UE's is a material graph) it says that too.
struct VolumetricCloudSettings
{
    bool enabled = false;

    // --- The layer. UE VolumetricCloudComponent: LayerBottomAltitude 5 km, LayerHeight 10 km are a
    // big-sky default. Ours is a trade-wind cumulus deck over an island: bases at 600 m (where they sit
    // over a tropical sea), 700 m of layer for the profile's tallest column to fill.
    float layerBottomKm = 0.6f;
    float layerHeightKm = 0.7f;

    // --- The density model (cloud_common.hlsli): Ultra Dynamic Sky's scheme -- a formation volume
    // thresholded by coverage, shaped by a profile LUT, eroded by a cells volume -- on OUR textures,
    // generated at load (cloud_noise_cs.hlsl; --cloud-* files replace them for an A/B). The defaults
    // reproduce UDS's look (fitted 2026-10-12); the knobs below them move away from it.
    float coverage = 0.5f;          // UDS's "Cloud Density" / 3: 0 clear .. 1 closed; also picks the profile's column (how tall)
    float cloudType = 0.0f;         // offsets the profile's column: -1 flatter .. +1 taller clouds at the same coverage
    float weatherTileKm = 12.0f;    // the formation volume's tile: the spacing of the cloud groups (UDS's is 12 km)
    float windKmH = 20.0f;          // drift speed along the level's wind heading (the shared wind clock)
    std::uint32_t seed = 0u;        // reseeds every generated texture: a different sky, the same weather
    //   formationChangeSpeed -- how fast the shapes evolve RELATIVE TO THE DRIFT (UDS's Formation
    //     Change Speed): 1 = one full cycle of the volume's time axis while the clouds drift 16
    //     formation tiles; at 30 km/h over a 12 km tile that is a new slice every ~3 min, a cloud
    //     living 15-30 min. No wind, no change.
    //   macroVariation -- coverage +- this, from the weather map's field over macroTileKm (UDS's
    //     Macro Variation): cloudy regions and clear ones instead of an even scatter.
    float formationChangeSpeed = 1.0f;
    float macroVariation = 0.0f;
    float macroTileKm = 60.0f;
    // Fine tuning. Each default is UDS's value or a fit to their look (cloud_noise_cs.hlsl).
    float formationRoughness = 0.85f;     // the formation fBm's gain: higher = flatter, more broken-up formations (regenerates the volume)
    float floorVariationClear = 0.08f;    // how far the formation's B channel lifts cloud bases at partial cover ...
    float floorVariationCloudy = 0.64f;   // ... and at heavy cover (UDS Floor Variation Clear / Cloudy)
    float erosionStrength = 1.2f;         // UDS 3D Erosion: how much the cells carve the formation
    float erosionPower = 3.0f;            // UDS Erosion Power: higher = only the cells' brightest parts carve
    float erosionTileKm = 1.2f;           // world period of the cells volume (vertically 0.58 of it, as UDS's 1200 x 696 m)
    float highFrequencyStrength = 0.24f;  // UDS High Frequency Noise: the fine near-camera detail
    float highFrequencyDistanceKm = 1.5f; // ... faded out by this distance (UDS HF Octave Zero Distance)
    // The generated profile LUT's shape (CSProfile; a --cloud-profile file replaces it). The defaults
    // are a least-squares fit to Ultra Dynamic Sky's LUT over coverage 0.2 .. 0.7 (cloud_noise_cs.hlsl).
    float profileHeight = 1.0f;           // scales how high formations reach at every coverage
    float profileDome = 0.46f;            // how fast the envelope thins with height: 0 = flat-topped slabs, 1 = steep domes
    float profileTopSoftness = 0.30f;     // the fraction of a formation's height its top closes over
    float profileBaseSoftness = 0.29f;    // the height over which the underside gains density (soft vs crisp bases)
    float profileSmallDensity = 0.09f;    // density at no coverage (the smallest clouds): low = thin and wispy
    float profileBodyDensity = 0.65f;     // density at full coverage (cubed: 0.65 = 0.27 of Extinction)
    float profileBaseErosion = 0.12f;     // detail erosion at the floor: low = flat, firm undersides
    float profileErosionRise = 0.53f;     // how far up (fraction of the top) erosion reaches full strength
    float profileHeavyErosionCut = 0.41f; // how much less heavy cover erodes (smoother overcast)
    float profileHeavyThinning = 0.74f;   // how much cover above ~0.4 thins with height
    // Extinction at profile density 1, per metre. The profile's density is applied CUBED (a developed
    // cloud's ~0.5 is 1/8 of this), so it is ~8x a real cumulus' 0.04-0.1 /m; UDS run 10.
    float extinctionScale = 10.0f;
    float albedo = 0.95f;           // single-scattering albedo of the droplets

    // --- Lighting. UE's VolumetricAdvancedMaterialOutput node defaults are isotropic (g 0 / 0 /
    // blend 0, zero extra octaves), which no shipped cloud material keeps; these are Ultra Dynamic
    // Sky's. SIGN: POSITIVE g is forward scattering, here AND in UE's node -- their HG takes +2g cos and
    // they negate the cosine of a light direction that points TO the sun (VolumetricCloud.usf:332,
    // SceneRendering.cpp:1546), which is the textbook form again. A UE or UDS value is entered as is.
    float phaseG = 0.85f;
    float phaseG2 = 0.4f;
    float phaseBlend = 0.65f;
    float msContribution = 0.85f;   // UE ConstMultiScatteringContribution
    float msOcclusion = 0.5f;       // UE ConstMultiScatteringOcclusion
    float msEccentricity = 0.4f;    // UE ConstMultiScatteringEclipse
    // UE MultiScatteringApproximationOctaveCount: extra octaves beyond single scattering, 0..2. UDS run
    // 2, which carries the sun's light deeper into a dense cloud (lighter, sun-coloured undersides).
    std::uint32_t msOctaves = 2u;
    float skyLightBottomOcclusion = 0.0f; // UE SkyLightCloudBottomOcclusion: the sky's light fades out towards the layer's bottom (UDS: none)
    // Scales the sky's light on the clouds (the distant sky light, the only light in their shadowed
    // parts besides multi-scattering): higher = lighter AND bluer shadows, since that light is the
    // blue sky's. 1 = the atmosphere as computed. Ultra Dynamic Sky replaces it with authored colours.
    float skyLightIntensity = 1.0f;

    // --- Tracing. UE r.VolumetricCloud.ViewRaySampleMaxCount is 768 at its cinematic default and
    // DistanceToSampleMaxCount 15 km, i.e. a 19.5 m step; the step is distanceToSampleCountMaxKm /
    // this, held constant below that distance (cloud_march.hlsli). 3 km / 128 = 23 m steps through
    // the thin default layer (UDS raise UE's count 4x for the same reason); 0.71 ms at 256 (12 m) vs
    // 0.43 at 128 on wind_test, 2560 x 1440 under DLSS Balanced (2026-10-12).
    std::uint32_t viewSampleCountMax = 128u;
    std::uint32_t viewSampleCountMin = 2u;       // UE SampleMinCount
    float distanceToSampleCountMaxKm = 3.0f;     // UE DistanceToSampleMaxCount (theirs 15)
    std::uint32_t shadowSampleCount = 4u;        // the march to the sun per lit sample (UE Shadow.ViewRaySampleMaxCount 80 -- dear; UDS 4)
    float shadowTracingDistanceKm = 0.156f;      // UE ShadowTracingDistance (theirs 15; UDS 0.156)
    float stopTracingTransmittance = 0.005f;     // UE StopTracingTransmittanceThreshold
    float tracingStartMaxDistanceKm = 350.0f;    // UE TracingStartMaxDistance
    float tracingMaxDistanceKm = 50.0f;          // UE TracingMaxDistance (mode: from the layer entry point)
    bool temporal = true;
    float historyWeight = 0.9f;                  // plan C2

    // --- The cloud shadow map (plan C3): what puts the cloud's shadow on the island and in the fog.
    // UE's DirectionalLight defaults are CloudShadowExtent 150 km / 512 texels -- 585 m a texel,
    // too coarse for a cloud shape over a 2 km island -- so the extent here is ours; the snap is
    // scaled with it (UE 20 km at 150).
    bool shadowMap = true;
    float shadowExtentKm = 20.0f;      // half-width of the map on the ground
    float shadowStrength = 1.0f;       // UE CloudShadowStrength
    // Ours: how much of the cloud shadow the WATER BODY takes (the glints and the foam always lose the
    // sun). 0 = the water-leaving light ignores the cloud, 1 = it follows the sun as the land does.
    // Stands in for the light the bright cloud sides/undersides add under broken cloud (Mobley: the
    // diffuse part stays 30-60 % of the clear-sky total), which our environment cube does not carry.
    float oceanBodyShadow = 0.35f;
    float shadowSnapKm = 2.0f;         // UE ShadowMap.SnapLength
    float shadowDepthBiasKm = 0.0f;    // UE CloudShadowDepthBias
    std::uint32_t shadowMapSampleCount = 16u; // UE CloudShadowRaySampleBaseCount 16 (x horizon factor)

    // --- Session only (never in a level): 0 scene, 1 the coverage (the thresholded formation field) over the sky, 2 the
    // transmittance, 3 samples taken / max.
    unsigned debugView = 0;
};

// MIRROR of `CloudCB` in shaders/cloud_common.hlsli, field for field and in order. Uploaded as one
// blob (like FogApplyConstants), so all members are float4 / float4x4 and nothing pads.
struct VolumetricCloudConstants
{
    float invViewProjNoJitter[16];
    float viewProjNoJitter[16];
    float prevViewProjNoJitter[16];
    float shadowViewProj[16];
    float shadowInvViewProj[16];
    float camera[4];      // xyz camera (m), w planet bottom radius (km)
    float layer[4];       // bottom radius km, top radius km, 1 / height km, bottom altitude km
    float sun[4];         // xyz to sun, w phase g
    float sunColor[4];    // rgb effective illuminance, w phase g2
    float phase[4];       // blend, ms contribution, ms occlusion, ms eccentricity
    float trace[4];       // sample max, sample min, 1 / distance to max (1/km), stop transmittance
    float shadowTrace[4]; // shadow samples, shadow distance km, sky bottom visibility, tracing max distance km
    float medium[4];      // extinction 1/m, albedo, coverage, cloud type bias
    float formation[4];   // 1 / formation tile (1/km), its time coordinate (0..1, wraps), macro variation, 1 / macro tile (1/km)
    float wind[4];        // xyz world drift offset (m), w tracing start max distance km
    float output[4];      // width, height, preExposure, 1/preExposure
    float depth[4];       // depth width, depth height, proj._33, proj._43
    float aerial[4];      // aerial on, aerial start km, distant on, debug view
    float temporal[4];    // history valid, history weight, exposure ratio, frame index
    float shadowMap[4];   // resolution, 1/resolution, far depth km, strength
    float shadowMap2[4];  // sample count, depth bias km, zw unused
    float shadowMap3[4];  // x: shadow-map texel stride this frame (1 = all), yz: traced offset in the stride cell, w: cheap-density bits (1 bilinear macro variation, 2 mean erosion)
    float profile[4];     // x: unused, y: 1 / LUT width, z: 1 / LUT height, w: unused
    float lighting[4];    // x: lit extra multi-scattering octaves (0..2), y: sky light intensity, zw: unused
    float erosion[4];     // x erosion strength, y erosion power, z 1 / erosion tile (1/km), w high-frequency strength
    float floorShape[4];  // x floor variation clear, y cloudy, z 1 / high-frequency distance (1/km), w unused
};
static_assert(sizeof(VolumetricCloudConstants) == 5 * 64 + 21 * 16, "CloudCB layout");

// --- PROJECT-WIDE, not the level: graphics_settings.json "cloudShadow", Developer Controls > Sky,
// `--set=cloudShadow.updateFrames`. A performance switch, the role UE gives a cvar
// (r.VolumetricCloud.ShadowMap.*), so it does not travel with a level.
//
// Over how many frames ONE full refresh of the cloud shadow map is spread. 1 = every texel every
// frame, which is UE's behaviour (they re-trace the map each frame; their only amortisation is an
// experimental half-res temporal filter, TemporalFiltering.NewFrameWeight, off by default). 4 = a
// 2x2-interleaved quarter per frame, 16 = a 4x4-interleaved sixteenth. Sound because the cloud
// field moves RIGIDLY with the wind: at 150 km/h it drifts 0.14 m a frame at 300 fps (1.4 m at
// 30) against a 78 m texel, so a texel re-traced every 4 frames lags by less than a metre. Whatever
// moves the map itself -- the sun, the snapped anchor, the cloud settings, a noise rebuild, a wind
// jump -- forces a full trace that frame (VolumetricCloud::BuildShadow).
namespace render
{
inline std::uint32_t g_cloudShadowUpdateFrames = 4u;
inline std::uint32_t SanitizeCloudShadowUpdateFrames(std::uint32_t frames)
{
    return frames >= 16u ? 16u : (frames >= 4u ? 4u : 1u);
}

// Cheap density for the MAP's march only (the view, its self-shadow and the capture keep the full
// model), same file / tab / --set family. Each drops texture taps from every map sample:
//   bilinearWeather: the macro variation's field in one bilinear tap instead of the view's 4-tap
//     B-spline (only while macroVariation > 0). The B-spline exists for a PERSPECTIVE artefact (facets
//     drawn out into streaks along the view); the map is an ortho grid, filtered 3x3 afterwards.
//   detailMean: erode by the cells' MEAN (cloud_common.hlsli CloudMeanErosion) without fetching them,
//     and without the fine detail -- what the view's own far self-shadow samples do. The cells are
//     150 m, ~4 map texels.
// The price is a shadow that can differ slightly from the cloud casting it; switching one off restores
// the view's model for that term exactly. Measured 2026-10-12 on wind_test (the owner's pose, the map
// traced whole every frame, one binary; the old density model's numbers were -0.29 ms for both):
//   Pass_CloudShadow  both off 0.167 / 0.164 ms (twice) / mean 0.157 / bilinear 0.154 / both 0.150
//   below the horizon, pixels > 8 codes vs both off: floor 0.27 % (max 30), bilinear 0.25 % (max 29),
//   mean 0.68 % (max 66)
// So bilinear stays ON (free in the image), and mean erosion is OFF by default: for this density
// model it buys 0.01 ms for a shadow that visibly leaves its cloud.
inline bool g_cloudShadowBilinearWeather = true;
inline bool g_cloudShadowDetailMean = false;
inline std::uint32_t CloudShadowCheapBits()
{
    return (g_cloudShadowBilinearWeather ? 1u : 0u) | (g_cloudShadowDetailMean ? 2u : 0u);
}

// What the map's update ACTUALLY did last frame, for the Developer window: a scheduling switch
// without the number that says it reached the GPU is not a control. Written by BuildShadow in the
// serial graph build, read by the UI on the same thread.
struct CloudShadowUpdateStats
{
    std::uint32_t tracedTexels = 0; // last frame; 0 when no map was built
    std::uint32_t totalTexels = 0;
    std::uint64_t forcedFullTraces = 0; // while slicing: full traces the key forced
    const char* lastForcedReason = "none";
};
inline CloudShadowUpdateStats g_cloudShadowStats{};
}
