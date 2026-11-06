// The page request, UE's (VirtualShadowMapPageMarking.usf / .ush): one thread per pixel of a grid every
// vsm::g_requestDownscale pixels (r.Shadow.Virtual.PageMarkingPixelStride). The pixel's world position marks, for the sun,
// the one page of the clipmap level its distance picks (MarkClipmap) and, for each local light that reaches it, the one
// page of the mip its footprint picks (MarkLocalView); each with its neighbours across a page edge within
// PageDilationBorderSize, by one diagonal dithered over the group. A pixel turned away from the light marks nothing
// (r.Shadow.Virtual.CullBackfacingPixels). Thread (0, 0) also marks the coarse pages (MarkCoarsePages).
#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "vsm_addressing.hlsli" // plan S10b: the view layout (VsmLocalView, VSM_CLIPMAP_FIRST / END)

#define VSM_PAGE_REQUEST_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

// Plan S10b: local views are [0, VSM_CLIPMAP_FIRST) and [VSM_CLIPMAP_END, numViews).

cbuffer PageRequestCB : register(b0)
{
    float4x4 invView;
    float4x4 invProj;
    float4   camPosWS;
    float4   screen;    // x=w, y=h, z=1/w, w=1/h
    float4   lodParams; // x=refDist, y=maxLevel, z=downscale
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
    // The ocean's surface (x = 1 when it draws, y = its height): the forward water samples the clipmap there, and the
    // camera depth this pass reads is the opaque scene's (the water draws later).
    float4   waterPlane;
    // Where the water's marks go (vsm::g_waterCasterMask): x = the caster mask's first word in Request (the bitfield's
    // length, vsm_water_mask_cs.hlsl), y = 0 every water pixel, 1 only under a caster's box, 2 none (the control).
    // z = the coarse pages' own bitfield in Request: MarkCoarsePages's marks, apart from the pixels' (the setup asks which).
    uint4    waterMask;
    // The sun's clipmap: x = the resolution LOD bias, the frame's and the dynamic resolution's (GetBiasedClipmapLevel),
    // y = the first level's absolute number; z / w = PageDilationBorderSize, directional / local.
    float4   clipLod;
    // xyz = toward the sun, w = sin of its angular radius (IsBackfaceToDirectionalLight); xyz 0 = no sun.
    float4   lightToward;
    // x = 1 culls back-facing pixels; y bit 0 = the sun's coarse pages, bit 1 = the local lights' last mip;
    // z, w = the sun's coarse levels, as indices (r.Shadow.Virtual.Clipmap.First / LastCoarseLevel).
    uint4    markFlags;
};

Texture2D                       DepthT  : register(t0);
StructuredBuffer<VsmViewData>   Views   : register(t1); // plan S9e: numViews records (the request slot)
Texture2D                       GB1     : register(t2); // the shading normal
Texture2D                       GBAux   : register(t3); // the shading model
RWStructuredBuffer<uint>        Request : register(u0);

// Sets a page's request bit -- read first, the atomic only while the bit is still clear. Every pixel of every light marks
// its page's whole coarser mip chain, so the coarse levels' few words took one InterlockedOr from nearly every thread:
// the same address, serialised (the bathroom at night, five candles: 0.55 ms). A read that races the first setter only
// costs a redundant atomic; nothing is lost.
void MarkBit(uint base, uint page)
{
    const uint word = base + (page >> 5u);
    const uint bit = 1u << (page & 31u);
    if ((Request[word] & bit) != 0u) { return; }
    uint prev;
    InterlockedOr(Request[word], bit, prev);
}
void MarkPage(uint page) { MarkBit(0u, page); }
void MarkCoarsePage(uint page) { MarkBit(waterMask.z, page); }

