// Spatial scatter cull, pass 0: zero the per (page, mesh-group) instance counts and the per-page
// dynamic-overlap flags before the scatter appends into them. One thread per count entry; the
// (much smaller) per-page flag array is cleared by the first kPoolPageCount threads.
// Occlusion plan S5b.2: also zeroes pass B's counts and the HZB counters when the two-pass light
// occlusion runs this frame (gClearB); placeholders are bound otherwise and left untouched.
#define VSM_SCATTER_CLEAR_RS \
    "CBV(b0), " \
    "DescriptorTable(UAV(u0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer ClearCB : register(b0)
{
    uint gCountElems; // kPoolPageCount * gNumGroups
    uint gNumPages;   // kPoolPageCount
    uint gClearB;     // S5b.2: 1 = clear PageGroupCountB + HzbCounters too
    uint _pad1;
};

RWStructuredBuffer<uint> PageGroupCount  : register(u0); // per (page, group) count / append cursor
RWStructuredBuffer<uint> PageScatterDyn  : register(u1); // per page: a dynamic caster landed here
RWStructuredBuffer<uint> PageGroupCountB : register(u2); // S5b.2: pass B's counts (same shape as u0)
RWStructuredBuffer<uint> HzbCounters     : register(u3); // S5b.2: 4 uints

// numthreads(8,8,1) to match RecordComputeDispatch's group size; the dispatch is 1-D so rows y>0 exit.
[numthreads(8, 8, 1)]
[RootSignature(VSM_SCATTER_CLEAR_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.y != 0u) { return; }
    const uint i = dtid.x;
    if (i < gCountElems) { PageGroupCount[i] = 0u; }
    if (i < gNumPages) { PageScatterDyn[i] = 0u; }
    if (gClearB != 0u)
    {
        if (i < gCountElems) { PageGroupCountB[i] = 0u; }
        if (i < 4u) { HzbCounters[i] = 0u; }
    }
}
