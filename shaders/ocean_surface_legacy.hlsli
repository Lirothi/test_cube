// THE CLASSIC OCEAN SURFACE, verbatim from commit 3e54d5d (2026-06-22) - the last state
// before the shore/run-up rework. Compiled when OCEAN_SHORE_RUNUP=0 ("--ocean-classic-shore");
// see ocean_surface.hlsl, which dispatches between this file and the modern stack.
//
// The ONLY edits against the original are binding plumbing for today's C++ SRV table:
// RS numDescriptors 14->16, SceneDepth t11->t12, ShoreDepth t12->t13, Reflection t13->t15.
// ContactFoamTex stays at t10: today's slot 10 carries the same ContactFoam.dds (loaded linear
// rather than sRGB, the one known deviation). One FUNCTIONAL edit is sanctioned on top: the
// nearshore attenuation is authored (shoreLegacyDampParams) instead of the original hardcoded
// saturate(depth * 0.15); its defaults reproduce the original curve. Do not otherwise touch.
// numDescriptors 18: t16/t17 are the surf sim height and foam fields (surf sim injection) -
// the C++ table always stages 18 entries, the modern RS keeps saying 16 and never addresses
// the extras.
#define OCEAN_SURFACE_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), DescriptorTable(SRV(t0, numDescriptors=18, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE))"
#pragma pack_matrix(row_major)

#include "utils.hlsli" // renamed since June; the only include fix

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
    float4 sunDirAmbient;              // xyz: sun direction, w: ambient intensity
    float4 sunColorExposure;           // xyz: sun color, w: exposure multiplier
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
    float4 surfSimParams3;             // surf sim injection: y: cap width (>1 wider dense zone, <1 narrower), xzw: spare
    float4 shoreWetnessParams;         // xy: history centre, z: 1 / half extent, w: deposit depth
    float4 shoreWetnessParams2;        // x: wetness edge offset from the SDF waterline (m)
    float4 shoreFoamAlbedoParams;      // x: shore albedo scale, y: shore albedo scroll speed (shared with the modern surface)
    float4 shoreSlopeParams;           // z: edge soft depth = the contact foam's edge fade (shared with the modern surface)
    float4 depthTextureSize;           // xy: texel size, zw: texture size
    float2 depthParams;                // x: zNear / (zNear - zFar) y :(zNear * zFar) / (zFar - zNear)
};

Texture2DArray<float4> DisplacementDerivatives : register(t0);
Texture2DArray<float4> PrevDisplacementDerivatives : register(t1);
Texture2DArray<float4> FoamTurbulence : register(t2);
Texture2D SceneColorTexture : register(t3);
TextureCube SkyboxTexture : register(t4);
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
static const float kLodThreshold = 0.05f;

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

Gradient CreateGradient(float4 src[kGradientMaxKeys], float2 params)
{
    Gradient g;
    [unroll]
    for (uint i = 0u; i < kGradientMaxKeys; ++i)
    {
        g.colors[i] = src[i];
    }
    g.colorsCount = (int)params.x;
    g.type = params.y > 0.5f;
    return g;
}

float3 SampleGradient(Gradient grad, float t)
{
    float3 color = grad.colors[0].rgb;
    [unroll]
    for (uint i = 1u; i < kGradientMaxKeys; ++i)
    {
        float prevPos = grad.colors[i - 1u].w;
        float nextPos = grad.colors[i].w;
        float denom = max(nextPos - prevPos, 1e-4f);
        float colorPos = saturate((t - prevPos) / denom);
        float active = step((float)i, (float)(grad.colorsCount - 1));
        colorPos *= active;
        float typeMask = grad.type ? 1.0f : 0.0f;
        float blendType = lerp(colorPos, step(0.01f, colorPos), typeMask);
        color = lerp(color, grad.colors[i].rgb, blendType);
    }
    return color;
}

float2 ComputeScreenUV(float4 clipPosition)
{
    float2 ndc = clipPosition.xy / max(clipPosition.w, 1e-5f);
    return ndc * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
}

float2 ScreenUVToNDC(float2 uv)
{
    float2 ndc;
    ndc.x = uv.x * 2.0f - 1.0f;
    ndc.y = 1.0f - uv.y * 2.0f;
    return ndc;
}

float SampleSceneDepth(float2 uv)
{
    return SceneDepthTexture.SampleLevel(PointSampler, uv, 0).r;
}

