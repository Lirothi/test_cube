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
    // big-sky default; the plan's 1.5 / 2.5 km puts a fair-weather cumulus deck over an island.
    float layerBottomKm = 1.5f;
    float layerHeightKm = 2.5f;

    // --- The density model (ours, Schneider 2015 / Nubis; cloud_common.hlsli).
    float coverage = 0.5f;          // threshold on the weather map's coverage field, 0 clear .. 1 overcast
    float cloudType = 0.0f;         // bias on the weather map's type field: -1 all stratus .. +1 all cumulus
    float extinctionScale = 0.05f;  // extinction at full density, 1/m (real cumulus 0.04-0.1)
    float albedo = 0.95f;           // single-scattering albedo of the droplets
    float detailStrength = 0.35f;   // how much the Worley detail erodes the base shape
    float baseTileKm = 6.0f;        // world period of the 128^3 base noise
    float detailTileKm = 0.5f;      // world period of the 32^3 detail noise
    float weatherTileKm = 40.0f;    // world period of the 512^2 weather map
    float windKmH = 20.0f;          // drift speed along the level's wind heading (the shared wind clock)
    std::uint32_t seed = 0u;        // reseeds all three noise textures

    // --- Lighting. UE's VolumetricAdvancedMaterialOutput node defaults are isotropic (g 0 / 0 /
    // blend 0, zero extra octaves), which no shipped cloud material keeps; these are the sample
    // content's shape. SIGN: ours is the textbook Henyey-Greenstein, POSITIVE g is forward
    // scattering (UE's node negates the cosine, so theirs is the other way round).
    float phaseG = 0.6f;
    float phaseG2 = -0.3f;
    float phaseBlend = 0.25f;
    float msContribution = 0.5f;    // UE ConstMultiScatteringContribution
    float msOcclusion = 0.5f;       // UE ConstMultiScatteringOcclusion
    float msEccentricity = 0.5f;    // UE ConstMultiScatteringEclipse
    float skyLightBottomOcclusion = 0.5f; // UE SkyLightCloudBottomOcclusion: the sky's light fades out towards the layer's bottom

    // --- Tracing. UE r.VolumetricCloud.ViewRaySampleMaxCount is 768 at its cinematic default and
    // DistanceToSampleMaxCount 15 km, i.e. a 19.5 m step; the step is 15 km / this, held constant
    // below 15 km (cloud_trace_cs.hlsl). 256 = 58 m steps, the first count that resolves a
    // kilometre-scale cumulus without a mushy edge; 128 (117 m) was the budget's starting point.
    std::uint32_t viewSampleCountMax = 256u;
    std::uint32_t viewSampleCountMin = 2u;       // UE SampleMinCount
    float distanceToSampleCountMaxKm = 15.0f;    // UE DistanceToSampleMaxCount
    std::uint32_t shadowSampleCount = 6u;        // the march to the sun per lit sample (UE Shadow.ViewRaySampleMaxCount 80 -- dear)
    float shadowTracingDistanceKm = 15.0f;       // UE ShadowTracingDistance
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
    float shadowSnapKm = 2.0f;         // UE ShadowMap.SnapLength
    float shadowDepthBiasKm = 0.0f;    // UE CloudShadowDepthBias
    std::uint32_t shadowMapSampleCount = 16u; // UE CloudShadowRaySampleBaseCount 16 (x horizon factor)

    // --- Session only (never in a level): 0 scene, 1 weather coverage over the sky, 2 the
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
    float medium[4];      // extinction 1/m, albedo, coverage, type bias
    float shape[4];       // 1/base tile, 1/detail tile, 1/weather tile, detail strength
    float wind[4];        // xyz world drift offset (m), w tracing start max distance km
    float output[4];      // width, height, preExposure, 1/preExposure
    float depth[4];       // depth width, depth height, proj._33, proj._43
    float aerial[4];      // aerial on, aerial start km, distant on, debug view
    float temporal[4];    // history valid, history weight, exposure ratio, frame index
    float shadowMap[4];   // resolution, 1/resolution, far depth km, strength
    float shadowMap2[4];  // sample count, depth bias km, 0, 0
};
static_assert(sizeof(VolumetricCloudConstants) == 5 * 64 + 16 * 16, "CloudCB layout");
