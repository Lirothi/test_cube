// Where each view has pages: per (view, mip) the rect of its mapped pages and per view their count (vsm_addressing.hlsli VsmBoundsIndex).
// A clipmap level is 16384 page-table entries and has a few dozen pages in a small rect round the camera (the request's level is the
// distance, so at most 2^(5 - bias) pages either side); a local view is 21845 entries, and a caster across its light's plane overlaps all of
// them. vsm_page_scatter_cs.hlsl walks a caster's pages inside these rects and vsm_page_propagate_cs.hlsl looks for a coarser page inside
// them. One group: two pages a thread, as the free list's sort. In the buffer, not in groupshared: 800 views x 9 entries do not fit there.
//
// And what each page was asked for as (PageDetail, bit 0: a pixel asked; bit 1: that changed this frame). UE keeps these flags on the
// physical page and takes a change as a full invalidation (VirtualShadowMapPhysicalPageManagement.usf:393-401): a page asked for only as a
// coarse one holds no small instances and has ignored what moves, so the first pixel to ask gets it redrawn.
#include "vsm_page_alloc_common.hlsli"

#define VSM_PAGE_BOUNDS_RS \
    "CBV(b0), " \
    "DescriptorTable(UAV(u0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

RWStructuredBuffer<uint> PhysOwner   : register(u0); // [gNumPages] physical -> virtual owner / VSM_INVALID
RWStructuredBuffer<uint> LevelBounds : register(u1); // vsm::LevelBoundsWords
RWStructuredBuffer<uint> Request     : register(u2); // this frame's marks (read), the pixels' and the coarse pages'
RWStructuredBuffer<uint> PageDetail  : register(u3); // [gNumPages]

[numthreads(1024, 1, 1)]
[RootSignature(VSM_PAGE_BOUNDS_RS)]
void CSMain(uint3 gtid : SV_GroupThreadID)
{
    const uint t = gtid.x;
    // Every rect empty first: min past max, the counts 0.
    const uint entries = gNumViews * VSM_BOUNDS_ENTRIES_PER_VIEW;
    const uint perThread = (entries + 1023u) / 1024u;
    const uint entryEnd = min((t + 1u) * perThread, entries);
    for (uint e = t * perThread; e < entryEnd; ++e)
    {
        LevelBounds[e * 4u] = 0xFFFFFFFFu;
        LevelBounds[e * 4u + 1u] = 0xFFFFFFFFu;
        LevelBounds[e * 4u + 2u] = 0u;
        LevelBounds[e * 4u + 3u] = 0u;
    }
    DeviceMemoryBarrierWithGroupSync();
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
        uint view, level, x, y;
        VsmDecodePage(owner, view, level, x, y);
        if (view >= gNumViews) { continue; }
        const uint bounds = VsmBoundsIndex(view, level);
        // Read first, as the request's marks do: most pages lie inside the rect already, and a read that races another thread's atomic costs one more atomic.
        uint previous;
        if (x < LevelBounds[bounds]) { InterlockedMin(LevelBounds[bounds], x, previous); }
        if (y < LevelBounds[bounds + 1u]) { InterlockedMin(LevelBounds[bounds + 1u], y, previous); }
        if (x > LevelBounds[bounds + 2u]) { InterlockedMax(LevelBounds[bounds + 2u], x, previous); }
        if (y > LevelBounds[bounds + 3u]) { InterlockedMax(LevelBounds[bounds + 3u], y, previous); }
        const uint hasPage = VsmBoundsIndex(view, VSM_BOUNDS_ANY);
        if (LevelBounds[hasPage] == 0u) { InterlockedOr(LevelBounds[hasPage], 1u, previous); }
    }
}
