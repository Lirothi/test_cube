// Rung 0 / Step 4: init the per-(view, mesh-group) indirect draw arguments before the cull
// atomically bumps their InstanceCount. One thread per (view, mesh-group). Also seeds the
// per-view draw count (= number of mesh-groups; empty groups draw with InstanceCount 0).
#define SHADOW_CULL_CLEAR_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer CullParams : register(b0)
{
    uint gNumCasters;
    uint gNumViews;
    uint gNumGroups;
    uint gPad;
};

// x = base offset (this group's slice within a view's visible-list region), y = index count,
// z = start index (B3: a group is a (mesh, submesh) range; whole-mesh groups carry z = 0).
StructuredBuffer<uint4> PerGroup : register(t0);

RWByteAddressBuffer      Args   : register(u0); // D3D12_DRAW_INDEXED_ARGUMENTS[view*numGroups + group]
RWStructuredBuffer<uint> Counts : register(u1); // per-view ExecuteIndirect command count
// Occlusion plan S5b: the cascade HZB cull's counters -- [0..4) casters deferred by the main
// cull, [4..8) casters the post cull drew; S5: [8] the camera's deferred list length, [9] the
// same weighted (a fading caster twice), [10] pass-B entries, [11] spare. Zeroed here so the
// cull's InterlockedAdds start from nothing; the first 12 threads of the view axis each clear one.
RWStructuredBuffer<uint> DeferredCount : register(u2);
static const uint kDeferredCounters = 12u;

// D3D12_DRAW_INDEXED_ARGUMENTS is 5 x uint = 20 bytes:
//   0:IndexCountPerInstance 4:InstanceCount 8:StartIndexLocation 12:BaseVertexLocation 16:StartInstanceLocation
static const uint kArgStride = 20u;

[numthreads(8, 8, 1)]
[RootSignature(SHADOW_CULL_CLEAR_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.y != 0u) { return; }
    uint idx = dtid.x;
    uint total = gNumViews * gNumGroups;
    if (idx >= total) { return; }

    uint v = idx / gNumGroups;
    uint g = idx % gNumGroups;
    // S3.6: the bound buffer is now perGroupVg_ -- per VIRTUAL group (group * kMaxShadowLods +
    // receiver LOD), which is VIEW-INDEPENDENT, so indexing by the group alone is correct here.
    // It was NOT before: the buffer was perViewGroup_, laid out (view * numGroups + group), and
    // this same index handed EVERY view cascade 0's index range while the draw bound that view's
    // own LOD index buffer. Measured at ~6.5% of the frame. The view axis is gone now, not
    // papered over -- the LOD moved into the group id.
    uint4 pg = PerGroup[g];

    uint base = idx * kArgStride;
    Args.Store(base + 0u,  pg.y);                        // IndexCountPerInstance = group (submesh) index count
    Args.Store(base + 4u,  0u);                          // InstanceCount (bumped by the cull)
    Args.Store(base + 8u,  pg.z);                        // StartIndexLocation = submesh range start (B3)
    Args.Store(base + 12u, 0u);                          // BaseVertexLocation
    Args.Store(base + 16u, v * gNumCasters + pg.x);      // StartInstanceLocation -> visible-list slice

    if (g == 0u) { Counts[v] = gNumGroups; }
    if (g == 0u && v < kDeferredCounters) { DeferredCount[v] = 0u; }
}
