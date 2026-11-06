// Rung 2 / Step 20, pass 2 (build free list): the pages allocate may take this frame, in the order it takes them --
// UE's PHYSICAL_PAGE_LIST_AVAILABLE (VirtualShadowMapPhysicalPageManagement.usf: UpdatePhysicalPages, PackAvailablePages,
// AppendPhysicalPageLists; PopPhysicalPageList takes from the END). A page is
//   - EMPTY: no owner, or its owner has not been requested for >= gLruThreshold frames (UE MaxPageAgeSinceLastRequest:
//     evicted here, its page-table entry cleared) -- or for >= gRedrawUnrequestedFrames and the last render found it
//     dirty. UE never redraws a page nobody requested (it keeps the invalidation and redraws on the next request); our
//     setup redraws every dirty resident page, because the sub-sampled request (requestDownscale) misses pages that
//     pixels still read. Past that window the redraw buys nothing, so the page goes and is redrawn when asked for again
//     -- UE's cost;
//   - AVAILABLE: resident and cached, but not requested this frame (touch ran first, so a requested page has
//     last-frame == gCurFrame) -- it keeps its content and its mapping, and allocate evicts it only if it has to;
//   - in use: requested this frame. Not listed.
// The list is [available, most recently requested first ... least recently last | empty], so allocate's pop from the end
// takes the empty pages first and then the least recently requested cached ones: LRU under pressure, nothing thrown away
// while there is room. FreeCount = the list's length (UE's "available" count, the dynamic-resolution load's free pages).
// One group of 1024 threads, each handling two pages of the 2048 (t and t + 1024), sorted in groupshared by a bitonic sort:
// the comparators of a step are disjoint pairs, so one thread may run two of them.
#include "vsm_page_alloc_common.hlsli"

#define VSM_PAGE_ALLOC_FREELIST_RS \
    "CBV(b0), " \
    "DescriptorTable(UAV(u0, numDescriptors=6, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

RWStructuredBuffer<uint> PhysOwner     : register(u0); // physical -> virtual owner / VSM_INVALID (rw)
RWStructuredBuffer<uint> PhysLastFrame : register(u1); // physical -> last requested frame (read)
RWStructuredBuffer<uint> PageTable     : register(u2); // virtual -> packed entry (evict clears)
RWStructuredBuffer<uint> FreeList      : register(u3); // the pages allocate may take, in pop order (write)
RWStructuredBuffer<uint> AllocCounters : register(u4); // VSM_CNT_FREE = list length, VSM_CNT_RESIDENT = pages kept
RWStructuredBuffer<uint> PerPageDirty  : register(u5); // the last render's verdict per page (read; 1 = it was redrawn)

#define VSM_FREELIST_THREADS 1024
#define VSM_FREELIST_PAGES 2048 // kPoolPageCount
static const uint kPageBits = 11u;
static const uint kPageMask = 0x7FFu;
// Sort key, above the page index's 11 bits: the age of an available page (1 .. gLruThreshold - 1, ascending = most
// recently requested first), then every empty page, then the pages in use (past the list's end).
static const uint kKeyEmpty = 0xFFFFEu;
static const uint kKeyInUse = 0xFFFFFu;

groupshared uint gsKey[VSM_FREELIST_PAGES];
groupshared uint gsListed;

uint PageKey(uint p)
{
    uint key = kKeyInUse;
    if (p < gNumPages)
    {
        const uint owner = PhysOwner[p];
        if (owner == VSM_INVALID)
        {
            key = kKeyEmpty;
        }
        else
        {
            const uint age = gCurFrame - PhysLastFrame[p]; // gCurFrame >= last always
            const bool redrawnForNobody = gRedrawUnrequestedFrames != 0u && age >= gRedrawUnrequestedFrames &&
                                          PerPageDirty[p] != 0u;
            if (age >= gLruThreshold || redrawnForNobody)
            {
                // Too old to keep (UE: its flags go to 0 and it joins the EMPTY list): drop the mapping, free the page.
                PageTable[owner] = 0u;
                PhysOwner[p] = VSM_INVALID;
                key = kKeyEmpty;
            }
            else
            {
                // Kept resident (requested, or a carried-over cache page) -- the debug resident total; allocate takes
                // one off for every cached page it evicts.
                uint dummy;
                InterlockedAdd(AllocCounters[VSM_CNT_RESIDENT], 1u, dummy);
                key = (age == 0u) ? kKeyInUse : min(age, kKeyEmpty - 1u);
            }
        }
        if (key != kKeyInUse)
        {
            uint slot;
            InterlockedAdd(gsListed, 1u, slot);
        }
    }
    return (key << kPageBits) | (p & kPageMask);
}

[numthreads(VSM_FREELIST_THREADS, 1, 1)]
[RootSignature(VSM_PAGE_ALLOC_FREELIST_RS)]
void CSMain(uint3 gtid : SV_GroupThreadID)
{
    const uint t = gtid.x;
    if (t == 0u) { gsListed = 0u; }
    GroupMemoryBarrierWithGroupSync();

    const uint pages[2] = { t, t + VSM_FREELIST_THREADS };
    gsKey[pages[0]] = PageKey(pages[0]);
    gsKey[pages[1]] = PageKey(pages[1]);
    GroupMemoryBarrierWithGroupSync();

    // Bitonic sort, ascending. Literal bounds: every thread runs every step (the barriers need it).
    [loop]
    for (uint k = 2u; k <= VSM_FREELIST_PAGES; k <<= 1u)
    {
        [loop]
        for (uint j = k >> 1u; j > 0u; j >>= 1u)
        {
            [unroll] for (uint h = 0u; h < 2u; ++h)
            {
                const uint p = pages[h];
                const uint q = p ^ j;
                if (q > p)
                {
                    const uint a = gsKey[p];
                    const uint b = gsKey[q];
                    const bool ascending = (p & k) == 0u;
                    if ((a > b) == ascending)
                    {
                        gsKey[p] = b;
                        gsKey[q] = a;
                    }
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }

    const uint listed = gsListed;
    if (pages[0] < listed) { FreeList[pages[0]] = gsKey[pages[0]] & kPageMask; }
    if (pages[1] < listed) { FreeList[pages[1]] = gsKey[pages[1]] & kPageMask; }
    if (t == 0u) { AllocCounters[VSM_CNT_FREE] = listed; }
}
