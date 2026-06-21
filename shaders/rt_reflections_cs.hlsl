// Tier-1 hardware ray-traced reflections (S7).
//
// cs_6_5 inline RayQuery: trace one mirror reflection ray per GBuffer surface
// against the scene TLAS. On a hit that reprojects onto the visible surface at
// some screen pixel, sample the HDR light buffer for radiance; otherwise leave
// coverage 0 so compose's existing skybox fallback fills it. Writes premultiplied
// (rgb*coverage, coverage) into the SSR target so the unchanged Pass_SSR_Blur +
// Pass_Compose consume it (compose: refl = ssrRGB + sky*(1-ssrA), then Fresnel*gloss).
//
// Tier-1 has no bindless geometry/material table, so off-screen or occluded hits
// can't be shaded and fall back to skybox (S10 adds real hit shading). The b0
// layout matches ssr_cs.hlsl's PerFrame so WriteSsrConstants fills it.
#define RT_REFLECT_CS_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"
// t0: Scene TLAS, t1: LightTarget (HDR), t2: GB1 (world normal), t3: Depth
// u0: SSR output (premultiplied); s0 LinearClamp, s1 PointClamp

#pragma pack_matrix(row_major)
#include "utils.hlsl"

RaytracingAccelerationStructure Scene : register(t0);
Texture2D           LightTarget : register(t1);
Texture2D           GB1         : register(t2);
Texture2D           DepthT      : register(t3);
RWTexture2D<float4> SsrOut      : register(u0);
SamplerState        gSmp        : register(s0);
SamplerState        gSmpPoint   : register(s1);

cbuffer PerFrame : register(b0)
{
    float4x4 view, proj, invView, invProj;
    float    depthA, depthB, zNear, zFar;
    float2   screenSize;
    float2   invScreenSize;
    uint     tech;
    float3   _padding;
}

static const float kRayTMax = 1e4f;
static const float kRtEps   = 1e-6f;

float DepthToViewZ(float d) { return depthB / (d - depthA); }
float ReadDepth(float2 uv)  { return DepthT.SampleLevel(gSmpPoint, uv, 0).r; }

[numthreads(8, 8, 1)]
[RootSignature(RT_REFLECT_CS_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint outW, outH;
    SsrOut.GetDimensions(outW, outH);
    if (dtid.x >= outW || dtid.y >= outH)
    {
        return;
    }

    float2 uv = (float2(dtid.xy) + 0.5f) / float2(outW, outH);
    float depth = ReadDepth(uv);

    float4 result = float4(0.0f, 0.0f, 0.0f, 0.0f); // no surface / no reflection -> skybox via compose
    if (depth > kRtEps)
    {
        float3 N = normalize(GB1.SampleLevel(gSmp, uv, 0).rgb * 2.0f - 1.0f);
        float3 P = ReconstructPosWS(uv, depth, invProj, invView);
        float3 camPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), invView).xyz;
        float3 I = normalize(P - camPos); // camera -> surface
        float3 R = reflect(I, N);

        RayDesc ray;
        ray.Origin    = P + N * 0.02f;    // nudge off the surface
        ray.Direction = R;
        ray.TMin      = 0.0f;
        ray.TMax      = kRayTMax;

        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFFu, ray);
        while (q.Proceed()) {}

        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            // Reproject the hit point to screen and, if it is the visible surface
            // there, sample the lit HDR color (Tier-1 screen-color reflection).
            float3 hitWS = ray.Origin + R * q.CommittedRayT();
            float4 hv = mul(float4(hitWS, 1.0f), view);
            float4 hc = mul(hv, proj);
            if (hc.w > kRtEps)
            {
                float2 huv = (hc.xy / hc.w) * float2(0.5f, -0.5f) + 0.5f;
                if (all(huv >= 0.0f) && all(huv <= 1.0f))
                {
                    float sd = ReadDepth(huv);
                    if (sd > kRtEps)
                    {
                        float hitVZ = hv.z;
                        float visVZ = DepthToViewZ(sd);
                        // Visible iff the hit sits at the depth of what the camera
                        // sees at that pixel (else it's occluded -> skybox).
                        if (abs(hitVZ - visVZ) / max(visVZ, 1e-3f) < 0.05f)
                        {
                            float3 c = LightTarget.SampleLevel(gSmp, huv, 0).rgb;
                            result = float4(c, 1.0f); // premultiplied, full coverage
                        }
                    }
                }
            }
        }
    }

    SsrOut[dtid.xy] = result;
}
