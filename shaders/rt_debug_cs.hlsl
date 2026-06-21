// RT hit/visibility debug visualization (S6).
//
// cs_6_5 inline RayQuery against the scene TLAS: from each GBuffer surface,
// trace one ray along the reflection vector and write a hit-distance / miss
// visualization into the SSR target so it can be inspected via
// TextureDebugViewer -> Ssr. Debug-only — it runs AFTER compose and never feeds
// the reflection blend. The b0 layout matches ssr_cs.hlsl's PerFrame so the
// existing WriteSsrConstants path fills it.
#define RT_DEBUG_CS_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"
// t0: Scene TLAS, t1: GB1 (world normal.xyz packed 0..1), t2: Depth
// u0: debug output (SSR target); s0: LinearClamp, s1: PointClamp

#pragma pack_matrix(row_major)
#include "utils.hlsl"

RaytracingAccelerationStructure Scene : register(t0);
Texture2D           GB1      : register(t1);
Texture2D           DepthT   : register(t2);
RWTexture2D<float4> DebugOut : register(u0);
SamplerState        gSmp     : register(s0);
SamplerState        gSmpPoint: register(s1);

cbuffer PerFrame : register(b0)
{
    float4x4 view, proj, invView, invProj;
    float    depthA, depthB, zNear, zFar;
    float2   screenSize;
    float2   invScreenSize;
    uint     tech;
    float3   _padding;
}

static const float kRayTMax    = 1e4f;
static const float kVizMaxDist = 30.0f; // world units mapped onto the 0..1 brightness ramp

[numthreads(8, 8, 1)]
[RootSignature(RT_DEBUG_CS_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint outW, outH;
    DebugOut.GetDimensions(outW, outH);
    if (dtid.x >= outW || dtid.y >= outH)
    {
        return;
    }

    float2 uv = (float2(dtid.xy) + 0.5f) / float2(outW, outH);
    float depth = DepthT.SampleLevel(gSmpPoint, uv, 0).r;

    float4 result = float4(0.0f, 0.0f, 0.0f, 1.0f); // background / sky: black
    if (depth > 1e-6f)
    {
        float3 N = normalize(GB1.SampleLevel(gSmp, uv, 0).rgb * 2.0f - 1.0f);
        float3 P = ReconstructPosWS(uv, depth, invProj, invView);
        float3 camPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), invView).xyz;
        float3 I = normalize(P - camPos); // camera -> surface
        float3 R = reflect(I, N);         // reflection direction

        RayDesc ray;
        ray.Origin    = P + N * 0.01f;    // nudge off the surface to avoid self-hit
        ray.Direction = R;
        ray.TMin      = 0.0f;
        ray.TMax      = kRayTMax;

        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFFu, ray);
        while (q.Proceed()) {}

        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            // Hit: brightness by distance (near = white, far = dark grey).
            float c = saturate(1.0f - q.CommittedRayT() / kVizMaxDist);
            result = float4(c, c, c, 1.0f);
        }
        else
        {
            // Miss: the reflection ray escaped to the sky.
            result = float4(0.10f, 0.20f, 0.60f, 1.0f);
        }
    }

    DebugOut[dtid.xy] = result;
}
