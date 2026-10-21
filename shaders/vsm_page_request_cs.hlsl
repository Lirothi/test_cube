// Rung 2 / Step 19b: screen-space page-request pass — mip-aware, local-light scope, reduced res.
// One thread per DOWN-SAMPLED screen block (1/kRequestDownscale per axis): reconstruct the
// block-center pixel's world position from camera depth, then for each active LOCAL shadow view
// (spot light or point-light cube face — NOT the CSM cascades, which stay on Pass_CSM until
// Step 24) project it into that view's virtual shadow space, pick the mip level from camera
// distance (dense-near / sparse-far), and mark the single virtual page it needs at that level.
// Produces the page-request bitfield (1 bit per virtual page across all views × levels); NOTHING
// consumes it yet (Step 20 allocates physical pages from it).
#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "vsm_addressing.hlsli" // plan S10b: the view layout (VsmLocalView, VSM_CLIPMAP_FIRST / END)

#define VSM_PAGE_REQUEST_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

// Must match the vsm:: constants in VirtualShadowMap.h; the view counts ARE them (plan S10a, Material.cpp
// EngineShaderDefines). Plan S10b: local views are [0, VSM_CLIPMAP_FIRST) and [VSM_CLIPMAP_END, numViews).
static const uint kMaxViews     = VSM_MAX_VIEWS;  // kMaxVirtualViews
static const uint kL0Axis       = 16u;   // kVirtualPagesL0Axis (kVirtualRes / kPageSize)
static const uint kMaxLevel     = 4u;    // kMaxMipLevel
static const uint kPagesPerView = 341u;  // kPagesPerView (16²+8²+4²+2²+1)
// Prefix sum of per-level page counts within a view (kLevelOffset[L] = vsm::LevelPageOffset(L)).
static const uint kLevelOffset[6] = { 0u, 256u, 320u, 336u, 340u, 341u };

struct ViewProjEntry
{
    float4x4 viewProj;
    float4   params; // x = valid (0/1), y = a point face's cube face (0..5; -1 = a spot / clipmap), z = zFar,
                     // w = the projection's x scale (S10e: the pixel footprint)
    float4   cull;   // plan S10e: the light's position + reach (w < 0 = none): a receiver it does not reach marks nothing
};

