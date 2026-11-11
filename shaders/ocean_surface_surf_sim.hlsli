// THE SURF-SIM OCEAN SURFACE -- the one in use (OCEAN_SHORE_RUNUP=0, the default; see
// ocean_surface.hlsl, which dispatches between this file and ocean_surface_runup.hlsli).
//
// History: this began as the classic surface, verbatim from commit 3e54d5d (2026-06-22, the last
// state before the shore/run-up rework), kept as a baseline while the run-up stack was built. It
// has since grown into the main surface -- the surf sim injection (t16/t17 height and foam fields,
// docs/ocean_surf_sim_plan.md), the authored nearshore attenuation (shoreLegacyDampParams, defaults
// reproduce the original curve), the shared environment (t18/t19), the volumetric fog volume (t20)
// and the cloud shadow map (t21) -- so "byte-faithful" stopped being true, and on 2026-09-12 the
// owner renamed it from "legacy" to what it is. The binding plumbing against the original: RS
// numDescriptors 14->22, SceneDepth t11->t12, ShoreDepth t12->t13, Reflection t13->t15;
// ContactFoamTex stays at t10 (today's slot 10 carries the same ContactFoam.dds, loaded linear
// rather than sRGB). The C++ table stages the same 26 entries for both surfaces; t22-t25, s4 and the
// transparent pass's GlassView CB (b1) are the sun's shadow (ocean_surface_common.hlsli OceanSunShadow).
#define OCEAN_SURFACE_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), CBV(b1), CBV(b2), DescriptorTable(SRV(t0, numDescriptors=31, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=5, flags=DESCRIPTORS_VOLATILE))"
#pragma pack_matrix(row_major)

#include "utils.hlsli" // renamed since June; the only include fix
#include "ibl_common.hlsli" // P5: the shared roughness <-> mip mapping
#include "height_fog.hlsli"
#include "fog_common.hlsli" // volumetric fog plan A5: the froxel volume on the water // P7: the same medium the modern surface and compose use

cbuffer OceanCB : register(b0)
{
    float4x4 model;
    float4x4 view;
    float4x4 proj;
    float4x4 prevModel;
    float4x4 viewProj;
    float4x4 viewProjNoJitter;
    float4x4 prevViewProjNoJitter;
    float4x4 invView;
    float4x4 invProj;
    float4 shoreViewParams;            // x: center x, y: center z, z: height, w: inv extent (1 / 500)
    float4 shoreDepthParams;           // x: zNear, y: zFar
    float4x4 worldToWind;
    float4 simulationParams;           // x: patch length, y: inv patch length, z: time, w: cascades count
    float4 viewerParams;               // x: viewer x, y: viewer z, z: amplitude, w: fade distance
    float4 cascadeLengthScales;        // length scales per cascade
    float4 inverseCascadeLengthScales; // inv length scales per cascade
    float4 clipMapParams;              // x: scale, y: level half size, z: vertex density, w: fade distance
    float4 clipMapViewer;              // xyz: viewer position
    float4 prevClipMapParams;          // previous frame clipmap params
    float4 prevClipMapViewer;          // previous frame viewer position
    float4 foamParams0;                // x: coverage, y: density, z: sharpness, w: persistence
    float4 foamParams1;                // x: trail, y: trail strength, z: underwater intensity, w: normal strength
    float4 foamCascadeWeights;         // per-cascade foam weighting
    float4 specularParams;             // x: spec strength, y: roughness scale, z: roughness distance, w: horizon fog strength
    float4 refractionParams;           // x: surface refraction strength, y: underwater refraction strength, z: absorption depth scale, w: fog density
    float4 subsurfaceParams;           // x: sun scatter strength, y: sky scatter strength, z: scatter spread, w: view alignment strength
    float4 heightFogParams;            // x: SSS height bias, y: SSS fade distance, z: horizon fog distance scale, w: reflection normal strength
    float4 normalSamplingParams;       // x: detail normal mip bias, y: active macro normal mip bias,
                                       // z: FFT cascades in the reflection normal (4 = all)
    // P4: how bright this level's sky is. compose already scaled its sky samples by
    // `skyboxIntensity`; the ocean did not, so the water kept reflecting the raw cube and the
    // control meant two different things depending on which surface you looked at.
    float4 skyParams;                  // x: sky intensity, y: prefiltered mip count (0 = none),
                                       // z: P16.1 pre-exposure -- the water writes into scene
                                       // colour AFTER compose, so it applies the factor itself.
                                       // w: reserved
    float4 sunDirAmbient;              // xyz: sun direction, w: ambient intensity
    float4 sunColorExposure;           // xyz: sun color, w: exposure multiplier
    // P7 height fog, packed by PackHeightFog -- the SAME numbers compose gets.
    float4 fogParams0;                 // x: density, y: height falloff, z: reference height, w: start distance
    float4 fogParams1;                 // x: max opacity, y: sun scatter strength, z: sun scatter exponent, w: sun scatter start
    float4 fogParams2;                 // x: sky blur (roughness at the lightly-fogged end), yzw reserved
    uint fogDebugView;                 // P7 item 8: 0 normal, 1 transmittance, 2 in-scattering
    uint3 _fogDebugPad;
    // Volumetric fog (plan A5): (on, far view depth, 1/preExposure, slice count) and the grid's
    // (B, O, S) -- the SAME numbers compose samples the volume with (SceneRenderer decides once).
    float4 fogVolumeParams;
    float4 fogVolumeZParams;
    float4 deepScatterColor;           // xyz: deep scatter tint, w: unused
    float4 sssColor;                   // xyz: subsurface scattering tint, w: unused
    float4 diffuseColor;               // xyz: diffuse tint, w: unused
    float4 absorptionGradientParams;   // x: color count, y: gradient type (0 = linear, 1 = curved)
    float4 absorptionColors[8];        // gradient color keys (rgb) and position in w
    float4 windParams0;                // x: wind speed, y: waves scale, z: alignment, w: uv warp strength
    float4 windParams1;                // xy: wind direction, z: reference wave height, w: the sun glint's roughness smear (OceanSunSpecular)
    float4 foamTrailParams0;           // xy: trail size 0, zw: trail size 1
    float4 foamTrailParams1;           // xy: trail dir 0, zw: trail dir 1
    float4 foamParams2;                // x: trail blend, y: contact foam strength, z: underwater parallax, w: padding
    float4 foamTint;                   // xyz: foam tint, w: unused
    float4 shoreLegacyDampParams;      // x: vertical damp strength, y: xz damp strength, z: damp fade depth (m), w: shoreline normal fade depth (m)
    float4 shoreNormalMinWeights;      // minimum normal/foam weight per cascade at the shoreline
    float4 shoreLegacyFoamParams;      // x: tail texture scale (tiles/m), y: tail depth (m), z: tail scroll speed (m/s), w: de-tile amount
    float4 shoreLegacyFoamParams2;     // x: tail edge fade (depth units of softness), y: wind thinning amount (0 = off), z: tail contrast (around 0.5), w: tail brightness bias
    float4 shoreLegacyDissipationParams; // foam dissipation injection: x: patch scale (m), y: drift speed (m/s), z: amount (0 = off), w: contrast
    float4 shoreFoamWindParams;        // shared with the modern surface (same C++ feed): x: wind force 0..1, y: calm threshold, z: full threshold
    float4 shoreSdfParams;             // surf sim injection (debug): shared name with the modern surface - x: centre x, y: centre z, z: inv extent, w: texel world size
    float4 surfSimParams;              // surf sim injection: xy: window centre, z: 1 / half extent, w: debug view (0 = off)
    float4 surfSimParams2;             // surf sim injection (S4/S6): x: front breakup, y: tail breakup (0..2), z: tear patch scale (m), w: wave displacement scale
    float4 surfSimParams3;             // x: final foam coverage multiplier, y: cap width (>1 wider dense zone, <1 narrower), zw: spare
    float4 shoreWetnessParams;         // xy: history centre, z: 1 / half extent, w: deposit depth
    float4 shoreWetnessParams2;        // x: wetness edge offset from the SDF waterline (m)
    float4 shoreFoamAlbedoParams;      // x: shore albedo scale, y: shore albedo scroll speed (shared with the modern surface)
    float4 shoreSlopeParams;           // z: edge soft depth = the contact foam's edge fade (shared with the modern surface)
    float4 depthTextureSize;           // xy: texel size, zw: texture size
    float2 depthParams;                // x: zNear / (zNear - zFar) y :(zNear * zFar) / (zFar - zNear)
    // Plan C3: the cloud shadow map's projection and (on, far depth km, 0, 0) -- the SAME numbers the
    // deferred lighting and the fog get (SceneRenderer sets them from the frame's VolumetricCloud).
    float4x4 cloudShadowViewProj;
    float4 cloudShadowParams;          // x: on, as the share by the light's height (0 = off), y: far depth km, z: how much of the shadow the water body takes (cloud.oceanBodyShadow), w: low-sun body light (SunBodyEntry's floor, ocean preset lowSunBodyLight)
    float4 sunShapeParams;             // x: the light's angular RADIUS (rad, UE's half apex) for the sphere-light glint (OceanSunSpecular), y: its soft radius (UE SoftSourceRadius), zw: reserved
    // A boat's wake (ocean/OceanWake, t26, its foam t27): xy window centre, z 1 / half extent (0 = no field this frame), w
    // the height's scale in the vertices; wakeParams2 x texel metres, y the slope's scale in the normal (1 = true), z debug
    // view (1 the height, 2 the foam), w the foam's strength (0 = none); wakeParams3 x the white water's relief (WakeChurn,
    // 0 = a smooth sheet), y seconds since the field's step (ocean.wakeHz: WakeHeight carries the height on by the
    // field's vertical velocity), z metres of water a tile of the foam's detail texture covers (wakeFoamTile), w the same
    // for the churned white water's relief (wakeChurnSize).
    float4 wakeParams;
    float4 wakeParams2;
    float4 wakeParams3;
    float4 wakeParams5;                // x the most of the water the densest wake foam covers (wakeFoamCap), y how far a hexagon's own scale strays from the tile (wakeFoamTileVary: x (1 +- y)), zw reserved
    float4 wakeParams4;                // the wake foam's look (OceanWakeKnobs.h): x the streaks' stretch, y the lace's edge softness, z the boil's speed (m/s), w the aeration's strength
    // x how much of the geometric sun shadow the water's BODY takes (ocean.sunShadowBody, 1 = all, as the glint), yzw reserved.
    float4 bodyShadowParams;
};

Texture2DArray<float4> DisplacementDerivatives : register(t0);
Texture2DArray<float4> PrevDisplacementDerivatives : register(t1);
Texture2DArray<float4> FoamTurbulence : register(t2);
Texture2D SceneColorTexture : register(t3);
TextureCube SkyboxTexture : register(t4);
// P5/F8: the shared environment. `SkySpecular` is the GGX-prefiltered radiance whose mip IS a
// roughness (see ibl_common.hlsli), `SkyIrradiance` is the cosine-convolved fill. Both are inert
// unless skyParams.y says this level's sky was imported with them, in which case the water stops
// guessing a mip off the display cube and reads the same environment every other surface does.
TextureCube SkySpecular : register(t18);
TextureCube SkyIrradiance : register(t19);
Texture3D<float4> FogVolume : register(t20); // volumetric fog plan A5: the integrated froxel volume
Texture2D DistantRoughnessMap : register(t5);
Texture2D FoamDetailMap : register(t6);
Texture2D FoamAlbedoTex : register(t7);
Texture2D FoamUnderwaterTex : register(t8);
Texture2D FoamTrailTex : register(t9);
Texture2D ContactFoamTex : register(t10);
Texture2D ShoreFoamAlbedoTex : register(t11);
Texture2D SceneDepthTexture : register(t12);
Texture2D ShoreDepthTexture : register(t13);
// surf sim injection (debug views): the shore SDF already rides the shared table at t14; the
// legacy surface never read it before. x: metres to the waterline (negative inland), y: depth.
Texture2D<float2> ShoreSdfTexture : register(t14);
// t15 RETIRED (plan S4): the screen-space reflection is traced after this draw and composited by
// Main_WaterReflection. The table is positional, so the slot stays and is filled with a stand-in.
// surf sim injection: the sim's height field (x: height m, y: vertical velocity) and its surf
// foam field (r: coverage).
Texture2D<float2> SurfSimWaveTex : register(t16);
Texture2D<float> SurfSimFoamTex : register(t17);
Texture2D<float4> WakeTex : register(t26); // the boat wake's field: x height (m), y vertical velocity
Texture2D<float4> WakeFoamTex : register(t27); // its foam: x 0..2 (1 and over fresh white water), yz the way it was laid
                                               // times x, w the bow waves' breakers as they stand now
