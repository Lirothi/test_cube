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
    float4 cloudDistortion; // x: weather strength (UV), y: 1/distortion tile km, zw: unused
    float4 cloudDistortionOffset; // xyz: distortion offset in texture UVW, w: unused
    float4 cloudShadowMap3;  // x: shadow-map texel stride this frame (1 = every texel), yz: the traced texel's offset in its stride x stride cell, w: cheap-density bits (1 bilinear weather, 2 detail mean)
    float4 cloudProfile;     // x: profile LUT on (1/0), y: 1 / LUT width, z: 1 / LUT height, w: unused
    float4 cloudFormation;   // x: formation volume on (bound as CloudBaseNoise), y: its time coordinate (0..1, wraps), z: macro variation (coverage +-), w: 1 / macro tile (1/km)
    float4 cloudLighting;    // x: lit extra multi-scattering octaves (0..2, UE's count), yzw: unused
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

// The view ray through a screen point (ndc). THE POINT IT IS BUILT FROM IS 1 KM OUT, NOT AT THE NEAR
// PLANE. Clip z 0.5 under this reverse-Z camera is 2 cm from the eye (zNear 0.01 m); rebuilt through
// the inverse view-projection it is a WORLD position, and float spaces those 0.24 mm apart once the
// camera is 2-4 km from the origin -- subtracting the camera left a 2 cm vector with 0.1 mm of
// rounding in it, ~1e-2 rad of direction error in a pattern quantised across the screen. Every
// cloud texel snapped to that lattice: blocky clouds that shimmer as the camera moves, worse the
// further the camera is from the origin (owner, 2026-10-12, at 2.7 km; invisible near the island's
// centre). At 1 km the same rounding is ~4e-7 rad. The device z of that distance comes from the
// projection's own terms (viewZ = _43 / (z - _33)), so a finite or infinite far plane both work.
static const float kCloudRayPointViewZ = 1000.0f; // metres
float3 CloudViewRayDirection(float2 ndc)
{
    const float deviceZ = cloudDepth.z + cloudDepth.w / kCloudRayPointViewZ;
    const float4 onRay = mul(float4(ndc, deviceZ, 1.0f), cloudInvViewProjNoJitter);
    return normalize(onRay.xyz / onRay.w - cloudCamera.xyz);
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
// they negate the cosine at the call, of a light direction that points TO the sun
// (SceneRendering.cpp:1546) -- the two flips cancel, so their node's g has the textbook sign, the
// same as ours: cosTheta = dot(direction TO the sun, camera -> point), POSITIVE g throws light
// forward. (This comment said the opposite until 2026-10-12; the code was always right.)
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
// Room for UE's maximum, two EXTRA octaves (MultiScatteringApproximationOctaveCount 0..2); how many
// are lit is the level's `msOctaves` (cloudLighting.x), default 1 -- what this always did. Ultra
// Dynamic Sky runs 2: its third octave is occluded by msOcclusion^2 (0.25 at 0.5), so sunlight reaches
// the base of a dense cloud in the sun's colour -- what keeps their undersides grey instead of
// sky-blue (owner, 2026-10-12: "низ облаков темный очень"). An unlit octave scatters exactly 0.
#define CLOUD_MS_COUNT 3

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
    const float litOctaves = cloudLighting.x;
    [unroll] for (int ms = 1; ms < CLOUD_MS_COUNT; ++ms)
    {
        m.scattering[ms] = (float)ms <= litOctaves ? m.scattering[ms - 1] * msS : 0.0f;
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
// and one uploaded at load (VolumetricCloud::Prepare): the PROFILE LUT, used when cloudProfile.x.
#ifdef CLOUD_DENSITY
Texture3D<float4> CloudBaseNoise : register(CLOUD_T_BASE);
Texture3D<float4> CloudDetailNoise : register(CLOUD_T_DETAIL);
Texture2D<float4> CloudWeather : register(CLOUD_T_WEATHER);
Texture2D<float4> CloudProfileLut : register(CLOUD_T_PROFILE);
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

// THE PROFILE LUT, the alternative to the three gradients above (cloudProfile.x). The semantics are
// Ultra Dynamic Sky's Cloud_Profile (decoded from its material, 2026-10-12): u is the cloud type --
// UDS index it by coverage, we by the weather map's type field plus the cloudType bias, so 0 is the
// flat, low fair-weather end and 1 the full-height end -- and v = 1 - height in the layer (row 0 is
// the top). R is the ENVELOPE: the density is the coverage field minus (1 - R), so only the dense
// middle of a cloud reaches the top of its profile (flat bases, domed tops). G scales the extinction,
// cubed. B scales the detail erosion at that height (low near the base = a firm, flat underside).
// Read with the set's wrap sampler, the coordinates held half a texel inside so neither axis wraps.
float4 CloudSampleProfile(float type, float normAlt)
{
    const float2 halfTexel = 0.5f * cloudProfile.yz;
    const float2 uv = clamp(float2(type, 1.0f - normAlt), halfTexel, 1.0f - halfTexel);
    return CloudProfileLut.SampleLevel(CloudLinearWrap, uv, 0);
}

struct CloudSample
{
    float extinction; // 1/m, 0 = clear air
    float coverage;   // the weather map's coverage after the knob (debug view 1)
};

// Animated weather-map domain warp. Sample the same R/B Worley channels and coordinates as
// before, but only compute the horizontal displacement needed by coverage and cloud type.
// The 3D field bends weather boundaries through height; base noise coordinates stay undeformed.
float2 CloudWeatherCoordinates(float3 shapeKm, float2 weatherUV)
{
    [branch] if (cloudDistortion.x > 0.0f)
    {
        const float2 noise = CloudDetailNoise.SampleLevel(CloudLinearWrap,
            shapeKm * cloudDistortion.y + cloudDistortionOffset.xyz, 0).rb;
        // Worley channels have mean ~0.48; remove the mean translation and bound each axis.
        const float2 displacement = clamp((noise - 0.48f) * 2.0f, -1.0f, 1.0f);
        weatherUV += displacement * cloudDistortion.x;
    }
    return weatherUV;
}

// The cheap half: weather + base shape. `conservative` > 0 means "medium may be here"; the detail
// erosion is only paid when it is (UE's VolumeSampleConservativeDensity, usf:781-787). The wind
// offset drifts the whole field with the level's wind. Evaluate the warp here too: empty-space
// decisions must use the same warped weather as the full view, self-shadow, map and capture samples.
//
// bicubicWeather false = one bilinear tap instead of the B-spline's four: the cloud shadow map's
// cheap option only (render::g_cloudShadowBilinearWeather). The facets the B-spline exists to
// remove are a PERSPECTIVE artefact of the view; the map is an ortho grid of the same 78 m pitch as
// the weather texel, filtered 3x3 afterwards. Every other caller passes a literal true.
//
// profileOut: the profile LUT's texel when cloudProfile.x is on, else (1,1,1,1) -- which leaves the
// erosion and the extinction in CloudSampleAt multiplied by exactly 1, i.e. the old model bit for bit.
float CloudBaseDensity(float3 worldMetres, float normAlt, bool bicubicWeather, out float coverageOut, out float typeOut,
                       out float4 profileOut)
{
    const float3 p = (worldMetres + cloudWind.xyz) / CloudMetresPerKm; // km, drifting
    [branch] if (cloudFormation.x > 0.0f)
    {
        // THE FORMATION VOLUME (a test of Ultra Dynamic Sky's model, render::g_cloudFormationPath),
        // bound in the base noise's slot: x/y = the field over one weather tile, z = time (it loops).
        // UDS's Volumetric_Clouds_Conservative_Density, term for term: the coverage knob is their
        // "Cloud Density" D / 3 (so it is also the profile's column, as theirs is), the threshold
        // (1 - D/6.5)^3 - 0.25 on R, and the floor variation -- B squared lowers the height the
        // profile is read at, by their Floor Variation Clear 0.08 .. Cloudy 0.64 -- so bases undulate.
        const float4 formation = CloudBaseNoise.SampleLevel(CloudLinearWrap, float3(p.xz * cloudShape.z, cloudFormation.y), 0);
        // MACRO VARIATION (UDS's, which is a sine product; ours is the weather map's coverage field,
        // unused otherwise in this branch): the coverage moves +- cloudFormation.z over a much larger
        // tile, so the sky has cloudy regions and clear ones -- and since the profile's column follows
        // the coverage, the clouds in a cloudy region are also taller, as UDS's are.
        float coverageHere = saturate(cloudMedium.z);
        [branch] if (cloudFormation.z > 0.0f)
        {
            const float macro = CloudSampleWeather(p.xz * cloudFormation.w).r;
            coverageHere = saturate(coverageHere + cloudFormation.z * (macro * 2.0f - 1.0f));
        }
        const float density = 3.0f * coverageHere;
        const float threshold = pow(1.0f - density / 6.5f, 3.0f) - 0.25f;
        const float field = saturate(formation.r - threshold) / (1.0f - threshold);
        const float floorVariation = lerp(0.08f, 0.64f, saturate(density - 1.5f));
        const float profileHeight = saturate(lerp(normAlt, 1.0f, -formation.b * formation.b * floorVariation));
        const float column = saturate(density / 3.0f + cloudMedium.w);
        profileOut = CloudSampleProfile(column, profileHeight);
        coverageOut = field;
        typeOut = column;
        return saturate(field - (1.0f - profileOut.r));
    }
    const float3 shapeKm = float3(p.x, p.y * cloudShadowMap2.z, p.z);
    const float3 baseCoordinates = shapeKm * cloudShape.x;
    // Warp before ALL weather interpretation: coverage, type and conservative density agree.
    const float2 weatherCoordinates = CloudWeatherCoordinates(shapeKm, p.xz * cloudShape.z);
    // A branch, not `?:` -- HLSL evaluates both arms of a ternary, which would pay all five taps.
    float4 weather;
    [branch] if (bicubicWeather) { weather = CloudSampleWeather(weatherCoordinates); }
    else { weather = CloudWeather.SampleLevel(CloudLinearWrap, weatherCoordinates, 0); }
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
    const float4 base = CloudBaseNoise.SampleLevel(CloudLinearWrap, baseCoordinates, 0);
    const float lowFreqFbm = base.g * 0.625f + base.b * 0.25f + base.a * 0.125f;
    float baseCloud = saturate(CloudRemap(base.r, -(1.0f - lowFreqFbm), 1.0f, 0.0f, 1.0f));
    [branch] if (cloudProfile.x > 0.0f)
    {
        // UDS's Volumetric_Clouds_Conservative_Density: the thresholded field normalised to 0..1
        // (no trailing "* coverage" -- the envelope does the thinning), minus what the profile takes
        // away at this height.
        profileOut = CloudSampleProfile(type, normAlt);
        const float field = saturate(CloudRemap(baseCloud, 1.0f - coverage, 1.0f, 0.0f, 1.0f));
        return saturate(field - (1.0f - profileOut.r));
    }
    profileOut = 1.0f.xxxx;
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
CloudSample CloudSampleAt(float3 worldMetres, float normAlt, float detailWeight, bool bicubicWeather)
{
    CloudSample s;
    float coverage, type;
    float4 profile;
    const float base = CloudBaseDensity(worldMetres, normAlt, bicubicWeather, coverage, type, profile);
    s.coverage = coverage;
    s.extinction = 0.0f;
    if (base <= 0.0f) { return s; }
    [branch] if (cloudFormation.x > 0.0f)
    {
        // UDS's Volumetric_Clouds_Extinction, term for term, with THEIR erosion volume (3D_Cells,
        // render::g_cloudCellsPath) bound in the detail slot: tiled 1.2 x 0.696 x 1.2 km (their 3D Noise
        // Scale), R cubed times 1.2 (3D Erosion, Erosion Power 3) times the profile's B; then their
        // high-frequency custom node -- two octaves at 3.5x the erosion's frequency, distorted by it
        // (HF Distortion 0.21), faded out by 1.5 km from the camera (HF Octave Zero Distance) -- at
        // 0.24 (High Frequency Noise) under the cells' B; then their extinction scale times G cubed.
        const float3 p = (worldMetres + cloudWind.xyz) / CloudMetresPerKm;
        const float3 q = float3(p.x / 1.2f, p.y / 0.696f, p.z / 1.2f);
        const float3 cells = CloudDetailNoise.SampleLevel(CloudLinearWrap, q, 0).rgb;
        float shape = base - cells.r * cells.r * cells.r * 1.2f * profile.b;
        float octaves = min(1.5f / max(length(worldMetres - cloudCamera.xyz) / CloudMetresPerKm, 1.0e-3f), 2.0f);
        float3 uvw = q * 3.5f + cells * 0.21f;
        float highFrequency = 0.5f, strength = 1.0f;
        [unroll] for (int octave = 0; octave < 2; ++octave)
        {
            [branch] if (octaves > 0.3f)
            {
                highFrequency += (CloudDetailNoise.SampleLevel(CloudLinearWrap, frac(uvw), 0).r - 0.55f) * strength * saturate(octaves * 4.0f);
            }
            strength *= 0.6f; uvw *= 2.0f; octaves -= 1.0f;
        }
        shape += (highFrequency - 0.55f) * 0.24f * cells.b;
        s.extinction = max(shape, 0.0f) * max(cloudMedium.x, 0.0f) * (profile.g * profile.g * profile.g);
        return s;
    }
    // A sample that resolves NONE of the detail does not fetch it: lerp(mean, x, 0) is the mean
    // exactly, so this is bit-identical and saves a 3D fetch on every far self-shadow sample of
    // the view march and on every shadow-map sample in its detail-mean mode.
    float highFreqFbm = kCloudDetailMean;
    [branch] if (detailWeight > 0.0f)
    {
        const float3 p = (worldMetres + cloudWind.xyz) / CloudMetresPerKm;
        const float4 detail = CloudDetailNoise.SampleLevel(CloudLinearWrap, p * cloudShape.y, 0);
        const float pointFbm = detail.r * 0.625f + detail.g * 0.25f + detail.b * 0.125f;
        highFreqFbm = lerp(kCloudDetailMean, pointFbm, saturate(detailWeight));
    }
    // Wispy at the bottom of the cloud, billowy towards the top.
    const float modifier = lerp(highFreqFbm, 1.0f - highFreqFbm, saturate(normAlt * 10.0f));
    const float eroded = saturate(CloudRemap(base, modifier * saturate(cloudShape.w) * profile.b, 1.0f, 0.0f, 1.0f));
    s.extinction = eroded * max(cloudMedium.x, 0.0f) * (profile.g * profile.g * profile.g);
    return s;
}

CloudSample CloudSampleAt(float3 worldMetres, float normAlt, float detailWeight)
{
    return CloudSampleAt(worldMetres, normAlt, detailWeight, true);
}

CloudSample CloudSampleAt(float3 worldMetres, float normAlt)
{
    return CloudSampleAt(worldMetres, normAlt, 1.0f, true);
}
#endif // CLOUD_DENSITY

#endif // CLOUD_COMMON_HLSLI
