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
    "DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

// Must match the vsm:: constants in VirtualShadowMap.h; the view counts ARE them (plan S10a, Material.cpp
// EngineShaderDefines). Plan S10b: local views are [0, VSM_CLIPMAP_FIRST) and [VSM_CLIPMAP_END, numViews).
static const uint kL0Axis       = 16u;   // kVirtualPagesL0Axis (kVirtualRes / kPageSize)
static const uint kMaxLevel     = 4u;    // kMaxMipLevel
static const uint kPagesPerView = 341u;  // kPagesPerView (16²+8²+4²+2²+1)
// Prefix sum of per-level page counts within a view (kLevelOffset[L] = vsm::LevelPageOffset(L)).
static const uint kLevelOffset[6] = { 0u, 256u, 320u, 336u, 340u, 341u };

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
    // Plan S9e (cost without a limit): the request walks the shadowed LIGHTS, not their views -- spots one view each
    // (vsm::SpotView), points six faces each from PointFaceView(slot * 6) (the extra lights' from gPointExtraView).
    uint     gSpotLights;
    uint     gPointLights;
    uint     gPointExtraView;
};

Texture2D                       DepthT  : register(t0);
StructuredBuffer<VsmViewData>   Views   : register(t1); // plan S9e: numViews records (the request slot)
RWStructuredBuffer<uint>        Request : register(u0);

// Sets a page's request bit -- read first, the atomic only while the bit is still clear. Every pixel of every light marks
// its page's whole coarser mip chain, so the coarse levels' few words took one InterlockedOr from nearly every thread:
// the same address, serialised (the bathroom at night, five candles: 0.55 ms). A read that races the first setter only
// costs a redundant atomic; nothing is lost.
void MarkPage(uint page)
{
    const uint word = page >> 5u;
    const uint bit = 1u << (page & 31u);
    if ((Request[word] & bit) != 0u) { return; }
    uint prev;
    InterlockedOr(Request[word], bit, prev);
}

// One local view's pages for receiver P: its projection, the mip (the pixel's footprint in ITS texels, plan S10e, or the
// camera-distance level), and that mip with every coarser one marked.
void MarkLocalView(uint v, float3 P, bool pixelMip, float pixelRadiusWorld, uint levelByDistance, uint maxLevel)
{
    const VsmViewData view = Views[v];
    if (view.params.x < 0.5f) { return; } // inactive view slot
    float4 clip = mul(float4(P, 1.0f), view.viewProj);
    if (clip.w <= 0.0f) { return; }
    float3 ndc = clip.xyz / clip.w;
    if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f) { return; }
    if (ndc.z < 0.0f || ndc.z > 1.0f) { return; }
    const uint level = pixelMip ? VsmLocalMipFromPixel(pixelRadiusWorld, view.params.w, clip.w, localMip.y + view.mip.x)
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
        MarkPage(v * kPagesPerView + kLevelOffset[L] + pageY * axisL + pageX);
    }
}