// The wake foam's detail (tools/gen_wake_foam.py, textures/ocean/WakeFoam.png, wakeFoamTile m a tile): R the lace as a gaussianised
// rank, G the churned white water's height, BA its slope along u and v (0.5 + slope / (2 kWakeFoamSlopeMax)).
Texture2D<float4> WakeFoamDetailTex : register(t29);
RWTexture2D<uint> ShoreWetnessStampMap : register(u0);
SamplerState LinearWrapSampler : register(s0);
SamplerState LinearClampSampler : register(s1);
SamplerState PointSampler : register(s2);
SamplerState AnisotropicWrapSampler : register(s3);

// The hulls the surface is not drawn inside (ocean/WaterMask.h, mesh.json "waterMask"; OceanRenderable fills it every
// frame). What keeps the water out is the hulls' LIDS -- their caps and curtains, drawn by Main_WaterMask into the two
// nearest crossings of every pixel (water_mask_lid.hlsl, t30): with the hull's walls they close its basins, so the water is
// inside where the crossings in front of it, counted from the camera's own place, are odd. The hulls' rows and their
// columns (t28, water_mask_columns.hlsli) are the PLAN, only for ocean.waterMask:3.
#define WATER_MASK_MAX_HULLS 4
cbuffer WaterMaskCB : register(b2)
{
    uint4 waterMaskCount; // x hulls this frame, y 1 = discard, 2 = paint the kept-out water magenta (ocean.waterMask:2),
                          // 3 = discard, and paint red the water drawn OVER a hull's plan (a leak, a wave over the deck);
                          // z the boats whose bow wave breakers come in over a gate (wakeBowGate);
                          // w bit 1: the lids were drawn this frame (t30 holds them), bit 0: the camera is inside a basin
    float4 waterMaskHull[WATER_MASK_MAX_HULLS * 5];
    // OceanWake::BowGates, two a boat: (the bow wave's roots' midpoint x z, the hull's forward x z), (the gate's length m,
    // half its lane m, 0, 0).
    float4 wakeBowGate[WATER_MASK_MAX_HULLS * 2];
};
StructuredBuffer<float4> WaterMaskColumns : register(t28);
Texture2DArray<uint> WaterMaskCrossings : register(t30);
#include "water_mask_columns.hlsli"

// How much of the bow waves' breakers (the foam field's w) the water at a point shows: nothing on the line a boat's arms
// leave its hull at, all of them a gate's length aft. The field steps at most 30 times a second and stands on the water
// between two steps while the hull runs on, so a band that started at the hull at full strength (or by its points' age)
// jumped forward with every step; the gate is this FRAME's hull. Beside the boat's lane and well ahead of it: 1 (another
// boat's arms).
float WakeBowGate(float2 xz)
{
    float gate = 1.0f;
    for (uint g = 0u; g < min(waterMaskCount.z, (uint)WATER_MASK_MAX_HULLS); ++g)
    {
        const float4 roots = wakeBowGate[g * 2u];
        const float4 reach = wakeBowGate[g * 2u + 1u];
        const float2 off = xz - roots.xy;
        const float aft = -dot(off, roots.zw);
        const float beside = abs(dot(off, float2(-roots.w, roots.z)));
        const float come = smoothstep(0.0f, max(reach.x, 1e-3f), aft);
        const float elsewhere = max(smoothstep(reach.y, reach.y + 2.0f, beside), smoothstep(3.0f, 6.0f, -aft));
        gate = min(gate, max(come, elsewhere));
    }
    return gate;
}

// Is this pixel's water inside a hull's basin? The lids' crossings in front of it (of the two nearest; a third in front is
// taken as none -- a ray through two basins to a third), plus the camera's own place, odd. Its depth is the view depth the
// lids wrote with (water_mask_lid.hlsl), compared as the bits of a positive float.
bool InsideWaterMask(float4 svPosition, float viewDepth)
{
    const int2 pixel = int2(svPosition.xy);
    const uint depth = asuint(max(viewDepth, 0.0f));
    const uint first = WaterMaskCrossings.Load(int4(pixel, 0, 0));
    const uint second = WaterMaskCrossings.Load(int4(pixel, 1, 0));
    const uint inFront = (first < depth ? 1u : 0u) + (second < depth ? 1u : 0u);
    return ((inFront + waterMaskCount.w) & 1u) != 0u;
}

// ocean.waterMask:3 -- the point is over a hull's plan and not under its lowest surface there: water drawn here is IN the
// boat or over its deck.
bool OverWaterMaskPlan(float3 worldPos)
{
    for (uint h = 0u; h < min(waterMaskCount.x, (uint)WATER_MASK_MAX_HULLS); ++h)
    {
        if (OverHullPlan(worldPos, waterMaskHull[h * 5u + 0u], waterMaskHull[h * 5u + 1u], waterMaskHull[h * 5u + 2u],
                         waterMaskHull[h * 5u + 3u], waterMaskHull[h * 5u + 4u], WaterMaskColumns))
        {
            return true;
        }
    }
    return false;
}

struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float2 baseXZ : TEXCOORD1;
    float4 positionNDC : TEXCOORD2;
    float viewDepth : TEXCOORD3;
    float4 prevPositionNDC : TEXCOORD4;
    float4 positionNDCJitter : TEXCOORD5;
};

struct DerivativesSet
{
    float4 cascades[4];
};

struct FoamInput
{
    DerivativesSet derivatives;
    float2 worldUV;
    float viewDist;
    float4 lodWeights;
    float4 shoreWeights;
    float4 positionNDC;
    float viewDepth;
    float time;
    float3 viewDir;
    float3 normal;
    float shoreMapDepth; // water depth from the shore map at this pixel; 1000 = outside the map
    // Screen derivatives of worldUV, taken at the top of PSMain where control flow is uniform: the
    // foam textures are gradient-sampled with them from inside per-pixel branches, where implicit
    // derivatives are undefined -- which is why they used to be read at mip 0 and shimmered far off.
    float2 worldUVdx;
    float2 worldUVdy;
};

struct FoamData
{
    float2 coverage;
    float aeration; // a boat wake's bubbles under the surface: the underwater foam's colour, not faded with distance
    float3 normal;
    float3 albedo;
};

struct FoamTurbulenceSet
{
    float4 cascades[4];
};

struct LightData
{
    float3 direction;
    float3 color;
    float shadowAttenuation; // the cloud's (plan C3); the body takes cloudShadowParams.z of it
    float geometryShadow;    // the sun's shadow from geometry (OceanSunShadow): every sun term takes it whole
};

struct LightingInput
{
    float3 normal;
    // The normal the REFLECTION's direction is taken about -- the sky's direction, GB1 for the screen-space ray: the same
    // derivatives filtered over 2^w more pixels (normalSamplingParams.w, PSMain; w = 0 = `normal`). `normal` keeps every
    // ripple for the Fresnel weight, the sun's glint, the foam and the body.
    float3 reflectionNormal;
    float3 viewDir;
    float viewDist;
    float roughnessMap;
    float3 positionWS;
    float2 screenUV;
    float4 shore;
    float4 positionNDC;
    float viewDepth;
    float3 cameraPos;
    float height;
    float referenceWaveHeight;
    float slopeFactor;
    LightData mainLight;
    float ambient;
};

struct BrunetonInputs
{
    float3 lightDirWind;
    float3 viewDirWind;
    float3 normalWind;
    float3 tangentXWind;
    float3 tangentYWind;
    float2 slopeVarianceSquared;
};

// P4: was the hardcoded sky the foam was lit by, in every level and at every time of day. Now only
// the reference MAGNITUDE the real-sky path is normalised against; see SkyFillRadiance.
// NOTE: this file is documented above as a byte-faithful copy of 3e54d5d. That contract is broken
// here deliberately and on request -- with `g_shoreRunup = false` as the compiled default this is
// the surface that actually ships, and a "baseline" nobody renders is not worth a hardcoded sky.
static const float3 kLegacyFoamSkyColor = float3(0.24f, 0.38f, 0.55f);
// Keep the presentation boundary in step with the compute shader's 24-texel absorber. The
// camera-following window eventually discards persistent wave/foam data in Relocate; fading the
// sampled result first prevents that loss from reading as a moving square cut.
float SurfSimWindowEdgeFade(float2 simUV)
{
    const float kResolution = 512.0f;
    const float kFadeTexels = 24.0f;
    const float2 edgeUV = min(simUV, 1.0f - simUV);
    const float edgeTexels = min(edgeUV.x, edgeUV.y) * kResolution;
    return smoothstep(0.5f, kFadeTexels, edgeTexels);
}

// The boat wake (ocean/OceanWake). Its window (512 texels round the camera) absorbs over its outer eighth; the fade only
// hides the last texels, where the absorber has already taken the field to nothing.
float2 WakeUV(float2 xz)
{
    return (xz - wakeParams.xy) * (wakeParams.z * 0.5f) + 0.5f;
}
float WakeWindowEdgeFade(float2 wakeUV)
{
    const float2 edgeUV = min(wakeUV, 1.0f - wakeUV);
    return smoothstep(2.0f / 512.0f, 32.0f / 512.0f, min(edgeUV.x, edgeUV.y));
}
// The wake's height at `uv` NOW: the field steps at most ocean.wakeHz times a second (OceanWake), and between steps the
// height goes on by the field's vertical velocity, h + v tau (tau = wakeParams3.y, under 1/20 s). Linear in the field, so
// one bilinear tap of both is the bilinear tap of the sum.
float WakeHeight(float2 uv)
{
    const float2 hv = WakeTex.SampleLevel(LinearClampSampler, uv, 0).xy;
    return hv.x + hv.y * wakeParams3.y;
}
// The wake's slope (dh/dx, dh/dz) at `xz` by central differences a texel apart; 0 off its window or with no field.
float2 WakeSlope(float2 xz)
{
    if (wakeParams.z <= 0.0f || wakeParams2.y <= 0.0f)
    {
        return float2(0.0f, 0.0f);
    }
    const float2 uv = WakeUV(xz);
    if (any(uv < 0.0f) || any(uv > 1.0f))
    {
        return float2(0.0f, 0.0f);
    }
    const float du = wakeParams.z * 0.5f * wakeParams2.x; // one texel in uv
    const float hxp = WakeHeight(uv + float2(du, 0.0f));
    const float hxm = WakeHeight(uv - float2(du, 0.0f));
    const float hzp = WakeHeight(uv + float2(0.0f, du));
    const float hzm = WakeHeight(uv - float2(0.0f, du));
    return float2(hxp - hxm, hzp - hzm) * (0.5f / wakeParams2.x) * wakeParams2.y * WakeWindowEdgeFade(uv);
}

static const uint kGradientMaxKeys = 8u;

struct Gradient
{
    float4 colors[kGradientMaxKeys];
    int colorsCount;
    bool type;
};
#include "ocean_surface_common.hlsli" // the functions both surfaces share (and the cloud shadow read)


float SampleSceneDepth(float2 uv)
{
    return SceneDepthTexture.SampleLevel(PointSampler, uv, 0).r;
}

