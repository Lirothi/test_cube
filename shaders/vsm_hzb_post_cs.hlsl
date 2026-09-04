// Occlusion plan S5b.2: the POST pass of the VSM two-pass light-space occlusion. The scatter
// (vsm_page_scatter_cs.hlsl) deferred every (caster, clipmap page) pair that LAST frame's pool
// pyramid hid; pass A drew the rest; the pyramid was rebuilt from the pages pass A rendered. Now
// each deferred pair is retested against THAT pyramid with THIS frame's page projection: still
// hidden -> not drawn at all; visible -> appended into the page's visible-list slice right after
// pass A's entries of the same bucket, counted in PageGroupCountB, and drawn by pass B (the setup
// CS in its pass-B mode turns the counts into args, StartInstance = bucket base + pass A's count).
//
// Same bucket arithmetic as the scatter, so both passes' entries of a (page, virtual group) are one
// contiguous run: [A entries][B entries], each bucket sized for the group's whole caster count,
// and a caster lands in exactly one of A or B per page -- the run never overflows the bucket.
#pragma pack_matrix(row_major)
#include "vsm_addressing.hlsli"
#include "hzb_cull.hlsli"

#define VSM_HZB_POST_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=10, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

#define KMAX_SHADOW_LODS 4 // matches render::kMaxShadowLods

cbuffer PostCB : register(b0)
{
    uint gNumCasters;     // active caster SLOT count (the per-page slice stride)
    uint gNumGroups;
    uint gNumLevels;      // active clipmap levels (target index arithmetic)
    uint gPageIdShift;    // single-draw packing of the list entry (never 0 here: pass B needs single-draw)
    uint gPerInstanceLod; // mirrors the scatter's gPerInstanceLod
    uint gDeferredCap;    // capacity of DeferredPairs (the scatter counted past it = treated visible)
    uint2 _pad0;
    float4x4 gViewProj[VSM_MAX_VIEWS];             // THIS frame's, same indexing as the scatter
    uint4 gTargetLod[(VSM_MAX_VIEWS + 3) / 4];     // per scatter target, as the scatter
};

struct CasterBounds { float4 center; float4 halfExtents; };

StructuredBuffer<CasterBounds> Bounds         : register(t0);
StructuredBuffer<uint>         CasterGroup    : register(t1);
StructuredBuffer<uint>         CasterMeta     : register(t2); // bits1+ = slot count on the leading slot
StructuredBuffer<uint>         PageTable      : register(t3); // THIS frame's mapping
StructuredBuffer<uint4>        PerGroup       : register(t4); // .x global base, .w group caster count
StructuredBuffer<uint>         CasterLod      : register(t5);
StructuredBuffer<uint2>        DeferredPairs  : register(t6); // (leading caster slot, virtual page id)
StructuredBuffer<uint>         PageGroupCount : register(t7); // pass A's counts (final after the scatter)
StructuredBuffer<uint>         PerPageDirty   : register(t8); // pages rendered this frame
Texture2D<float>               VsmHzb         : register(t9); // the pool pyramid after pass A

RWStructuredBuffer<uint> PageGroupCountB : register(u0); // per (page, virtual group) pass-B count / cursor
RWStructuredBuffer<uint> PageVisibleList : register(u1); // the shared per-page slices
RWStructuredBuffer<uint> HzbCounters     : register(u2); // [0] deferred pairs, [1] drawn in B, [2] overflow

static const uint kHzbMip0 = VSM_POOL_PAGES_AXIS * VSM_PAGE_SIZE / 2u; // 2048

static const float4x4 kIdentity = float4x4(1, 0, 0, 0,
                                           0, 1, 0, 0,
                                           0, 0, 1, 0,
                                           0, 0, 0, 1);
// Forward-Z page projection -> the reverse-Z the library expects: z' = w - z (w == 1, ortho).
static const float4x4 kFlipZ = float4x4(1, 0, 0, 0,
                                        0, 1, 0, 0,
                                        0, 0, -1, 0,
                                        0, 0, 1, 1);

