// Volumetric clouds, part C of docs/volumetric_fog_sky_clouds_ssgi_plan.md: what the view trace
// (cloud_trace_cs.hlsl) and the cloud shadow map (cloud_shadow_cs.hlsl) share -- the layer, the
// constant buffer, the density model and UE's participating-media helpers.
//
// UE's cloud renderer (VolumetricCloud.usf) is transcribed for everything that is NOT the material:
// the layer intersection, the sample distribution, the multi-scattering octaves, the two-lobe phase,
// the shadow march, the Frostbite integration, the shadow map. UE's DENSITY is a material graph; ours
// is a fixed Schneider 2015 / Nubis model (Perlin-Worley base, Worley detail, a 2D weather map) in
// this file, and it is the one part of this feature that is not a transcription.
//
// Units. The layer and every ray length here are KILOMETRES, as UE's cloud parameters are (their
// shader multiplies back to cm; ours multiplies back to metres only for the extinction integral,
// which is per metre like UE's material extinction). World is Y-up metres; the planet centre sits
// `planetRadiusKm` below sea level on the Y axis, so a world point's altitude is a length, not its Y.
#ifndef CLOUD_COMMON_HLSLI
#define CLOUD_COMMON_HLSLI

static const float CloudPi = 3.14159265358979323846f;
static const float CloudMetresPerKm = 1000.0f;

// MIRROR of VolumetricCloudConstants (rendering/lighting/VolumetricCloud.h), field for field: it is
// uploaded as one blob, so the two must not drift. All members are float4 / float4x4.
#ifndef CLOUD_B_CB
#define CLOUD_B_CB b0
#endif
cbuffer CloudCB : register(CLOUD_B_CB)
{
    float4x4 cloudInvViewProjNoJitter;   // clip -> world, this frame's UNJITTERED camera
    float4x4 cloudViewProjNoJitter;      // world -> clip (w = view depth)
    float4x4 cloudPrevViewProjNoJitter;  // world -> last frame's unjittered clip (history UV)
    float4x4 cloudShadowViewProj;        // world -> cloud shadow map clip (plan C3)
    float4x4 cloudShadowInvViewProj;     // cloud shadow map clip -> world
    float4 cloudCamera;      // xyz: camera position (world m), w: planet bottom radius (km)
    float4 cloudLayer;       // x: bottom radius (km), y: top radius (km), z: 1 / layer height (km), w: layer bottom altitude (km)
    float4 cloudSun;         // xyz: direction TO the sun (world), w: phase g (textbook sign: + forward)
    float4 cloudSunColor;    // rgb: the sun's effective illuminance (post transmittance), w: second phase g
    float4 cloudPhase;       // x: phase blend, y: ms contribution, z: ms occlusion, w: ms eccentricity
    float4 cloudTrace;       // x: view sample count max, y: min, z: 1 / distance to max count (1/km), w: stop transmittance
    float4 cloudShadowTrace; // x: shadow sample count, y: shadow tracing distance (km), z: sky light bottom visibility, w: tracing max distance (km)
    float4 cloudMedium;      // x: extinction at density 1 (1/m), y: albedo, z: coverage, w: cloud type bias
    float4 cloudShape;       // x: 1 / base tile (1/km), y: 1 / detail tile, z: 1 / weather tile, w: detail strength
    float4 cloudWind;        // xyz: world drift offset (m) added to every sample, w: tracing start max distance (km)
    float4 cloudOutput;      // x: output width, y: output height, z: preExposure, w: 1 / preExposure
    float4 cloudDepth;       // x: depth width, y: depth height, z: proj._33, w: proj._43 (reverse-Z: viewZ = w / (z - z33))
    float4 cloudAerial;      // x: aerial perspective on, y: aerial start depth (km), z: distant sky light on, w: debug view
    float4 cloudTemporal;    // x: history valid, y: history weight, z: preExposure / previous preExposure, w: frame index
    float4 cloudShadowMap;   // x: resolution, y: 1 / resolution, z: far depth (km), w: strength
    float4 cloudShadowMap2;  // x: sample count, y: depth bias (km), z: base noise vertical compression (>= 1), w: overcast (0..1)
#ifdef CLOUD_WITH_SKY_CB
    // The sky's SkyAtmosphereCB (sky_atmosphere.hlsli, its SKY_VIEW layout), appended for the one
    // pass that needs both -- the environment capture composites the cloud INTO the sky
    // (cloud_capture_cs.hlsl). The names are the sky's own, so its functions read them unchanged;
    // sky_atmosphere.hlsli is then included with SKY_CB_EXTERNAL and declares no cbuffer. The C++
    // blob is VolumetricCloudConstants + SkyAtmosphereParameters + SkyViewFrameData, in that order.
    float4 AtmosphereRadii;
    float4 RayleighScattering;
    float4 MieScattering;
    float4 MieAbsorption;
    float4 AbsorptionExtinction;
    float4 AbsorptionDensity;
    float4 GroundAlbedo;
    float4 SkySunDirection;
    float4 SkyIlluminance;
    float4 SkyExposure;
    float4 SkyPlanet;
#endif
};

