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
// Local (spot/point) views are PERSPECTIVE — an AABB's NDC rect is not well defined when the box
// crosses the near plane — so they are NOT scattered here and keep the brute-force per-page path.
#pragma pack_matrix(row_major)
#include "vsm_addressing.hlsli"

#define VSM_SCATTER_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=5, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer ScatterCB : register(b0)
{
    uint gNumCasters;   // active caster SLOT count (also the per-page slice stride)
    uint gNumGroups;    // mesh-group count
    uint gNumLevels;    // active clipmap levels (VSM_NUM_CLIPMAP_LEVELS)
    // Single-draw page render: see the identically-named field in vsm_page_setup_cs.hlsl's SetupCB.
    // Both writers of PageVisibleList must pack identically, so this is fed from the same CPU value.
    uint gPageIdShift;
    float4x4 gClipViewProj[VSM_NUM_CLIPMAP_LEVELS]; // per clipmap level (VSM view 32+L)
};

struct CasterBounds { float4 center; float4 halfExtents; };

StructuredBuffer<CasterBounds> Bounds      : register(t0); // per-caster world AABB (unified buffer)
StructuredBuffer<uint>         CasterGroup : register(t1); // per-caster mesh-group id
StructuredBuffer<uint>         CasterMeta  : register(t2); // bit0 = dynamic, bits1+ = slot count on the FIRST slot
StructuredBuffer<uint>         PageTable   : register(t3); // virtual page -> resident bit | physical index
// Global base of each group's run inside every page's slice (prefix sum of per-group TOTAL caster
// counts) — Rung 0's PerGroup.x. A buffer rather than a CB array so it needs no group-count cap.
StructuredBuffer<uint4>        PerGroup    : register(t4);

RWStructuredBuffer<uint> PageGroupCount  : register(u0); // per (page, group) count, doubles as the cursor
RWStructuredBuffer<uint> PageVisibleList : register(u1); // per (page, slot) caster id
RWStructuredBuffer<uint> PageScatterDyn  : register(u2); // per page: a dynamic caster landed here

// numthreads(8,8,1) matches RecordComputeDispatch's group size; y spans the 8 clipmap levels exactly.
[numthreads(8, 8, 1)]
[RootSignature(VSM_SCATTER_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint c = dtid.x;              // caster slot id (only object-leading slots do work)
    const uint level = dtid.y;          // clipmap level == VSM view (VSM_NUM_LOCAL_VIEWS + level)
    if (c >= gNumCasters || level >= gNumLevels) { return; }

    const uint meta = CasterMeta[c];
    const uint slots = meta >> 1;
    if (slots == 0u) { return; }        // continuation slot: its object was handled by its first slot

    const CasterBounds b = Bounds[c];
    const float3 ctr = b.center.xyz;
    const float3 ext = b.halfExtents.xyz;

    // Project the AABB into this level's clip space and take the NDC bounding rect. The clipmap
    // views are ORTHOGRAPHIC, so w is constant and the 8 corners map affinely — the min/max over
    // them is exact and conservative (a superset is always safe: a page may draw a caster that turns
    // out not to touch it, which the rasterizer then discards).
    const float4x4 vp = gClipViewProj[level];
    float2 lo = float2( 1e30f,  1e30f);
    float2 hi = float2(-1e30f, -1e30f);
    [unroll] for (uint k = 0u; k < 8u; ++k)
    {
        const float3 corner = ctr + ext * float3((k & 1u) ? 1.0f : -1.0f,
                                                 (k & 2u) ? 1.0f : -1.0f,
                                                 (k & 4u) ? 1.0f : -1.0f);
        const float4 clip = mul(float4(corner, 1.0f), vp);
        lo = min(lo, clip.xy);
        hi = max(hi, clip.xy);
    }
    if (hi.x < -1.0f || lo.x > 1.0f || hi.y < -1.0f || lo.y > 1.0f) { return; } // outside this level

    // NDC -> page grid. Clipmap views use level 0 of the mip chain only (the clipmap LEVEL is the
    // LOD), so the grid is VSM_L0_AXIS x VSM_L0_AXIS. y flips (NDC +y is up, page rows run down).
    const float axis = (float)VSM_L0_AXIS;
    const int x0 = (int)floor((lo.x * 0.5f + 0.5f) * axis);
    const int x1 = (int)floor((hi.x * 0.5f + 0.5f) * axis);
    const int y0 = (int)floor((0.5f - hi.y * 0.5f) * axis);
    const int y1 = (int)floor((0.5f - lo.y * 0.5f) * axis);
    const uint px0 = (uint)clamp(x0, 0, (int)VSM_L0_AXIS - 1);
    const uint px1 = (uint)clamp(x1, 0, (int)VSM_L0_AXIS - 1);
    const uint py0 = (uint)clamp(y0, 0, (int)VSM_L0_AXIS - 1);
    const uint py1 = (uint)clamp(y1, 0, (int)VSM_L0_AXIS - 1);

    const uint view = VSM_NUM_LOCAL_VIEWS + level;
    const bool isDynamic = (meta & 1u) != 0u;

    for (uint py = py0; py <= py1; ++py)
    {
        for (uint pxi = px0; pxi <= px1; ++pxi)
        {
            const uint entry = PageTable[VsmPageId(view, 0u, pxi, py)];
            if ((entry & 0x80000000u) == 0u) { continue; }   // not resident -> nothing to draw into
            const uint p = entry & 0x0000FFFFu;              // physical page index

            for (uint s = 0u; s < slots; ++s)
            {
                const uint g = CasterGroup[c + s];
                if (g >= gNumGroups) { continue; }
                uint rank;
                InterlockedAdd(PageGroupCount[p * gNumGroups + g], 1u, rank);
                // Global group base inside this page's slice (see SLICE LAYOUT above).
                const uint vid = c + s;
                PageVisibleList[p * gNumCasters + PerGroup[g].x + rank] =
                    (gPageIdShift != 0u) ? (vid | (p << gPageIdShift)) : vid;
            }
            if (isDynamic) { PageScatterDyn[p] = 1u; }
        }
    }
}
