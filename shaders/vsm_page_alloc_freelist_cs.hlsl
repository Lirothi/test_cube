// Rung 2 / Step 20, pass 2 (build free list): one thread per PHYSICAL page. A page is free if it
// has no owner, or its owner has not been requested for >= gLruThreshold frames (LRU eviction —
// build-free runs after touch, so a page requested this frame has last-frame == gCurFrame and is
// never evicted). Evicting clears the owner's page-table entry and releases the physical page.
// Free pages are appended to FreeList (atomic FreeCount); allocate pops from it.
#include "vsm_page_alloc_common.hlsli"

#define VSM_PAGE_ALLOC_FREELIST_RS \
    "CBV(b0), " \
    "DescriptorTable(UAV(u0, numDescriptors=5, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

RWStructuredBuffer<uint> PhysOwner     : register(u0); // physical -> virtual owner / VSM_INVALID (rw)
RWStructuredBuffer<uint> PhysLastFrame : register(u1); // physical -> last requested frame (read)
RWStructuredBuffer<uint> PageTable     : register(u2); // virtual -> packed entry (evict clears)
RWStructuredBuffer<uint> FreeList      : register(u3); // free physical indices (append)
RWStructuredBuffer<uint> AllocCounters : register(u4); // VSM_CNT_FREE atomic append cursor

[numthreads(8, 8, 1)]
[RootSignature(VSM_PAGE_ALLOC_FREELIST_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.y != 0u) { return; }
    const uint p = dtid.x;
    if (p >= gNumPages) { return; }

    const uint owner = PhysOwner[p];
    bool isFree = false;
    if (owner == VSM_INVALID)
    {
        isFree = true;
    }
    else
    {
        const uint age = gCurFrame - PhysLastFrame[p]; // gCurFrame >= last always
        if (age >= gLruThreshold)
        {
            // Evict: drop the virtual page's mapping and release this physical page.
            PageTable[owner] = 0u;
            PhysOwner[p] = VSM_INVALID;
            isFree = true;
        }
        else
        {
            // Kept resident (a carried-over cache page) — count it for the debug resident total.
            uint dummy;
            InterlockedAdd(AllocCounters[VSM_CNT_RESIDENT], 1u, dummy);
        }
    }

    if (isFree)
    {
        uint slot;
        InterlockedAdd(AllocCounters[VSM_CNT_FREE], 1u, slot);
        if (slot < gNumPages) { FreeList[slot] = p; }
    }
}
