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
// rather than sRGB). The C++ table stages the same 22 entries for both surfaces.
#define OCEAN_SURFACE_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), DescriptorTable(SRV(t0, numDescriptors=22, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE))"
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
    float4 normalSamplingParams;       // x: detail normal mip bias, y: active macro normal mip bias
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
    float4 windParams1;                // xy: wind direction, z: reference wave height, w: padding
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
    float4 cloudShadowParams;          // x: on, y: far depth km, z: how much of the shadow the water body takes (cloud.oceanBodyShadow), w: 0
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
Texture2D OceanReflectionTexture : register(t15);
// surf sim injection: the sim's height field (x: height m, y: vertical velocity) and its surf
// foam field (r: coverage).
Texture2D<float2> SurfSimWaveTex : register(t16);
Texture2D<float> SurfSimFoamTex : register(t17);
RWTexture2D<uint> ShoreWetnessStampMap : register(u0);
SamplerState LinearWrapSampler : register(s0);
SamplerState LinearClampSampler : register(s1);
SamplerState PointSampler : register(s2);
SamplerState AnisotropicWrapSampler : register(s3);

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
};

struct FoamData
{
    float2 coverage;
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
    float shadowAttenuation;
};

struct LightingInput
{
    float3 normal;
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
static const float kSpecularMinPower = 64.0f;
static const float kSpecularMaxPower = 512.0f;
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

float ContactFoam(float4 positionNDC, float viewDepth, float2 worldUV, float shoreMapDepth)
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
    float2 tailUV =
        (worldUV - windDirection * simulationParams.z * max(shoreLegacyFoamParams.z, 0.0f)) *
        max(shoreLegacyFoamParams.x, 1e-3f);
    float tail = ContactFoamTex.SampleLevel(LinearWrapSampler, tailUV, 0).r;
    [branch]
    if (shoreLegacyFoamParams.w > 1e-3f)
    {
        // Second octave: same texture, rotated ~37 degrees and rescaled by an irrational-ish
        // factor, so the two grids never line up and the repeat period stops reading.
        float2 rotatedUV =
            float2(tailUV.x * 0.8f - tailUV.y * 0.6f,
                   tailUV.x * 0.6f + tailUV.y * 0.8f) * 0.531f + 17.31f;
        float tail2 = ContactFoamTex.SampleLevel(LinearWrapSampler, rotatedUV, 0).r;
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
        ContactFoamTex, LinearWrapSampler, worldUV, simulationParams.z,
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
float SurfSimFoamCoverage(float2 baseXZ)
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
        ContactFoamTex, LinearWrapSampler, baseXZ, simulationParams.z,
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

FoamData GetFoamData(FoamInput input, uint cascadesCount)
{
    FoamData data;
    data.coverage = float2(0.0f, 0.0f);
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

    float contactCoverage = 0.0f;
    if (foamParams2.y > 0.0f)
    {
        contactCoverage = ContactFoam(input.positionNDC, input.viewDepth, input.worldUV, input.shoreMapDepth);
    }
    // surf sim injection (S4): the sim's breaking foam joins as another additive shore
    // coverage source - same slot as contact foam, so it wears the shore foam albedo below.
    contactCoverage = saturate(contactCoverage + SurfSimFoamCoverage(input.worldUV));
    data.coverage.x = saturate(data.coverage.x + contactCoverage);

    float4 foamNormalWeights = saturate(float4(1.0f, 0.66f, 0.33f, 0.0f) + foamParams1.w) * activeCascades;
    float3 foamNormal = NormalFromDerivatives(input.derivatives, foamNormalWeights);
    data.normal = foamNormal;

    float2 uv = input.worldUV * 1.0f;
    data.albedo = FoamAlbedoTex.SampleLevel(LinearWrapSampler, uv, 0).rgb;
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
        float2 shoreAlbedoUV =
            (input.worldUV -
             windDirection * input.time * max(shoreFoamAlbedoParams.y, 0.0f)) *
            max(shoreFoamAlbedoParams.x, 1e-3f);
        float3 shoreAlbedo = ShoreFoamAlbedoTex.SampleLevel(
            LinearWrapSampler, shoreAlbedoUV, 0).rgb;
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
    // A black or absent cubemap falls back to the old constant, so a level without a skybox keeps
    // lit foam instead of losing its surf entirely.
    const float lum = dot(sky, float3(0.2126f, 0.7152f, 0.0722f));
    return (lum > 1e-5f) ? sky : kLegacyFoamSkyColor;
}

float3 LitFoamColor(const LightingInput li, const FoamData foamData)
{
    float ndotl = (0.2f + 0.8f * saturate(dot(foamData.normal, -li.mainLight.direction)))
        * li.mainLight.shadowAttenuation;
    // P16.7: `SkyFillRadiance` is the sky's MEASURED irradiance; `li.ambient` is the legacy
    // FRACTION-OF-THE-SUN knob (0.05-0.15). Multiplying one by the other is the exact mistake F8
    // documents about its own first version -- "multiplying an absolute measured irradiance by a
    // level's 0.05 buries the fill about twenty times too deep" -- and it survived here because
    // foam is the one thing the ocean lights with the sky. The directional term stays: it is a
    // shape (upward-facing foam sees more sky), not a strength.
    float3 skyAmbient = SkyFillRadiance(foamData.normal) * (1.0f + 0.3f * (1.0f - foamData.normal.y));
    return foamData.albedo * foamTint.rgb * (ndotl * li.mainLight.color + skyAmbient);
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

    float windSpeed = max(windParams0.x, 0.0f);
    float wavesScale = max(windParams0.y, 0.0f);
    float alignment = windParams0.z;
    float roughScale = max(specularParams.y, 0.0f);
    float2 slopeVariance = roughScale * (1.0f + li.roughnessMap * 0.3f)
        * SlopeVarianceSquared(windSpeed * wavesScale, li.viewDist, alignment, max(specularParams.z, 1.0f));
    bi.slopeVarianceSquared = slopeVariance;
    return bi;
}

float3 Specular(const LightingInput li, const BrunetonInputs bi)
{
    //(void)bi;
    float3 halfDir = normalize(-li.mainLight.direction + li.viewDir);
    float roughness = saturate(specularParams.y * (1.0f + li.roughnessMap * 0.3f));
    float specPower = lerp(kSpecularMinPower, kSpecularMaxPower, 1.0f - roughness);
    float spec = pow(saturate(dot(li.normal, halfDir)), specPower);
    spec *= specularParams.x * li.mainLight.shadowAttenuation;
    return spec * li.mainLight.color;
}

// P5: takes a roughness now. The legacy surface has no per-pixel roughness of its own -- it is a
// Bruneton model -- but it does carry the physical quantity roughness stands for: the slope
// VARIANCE of the microfacet distribution. sqrt of it is an RMS slope, which is what maps onto a
// GGX alpha, so the caller passes that instead of the fixed mip 3 this used to blur by.
float3 Reflection(const LightingInput li, float roughness)
{
    float reflectionNormalStrength = heightFogParams.w;
    float3 adjustedNormal = normalize(lerp(li.normal, float3(0.0f, 1.0f, 0.0f), reflectionNormalStrength));
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
    float2 reflectionUV = li.screenUV + OceanReflectionUvOffset(li, adjustedNormal);
    float edgeFade = OceanReflectionEdgeFade(reflectionUV);
    // P16.1: the planar reflection is rendered from scene colour and is pre-exposed too.
    float4 oceanReflection = OceanReflectionTexture.SampleLevel(LinearClampSampler, saturate(reflectionUV), 0);
    oceanReflection.rgb /= max(skyParams.z, 1.0e-8f);
    float visibility = saturate(oceanReflection.a) * edgeFade;
    return oceanReflection.rgb * edgeFade + skySample * (1.0f - visibility);
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
    const float bodyShadow = lerp(1.0f, li.mainLight.shadowAttenuation, saturate(cloudShadowParams.z));
    const float3 skyFill = SkyFillRadiance(li.normal);
    const float3 waterLight = li.mainLight.color * bodyShadow + skyFill;
    float3 color = DeepScatterColor(depthScale) * waterLight;

    float3 sssColor = SssColor(depthScale);
    color += sssColor * saturate(sss.x + sss.y) * waterLight;
    
    //return color;

    float ndotl = saturate(dot(li.normal, -li.mainLight.direction));
    // The directional part keeps its 0.8 weight; the 0.2 sun-tinted floor is the sky term above.
    color += (ndotl * 0.8f * bodyShadow * li.mainLight.color + skyFill) * DiffuseColor(depthScale);
    
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
    color = lerp(color, underwaterFoamColor, foamData.coverage.y * underwaterFoamVisibility);
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

float3 GetOceanColor(const LightingInput li, const FoamData foamData)
{
    BrunetonInputs bi = BuildBrunetonInputs(li);
    float2 sss = SubsurfaceScatteringFactor(li);
    float3 foamLitColor = LitFoamColor(li, foamData);

    float fresnel = EffectiveFresnel(li, bi);
    float3 specular = Specular(li, bi) * Pow5(1.0f - saturate(foamData.coverage.y));
    // Bruneton slope variance -> an RMS slope -> a GGX-ish roughness for the shared environment.
    // Clamped low: open water is a near-mirror and the variance can spike on wave crests.
    const float oceanRoughness = clamp(sqrt(max(bi.slopeVarianceSquared.x, 0.0f)), 0.02f, 0.6f);
    float3 reflected = Reflection(li, oceanRoughness);
    //return reflected;
    float3 refracted = Refraction(li, foamData, sss, foamLitColor);
    //return refracted;
    float4 horizon = HorizonBlend(li);
    //return horizon.aaa;

    float3 color = specular + lerp(refracted, reflected, fresnel);
    //color = fresnel.xxx;
    color = lerp(color, foamLitColor, foamData.coverage.x * 1.0f);

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
        // Same rule compose follows: a debug view must not leave un-measured pixels showing the
        // ordinary image, or "no fog" and "not part of this view" look identical.
        if (fogDebugView != 0u) { color = 0.0f.xxx; }
    }
    return color;
}

struct PSOut
{
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
    float bias : SV_Target2;
};

[earlydepthstencil]
[RootSignature(OCEAN_SURFACE_RS)]
PSOut PSMain(VSOutput input)
{
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
    float3 normal = NormalFromCombinedDerivatives(combinedDerivatives);
    //return float4(normal, 1);

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

    FoamData foamData = GetFoamData(foamInput, cascadesCount);
    //return float4(foamData.coverage.xxx, 1);

    float roughnessMap = SampleDistantRoughness(input.baseXZ, viewDist);
    //return float4(roughnessMap.xxx, 1);
    
    LightData light;
    light.direction = lightDir;
    light.color = sunColorExposure.xyz * sunColorExposure.w;
    light.shadowAttenuation = CloudSunVisibility(input.worldPos); // plan C3: the cloud shadow (was a constant 1)

    LightingInput li;
    li.normal = normal;
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

    float3 color = GetOceanColor(li, foamData);

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
    o.color = outColor;
    o.velocity = motion;
    o.bias = 0.0f;
    return o;
}