float3 ViewSpacePosition(float depthSample, float2 uv)
{
    float2 ndc = ScreenUVToNDC(uv);
    float4 clipPos = float4(ndc, depthSample, 1.0f);
    float4 viewPos = mul(clipPos, invProj);
    return viewPos.xyz / max(viewPos.w, 1e-6f);
}

float DepthToViewZ_Fast(float d)
{
    return depthParams.y / (d - depthParams.x);
}

float3 PositionWsFromDepth(float depthSample, float2 uv)
{
    float3 viewPos = ViewSpacePosition(depthSample, uv);
    float4 worldPos = mul(float4(viewPos, 1.0f), invView);
    float invW = rcp(max(worldPos.w, 1e-6f));
    return worldPos.xyz * invW;
}

float SampleShoreDepth(float2 uv)
{
    return ShoreDepthTexture.SampleLevel(LinearClampSampler, uv, 0).r;
}

float2 ShoreDepthUV(float2 baseXZ)
{
    float2 offsetXZ = baseXZ - shoreViewParams.xy;
    float invExtent = shoreViewParams.w;
    return float2(offsetXZ.x * invExtent + 0.5f, 0.5f - offsetXZ.y * invExtent);
}

// surf sim injection (debug): the modern surface's SDF UV mapping, duplicated for the debug view.
float2 ShoreSdfUV(float2 baseXZ)
{
    float2 offsetXZ = baseXZ - shoreSdfParams.xy;
    float invExtent = shoreSdfParams.z;
    return float2(offsetXZ.x * invExtent + 0.5f, 0.5f - offsetXZ.y * invExtent);
}

float ShoreViewDepth(float depthSample)
{
    return lerp(shoreDepthParams.x, shoreDepthParams.y, depthSample);
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

float ModifiedManhattanDistance(float3 a, float3 b)
{
    float3 v = a - b;
    return max(abs(v.x + v.z) + abs(v.x - v.z), abs(v.y)) * 0.5f;
}

float EaseInOutClamped(float x)
{
    x = saturate(x);
    return 3.0f * x * x - 2.0f * x * x * x;
}

float4 LodWeights(float viewDist, float lodScale)
{
    float4 length = max(cascadeLengthScales, float4(1e-3f, 1e-3f, 1e-3f, 1e-3f));
    float4 fade = max(length * lodScale, float4(1e-3f, 1e-3f, 1e-3f, 1e-3f));
    float4 x = (viewDist - fade) / fade;
    return float4(1.0f, 1.0f, 1.0f, 1.0f) - float4(
        EaseInOutClamped(x.x),
        EaseInOutClamped(x.y),
        EaseInOutClamped(x.z),
        EaseInOutClamped(x.w));
}

float3 ClipMapVertexInternal(float3 positionOS,
    float2 uv,
    float clipScale,
    float levelHalfSize,
    float3 viewerPosition)
{
    float3 morphOffset = float3(uv.x, 0.0f, uv.y);
    positionOS *= clipScale;
    float meshScale = positionOS.y;
    float step = max(meshScale * 4.0f, 1e-3f);

    float snappedX = floor(viewerPosition.x / step) * step;
    float snappedZ = floor(viewerPosition.z / step) * step;
    float3 worldPos = float3(snappedX + positionOS.x, 0.0f, snappedZ + positionOS.z);

    float morphStart = ((levelHalfSize + 1.0f) * 0.5f + 8.0f) * meshScale;
    float morphEnd = (levelHalfSize - 2.0f) * meshScale;

    float denom = max(1e-3f, morphEnd - morphStart);
    float t = saturate((ModifiedManhattanDistance(worldPos, viewerPosition) - morphStart) / denom);
    worldPos += morphOffset * meshScale * t;
    return worldPos;
}

float3 ClipMapVertex(float3 positionOS, float2 uv)
{
    return ClipMapVertexInternal(positionOS, uv, clipMapParams.x, clipMapParams.y, clipMapViewer.xyz);
}

float3 ClipMapVertexPrev(float3 positionOS, float2 uv)
{
    return ClipMapVertexInternal(positionOS, uv, prevClipMapParams.x, prevClipMapParams.y, prevClipMapViewer.xyz);
}

float2 ApplyClipMapWarp(float2 worldUV, float viewDistXzSquared, float warpDistance)
{
    float warpScale = min(1.0f, viewDistXzSquared / max(warpDistance * warpDistance * 100.0f, 1.0f));
    float2 warpOffset = sin(worldUV.yx / max(warpDistance, 1e-3f)) * warpDistance * 0.4f * windParams0.w;
    return worldUV + warpOffset * warpScale;
}

float3 SampleDisplacementCascadeTexture(Texture2DArray<float4> tex, float2 worldXZ, uint cascade)
{
    float lengthScale = max(cascadeLengthScales[cascade], 1e-3f);
    float3 uvw = float3(worldXZ / lengthScale, cascade * 2.0f);
    float4 sample = tex.SampleLevel(LinearWrapSampler, uvw, 0);
    return sample.xyz;
}

float4 SampleDerivativesCascade(float2 worldXZ, uint cascade, float mipBias)
{
    float lengthScale = max(cascadeLengthScales[cascade], 1e-3f);
    float3 uvw = float3(worldXZ / lengthScale, cascade * 2.0f + 1.0f);
    //float4 sample = DisplacementDerivatives.SampleLevel(LinearWrapSampler, uvw, 0);
    float4 sample = DisplacementDerivatives.SampleBias(AnisotropicWrapSampler, uvw, mipBias); //give more details far away
    return sample;
}

float3 SampleDisplacementTexture(Texture2DArray<float4> tex, float2 worldXZ, float4 weights, uint cascadesCount)
{
    float3 displacement = float3(0.0f, 0.0f, 0.0f);
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
            displacement += w * SampleDisplacementCascadeTexture(tex, worldXZ, cascade);
        }
    }
    return displacement;
}

