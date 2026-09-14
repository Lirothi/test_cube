// S16 (docs/csm_improvement_plan.md): separable box blur of the EVSM moments, in LIGHT SPACE.
//
// Transcribed from the Intel SDSM DX11 sample's BoxBlur.hlsl + BlurUtil.hlsl (`ComputeBlurData`,
// `BoxBlurPS`), Intel Sample Source Code License -- permissive with notice.
//
// THIS IS THE POINT OF MOMENTS. A percentage-closer filter cannot be pre-blurred: its taps are
// binary compares of THIS receiver's depth, so the only way to soften it is more taps per pixel,
// every pixel, every frame. Moments are a distribution, so blurring them once per frame softens
// every subsequent lookup for free -- and the softness is an authored WORLD SIZE (a fraction of
// the partition), not a texel count that changes meaning between partitions.
//
// OUR DEVIATIONS:
//  * compute, not a full-screen PS per partition. The sample's VS computes the blur weights once
//    per triangle and hands them down as nointerpolation; here the same arithmetic runs per
//    thread, which is a few ALU against a dispatch that is bandwidth-bound anyway.
//  * the filter width is a fraction of the partition's CONTENT rect directly. Theirs is a
//    normalized light-space size scaled by `partition.scale.xy` and then by the texture
//    dimensions -- the same number, minus a global light projection we do not have.
//  * reads are CLAMPED TO THE TILE. The sample blur a texture array, one partition per slice, so
//    a tap can never leave its partition. Our four partitions share one atlas, and a 16-texel
//    kernel on a 4-texel gutter reaches past it into the NEIGHBOUR -- another partition's depth,
//    which is the exact failure the gutter exists to prevent for the sampler.
#pragma pack_matrix(row_major)
#define SDSM_PARTITIONS_STRUCTS_ONLY 1
#include "sdsm_partitions.hlsli"

// BOTH SIDES ARE UAVs, and that is not laziness. The horizontal pass reads the moments atlas and
// the vertical pass writes it, so within this one barrier point the atlas is both source and
// destination. A texture cannot be an SRV and a UAV at the same time under enhanced barriers --
// binding it as an SRV while its layout is UNORDERED_ACCESS is GBV id=1358 (caught 2026-09-14),
// and the alternative, flipping its layout between every pair of dispatches, is four extra
// barriers per frame to avoid one typed load.
#define SDSM_EVSM_BLUR_RS \
    "CBV(b0), " \
    "DescriptorTable(UAV(u0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer SdsmBlurCB : register(b0)
{
    int2  gSrcOrigin;    // where the tile starts in the SOURCE texture
    int2  gDstOrigin;    // ...and in the destination
    int2  gTileSize;     // the tile edge (both are square)
    uint  gDimension;    // 0 = horizontal, 1 = vertical
    float gFilterTexels; // box width in texels, >= 1
};

RWTexture2D<float4> Src : register(u0);
RWTexture2D<float4> Dst : register(u1);

[numthreads(8, 8, 1)]
[RootSignature(SDSM_EVSM_BLUR_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if ((int)dtid.x >= gTileSize.x || (int)dtid.y >= gTileSize.y) { return; }

    // `ComputeBlurData`, verbatim: a box of FRACTIONAL width is whole interior samples at equal
    // weight plus one partial sample at each end. Without the fractional end the filter would
    // quantise to odd texel counts and the softness would step as the partition moves.
    const float filterSize = max(gFilterTexels, 1.0f);
    const float sideSamples = 0.5f * filterSize - 0.5f;
    float interiorCount;
    const float edgeFrac = modf(sideSamples, interiorCount);
    const float interiorWeight = 1.0f / filterSize;
    const float edgeWeight = edgeFrac * interiorWeight;

    const int2 step = (gDimension == 0u) ? int2(1, 0) : int2(0, 1);
    const int2 local = int2(dtid.xy);

    // Clamped to the TILE, never to the texture: a tap that leaves this tile lands in another
    // partition's moments.
    const int2 lo = int2(0, 0);
    const int2 hi = gTileSize - int2(1, 1);
    float4 sum = interiorWeight * Src[uint2(gSrcOrigin + clamp(local, lo, hi))];

    const int loops = (int)interiorCount;
    for (int i = 1; i <= loops; ++i)
    {
        sum += interiorWeight * Src[uint2(gSrcOrigin + clamp(local - i * step, lo, hi))];
        sum += interiorWeight * Src[uint2(gSrcOrigin + clamp(local + i * step, lo, hi))];
    }
    const int2 edge = (loops + 1) * step;
    sum += edgeWeight * Src[uint2(gSrcOrigin + clamp(local - edge, lo, hi))];
    sum += edgeWeight * Src[uint2(gSrcOrigin + clamp(local + edge, lo, hi))];

    Dst[uint2(gDstOrigin + local)] = sum;
}
