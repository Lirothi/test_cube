// Spatial scatter cull for the DIRECTIONAL clipmap views (the inversion of the per-page cull).
//
// The per-page cull in vsm_page_setup_cs is O(pages x casters): every one of the 1024 pool pages
// tests every caster's AABB. That term grows linearly with the scene (measured: 0.40 ms at 610
// palms, and it is the part that keeps growing). This pass inverts it — each CASTER projects its
// world AABB into a clipmap level's page grid and writes itself only into the pages it actually
// covers. A plant covers a handful of pages, not 1024, so the work collapses.
//
// Thread = (caster object, clipmap level). Only the FIRST slot of an object leads (CasterMeta slot
// count), so the AABB is projected once per object and all its submesh slots are appended together —
// the same per-object dedupe the brute-force path relies on.
//
// SLICE LAYOUT (why no prefix-sum pass is needed): a page's visible-list slice is already
// gNumCasters long, which is exactly the sum of all groups' TOTAL caster counts. So each group can
// live at its GLOBAL base (PerGroup[g].x) inside every page's slice instead of at a per-page prefix
// sum, and appending is just an atomic bump of that (page, group) counter for the rank. The setup
// pass then reads the same counter as InstanceCount and uses the global base as
// StartInstanceLocation. Costs nothing extra: the slice was already sized for the worst case.
//
// S5: LOCAL (spot/point) views scatter here too. They are PERSPECTIVE, so the NDC rect needs the
// per-corner divide, and a box straddling w=0 has no well-defined rect at all — the answer is
// Unreal's (Nanite/NaniteHZBCull.ush::BoxCullFrustumPerspective): detect the straddle and bail to
// the FULL [-1,1] rect instead of producing a wrapped one. Conservative, and conservative is always
// safe here (a page may draw a caster it turns out not to touch; the rasterizer discards it).
// Locals also use the whole mip PYRAMID (a pixel marks one page at its own level), unlike clipmap
// views where the level IS the LOD and only mip 0 exists — so a local view walks all 5 levels.
#pragma pack_matrix(row_major)
#include "vsm_addressing.hlsli"

#define VSM_SCATTER_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=6, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

#define KMAX_SHADOW_LODS 4 // matches render::kMaxShadowLods; the per-group LOD bucket count

cbuffer ScatterCB : register(b0)
{
    uint gNumCasters;   // active caster SLOT count (also the per-page slice stride)
    uint gNumGroups;    // mesh-group count
    uint gNumLevels;    // active clipmap levels (VSM_NUM_CLIPMAP_LEVELS)
    // Single-draw page render: see the identically-named field in vsm_page_setup_cs.hlsl's SetupCB.
    // Both writers of PageVisibleList must pack identically, so this is fed from the same CPU value.
    uint gPageIdShift;
    uint gNumLocalViews; // local (spot/point) views scattered after the clipmap levels; 0 = none
    uint gPerInstanceLod; // 1 = bucket FLOOR casters by their receiver LOD; 0 = old per-view (A/B)
    uint2 _pad0;
    // Every VSM view, same indexing as the setup CB (0..31 local, 32..39 clipmap). Was only the 8
    // clipmap matrices before S5. An inactive slot is a ZERO matrix, which the perspective path
    // rejects on its own (maxW <= 0) — and its pages are not resident either.
    float4x4 gViewProj[VSM_MAX_VIEWS];
    // Per SCATTER TARGET (same y indexing as the dispatch: clipmap levels, then local views): the
    // view's own shadow LOD from the tier curve. It is only the lower bound for FLOOR casters —
    // the per-caster LOD below can only push COARSER past it, never finer.
    uint4 gTargetLod[(VSM_MAX_VIEWS + 3) / 4];
};

struct CasterBounds { float4 center; float4 halfExtents; };

StructuredBuffer<CasterBounds> Bounds      : register(t0); // per-caster world AABB (unified buffer)
StructuredBuffer<uint>         CasterGroup : register(t1); // per-caster mesh-group id
StructuredBuffer<uint>         CasterMeta  : register(t2); // bit0 = dynamic, bits1+ = slot count on the FIRST slot
StructuredBuffer<uint>         PageTable   : register(t3); // virtual page -> resident bit | physical index
// Global base of each group's run inside every page's slice (prefix sum of per-group TOTAL caster
// counts) — Rung 0's PerGroup.x. .w carries the group's own caster COUNT, which sizes the per-LOD
// buckets below. A buffer rather than a CB array so it needs no group-count cap.
StructuredBuffer<uint4>        PerGroup    : register(t4);
// Per caster SLOT: the LOD its RECEIVER draws this frame (Unreal's per-primitive rule — the shadow
// pass starts from the LOD the main view picked). bits 0..3 = LOD; bit 7 = EXACT (chunked terrain:
// that LOD on every view, no per-view coarsening). Without the bit, the caster takes
// max(receiverLod, view LOD): never FINER than its receiver, coarser only where the view already
// coarsens. Written per frame by ShadowGpuData::RefreshCasterLods.
StructuredBuffer<uint>         CasterLod   : register(t5);

