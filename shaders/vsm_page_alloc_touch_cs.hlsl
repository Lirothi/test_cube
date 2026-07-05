// Rung 2 / Step 20, pass 1 (touch): one thread per virtual page. For every page that is BOTH
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

[numthreads(8, 8, 1)]
[RootSignature(VSM_PAGE_ALLOC_TOUCH_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.y != 0u) { return; }
    const uint i = dtid.x;

    // Reset this frame's counters (free / needs / fail / resident).
    if (i < VSM_CNT_COUNT) { AllocCounters[i] = 0u; }

    if (i >= gNumEntries) { return; }
    if (!VsmRequested(Request, i)) { return; }

    const uint entry = PageTable[i];
    if ((entry & VSM_RESIDENT_BIT) != 0u)
    {
        const uint phys = entry & VSM_PHYS_MASK;
        PhysLastFrame[phys] = gCurFrame; // keep-alive
    }
}
