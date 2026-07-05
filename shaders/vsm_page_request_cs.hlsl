// Rung 2 / Step 19b: screen-space page-request pass — mip-aware, local-light scope, reduced res.
// One thread per DOWN-SAMPLED screen block (1/kRequestDownscale per axis): reconstruct the
// block-center pixel's world position from camera depth, then for each active LOCAL shadow view
// (spot light or point-light cube face — NOT the CSM cascades, which stay on Pass_CSM until
// Step 24) project it into that view's virtual shadow space, pick the mip level from camera
// distance (dense-near / sparse-far), and mark the single virtual page it needs at that level.
// Produces the page-request bitfield (1 bit per virtual page across all views × levels); NOTHING
// consumes it yet (Step 20 allocates physical pages from it).
#pragma pack_matrix(row_major)
#include "utils.hlsl"

#define VSM_PAGE_REQUEST_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

// Must match the vsm:: constants in VirtualShadowMap.h.
static const uint kMaxViews     = 32u;   // kMaxVirtualViews (spots + point faces, no cascades)
static const uint kL0Axis       = 16u;   // kVirtualPagesL0Axis (kVirtualRes / kPageSize)
static const uint kMaxLevel     = 4u;    // kMaxMipLevel
static const uint kPagesPerView = 341u;  // kPagesPerView (16²+8²+4²+2²+1)
// Prefix sum of per-level page counts within a view (kLevelOffset[L] = vsm::LevelPageOffset(L)).
static const uint kLevelOffset[6] = { 0u, 256u, 320u, 336u, 340u, 341u };

struct ViewProjEntry
{
    float4x4 viewProj;
    float4   params; // x = valid (0/1), y = zNear, z = zFar
};

cbuffer PageRequestCB : register(b0)
{
    float4x4 invView;
    float4x4 invProj;
    float4   camPosWS;
    float4   screen;    // x=w, y=h, z=1/w, w=1/h
    float4   lodParams; // x=refDist, y=maxLevel, z=downscale, w=unused
    uint     numViews;
    uint3    _pad;
    ViewProjEntry views[kMaxViews];
};

Texture2D                DepthT  : register(t0);
RWStructuredBuffer<uint> Request : register(u0);

[numthreads(8, 8, 1)]
[RootSignature(VSM_PAGE_REQUEST_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint w = (uint)screen.x;
    const uint h = (uint)screen.y;
    const uint ds = max((uint)lodParams.z, 1u);

    // Reduced-res: dtid indexes down-sampled blocks; sample the block-center full-res pixel.
    const uint2 px = dtid.xy * ds + (ds >> 1u);
    if (px.x >= w || px.y >= h) { return; }

    const float2 uv = (float2(px) + 0.5f) * screen.zw;
    const float z = DepthT.Load(int3(int2(px), 0)).r;
    if (z <= kEpsilon) { return; } // background / no geometry (reverse-Z: 0 = far/cleared)

    const float3 P = ReconstructPosWS(uv, z, invProj, invView);

    // Camera-distance mip: farther receivers need coarser shadow texels (screen-pixel world
    // footprint grows with distance). Distance heuristic first; ddx/ddy refinement is future.
    const float refDist  = max(lodParams.x, 1e-3f);
    const uint  maxLevel = min((uint)lodParams.y, kMaxLevel);
    const float distCam  = length(P - camPosWS.xyz);
    const int   rawLevel = (int)floor(log2(max(distCam, 1e-3f) / refDist));
    const uint  level    = (uint)clamp(rawLevel, 0, (int)maxLevel);

    const uint count = min(numViews, kMaxViews);
    for (uint v = 0; v < count; ++v)
    {
        if (views[v].params.x < 0.5f) { continue; } // inactive view slot

        float4 clip = mul(float4(P, 1.0f), views[v].viewProj);
        if (clip.w <= 0.0f) { continue; }
        float3 ndc = clip.xyz / clip.w;
        if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f) { continue; }
        if (ndc.z < 0.0f || ndc.z > 1.0f) { continue; }

        // Shadow UV (D3D clip -> UV, y flipped). Mark the selected level AND all coarser levels (a
        // mip chain): the sampler may pick a slightly different level per pixel, so keeping the
        // coarser pages resident gives it something to fall back to (no distance pop-in). Coarser
        // levels have very few pages (level 4 = 1/view), so the extra requests are cheap.
        float2 suv = float2(0.5f * ndc.x + 0.5f, 0.5f - 0.5f * ndc.y);
        for (uint L = level; L <= maxLevel; ++L)
        {
            uint axisL = kL0Axis >> L;
            uint pageX = min((uint)(suv.x * axisL), axisL - 1u);
            uint pageY = min((uint)(suv.y * axisL), axisL - 1u);
            uint page = v * kPagesPerView + kLevelOffset[L] + pageY * axisL + pageX;
            uint prev;
            InterlockedOr(Request[page >> 5u], 1u << (page & 31u), prev);
        }
    }
}