// ---------------------------------------------------------------------------------------------
// Geometry. UE VolumetricCloud.usf:188-213 RayIntersectSphereSolution: both roots, signed.
bool CloudRaySphere(float3 origin, float3 dir, float3 centre, float radius, out float2 solutions)
{
    solutions = -1.0f;
    const float3 p = origin - centre;
    const float a = dot(dir, dir);
    const float b = 2.0f * dot(dir, p);
    const float c = dot(p, p) - radius * radius;
    const float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f) { return false; }
    const float s = sqrt(discriminant);
    solutions = (-b + float2(-1.0f, 1.0f) * s) / (2.0f * a);
    return true;
}

// World metres -> planet-centred kilometres (Y-up, planet centre on the Y axis below sea level).
float3 CloudWorldToPlanetKm(float3 worldMetres)
{
    return float3(worldMetres.x, worldMetres.y + cloudCamera.w * CloudMetresPerKm, worldMetres.z) / CloudMetresPerKm;
}

// UE VolumetricCloud.usf:459-497 -- the segment of the view ray that lies inside the layer. Returns
// false when the ray misses the top shell altogether. tMin/tMax in km along `dir`.
bool CloudLayerSegment(float3 originKm, float3 dir, out float tMin, out float tMax, out float2 tBottom)
{
    tMin = -999999999.0f; tMax = -999999999.0f; tBottom = -1.0f;
    float2 tTop;
    if (!CloudRaySphere(originKm, dir, 0.0f.xxx, cloudLayer.y, tTop)) { return false; }
    if (CloudRaySphere(originKm, dir, 0.0f.xxx, cloudLayer.x, tBottom))
    {
        // Both intersections in front: the closest; otherwise the furthest.
        float tempTop = all(tTop > 0.0f) ? min(tTop.x, tTop.y) : max(tTop.x, tTop.y);
        const float tempBottom = all(tBottom > 0.0f) ? min(tBottom.x, tBottom.y) : max(tBottom.x, tBottom.y);
        if (all(tBottom > 0.0f))
        {
            // The bottom of the layer is visible: start at the camera or the highest top intersection.
            tempTop = max(0.0f, min(tTop.x, tTop.y));
        }
        tMin = min(tempBottom, tempTop);
        tMax = max(tempBottom, tempTop);
    }
    else
    {
        tMin = tTop.x;
        tMax = tTop.y;
    }
    tMin = max(0.0f, tMin);
    tMax = max(0.0f, tMax);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Phase. UE VolumetricCloud.usf:329-337 blend two Henyey-Greenstein lobes. Their HG takes +2g cos and
// they negate the cosine at the call, so a NEGATIVE g is forward-scattering in their material node;
// ours is the textbook form with cosTheta = dot(direction TO the sun, camera -> point), so a POSITIVE
// g throws light forward -- the same pair fog_common.hlsli transcribes as a pair.
float CloudHG(float g, float cosTheta)
{
    g = clamp(g, -0.999f, 0.999f);
    const float g2 = g * g;
    return (1.0f - g2) / (4.0f * CloudPi * pow(max(1.0f + g2 - 2.0f * g * cosTheta, 1.0e-4f), 1.5f));
}
float CloudIsotropicPhase() { return 1.0f / (4.0f * CloudPi); }
float CloudPhase(float cosTheta)
{
    const float p0 = CloudHG(cloudSun.w, cosTheta);
    const float p1 = CloudHG(cloudSunColor.w, cosTheta);
    return p0 + saturate(cloudPhase.x) * (p1 - p0);
}

// Multi-scattering octaves, UE VolumetricCloud.usf:368-424 (Wrenninge). Octave 0 is single scattering;
// each further octave scales scattering by the contribution factor, extinction by the occlusion factor
// and pulls the phase towards isotropic by the eccentricity factor -- each factor squared per octave.
// Two octaves total: UE's material node ships with ZERO extra octaves (isotropic single scattering),
// which no cloud material actually uses; the sample content uses one extra, so does this.
#define CLOUD_MS_COUNT 2

struct CloudMedia
{
    float scattering[CLOUD_MS_COUNT];
    float extinction[CLOUD_MS_COUNT];
    float phase[CLOUD_MS_COUNT];
    float transmittanceToSun[CLOUD_MS_COUNT];
};

CloudMedia CloudSetupMedia(float albedo, float extinction, float basePhase)
{
    CloudMedia m;
    m.scattering[0] = albedo * extinction;
    m.extinction[0] = extinction;
    m.phase[0] = basePhase;
    m.transmittanceToSun[0] = 1.0f;
    float msS = saturate(cloudPhase.y), msE = saturate(cloudPhase.z), msP = saturate(cloudPhase.w);
    [unroll] for (int ms = 1; ms < CLOUD_MS_COUNT; ++ms)
    {
        m.scattering[ms] = m.scattering[ms - 1] * msS;
        m.extinction[ms] = m.extinction[ms - 1] * msE;
        m.phase[ms] = lerp(CloudIsotropicPhase(), m.phase[0], msP);
        m.transmittanceToSun[ms] = 1.0f;
        msS *= msS; msE *= msE; msP *= msP;
    }
    return m;
}

// ---------------------------------------------------------------------------------------------
// DENSITY MODEL (ours; Schneider 2015 "The Real-time Volumetric Cloudscapes of Horizon: Zero Dawn").
// Three textures built at load by cloud_noise_cs.hlsl:
//   CloudBaseNoise   128^3 RGBA8  r: Perlin-Worley, gba: Worley at 2x / 4x / 8x
//   CloudDetailNoise 128^3 RGBA8  rgb: Worley at 1x / 2x / 4x (128^3, not Schneider's 32^3: see cloud_noise_cs.hlsl)
//   CloudWeather     512^2 RGBA8  r: coverage field, g: cloud type field, b: unused
#ifdef CLOUD_DENSITY
Texture3D<float4> CloudBaseNoise : register(CLOUD_T_BASE);
Texture3D<float4> CloudDetailNoise : register(CLOUD_T_DETAIL);
Texture2D<float4> CloudWeather : register(CLOUD_T_WEATHER);
SamplerState CloudLinearWrap : register(CLOUD_S_WRAP);

float CloudRemap(float v, float lo, float hi, float newLo, float newHi)
{
    return newLo + (v - lo) / max(hi - lo, 1.0e-5f) * (newHi - newLo);
}

// THE WEATHER MAP IS READ BICUBICALLY. Its texel is 78 m at the default 40 km tile, and bilinear
// filtering is only C0: the coverage field's gradient breaks at every texel edge, the threshold
// remap below turns those breaks into facets aligned with the world axes, and perspective draws
// the along-view facets as long radial streaks across every distant cloud (owner, 2026-09-12;
// the streak spacing scaled 4x with the weather tile and with nothing else). A cubic B-spline is
// C2 -- no facets -- and costs four bilinear taps (GPU Gems 2, ch. 20). Mirror of
// VolumetricCloud::kWeatherSize.
static const float kCloudWeatherSize = 512.0f;
float4 CloudSampleWeather(float2 uv)
{
    const float2 coord = uv * kCloudWeatherSize - 0.5f;
    const float2 f = frac(coord);
    const float2 cell = coord - f;
    const float2 f2 = f * f, f3 = f2 * f;
    const float2 w0 = (1.0f - 3.0f * f + 3.0f * f2 - f3) / 6.0f;
    const float2 w1 = (4.0f - 6.0f * f2 + 3.0f * f3) / 6.0f;
    const float2 w2 = (1.0f + 3.0f * f + 3.0f * f2 - 3.0f * f3) / 6.0f;
    const float2 w3 = f3 / 6.0f;
    const float2 g0 = w0 + w1, g1 = w2 + w3;
    const float2 h0 = (cell + 0.5f - 1.0f + w1 / g0) / kCloudWeatherSize;
    const float2 h1 = (cell + 0.5f + 1.0f + w3 / g1) / kCloudWeatherSize;
    return CloudWeather.SampleLevel(CloudLinearWrap, float2(h0.x, h0.y), 0) * (g0.x * g0.y)
         + CloudWeather.SampleLevel(CloudLinearWrap, float2(h1.x, h0.y), 0) * (g1.x * g0.y)
         + CloudWeather.SampleLevel(CloudLinearWrap, float2(h0.x, h1.y), 0) * (g0.x * g1.y)
         + CloudWeather.SampleLevel(CloudLinearWrap, float2(h1.x, h1.y), 0) * (g1.x * g1.y);
}

// Schneider's three height profiles, blended by the weather map's type channel: stratus hugs the
// bottom of the layer, cumulus fills most of it.
float CloudHeightGradient(float normAlt, float type)
{
    const float stratus = saturate(CloudRemap(normAlt, 0.0f, 0.07f, 0.0f, 1.0f)) * saturate(CloudRemap(normAlt, 0.2f, 0.3f, 1.0f, 0.0f));
    const float stratocumulus = saturate(CloudRemap(normAlt, 0.0f, 0.2f, 0.0f, 1.0f)) * saturate(CloudRemap(normAlt, 0.45f, 0.65f, 1.0f, 0.0f));
    const float cumulus = saturate(CloudRemap(normAlt, 0.0f, 0.1f, 0.0f, 1.0f)) * saturate(CloudRemap(normAlt, 0.7f, 1.0f, 1.0f, 0.0f));
    const float a = saturate(type * 2.0f), b = saturate(type * 2.0f - 1.0f);
    return lerp(lerp(stratus, stratocumulus, a), cumulus, b);
}

struct CloudSample
{
    float extinction; // 1/m, 0 = clear air
    float coverage;   // the weather map's coverage after the knob (debug view 1)
};

// The cheap half: weather + base shape. `conservative` > 0 means "medium may be here"; the detail
// erosion is only paid when it is (UE's VolumeSampleConservativeDensity, usf:781-787). The wind
// offset drifts the whole field rigidly with the level's wind.
float CloudBaseDensity(float3 worldMetres, float normAlt, out float coverageOut, out float typeOut)
{
    const float3 p = (worldMetres + cloudWind.xyz) / CloudMetresPerKm; // km, drifting
    const float4 weather = CloudSampleWeather(p.xz * cloudShape.z);
    // The coverage knob is a THRESHOLD on the weather field: 0 clears the sky, 1 lets every part of
    // the field through in proportion -- and no further: a third of the procedural map is zero after
    // its contrast stretch, so coverage 1 is "the map as drawn", not a closed sky (owner,
    // 2026-09-12). The gaps are the map's alone: the base shape below never reaches zero (its remap's
    // low end is <= 0), it only thins. Schneider closes the sky by PAINTING the weather map; ours is
    // procedural, so the OVERCAST knob does the painting: it lifts the weather field towards 1, and
    // nothing else -- a first version also lifted the base towards 1 and got a deck with no texture
    // at all (a constant density erodes nowhere, and an opaque slab's underside is one flat tone).
    // With the field lifted the base keeps its own 0.1..1 modulation: a closed, mottled layer. At 0
    // this is an exact identity (the field is already in 0..1).
    const float overcast = saturate(cloudShadowMap2.w);
    const float weatherCoverage = saturate(weather.r + overcast);
    const float coverage = saturate(CloudRemap(weatherCoverage, 1.0f - saturate(cloudMedium.z), 1.0f, 0.0f, 1.0f));
    const float type = saturate(weather.g + cloudMedium.w);
    coverageOut = coverage;
    typeOut = type;
    // The base noise is compressed VERTICALLY so one tile spans at most `baseVerticalTiles` layer
    // heights (cloudShadowMap2.z, >= 1): a 6 km tile through a 0.5 km layer is otherwise constant
    // with height, the density becomes the weather map extruded into columns, and a grazing ray
    // integrates the columns along itself into radial streaks. Flat cells instead of columns.
    const float4 base = CloudBaseNoise.SampleLevel(CloudLinearWrap, float3(p.x, p.y * cloudShadowMap2.z, p.z) * cloudShape.x, 0);
    const float lowFreqFbm = base.g * 0.625f + base.b * 0.25f + base.a * 0.125f;
    float baseCloud = saturate(CloudRemap(base.r, -(1.0f - lowFreqFbm), 1.0f, 0.0f, 1.0f));
    baseCloud *= CloudHeightGradient(normAlt, type);
    return saturate(CloudRemap(baseCloud, 1.0f - coverage, 1.0f, 0.0f, 1.0f)) * coverage;
}

// The mean of the detail texture's Worley fBm (measured on the bit-exact numpy port of
// cloud_noise_cs.hlsl: 0.480 / 0.479 / 0.479 for the three channels). A sample that must not
// resolve the detail (see detailWeight below) erodes by this mean instead of by the point value.
static const float kCloudDetailMean = 0.48f;

// detailWeight: how much of the detail erosion this sample resolves, 1 = the point value, 0 = its
// mean. The view march passes 1. The shadow march passes less for its far samples (see the march
// in cloud_trace_cs.hlsl for why): UE hands the same distance to the material as
// ShadowSampleDistance (VolumetricCloud.usf:1096, MaterialTemplate.ush:2568-2571) so that a far
// shadow sample can drop its detail; we have no material graph, so the rule lives in the march.
CloudSample CloudSampleAt(float3 worldMetres, float normAlt, float detailWeight)
{
    CloudSample s;
    float coverage, type;
    const float base = CloudBaseDensity(worldMetres, normAlt, coverage, type);
    s.coverage = coverage;
    s.extinction = 0.0f;
    if (base <= 0.0f) { return s; }
    const float3 p = (worldMetres + cloudWind.xyz) / CloudMetresPerKm;
    const float4 detail = CloudDetailNoise.SampleLevel(CloudLinearWrap, p * cloudShape.y, 0);
    const float pointFbm = detail.r * 0.625f + detail.g * 0.25f + detail.b * 0.125f;
    const float highFreqFbm = lerp(kCloudDetailMean, pointFbm, saturate(detailWeight));
    // Wispy at the bottom of the cloud, billowy towards the top.
    const float modifier = lerp(highFreqFbm, 1.0f - highFreqFbm, saturate(normAlt * 10.0f));
    const float eroded = saturate(CloudRemap(base, modifier * saturate(cloudShape.w), 1.0f, 0.0f, 1.0f));
    s.extinction = eroded * max(cloudMedium.x, 0.0f);
    return s;
}

CloudSample CloudSampleAt(float3 worldMetres, float normAlt)
{
    return CloudSampleAt(worldMetres, normAlt, 1.0f);
}
#endif // CLOUD_DENSITY

#endif // CLOUD_COMMON_HLSLI