// UE's MarkPage: the page under the point, and the neighbours PageDilationBorderSize away on the group's diagonal.
void MarkDilated(uint v, uint level, uint axis, float2 pageAddress, float2 dilation)
{
    const uint2 page = min((uint2)pageAddress, axis - 1u);
    MarkPage(VsmPageId(v, level, page.x, page.y));
    if (dilation.x == 0.0f) { return; }
    const uint2 page2 = min((uint2)max(pageAddress + dilation, 0.0f), axis - 1u);
    if (any(page2 != page)) { MarkPage(VsmPageId(v, level, page2.x, page2.y)); }
    const uint2 page3 = min((uint2)max(pageAddress - dilation, 0.0f), axis - 1u);
    if (any(page3 != page)) { MarkPage(VsmPageId(v, level, page3.x, page3.y)); }
}

// One local view's page for receiver P: its projection and the mip (the pixel's footprint in ITS texels, plan S10e, or the
// camera-distance level). One mip: what else a sampler may reach for is the last one, MarkCoarsePages's.
void MarkLocalView(uint v, float3 P, bool pixelMip, float pixelRadiusWorld, uint levelByDistance, uint maxLevel, float2 ditherSign)
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

    // Shadow UV (D3D clip -> UV, y flipped).
    const float2 suv = float2(0.5f * ndc.x + 0.5f, 0.5f - 0.5f * ndc.y);
    const uint axis = VSM_L0_AXIS >> level;
    MarkDilated(v, level, axis, suv * (float)axis, clipLod.w * ditherSign);
}

// Plan S10e (UE marks a pixel's pages only for the lights its light-grid cell holds): beyond the light's reach nothing
// samples its views. `cull` = the light's sphere (w < 0 = none).
bool OutsideLight(float4 cull, float3 P)
{
    const float3 toP = P - cull.xyz;
    return cull.w >= 0.0f && dot(toP, toP) > cull.w * cull.w;
}

// The sun (UE MarkPageDirectional, GetBiasedClipmapLevel): the level is the point's distance to the camera, biased by the
// resolution, and nothing else; the levels are nested, so the page exists. Past the last level nothing is marked.
void MarkClipmap(float3 P, uint count, bool water, float2 ditherSign)
{
    const float distCm = length(P - camPosWS.xyz) * 100.0f;
    const int level = (int)floor(log2(max(distCm, 1e-4f)) + clipLod.x);
    const uint index = (uint)max(0, level - (int)clipLod.y);
    if (index >= VSM_NUM_CLIPMAP_LEVELS) { return; }
    const uint cv = VSM_CLIPMAP_FIRST + index;
    if (cv >= count || Views[cv].params.x < 0.5f) { return; }
    const float4 cclip = mul(float4(P, 1.0f), Views[cv].viewProj);
    const float3 cndc = cclip.xyz / cclip.w;
    if (any(abs(cndc.xy) > 1.0f) || cndc.z < 0.0f || cndc.z > 1.0f) { return; }
    const float2 pageAddress = float2(0.5f * cndc.x + 0.5f, 0.5f - 0.5f * cndc.y) * (float)VSM_CLIPMAP_AXIS;
    // The water: no caster's box touches this point's sixteenth of the level, so nothing can shadow it on any level. It
    // stays unmarked and reads lit, which it is.
    if (water && waterMask.y != 0u)
    {
        if (waterMask.y == 2u) { return; }
        const uint2 cell = min((uint2)pageAddress, VSM_CLIPMAP_AXIS - 1u) >> 3u;
        const uint maskWord = waterMask.x + index * 8u + (cell.y >> 1u);
        if ((Request[maskWord] & (1u << (cell.x + (cell.y & 1u) * 16u))) == 0u) { return; }
    }
    MarkDilated(cv, 0u, VSM_CLIPMAP_AXIS, pageAddress, clipLod.z * ditherSign);
}

