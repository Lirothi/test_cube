// Rung 0 / Step 4: init the per-(view, mesh-group) indirect draw arguments before the cull
// atomically bumps their InstanceCount. One thread per (view, mesh-group). Also seeds the
// per-view draw count (= number of mesh-groups; empty groups draw with InstanceCount 0).
#define SHADOW_CULL_CLEAR_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer CullParams : register(b0)
{
    uint gNumCasters;
    uint gNumViews;
    uint gNumGroups;
    uint gPad;
};

// x = base offset (this group's slice within a view's visible-list region), y = mesh index count.
StructuredBuffer<uint2> PerGroup : register(t0);

RWByteAddressBuffer      Args   : register(u0); // D3D12_DRAW_INDEXED_ARGUMENTS[view*numGroups + group]
RWStructuredBuffer<uint> Counts : register(u1); // per-view ExecuteIndirect command count

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
    uint2 pg = PerGroup[g];

    uint base = idx * kArgStride;
    Args.Store(base + 0u,  pg.y);                        // IndexCountPerInstance = mesh index count
    Args.Store(base + 4u,  0u);                          // InstanceCount (bumped by the cull)
    Args.Store(base + 8u,  0u);                          // StartIndexLocation
    Args.Store(base + 12u, 0u);                          // BaseVertexLocation
    Args.Store(base + 16u, v * gNumCasters + pg.x);      // StartInstanceLocation -> visible-list slice

    if (g == 0u) { Counts[v] = gNumGroups; }
}
