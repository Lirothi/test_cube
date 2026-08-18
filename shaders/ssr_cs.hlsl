#define SSR_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=5, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"
// t0: LightTarget            (HDR color sampled at the marched hit)
// t1: GB1 (reflector normal.rgb, shading model ID in A; SSR currently consumes RGB only)
// t2: Depth  (R32F) marched against in screen space (the OPAQUE scene depth)
// t3: OriginDepth (R32F) reconstructs the reflector surface position. For opaque this is the
//     same texture as t2; for glass it is the glass front-face depth (origin) while t2 stays
//     the opaque depth (march), so glass rays reflect the opaque scene.
// t4: Hzb (P6C) -- the FURTHEST depth pyramid (min device Z), the same one GTAO reads. That is
//     what Unreal's own SSR binds (EHZBType::FurthestHZB); it is used as cheap pre-filtered depth,
//     not as a hierarchy. Built from the same opaque depth as t2.
// u0: SSR output (premultiplied RGBA)
// s0: LinearClamp, s1: PointClamp

#pragma pack_matrix(row_major)
#include "utils.hlsli" // UnpackRM if needed

Texture2D   LightTarget : register(t0);
Texture2D   GB1         : register(t1);
Texture2D   DepthT      : register(t2);
Texture2D   OriginDepthT : register(t3);
Texture2D   HzbFurthest : register(t4);
RWTexture2D<float4> SsrOut : register(u0);
SamplerState gSmp       : register(s0);
SamplerState gSmpPoint  : register(s1);

cbuffer PerFrame : register(b0)
{
    float4x4 view, proj, invView, invProj;
    float    depthA, depthB, zNear, zFar;
    float2   screenSize;
    float2   invScreenSize;
    uint     tech;
    // 0 = no depth pyramid exists yet (before the first build), so the UE march must not read it.
    uint     useHzb;
    uint     hzbMipCount;
    uint     frameIndexMod8;
    float2   hzbSize;     // pyramid mip 0, in texels (HALF the render resolution)
    float2   hzbInvSize;
}

static const float kEps = 1e-6f;

// Mirrors SsrTechnique in SceneFrameData.h. (A third option, a fixed-step screen-space march after
// Lettier's article, was removed with P6C step 6: LogMarch strictly dominated it and a third path
// turned every SSR comparison into a three-way.)
static const uint SSR_TECHNIQUE_LOGMARCH = 0u;
static const uint SSR_TECHNIQUE_UE = 1u;

float  DepthToViewZ_Fast(float d){ return depthB / (d - depthA); }
float3 ReconstructPosVS(float2 uv, float d){
    float2 ndc=UVtoNDC(uv); float4 clip=float4(ndc,d,1);
    float4 v=mul(clip, invProj); return v.xyz / max(v.w, kEps);
}
float  ReadDepth(float2 uv){ return DepthT.SampleLevel(gSmpPoint, uv, 0).r; }
#include "ssr_trace_logmarch.hlsli"
#include "ssr_trace_ue.hlsli" // Unreal's own SSR ray cast; reuses SSRHit / BuildSsrHit above

[numthreads(8, 8, 1)]
[RootSignature(SSR_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint renderWidth, renderHeight;
    LightTarget.GetDimensions(renderWidth, renderHeight);
    uint ssrWidth, ssrHeight;
    SsrOut.GetDimensions(ssrWidth, ssrHeight);

    if (dispatchThreadId.x >= ssrWidth || dispatchThreadId.y >= ssrHeight)
    {
        return;
    }

    float2 fullRes = float2(renderWidth, renderHeight);
    float2 ssrRes = float2(max(ssrWidth, 1u), max(ssrHeight, 1u));
    float2 pixelScale = fullRes / ssrRes;
    float2 pixel = (float2(dispatchThreadId.xy) + 0.5f) * pixelScale;
    float2 uv = pixel / fullRes;

    // Origin (reflector) depth: glass front-face depth for the glass pass, opaque depth for the
    // opaque pass (where t3 == t2). Pixels with no reflector (cleared depth 0) are skipped.
    float depth = OriginDepthT.SampleLevel(gSmpPoint, uv, 0).r;
    float4 result = float4(0, 0, 0, 0);

    if (depth > 1e-6f)
    {
        float3 N_ws = normalize(GB1.SampleLevel(gSmp, uv, 0).rgb * 2 - 1);
        float3 Pv   = ReconstructPosVS(uv, depth);
        float3 Nv   = normalize(mul(N_ws, (float3x3)view));

        SSRHit ssr;
        if (tech == SSR_TECHNIQUE_UE && useHzb != 0u)
        {
            ssr = TraceSSR_UeHzb(Pv, Nv, uv, depth, float2(dispatchThreadId.xy), frameIndexMod8);
        }
        else
        {
            // LogMarch, and the safety net for a frame with no pyramid yet.
            float2 seed = float2(dispatchThreadId.xy);
            ssr = TraceSSR_LogMarch(Pv, Nv, seed);
        }
        if (ssr.hit != 0)
        {
            int2 ip = int2(ssr.uv * float2(renderWidth, renderHeight) + 0.5);
            ip = clamp(ip, int2(0, 0), int2(int(renderWidth) - 1, int(renderHeight) - 1));
            float3 c = LightTarget.Load(int3(ip, 0)).rgb;
            float vis = ssr.visibility;
            result = float4(c * vis, vis);
        }
    }

    SsrOut[dispatchThreadId.xy] = result;
}