RWStructuredBuffer<uint> PageGroupCount  : register(u0); // per (page, group) count, doubles as the cursor
RWStructuredBuffer<uint> PageVisibleList : register(u1); // per (page, slot) caster id
RWStructuredBuffer<uint> PageScatterDyn  : register(u2); // per page: a dynamic caster landed here

// Project an AABB to an NDC rect. Ortho (clipmap) views map the 8 corners AFFINELY, so the min/max
// over them is exact. Perspective (local) views need the divide, and the box can straddle w=0 where
// no rect exists — transcribed from Unreal's BoxCullFrustumPerspective: notice the straddle and
// return the FULL rect. Returns false only when the box is entirely behind the eye.
// Positive-vertex AABB-vs-frustum test (same rule as vsm_page_setup_cs::PageIntersects). Planes are
// unnormalized straight from the matrix — the test is scale-invariant per plane.
bool BoxInPagePlanes(float4x4 vp, uint level, uint px, uint py, float3 c, float3 e)
{
    // The page's off-center projection, built exactly like the setup pass builds it.
    const float a = (float)(VSM_L0_AXIS >> level);
    const float cx = -1.0f + (2.0f * px + 1.0f) / a;
    const float cy =  1.0f - (2.0f * py + 1.0f) / a;
    const float4x4 S = float4x4(a, 0, 0, 0,
                                0, a, 0, 0,
                                0, 0, 1, 0,
                                -cx * a, -cy * a, 0, 1);
    const float4x4 pt = transpose(mul(vp, S));
    float4 planes[6];
    planes[0] = pt[3] + pt[0];
    planes[1] = pt[3] - pt[0];
    planes[2] = pt[3] + pt[1];
    planes[3] = pt[3] - pt[1];
    planes[4] = pt[2];
    planes[5] = pt[3] - pt[2];
    [unroll] for (int i = 0; i < 6; ++i)
    {
        if (dot(planes[i].xyz, c) + planes[i].w + dot(abs(planes[i].xyz), e) < 0.0f) { return false; }
    }
    return true;
}

bool ProjectAabbNdc(float4x4 vp, float3 ctr, float3 ext, bool perspective,
                    out float2 lo, out float2 hi, out bool straddles)
{
    lo = float2( 1e30f,  1e30f);
    hi = float2(-1e30f, -1e30f);
    float minW =  1e30f;
    float maxW = -1e30f;
    [unroll] for (uint k = 0u; k < 8u; ++k)
    {
        const float3 corner = ctr + ext * float3((k & 1u) ? 1.0f : -1.0f,
                                                 (k & 2u) ? 1.0f : -1.0f,
                                                 (k & 4u) ? 1.0f : -1.0f);
        const float4 clip = mul(float4(corner, 1.0f), vp);
        minW = min(minW, clip.w);
        maxW = max(maxW, clip.w);
        // The divide can produce inf/NaN when w is at or near 0 — harmless, because that is exactly
        // the straddle case below, which discards lo/hi wholesale.
        const float2 sp = perspective ? (clip.xy / clip.w) : clip.xy;
        lo = min(lo, sp);
        hi = max(hi, sp);
    }
    straddles = false;
    if (!perspective) { return true; }
    if (maxW <= 0.0f) { return false; }                              // entirely behind the eye
    if (minW <= 0.0f)
    {
        // No NDC rect exists. Unreal takes the full rect here and moves on; measured on demo.json
        // that costs +0.47 ms on Pass_VsmPageRender, because a straddling caster then appends to
        // EVERY resident page of the view (0.776 vs 0.308 for the brute-force path it replaced).
        // So the full rect becomes a page CANDIDATE SET instead of an answer: each candidate still
        // has to pass the exact per-page frustum test below. Only straddlers pay for it.
        lo = float2(-1.0f, -1.0f);
        hi = float2( 1.0f,  1.0f);
        straddles = true;
    }
    return true;
}

