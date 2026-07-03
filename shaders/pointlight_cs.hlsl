#define POINTLIGHT_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=7, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE))"
// t0..t3 : GBuffer (GB0, GB1, GB2, GBVelocity)
// t4     : Depth
// t5     : StructuredBuffer<PointLightData>
// t6     : TextureCubeArray point shadow depth cube (D16 -> R16), B2b
// u0     : Light accumulation RWTexture2D
// s0     : LinearClamp
// s1     : PointClamp
// s2     : ComparisonLinearClamp (shadow compare)

#pragma pack_matrix(row_major)

#include "utils.hlsl"

struct PointLightData
{
    float3 position;
    float radius;
    float3 color;
    float intensity;
    float4 shadowParams; // x = shadow slot (-1 = none), y = bias, z = nearPlane, w = farPlane (radius)
};

Texture2D GB0 : register(t0);
Texture2D GB1 : register(t1);
Texture2D GB2 : register(t2);
Texture2D GBVelocity : register(t3);
Texture2D DepthT : register(t4);
StructuredBuffer<PointLightData> PointLights : register(t5);
TextureCubeArray PointShadowCube : register(t6);
RWTexture2D<float4> LightTarget : register(u0);

SamplerState gSmpLinear : register(s0);
SamplerState gSmpPoint : register(s1);
SamplerComparisonState gSmpShadowCmp : register(s2);

// Omnidirectional (cube) point shadow, depth-cube approach (B3). Reconstructs the
// standard-projection compare depth from the world offset (matches the render:
// PerspectiveFovLH(90,1,near,far), LESS_EQUAL, clear 1.0 = far — NOT reverse-Z), then
// SampleCmpLevelZero on the cube-array slice. Returns 1 (lit) .. 0 (fully shadowed).
static const float kPointNormalBias = 0.05f;  // world units, along the surface normal (B4)
static const float kPointPcfRadius  = 0.015f; // PCF disk radius as a fraction of the light distance
// 8 cube-corner offset directions for the PCF kernel (each SampleCmp is itself 2x2 HW PCF).
static const float3 kPointPcfOffsets[8] = {
    float3( 1,  1,  1), float3( 1, -1,  1), float3(-1, -1,  1), float3(-1,  1,  1),
    float3( 1,  1, -1), float3( 1, -1, -1), float3(-1, -1, -1), float3(-1,  1, -1)
};
float PointShadowFactor(PointLightData Ld, float3 P, float3 N)
{
    if (Ld.shadowParams.x < 0.0f) { return 1.0f; } // this light has no shadow slot this frame
    float3 d = (P + N * kPointNormalBias) - Ld.position; // normal-offset receiver; HW picks the face from d
    float m = max(abs(d.x), max(abs(d.y), abs(d.z)));     // view-space Z on the selected face
    float nearP = Ld.shadowParams.z;
    float farP = max(Ld.shadowParams.w, nearP + 1e-3f);
    // WORLD-space depth bias: pull the compare distance toward the light BEFORE projecting.
    // A constant NDC bias is unusable — perspective depth is crushed into [~0.95,1] at any
    // real distance, so a fixed NDC bias is huge up close (no shadow) and nil far away.
    float mBiased = max(m - Ld.shadowParams.y, nearP);
    float zc = (farP / (farP - nearP)) * (1.0f - nearP / mBiased); // standard LH NDC depth
    // PCF: average 8 comparison taps on the cube. The offset scales with m so the world-space
    // filter footprint stays ~constant with distance (a fixed dir offset blurs near, sharpens far).
    float radius = m * kPointPcfRadius;
    float shadow = 0.0f;
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        float3 sd = d + kPointPcfOffsets[i] * radius;
        shadow += PointShadowCube.SampleCmpLevelZero(gSmpShadowCmp,
            float4(sd, Ld.shadowParams.x), zc); // .w = CUBE INDEX (slot)
    }
    return shadow * 0.125f;
}

cbuffer PointLightFrame : register(b0)
{
    float4x4 invView;
    float4x4 invProj;
    float3 camPosWS;
    uint lightCount;
    float2 screenSize;
    float2 invScreenSize;
}

[numthreads(8,8,1)]
[RootSignature(POINTLIGHT_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width = (uint)screenSize.x;
    uint height = (uint)screenSize.y;
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) * invScreenSize;

    float4 g0 = GB0.SampleLevel(gSmpLinear, uv, 0);
    float4 g1 = GB1.SampleLevel(gSmpLinear, uv, 0);
    float z = DepthT.SampleLevel(gSmpPoint, uv, 0).r;
    if (z <= kEpsilon)
    {
        return;
    }

    float3 albedo = g0.rgb;
    float2 rm = UnpackRM(g0.a);
    float rough = rm.x;
    float metal = rm.y;
    float3 N = normalize(g1.rgb * 2.0 - 1.0);
    float3 P = ReconstructPosWS(uv, z, invProj, invView);
    float3 V = normalize(camPosWS - P);

    float4 base = LightTarget[dispatchThreadId.xy];
    float3 accum = base.rgb;

    uint count = (uint)lightCount;
    for (uint i = 0; i < count; ++i)
    {
        PointLightData Ld = PointLights[i];
        float3 Lvec = Ld.position - P;
        float dist = length(Lvec);
        if (dist > Ld.radius || Ld.radius <= kEpsilon)
        {
            continue;
        }
        float3 L = Lvec / max(dist, kEpsilon);

        float x = saturate(1.0 - dist / Ld.radius);
        float atten = x * x;

        BRDFInput bi;
        bi.albedo = albedo;
        bi.rough = rough;
        bi.metal = metal;
        bi.N = N;
        bi.V = V;
        bi.L = L;

        BRDFResult br = EvalBRDF(bi);
        if (br.NdotL <= 0.0)
        {
            continue;
        }

        float shadow = PointShadowFactor(Ld, P, N);
        if (shadow <= 0.0f)
        {
            continue;
        }

        float3 radiance = Ld.color * Ld.intensity * atten;
        accum += (br.diffBRDF + br.specBRDF) * br.NdotL * radiance * shadow;
    }

    LightTarget[dispatchThreadId.xy] = float4(accum, base.a);
}
