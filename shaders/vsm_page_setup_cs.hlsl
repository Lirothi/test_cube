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
    "DescriptorTable(SRV(t0, numDescriptors=11, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=5, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

#define VSM_MAX_SETUP_GROUPS 64 // matches kMaxMegaGroups in ShadowGpuData::Rebuild
#define KMAX_SHADOW_LODS 4      // matches render::kMaxShadowLods
#define KVIEWLOD_VEC4    11     // (render::kMaxShadowViews=44 cull-views + 3) / 4, packed 4/uint4
#define VSM_SETUP_LANES  8      // == numthreads y; threads per page that share the per-group loop

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
    // Single-draw page render: bits to shift the PHYSICAL page index by when packing it into a
    // visible-list entry (vsm::kPageIdShift = 22). 0 = do NOT pack — the per-page loop path binds a
    // per-page root CBV and the entry is a bare caster id. Never shift by 0: `id | (p << 0)` would
    // corrupt the low bits, which is why both writers branch instead of relying on a shift.
    uint gPageIdShift;
    // Compacted draw args: 1 = append only NON-EMPTY (page, group) records at an atomically bumped
    // slot and let PageArgCount drive ExecuteIndirect's count buffer; 0 = the fixed [page][group]
    // layout the per-page loop indexes directly. Only ever 1 when the single-draw path is active —
    // the loop computes its own argOffset from (page, group) and would read garbage otherwise.
    uint gCompactArgs;
    // S5: 1 = LOCAL (spot/point) views were scattered too, so their pages skip the brute-force
    // cull like clipmap pages already do. Occupies what used to be _pad5, so the CB layout is
    // unchanged. Toggled live by vsm::g_scatterLocalViews, which exists so the two local-light
    // paths can be A/B-ed inside ONE binary.
    uint gScatterLocals;
    // W5: the global wind, copied verbatim into every page's PerView slot at byte 192 so the shadow
    // VS (shadow_indirect_csm.hlsl) sways casters exactly like the gbuffer does. Packed as the two
    // float4s that make up that cbuffer tail.
    float4 gWind0;      // x=time, y=prevTime, z=windDirXZ.x, w=windDirXZ.y
    float4 gWind1;      // x=swayAmp, y=swayFreq, z=gustMul, w=prevGustMul
    float4x4 gViewProj[VSM_MAX_VIEWS];    // per VSM local view (spots then point faces)
    uint4    gViewLod[KVIEWLOD_VEC4];      // per cull-view shadow LOD (near->far tier + bias), packed 4/uint4
    // NOTE: the per-(group,lod) mega ranges and the per-group LOD override used to live HERE, as CB
    // arrays sized VSM_MAX_SETUP_GROUPS. That is what capped the whole shadow path at 64 mesh-groups
    // — a CB array cannot be sized by gNumGroups. Both are SRVs now (t9/t10), sized by the real group
    // count. gViewLod stays: the view count is fixed at 44, so it has no such problem.
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
// Per-group geometry tables, sized by the REAL group count (CB arrays capped at 64 until the cap
// removal). A descriptor table is POSITIONAL: t9/t10 must be the 10th and 11th handles the CPU
// stages, in this order.
StructuredBuffer<uint4>        GroupLodMega     : register(t9);  // per (group,lod): {megaStart, lodRel, count, baseVertex}
StructuredBuffer<int>          GroupLodOverride : register(t10); // per group: ABSOLUTE LOD, -1 = use the view LOD