cbuffer PageRequestCB : register(b0)
{
    float4x4 invView;
    float4x4 invProj;
    float4   camPosWS;
    float4   screen;    // x=w, y=h, z=1/w, w=1/h
    float4   lodParams; // x=refDist, y=maxLevel, z=downscale, w=directional clipmap blend width
    // Plan S10e: x = 1 picks a local view's mip by the camera pixel's footprint (VsmLocalMipFromPixel, UE's), y = the lod
    // bias, z = the pixel radius per unit of view depth (min(1 / (W P00), 1 / (H P11)), render resolution), w = 1 tests a
    // receiver against each view's light sphere first (vsm.requestLightCull).
    float4   localMip;
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
    const uint  levelByDistance = (uint)clamp(rawLevel, 0, (int)maxLevel);

    const uint count = min(numViews, kMaxViews);

    // --- Local views (spots + point faces): per-view projection + a distance-selected mip chain. Plan S10b: the i-th
    // local view is VsmLocalView(i) -- the prefix before the clipmap, then the rest after it, up to numViews. ---
    const uint localCount = (count <= VSM_CLIPMAP_FIRST) ? count
                          : ((count > VSM_CLIPMAP_END) ? count - VSM_NUM_CLIPMAP_LEVELS : VSM_CLIPMAP_FIRST);
    // Plan S10e: the camera pixel's radius at this receiver (UE VirtualShadowMapCalcPixelFootprintLocal).
    const bool pixelMip = localMip.x != 0.0f;
    const float pixelRadiusWorld = dot(P - camPosWS.xyz, invView[2].xyz) * localMip.z;
    for (uint li = 0; li < localCount; ++li)
    {
        const uint v = VsmLocalView(li);
        if (views[v].params.x < 0.5f) { continue; } // inactive view slot
        // Plan S10e (UE marks a pixel's pages only for the lights its light-grid cell holds): beyond the light's reach
        // nothing samples this view, and of a point light's six faces only the one P's direction falls in can hold it --
        // one distance test and a face pick instead of six projections.
        const float4 cull = views[v].cull; // always the light's sphere (plan S10d reads it too); localMip.w = test it
        if (localMip.w != 0.0f && cull.w >= 0.0f)
        {
            const float3 toP = P - cull.xyz;
            if (dot(toP, toP) > cull.w * cull.w) { continue; }
            if (views[v].params.y >= 0.0f && VsmCubeFace(toP) != (uint)views[v].params.y) { continue; }
        }

        float4 clip = mul(float4(P, 1.0f), views[v].viewProj);
        if (clip.w <= 0.0f) { continue; }
        float3 ndc = clip.xyz / clip.w;
        if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f) { continue; }
        if (ndc.z < 0.0f || ndc.z > 1.0f) { continue; }
        // S10e: this view's mip -- the pixel's footprint in ITS texels (the light's distance and cone width), or the old
        // camera-distance level shared by every view.
        const uint level = pixelMip ? VsmLocalMipFromPixel(pixelRadiusWorld, views[v].params.w, clip.w, localMip.y)
                                    : levelByDistance;

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

    // --- Directional clipmap (Step 24d): views [VSM_CLIPMAP_FIRST, VSM_CLIPMAP_END) are camera-centered ortho
    // levels ordered finest -> coarsest by extent. The LEVELS are the LOD (not a mip chain). Mark
    // the finest containing level and, inside its transition band, the next coarser level too. A
    // zero blend width preserves the old exact-one-page request path. ---
    const float clipBlendWidth = clamp(lodParams.w, 0.0f, 0.5f);
    const uint clipEnd = min(count, VSM_CLIPMAP_END);
    for (uint cv = VSM_CLIPMAP_FIRST; cv < clipEnd; ++cv)
    {
        if (views[cv].params.x < 0.5f) { continue; }
        float4 cclip = mul(float4(P, 1.0f), views[cv].viewProj);
        if (cclip.w <= 0.0f) { continue; }
        float3 cndc = cclip.xyz / cclip.w;
        if (cndc.x < -1.0f || cndc.x > 1.0f || cndc.y < -1.0f || cndc.y > 1.0f) { continue; } // outside this level
        if (cndc.z < 0.0f || cndc.z > 1.0f) { continue; }
        float2 csuv = float2(0.5f * cndc.x + 0.5f, 0.5f - 0.5f * cndc.y);
        uint axis0 = kL0Axis; // clipmap levels use only level 0 (the level IS the LOD)
        uint cpx = min((uint)(csuv.x * axis0), axis0 - 1u);
        uint cpy = min((uint)(csuv.y * axis0), axis0 - 1u);
        uint cpage = cv * kPagesPerView + kLevelOffset[0] + cpy * axis0 + cpx;
        uint cprev;
        InterlockedOr(Request[cpage >> 5u], 1u << (cpage & 31u), cprev);

        // The sampler blends by the fine level's square-edge coordinate. Request the parent page
        // only in that same band, so width 0 has zero residency/raster cost and a narrow band adds
        // only the coarse pages that can actually receive a second PCF sample.
        if (clipBlendWidth > 0.0f && cv + 1u < clipEnd && views[cv + 1u].params.x >= 0.5f)
        {
            const float edge = max(abs(cndc.x), abs(cndc.y));
            if (edge > 1.0f - clipBlendWidth)
            {
                float4 pclip = mul(float4(P, 1.0f), views[cv + 1u].viewProj);
                if (pclip.w > 0.0f)
                {
                    float3 pndc = pclip.xyz / pclip.w;
                    if (all(abs(pndc.xy) <= 1.0f) && pndc.z >= 0.0f && pndc.z <= 1.0f)
                    {
                        float2 puv = float2(0.5f * pndc.x + 0.5f, 0.5f - 0.5f * pndc.y);
                        uint ppx = min((uint)(puv.x * (float)kL0Axis), kL0Axis - 1u);
                        uint ppy = min((uint)(puv.y * (float)kL0Axis), kL0Axis - 1u);
                        uint ppage = (cv + 1u) * kPagesPerView + kLevelOffset[0] + ppy * kL0Axis + ppx;
                        uint pprev;
                        InterlockedOr(Request[ppage >> 5u], 1u << (ppage & 31u), pprev);
                    }
                }
            }
        }
        break; // finest containing clipmap level only
    }
}
