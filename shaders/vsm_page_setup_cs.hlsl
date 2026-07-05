// Rung 2 / Step 22 setup: one thread per PHYSICAL pool page. For a resident page, decode the
// virtual page it holds -> (view, mip level, px, py), build an off-center projection that maps
// that page's virtual sub-rect to full NDC (so a 128² viewport renders exactly that sub-rect),
// and copy the page's VIEW's Rung-0 cull args (InstanceCount + StartInstanceLocation into the
// shared visible list) into a per-page indirect-args slot. Free pages get zero InstanceCount.
// The CPU then loops physical pages, setting each page's pool-cell viewport + this projection, and
// ExecuteIndirect draws the casters (reusing Rung 0's instance buffer + visible list + indirect VS).
#pragma pack_matrix(row_major)
#include "vsm_addressing.hlsli"

static const uint VSM_INVALID = 0xFFFFFFFFu; // "no owner" sentinel (matches vsm_page_alloc_common.hlsli)

#define VSM_PAGE_SETUP_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer SetupCB : register(b0)
{
    uint gNumGroups;    // ShadowGpuData mesh-group count
    uint gArgBaseElems; // frame region base into Rung0Args, in 5-uint arg units (f*numViewsRung0*numGroups)
    uint gNumPages;     // kPoolPageCount
    uint _pad;
    float4x4 gViewProj[VSM_MAX_VIEWS]; // per VSM local view (spots then point faces)
};

StructuredBuffer<uint> PhysOwner    : register(t0); // physical page -> virtual owner / INVALID
ByteAddressBuffer      Rung0Args    : register(t1); // D3D12_DRAW_INDEXED_ARGUMENTS[view*group] region f
RWByteAddressBuffer    PageDrawArgs : register(u0); // per (page, group) D3D12_DRAW_INDEXED_ARGUMENTS
RWByteAddressBuffer    PageProj     : register(u1); // per page off-center viewProj (256-byte stride for root-CBV)

[numthreads(8, 8, 1)]
[RootSignature(VSM_PAGE_SETUP_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.y != 0u) { return; }
    const uint p = dtid.x;
    if (p >= gNumPages) { return; }

    const uint owner = PhysOwner[p];
    if (owner == VSM_INVALID)
    {
        // Free page: emit zero-instance args for every group (drawn as a no-op). pageProj is
        // left stale (unread — the draw is a no-op); the setup runs every frame so a page that
        // becomes resident is fully rewritten before it is drawn.
        for (uint g = 0u; g < gNumGroups; ++g)
        {
            uint off = (p * gNumGroups + g) * 20u;
            PageDrawArgs.Store4(off, uint4(0u, 0u, 0u, 0u));
            PageDrawArgs.Store(off + 16u, 0u);
        }
        return;
    }

    uint view, level, px, py;
    VsmDecodePage(owner, view, level, px, py);

    // Off-center projection: the page (px,py) at this level covers the NDC sub-rect centered at
    // (cx,cy) with half-size 1/axis; scale/bias clip so that sub-rect fills [-1,1] (z preserved,
    // so the stored depth matches the light-space depth the sampler compares against).
    const float a = (float)(VSM_L0_AXIS >> level);
    const float cx = -1.0f + (2.0f * px + 1.0f) / a;
    const float cy =  1.0f - (2.0f * py + 1.0f) / a;
    const float4x4 S = float4x4(a, 0, 0, 0,
                                0, a, 0, 0,
                                0, 0, 1, 0,
                                -cx * a, -cy * a, 0, 1);
    const float4x4 pm = mul(gViewProj[view], S);
    uint po = p * 256u; // 256-byte stride (root-CBV alignment)
    PageProj.Store4(po +  0u, asuint(pm[0]));
    PageProj.Store4(po + 16u, asuint(pm[1]));
    PageProj.Store4(po + 32u, asuint(pm[2]));
    PageProj.Store4(po + 48u, asuint(pm[3]));

    // Copy this page's VIEW's Rung-0 cull args (VSM local view v -> Rung-0 slot v+4, since VSM
    // drops the 4 CSM cascades). The StartInstanceLocation is a global offset into region f's
    // visible list, which the CPU binds once as the per-instance stream.
    const uint rung0View = view + 4u;
    for (uint g = 0u; g < gNumGroups; ++g)
    {
        uint src = (gArgBaseElems + rung0View * gNumGroups + g) * 20u;
        uint4 a0 = Rung0Args.Load4(src);      // IndexCountPerInstance, InstanceCount, StartIndex, BaseVertex
        uint  a4 = Rung0Args.Load(src + 16u); // StartInstanceLocation
        uint dst = (p * gNumGroups + g) * 20u;
        PageDrawArgs.Store4(dst, a0);
        PageDrawArgs.Store(dst + 16u, a4);
    }
}
