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
    uint gCountElems; // kPoolPageCount * gNumGroups * kMaxShadowLods
    uint gNumPages;   // kPoolPageCount
    uint gClearB;     // S5b.2: 1 = clear PageGroupCountB + HzbCounters too
    uint gGroupsX;    // the dispatch's row width in thread groups (VirtualShadowMap::RecordPageRender)
};

RWStructuredBuffer<uint> PageGroupCount  : register(u0); // per (page, group) count / append cursor
RWStructuredBuffer<uint> PageScatterDyn  : register(u1); // per page: a dynamic caster landed here
RWStructuredBuffer<uint> PageGroupCountB : register(u2); // S5b.2: pass B's counts (same shape as u0)
RWStructuredBuffer<uint> HzbCounters     : register(u3); // S5b.2: 4 uints

// numthreads(8,8,1) to match RecordComputeDispatch's group size, and ALL 64 threads work: one element
// per thread, the groups laid out row-major in a 2-D grid gGroupsX wide. It used to be a 1-D dispatch
// with only row y == 0 alive, i.e. one group per 8 elements along X -- at 141 mesh groups that is
// 1024*141*4/8 = 72192 groups, past D3D12's 65535-per-dimension limit (debug layer id 1296), and the
// dispatch is then undefined: the counters of the high pages were never zeroed and kept growing.
[numthreads(8, 8, 1)]
[RootSignature(VSM_SCATTER_CLEAR_RS)]
void CSMain(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint i = (gid.y * gGroupsX + gid.x) * 64u + gi;
    if (i < gCountElems) { PageGroupCount[i] = 0u; }
    if (i < gNumPages) { PageScatterDyn[i] = 0u; }
    if (gClearB != 0u)
    {
        if (i < gCountElems) { PageGroupCountB[i] = 0u; }
        if (i < 4u) { HzbCounters[i] = 0u; }
    }
}