float SampleShoreDepth(float2 uv)
{
    return ShoreDepthTexture.SampleLevel(LinearClampSampler, uv, 0).r;
}

float ShoreWaterDepthAt(float2 worldXZ)
{
    const float2 uv = ShoreDepthUV(worldXZ);
    if (any(uv < 0.0f) || any(uv > 1.0f))
    {
        return 1000.0f;
    }

    const float shoreDepth = SampleShoreDepth(uv);
    if (shoreDepth <= 0.0f)
    {
        return 1000.0f;
    }

    const float viewDepth = ShoreViewDepth(shoreDepth);
    const float terrainHeight = shoreViewParams.z - viewDepth;
    return -terrainHeight;
}

float4 SampleDerivativesCascade(float2 worldXZ, uint cascade, float mipBias)
{
    float lengthScale = max(cascadeLengthScales[cascade], 1e-3f);
    float3 uvw = float3(worldXZ / lengthScale, cascade * 2.0f + 1.0f);
    //float4 sample = DisplacementDerivatives.SampleLevel(LinearWrapSampler, uvw, 0);
    float4 sample = DisplacementDerivatives.SampleBias(AnisotropicWrapSampler, uvw, mipBias); //give more details far away
    return sample;
}

DerivativesSet SampleDerivatives(float2 worldXZ, float4 weights, uint cascadesCount, float mipBias)
{
    DerivativesSet derivatives;
    [unroll]
    for (uint cascade = 0; cascade < 4; ++cascade)
    {
        derivatives.cascades[cascade] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    [unroll]
    for (uint cascade = 0; cascade < 4; ++cascade)
    {
        if (cascade >= cascadesCount)
        {
            break;
        }

        float w = weights[cascade];
        if (cascade == 0 || w > kLodThreshold)
        {
            derivatives.cascades[cascade] = SampleDerivativesCascade(worldXZ, cascade, mipBias) * w;
        }
    }
    return derivatives;
}

[RootSignature(OCEAN_SURFACE_RS)]
VSOutput VSMain(VSInput input)
{
    VSOutput output;

    uint cascadesCount = max((uint)simulationParams.w, 1u);

    float3 baseWorld = ClipMapVertex(input.position.xyz, input.uv);
    //float3 prevBaseWorld = ClipMapVertexPrev(input.position.xyz, input.uv);

    float2 worldUV = baseWorld.xz;
    //float2 prevWorldUV = prevBaseWorld.xz;

    float3 viewVector = baseWorld - clipMapViewer.xyz;
    //float3 prevViewVector = prevBaseWorld - prevClipMapViewer.xyz;
    float viewDist = length(viewVector);
    //float prevViewDist = length(prevViewVector);
    float viewDistXzSquared = dot(viewVector.xz, viewVector.xz);
    //float prevViewDistXzSquared = dot(prevViewVector.xz, prevViewVector.xz);

    float warpDistance = max(cascadeLengthScales.x, 1.0f) * 0.5f;
    worldUV = ApplyClipMapWarp(worldUV, viewDistXzSquared, warpDistance);
    //prevWorldUV = ApplyClipMapWarp(prevWorldUV, prevViewDistXzSquared, warpDistance);

    float4 weights = LodWeights(viewDist, clipMapParams.w);
    //float4 prevWeights = LodWeights(prevViewDist, prevClipMapParams.w);

    float3 displacement = SampleCurrentDisplacement(worldUV, weights, cascadesCount);
    //float3 prevDisplacement = SamplePreviousDisplacement(prevWorldUV, prevWeights, cascadesCount);

    // Nearshore attenuation, AUTHORED (the second sanctioned edit, see the header note): the
    // original hardcoded `saturate(waterDepth * 0.15)` on the whole displacement vector. Split
    // into separate vertical and XZ fades with sliders for strength and for the depth where the
    // damping begins. Defaults (1 / 1 / 6.67 m) reproduce the original curve exactly.
    float verticalAttenuation = 1.0f;
    float horizontalAttenuation = 1.0f;
    float2 shoreUV = ShoreDepthUV(worldUV);
    float simDisplacementFade = 1.0f;
    if (all(shoreUV >= 0.0f) && all(shoreUV <= 1.0f))
    {
        float shoreDepth = SampleShoreDepth(shoreUV);
        if (shoreDepth > 0.0f)
        {
            float viewDepth = ShoreViewDepth(shoreDepth);
            float terrainHeight = shoreViewParams.z - viewDepth;
            float waterDepth = -terrainHeight;
            float depthFade = saturate(waterDepth / max(shoreLegacyDampParams.z, 0.01f));
            verticalAttenuation = lerp(1.0f - saturate(shoreLegacyDampParams.x), 1.0f, depthFade);
            horizontalAttenuation = lerp(1.0f - saturate(shoreLegacyDampParams.y), 1.0f, depthFade);
            simDisplacementFade = saturate(waterDepth / 0.15f);
        }
    }
    else
    {
        // Outside the shore-depth window: probe the DEPTH BUFFER under the undisplaced vertex
        // and damp by the RAW view-Z gap between the scene and the water plane (same pattern the
        // PS refraction soft edge uses) - Y only, the XZ damping matters at the waterline and
        // the waterline is always inside the window. DELIBERATELY not reconstructed to vertical
        // metres (the user chose the plain gap to evaluate): the gap is along-view metres, so it
        // grows past the true water depth at grazing angles and Damp fade depth reads
        // differently here than in the map branch. Other accepted probe limits: an off-screen
        // vertex gets no answer (full amplitude), and the buffer holds props as well as terrain.
        float4 baseClip = mul(mul(float4(baseWorld.x, 0.0f, baseWorld.z, 1.0f), model), viewProjNoJitter);
        if (baseClip.w > 1e-3f)
        {
            float2 baseNDC = baseClip.xy / baseClip.w;
            if (all(abs(baseNDC) <= 1.0f))
            {
                float2 screenUV = baseNDC * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
                float rawDepth = SampleSceneDepth(screenUV);
                float depthGap = max(DepthToViewZ_Fast(rawDepth) - baseClip.w, 0.0f);
                float depthFade = saturate(depthGap / 15.0f);
                verticalAttenuation = lerp(1.0f - saturate(shoreLegacyDampParams.x), 1.0f, depthFade);
            }
        }
    }
    
    displacement.y *= verticalAttenuation;
    displacement.xz *= horizontalAttenuation;
    //prevDisplacement *= attenuation;

    // surf sim injection (S6): the sim's wave rides the surface — its height field adds to the
    // vertical displacement AFTER the shore damping (the sim wave IS the nearshore wave the
    // damping exists to remove), scaled by the authored knob. Sampled at the same worldUV the
    // PS consumes as baseXZ, so the hump and its foam agree. (A display-side chopness lived
    // here briefly and was removed at the user's call: peak shape must come from the SIM's
    // authored injection - the foam criterion reads the field, cosmetics would lie to it.)
    [branch]
    if (surfSimParams.z > 0.0f && surfSimParams2.w > 0.0f)
    {
        const float2 simUV = (worldUV - surfSimParams.xy) * (surfSimParams.z * 0.5f) + 0.5f;
        if (all(simUV >= 0.0f) && all(simUV <= 1.0f))
        {
            const float simHeight = SurfSimWaveTex.SampleLevel(LinearClampSampler, simUV, 0).x;
            displacement.y += simHeight * surfSimParams2.w * simDisplacementFade *
                SurfSimWindowEdgeFade(simUV);
        }
    }
    // A boat's wake (ocean/OceanWake): its height rides the surface the same way, over the FFT's -- linear waves add.
    [branch]
    if (wakeParams.z > 0.0f && wakeParams.w > 0.0f)
    {
        const float2 wakeUV = WakeUV(worldUV);
        if (all(wakeUV >= 0.0f) && all(wakeUV <= 1.0f))
        {
            displacement.y += WakeHeight(wakeUV) * wakeParams.w * simDisplacementFade * WakeWindowEdgeFade(wakeUV);
        }
    }

    float3 world = float3(baseWorld.x + displacement.x, displacement.y, baseWorld.z + displacement.z);
    //float3 prevWorldPos = float3(prevBaseWorld.x + prevDisplacement.x, prevDisplacement.y, prevBaseWorld.z + prevDisplacement.z);
    float3 prevWorldPos = world;
    output.worldPos = world;

    output.baseXZ = worldUV;

    float4 local = float4(world, 1.0f);
    float4 worldH = mul(local, model);
    float4 viewPos = mul(worldH, view);
    output.viewDepth = viewPos.z;
    float4 clipPos = mul(worldH, viewProj);
    output.position = clipPos;
    output.positionNDC = mul(worldH, viewProjNoJitter);
    output.positionNDCJitter = mul(worldH, viewProj);
    float4 prevLocal = float4(prevWorldPos, 1.0f);
    float4 prevWorld = mul(prevLocal, prevModel);
    output.prevPositionNDC = mul(prevWorld, prevViewProjNoJitter);
    return output;
}

// foam dissipation injection (see docs/ocean_shore_foam_breakup_plan.md, variant A); the other
// two touch points are the shoreLegacyDissipationParams cbuffer field and one multiply below.
#include "ocean_shore_foam_dissipation.hlsli"

float ContactFoam(float4 positionNDC, float viewDepth, float2 worldUV, float2 worldDx, float2 worldDy,
                  float shoreMapDepth)
{
    // AUTHORED since the June original (sanctioned edit, see the header note). Two changes of
    // substance on top of the June formula:
    //   - THE DEPTH SOURCE: the shore map's water depth wherever the pixel is inside the map,
    //     and only outside it the depth buffer - reconstructed to a WORLD position so both
    //     branches measure the same thing, vertical metres of water, instead of the June
    //     along-ray separation whose scale swung with the camera angle.
    //   - THE TAIL: its texture has a scale, drifts with the wind, and an optional rotated
    //     second octave breaks the tiling.
    // The coverage math itself is the June shape, all knobs authored: the texture eats the
    // distance, the reach is Tail depth, the softness is Tail edge fade.
    float waterDepth;
    [branch]
    if (shoreMapDepth < 999.0f)
    {
        waterDepth = max(shoreMapDepth, 0.0f);
    }
    else
    {
        float2 screenUV = positionNDC.xy / max(positionNDC.w, 1e-5f);
        screenUV = screenUV * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
        float rawDepth = SampleSceneDepth(screenUV);
        float3 scenePositionWS = PositionWsFromDepth(rawDepth, screenUV);
        waterDepth = max(-scenePositionWS.y, 0.0f);
    }

    float2 windDirection = windParams1.xy;
    float directionLengthSquared = dot(windDirection, windDirection);
    windDirection = directionLengthSquared > 1e-8f
        ? windDirection * rsqrt(directionLengthSquared)
        : float2(1.0f, 0.0f);
    const float tailScale = max(shoreLegacyFoamParams.x, 1e-3f);
    float2 tailUV =
        (worldUV - windDirection * simulationParams.z * max(shoreLegacyFoamParams.z, 0.0f)) * tailScale;
    const float2 tailDx = worldDx * tailScale;
    const float2 tailDy = worldDy * tailScale;
    float tail = ContactFoamTex.SampleGrad(AnisotropicWrapSampler, tailUV, tailDx, tailDy).r;
    [branch]
    if (shoreLegacyFoamParams.w > 1e-3f)
    {
        // Second octave: same texture, rotated ~37 degrees and rescaled by an irrational-ish
        // factor, so the two grids never line up and the repeat period stops reading.
        float2 rotatedUV =
            float2(tailUV.x * 0.8f - tailUV.y * 0.6f,
                   tailUV.x * 0.6f + tailUV.y * 0.8f) * 0.531f + 17.31f;
        const float2 rotDx = float2(tailDx.x * 0.8f - tailDx.y * 0.6f, tailDx.x * 0.6f + tailDx.y * 0.8f) * 0.531f;
        const float2 rotDy = float2(tailDy.x * 0.8f - tailDy.y * 0.6f, tailDy.x * 0.6f + tailDy.y * 0.8f) * 0.531f;
        float tail2 = ContactFoamTex.SampleGrad(AnisotropicWrapSampler, rotatedUV, rotDx, rotDy).r;
        tail = lerp(tail, saturate(tail + tail2 - 0.5f), saturate(shoreLegacyFoamParams.w));
    }

    // Authored remap of the texel BEFORE it eats the depth. A texel of brightness t dies at
    // TailDepth / (1 - t), so the top of the brightness distribution decides how far the bright
    // tongues outrun the dark ones - the dissipation length. Contrast stretches/squashes that
    // spread around mid-grey, bias shifts the whole distribution; pulling the top down gives the
    // tail a finite dissipation depth of about TailDepth / (1 - maxTexel). Defaults (1, 0) are
    // the identity - the June behaviour.
    tail = saturate(
        (tail - 0.5f) * max(shoreLegacyFoamParams2.z, 0.0f) + 0.5f + shoreLegacyFoamParams2.w);

    float contactTexture = saturate(1.0f - tail);
    float effectiveDepth = waterDepth * contactTexture;
    // foam breakup injection: dissipation patches x wind thinning squeeze the depth threshold,
    // so foam geometrically vanishes instead of alpha-fading.
    float breakup = ShoreFoamBreakupThresholdFactor(
        ContactFoamTex, AnisotropicWrapSampler, worldUV, worldDx, worldDy, simulationParams.z,
        ShoreFoamWindAmount(shoreFoamWindParams), shoreLegacyFoamParams2.y,
        shoreLegacyDissipationParams);
    float coverage = saturate(
        (max(shoreLegacyFoamParams.y, 0.0f) * breakup - effectiveDepth) /
        max(shoreLegacyFoamParams2.x, 1e-3f));
    // The strength slider keeps its June default (0.1) reading as full intensity.
    return coverage * saturate(foamParams2.y * 10.0f);
}

// surf sim injection (S4): coverage from the surf sim's breaking-foam field. The field itself
// is a SMOOTH physical quantity — the sim never tears it, because at its ~1 m texels any
// pattern aliases into per-texel noise (soft squares under magnification). The tear happens
// here, per PIXEL, as a THRESHOLD the decaying foam sinks through — a fresh stamp (>= 1) is
// solid whitewater, dissolving foam breaks into patches that vanish darkest-first, so
// dissipation reads as structure instead of an alpha fade.
// FRONT and TAIL are torn by SEPARATE amounts: the foam value doubles as the age proxy
// (fresh >= 1, dissipation sinks it toward 0), so the tear amount blends from
// surfSimParams2.x on the leading front to surfSimParams2.y on the decayed tail. The pattern
// is the shore dissipation include's counter-drifting two-octave field — the SAME breathing
// patches that tear the contact rim — at the authored patch scale (surfSimParams2.z, metres),
// so the tear itself is alive instead of a frozen stencil.
float SurfSimFoamCoverage(float2 baseXZ, float2 baseDx, float2 baseDy)
{
    [branch]
    if (surfSimParams.z <= 0.0f || surfSimParams3.x <= 0.0f)
    {
        return 0.0f; // sim off or foam hidden - zero cost beyond the uniform branch
    }
    float2 simUV = (baseXZ - surfSimParams.xy) * (surfSimParams.z * 0.5f) + 0.5f;
    if (any(simUV < 0.0f) || any(simUV > 1.0f))
    {
        return 0.0f;
    }
    const float kHalfTexel = 0.5f / 512.0f;
    const float foam = 0.25f *
        (SurfSimFoamTex.SampleLevel(LinearClampSampler, simUV + float2(-kHalfTexel, -kHalfTexel), 0) +
         SurfSimFoamTex.SampleLevel(LinearClampSampler, simUV + float2(kHalfTexel, -kHalfTexel), 0) +
         SurfSimFoamTex.SampleLevel(LinearClampSampler, simUV + float2(-kHalfTexel, kHalfTexel), 0) +
         SurfSimFoamTex.SampleLevel(LinearClampSampler, simUV + float2(kHalfTexel, kHalfTexel), 0));
    
    if (foam <= 1e-3f)
    {
        return 0.0f;
    }
    // Front and tail torn by SEPARATE amounts: the foam value is the age proxy (a fresh stamp
    // is >= 1, dissipation sinks it toward 0), so the tear amount blends from the front knob on
    // fresh foam to the tail knob on decayed foam. The pattern is the shore dissipation
    // include's counter-drifting two-octave field (the same "breathing patches" that tear the
    // contact rim) at the authored patch scale - a moving threshold the decaying foam sinks
    // through: patches vanish darkest-first and the tear itself is alive.
    const float kDriftSpeed = 0.12f; // m/s, fixed like the include's shore usage
    const float kContrast = 1.6f;
    const float pattern = 1.0f - ShoreFoamDissipationFactor(
        ContactFoamTex, AnisotropicWrapSampler, baseXZ, baseDx, baseDy, simulationParams.z,
        float4(max(surfSimParams2.z, 1.0f), kDriftSpeed, 1.0f, kContrast));
    // Tail breakup runs 0..2 (NOT saturated): above 1 the threshold climbs past the pattern's
    // range, so even mid-fresh foam tears - the "рвать сильнее" headroom. Cap width bends the
    // value-age curve: > 1 keeps mid-fresh foam on the FRONT amount longer (wider dense
    // zone), < 1 hands it to the tail sooner (narrower).
    const float age = pow(saturate(foam), 1.0f / max(surfSimParams3.y, 0.05f));
    const float tearAmount =
        lerp(max(surfSimParams2.y, 0.0f), saturate(surfSimParams2.x), age);
    const float threshold = tearAmount * pattern;
    const float coverage = saturate((foam - threshold) / max(1.0f - threshold, 1e-3f));
    return saturate(coverage * SurfSimWindowEdgeFade(simUV) * surfSimParams3.x);
}

// ---- A boat's wake foam (ocean/OceanWake): the white water and trail a moving hull leaves, its bow wave's two arms, the
// steep crests ----
// The field is smooth at its 0.5 m texels; its value is read as the SHARE of the water the foam covers, not as an opacity:
// a pixel is foam where the value's rank exceeds the LACE's there -- WakeFoamDetailTex.r (tools/gen_wake_foam.py): filaments
// at three scales with rafts of bubbles between them, equalised so a value v covers the share v, stored gaussianised --
// so 1 and over covers the water and 0.2 leaves the stoutest filaments, the lace a dissolving trail is in the photographs
// (as an opacity the trail was a solid sheet: owner, "the trail is incredibly dense").
//
// NO REPEAT (2026-11-10, owner: "even the tile shows on the trail"): the lace was the contact foam's noise at a 1.8 m
// tile (its streaks at 6.3 m), the churn's relief the same texture at 0.7 and 0.3 m and the foam's albedo a 1 m tile --
// three grids on a trail tens of metres long. Now a hex tiling (Mikkelsen 2022, "Practical Real-Time Hex-Tiling"): the
// water is cut into hexagons (kWakeFoamHexShare of the tile across), each shows its own window of the tile (wakeParams3.z
// metres of water: OceanRenderConfig::wakeFoamTile, 3 by default; a random offset, drifting
// its own way: neighbours slide against each other and the foam boils), and a pixel blends the three nearest. The lace
// is blended as Heitz & Neyret blend (2018): gaussian values, the variance kept, so the blend has the tile's own
// contrast and the share a value covers stays that value.
// STREAKS: a window is drawn out wakeParams4.x times along the way the foam at its hexagon was laid (the field's yz), a
// pure stretch along that way, no turn -- a window turned with the way sheared wherever the way bent, and popped when a
// hexagon's first foam gave it a way. Round where nothing says which way (the crests' foam).
// FAR AWAY the texture's mips lose the lace's variance and the cover goes to the value itself (the share): the edge's
// softness is the variance lost (measured on the tile's mip chain), in the gaussian's units.
// The AERATION is the bubbles a trail leaves under the surface, the lighter turquoise scar that outlasts the white, seen
// from a drone as well as from the boat (FoamData.aeration).
static const float kWakeFoamHexShare = 0.475f; // the hexagons' spacing, a share of the tile (wakeParams3.z m of water)
static const float kWakeFoamSlopeMax = 160.0f; // tools/gen_wake_foam.py SLOPE_MAX -- MUST MATCH
// wakeParams4: x a streak this many times longer than wide (3.5), y the lace's edge in the gaussian's units (0.14:
// 10-90 % over 2.56 of it), z m/s a hexagon's window drifts, each its own way (0.06), w the aeration's strength (0.4).

uint WakeHashU(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float4 WakeCellRandom(int2 cell)
{
    const uint h = WakeHashU((uint)cell.x * 0x9E3779B1u + WakeHashU((uint)cell.y));
    const uint h2 = WakeHashU(h + 0x68E31DA4u);
    return float4((float)(h & 0xFFFFu), (float)(h >> 16), (float)(h2 & 0xFFFFu), (float)(h2 >> 16)) * (1.0f / 65536.0f);
}

// What the wake's foam gives a pixel (WakeFoamCoverage).
struct WakeFoamSample
{
    float coverage; // the share of the pixel the foam covers
    float aeration; // the bubbles under it: the underwater foam's colour (FoamData.aeration)
    float churn;    // how much of it is churned white water, 0..1 -- it wears the relief below
    float height;   // the churned white water's relief, 0..1 (lumps with creases between them)
    float2 slope;   // its slope, heights per metre along world x and z
};

// One hexagon's window of the detail texture at this pixel: x the lace (the gaussian's units), y the relief's height,
// zw its slope (heights per metre, world x z); `lod` the mip the lace is read at. The relief is read only where the
// pixel wears it (`relief`: fresh white water), at ITS OWN scale (wakeParams3.w m a tile, at least the lace's): at the
// lace's it was a grain of centimetres, and a slab of solid white water behind a turning hull read as plaster (owner:
// "this cringe patch with the bump").
float4 WakeFoamWindow(int2 cell, float2 baseXZ, float2 baseDx, float2 baseDy, bool relief, out float lod)
{
    const float tile = max(wakeParams3.z, 0.05f);
    const float2 cellXZ = float2((float)cell.x + 0.5f * (float)cell.y, 0.8660254f * (float)cell.y) * (tile * kWakeFoamHexShare);
    const float4 random = WakeCellRandom(cell);
    // The way the foam at the hexagon was laid: the window is squeezed along it (S = I + squeeze way way^T, symmetric).
    const float4 laid = WakeFoamTex.SampleLevel(LinearClampSampler, WakeUV(cellXZ), 0);
    const float wayLength = length(laid.yz);
    const float2 way = wayLength > 1e-5f ? laid.yz / wayLength : float2(1.0f, 0.0f);
    const float oneWay = saturate(wayLength / max(laid.x, 1e-4f)) * saturate(laid.x * 8.0f);
    const float squeeze = 1.0f / lerp(1.0f, max(wakeParams4.x, 1.0f), oneWay) - 1.0f;
    // Each hexagon draws its window at its own scale, the tile x (1 +- wakeParams5.y) (owner: "not one tiling, +-15 %
    // for different parts of the tail"): the lace is finer here and coarser there, and no seam -- the blends are the
    // tiling's own. The relief takes the same factor.
    const float own = 1.0f + (random.w * 2.0f - 1.0f) * clamp(wakeParams5.y, 0.0f, 0.4f);
    const float invTile = 1.0f / (tile * own);
    const float2 local = baseXZ - cellXZ;
    float s, c;
    sincos(random.z * 6.2831853f, s, c);
    // The window's metres: squeezed along the way, drifting.
    const float2 metres = local + way * (squeeze * dot(way, local)) + float2(c, s) * (wakeParams4.z * simulationParams.z);
    const float2 metresDx = baseDx + way * (squeeze * dot(way, baseDx));
    const float2 metresDy = baseDy + way * (squeeze * dot(way, baseDy));
    const float2 uvDx = metresDx * invTile;
    const float2 uvDy = metresDy * invTile;
    const float lace = (WakeFoamDetailTex.SampleGrad(AnisotropicWrapSampler, metres * invTile + random.xy, uvDx, uvDy).r - 0.5f) * 6.0f;
    lod = log2(max(sqrt(length(uvDx) * length(uvDy)) * 1024.0f, 1.0f));
    float height = 0.0f;
    float2 slope = float2(0.0f, 0.0f);
    [branch]
    if (relief)
    {
        const float invRelief = 1.0f / (max(wakeParams3.w, tile) * own);
        const float4 t = WakeFoamDetailTex.SampleGrad(AnisotropicWrapSampler, metres * invRelief + random.yx + 0.37f,
                                                     metresDx * invRelief, metresDy * invRelief);
        const float2 slopeUv = (t.ba - 0.5f) * (2.0f * kWakeFoamSlopeMax);             // heights per tile, along u and v
        slope = (slopeUv + way * (squeeze * dot(way, slopeUv))) * invRelief;          // S^T = S: heights per metre, world
        height = t.g;
    }
    return float4(lace, height, slope);
}

WakeFoamSample WakeFoamCoverage(float2 baseXZ, float2 baseDx, float2 baseDy)
{
    WakeFoamSample result;
    result.coverage = 0.0f;
    result.aeration = 0.0f;
    result.churn = 0.0f;
    result.height = 0.0f;
    result.slope = float2(0.0f, 0.0f);
    [branch]
    if (wakeParams.z <= 0.0f || wakeParams2.w <= 0.0f)
    {
        return result;
    }
    const float2 wakeUV = WakeUV(baseXZ);
    if (any(wakeUV < 0.0f) || any(wakeUV > 1.0f))
    {
        return result;
    }
    // x the foam that stays (the trail, the lace), w the bow waves' breakers as they stand now (the two arms).
    const float4 laid = WakeFoamTex.SampleLevel(LinearClampSampler, wakeUV, 0);
    const float foam = laid.x + laid.w * WakeBowGate(baseXZ);
    if (foam <= 1e-3f)
    {
        return result;
    }
    // The three hexagons round the pixel and their weights (the triangle grid's barycentrics), sharpened.
    const float2 skewed = float2(baseXZ.x - 0.57735027f * baseXZ.y, 1.15470054f * baseXZ.y) /
                          (max(wakeParams3.z, 0.05f) * kWakeFoamHexShare);
    const int2 base = (int2)floor(skewed);
    const float2 f = frac(skewed);
    const float upper = step(1.0f, f.x + f.y); // which of the cell's two triangles
    const float flip = 2.0f * upper - 1.0f;
    const float z3 = 1.0f - f.x - f.y;
    float3 weights = float3(-z3 * flip, upper - f.y * flip, upper - f.x * flip);
    const int iu = (int)upper;
    const int2 cell0 = base + int2(iu, iu);
    const int2 cell1 = base + int2(iu, 1 - iu);
    const int2 cell2 = base + int2(1 - iu, iu);
    weights = weights * weights;
    weights = weights * weights;
    weights /= weights.x + weights.y + weights.z;
    // Churned where it is fresh white water (1 and over); the lace of a dissolving trail is a flat film.
    const float churned = smoothstep(0.4f, 1.2f, foam);
    const bool relief = churned > 0.0f && wakeParams3.x > 0.0f;
    float lod0, lod1, lod2;
    const float4 t0 = WakeFoamWindow(cell0, baseXZ, baseDx, baseDy, relief, lod0);
    const float4 t1 = WakeFoamWindow(cell1, baseXZ, baseDx, baseDy, relief, lod1);
    const float4 t2 = WakeFoamWindow(cell2, baseXZ, baseDx, baseDy, relief, lod2);
    // The lace: gaussian values blended with their variance kept.
    const float lace = (weights.x * t0.x + weights.y * t1.x + weights.z * t2.x) * rsqrt(dot(weights, weights));
    // The variance the mips have lost (the tile's own chain: its deviation is 1, 0.93, 0.75, 0.45, 0.18 at mips 0, 2, 4, 6, 8)
    // is the variance of the foam inside the pixel: it widens the edge, and with all of it lost the cover is the value.
    const float lod = weights.x * lod0 + weights.y * lod1 + weights.z * lod2;
    const float kept = saturate(1.0f - pow(lod * (1.0f / 8.5f), 1.6f));
    const float softness = max(wakeParams4.y, 0.01f);
    const float spread = sqrt(1.0f - kept * kept + softness * softness);
    // The value's rank in the gaussian's units (the logistic's inverse), and the normal CDF as a logistic. The densest
    // foam covers wakeParams5.x of the water at most (wakeFoamCap, 0.93): the clearest few per cent of the lace stay
    // holes, milky water in them -- froth. Whole (1), fresh white water was a slab of one white.
    const float share = clamp(foam, 0.002f, min(wakeParams5.x, 0.998f));
    const float rank = log(share / (1.0f - share)) * (1.0f / 1.702f);
    const float coverage = 1.0f / (1.0f + exp(-1.702f * (rank - lace) / spread));
    const float edge = WakeWindowEdgeFade(wakeUV) * wakeParams2.w;
    result.coverage = saturate(coverage * edge);
    // The aeration goes with the foam, linearly: sqrt(foam) lifted the trace the field's spreading leaves round the lace
    // (a few hundredths) into a milky halo wider than the trail (owner: "what is this whitish halo round the foam?").
    result.aeration = saturate(foam * 2.0f) * wakeParams4.w * edge;
    result.churn = churned * result.coverage;
    // The churned white water's relief (photographs of a prop wash: boils of half a metre with smaller lumps on them and
    // grey creases between, not a smooth white sheet), from the same three windows at the relief's own scale: the height
    // and its slope, baked into the texture -- their mips average a far pixel's to a mean grey and no relief, as it
    // should be.
    result.height = weights.x * t0.y + weights.y * t1.y + weights.z * t2.y;
    result.slope = weights.x * t0.zw + weights.y * t1.zw + weights.z * t2.zw;
    return result;
}

FoamData GetFoamData(FoamInput input, uint cascadesCount)
{
    FoamData data;
    data.coverage = float2(0.0f, 0.0f);
    data.aeration = 0.0f;
    data.normal = input.normal;
    data.albedo = float3(1.0f, 1.0f, 1.0f);

    float4 activeCascades = ActiveCascadesMask(cascadesCount);
    FoamTurbulenceSet turbulence = SampleFoamTurbulence(input.worldUV, input.lodWeights * input.shoreWeights, cascadesCount);
    float4 mixWeights = input.lodWeights * activeCascades;

    float biasSample = FoamDetailMap.SampleLevel(LinearWrapSampler, input.worldUV * 0.01f * 0.01f, 0).r;
    float bias = biasSample * saturate(input.viewDist / max(simulationParams.x, 1.0f) * 0.5f);
    
    //data.coverage.x = bias;
    //return data;

    float deepFoam = DeepFoam(input.worldUV, input.viewDir, input.normal, input.time);
    data.coverage = Coverage(turbulence, mixWeights, input.worldUV, deepFoam, bias);
    // A boat's wake foam joins the whitecaps (their albedo): it is white water on open sea, not the shore's. Its
    // aeration joins the underwater foam.
    const WakeFoamSample wakeFoam = WakeFoamCoverage(input.worldUV, input.worldUVdx, input.worldUVdy);
    data.coverage.x = saturate(data.coverage.x + wakeFoam.coverage);
    data.aeration = wakeFoam.aeration;

    float contactCoverage = 0.0f;
    if (foamParams2.y > 0.0f)
    {
        contactCoverage = ContactFoam(input.positionNDC, input.viewDepth, input.worldUV,
                                      input.worldUVdx, input.worldUVdy, input.shoreMapDepth);
    }
    // surf sim injection (S4): the sim's breaking foam joins as another additive shore
    // coverage source - same slot as contact foam, so it wears the shore foam albedo below.
    contactCoverage = saturate(contactCoverage +
                               SurfSimFoamCoverage(input.worldUV, input.worldUVdx, input.worldUVdy));
    data.coverage.x = saturate(data.coverage.x + contactCoverage);

    float4 foamNormalWeights = saturate(float4(1.0f, 0.66f, 0.33f, 0.0f) + foamParams1.w) * activeCascades;
    float3 foamNormal = NormalFromDerivatives(input.derivatives, foamNormalWeights);
    data.normal = foamNormal;

    // Gradient samples (derivatives from the top of PSMain): at mip 0 this 1-metre tile shimmered
    // in the distance without DLSS.
    float2 uv = input.worldUV * 1.0f;
    data.albedo = FoamAlbedoTex.SampleGrad(AnisotropicWrapSampler, uv, input.worldUVdx, input.worldUVdy).rgb;
    // The wake's own white (not the whitecaps' 1 m tile, which showed as a grid on a trail), and its churned white water:
    // its lumps tilt the foam's normal (the sun lights their faces, a low sun leaves their backs grey) and its creases are
    // darker (in the lumps' shadow, seeing less sky) -- by the wake's share of the foam.
    [branch]
    if (wakeFoam.coverage > 1e-3f)
    {
        const float wakeShare = saturate(wakeFoam.coverage / max(data.coverage.x, 1e-3f));
        data.albedo = lerp(data.albedo, float3(1.0f, 1.0f, 1.0f), wakeShare);
        const float w = wakeFoam.churn * saturate(wakeParams3.x) * wakeShare;
        // 30 % under the first cut twice (owner, 2026-11-07: "take the normal down 30 %"; 2026-11-09: "another 30 %"): the
        // first cut's 15 cm lumps and crevices at 0.65 -> 10.5 cm / 0.755 -> 7.35 cm / 0.8285.
        const float kRelief = 0.0735f; // m: the lumps' height
        data.normal = normalize(data.normal - float3(wakeFoam.slope.x, 0.0f, wakeFoam.slope.y) * (kRelief * w));
        data.albedo *= lerp(1.0f, lerp(0.8285f, 1.0f, smoothstep(0.0f, 0.5f, wakeFoam.height)), w);
    }
    // The shore strip wears the SHORE foam albedo (same asset and sliders as the modern
    // surface), blended in by its share of the total coverage so simulated whitecap foam
    // keeps its own look.
    [branch]
    if (contactCoverage > 1e-3f)
    {
        float2 windDirection = windParams1.xy;
        float directionLengthSquared = dot(windDirection, windDirection);
        windDirection = directionLengthSquared > 1e-8f
            ? windDirection * rsqrt(directionLengthSquared)
            : float2(1.0f, 0.0f);
        const float shoreAlbedoScale = max(shoreFoamAlbedoParams.x, 1e-3f);
        float2 shoreAlbedoUV =
            (input.worldUV -
             windDirection * input.time * max(shoreFoamAlbedoParams.y, 0.0f)) * shoreAlbedoScale;
        float3 shoreAlbedo = ShoreFoamAlbedoTex.SampleGrad(AnisotropicWrapSampler, shoreAlbedoUV,
            input.worldUVdx * shoreAlbedoScale, input.worldUVdy * shoreAlbedoScale).rgb;
        float shoreMix = saturate(contactCoverage / max(data.coverage.x, 1e-3f));
        data.albedo = lerp(data.albedo, shoreAlbedo, shoreMix);
    }
    return data;
}

// Sky radiance for a surface with this normal: the blurriest mip of the REAL cubemap in the
// normal's direction, as a crude irradiance lookup. Normalised to the luminance of the old constant
// so the sky decides hue and response while the level's existing foam tuning keeps deciding
// brightness -- an HDR skybox can sit anywhere on the scale and its raw magnitude would rescale
// every foam pixel on every level. Kept identical to the modern variant in ocean_surface.hlsl.
float3 SkyFillRadiance(float3 normal)
{
    // P5: the real cosine-convolved irradiance when this sky has it. That is what "the sky arriving
    // at a surface with this normal" actually means -- the blurriest mip of the DISPLAY cube was
    // only ever an impression of it, and it is the last place the water guessed instead of reading
    // the shared environment.
    const float3 sky = (skyParams.y > 0.0f)
        ? SkyIrradiance.SampleLevel(LinearClampSampler, normal, 0).rgb * skyParams.x
        : SkyboxTexture.SampleLevel(LinearClampSampler, normal, 5.0f).rgb * skyParams.x;
    // An ABSENT cubemap falls back to the old constant (a level without a skybox keeps lit foam); a
    // measured sky that is black is the truth -- see ocean_surface_runup.hlsli (time-of-day N4).
    const float lum = dot(sky, float3(0.2126f, 0.7152f, 0.0722f));
    return (skyParams.y > 0.0f || lum > 1e-5f) ? sky : kLegacyFoamSkyColor;
}

float3 LitFoamColor(const LightingInput li, const FoamData foamData)
{
    const float bodyShadow = lerp(1.0f, li.mainLight.shadowAttenuation, saturate(cloudShadowParams.z))
                           * li.mainLight.geometryShadow;
    float ndotl = (0.2f + 0.8f * saturate(dot(foamData.normal, -li.mainLight.direction))) * bodyShadow;
    // P16.7: `SkyFillRadiance` is the sky's MEASURED irradiance; `li.ambient` is the legacy
    // FRACTION-OF-THE-SUN knob (0.05-0.15). Multiplying one by the other is the exact mistake F8
    // documents about its own first version -- "multiplying an absolute measured irradiance by a
    // level's 0.05 buries the fill about twenty times too deep" -- and it survived here because
    // foam is the one thing the ocean lights with the sky. The directional term stays: it is a
    // shape (upward-facing foam sees more sky), not a strength.
    float3 skyAmbient = SkyFillRadiance(foamData.normal) * (1.0f + 0.3f * (1.0f - foamData.normal.y));
    // THE TWO HALVES OF THIS SUM MUST BE THE SAME QUANTITY, and `kInvPi` is what makes them so.
    // `skyAmbient` is already a RADIANCE -- SkyFillRadiance reads the cosine-convolved irradiance
    // cube, E/pi, which is why Refraction's comment calls it "E/pi in the sun colour's units".
    // `li.mainLight.color` is the opposite: an ILLUMINANCE, the same `GetEffectiveColor()` lux that
    // lighting_cs binds as `lightRgb`. Adding one to the other without the 1/pi made the sun half
    // pi times the sky half, and pi times what every other diffuse surface in the engine gets --
    // the Lambertian everywhere else is `albedo * kInvPi * NdotL * lightRgb` (`utils.hlsli:301`
    // feeding `lighting_cs.hlsl:385`). Foam is near-white, so 1.65 stops of extra gain put it at
    // the tone curve's white point and it started reading as a specular highlight rather than as
    // wet white cloth. The wrap term (0.2 + 0.8*NdotL) stays: it is a shape, not a strength.
    return foamData.albedo * foamTint.rgb * (ndotl * li.mainLight.color * kInvPi + skyAmbient);
}

// Bruneton's slope variance the normal does not resolve at `dist` (per wind axis), scaled by the preset's roughness.
float2 OceanUnresolvedSlopeVariance(const LightingInput li, float dist)
{
    float windSpeed = max(windParams0.x, 0.0f);
    float wavesScale = max(windParams0.y, 0.0f);
    float alignment = windParams0.z;
    float roughScale = max(specularParams.y, 0.0f);
    return roughScale * (1.0f + li.roughnessMap * 0.3f)
        * SlopeVarianceSquared(windSpeed * wavesScale, dist, alignment, max(specularParams.z, 1.0f));
}

BrunetonInputs BuildBrunetonInputs(const LightingInput li)
{
    float3 tangentY = float3(0.0f, li.normal.z, -li.normal.y);
    tangentY /= max(0.001f, length(tangentY));
    float3 tangentX = cross(tangentY, li.normal);

    BrunetonInputs bi;
    bi.lightDirWind = TransformToWind(-li.mainLight.direction);
    bi.viewDirWind = TransformToWind(li.viewDir);
    bi.normalWind = TransformToWind(li.normal);
    bi.tangentXWind = TransformToWind(tangentX);
    bi.tangentYWind = TransformToWind(tangentY);

    bi.slopeVarianceSquared = OceanUnresolvedSlopeVariance(li, li.viewDist);
    return bi;
}

// The REFLECTION's Bruneton inputs: its filtered normal, and the slope variance that normal leaves unresolved -- the
// same distance law at the distance whose pixel is as wide as the filter (2^w the pixel: the footprint grows with the
// distance, so the filter is the water seen 2^w times farther). w = 0 is the sun's inputs exactly.
BrunetonInputs BuildReflectionBrunetonInputs(const LightingInput li, const BrunetonInputs sun)
{
    BrunetonInputs bi = sun;
    bi.normalWind = TransformToWind(li.reflectionNormal);
    bi.slopeVarianceSquared = OceanUnresolvedSlopeVariance(li, li.viewDist * exp2(max(normalSamplingParams.w, 0.0f)));
    return bi;
}

// P5: takes a roughness now. The legacy surface has no per-pixel roughness of its own -- it is a
// Bruneton model -- but it does carry the physical quantity roughness stands for: the slope
// VARIANCE of the microfacet distribution. sqrt of it is an RMS slope, which is what maps onto a
// GGX alpha, so the caller passes that instead of the fixed mip 3 this used to blur by.
// The normal the reflection is taken about: the reflection's filtered normal tilted toward +Y by the reflection
// normal strength. The screen-space trace starts from the same one (GB1, OceanGBuffer, plan S3).
float3 OceanReflectionNormal(const LightingInput li)
{
    float reflectionNormalStrength = heightFogParams.w;
    return normalize(lerp(li.reflectionNormal, float3(0.0f, 1.0f, 0.0f), reflectionNormalStrength));
}

float3 Reflection(const LightingInput li, float roughness)
{
    float3 adjustedNormal = OceanReflectionNormal(li);
    float3 reflectDir = OceanSkyReflectDir(reflect(-li.viewDir, adjustedNormal));
    //reflectDir.y = max(reflectDir.y, 0.001f);
    reflectDir.y = abs(reflectDir.y);

    // P5: see the modern variant. The `3` here was a fixed blur with no relation to roughness.
    float3 skySample;
    if (skyParams.y > 0.0f)
    {
        const float specMip = IblMipFromRoughness(roughness, skyParams.y);
        // Bounded by the sharp sample in the same direction: the water must reflect the SKY.
        skySample = IblClampToSharp(
            SkySpecular.SampleLevel(LinearClampSampler, reflectDir, specMip).rgb,
            SkySpecular.SampleLevel(LinearClampSampler, reflectDir, 0.0f).rgb) * skyParams.x;
    }
    else
    {
        skySample = SkyboxTexture.SampleLevel(LinearClampSampler, reflectDir, 3).rgb * skyParams.x;
    }
    // Plan S4: the sky alone. The screen-space hit replaces it later, in water_composite_cs.hlsl,
    // using the weight and this very sample (GB2) that the G-buffer layer carries there.
    return skySample;
}

float3 RefractionCoords(float refractionStrength, float4 positionNDC, float viewDepth, float3 normal)
{
    float2 uvOffset = normal.xz * refractionStrength;
    uvOffset.y *= depthTextureSize.z * abs(depthTextureSize.y);

    float2 refractedUV = ((positionNDC.xy + uvOffset) / positionNDC.w);
    refractedUV = saturate(refractedUV * float2(0.5f, -0.5f) + float2(0.5f, 0.5f));
    
    float depthSample = SampleSceneDepth(refractedUV);
    float refractedDepth = DepthToViewZ_Fast(depthSample);

    float depthDiff = refractedDepth - viewDepth;
    uvOffset *= saturate(depthDiff);

    refractedUV = ((positionNDC.xy + uvOffset) / positionNDC.w);
    refractedUV = saturate(refractedUV * float2(0.5f, -0.5f) + float2(0.5f, 0.5f));

    depthSample = SampleSceneDepth(refractedUV);
    return float3(refractedUV, depthSample);
}

float3 Refraction(const LightingInput li, const FoamData foamData, float2 sss, float3 foamColor)
{
    float depthScale = 0.0f;
    // P16.5 -- THESE TWO ARE SCATTERED LIGHT, NOT EMISSION. They were added as ABSOLUTE radiance,
    // which only ever worked while the scene's light happened to sit near 1. Against a physical sun
    // they are tens of thousands of times too small, the water loses its own colour entirely and
    // nothing is left but the lit diffuse term below -- which is why the sea went from blue to
    // green the moment the units became real. Multiplying by the light that does the scattering is
    // what puts them on the same scale as every other term.
    //
    // At the pre-P16 scale the sun colour was ~0.92, so this is under 0.13 stops on these two terms
    // for any level that has not been converted yet.
    // Plan C3/C4 (owner, 2026-09-12): THE SKY LIGHTS THE WATER BODY TOO. These terms were sun-only
    // (the diffuse line carried its "sky" as a 0.2 floor ON THE SUN COLOUR), so a cloud shadow that
    // took the sun away left the sea black beside sand that keeps its sky irradiance ("дико тёмные"),
    // and a floor tied to the sun could never follow the sky. Now the light that scatters in the
    // body is the sun through the cloud (shadowAttenuation, C3) PLUS the measured sky irradiance at
    // the surface normal -- the same SkyFillRadiance the foam uses, E/pi in the sun colour's units,
    // from the environment cube that carries the clouds (C4), so under a deck it dims by itself.
    // HOW MUCH OF THE CLOUD SHADOW THE BODY TAKES is dosed (cloudShadowParams.z, the level's
    // cloud.oceanBodyShadow): the glints and the foam lose the sun outright, the water-leaving
    // light only partly. Mobley: the body is lit by the whole downwelling irradiance, and under
    // broken cloud the diffuse part stays 30-60 % of the clear-sky total because the bright cloud
    // sides and undersides add light -- light our cube does not yet carry (its undersides are lit
    // by the distant sky only), so a body that followed the sun as the land does went too dark
    // (owner, 2026-09-12: "вода не так реагирует на тень, как плотная геометрия").
    // A shadow from GEOMETRY is the surface point's, sharp, and the body takes `bodyShadowParams.x` of it (ocean.sunShadowBody):
    // taken whole, a hull's shadow on the open sea was a navy hole -- the shaded water is still lit by the lit water round
    // it, which this single-scattering body does not carry. Averaged down the column instead (2026-11-07) it came out as a
    // stack of offset silhouettes (owner, 2026-11-08: "several silhouettes instead of a sharp shadow, looks bad").
    const float bodyShadow = lerp(1.0f, li.mainLight.shadowAttenuation, saturate(cloudShadowParams.z))
                           * lerp(1.0f, li.mainLight.geometryShadow, saturate(bodyShadowParams.x));
    const float3 skyFill = SkyFillRadiance(li.normal);
    // The sun's share scaled by how much of it gets INTO the water at its height (SunBodyEntry):
    // a low sun lights the sea through its reflection and its sky, not through a glowing body.
    const float sunEntry = SunBodyEntry(li.mainLight.direction);
    const float3 waterLight = li.mainLight.color * (bodyShadow * sunEntry) + skyFill;
    float3 color = DeepScatterColor(depthScale) * waterLight;

    float3 sssColor = SssColor(depthScale);
    color += sssColor * saturate(sss.x + sss.y) * waterLight * bodyShadow;
    
    //return color;

    float ndotl = saturate(dot(li.normal, -li.mainLight.direction));
    // The directional part keeps its 0.8 weight; the 0.2 sun-tinted floor is the sky term above.
    color += (ndotl * 0.8f * bodyShadow * sunEntry * li.mainLight.color + skyFill) * DiffuseColor(depthScale);
    
    //return color;

    float3 refractionCoords = RefractionCoords(refractionParams.x, li.positionNDC, li.viewDepth, li.normal);
    // P16.1: this is a copy of SCENE COLOUR, already pre-exposed. The surface multiplies its
    // whole result by the factor at the end, so an already-scaled input has to be brought back
    // first -- otherwise the refracted part gets it twice, which is what left the near water
    // 20/255 too dark.
    float3 backgroundColor = SceneColorTexture.SampleLevel(LinearClampSampler, refractionCoords.xy, 0).rgb
                           / max(skyParams.z, 1.0e-8f);

    //return backgroundColor;

    float3 backgroundPositionWS = PositionWsFromDepth(refractionCoords.z, refractionCoords.xy);
    float backgroundDistance = length(backgroundPositionWS - li.cameraPos) - li.viewDist;
    color = ColorThroughWater(backgroundColor, color, backgroundDistance, -backgroundPositionWS.y);

    //return color;

    float underwaterFoamVisibility = 20.0f / (20.0f + li.viewDist);
    float3 tint = AbsorptionTint(0.8f);
    float3 underwaterFoamColor = foamColor * tint * tint;
    color = lerp(color, underwaterFoamColor, max(foamData.coverage.y * underwaterFoamVisibility, foamData.aeration));
    return color;
}

float4 HorizonBlend(const LightingInput li)
{
    float3 dir = -float3(li.viewDir.x, 0.0f, li.viewDir.z);
    float3 horizonColor = SkyboxTexture.SampleLevel(LinearClampSampler, dir, 0).rgb * skyParams.x;

    float horizonFog = max(specularParams.w, 0.01f);
    float distanceScale = 100.0f + 7.0f * abs(li.cameraPos.y);
    float exponent = -5.0f / horizonFog * (abs(li.viewDir.y) + distanceScale / (li.viewDist + distanceScale));
    float blend = exp(exponent);
    return float4(horizonColor, saturate(blend));
}

float3 GetOceanColor(const LightingInput li, const FoamData foamData, out OceanGBuffer gb)
{
    BrunetonInputs bi = BuildBrunetonInputs(li);
    // The reflection's DIRECTION takes its own (filtered) normal and its blur the slope variance the filter left to it:
    // point-sampled through every ripple, the reflected direction flipped pixel to pixel between the sky and the shore --
    // grain that only the sun-lit body hid (2026-11-02, the water in the palms' shadow). The Fresnel WEIGHT keeps the
    // full normal: at a grazing view the mean of F over the ripples is below F of their mean normal, which the distance
    // law's variance does not make up -- taken from the filtered normal the sun-lit lagoon turned bluer.
    const BrunetonInputs reflectionBi = BuildReflectionBrunetonInputs(li, bi);
    float2 sss = SubsurfaceScatteringFactor(li);
    float3 foamLitColor = LitFoamColor(li, foamData);

    float fresnel = EffectiveFresnel(li, bi);
    float3 specular = OceanSunSpecular(li, bi) * Pow5(1.0f - saturate(foamData.coverage.x));
    // Bruneton slope variance -> an RMS slope -> a GGX-ish roughness for the shared environment.
    // Clamped low: open water is a near-mirror and the variance can spike on wave crests.
    const float oceanRoughness = clamp(sqrt(max(bi.slopeVarianceSquared.x, 0.0f)), 0.02f, 0.6f);
    // The sky's blur takes the reflection's unresolved variance too. GB0 (below) keeps the sun's: the screen-space ray
    // casts GGX rays above roughness 0.1 (UE High), and a rougher GB0 would trade the grain for their noise.
    const float skyRoughness = clamp(sqrt(max(reflectionBi.slopeVarianceSquared.x, 0.0f)), 0.02f, 0.6f);
    float3 reflected = Reflection(li, skyRoughness);
    //return reflected;
    float3 refracted = Refraction(li, foamData, sss, foamLitColor);
    //return refracted;
    float4 horizon = HorizonBlend(li);
    //return horizon.aaa;

    float3 color = specular + lerp(refracted, reflected, fresnel);
    //color = fresnel.xxx;
    color = lerp(color, foamLitColor, foamData.coverage.x * 1.0f);
    // Plan S1: the G-buffer layer; the weight follows `reflected` through every lerp below.
    gb.reflectionNormal = OceanReflectionNormal(li);
    gb.reflectionRoughness = oceanRoughness;
    gb.reflectionWeight = fresnel * (1.0f - foamData.coverage.x * 1.0f);
    gb.reflectionSky = reflected;

    // P7. Identical to the modern surface's ending, and it has to be: this variant is the one that
    // actually runs today (`ocean::g_shoreRunup` defaults false), so fogging only the other one
    // would have shipped a feature that never executes. Exactly one of the ocean's own horizon
    // fade and the global aerial perspective runs; with fog off this is the tuned behaviour
    // unchanged, byte for byte.
    if (fogParams0.x > 0.0f)
    {
        // B6.1: the fog for this surface is applied by Main_TransparentFog (fog_apply.hlsl), in the
        // SAME code that fogs the opaque scene, after the water has written depth and before
        // anything alpha-blended is drawn over it. That is UE's arrangement -- their water surface
        // is fogged by the screen-space passes and SingleLayerWaterShading.ush carries no fog code
        // at all. What stays here is only the decision this branch always encoded: with fog on, the
        // ocean's OWN horizon fade does not run. P7's rule that exactly one of the two applies is
        // unchanged, it is just no longer this shader's job to perform the one that wins.
        //
        // Debug views need nothing either: the fog pass writes them over the water itself.
    }
    else
    {
        color = lerp(color, horizon.rgb, horizon.a);
        gb.reflectionWeight *= 1.0f - horizon.a;
        // Same rule compose follows: a debug view must not leave un-measured pixels showing the
        // ordinary image, or "no fog" and "not part of this view" look identical.
        if (fogDebugView != 0u) { color = 0.0f.xxx; gb.reflectionWeight = 0.0f; }
    }
    return color;
}

// Plan S1/S4: RT2..RT5 are the water's G-buffer layer (OceanGBuffer). The editor's objectID target
// sits AFTER them (RT6, editor builds only) with a zero write mask and no output here, so one PS
// serves both builds.
struct PSOut
{
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
    float4 gb0 : SV_Target2;
    float4 gb1 : SV_Target3;
    float4 gb2 : SV_Target4;
    float4 gbAux : SV_Target5;
};

// No [earlydepthstencil] (2026-11-11): with it a pixel discarded below kept its depth -- the water's height inside a hull's
// cockpit -- and whatever reads the depth after the water took the floor for water: fog_apply, which fogs what changed the
// depth, fogged the floor a second time (owner, at police_boat's helm: "the water is just in the boat" -- a wavy band a
// few levels lighter where the sea stood over the floor, gone with fog.enabled:0), DLSS and the particles' soft fade read
// the wrong surface. Without it a discard writes no depth. What it was there for -- the shore wetness stamp (the UAV
// below) must not run for a pixel the opaque scene hides, nor the whole shader -- is the first test now: the opaque depth
// as it stood before the water (SceneDepthTexture), reverse-Z, the depth test's own rule.
[RootSignature(OCEAN_SURFACE_RS)]
PSOut PSMain(VSOutput input)
{
    if (input.position.z < SceneDepthTexture.Load(int3(int2(input.position.xy), 0)).r)
    {
        discard;
    }
    // The water a marked hull keeps out (WaterMaskCB): not drawn there -- no colour, no G-buffer, no depth -- the hull's own
    // floor and walls show.
    const bool keptOut = (waterMaskCount.w & 2u) != 0u && InsideWaterMask(input.position, input.viewDepth);
    const bool leaked = waterMaskCount.y == 3u && !keptOut && OverWaterMaskPlan(input.worldPos);
    [branch] if (keptOut && waterMaskCount.y != 2u)
    {
        discard;
    }
    uint cascadesCount = max((uint)simulationParams.w, 1u);

    float3 baseWorld = float3(input.baseXZ.x, 0.0f, input.baseXZ.y);
    float3 viewVector = baseWorld - clipMapViewer.xyz;
    float viewDist = length(viewVector);
    float2 screenUV = ComputeScreenUV(input.positionNDCJitter);

    // Shoreline normal attenuation, PORTED FROM THE MODERN SURFACE (minus its shore-field
    // weight, which legacy has no equivalent of): instead of one scalar crushing every cascade
    // equally, each cascade fades toward its authored minimum (shoreNormalMinWeights) as the
    // water shallows over the normal fade depth - fine ripple detail dies first, the swell's
    // shape survives to the waterline. The same weights feed the foam cascades below, exactly
    // like the modern surface does.
    const float shorePixelDepth = ShoreWaterDepthAt(baseWorld.xz);
    float normalFade = smoothstep(
        0.0f, max(shoreLegacyDampParams.w, 0.01f), max(shorePixelDepth, 0.0f));
    float4 normalWeights = lerp(saturate(shoreNormalMinWeights), 1.0f.xxxx, normalFade);

    float4 weights = LodWeights(viewDist, clipMapParams.w);
    DerivativesSet deriv = SampleDerivatives(input.baseXZ, weights, cascadesCount, normalSamplingParams.y);
    float4 activeCascades = ActiveCascadesMask(cascadesCount);
    float4 combinedDerivatives = CombineDerivatives(deriv, normalWeights);
    // A boat's wake: its slope joins the FFT's, which NormalFromCombinedDerivatives scales by kNormalScale -- divided out
    // here, so wakeNormal 1 is the field's true slope. Faded on the beach as its height is in the VS.
    const float2 wakeSlope = WakeSlope(input.baseXZ) * saturate(max(shorePixelDepth, 0.0f) / 0.15f);
    combinedDerivatives.xy += wakeSlope / kNormalScale;
    float3 normal = NormalFromCombinedDerivatives(combinedDerivatives);
    //return float4(normal, 1);
    // The REFLECTION's normal (LightingInput::reflectionNormal): the same derivatives `w` mips coarser
    // (`ocean.reflectionNormalFilter`, normalSamplingParams.w; 0 = the shading normal). A mip IS the pixel footprint, so
    // this keeps every ripple wider than 2^w pixels -- near the camera the waves still bend the reflection -- and drops
    // the ones a single mirror sample per pixel can only alias; their slope variance goes to the reflection's roughness.
    DerivativesSet reflectionDeriv = deriv;
    [branch] if (normalSamplingParams.w > 0.0f)
    {
        reflectionDeriv = SampleDerivatives(input.baseXZ, weights, cascadesCount,
                                            normalSamplingParams.y + normalSamplingParams.w);
    }
    const float3 reflectionNormal = NormalFromCombinedDerivatives(
        CombineDerivatives(reflectionDeriv, normalWeights) + float4(wakeSlope / kNormalScale, 0.0f, 0.0f));

    float3 viewDir = normalize(clipMapViewer.xyz - input.worldPos);
    float3 lightDir = normalize(sunDirAmbient.xyz);

    float slopeFactor = saturate(1.0f - normal.y);
    float height = input.worldPos.y;

    FoamInput foamInput;
    foamInput.derivatives = deriv;
    foamInput.worldUV = input.baseXZ;
    foamInput.viewDist = viewDist;
    foamInput.lodWeights = weights;
    foamInput.shoreWeights = normalWeights;
    foamInput.positionNDC = input.positionNDCJitter;
    foamInput.time = simulationParams.z;
    foamInput.viewDir = viewDir;
    foamInput.normal = normal;
    foamInput.viewDepth = input.viewDepth;
    foamInput.shoreMapDepth = shorePixelDepth;
    foamInput.worldUVdx = ddx(input.baseXZ);
    foamInput.worldUVdy = ddy(input.baseXZ);

    FoamData foamData = GetFoamData(foamInput, cascadesCount);
    //return float4(foamData.coverage.xxx, 1);

    float roughnessMap = SampleDistantRoughness(input.baseXZ, viewDist);
    //return float4(roughnessMap.xxx, 1);
    
    LightData light;
    light.direction = lightDir;
    light.color = sunColorExposure.xyz * sunColorExposure.w;
    light.shadowAttenuation = CloudSunVisibility(input.worldPos); // plan C3: the cloud shadow (was a constant 1)
    light.geometryShadow = OceanSunShadow(mul(float4(input.worldPos, 1.0f), model).xyz); // worldPos is pre-model

    LightingInput li;
    li.normal = normal;
    li.reflectionNormal = reflectionNormal;
    li.viewDir = viewDir;
    li.viewDist = viewDist;
    li.roughnessMap = roughnessMap;
    li.positionWS = input.worldPos;
    li.screenUV = screenUV;
    li.shore = float4(0.0f, 0.0f, 0.0f, 0.0f);
    li.viewDepth = input.viewDepth;
    li.cameraPos = clipMapViewer.xyz;
    li.height = height;
    li.positionNDC = input.positionNDCJitter;
    li.referenceWaveHeight = windParams1.z;
    li.slopeFactor = slopeFactor;
    li.mainLight = light;
    li.ambient = sunDirAmbient.w;

    OceanGBuffer gb;
    float3 color = GetOceanColor(li, foamData, gb);
    // EXPERIMENT (2026-10-12): the reflection normal (GB1) from the first N cascades only; the sky sample
    // inside GetOceanColor keeps the full one. 4 = all, and GB1 is what it always was.
    [branch] if (normalSamplingParams.z < 3.5f)
    {
        LightingInput reflectionLi = li;
        reflectionLi.reflectionNormal = NormalFromCombinedDerivatives(
            CombineDerivatives(reflectionDeriv, normalWeights * ReflectionCascadeMask(normalSamplingParams.z)));
        gb.reflectionNormal = OceanReflectionNormal(reflectionLi);
    }

    // Refraction soft edge, PORTED FROM THE MODERN SURFACE: where the water sheet meets
    // geometry, the distorted refraction is faded back to the UNDISTORTED scene sample over
    // the authored edge-soft depth, so the waterline does not wear a wobbling distortion rim.
    [branch]
    if (shoreSlopeParams.z > 0.0f)
    {
        float sceneRawDepth = SampleSceneDepth(screenUV);
        float geometrySeparation =
            max(DepthToViewZ_Fast(sceneRawDepth) - input.viewDepth, 0.0f);
        float refractionEdgeWeight =
            smoothstep(0.0f, shoreSlopeParams.z, geometrySeparation);
        [branch]
        if (refractionEdgeWeight < 0.95f)
        {
            float3 softEdgeRefraction =
                SceneColorTexture.SampleLevel(LinearClampSampler, screenUV, 0).rgb / max(skyParams.z, 1.0e-8f);   // P16.1, see above
            color = lerp(softEdgeRefraction, color, refractionEdgeWeight);
            gb.reflectionWeight *= refractionEdgeWeight;
        }
    }

    // NOT saturate(): this writes into the HDR scene target (R16G16B16A16_FLOAT), and clamping to
    // 1.0 here capped the water at a value the sky is free to exceed. The reflection could then
    // never match the sky it reflects -- the horizon showed a pale plate against a brighter sky,
    // and no amount of work on the cubemap, the mip or the roughness could close a gap that was
    // being imposed after all of them. Only the low end is clamped, which is what the rest of the
    // renderer does.
    float4 outColor = float4(max(color, 0.0f.xxx) * skyParams.z, 1.0f);

    // surf sim injection: debug tint (docs/ocean_surf_sim_plan.md). A plain uniform branch, no
    // variant - surfSimParams.w = 0 keeps every water pixel on the early side of the branch.
    // 1: sim height (red +, blue -), 2: sim vertical velocity, 3: shore SDF isolines (5 m),
    // 4: shore depth map.
    [branch]
    if (surfSimParams.w > 0.5f)
    {
        const uint debugView = (uint)surfSimParams.w;
        float3 debugColor = float3(0.0f, 0.0f, 0.0f);
        float debugWeight = 0.0f;
        if (debugView <= 2u)
        {
            const float2 simUV =
                (input.baseXZ - surfSimParams.xy) * (surfSimParams.z * 0.5f) + 0.5f;
            if (all(simUV >= 0.0f) && all(simUV <= 1.0f))
            {
                if (debugView == 1u)
                {
                    const float value = SurfSimWaveTex.SampleLevel(LinearClampSampler, simUV, 0).x;
                    debugColor = float3(saturate(value * 2.0f), 0.1f, saturate(-value * 2.0f));
                    debugWeight = 0.85f;
                }
                else // S3: the surf foam field, white on dark — OPAQUE, so nothing that is not
                {    // sim foam can read as white (the legacy foam bled through at 0.85)
                    const float f = SurfSimFoamTex.SampleLevel(LinearClampSampler, simUV, 0);
                    debugColor = saturate(f).xxx;
                    debugWeight = 1.0f;
                }
            }
        }
        else if (debugView == 3u)
        {
            const float2 sdfUV = ShoreSdfUV(input.baseXZ);
            if (all(sdfUV >= 0.0f) && all(sdfUV <= 1.0f))
            {
                const float dist = ShoreSdfTexture.SampleLevel(LinearClampSampler, sdfUV, 0).x;
                const float band = abs(frac(dist * 0.2f) - 0.5f) * 2.0f;
                debugColor = dist < 0.0f
                    ? float3(0.8f, 0.2f, 0.1f)                       // inland
                    : float3(band, saturate(dist * 0.01f), 1.0f - band);
                debugWeight = 0.85f;
            }
        }
        else
        {
            const float2 shoreUV = ShoreDepthUV(input.baseXZ);
            if (all(shoreUV >= 0.0f) && all(shoreUV <= 1.0f))
            {
                debugColor = SampleShoreDepth(shoreUV).xxx;
                debugWeight = 0.85f;
            }
        }
        outColor.rgb = lerp(outColor.rgb, debugColor, debugWeight);
        gb.reflectionWeight *= 1.0f - debugWeight;
    }
    // `--set ocean.wakeDebug:1`: the boat wake's height, opaque (red up, blue down, 0.25 m to full) -- the wave pattern
    // to read the Kelvin angle off, without the glints.
    [branch]
    if (wakeParams2.z > 0.5f && wakeParams.z > 0.0f)
    {
        const float2 wakeUV = WakeUV(input.baseXZ);
        if (all(wakeUV >= 0.0f) && all(wakeUV <= 1.0f))
        {
            if (wakeParams2.z > 1.5f) // 2: the foam field, white on dark (1 = fresh white water), the tear not applied
            {
                const float4 laid = WakeFoamTex.SampleLevel(LinearClampSampler, wakeUV, 0);
                const float f = laid.x + laid.w * WakeBowGate(input.baseXZ);
                outColor.rgb = float3(saturate(f), saturate(f), saturate(f) * 0.8f + 0.08f);
            }
            else
            {
                const float h = WakeTex.SampleLevel(LinearClampSampler, wakeUV, 0).x;
                outColor.rgb = float3(saturate(h * 4.0f), 0.08f, saturate(-h * 4.0f));
            }
            gb.reflectionWeight = 0.0f;
        }
    }

    float2 currUv = ClipToUV(input.positionNDC);
    float2 prevUv = ClipToUV(input.prevPositionNDC);
    float2 motion = currUv - prevUv;

    // The classic surface has no run-up sheet, but its ordinary visible water still leaves a
    // narrow trace at the waterline. The stamp and its depth lookup MUST use the same displaced
    // XZ position: using baseXZ for depth while writing at worldPos translated the wet band by
    // the wave's horizontal displacement, so the history visibly detached from the water edge.
    // The 1000 m sentinel used outside the shore map naturally produces zero coverage here.
    if (shoreWetnessParams.z > 0.0f)
    {
        const float wetnessDepth = ShoreWaterDepthAt(input.worldPos.xz);
        float wetnessEdgeWeight = 1.0f;
        const float wetnessEdgeOffset = max(shoreWetnessParams2.x, 0.0f);
        const float2 wetnessSdfUV = ShoreSdfUV(input.worldPos.xz);
        if (all(wetnessSdfUV >= 0.0f) && all(wetnessSdfUV <= 1.0f))
        {
            const float distanceToWaterline =
                ShoreSdfTexture.SampleLevel(LinearClampSampler, wetnessSdfUV, 0).x;

            // The SDF describes the STILL-water line, while legacy's two damp controls alter the
            // already-displaced sheet: Y displacement changes where it intersects the bed and XZ
            // displacement moves the fragment itself. Convert the actual displaced surface height
            // to a horizontal shoreline shift using the local mean bed slope. Both inputs therefore
            // affect the same dynamic signed distance that gates the wet stamp.
            //
            //     bed slope ~= vertical bed offset / horizontal distance to the waterline
            //
            // A flat beach turns a crest into a long run-up; a vertical wall produces almost no
            // horizontal shift. The clamps only regularize the exact zero crossing and pathological
            // terrain, while the hardware depth test still remains the final visibility authority.
            const float slopeDistance = max(abs(distanceToWaterline), 0.25f);
            const float meanBedSlope = clamp(abs(wetnessDepth) / slopeDistance, 0.02f, 8.0f);
            const float displacedShoreShift = clamp(input.worldPos.y / meanBedSlope, -20.0f, 20.0f);
            const float displacedDistanceToWaterline =
                distanceToWaterline + displacedShoreShift;

            // Keep the authored offset metric while softening the threshold over roughly one
            // wetness texel. At offset zero the dynamic edge still follows crests and troughs.
            const float edgeFeather = max(0.25f, wetnessEdgeOffset * 0.25f);
            wetnessEdgeWeight = smoothstep(
                wetnessEdgeOffset,
                wetnessEdgeOffset + edgeFeather,
                displacedDistanceToWaterline);
        }
        const float2 wetnessUV =
            (input.worldPos.xz - shoreWetnessParams.xy) *
                (shoreWetnessParams.z * 0.5f) + 0.5f;
        const float coverage = wetnessEdgeWeight *
            (1.0f - smoothstep(
                0.0f,
                max(shoreWetnessParams.w, 1e-3f),
                max(wetnessDepth, 0.0f)));
        if (coverage > 0.0f && all(wetnessUV >= 0.0f) && all(wetnessUV < 1.0f))
        {
            uint wetWidth;
            uint wetHeight;
            ShoreWetnessStampMap.GetDimensions(wetWidth, wetHeight);
            const uint2 wetCoord = min(
                uint2(wetnessUV * float2(wetWidth, wetHeight)),
                uint2(wetWidth - 1u, wetHeight - 1u));
            InterlockedMax(
                ShoreWetnessStampMap[wetCoord],
                (uint)round(saturate(coverage) * 65535.0f));
        }
    }

    //outColor = float4(attenuation.xxx, 1.0f);

    PSOut o;
    // ocean.waterMask:2: the kept-out water magenta, at three times the water's own brightness (the exposure stays the frame's).
    o.color = keptOut ? float4(float3(1.0f, 0.0f, 1.0f) * max(dot(outColor.rgb, (1.0f / 3.0f).xxx) * 3.0f, 1e-3f), outColor.a) : outColor;
    // ocean.waterMask:3: the water drawn in or over a hull, red.
    if (leaked) { o.color = float4(float3(1.0f, 0.0f, 0.0f) * max(dot(outColor.rgb, (1.0f / 3.0f).xxx) * 3.0f, 1e-3f), outColor.a); }
    o.velocity = motion;
    o.gb0 = OceanGBuffer0(gb);
    o.gb1 = OceanGBuffer1(gb);
    o.gb2 = OceanGBuffer2(gb, skyParams.z);
    o.gbAux = OceanGBufferAux(gb);
    return o;
}
