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

#define VSM_MAX_SETUP_GROUPS 64 // matches kMaxMegaGroups in ShadowGpuData::Rebuild

cbuffer SetupCB : register(b0)
{
    uint gNumGroups;    // ShadowGpuData mesh-group count
    uint gArgBaseElems; // frame region base into Rung0Args, in 5-uint arg units (f*numViewsRung0*numGroups)
    uint gNumPages;     // kPoolPageCount
    uint _pad;
    float4x4 gViewProj[VSM_MAX_VIEWS];    // per VSM local view (spots then point faces)
    uint4    gGroupMega[VSM_MAX_SETUP_GROUPS]; // per mesh-group mega-buffer offset: x=baseVertex, y=startIndex (0 when the mega path is off)
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

    // Step 24d (add-dormant): directional clipmap pages (view >= VSM_NUM_LOCAL_VIEWS) have no Rung-0
    // cull yet (24e wires it). Emit zero-instance args so they draw nothing, and skip the
    // rung0View = view + 4 arg copy below (which would read out of bounds for a clipmap view).
    if (view >= VSM_NUM_LOCAL_VIEWS)
    {
        for (uint gc = 0u; gc < gNumGroups; ++gc)
        {
            uint offc = (p * gNumGroups + gc) * 20u;
            PageDrawArgs.Store4(offc, uint4(0u, 0u, 0u, 0u));
            PageDrawArgs.Store(offc + 16u, 0u);
        }
        return;
    }

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
        // Rebase into the consolidated mega VB/IB (offsets are 0 when the mega path is off, so the
        // args stay per-mesh for the per-group fallback binding). z = StartIndexLocation, w = BaseVertexLocation.
        a0.z += gGroupMega[g].y;
        a0.w += gGroupMega[g].x;
        uint dst = (p * gNumGroups + g) * 20u;
        PageDrawArgs.Store4(dst, a0);
        PageDrawArgs.Store(dst + 16u, a4);
    }
}