// The page's off-center projection, built exactly like the setup / scatter passes build it.
float4x4 PageWorldToClip(float4x4 vp, uint level, uint px, uint py)
{
    const float a = (float)(VSM_L0_AXIS >> level);
    const float cx = -1.0f + (2.0f * px + 1.0f) / a;
    const float cy =  1.0f - (2.0f * py + 1.0f) / a;
    const float4x4 S = float4x4(a, 0, 0, 0,
                                0, a, 0, 0,
                                0, 0, 1, 0,
                                -cx * a, -cy * a, 0, 1);
    return mul(mul(vp, S), kFlipZ);
}

[numthreads(8, 8, 1)]
[RootSignature(VSM_HZB_POST_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.y != 0u) { return; }
    const uint i = dtid.x;
    if (i >= min(HzbCounters[0], gDeferredCap)) { return; }
    const uint2 pair = DeferredPairs[i];
    const uint c = pair.x;
    const uint pageId = pair.y;
    if (c >= gNumCasters) { return; }

    const uint entry = PageTable[pageId];
    if ((entry & 0x80000000u) == 0u) { return; }   // unmapped since the scatter: cannot happen in-frame
    const uint p = entry & 0x0000FFFFu;
    if (PerPageDirty[p] == 0u) { return; }         // a cached page draws nothing this frame

    uint view, level, px, py;
    VsmDecodePage(pageId, view, level, px, py);
    if (view < VSM_NUM_LOCAL_VIEWS) { return; }    // clipmap pages only (the scatter defers no local pair)

    const CasterBounds b = Bounds[c];
    const float3 ctr = b.center.xyz;
    const float3 ext = b.halfExtents.xyz;

    // Nanite's post rule (NaniteCullingCommon.ush:483-497): the frustum again with this frame's
    // matrices, the near crossing = visible, otherwise the depth test against the fresh pyramid.
    const HzbFrustumCull cur = HzbBoxCullFrustumOrtho(ctr, ext, kIdentity, PageWorldToClip(gViewProj[view], level, px, py), false, false);
    if (!cur.isVisible) { return; }
    if (!cur.crossesNearPlane)
    {
        const int ox = (int)((p % VSM_POOL_PAGES_AXIS) * VSM_PAGE_SIZE);
        const int oy = (int)((p / VSM_POOL_PAGES_AXIS) * VSM_PAGE_SIZE);
        const HzbScreenRect rect = HzbGetScreenRect(int4(ox, oy, ox + (int)VSM_PAGE_SIZE, oy + (int)VSM_PAGE_SIZE),
                                                    cur.rectMin, cur.rectMax, 4);
        if (!HzbIsVisible(VsmHzb, uint2(kHzbMip0, kHzbMip0), rect)) { return; }
    }

    // Visible after all: every slot of the object into pass B, the scatter's bucket arithmetic.
    const uint slots = CasterMeta[c] >> 1;
    const uint target = view - VSM_NUM_LOCAL_VIEWS;   // clipmap level == scatter target
    const uint targetLod = gTargetLod[target >> 2u][target & 3u];
    const uint numVg = gNumGroups * KMAX_SHADOW_LODS;
    const uint pageBase = p * gNumCasters * KMAX_SHADOW_LODS;
    for (uint s = 0u; s < slots; ++s)
    {
        const uint g = CasterGroup[c + s];
        if (g >= gNumGroups) { continue; }
        const uint enc = CasterLod[c + s];
        const uint recvLod = enc & 0xFu;
        const uint matched = (enc & 0x80u) ? recvLod
            : ((gPerInstanceLod != 0u) ? max(recvLod, targetLod) : targetLod);
        const uint lod = min(matched, (uint)(KMAX_SHADOW_LODS - 1));
        const uint vg = g * KMAX_SHADOW_LODS + lod;
        uint rank;
        InterlockedAdd(PageGroupCountB[p * numVg + vg], 1u, rank);
        const uint bucketBase = PerGroup[g].x * KMAX_SHADOW_LODS + lod * PerGroup[g].w;
        const uint vid = c + s;
        PageVisibleList[pageBase + bucketBase + PageGroupCount[p * numVg + vg] + rank] =
            (gPageIdShift != 0u) ? (vid | (p << gPageIdShift)) : vid;
    }
    uint drawn;
    InterlockedAdd(HzbCounters[1], 1u, drawn);
}
