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

// Occlusion plan S5b.2: a second invocation per frame in PASS-B mode (gPassB) turns the post
// cull's counts (PageGroupCountB, t11) into pass B's args (u0 = the B args buffer, u4 = its
// compaction counter): StartInstance = bucket base + pass A's count, so pass B draws only the
// entries the post cull appended after pass A's. Nothing else is written in that mode -- the
// page projections, the dirty bits and the brute-force lists are pass A's.
#define VSM_PAGE_SETUP_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=12, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
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
    // Wind page-cache contract (transcribed from Unreal's per-level WPO disable threshold,
    // VirtualShadowMapClipmap.cpp): clipmap levels BELOW this animate wind and are re-rendered
    // every frame while wind blows; levels AT/ABOVE it render their casters RIGID (the per-page
    // wind amplitude below is zeroed) so a cached page and a fresh render agree — that
    // consistency is what makes caching them valid, with no pop when one finally re-renders.
    // Local views always animate (their whole frustum is near-field). 0 = wind is not blowing
    // this frame (nothing wind-dirties); >= clipmap level count = the old animate-everywhere.
    uint gWindDirtyMaxLevel;
    // Per-VSM-view "matrix changed this frame" bits (x = views 0..31, y = 32..39). A cached
    // page's id is (view, level, px, py) — an NDC rect of ITS VIEW, with no scroll offset —
    // so ANY view translation/snap/rotation makes every cached page of that view hold depth
    // for the WRONG world rect while its owner id never changes (isNew cannot see it; Unreal
    // solves the clipmap case with a per-level PageOffset). Until pages scroll, the honest
    // cache contract is: a view that moved re-renders wholesale. Static camera + static
    // lights = everything caches; a dolly re-renders exactly what it invalidates.
    uint2 gViewDirtyMask;
    uint gPassB; // S5b.2: 1 = pass-B args from PageGroupCountB (see the header comment)
    // W5: the global wind, copied verbatim into every page's PerView slot at byte 192 so the shadow
    // VS (shadow_indirect_csm.hlsl) sways casters exactly like the gbuffer does. Packed as the two
    // float4s that make up that cbuffer tail.
    float4 gWind0;      // x=time, y=prevTime, z=windDirXZ.x, w=windDirXZ.y
    float4 gWind1;      // x=swayAmp, y=swayFreq, z=gustMul, w=prevGustMul
    // (camPos.xyz, windFadeEnd): the world-distance sway falloff the shadow VS applies (byte
    // 224 of every page slot). Identical across levels, which is what keeps the clipmap blend
    // band consistent; the per-level rigid gate above it only decides CACHING. w = 0 disables.
    float4 gWindFade;
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
StructuredBuffer<int>          GroupLodOverride : register(t10); // per group: see ResolveGroupLod
StructuredBuffer<uint>         PageGroupCountB  : register(t11); // S5b.2: pass B's per (page, group) counts (placeholder in pass A)

