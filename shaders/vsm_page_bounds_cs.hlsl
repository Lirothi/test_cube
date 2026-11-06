// Where each clipmap level has pages: per level the rect of its mapped pages, two words (min x | min y << 16, max x | max y << 16;
// nothing mapped: min past max). A level is 16384 page-table entries and has a few dozen pages, in a small rect round the camera (the
// request's level is the distance, so at most 2^(5 - bias) pages either side). vsm_page_scatter_cs.hlsl walks a caster's pages inside it
// and vsm_page_propagate_cs.hlsl looks for a coarser page inside it. One group: two pages a thread, as the free list's sort.
//
// And what each page was asked for as (PageDetail, bit 0: a pixel asked; bit 1: that changed this frame). UE keeps these flags on the
// physical page and takes a change as a full invalidation (VirtualShadowMapPhysicalPageManagement.usf:393-401): a page asked for only as a
// coarse one holds no small instances and has ignored what moves, so the first pixel to ask gets it redrawn.
#include "vsm_page_alloc_common.hlsli"

#define VSM_PAGE_BOUNDS_RS \
    "CBV(b0), " \
    "DescriptorTable(UAV(u0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

RWStructuredBuffer<uint> PhysOwner   : register(u0); // [gNumPages] physical -> virtual owner / VSM_INVALID
RWStructuredBuffer<uint> LevelBounds : register(u1); // [VSM_NUM_CLIPMAP_LEVELS * 2]
RWStructuredBuffer<uint> Request     : register(u2); // this frame's marks (read), the pixels' and the coarse pages'
RWStructuredBuffer<uint> PageDetail  : register(u3); // [gNumPages]

groupshared uint gsMinX[VSM_NUM_CLIPMAP_LEVELS];
groupshared uint gsMinY[VSM_NUM_CLIPMAP_LEVELS];
groupshared uint gsMaxX[VSM_NUM_CLIPMAP_LEVELS];
groupshared uint gsMaxY[VSM_NUM_CLIPMAP_LEVELS];

[numthreads(1024, 1, 1)]
[RootSignature(VSM_PAGE_BOUNDS_RS)]
void CSMain(uint3 gtid : SV_GroupThreadID)
{
    const uint t = gtid.x;
    if (t < VSM_NUM_CLIPMAP_LEVELS)
    {
        gsMinX[t] = 0xFFFFu;
        gsMinY[t] = 0xFFFFu;
        gsMaxX[t] = 0u;
        gsMaxY[t] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint h = 0u; h < 2u; ++h)
    {
        const uint p = t + h * 1024u;
        const uint owner = PhysOwner[p];
        if (owner == VSM_INVALID) { continue; }
        const uint ownerBit = 1u << (owner & 31u);
        const bool pixelAsked = (Request[owner >> 5u] & ownerBit) != 0u;
        const bool coarseAsked = (Request[gCoarseBase + (owner >> 5u)] & ownerBit) != 0u;
        const uint was = PageDetail[p] & 1u;
        const uint now = (pixelAsked || coarseAsked) ? ((pixelAsked || gCoarseCache == 0u) ? 1u : 0u) : was;
        PageDetail[p] = now | (now != was ? 2u : 0u);
        const uint view = VsmViewOfPage(owner);
        if (!VsmIsClipmapView(view)) { continue; }
        const uint level = view - VSM_CLIPMAP_FIRST;
        const uint inLevel = owner - VsmViewBase(view);
        const uint x = inLevel % VSM_CLIPMAP_AXIS;
        const uint y = inLevel / VSM_CLIPMAP_AXIS;
        InterlockedMin(gsMinX[level], x);
        InterlockedMin(gsMinY[level], y);
        InterlockedMax(gsMaxX[level], x);
        InterlockedMax(gsMaxY[level], y);
    }
    GroupMemoryBarrierWithGroupSync();
    if (t < VSM_NUM_CLIPMAP_LEVELS)
    {
        LevelBounds[t * 2u] = gsMinX[t] | (gsMinY[t] << 16u);
        LevelBounds[t * 2u + 1u] = gsMaxX[t] | (gsMaxY[t] << 16u);
    }
}