// UE's MarkCoarsePages. The coarse levels' pages round the camera, whatever the frame's pixels ask for, so anything that
// reads the sun's shadow off the receivers -- the fog's air -- has data; and each local view's last mip.
void MarkCoarsePages(uint count)
{
    if ((markFlags.y & 1u) != 0u)
    {
        for (uint index = 0u; index < VSM_NUM_CLIPMAP_LEVELS; ++index)
        {
            if (!((index >= markFlags.z && index <= markFlags.w) || index + 1u == VSM_NUM_CLIPMAP_LEVELS)) { continue; }
            const uint cv = VSM_CLIPMAP_FIRST + index;
            if (cv >= count || Views[cv].params.x < 0.5f) { continue; }
            const float2 ndc = mul(float4(camPosWS.xyz, 1.0f), Views[cv].viewProj).xy;
            const float2 pageAddress = float2(0.5f * ndc.x + 0.5f, 0.5f - 0.5f * ndc.y) * (float)VSM_CLIPMAP_AXIS;
            // Page addresses round down, so the four round the point.
            const int2 lo = clamp((int2)floor(pageAddress - 0.5f), 0, (int)VSM_CLIPMAP_AXIS - 1);
            const int2 hi = clamp((int2)ceil(pageAddress - 0.5f), 0, (int)VSM_CLIPMAP_AXIS - 1);
            MarkCoarsePage(VsmPageId(cv, 0u, (uint)lo.x, (uint)lo.y));
            MarkCoarsePage(VsmPageId(cv, 0u, (uint)lo.x, (uint)hi.y));
            MarkCoarsePage(VsmPageId(cv, 0u, (uint)hi.x, (uint)lo.y));
            MarkCoarsePage(VsmPageId(cv, 0u, (uint)hi.x, (uint)hi.y));
        }
    }
    if ((markFlags.y & 2u) != 0u)
    {
        [loop] for (uint s = 0u; s < VSM_VIEW_CEILING; ++s)
        {
            if (s >= gSpotLights) { break; }
            const uint v = (s < VSM_PREFIX_SPOTS) ? s : VSM_CLIPMAP_END + (s - VSM_PREFIX_SPOTS);
            if (v < count && Views[v].params.x >= 0.5f) { MarkCoarsePage(VsmPageId(v, VSM_MAX_LEVEL, 0u, 0u)); }
        }
        const uint prefix = (VSM_CLIPMAP_FIRST - VSM_PREFIX_SPOTS) / 6u;
        [loop] for (uint q = 0u; q < VSM_VIEW_CEILING / 6u; ++q)
        {
            if (q >= gPointLights) { break; }
            const uint first = (q < prefix) ? VSM_PREFIX_SPOTS + 6u * q : gPointExtraView + 6u * (q - prefix);
            for (uint face = 0u; face < 6u; ++face)
            {
                const uint v = first + face;
                if (v < count && Views[v].params.x >= 0.5f) { MarkCoarsePage(VsmPageId(v, VSM_MAX_LEVEL, 0u, 0u)); }
            }
        }
    }
}

