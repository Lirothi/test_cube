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
    "DescriptorTable(SRV(t0, numDescriptors=9, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

#define VSM_MAX_SETUP_GROUPS 64 // matches kMaxMegaGroups in ShadowGpuData::Rebuild
#define KMAX_SHADOW_LODS 4      // matches render::kMaxShadowLods
#define KVIEWLOD_VEC4    11     // (render::kMaxShadowViews=44 cull-views + 3) / 4, packed 4/uint4

cbuffer SetupCB : register(b0)
{
    uint gNumGroups;    // ShadowGpuData mesh-group count
    uint gArgBaseElems; // frame region base into Rung0Args, in 5-uint arg units (f*numViewsRung0*numGroups)
    uint gNumPages;     // kPoolPageCount
    uint gNumCasters;   // active caster count (ShadowGpuData::ActiveCasterCount) + per-page slice stride
    uint gForceAll;     // page cache: 1 = mark every resident page dirty (static caster moved / rebuild)
    uint gMegaActive;   // 1 = consolidated mega buffer (absolute starts); 0 = per-mesh IB fallback (lod-relative)
    uint gFlatLod;      // mega-off fallback LOD (uniform across pages; the per-page bind can't know each view)
    uint gNumLods;      // render::kMaxShadowLods (stride into gGroupLodMega)
    uint gScatterActive; // 1 = vsm_page_scatter_cs ran this frame (clipmap pages read its output)
    uint _pad3, _pad4, _pad5;
    // W5: the global wind, copied verbatim into every page's PerView slot at byte 192 so the shadow
    // VS (shadow_indirect_csm.hlsl) sways casters exactly like the gbuffer does. Packed as the two
    // float4s that make up that cbuffer tail.
    float4 gWind0;      // x=time, y=prevTime, z=windDirXZ.x, w=windDirXZ.y
    float4 gWind1;      // x=swayAmp, y=swayFreq, z=gustMul, w=prevGustMul
    float4x4 gViewProj[VSM_MAX_VIEWS];    // per VSM local view (spots then point faces)
    uint4    gViewLod[KVIEWLOD_VEC4];      // per cull-view shadow LOD (near->far tier + bias), packed 4/uint4
    // per (mesh-group, lod): x=mega absolute start, y=lod-relative start, z=index count, w=mega base vertex.
    uint4    gGroupLodMega[VSM_MAX_SETUP_GROUPS * KMAX_SHADOW_LODS];
};

struct CasterBounds { float4 center; float4 halfExtents; }; // xyz world center/half-extents (matches render::CasterBounds)