// Plan S10e (UE marks a pixel's pages only for the lights its light-grid cell holds): beyond the light's reach nothing
// samples its views. `cull` = the light's sphere (w < 0 = none).
bool OutsideLight(float4 cull, float3 P)
{
    const float3 toP = P - cull.xyz;
    return cull.w >= 0.0f && dot(toP, toP) > cull.w * cull.w;
}

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

    const uint count = min(numViews, VSM_VIEW_CEILING);

    // --- Local lights: per view, a projection + a mip chain. Plan S9e: walked per LIGHT (a point's six faces share its
    // sphere: one distance test, then only the face P's direction falls in -- VsmCubeFace, the sampler's rule), bounded
    // by the literal ceiling and stopped by the CB's counts. localMip.w = 0 (vsm.requestLightCull) tests nothing and
    // projects into every face. ---
    // Plan S10e: the camera pixel's radius at this receiver (UE VirtualShadowMapCalcPixelFootprintLocal).
    const bool pixelMip = localMip.x != 0.0f;
    const float pixelRadiusWorld = dot(P - camPosWS.xyz, invView[2].xyz) * localMip.z;
    const bool lightCull = localMip.w != 0.0f;
    [loop] for (uint s = 0; s < VSM_VIEW_CEILING; ++s)
    {
        if (s >= gSpotLights) { break; }
        const uint v = (s < VSM_PREFIX_SPOTS) ? s : VSM_CLIPMAP_END + (s - VSM_PREFIX_SPOTS); // vsm::SpotView
        if (lightCull && OutsideLight(Views[v].cull, P)) { continue; }
        MarkLocalView(v, P, pixelMip, pixelRadiusWorld, levelByDistance, maxLevel);
    }
    const uint prefixPoints = (VSM_CLIPMAP_FIRST - VSM_PREFIX_SPOTS) / 6u;
    [loop] for (uint q = 0; q < VSM_VIEW_CEILING / 6u; ++q)
    {
        if (q >= gPointLights) { break; }
        const uint first = (q < prefixPoints) ? VSM_PREFIX_SPOTS + 6u * q : gPointExtraView + 6u * (q - prefixPoints);
        const float4 cull = Views[first].cull; // the light's sphere, the same on all six faces
        if (lightCull && cull.w >= 0.0f)
        {
            if (OutsideLight(cull, P)) { continue; }
            MarkLocalView(first + VsmCubeFace(P - cull.xyz), P, pixelMip, pixelRadiusWorld, levelByDistance, maxLevel);
            continue;
        }
        [loop] for (uint face = 0u; face < 6u; ++face)
        {
            MarkLocalView(first + face, P, pixelMip, pixelRadiusWorld, levelByDistance, maxLevel);
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
        if (Views[cv].params.x < 0.5f) { continue; }
        float4 cclip = mul(float4(P, 1.0f), Views[cv].viewProj);
        if (cclip.w <= 0.0f) { continue; }
        float3 cndc = cclip.xyz / cclip.w;
        if (cndc.x < -1.0f || cndc.x > 1.0f || cndc.y < -1.0f || cndc.y > 1.0f) { continue; } // outside this level
        if (cndc.z < 0.0f || cndc.z > 1.0f) { continue; }
        float2 csuv = float2(0.5f * cndc.x + 0.5f, 0.5f - 0.5f * cndc.y);
        uint axis0 = kL0Axis; // clipmap levels use only level 0 (the level IS the LOD)
        uint cpx = min((uint)(csuv.x * axis0), axis0 - 1u);
        uint cpy = min((uint)(csuv.y * axis0), axis0 - 1u);
        MarkPage(cv * kPagesPerView + kLevelOffset[0] + cpy * axis0 + cpx);

        // The sampler blends by the fine level's square-edge coordinate. Request the parent page
        // only in that same band, so width 0 has zero residency/raster cost and a narrow band adds
        // only the coarse pages that can actually receive a second PCF sample.
        if (clipBlendWidth > 0.0f && cv + 1u < clipEnd && Views[cv + 1u].params.x >= 0.5f)
        {
            const float edge = max(abs(cndc.x), abs(cndc.y));
            if (edge > 1.0f - clipBlendWidth)
            {
                float4 pclip = mul(float4(P, 1.0f), Views[cv + 1u].viewProj);
                if (pclip.w > 0.0f)
                {
                    float3 pndc = pclip.xyz / pclip.w;
                    if (all(abs(pndc.xy) <= 1.0f) && pndc.z >= 0.0f && pndc.z <= 1.0f)
                    {
                        float2 puv = float2(0.5f * pndc.x + 0.5f, 0.5f - 0.5f * pndc.y);
                        uint ppx = min((uint)(puv.x * (float)kL0Axis), kL0Axis - 1u);
                        uint ppy = min((uint)(puv.y * (float)kL0Axis), kL0Axis - 1u);
                        MarkPage((cv + 1u) * kPagesPerView + kLevelOffset[0] + ppy * kL0Axis + ppx);
                    }
                }
            }
        }
        break; // finest containing clipmap level only
    }
}
