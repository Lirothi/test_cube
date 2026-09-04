// Occlusion plan S5b.2: the light-space depth pyramid of the VSM physical pool, ONE thread group
// per physical page (Unreal's BuildHZBPerPageCS, VirtualShadowMapPhysicalPageManagement.usf). The
// pyramid is the pool at half resolution with 7 levels (2048 -> 32 texels), so every 128-texel
// page owns a self-contained 64 -> 1 chain at its own offset: a page's HZB region never mixes
// with a neighbour's, which is what lets the per-(caster, page) test of vsm_page_scatter_cs /
// vsm_hzb_post_cs treat the page as a tiny view.
//
// Stores 1 - z (the pool is forward-Z, cleared to 1.0) with a MIN reduction, so "furthest" is the
// minimum, exactly as the camera pyramid and the S5b.1 cascade pyramids: the hzb_cull.hlsli test
// is untouched, the caller flips the projection's z instead (vp * FlipZ). A cleared texel is 0 =
// furthest = hides nothing.
//
// Which pages: only those RENDERED this frame (PerPageDirty, after pass A) -- a cached page's
// region still describes its cached depth from the frame it was last drawn. gFull = 1 rebuilds
// every resident page and zeroes the free ones (the first build, and after a level switch).
#include "vsm_addressing.hlsli"

#define VSM_HZB_BUILD_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=7, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

static const uint VSM_INVALID = 0xFFFFFFFFu; // "no owner" sentinel (vsm_page_alloc_common.hlsli)

cbuffer BuildCB : register(b0)
{
    uint gFull;      // 1 = every resident page, free pages zeroed; 0 = dirty pages only
    uint _pad0, _pad1, _pad2;
};

Texture2D<float>       Pool         : register(t0); // the physical pool, D32 read as R32_FLOAT
StructuredBuffer<uint> PhysOwner    : register(t1); // physical page -> owner / VSM_INVALID
StructuredBuffer<uint> PerPageDirty : register(t2); // per physical page: rendered this frame

RWTexture2D<float> Mip0 : register(u0); // 2048^2, 64 per page
RWTexture2D<float> Mip1 : register(u1); // 1024^2, 32
RWTexture2D<float> Mip2 : register(u2); //  512^2, 16
RWTexture2D<float> Mip3 : register(u3); //  256^2,  8
RWTexture2D<float> Mip4 : register(u4); //  128^2,  4
RWTexture2D<float> Mip5 : register(u5); //   64^2,  2
RWTexture2D<float> Mip6 : register(u6); //   32^2,  1

groupshared float gs2[16][16];
groupshared float gs3[8][8];
groupshared float gs4[4][4];
groupshared float gs5[2][2];

// 16x16 threads per page: each thread reduces an 8x8 source block into a 4x4 block of mip 0, a
// 2x2 of mip 1 and one texel of mip 2; the last four levels fold through groupshared memory.
[numthreads(16, 16, 1)]
[RootSignature(VSM_HZB_BUILD_RS)]
void CSMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const uint page = gid.y * VSM_POOL_PAGES_AXIS + gid.x;
    const bool resident = PhysOwner[page] != VSM_INVALID;
    // Uniform per group, so the barriers below are reached by every thread or by none.
    if (gFull == 0u && (!resident || PerPageDirty[page] == 0u)) { return; }

    const uint2 t = gtid.xy;
    const uint2 pageOrigin = gid.xy * VSM_PAGE_SIZE;         // pool texels
    const uint2 m0Origin = gid.xy * (VSM_PAGE_SIZE / 2u);    // 64 per page
    const uint2 m1Origin = gid.xy * (VSM_PAGE_SIZE / 4u);    // 32
    const uint2 m2Origin = gid.xy * (VSM_PAGE_SIZE / 8u);    // 16
    const uint2 m3Origin = gid.xy * (VSM_PAGE_SIZE / 16u);   // 8
    const uint2 m4Origin = gid.xy * (VSM_PAGE_SIZE / 32u);   // 4
    const uint2 m5Origin = gid.xy * (VSM_PAGE_SIZE / 64u);   // 2

    float m1v[2][2];
    [unroll] for (uint j = 0u; j < 2u; ++j)
    {
        [unroll] for (uint i = 0u; i < 2u; ++i)
        {
            float mn = 1.0f;
            [unroll] for (uint jj = 0u; jj < 2u; ++jj)
            {
                [unroll] for (uint ii = 0u; ii < 2u; ++ii)
                {
                    const uint2 c0 = uint2(4u * t.x + 2u * i + ii, 4u * t.y + 2u * j + jj); // page-local mip 0
                    const int3 s = int3(pageOrigin + c0 * 2u, 0);
                    // 1 - max(z) over the 2x2 source: the furthest surface of the four.
                    const float z = max(max(Pool.Load(s), Pool.Load(s + int3(1, 0, 0))),
                                        max(Pool.Load(s + int3(0, 1, 0)), Pool.Load(s + int3(1, 1, 0))));
                    const float v = resident ? (1.0f - z) : 0.0f;
                    Mip0[m0Origin + c0] = v;
                    mn = min(mn, v);
                }
            }
            Mip1[m1Origin + uint2(2u * t.x + i, 2u * t.y + j)] = mn;
            m1v[j][i] = mn;
        }
    }
    const float v2 = min(min(m1v[0][0], m1v[0][1]), min(m1v[1][0], m1v[1][1]));
    Mip2[m2Origin + t] = v2;
    gs2[t.y][t.x] = v2;
    GroupMemoryBarrierWithGroupSync();

    if (t.x < 8u && t.y < 8u)
    {
        const float v3 = min(min(gs2[2u * t.y][2u * t.x], gs2[2u * t.y][2u * t.x + 1u]),
                             min(gs2[2u * t.y + 1u][2u * t.x], gs2[2u * t.y + 1u][2u * t.x + 1u]));
        Mip3[m3Origin + t] = v3;
        gs3[t.y][t.x] = v3;
    }
    GroupMemoryBarrierWithGroupSync();

    if (t.x < 4u && t.y < 4u)
    {
        const float v4 = min(min(gs3[2u * t.y][2u * t.x], gs3[2u * t.y][2u * t.x + 1u]),
                             min(gs3[2u * t.y + 1u][2u * t.x], gs3[2u * t.y + 1u][2u * t.x + 1u]));
        Mip4[m4Origin + t] = v4;
        gs4[t.y][t.x] = v4;
    }
    GroupMemoryBarrierWithGroupSync();

    if (t.x < 2u && t.y < 2u)
    {
        const float v5 = min(min(gs4[2u * t.y][2u * t.x], gs4[2u * t.y][2u * t.x + 1u]),
                             min(gs4[2u * t.y + 1u][2u * t.x], gs4[2u * t.y + 1u][2u * t.x + 1u]));
        Mip5[m5Origin + t] = v5;
        gs5[t.y][t.x] = v5;
    }
    GroupMemoryBarrierWithGroupSync();

    if (t.x == 0u && t.y == 0u)
    {
        Mip6[gid.xy] = min(min(gs5[0][0], gs5[0][1]), min(gs5[1][0], gs5[1][1]));
    }
}
