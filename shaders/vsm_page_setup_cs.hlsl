// Rung 2 per-page instance cull: one thread per PHYSICAL pool page. For a resident page, decode the
// virtual page it holds -> (view, mip level, px, py), build the off-center projection that maps that
// page's virtual sub-rect to full NDC, extract the page's 6 frustum planes, and cull the WHOLE caster
// set to THIS page (positive-vertex AABB test, mirroring shadow_cull_cs). Each visible caster id is
// appended to the page's slice of PageVisibleList, grouped by mesh-group; the per (page, group) draw
// args get the per-page InstanceCount + a StartInstanceLocation into that slice. This replaces the old
// "copy the whole VIEW's args to every page" (which redrew every view-visible caster into every page
// and leaned on the 128 scissor) -> the draw only submits instances that actually land in the page.
// The CPU loops physical pages, sets each page's pool-cell viewport + this projection, binds
// PageVisibleList as the per-instance stream, and ExecuteIndirect draws the page's own casters.
#pragma pack_matrix(row_major)
#include "vsm_addressing.hlsli"

static const uint VSM_INVALID = 0xFFFFFFFFu; // "no owner" sentinel (matches vsm_page_alloc_common.hlsli)

#define VSM_PAGE_SETUP_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=6, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

#define VSM_MAX_SETUP_GROUPS 64 // matches kMaxMegaGroups in ShadowGpuData::Rebuild

cbuffer SetupCB : register(b0)
{
    uint gNumGroups;    // ShadowGpuData mesh-group count
    uint gArgBaseElems; // frame region base into Rung0Args, in 5-uint arg units (f*numViewsRung0*numGroups)
    uint gNumPages;     // kPoolPageCount
    uint gNumCasters;   // active caster count (ShadowGpuData::ActiveCasterCount) + per-page slice stride
    uint gForceAll;     // page cache: 1 = mark every resident page dirty (static caster moved / rebuild)
    uint _pad0, _pad1, _pad2;
    float4x4 gViewProj[VSM_MAX_VIEWS];    // per VSM local view (spots then point faces)
    uint4    gGroupMega[VSM_MAX_SETUP_GROUPS]; // per mesh-group mega-buffer offset: x=baseVertex, y=startIndex (0 when the mega path is off)
};

struct CasterBounds { float4 center; float4 halfExtents; }; // xyz world center/half-extents (matches render::CasterBounds)

StructuredBuffer<uint>         PhysOwner     : register(t0); // physical page -> virtual owner / INVALID
ByteAddressBuffer              Rung0Args     : register(t1); // D3D12_DRAW_INDEXED_ARGUMENTS[view*group] region f (per-group index count / offsets)
StructuredBuffer<CasterBounds> Bounds        : register(t2); // per-caster world AABB (the unified buffer)
StructuredBuffer<uint>         CasterGroup   : register(t3); // per-caster mesh-group id
StructuredBuffer<uint>         PhysOwnerPrev : register(t4); // physical page -> last frame's owner (new-page detect)
StructuredBuffer<uint>         CasterDynamic : register(t5); // per-caster dynamic flag (1 = animating)

RWByteAddressBuffer      PageDrawArgs    : register(u0); // per (page, group) D3D12_DRAW_INDEXED_ARGUMENTS
RWByteAddressBuffer      PageProj        : register(u1); // per page off-center viewProj (256-byte stride for root-CBV)
RWStructuredBuffer<uint> PageVisibleList : register(u2); // per (page, slot) visible caster id
RWStructuredBuffer<uint> PerPageDirty    : register(u3); // per page: 1 = re-render this frame, 0 = cached

