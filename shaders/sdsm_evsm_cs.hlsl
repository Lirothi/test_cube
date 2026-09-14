// S16 (docs/csm_improvement_plan.md): the SDSM depth atlas -> EVSM4 MOMENTS.
//
// Transcribed from the Intel SDSM DX11 sample's RenderingEVSM.hlsl (`WarpDepth`,
// `ShadowDepthToEVSMPS`), Intel Sample Source Code License -- permissive with notice.
//
// WHY MOMENTS AT ALL. A percentage-closer filter can only average BINARY compares, each taken
// before the filter runs, so its only lever is more taps. Exponentially warped moments are a
// PREFILTERABLE representation of the same occluder distribution: hardware bilinear, a separable
// box blur and a mip chain all operate on the shadow ESTIMATE. That is what lets a smaller tile
// look better than a larger PCF one, which is the whole S16 bet.
//
// OUR DEVIATIONS (see render::sdsm::g_evsmPos for the exponent one):
//  * the sample convert a MULTISAMPLED depth tile and average the warps of the individual samples
//    ("transparency AA" for shadows). We convert the single-sampled D16 atlas the S15 depth pass
//    already produced -- shadow-MSAA is a later sub-step, and it is a separate render target, not
//    a change to this conversion;
//  * they run this as a full-screen PS per partition; ours is one compute dispatch over the whole
//    atlas, with the partition derived from the texel's tile. One dispatch instead of four, and
//    the tile layout is the same 2x2 the rest of the mode uses.
#pragma pack_matrix(row_major)
#define SDSM_PARTITIONS_STRUCTS_ONLY 1
#include "sdsm_partitions.hlsli"

#define SDSM_EVSM_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer SdsmEvsmCB : register(b0)
{
    uint  gAtlasRes;      // atlas edge in texels
    uint  gTileRes;       // half of it
    uint  gPartitionCount;
    uint  gEvsmPad;
};

Texture2D                       DepthAtlas : register(t0); // R16_UNORM view of the CSM atlas
StructuredBuffer<SdsmPartition> Partitions : register(t1);
RWTexture2D<float4>             Moments    : register(u0);

[numthreads(8, 8, 1)]
[RootSignature(SDSM_EVSM_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gAtlasRes || dtid.y >= gAtlasRes) { return; }

    // Which partition owns this texel: the same 2x2 grid Scene and BindShadowTarget use.
    const uint tx = (gTileRes > 0u) ? (dtid.x / gTileRes) : 0u;
    const uint ty = (gTileRes > 0u) ? (dtid.y / gTileRes) : 0u;
    const uint part = min(ty * 2u + tx, (uint)SDSM_MAX_PARTITIONS - 1u);

    // The atlas is FORWARD-Z (LESS_EQUAL, clear 1.0), so an untouched texel -- including the whole
    // S5 gutter -- reads 1.0 = far = lit, and its warp is the "nothing occludes here" moment. That
    // is exactly what the gutter is supposed to say, so it needs no special case.
    const float depth = DepthAtlas.Load(int3(int2(dtid.xy), 0)).r;

    const float2 exponents = SdsmEvsmExponents(Partitions[part]);
    const float2 warped = SdsmWarpDepth(depth, exponents);
    Moments[dtid.xy] = float4(warped, warped * warped);
}