// numthreads(8,8,1) matches RecordComputeDispatch's group size. y indexes a SCATTER TARGET:
// [0, gNumLevels) = clipmap level, [gNumLevels, gNumLevels+gNumLocalViews) = local view.
[numthreads(8, 8, 1)]
[RootSignature(VSM_SCATTER_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint c = dtid.x;              // caster slot id (only object-leading slots do work)
    const uint target = dtid.y;
    if (c >= gNumCasters || target >= gNumLevels + gNumLocalViews) { return; }

    const uint meta = CasterMeta[c];
    const uint slots = meta >> 1;
    if (slots == 0u) { return; }        // continuation slot: its object was handled by its first slot

    const CasterBounds b = Bounds[c];
    const float3 ctr = b.center.xyz;
    const float3 ext = b.halfExtents.xyz;

    const bool isLocal = (target >= gNumLevels);
    // Clipmap views sit after the local ones in the VSM view list; a clipmap LEVEL is its own view.
    const uint view = isLocal ? (target - gNumLevels) : (VSM_NUM_LOCAL_VIEWS + target);
    // A clipmap view is one page grid (the level IS the LOD). A local view is a mip pyramid, and a
    // receiver pixel marks whichever level matches its density, so every level can be resident.
    const uint mipCount = isLocal ? (VSM_MAX_LEVEL + 1u) : 1u;

    float2 lo, hi;
    bool straddles;
    if (!ProjectAabbNdc(gViewProj[view], ctr, ext, isLocal, lo, hi, straddles)) { return; }
    if (hi.x < -1.0f || lo.x > 1.0f || hi.y < -1.0f || lo.y > 1.0f) { return; } // outside this view

    const bool isDynamic = (meta & 1u) != 0u;
    const uint targetLod = gTargetLod[target >> 2u][target & 3u]; // this view's own tier-curve LOD

    for (uint L = 0u; L < mipCount; ++L)
    {
        // NDC -> this level's page grid. y flips (NDC +y is up, page rows run down).
        const uint axisI = VSM_L0_AXIS >> L;
        const float axis = (float)axisI;
        const int x0 = (int)floor((lo.x * 0.5f + 0.5f) * axis);
        const int x1 = (int)floor((hi.x * 0.5f + 0.5f) * axis);
        const int y0 = (int)floor((0.5f - hi.y * 0.5f) * axis);
        const int y1 = (int)floor((0.5f - lo.y * 0.5f) * axis);
        const uint px0 = (uint)clamp(x0, 0, (int)axisI - 1);
        const uint px1 = (uint)clamp(x1, 0, (int)axisI - 1);
        const uint py0 = (uint)clamp(y0, 0, (int)axisI - 1);
        const uint py1 = (uint)clamp(y1, 0, (int)axisI - 1);

        for (uint py = py0; py <= py1; ++py)
        {
            for (uint pxi = px0; pxi <= px1; ++pxi)
            {
                const uint entry = PageTable[VsmPageId(view, L, pxi, py)];
                if ((entry & 0x80000000u) == 0u) { continue; }   // not resident -> nothing to draw into
                // Residency first: the exact test is only worth building for a page that exists.
                if (isLocal && !BoxInPagePlanes(gViewProj[view], L, pxi, py, ctr, ext)) { continue; }
                const uint p = entry & 0x0000FFFFu;              // physical page index

                for (uint s = 0u; s < slots; ++s)
                {
                    const uint g = CasterGroup[c + s];
                    if (g >= gNumGroups) { continue; }
                    // Per-INSTANCE caster LOD (see CasterLod above). The bucket is a VIRTUAL group
                    // g*KMAX_SHADOW_LODS + lod, so instances of one mesh at different receiver LODs
                    // land in different draw args — the per-group single LOD could not represent
                    // that, and whichever value it picked self-shadowed the instances it mismatched.
                    const uint enc = CasterLod[c + s];
                    const uint recvLod = enc & 0xFu;
                    // gPerInstanceLod == 0 is the A/B lever (--set=vsm.perInstanceCasterLod:0):
                    // every FLOOR caster falls back to the view LOD, reproducing the old
                    // per-view record layout inside THIS binary for cost attribution. Chunk
                    // EXACT stays exact in both modes (one instance per group; no splitting).
                    const uint matched = (enc & 0x80u) ? recvLod
                        : ((gPerInstanceLod != 0u) ? max(recvLod, targetLod) : targetLod);
                    const uint lod = min(matched, (uint)(KMAX_SHADOW_LODS - 1));
                    const uint vg = g * KMAX_SHADOW_LODS + lod;
                    uint rank;
                    InterlockedAdd(PageGroupCount[p * gNumGroups * KMAX_SHADOW_LODS + vg], 1u, rank);
                    // Bucket base inside this page's slice: the group's global base scaled by the
                    // bucket count, plus this LOD's sub-run (each bucket is sized for the group's
                    // WHOLE caster count — an instance can land in any one of them).
                    const uint bucketBase = PerGroup[g].x * KMAX_SHADOW_LODS + lod * PerGroup[g].w;
                    const uint vid = c + s;
                    PageVisibleList[p * gNumCasters * KMAX_SHADOW_LODS + bucketBase + rank] =
                        (gPageIdShift != 0u) ? (vid | (p << gPageIdShift)) : vid;
                }
                if (isDynamic) { PageScatterDyn[p] = 1u; }
            }
        }
    }
}
