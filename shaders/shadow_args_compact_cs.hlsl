// Local-light shadow plan S6a: one ExecuteIndirect per atlas local-light view instead of one per mesh group.
//
// The cull leaves a row of D3D12_DRAW_INDEXED_ARGUMENTS per (view, VIRTUAL group) -- 564 on wind_test -- and the
// CPU walked every group that had a caster ANYWHERE, binding that mesh's VB/IB and issuing an ExecuteIndirect of
// one: ~170 us of recording per view, and thousands of zero-instance records for the GPU to step through. This
// kernel keeps a view's NON-EMPTY records only, rewritten for the consolidated mega VB/IB (the start and base vertex
// from GroupLodMega, as the VSM page setup writes them), and their count; the shadow pass then draws the whole view
// with ONE ExecuteIndirect reading that count. The order of a view's records is the arrival order of the atomics --
// a depth-only pass does not care.
//
// One group of 64 threads per view (RecordComputeDispatch: width = views x 8, height = 8). Cascade rows are not
// here: SDSM re-culls them later in the frame.
//
// b0: CompactParams   t0: GroupLodMega   u0: the cull's args (RAW, read)   u1: compact args (RAW)   u2: counts (RAW)
#define SHADOW_ARGS_COMPACT_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer CompactParams : register(b0)
{
    uint gNumGroups;  // virtual groups (a row's width)
    uint gFirstView;  // the first view compacted (render::kShadowViewCascades)
    uint gNumViews;   // views compacted
    uint gCompactPad;
};

StructuredBuffer<uint4> GroupLodMega : register(t0); // per (group, lod) = virtual group: {megaStart, lodRel, count, baseVertex}
RWByteAddressBuffer     Args         : register(u0); // [view * gNumGroups + vg], the cull's
RWByteAddressBuffer     CompactArgs  : register(u1); // [(view - gFirstView) * gNumGroups + k], k < count
RWByteAddressBuffer     CompactCount : register(u2); // [view - gFirstView]

// D3D12_DRAW_INDEXED_ARGUMENTS: 0 IndexCountPerInstance, 4 InstanceCount, 8 StartIndexLocation,
// 12 BaseVertexLocation, 16 StartInstanceLocation.
static const uint kArgStride = 20u;

groupshared uint gsCount;

[numthreads(8, 8, 1)]
[RootSignature(SHADOW_ARGS_COMPACT_RS)]
void CSMain(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint local = gid.x; // the dispatch has exactly gNumViews groups; the guard below never splits a group
    if (gi == 0u)
    {
        gsCount = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint view = gFirstView + local;
    const uint groups = (local < gNumViews) ? gNumGroups : 0u;
    for (uint vg = gi; vg < groups; vg += 64u)
    {
        const uint src = (view * gNumGroups + vg) * kArgStride;
        const uint instances = Args.Load(src + 4u);
        if (instances == 0u)
        {
            continue;
        }
        uint slot;
        InterlockedAdd(gsCount, 1u, slot);
        const uint4 e = GroupLodMega[vg];
        const uint dst = (local * gNumGroups + slot) * kArgStride;
        CompactArgs.Store(dst + 0u, Args.Load(src + 0u));
        CompactArgs.Store(dst + 4u, instances);
        CompactArgs.Store(dst + 8u, e.x);                 // mega-absolute start (LOD base + submesh range)
        CompactArgs.Store(dst + 12u, e.w);                // the mesh's base vertex in the mega VB
        CompactArgs.Store(dst + 16u, Args.Load(src + 16u)); // the view's visible-list slice
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u && local < gNumViews)
    {
        CompactCount.Store(local * 4u, gsCount);
    }
}
