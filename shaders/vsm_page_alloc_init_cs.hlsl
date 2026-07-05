// Rung 2 / Step 20: one-shot init of the persistent VSM allocation state. Runs once (first
// RecordPageAllocate) — clears the page table to "not resident" and marks every physical page
// free (no owner, last-frame 0). The pool + this state persist across level switches (the pool
// IS the cache), so this must NOT run per frame.
#include "vsm_page_alloc_common.hlsli"

#define VSM_PAGE_ALLOC_INIT_RS \
    "CBV(b0), " \
    "DescriptorTable(UAV(u0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

RWStructuredBuffer<uint> PageTable     : register(u0); // [gNumEntries] virtual page -> packed entry
RWStructuredBuffer<uint> PhysOwner     : register(u1); // [gNumPages] physical page -> virtual owner / VSM_INVALID
RWStructuredBuffer<uint> PhysLastFrame : register(u2); // [gNumPages] physical page -> last requested frame

[numthreads(8, 8, 1)]
[RootSignature(VSM_PAGE_ALLOC_INIT_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.y != 0u) { return; }
    const uint i = dtid.x;
    if (i < gNumEntries) { PageTable[i] = 0u; }
    if (i < gNumPages)   { PhysOwner[i] = VSM_INVALID; PhysLastFrame[i] = 0u; }
}