// Per-INSTANCE LOD note: the SCATTERED path no longer resolves a LOD here at all. The scatter pass
// buckets every caster instance into a VIRTUAL group (group * gNumLods + lod) chosen from that
// instance's own receiver LOD (CasterLod, Unreal's per-primitive rule), so by the time this shader
// runs, the LOD is already part of the group index. GroupLodOverride survives only for the
// BRUTE-FORCE fallback below (scatter PSO failed to build), where it carries the chunked-terrain
// EXACT override: >= 0 = that LOD on every view, -1 = the view LOD.
uint ResolveGroupLod(int ovr, uint viewLod, uint numLods)
{
    if (ovr >= 0) { return min((uint)ovr, numLods - 1u); }
    return viewLod;
}

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

    // Per-instance caster LOD: every [page][group] layout in this shader is really [page][VIRTUAL
    // group], where a virtual group is (static group * gNumLods + lod) — the scatter pass buckets
    // each instance by its own receiver LOD. numVg is the arg/count stride everywhere below.
    const uint numVg = gNumGroups * gNumLods;

    const uint owner = PhysOwner[p];
    if (owner == VSM_INVALID)
    {
        // Free page: zero-instance args for every group (drawn as a no-op) + not dirty (the clear
        // skips it). pageProj left stale (unread). A page that becomes resident is fully rewritten.
        // With compacted args there is nothing to write at all — a record that is never appended
        // cannot be reached, since the count buffer stops the draw short of it. That skip IS the
        // compaction: free pages are ~45% of the pool.
        if (lane == 0u && gPassB == 0u) { PerPageDirty[p] = 0u; }
        if (gCompactArgs == 0u)
        {
            for (uint g = lane; g < numVg; g += VSM_SETUP_LANES)
            {
                uint off = (p * numVg + g) * 20u;
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
    // S5b.2, pass B: only scattered pages can carry pass-B entries; a brute-force page's B args
    // are zero (or absent, compacted), and nothing below is pass B's to write.
    if (gPassB != 0u && !scattered)
    {
        if (gCompactArgs == 0u)
        {
            for (uint gb = lane; gb < numVg; gb += VSM_SETUP_LANES)
            {
                uint off = (p * numVg + gb) * 20u;
                PageDrawArgs.Store4(off, uint4(0u, 0u, 0u, 0u));
                PageDrawArgs.Store(off + 16u, 0u);
            }
        }
        return;
    }
    if (!scattered && lane != 0u) { return; }

    // Off-center projection: the page (px,py) at this level covers the NDC sub-rect centered at
    // (cx,cy) with half-size 1/axis; scale/bias clip so that sub-rect fills [-1,1] (z preserved).
    // Lane 0 only: one page has ONE projection, and only the serial path needs the planes derived
    // from it. The extra lanes exist for the per-group args loop, which does not read either.
    float4x4 pm = (float4x4)0;
    // Clipmap level of this page's VIEW (each clipmap level is its own view at mip 0);
    // locals report 0 and always animate.
    const bool isLocalView = (view < VSM_NUM_LOCAL_VIEWS);
    const uint clipLevel = isLocalView ? 0u : (view - VSM_NUM_LOCAL_VIEWS);
    const bool windHere = (gWindDirtyMaxLevel != 0u) &&
                          (isLocalView || clipLevel < gWindDirtyMaxLevel);
    if (lane == 0u && gPassB == 0u)
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
        // Rigid beyond the wind range: zero the sway amplitude THIS page's casters see, so
        // far clipmap levels draw un-swayed geometry that a cached page can keep bit-stable.
        float4 wind1 = gWind1;
        if (!windHere) { wind1.x = 0.0f; }
        PageProj.Store4(po + 208u, asuint(wind1));
        PageProj.Store4(po + 224u, asuint(gWindFade)); // (camPos, fade end) -> shadow VS w2
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
    // Wind no longer rides gForceAll: it dirties only the pages that actually ANIMATE it
    // (near clipmap levels + locals). Everything past the range renders rigid and caches.
    const bool viewDirty = (view < 32u) ? (((gViewDirtyMask.x >> view) & 1u) != 0u)
                                        : (((gViewDirtyMask.y >> (view - 32u)) & 1u) != 0u);
    // S5b.2, pass B: the verdict pass A wrote, read back through the same UAV (never rewritten).
    const bool dirty = (gPassB != 0u) ? (PerPageDirty[p] != 0u)
                                      : (isNew || dynamicOverlap || (gForceAll != 0u) || windHere || viewDirty);
    if (lane == 0u && gPassB == 0u) { PerPageDirty[p] = dirty ? 1u : 0u; }
    if (!dirty)
    {
        if (gCompactArgs == 0u) // compacted: append nothing (see the free-page branch above)
        {
            for (uint gc = gStart; gc < numVg; gc += gStep)
            {
                uint off = (p * numVg + gc) * 20u;
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

    const uint pageBase = p * gNumCasters * gNumLods; // slice is per-LOD bucketed -> x gNumLods

    // Write per (page, VIRTUAL group) args BEFORE the brute scatter (so perGroupBase can double as
    // the scatter cursor below). InstanceCount + StartInstanceLocation are per-page.
    uint viewLod = gMegaActive ? (gViewLod[rung0View >> 2u][rung0View & 3u]) : gFlatLod;
    if (viewLod >= gNumLods) { viewLod = gNumLods - 1u; }

    if (scattered)
    {
        // SCATTERED path: the LOD is baked into the virtual group index (vg = group * gNumLods +
        // lod, chosen per INSTANCE by the scatter pass from CasterLod), so there is nothing to
        // resolve here — and conveniently vg IS the GroupLodMega index, because that table was
        // always laid out (group, lod)-major with the same stride.
        for (uint vg = gStart; vg < numVg; vg += gStep)
        {
            // S5b.2: pass B's entries sit right after pass A's in the same bucket run, so its
            // count comes from the post cull and its start is offset by pass A's count.
            const uint countA = PageGroupCount[p * numVg + vg];
            const uint groupCount = (gPassB != 0u) ? PageGroupCountB[p * numVg + vg] : countA;
            const uint startOffset = (gPassB != 0u) ? countA : 0u;
            // Compacted args: an empty bucket would only ever be a zero-instance no-op, so don't
            // append it. This is where the bulk of the worst case disappears — even a RESIDENT
            // page usually touches one or two buckets out of all of them.
            if (gCompactArgs != 0u && groupCount == 0u) { continue; }
            const uint g = vg / gNumLods;
            const uint lod = vg - g * gNumLods;
            uint4 a0;
            const uint4 e = GroupLodMega[vg];   // {megaStart, lodRel, count, baseVertex}
            a0.x = e.z;                          // IndexCountPerInstance = LOD's index count
            a0.z = gMegaActive ? e.x : e.y;      // StartIndexLocation: mega-absolute / lod-relative
            a0.w = gMegaActive ? e.w : 0u;       // BaseVertexLocation: mega base / mesh-own VB (0)
            a0.y = groupCount;                   // this page's instance count for the bucket
            // Bucket base in the page slice: group base scaled by the bucket count plus this LOD's
            // sub-run — MUST mirror vsm_page_scatter_cs's append arithmetic exactly.
            const uint bucketBase = PerGroup[g].x * gNumLods + lod * PerGroup[g].w;
            uint dst;
            if (gCompactArgs != 0u)
            {
                uint slot;
                PageArgCount.InterlockedAdd(0u, 1u, slot);
                if (slot >= gNumPages * numVg) { continue; } // ExecuteIndirect clamps anyway; never store OOB
                dst = slot * 20u;
            }
            else
            {
                dst = (p * numVg + vg) * 20u;
            }
            PageDrawArgs.Store4(dst, a0);
            PageDrawArgs.Store(dst + 16u, pageBase + bucketBase + startOffset);
        }
    }
    else
    {
        // BRUTE-FORCE fallback (scatter PSO unavailable): stays per-GROUP with the view LOD (plus
        // the chunk EXACT override) — no per-instance data reaches this path, and it exists so a
        // failed scatter compile degrades to slow-but-correct instead of no shadows. Its single
        // per-group LOD picks the arg record's virtual-group slot, so both paths share one layout.
        for (uint g2 = gStart; g2 < gNumGroups; g2 += gStep)
        {
            uint groupCount, groupBase;
            if (g2 < (uint)VSM_MAX_SETUP_GROUPS)
            {
                groupCount = perGroupCount[g2];
                groupBase  = perGroupBase[g2];
            }
            else
            {
                groupCount = 0u;
                groupBase  = 0u;
            }
            if (gCompactArgs != 0u && groupCount == 0u) { continue; }
            uint4 a0;
            const int ovr = GroupLodOverride[g2];
            const uint groupLod = ResolveGroupLod(ovr, viewLod, gNumLods);
            const uint4 e = GroupLodMega[g2 * gNumLods + groupLod];
            a0.x = e.z;
            a0.z = gMegaActive ? e.x : e.y;
            a0.w = gMegaActive ? e.w : 0u;
            a0.y = groupCount;
            uint dst;
            if (gCompactArgs != 0u)
            {
                uint slot;
                PageArgCount.InterlockedAdd(0u, 1u, slot);
                if (slot >= gNumPages * numVg) { continue; }
                dst = slot * 20u;
            }
            else
            {
                dst = (p * numVg + g2 * gNumLods + groupLod) * 20u;
            }
            PageDrawArgs.Store4(dst, a0);
            // Brute path packs its list TIGHTLY from pageBase (local prefix sum), independent of
            // the scatter's bucket layout — the slice is bigger than it needs, which is fine.
            PageDrawArgs.Store(dst + 16u, pageBase + groupBase);
        }
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