[numthreads(8, 8, 1)]
[RootSignature(VSM_PAGE_REQUEST_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    const uint count = min(numViews, VSM_VIEW_CEILING);
    if (all(dtid.xy == 0u)) { MarkCoarsePages(count); }

    // UE: one diagonal is enough, as long as some pixel near the edge dithers across it.
    const float2 ditherSign = float2((gi & 1u) != 0u ? 1.0f : -1.0f, (gi & 2u) != 0u ? 1.0f : -1.0f);
    const uint w = (uint)screen.x;
    const uint h = (uint)screen.y;
    const uint ds = max((uint)lodParams.z, 1u);

    // Reduced-res: dtid indexes down-sampled blocks; sample the block-center full-res pixel.
    const uint2 px = dtid.xy * ds + (ds >> 1u);
    if (px.x >= w || px.y >= h) { return; }

    const float2 uv = (float2(px) + 0.5f) * screen.zw;
    const float z = DepthT.Load(int3(int2(px), 0)).r;
    const bool geometry = z > kEpsilon; // reverse-Z: 0 = far/cleared, the background

    // The water's surface (ocean_surface_common.hlsli OceanSunShadow samples the clipmap there): where this pixel's
    // view ray meets the ocean's plane before the opaque surface -- or the background -- its sun pages are marked too.
    // The plane, not the displaced wave: a crest moves the sample within a page or into its neighbour, which the
    // sampler's walk to a coarser resident level covers. Local lights are not the water's.
    if (waterPlane.x != 0.0f)
    {
        const float3 ray = ReconstructPosWS(uv, 1.0f, invProj, invView) - camPosWS.xyz; // reverse-Z: 1 = the near plane
        const float t = (abs(ray.y) > 1e-7f) ? (waterPlane.y - camPosWS.y) / ray.y : -1.0f;
        if (t > 0.0f)
        {
            const float3 Pw = camPosWS.xyz + ray * t;
            const float3 toW = Pw - camPosWS.xyz;
            bool inFront = !geometry;
            if (geometry)
            {
                const float3 toP = ReconstructPosWS(uv, z, invProj, invView) - camPosWS.xyz;
                inFront = dot(toW, toW) < dot(toP, toP);
            }
            if (inFront) { MarkClipmap(Pw, count, true, ditherSign); }
        }
    }
    if (!geometry) { return; }

    const float3 P = ReconstructPosWS(uv, z, invProj, invView);

    // Camera-distance mip: farther receivers need coarser shadow texels (screen-pixel world
    // footprint grows with distance). Distance heuristic first; ddx/ddy refinement is future.
    const float refDist  = max(lodParams.x, 1e-3f);
    const uint  maxLevel = min((uint)lodParams.y, VSM_MAX_LEVEL);
    const float distCam  = length(P - camPosWS.xyz);
    const int   rawLevel = (int)floor(log2(max(distCam, 1e-3f) / refDist));
    const uint  levelByDistance = (uint)clamp(rawLevel, 0, (int)maxLevel);

    // --- Local lights: per view, a projection + a mip chain. Plan S9e: walked per LIGHT (a point's six faces share its
    // sphere: one distance test, then only the face P's direction falls in -- VsmCubeFace, the sampler's rule), bounded
    // by the literal ceiling and stopped by the CB's counts. localMip.w = 0 (vsm.requestLightCull) tests nothing and
    // projects into every face. ---
    // Plan S10e: the camera pixel's radius at this receiver (UE VirtualShadowMapCalcPixelFootprintLocal).
    const bool pixelMip = localMip.x != 0.0f;
    const float pixelRadiusWorld = dot(P - camPosWS.xyz, invView[2].xyz) * localMip.z;
    const bool lightCull = localMip.w != 0.0f;
    // A pixel turned away from a light reads nothing from it (UE IsBackfaceToDirectionalLight / IsBackfaceToLocalLight, with
    // a source radius of 0 for a lamp); a surface that passes light through it (foliage) reads from both sides.
    const float3 N = GB1.Load(int3(int2(px), 0)).xyz * 2.0f - 1.0f;
    const bool backfaceCull = markFlags.x != 0u && DecodeShadingModel(GBAux.Load(int3(int2(px), 0)).b) != kShadingModelTwoSidedFoliage;
    [loop] for (uint s = 0; s < VSM_VIEW_CEILING; ++s)
    {
        if (s >= gSpotLights) { break; }
        const uint v = (s < VSM_PREFIX_SPOTS) ? s : VSM_CLIPMAP_END + (s - VSM_PREFIX_SPOTS); // vsm::SpotView
        if (lightCull && OutsideLight(Views[v].cull, P)) { continue; }
        if (backfaceCull && Views[v].cull.w >= 0.0f && dot(N, Views[v].cull.xyz - P) < 0.0f) { continue; }
        MarkLocalView(v, P, pixelMip, pixelRadiusWorld, levelByDistance, maxLevel, ditherSign);
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
            if (backfaceCull && dot(N, cull.xyz - P) < 0.0f) { continue; }
            MarkLocalView(first + VsmCubeFace(P - cull.xyz), P, pixelMip, pixelRadiusWorld, levelByDistance, maxLevel, ditherSign);
            continue;
        }
        [loop] for (uint face = 0u; face < 6u; ++face)
        {
            MarkLocalView(first + face, P, pixelMip, pixelRadiusWorld, levelByDistance, maxLevel, ditherSign);
        }
    }

    const bool sunBackface = backfaceCull && dot(N, lightToward.xyz) < -max(abs(lightToward.w), 0.1f);
    if (!sunBackface) { MarkClipmap(P, count, false, ditherSign); }
}
