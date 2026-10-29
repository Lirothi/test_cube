// Tessy N6 (docs/tessy_plan.md): the cluster draw's DispatchMesh arguments from the cull's count. One group, after
// tessy_vsm_cull_cs and before the render's consume point (the stats buffer turns into indirect arguments there).
// The count is the cull's InstanceCount ([4] of TessyStats, which may run past TessyVisible's capacity -- clamped);
// a dimension takes at most 65535 groups, so N clusters are (min(N, 65535), ceil(N / 65535), 1) groups, and the last
// row's groups past N read records this pass marks dead (only when N > 65535; the VS path never reads past N).
#pragma pack_matrix(row_major)
#include "tessy_common.hlsli"

#define TESSY_MS_ARGS_RS \
    "DescriptorTable(UAV(u0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

// Byte layout: VirtualShadowMap::TessyStats ([80..91] = D3D12_DISPATCH_MESH_ARGUMENTS).
RWByteAddressBuffer       TessyStats   : register(u0);
RWStructuredBuffer<uint4> TessyVisible : register(u1);

[numthreads(64, 1, 1)]
[RootSignature(TESSY_MS_ARGS_RS)]
void CSMain(uint lane : SV_GroupIndex)
{
    uint cap, stride;
    TessyVisible.GetDimensions(cap, stride);
    const uint count = min(TessyStats.Load(4u), cap);
    const uint x = min(count, kTessyMsRow);
    const uint y = (count + kTessyMsRow - 1u) / kTessyMsRow;
    if (lane == 0u) { TessyStats.Store3(80u, uint3(x, y, 1u)); }
    const uint end = min(x * y, cap);
    for (uint i = count + lane; i < end; i += 64u)
    {
        TessyVisible[i] = uint4(kTessyDeadRecord, 0u, 0u, 0u);
    }
}
