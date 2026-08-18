#define HZB_BUILD_CS_RS "CBV(b0), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

// P6C -- one level of the hierarchical depth pyramid.
//
// THE REDUCTION IS `min` OF DEVICE Z, and under this engine's REVERSED-Z projection that is the
// FURTHEST surface in the tile, not the nearest. Unreal build two pyramids for exactly this reason
// (HZB.usf: `FurthestHZBOutput` / `ClosestHZBOutput`) and hand different ones to different consumers:
//
//   * a HORIZON SEARCH (GTAO) takes the FURTHEST. Reading a coarse tile then UNDER-estimates how
//     much of the sky that tile blocks, so a low mip cannot invent contact shadows out of geometry
//     too small to matter at its scale. `GetHZBParametersForAO` asks for `EHZBType::FurthestHZB`.
//   * a RAY MARCH (SSR, and P9's screen-space GI) takes the CLOSEST, so a long step cannot tunnel
//     through a surface the tile really does contain. `HZBTracing.ush` binds `ClosestHZBTexture`.
//
// BOTH pyramids are built here, by one dispatch per level, because the source texels are the same
// four loads for either reduction -- splitting this into two passes would double the traffic to
// halve the ALU. Do not "simplify" them into a single shared pyramid: the two consumers want
// opposite answers, and either answer is WRONG for the other one.
//
// WHY THE WHOLE CHAIN LIVES IN UNORDERED_ACCESS: this engine's barrier layer transitions whole
// resources (ALL_SUBRESOURCES), so it cannot put mip N-1 in SHADER_RESOURCE while mip N is a UAV.
// Reading the source mip through its own UAV sidesteps that entirely and costs nothing -- a
// RWTexture2D is readable. Successive levels are separated by UAV barriers, not transitions.
//
// t0: scene depth at RENDER resolution (only used when building mip 0)
// u0: source mip of the FURTHEST chain (mip N-1). Unused for mip 0.
// u1: destination mip of the FURTHEST chain (mip N)
// u2/u3: the same pair for the CLOSEST chain. Written only when `writeClosest`.

RWTexture2D<float> SrcMip : register(u0);
RWTexture2D<float> DstMip : register(u1);
RWTexture2D<float> SrcMipClosest : register(u2);
RWTexture2D<float> DstMipClosest : register(u3);
Texture2D DepthTex : register(t0);
SamplerState gSmpPoint : register(s0);

cbuffer HzbCB : register(b0)
{
    uint2 dstSize;    // destination mip dimensions, texels
    uint2 srcSize;    // source dimensions: the depth buffer for mip 0, else mip N-1
    uint  fromDepth;  // 1 = reduce the render-res depth buffer into mip 0
    // P6C step 6: 0 = skip the closest chain entirely. Nothing reads it unless a screen-space
    // march is the active reflection source, and a pyramid nobody reads is pure bandwidth.
    uint  writeClosest;
    uint  pad1;
    uint  pad2;
};

// Mip 0 reduces the shared depth buffer, so both chains start from the same four values and this
// returns one number. From mip 1 on the chains have diverged and each reads its OWN source.
float LoadSource(int2 p)
{
    const int2 c = clamp(p, int2(0, 0), int2(srcSize) - int2(1, 1));
    return (fromDepth != 0u) ? DepthTex.Load(int3(c, 0)).r : SrcMip[c];
}

float LoadSourceClosest(int2 p)
{
    const int2 c = clamp(p, int2(0, 0), int2(srcSize) - int2(1, 1));
    return (fromDepth != 0u) ? DepthTex.Load(int3(c, 0)).r : SrcMipClosest[c];
}

[numthreads(8, 8, 1)]
[RootSignature(HZB_BUILD_CS_RS)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= dstSize.x || tid.y >= dstSize.y)
    {
        return;
    }

    const int2 dst = int2(tid.xy);
    const int2 src = dst * 2;

    float z = min(min(LoadSource(src + int2(0, 0)), LoadSource(src + int2(1, 0))),
                  min(LoadSource(src + int2(0, 1)), LoadSource(src + int2(1, 1))));

    // ODD SOURCE DIMENSIONS. Halving an odd extent drops a row or column, and the texels in it
    // would never reach any mip -- a silent hole at the frame edge that only appears at particular
    // resolutions. Fold the leftover neighbours in when this texel is the last one along an axis.
    const bool oddX = (srcSize.x & 1u) != 0u && (uint)dst.x == dstSize.x - 1u;
    const bool oddY = (srcSize.y & 1u) != 0u && (uint)dst.y == dstSize.y - 1u;
    if (oddX)
    {
        z = min(z, min(LoadSource(src + int2(2, 0)), LoadSource(src + int2(2, 1))));
    }
    if (oddY)
    {
        z = min(z, min(LoadSource(src + int2(0, 2)), LoadSource(src + int2(1, 2))));
    }
    if (oddX && oddY)
    {
        z = min(z, LoadSource(src + int2(2, 2)));
    }

    DstMip[dst] = z;

    if (writeClosest != 0u)
    {
        float zc = max(max(LoadSourceClosest(src + int2(0, 0)), LoadSourceClosest(src + int2(1, 0))),
                       max(LoadSourceClosest(src + int2(0, 1)), LoadSourceClosest(src + int2(1, 1))));
        if (oddX)
        {
            zc = max(zc, max(LoadSourceClosest(src + int2(2, 0)), LoadSourceClosest(src + int2(2, 1))));
        }
        if (oddY)
        {
            zc = max(zc, max(LoadSourceClosest(src + int2(0, 2)), LoadSourceClosest(src + int2(1, 2))));
        }
        if (oddX && oddY)
        {
            zc = max(zc, LoadSourceClosest(src + int2(2, 2)));
        }

        DstMipClosest[dst] = zc;
    }
}