float3 SampleCurrentDisplacement(float2 worldXZ, float4 weights, uint cascadesCount)
{
    return SampleDisplacementTexture(DisplacementDerivatives, worldXZ, weights, cascadesCount);
}

float3 SamplePreviousDisplacement(float2 worldXZ, float4 weights, uint cascadesCount)
{
    return SampleDisplacementTexture(PrevDisplacementDerivatives, worldXZ, weights, cascadesCount);
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

float4 CombineDerivatives(DerivativesSet derivatives, float4 weights)
{
    float4 combined = float4(0.0f, 0.0f, 0.0f, 0.0f);
    [unroll]
    for (uint cascade = 0; cascade < 4; ++cascade)
    {
        combined += derivatives.cascades[cascade] * weights[cascade];
    }
    return combined;
}

static const float kNormalScale = 1.5f;

float3 NormalFromCombinedDerivatives(float4 derivatives)
{
    float denomX = max(1e-3f, 1.0f + derivatives.z);
    float denomZ = max(1e-3f, 1.0f + derivatives.w);
    float2 slope = float2(derivatives.x / denomX, derivatives.y / denomZ) * kNormalScale;
    return normalize(float3(-slope.x, 1.0f, -slope.y));
}

float3 NormalFromDerivatives(DerivativesSet derivatives, float4 normalWeights)
{
    float4 combined = CombineDerivatives(derivatives, normalWeights);
    return NormalFromCombinedDerivatives(combined);
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

float4 SampleFoamCascade(float2 worldXZ, uint cascade)
{
    float lengthScale = max(cascadeLengthScales[cascade], 1e-3f);
    float3 uvw = float3(worldXZ / lengthScale, cascade);
    return FoamTurbulence.Sample(LinearWrapSampler, uvw);
}

FoamTurbulenceSet SampleFoamTurbulence(float2 worldXZ, float4 weights, uint cascadesCount)
{
    FoamTurbulenceSet set;
    [unroll]
    for (uint cascade = 0; cascade < 4; ++cascade)
    {
        set.cascades[cascade] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        if (cascade >= cascadesCount)
        {
            continue;
        }

        float w = weights[cascade];
        if (cascade == 0 || w > kLodThreshold)
        {
            float lengthScale = max(cascadeLengthScales[cascade], 1e-3f);
            float3 uvw = float3(worldXZ / lengthScale, cascade);
            set.cascades[cascade] = FoamTurbulence.Sample(LinearWrapSampler, uvw) * w;
        }
    }
    return set;
}

float4 ActiveCascadesMask(uint cascadesCount)
{
    return float4(
        cascadesCount > 0 ? 1.0f : 0.0f,
        cascadesCount > 1 ? 1.0f : 0.0f,
        cascadesCount > 2 ? 1.0f : 0.0f,
        cascadesCount > 3 ? 1.0f : 0.0f);
}

float4 MixTurbulence(FoamTurbulenceSet turbulence, float4 foamWeights, float4 mixWeights)
{
    float4 accum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    [unroll]
    for (uint cascade = 0; cascade < 4; ++cascade)
    {
        accum += turbulence.cascades[cascade] * foamWeights[cascade];
    }
    float totalWeight = dot(foamWeights * mixWeights, float4(1.0f, 1.0f, 1.0f, 1.0f));
    return accum / max(totalWeight, 1e-3f);
}

float2 RotateUV(float2 uv, float2 center, float2 rotation, float sign)
{
    uv -= center;
    float s = rotation.y;
    float c = rotation.x;
    float2x2 rMatrix = float2x2(c, -sign * s, sign * s, c);
    rMatrix *= 0.5f;
    rMatrix += 0.5f;
    rMatrix = rMatrix * 2.0f - 1.0f;
    uv = mul(uv, rMatrix);
    uv += center;
    return uv;
}

float FoamTrailSample(float2 worldUV, float2 direction, float2 scale)
{
    float2 rotated = RotateUV(worldUV, float2(0.0f, 0.0f), direction, 1.0f);
    float2 safeScale = max(scale, float2(1e-3f, 1e-3f));
    return FoamTrailTex.SampleLevel(LinearWrapSampler, rotated / safeScale, 0).r;
}

float DeepFoam(float2 worldUV, float3 viewDir, float3 normal, float time)
{
    float denom = max(dot(normal, viewDir), 1e-3f);
    float2 parallaxDir = (viewDir.xz / denom + 0.5f * normal.xz);
    float2 uv = worldUV - parallaxDir * foamParams2.z - windParams1.xy * time;
    return FoamUnderwaterTex.SampleLevel(LinearWrapSampler, uv * 0.2f, 0).r;
}

float2 Coverage(FoamTurbulenceSet turbulence, float4 mixWeights, float2 worldUV, float deepFoam, float bias)
{
    float4 mixed = MixTurbulence(turbulence, foamCascadeWeights, mixWeights);
    float foamValueCurrent = lerp(mixed.y, mixed.x, foamParams0.z);
    float foamValuePersistent = 0.5f * (mixed.z + mixed.w);
    foamValueCurrent = lerp(foamValueCurrent, foamValuePersistent, foamParams0.w);
    foamValueCurrent -= 1.0f;
    foamValuePersistent -= 1.0f;

    float trail0 = FoamTrailSample(worldUV, foamTrailParams1.xy, foamTrailParams0.xy);
    float trailTexture = trail0;
    if (foamParams2.x > 0.0f)
    {
        float trail1 = FoamTrailSample(worldUV, foamTrailParams1.zw, foamTrailParams0.zw);
        trailTexture = lerp(trail0, trail1, saturate(foamParams2.x));
    }
    
    foamValuePersistent += saturate(foamValuePersistent + 1.0f) * trailTexture * foamParams1.y;
    float foamValue = max(foamValuePersistent + foamParams1.x * (1.0f - bias),
        foamValueCurrent + foamParams0.x * (1.0f - bias));

    float surfaceFoam = saturate(foamValue * foamParams0.y);
    float shallowUnderwaterFoam = saturate((foamValue + 0.1f * foamParams1.z) * foamParams0.y);
    float deepUnderwaterFoam = deepFoam * saturate((foamValue + foamParams1.z * 0.25f) * foamParams0.y * 0.8f);
    return float2(surfaceFoam, max(shallowUnderwaterFoam, deepUnderwaterFoam));
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
    if (surfSimParams.z <= 0.0f)
    {
        return 0.0f; // sim off this frame - zero cost beyond the uniform branch
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
    return coverage * SurfSimWindowEdgeFade(simUV);
}

float Pow5(float x)
{
    float x2 = x * x;
    return x2 * x2 * x;
}

float SchlickFresnel(float cosTheta)
{
    const float baseReflectivity = 0.02f;
    float clamped = saturate(cosTheta);
    return baseReflectivity + (1.0f - baseReflectivity) * Pow5(1.0f - clamped);
}

float2 SlopeVarianceSquared(float windSpeed, float viewDist, float alignment, float scale)
{
    float upwind = 0.01f * sqrt(max(windSpeed, 0.0f)) * viewDist / max(viewDist + scale, 1e-3f);
    return float2(upwind, upwind * (1.0f - 0.3f * alignment));
}

float3 TransformToWind(float3 v)
{
    return mul(worldToWind, float4(v, 0.0f)).xyz;
}

float SampleDistantRoughness(float2 worldUV, float viewDist)
{
    float2 uv = worldUV * 0.001f * 0.01f;
    float roughness = DistantRoughnessMap.SampleLevel(LinearWrapSampler, uv, 0).r;
    float patchLength = max(simulationParams.x, 1.0f);
    roughness *= saturate((viewDist / patchLength) * 0.05f);
    return roughness;
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
    const float3 sky = SkyboxTexture.SampleLevel(LinearClampSampler, normal, 5.0f).rgb;
    // A black or absent cubemap falls back to the old constant, so a level without a skybox keeps
    // lit foam instead of losing its surf entirely.
    const float lum = dot(sky, float3(0.2126f, 0.7152f, 0.0722f));
    return (lum > 1e-5f) ? sky : kLegacyFoamSkyColor;
}

float3 LitFoamColor(const LightingInput li, const FoamData foamData)
{
    float ndotl = (0.2f + 0.8f * saturate(dot(foamData.normal, -li.mainLight.direction)))
        * li.mainLight.shadowAttenuation;
    float3 skyAmbient = SkyFillRadiance(foamData.normal) * (li.ambient + 0.3f * (1.0f - foamData.normal.y));
    return foamData.albedo * foamTint.rgb * (ndotl * li.mainLight.color + skyAmbient);
}

float2 SubsurfaceScatteringFactor(const LightingInput li)
{
    float3 aligned = normalize(lerp(li.viewDir, li.normal, subsurfaceParams.w));
    float normalFactor = saturate(dot(aligned, li.viewDir));

    float heightOffset = li.referenceWaveHeight * (1.0f + heightFogParams.x);
    float heightFactor = saturate((li.positionWS.y + heightOffset) * 0.5f / max(0.5f, li.referenceWaveHeight));
    heightFactor = pow(abs(heightFactor), max(1.0f, li.referenceWaveHeight * 0.4f));

    float spread = max(subsurfaceParams.z, 1e-3f);
    float sunDot = saturate(dot(-li.mainLight.direction, -li.viewDir));
    float sunExponent = min(50.0f, 1.0f / spread);
    float sun = subsurfaceParams.x * normalFactor * heightFactor * pow(sunDot, sunExponent);

    float distFade = heightFogParams.y;
    float environment = subsurfaceParams.y * normalFactor * heightFactor * saturate(1.0f - li.viewDir.y);
    float fade = distFade / (distFade + li.viewDist + 1e-3f);
    return float2(sun, environment) * fade;
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

float meanFresnel(float cosThetaV, float sigmaV)
{
    return pow(abs(1.0f - cosThetaV), 5.0f * exp(-2.69f * sigmaV)) / (1.0f + 22.7f * pow(abs(sigmaV), 1.5f));
}

// V, N in wind space
float MeanFresnel(float3 V, float3 N, float2 sigmaSq)
{
    float2 v = V.xz; // view direction in wind space
    float2 t = v * v / (1.0f - V.y * V.y); // cos^2 and sin^2 of view direction
    float sigmaV2 = dot(t, sigmaSq); // slope variance in view direction
    return meanFresnel(dot(V, N), sqrt(sigmaV2));
}

float EffectiveFresnel(const LightingInput li, const BrunetonInputs bi)
{
    //(void)bi;
    //return saturate(SchlickFresnel(dot(li.viewDir, li.normal)));

    const float R = 0.02f;
    float fresnel = R + (1.0f - R) * MeanFresnel(
		bi.viewDirWind,
		bi.normalWind,
		bi.slopeVarianceSquared);
    return saturate(fresnel);
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

float2 OceanReflectionUvOffset(const LightingInput li, float3 adjustedNormal)
{
    float3 flatReflectDir = reflect(-li.viewDir, float3(0.0f, 1.0f, 0.0f));
    float3 waveReflectDir = reflect(-li.viewDir, adjustedNormal);
    float2 reflectionDelta = waveReflectDir.xz - flatReflectDir.xz;

    float distanceFade = saturate(li.viewDist / max(specularParams.z, 1.0f));
    float grazing = saturate(1.0f - abs(waveReflectDir.y));
    float strength = lerp(0.08f, 0.025f, distanceFade) * lerp(0.45f, 1.0f, grazing) * 2;
    return reflectionDelta * strength;
}

float OceanReflectionEdgeFade(float2 uv)
{
    float2 edgeDist = min(uv, float2(1.0f, 1.0f) - uv);
    return saturate(min(edgeDist.x, edgeDist.y) * 64.0f);
}

float3 Reflection(const LightingInput li)
{
    float reflectionNormalStrength = heightFogParams.w;
    float3 adjustedNormal = normalize(lerp(li.normal, float3(0.0f, 1.0f, 0.0f), reflectionNormalStrength));
    float3 reflectDir = reflect(-li.viewDir, adjustedNormal);

    float3 skySample = SkyboxTexture.SampleLevel(LinearClampSampler, reflectDir, 3).rgb;
    float2 reflectionUV = li.screenUV + OceanReflectionUvOffset(li, adjustedNormal);
    float edgeFade = OceanReflectionEdgeFade(reflectionUV);
    float4 oceanReflection = OceanReflectionTexture.SampleLevel(LinearClampSampler, saturate(reflectionUV), 0);
    float visibility = saturate(oceanReflection.a) * edgeFade;
    return oceanReflection.rgb * edgeFade + skySample * (1.0f - visibility);
}

float3 DeepScatterColor(float depthScale)
{
    return deepScatterColor.rgb;
}

float3 SssColor(float depthScale)
{
    return sssColor.rgb;
}

float3 DiffuseColor(float depthScale)
{
    return diffuseColor.rgb;
}

float3 AbsorptionTint(float attenuation)
{
    float4 colors[kGradientMaxKeys];
    [unroll]
    for (uint i = 0u; i < kGradientMaxKeys; ++i)
    {
        colors[i] = absorptionColors[i];
    }
    Gradient gradient = CreateGradient(colors, absorptionGradientParams.xy);
    return SampleGradient(gradient, attenuation);
}

float3 ColorThroughWater(float3 color, float3 volumeColor, float distThroughWater, float depth)
{
    distThroughWater = max(distThroughWater, 0.0f);
    depth = max(depth, 0.0f);

    float absorptionScale = max(refractionParams.z, 1.0f);
    float fogDensity = max(refractionParams.w, 0.0f);

    float attenuation = exp(-(distThroughWater + depth) / absorptionScale);
    float3 tinted = color * AbsorptionTint(attenuation);

    float fog = 1.0f - exp(-fogDensity * distThroughWater);
    return lerp(tinted, volumeColor, saturate(fog));
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
    float3 color = DeepScatterColor(depthScale);
    
    float3 sssColor = SssColor(depthScale);
    color += sssColor * saturate(sss.x + sss.y);
    
    //return color;

    float ndotl = saturate(dot(li.normal, -li.mainLight.direction));
    color += (ndotl * 0.8f + 0.2f) * li.mainLight.color * DiffuseColor(depthScale);
    
    //return color;

    float3 refractionCoords = RefractionCoords(refractionParams.x, li.positionNDC, li.viewDepth, li.normal);
    float3 backgroundColor = SceneColorTexture.SampleLevel(LinearClampSampler, refractionCoords.xy, 0).rgb;

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
    float3 horizonColor = SkyboxTexture.SampleLevel(LinearClampSampler, dir, 0).rgb;

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
    float3 reflected = Reflection(li);
    //return reflected;
    float3 refracted = Refraction(li, foamData, sss, foamLitColor);
    //return refracted;
    float4 horizon = HorizonBlend(li);
    //return horizon.aaa;

    float3 color = specular + lerp(refracted, reflected, fresnel);
    //color = fresnel.xxx;
    color = lerp(color, foamLitColor, foamData.coverage.x);
    color = lerp(color, horizon.rgb, horizon.a);
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
    light.shadowAttenuation = 1.0f;

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
                SceneColorTexture.SampleLevel(LinearClampSampler, screenUV, 0).rgb;
            color = lerp(softEdgeRefraction, color, refractionEdgeWeight);
        }
    }

    float4 outColor = float4(saturate(color), 1.0f);

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