// Positive-vertex AABB-vs-frustum test (mirrors shadow_cull_cs::Intersects). The planes are
// unnormalized (extracted straight from the matrix) — the test is scale-invariant per plane.
bool PageIntersects(float4 planes[6], float3 c, float3 e)
{
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        float4 pl = planes[i];
        float signedDist = dot(pl.xyz, c) + pl.w;
        float projRadius = dot(abs(pl.xyz), e);
        if (signedDist + projRadius < 0.0f) { return false; }
    }
    return true;
}

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
        // Free page: zero-instance args for every group (drawn as a no-op) + not dirty (the clear
        // skips it). pageProj left stale (unread). A page that becomes resident is fully rewritten.
        PerPageDirty[p] = 0u;
        for (uint g = 0u; g < gNumGroups; ++g)
        {
            uint off = (p * gNumGroups + g) * 20u;
            PageDrawArgs.Store4(off, uint4(0u, 0u, 0u, 0u));
            PageDrawArgs.Store(off + 16u, 0u);
        }
        return;
    }

    // Page cache: a resident page needs re-render iff it was just allocated / re-owned (isNew), a
    // dynamic caster overlaps it (found in pass 1 below), or a static caster moved this frame
    // (gForceAll). Otherwise its depth is still valid from a previous frame -> skip clear + draw.
    const bool isNew = (owner != PhysOwnerPrev[p]);

    uint view, level, px, py;
    VsmDecodePage(owner, view, level, px, py);

    // Off-center projection: the page (px,py) at this level covers the NDC sub-rect centered at
    // (cx,cy) with half-size 1/axis; scale/bias clip so that sub-rect fills [-1,1] (z preserved).
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

    // The page's 6 frustum planes from pm (columns; clip = mul(world, pm)). Positive-vertex test is
    // scale-invariant, so unnormalized planes are fine. Near/far match the view (S preserves z).
    const float4x4 pt = transpose(pm); // pt[k] = column k of pm
    float4 planes[6];
    planes[0] = pt[3] + pt[0]; // left
    planes[1] = pt[3] - pt[0]; // right
    planes[2] = pt[3] + pt[1]; // bottom
    planes[3] = pt[3] - pt[1]; // top
    planes[4] = pt[2];         // near (z >= 0)
    planes[5] = pt[3] - pt[2]; // far

    // Pass 1: count the casters visible in THIS page, per mesh-group, and note whether any DYNAMIC
    // caster overlaps (page-cache invalidation).
    uint perGroupCount[VSM_MAX_SETUP_GROUPS];
    for (uint gi = 0u; gi < gNumGroups; ++gi) { perGroupCount[gi] = 0u; }
    bool dynamicOverlap = false;
    for (uint c = 0u; c < gNumCasters; ++c)
    {
        CasterBounds b = Bounds[c];
        if (PageIntersects(planes, b.center.xyz, b.halfExtents.xyz))
        {
            uint g = CasterGroup[c];
            if (g < gNumGroups) { perGroupCount[g] += 1u; }
            if (CasterDynamic[c] != 0u) { dynamicOverlap = true; }
        }
    }

    // Cache decision: dirty pages clear + redraw; clean pages keep their cached depth (draw nothing).
    const bool dirty = isNew || dynamicOverlap || (gForceAll != 0u);
    PerPageDirty[p] = dirty ? 1u : 0u;
    if (!dirty)
    {
        for (uint gc = 0u; gc < gNumGroups; ++gc)
        {
            uint off = (p * gNumGroups + gc) * 20u;
            PageDrawArgs.Store4(off, uint4(0u, 0u, 0u, 0u)); // InstanceCount 0 -> caster draw is a no-op
            PageDrawArgs.Store(off + 16u, 0u);
        }
        return;
    }

    // Prefix-sum -> each group's base within this page's [p*gNumCasters, ...) list slice.
    uint perGroupBase[VSM_MAX_SETUP_GROUPS];
    uint acc = 0u;
    for (uint gp = 0u; gp < gNumGroups; ++gp) { perGroupBase[gp] = acc; acc += perGroupCount[gp]; }

    const uint pageBase = p * gNumCasters;

    // Write per (page, group) args BEFORE the scatter (so perGroupBase can double as the scatter
    // cursor below). InstanceCount + StartInstanceLocation are per-page; index count / offsets are
    // the per-group constants (mega-rebased) read from Rung 0's args (view v -> Rung-0 slot v+4).
    const uint rung0View = view + 4u;
    for (uint g2 = 0u; g2 < gNumGroups; ++g2)
    {
        uint src = (gArgBaseElems + rung0View * gNumGroups + g2) * 20u;
        uint4 a0 = Rung0Args.Load4(src);      // IndexCountPerInstance, InstanceCount(view), StartIndex, BaseVertex
        a0.y = perGroupCount[g2];             // OVERRIDE: this page's instance count
        a0.z += gGroupMega[g2].y;             // startIndex mega-rebase
        a0.w += gGroupMega[g2].x;             // baseVertex mega-rebase
        uint dst = (p * gNumGroups + g2) * 20u;
        PageDrawArgs.Store4(dst, a0);
        PageDrawArgs.Store(dst + 16u, pageBase + perGroupBase[g2]); // StartInstanceLocation -> page slice
    }

    // Pass 2: scatter the visible caster ids into the page's slice, grouped (perGroupBase = cursor).
    for (uint c2 = 0u; c2 < gNumCasters; ++c2)
    {
        CasterBounds b = Bounds[c2];
        if (PageIntersects(planes, b.center.xyz, b.halfExtents.xyz))
        {
            uint g = CasterGroup[c2];
            if (g < gNumGroups)
            {
                PageVisibleList[pageBase + perGroupBase[g]] = c2;
                perGroupBase[g] += 1u;
            }
        }
    }
}
