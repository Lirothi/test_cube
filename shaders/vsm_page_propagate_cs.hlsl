// SMRT dependency -- CLIPMAP LOD FALLBACK CHAIN. Transcribed from Unreal's `PropagateMappedMips`
// (VirtualShadowMapPageManagement.usf), directional branch.
//
// THE PROBLEM IT SOLVES. A ray marched through the clipmap (vsm_smrt.hlsli) leaves the level its
// receiver picked almost immediately: the ray step in shadow UV is rayLength/levelExtent, which at
// a 12 m base extent and a ray of 1.5x distance-to-camera is about one whole UV. Every sample above
// the receiver therefore lands outside that level -- and the page request only ever marks the
// FINEST CONTAINING level (vsm_page_request_cs.hlsl), so there is no coarser page resident to
// continue in. Measured: with this chain absent, samplesPerRay 1 and 32 produce identical images,
// because exactly one sample per ray -- the one at the receiver -- is ever valid.
//
// THE FIX, WHICH IS UE'S. Do not make more pages resident. Instead give every UNMAPPED clipmap page
// a pointer to the nearest MAPPED page of a coarser level covering the same world area. The lookup
// then resolves at read time, residency is untouched, and the pool sees no extra pressure.
//
// RACE, AND WHY IT IS SAFE (UE make the same argument). Threads read other threads' entries while
// writing their own. A thread only ever READS entries that are MAPPED HERE (bit 31), and only ever
// WRITES entries that are not -- the two sets are disjoint, and a page-table entry is a single
// DWORD, so every write is atomic. The result is deterministic regardless of thread order.
//
// REBUILT EVERY FRAME, and it must be: the page table persists across frames (the pool IS the
// cache -- vsm_page_alloc_init_cs runs once), while the clipmap levels are re-snapped to the camera
// every frame and coarser pages come and go with the LRU. A fallback entry left from last frame
// points at a page that may now belong to somebody else. Every non-mapped clipmap page is therefore
// written unconditionally below, with 0 when no coarser level can serve it.
// Addressing + the page-table entry format only. Deliberately NOT vsm_page_alloc_common.hlsli:
// that header declares the allocation CB at b0, and this pass needs b0 for its own.
#include "vsm_addressing.hlsli"

#define VSM_PAGE_PROPAGATE_RS \
    "CBV(b0), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

RWStructuredBuffer<uint> PageTable : register(u0);

cbuffer VsmPropagateCB : register(b0)
{
    uint  gNumClipLevels;
    uint  gPagesPerAxis;      // VSM_L0_AXIS; clipmap levels use page level 0 (the LEVEL is the LOD)
    uint2 _padPropagate;
    // Per clipmap level, the square it covers in the FIXED light frame that Scene::UpdateClipmap
    // snaps every level in: xy = the level's texel-snapped centre projected on (right, trueUp),
    // z = the level's full extent. Levels share those axes, which is exactly what makes a
    // level-to-level page mapping plain 2D arithmetic rather than a matrix inverse.
    float4 gLevelXform[VSM_NUM_CLIPMAP_LEVELS];
}

// Page (px,py) of `level` -> the centre of that page in the light frame.
float2 VsmPageCentreLS(uint level, uint2 page)
{
    const float2 centre = gLevelXform[level].xy;
    const float  extent = gLevelXform[level].z;
    const float  radius = 0.5f * extent;
    const float2 uv = (float2(page) + 0.5f) / (float)gPagesPerAxis;
    // uv.y runs opposite to the light frame's up axis -- same flip the samplers apply
    // (uv = 0.5 - 0.5*ndc.y), and getting it wrong maps a page to its mirror image about the
    // level centre, which is a plausible-looking page that is simply the wrong part of the world.
    return float2(centre.x - radius + uv.x * extent,
                  centre.y + radius - uv.y * extent);
}

// Light-frame position -> page of `level`. Returns false when it falls outside that level.
bool VsmPageFromLS(uint level, float2 ls, out uint2 page)
{
    page = uint2(0u, 0u);
    const float2 centre = gLevelXform[level].xy;
    const float  extent = gLevelXform[level].z;
    if (extent <= 0.0f) { return false; }
    const float radius = 0.5f * extent;
    const float2 uv = float2((ls.x - (centre.x - radius)) / extent,
                             ((centre.y + radius) - ls.y) / extent);
    if (any(uv < 0.0f) || any(uv >= 1.0f)) { return false; }
    page = min((uint2)(uv * (float)gPagesPerAxis), gPagesPerAxis - 1u);
    return true;
}

[numthreads(8, 8, 1)]
[RootSignature(VSM_PAGE_PROPAGATE_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.y != 0u) { return; }

    const uint pagesPerLevel = gPagesPerAxis * gPagesPerAxis;
    const uint i = dtid.x;
    if (i >= gNumClipLevels * pagesPerLevel) { return; }

    const uint level = i / pagesPerLevel;
    const uint inLevel = i - level * pagesPerLevel;
    const uint2 page = uint2(inLevel % gPagesPerAxis, inLevel / gPagesPerAxis);

    const uint view = VSM_NUM_LOCAL_VIEWS + level;
    const uint slot = VsmPageId(view, 0u, page.x, page.y);

    // Mapped at this level: leave it completely alone. This is also the set every other thread
    // reads, which is what keeps the race benign.
    if ((PageTable[slot] & VSM_RESIDENT_BIT_C) != 0u) { return; }

    const float2 ls = VsmPageCentreLS(level, page);

    // Search outward for the first COARSER level with a mapped page over the same world area.
    // Outward only: a finer level cannot contain a point this one does not.
    uint result = 0u;
    for (uint k = 1u; k < VSM_NUM_CLIPMAP_LEVELS; ++k)
    {
        const uint coarse = level + k;
        if (coarse >= gNumClipLevels) { break; }

        uint2 coarsePage;
        if (!VsmPageFromLS(coarse, ls, coarsePage)) { continue; }

        const uint coarseSlot = VsmPageId(VSM_NUM_LOCAL_VIEWS + coarse, 0u, coarsePage.x, coarsePage.y);
        const uint coarseEntry = PageTable[coarseSlot];
        // Only a page MAPPED at that level will do. Chaining through another fallback entry would
        // read a pointer that may be being rewritten this very dispatch, and would also lose the
        // offset needed to get the depth back into the right level's range.
        if ((coarseEntry & VSM_RESIDENT_BIT_C) == 0u) { continue; }

        result = VSM_ANY_LOD_BIT
               | ((k << VSM_LOD_SHIFT) & VSM_LOD_MASK)
               | (coarseEntry & VSM_PHYS_MASK_C);
        break;
    }

    // Unconditional: 0 when nothing serves this page, which is what clears a stale pointer left by
    // a previous frame.
    PageTable[slot] = result;
}
