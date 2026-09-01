// Rung 2 / Step 20, pass 3 (allocate): one thread per virtual page. For a page requested this
// frame that is NOT resident, pop a free physical page from the free list (atomic decrement of
// FreeCount) and map it: write the page-table entry, claim the physical page's owner + last-frame,
// and append the physical page to the needs-render list (Step 22 will rasterize casters into it).
// If the pool is full (free list empty) the page stays not-resident and FailCount is bumped.
#include "vsm_page_alloc_common.hlsli"

#define VSM_PAGE_ALLOC_MAP_RS \
    "CBV(b0), " \
    "DescriptorTable(UAV(u0, numDescriptors=7, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

RWStructuredBuffer<uint> Request       : register(u0); // request bitfield (read)
RWStructuredBuffer<uint> PageTable     : register(u1); // virtual -> packed entry (rw)
RWStructuredBuffer<uint> PhysOwner     : register(u2); // physical -> virtual owner (write)
RWStructuredBuffer<uint> PhysLastFrame : register(u3); // physical -> last requested frame (write)
RWStructuredBuffer<uint> FreeList      : register(u4); // free physical indices (read/pop)
RWStructuredBuffer<uint> NeedsRender   : register(u5); // physical pages to render this frame (append)
RWStructuredBuffer<uint> AllocCounters : register(u6); // FREE (pop) / NEEDS / FAIL

[numthreads(8, 8, 1)]
[RootSignature(VSM_PAGE_ALLOC_MAP_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.y != 0u) { return; }
    const uint i = dtid.x;
    if (i >= gNumEntries) { return; }
    if (!VsmRequested(Request, i)) { return; }

    const uint entry = PageTable[i];
    if ((entry & VSM_RESIDENT_BIT) != 0u) { return; } // already resident (touched)

    // Pop a free page: decrement FreeCount, use the pre-decrement value as a 1-based cursor.
    uint prevFree;
    InterlockedAdd(AllocCounters[VSM_CNT_FREE], 0xFFFFFFFFu, prevFree);
    if (prevFree == 0u || prevFree > gNumPages)
    {
        // Pool full (or underflow past empty) — leave not resident; sampling falls back.
        uint dummy;
        InterlockedAdd(AllocCounters[VSM_CNT_FAIL], 1u, dummy);
        return;
    }

    const uint phys = FreeList[prevFree - 1u];
    // ANY_LOD is set on a mapped page too: "mapped here" implies "resolvable here", so a sampler
    // tests one bit instead of two. The LOD offset field stays 0, which is what marks it as this
    // level's own page rather than a pointer to a coarser one.
    PageTable[i] = VSM_RESIDENT_BIT | VSM_ANY_LOD_BIT | (phys & VSM_PHYS_MASK);
    PhysOwner[phys] = i;
    PhysLastFrame[phys] = gCurFrame;

    uint nr;
    InterlockedAdd(AllocCounters[VSM_CNT_NEEDS], 1u, nr);
    if (nr < gNumPages) { NeedsRender[nr] = phys; }
}
