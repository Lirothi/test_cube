// Texture streaming plan A6: expands the camera cull's per-virtual-group DRAW_INDEXED arguments
// (20 bytes, InstanceCount bumped by shadow_cull_cs / cam_cull_post_cs) into the 72-byte commands
// of the bindless G-buffer command signature {CONSTANT(b0), VBV, IBV, CBV(b2), DRAW_INDEXED}, so
// every group of one PSO draws with ONE ExecuteIndirect. The static part of each command -- the
// group's vertex/index buffer views, its material's SurfaceParams address and its LOD -- is
// written by the CPU per frame (ShadowGpuData::FillGBufferCommandsForFrame) in PSO-sorted order;
// this kernel only pairs it with the cull's draw counts. One thread per command.
//
// b0: ExpandParams   t0: Static (3 x uint4 per command)   u0: CamArgs (read)   u1: Cmds (written)
#define GBUFFER_INDIRECT_EXPAND_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer ExpandParams : register(b0)
{
    uint gCmdCount;
    uint3 gExpandPad;
};

// Per command: {vbVA.lo, vbVA.hi, vbSize, vbStride} {ibVA.lo, ibVA.hi, ibSize, ibFormat}
//              {cbVA.lo, cbVA.hi, groupLod, sourceVirtualGroup}
StructuredBuffer<uint4>  Static  : register(t0);
RWByteAddressBuffer      CamArgs : register(u0); // D3D12_DRAW_INDEXED_ARGUMENTS[virtual group]
RWByteAddressBuffer      Cmds    : register(u1); // 72-byte commands, PSO-sorted

static const uint kArgStride = 20u; // D3D12_DRAW_INDEXED_ARGUMENTS
static const uint kCmdStride = 72u; // CONSTANT 8 | VBV 16 | IBV 16 | CBV 8 | DRAW_INDEXED 20 | pad 4 (Renderer::kBindlessIndirectCommandBytes)

[numthreads(8, 8, 1)]
[RootSignature(GBUFFER_INDIRECT_EXPAND_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.y != 0u) { return; }
    const uint k = dtid.x;
    if (k >= gCmdCount) { return; }

    const uint4 s0 = Static[k * 3u + 0u];
    const uint4 s1 = Static[k * 3u + 1u];
    const uint4 s2 = Static[k * 3u + 2u];
    const uint vg = s2.w;
    const uint base = k * kCmdStride;
    const uint arg = vg * kArgStride;

    Cmds.Store2(base + 0u, uint2(s2.z, 0u));                      // root constants b0: LOD (+ pad)
    Cmds.Store4(base + 8u, s0);                                   // D3D12_VERTEX_BUFFER_VIEW (slot 0)
    Cmds.Store4(base + 24u, s1);                                  // D3D12_INDEX_BUFFER_VIEW
    Cmds.Store2(base + 40u, uint2(s2.x, s2.y));                   // CBV(b2) address
    Cmds.Store4(base + 48u, CamArgs.Load4(arg));                  // IndexCount, InstanceCount, StartIndex, BaseVertex
    Cmds.Store(base + 64u, CamArgs.Load(arg + 16u));              // StartInstanceLocation
    Cmds.Store(base + 68u, 0u);                                   // pad
}