StructuredBuffer<uint>         PhysOwner     : register(t0); // physical page -> virtual owner / INVALID
ByteAddressBuffer              Rung0Args     : register(t1); // D3D12_DRAW_INDEXED_ARGUMENTS[view*group] region f (per-group index count / offsets)
StructuredBuffer<CasterBounds> Bounds        : register(t2); // per-caster world AABB (the unified buffer)
StructuredBuffer<uint>         CasterGroup   : register(t3); // per-caster mesh-group id
StructuredBuffer<uint>         PhysOwnerPrev : register(t4); // physical page -> last frame's owner (new-page detect)
StructuredBuffer<uint>         CasterMeta    : register(t5); // per-caster: bit0=dynamic, bits1+=object slot count on its FIRST slot (0 on continuation slots)
// Spatial scatter cull outputs (directional clipmap pages only — local views still cull here):
StructuredBuffer<uint>         PageGroupCount : register(t6); // per (page, group) instance count
StructuredBuffer<uint4>        PerGroup       : register(t7); // .x = group's global base in a page slice
StructuredBuffer<uint>         PageScatterDyn : register(t8); // per page: a dynamic caster landed here

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
    // W5: the wind tail of the shadow PerView CB. Bytes 64..192 (viewProjNoJitter /
    // prevViewProjNoJitter) stay unwritten — the depth-only shadow VS never reads them.
    PageProj.Store4(po + 192u, asuint(gWind0));
    PageProj.Store4(po + 208u, asuint(gWind1));

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

    const uint rung0View = view + 4u;

    // DIRECTIONAL clipmap pages get their instance lists from the spatial scatter pass, which already
    // wrote this page's per-group counts (and appended the caster ids into the page slice at each
    // group's GLOBAL base). Nothing to cull here. LOCAL (spot/point) views are perspective, so an
    // AABB page-rect is ill-defined for them and they keep the brute-force per-page cull below.
    //
    // NOTE, previously measured dead end: iterating Rung 0's per-VIEW visible set here instead of all
    // casters is ~1.8x SLOWER (Setup 0.435 -> 0.791 ms at 610 palms) — most RESIDENT pages belong to
    // the COARSE clipmap levels, which see nearly every caster anyway. Only the spatial scatter, which
    // touches a handful of pages per caster instead of testing all of them, actually breaks the
    // O(pages x casters) term. Do not "optimize" this loop by reordering the iteration.
    // Clipmap pages consume the scatter pass's output; if that pass is unavailable (shader failed to
    // compile) every page falls back to culling here, so shadows stay correct — just slower.
    const bool scattered = (view >= VSM_NUM_LOCAL_VIEWS) && (gScatterActive != 0u);

    uint perGroupCount[VSM_MAX_SETUP_GROUPS];
    bool dynamicOverlap = false;
    if (scattered)
    {
        for (uint gs0 = 0u; gs0 < gNumGroups; ++gs0)
        {
            perGroupCount[gs0] = PageGroupCount[p * gNumGroups + gs0];
        }
        dynamicOverlap = (PageScatterDyn[p] != 0u);
    }
    else
    {
        // Pass 1: count the casters visible in THIS page, per mesh-group, and note whether any DYNAMIC
        // caster overlaps (page-cache invalidation). B3: an object's submesh slots are CONSECUTIVE and
        // share its bounds, so test once per OBJECT (CasterMeta slot count) and apply the result to all
        // its slots — otherwise the (pages x casters) plane tests scale with the submesh split.
        for (uint gi = 0u; gi < gNumGroups; ++gi) { perGroupCount[gi] = 0u; }
        for (uint c = 0u; c < gNumCasters; )
        {
            const uint meta = CasterMeta[c];
            uint n = meta >> 1;
            if (n == 0u) { n = 1u; } // safety: a continuation slot can't lead the loop by construction
            CasterBounds b = Bounds[c];
            if (PageIntersects(planes, b.center.xyz, b.halfExtents.xyz))
            {
                for (uint s = 0u; s < n; ++s)
                {
                    uint g = CasterGroup[c + s];
                    if (g < gNumGroups) { perGroupCount[g] += 1u; }
                }
                if ((meta & 1u) != 0u) { dynamicOverlap = true; }
            }
            c += n;
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

    // Each group's base within this page's [p*gNumCasters, ...) list slice. SCATTERED pages use the
    // group's GLOBAL base (PerGroup.x, the prefix sum of per-group TOTAL caster counts) because that
    // is where the scatter pass appended — the slice is gNumCasters long, i.e. exactly the sum of
    // those totals, so the fixed layout always fits. The brute-force path packs tightly instead, with
    // a local prefix sum over what IT found, and reuses the array as its scatter cursor.
    uint perGroupBase[VSM_MAX_SETUP_GROUPS];
    if (scattered)
    {
        for (uint gb = 0u; gb < gNumGroups; ++gb) { perGroupBase[gb] = PerGroup[gb].x; }
    }
    else
    {
        uint acc = 0u;
        for (uint gp = 0u; gp < gNumGroups; ++gp) { perGroupBase[gp] = acc; acc += perGroupCount[gp]; }
    }

    const uint pageBase = p * gNumCasters;

    // Write per (page, group) args BEFORE the scatter (so perGroupBase can double as the scatter
    // cursor below). InstanceCount + StartInstanceLocation are per-page. The geometry (index count /
    // start / base vertex) comes from this VIEW's shadow LOD: gViewLod[rung0View] picks the LOD, and
    // gGroupLodMega[group * numLods + lod] gives {megaStart, lodRel, count, baseVertex} for it. Mega on
    // -> absolute mega start; mega off -> lod-relative start into the mesh's own IB (baseVertex 0).
    uint viewLod = gMegaActive ? (gViewLod[rung0View >> 2u][rung0View & 3u]) : gFlatLod;
    if (viewLod >= gNumLods) { viewLod = gNumLods - 1u; }
    for (uint g2 = 0u; g2 < gNumGroups; ++g2)
    {
        uint4 a0;
        if (g2 < VSM_MAX_SETUP_GROUPS) // per-view LOD from the CB table (the normal path)
        {
            uint4 e = gGroupLodMega[g2 * gNumLods + viewLod]; // {megaStart, lodRel, count, baseVertex}
            a0.x = e.z;                                       // IndexCountPerInstance = LOD's index count
            a0.z = gMegaActive ? e.x : e.y;                   // StartIndexLocation: mega-absolute / lod-relative
            a0.w = gMegaActive ? e.w : 0u;                    // BaseVertexLocation: mega base / mesh-own VB (0)
        }
        else // > VSM_MAX_SETUP_GROUPS (mega already off): fall back to the Rung0 args (perViewGroup LOD)
        {
            uint src = (gArgBaseElems + rung0View * gNumGroups + g2) * 20u;
            uint4 r = Rung0Args.Load4(src);
            a0.x = r.x; a0.z = r.z; a0.w = r.w;
        }
        a0.y = perGroupCount[g2];             // OVERRIDE: this page's instance count
        uint dst = (p * gNumGroups + g2) * 20u;
        PageDrawArgs.Store4(dst, a0);
        PageDrawArgs.Store(dst + 16u, pageBase + perGroupBase[g2]); // StartInstanceLocation -> page slice
    }

    // Pass 2: scatter the visible caster ids into the page's slice, grouped (perGroupBase = cursor).
    // Same once-per-object test as pass 1. Scattered (clipmap) pages already had their list written
    // by vsm_page_scatter_cs, so they stop here — this loop is the local-light path only.
    if (scattered) { return; }
    for (uint c2 = 0u; c2 < gNumCasters; )
    {
        uint n2 = CasterMeta[c2] >> 1;
        if (n2 == 0u) { n2 = 1u; }
        CasterBounds b = Bounds[c2];
        if (PageIntersects(planes, b.center.xyz, b.halfExtents.xyz))
        {
            for (uint s2 = 0u; s2 < n2; ++s2)
            {
                uint g = CasterGroup[c2 + s2];
                if (g < gNumGroups)
                {
                    PageVisibleList[pageBase + perGroupBase[g]] = c2 + s2;
                    perGroupBase[g] += 1u;
                }
            }
        }
        c2 += n2;
    }
}