RWByteAddressBuffer      PageDrawArgs    : register(u0); // per (page, group) D3D12_DRAW_INDEXED_ARGUMENTS
RWByteAddressBuffer      PageProj        : register(u1); // per page off-center viewProj (256-byte stride for root-CBV)
RWStructuredBuffer<uint> PageVisibleList : register(u2); // per (page, slot) visible caster id
RWStructuredBuffer<uint> PerPageDirty    : register(u3); // per page: 1 = re-render this frame, 0 = cached
// RAW, not structured: ClearUnorderedAccessViewUint (how the CPU zeroes this each frame) rejects
// structured buffers outright — GBV id=1156. Same shape as PageDrawArgs above.
RWByteAddressBuffer      PageArgCount    : register(u4); // byte 0 = appended arg-record count

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
    // dtid.y spans VSM_SETUP_LANES because RecordComputeDispatch has a fixed 8x8 group shape. This
    // used to open `if (dtid.y != 0) return;` — 7 of every 8 threads died immediately. The per-group
    // args loop is the term that scales with the mesh-group count, and on a SCATTERED page it now
    // has no per-thread state (both counts and bases are direct buffer reads since the cap removal),
    // so it is split across the lanes instead of run serially in one.
    //
    // The brute-force (local-light) path keeps its serial prefix sum and stays lane 0 only.
    const uint p = dtid.x;
    const uint lane = dtid.y;
    if (p >= gNumPages) { return; }

    const uint owner = PhysOwner[p];
    if (owner == VSM_INVALID)
    {
        // Free page: zero-instance args for every group (drawn as a no-op) + not dirty (the clear
        // skips it). pageProj left stale (unread). A page that becomes resident is fully rewritten.
        // With compacted args there is nothing to write at all — a record that is never appended
        // cannot be reached, since the count buffer stops the draw short of it. That skip IS the
        // compaction: free pages are ~45% of the pool.
        if (lane == 0u) { PerPageDirty[p] = 0u; }
        if (gCompactArgs == 0u)
        {
            for (uint g = lane; g < gNumGroups; g += VSM_SETUP_LANES)
            {
                uint off = (p * gNumGroups + g) * 20u;
                PageDrawArgs.Store4(off, uint4(0u, 0u, 0u, 0u));
                PageDrawArgs.Store(off + 16u, 0u);
            }
        }
        return;
    }

    // Page cache: a resident page needs re-render iff it was just allocated / re-owned (isNew), a
    // dynamic caster overlaps it (found in pass 1 below), or a static caster moved this frame
    // (gForceAll). Otherwise its depth is still valid from a previous frame -> skip clear + draw.
    const bool isNew = (owner != PhysOwnerPrev[p]);

    uint view, level, px, py;
    VsmDecodePage(owner, view, level, px, py);

    // Established BEFORE the projection so the extra lanes can retire without paying for it: the
    // brute-force path is inherently serial (count -> prefix sum -> scatter cursor, all in one
    // thread's registers), so only lane 0 walks it.
    const bool scattered = (gScatterActive != 0u) &&
                           (view >= VSM_NUM_LOCAL_VIEWS || gScatterLocals != 0u);
    if (!scattered && lane != 0u) { return; }

    // Off-center projection: the page (px,py) at this level covers the NDC sub-rect centered at
    // (cx,cy) with half-size 1/axis; scale/bias clip so that sub-rect fills [-1,1] (z preserved).
    // Lane 0 only: one page has ONE projection, and only the serial path needs the planes derived
    // from it. The extra lanes exist for the per-group args loop, which does not read either.
    float4x4 pm = (float4x4)0;
    if (lane == 0u)
    {
        const float a = (float)(VSM_L0_AXIS >> level);
        const float cx = -1.0f + (2.0f * px + 1.0f) / a;
        const float cy =  1.0f - (2.0f * py + 1.0f) / a;
        const float4x4 S = float4x4(a, 0, 0, 0,
                                    0, a, 0, 0,
                                    0, 0, 1, 0,
                                    -cx * a, -cy * a, 0, 1);
        pm = mul(gViewProj[view], S);
        uint po = p * 256u; // 256-byte stride (root-CBV alignment)
        PageProj.Store4(po +  0u, asuint(pm[0]));
        PageProj.Store4(po + 16u, asuint(pm[1]));
        PageProj.Store4(po + 32u, asuint(pm[2]));
        PageProj.Store4(po + 48u, asuint(pm[3]));
        // W5: the wind tail of the shadow PerView CB. Bytes 64..192 (viewProjNoJitter /
        // prevViewProjNoJitter) stay unwritten — the depth-only shadow VS never reads them.
        PageProj.Store4(po + 192u, asuint(gWind0));
        PageProj.Store4(po + 208u, asuint(gWind1));
    }

    // The page's 6 frustum planes from pm (columns; clip = mul(world, pm)). Positive-vertex test is
    // scale-invariant, so unnormalized planes are fine. Near/far match the view (S preserves z).
    // Only the brute-force cull reads them, and that runs on lane 0 — the only lane holding pm.
    const float4x4 pt = transpose(pm); // pt[k] = column k of pm
    float4 planes[6];
    planes[0] = pt[3] + pt[0]; // left
    planes[1] = pt[3] - pt[0]; // right
    planes[2] = pt[3] + pt[1]; // bottom
    planes[3] = pt[3] - pt[1]; // top
    planes[4] = pt[2];         // near (z >= 0)
    planes[5] = pt[3] - pt[2]; // far

    const uint rung0View = view + 4u;
    // Groups this lane owns in the per-group loops below: strided across lanes on the scattered
    // path, the whole range on the serial one (where only lane 0 got this far).
    const uint gStart = scattered ? lane : 0u;
    const uint gStep  = scattered ? (uint)VSM_SETUP_LANES : 1u;

    // EVERY page now gets its instance list from the spatial scatter pass, which already wrote this
    // page's per-group counts (and appended the caster ids into the page slice at each group's
    // GLOBAL base). Nothing to cull here. S5 brought LOCAL (spot/point) views into that pass too —
    // their perspective AABB rect is Unreal's straddle-the-near-plane-and-take-the-full-rect trick.
    //
    // The brute-force block below is now purely the FALLBACK for `gScatterActive == 0`, i.e. the
    // scatter PSO failed to build. It is kept rather than deleted because without it that failure
    // means NO shadows instead of slow ones — and it is the one place the group cap still bites
    // (its two per-thread tables), which is acceptable for a degraded path that also has to be
    // simultaneously over 64 groups to notice.
    //
    // NOTE, previously measured dead end: iterating Rung 0's per-VIEW visible set here instead of all
    // casters is ~1.8x SLOWER (Setup 0.435 -> 0.791 ms at 610 palms) — most RESIDENT pages belong to
    // the COARSE clipmap levels, which see nearly every caster anyway. Only the spatial scatter, which
    // touches a handful of pages per caster instead of testing all of them, actually breaks the
    // O(pages x casters) term. Do not "optimize" this loop by reordering the iteration.
    // Clipmap pages consume the scatter pass's output; if that pass is unavailable (shader failed to
    // compile) every page falls back to culling here, so shadows stay correct — just slower.
    // Per-thread group tables. THEY are what caps this shader at VSM_MAX_SETUP_GROUPS: a local array
    // cannot be sized by gNumGroups, and indexing one past its end is undefined — an indexable temp
    // lives in the thread's own scratch, so the write lands on whatever else is there (the page
    // matrix, the frustum planes). gNumGroups arrives UNCLAMPED from the CPU, so the guard has to be
    // here.
    //
    // Only the BRUTE-FORCE path spends them. A scattered (clipmap) page reads the scatter pass's
    // buffers directly in the args loop below and never touches these arrays — which is what makes
    // the directional path cap-free, and it costs nothing: each element was written once and read
    // once anyway.
    uint perGroupCount[VSM_MAX_SETUP_GROUPS];
    const uint numLocalGroups = min(gNumGroups, (uint)VSM_MAX_SETUP_GROUPS);
    bool dynamicOverlap = false;
    if (scattered)
    {
        dynamicOverlap = (PageScatterDyn[p] != 0u);
    }
    else
    {
        // Pass 1: count the casters visible in THIS page, per mesh-group, and note whether any DYNAMIC
        // caster overlaps (page-cache invalidation). B3: an object's submesh slots are CONSECUTIVE and
        // share its bounds, so test once per OBJECT (CasterMeta slot count) and apply the result to all
        // its slots — otherwise the (pages x casters) plane tests scale with the submesh split.
        for (uint gi = 0u; gi < numLocalGroups; ++gi) { perGroupCount[gi] = 0u; }
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
                    // numLocalGroups, not gNumGroups: a group with no slot in the table cannot be
                    // counted, and it draws nothing below. Deterministic loss beyond the cap.
                    uint g = CasterGroup[c + s];
                    if (g < numLocalGroups) { perGroupCount[g] += 1u; }
                }
                if ((meta & 1u) != 0u) { dynamicOverlap = true; }
            }
            c += n;
        }
    }

    // Cache decision: dirty pages clear + redraw; clean pages keep their cached depth (draw nothing).
    // Every lane derives `dirty` from the same buffer reads, so they agree by construction; the
    // flag itself has one writer.
    const bool dirty = isNew || dynamicOverlap || (gForceAll != 0u);
    if (lane == 0u) { PerPageDirty[p] = dirty ? 1u : 0u; }
    if (!dirty)
    {
        if (gCompactArgs == 0u) // compacted: append nothing (see the free-page branch above)
        {
            for (uint gc = gStart; gc < gNumGroups; gc += gStep)
            {
                uint off = (p * gNumGroups + gc) * 20u;
                PageDrawArgs.Store4(off, uint4(0u, 0u, 0u, 0u)); // InstanceCount 0 -> caster draw is a no-op
                PageDrawArgs.Store(off + 16u, 0u);
            }
        }
        return;
    }

    // Each group's base within this page's [p*gNumCasters, ...) list slice. SCATTERED pages use the
    // group's GLOBAL base (PerGroup.x, the prefix sum of per-group TOTAL caster counts) because that
    // is where the scatter pass appended — the slice is gNumCasters long, i.e. exactly the sum of
    // those totals, so the fixed layout always fits. The brute-force path packs tightly instead, with
    // a local prefix sum over what IT found, and reuses the array as its scatter cursor.
    // Brute-force only, for the same reason as perGroupCount: a scattered page reads PerGroup[g].x
    // straight from the buffer in the args loop, so it neither fills nor reads this.
    uint perGroupBase[VSM_MAX_SETUP_GROUPS];
    if (!scattered)
    {
        uint acc = 0u;
        for (uint gp = 0u; gp < numLocalGroups; ++gp) { perGroupBase[gp] = acc; acc += perGroupCount[gp]; }
    }

    const uint pageBase = p * gNumCasters;

    // Write per (page, group) args BEFORE the scatter (so perGroupBase can double as the scatter
    // cursor below). InstanceCount + StartInstanceLocation are per-page. The geometry (index count /
    // start / base vertex) comes from this VIEW's shadow LOD: gViewLod[rung0View] picks the LOD, and
    // gGroupLodMega[group * numLods + lod] gives {megaStart, lodRel, count, baseVertex} for it. Mega on
    // -> absolute mega start; mega off -> lod-relative start into the mesh's own IB (baseVertex 0).
    uint viewLod = gMegaActive ? (gViewLod[rung0View >> 2u][rung0View & 3u]) : gFlatLod;
    if (viewLod >= gNumLods) { viewLod = gNumLods - 1u; }
    for (uint g2 = gStart; g2 < gNumGroups; g2 += gStep)
    {
        // This page's instance count for the group, and where its run starts in the page slice.
        // Real control flow, not a select: each branch may only read what its own path wrote, and
        // the cap test must actually gate the array access rather than pick between two evaluated
        // reads. Scattered pages take the buffers (uncapped); the brute-force path takes its tables,
        // and a group past the cap has no slot, so it reports empty and draws nothing.
        uint groupCount, groupBase;
        if (scattered)
        {
            groupCount = PageGroupCount[p * gNumGroups + g2];
            groupBase  = PerGroup[g2].x;
        }
        else if (g2 < (uint)VSM_MAX_SETUP_GROUPS)
        {
            groupCount = perGroupCount[g2];
            groupBase  = perGroupBase[g2];
        }
        else
        {
            groupCount = 0u;
            groupBase  = 0u;
        }
        // Compacted args: an empty group would only ever be a zero-instance no-op, so don't append
        // it. This is where the bulk of the 65k-record worst case disappears — even a RESIDENT page
        // usually touches one or two mesh-groups out of all of them.
        if (gCompactArgs != 0u && groupCount == 0u) { continue; }
        // Per-GROUP LOD, for EVERY group: a chunked-terrain group carries an ABSOLUTE override (its
        // camera tier this frame, -1 = none) so the caster is the same geometry the gbuffer
        // rasterized, on every view. Everything else takes the view LOD.
        //
        // No cap branch any more. This used to fall back to Rung0Args past VSM_MAX_SETUP_GROUPS,
        // which meant the tail groups quietly lost their override and reverted to the view LOD —
        // banding back on exactly the finest grids. Both tables are now SRVs sized by gNumGroups.
        uint4 a0;
        const int ovr = GroupLodOverride[g2];
        const uint groupLod = (ovr >= 0) ? min((uint)ovr, gNumLods - 1u) : viewLod;
        uint4 e = GroupLodMega[g2 * gNumLods + groupLod]; // {megaStart, lodRel, count, baseVertex}
        a0.x = e.z;                                      // IndexCountPerInstance = LOD's index count
        a0.z = gMegaActive ? e.x : e.y;                  // StartIndexLocation: mega-absolute / lod-relative
        a0.w = gMegaActive ? e.w : 0u;                   // BaseVertexLocation: mega base / mesh-own VB (0)
        a0.y = groupCount;                    // OVERRIDE: this page's instance count
        // Record slot. Fixed [page][group] for the loop path (which derives its argOffset from those
        // two). Compacted: any free slot will do — the VS no longer infers the page from the record
        // position, it unpacks it from the instance id, which is exactly what Step 1 bought.
        uint dst;
        if (gCompactArgs != 0u)
        {
            uint slot;
            PageArgCount.InterlockedAdd(0u, 1u, slot);
            // Unreachable by construction (appends <= pages x groups = the buffer's capacity), and
            // ExecuteIndirect clamps the count to maxCount anyway — but never store out of bounds.
            if (slot >= gNumPages * gNumGroups) { continue; }
            dst = slot * 20u;
        }
        else
        {
            dst = (p * gNumGroups + g2) * 20u;
        }
        PageDrawArgs.Store4(dst, a0);
        PageDrawArgs.Store(dst + 16u, pageBase + groupBase); // StartInstanceLocation -> page slice
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
                if (g < numLocalGroups) // same cap as pass 1: no slot -> not counted, so not scattered
                {
                    const uint vid = c2 + s2;
                    PageVisibleList[pageBase + perGroupBase[g]] =
                        (gPageIdShift != 0u) ? (vid | (p << gPageIdShift)) : vid;
                    perGroupBase[g] += 1u;
                }
            }
        }
        c2 += n2;
    }
}
