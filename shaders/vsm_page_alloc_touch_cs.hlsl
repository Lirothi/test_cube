// Rung 2 / Step 20, pass 1 (touch): one thread per WORD of the request bitfield, its set bits walked (a level is 16384 pages
// now, nearly all of them unrequested: one thread a page was 322 k threads for a few hundred pages). For every page that is BOTH
// requested this frame AND already resident, stamp its physical page's last-requested-frame =
// current frame — so build-free's LRU keeps it alive (this is what makes a still camera's pages
// persist). Also zeroes the per-frame AllocCounters (threads 0..2) before build-free/allocate
// use them (same dispatch; a UAV barrier follows before build-free reads them).
#include "vsm_page_alloc_common.hlsli"

#define VSM_PAGE_ALLOC_TOUCH_RS \
    "CBV(b0), " \
    "DescriptorTable(UAV(u0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

RWStructuredBuffer<uint> Request       : register(u0); // request bitfield (read)
RWStructuredBuffer<uint> PageTable     : register(u1); // virtual page -> packed entry (read)
RWStructuredBuffer<uint> PhysLastFrame : register(u2); // physical page -> last requested frame (write)
RWStructuredBuffer<uint> AllocCounters : register(u3); // per-frame counters (reset)

[numthreads(64, 1, 1)]
[RootSignature(VSM_PAGE_ALLOC_TOUCH_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint word = dtid.x;

    // Reset this frame's counters (free / needs / fail / resident).
    if (word < VSM_CNT_COUNT) { AllocCounters[word] = 0u; }

    if (word >= (gNumEntries + 31u) / 32u) { return; }
    uint bits = Request[word] | Request[gCoarseBase + word];
    while (bits != 0u)
    {
        const uint i = word * 32u + firstbitlow(bits);
        bits &= bits - 1u;
        const uint entry = PageTable[i];
        if ((entry & VSM_RESIDENT_BIT) != 0u)
        {
            const uint phys = entry & VSM_PHYS_MASK;
            PhysLastFrame[phys] = gCurFrame; // keep-alive
        }
    }
}
