// Local-light shadow plan S10c (UE's per-light VSM cache: a light whose virtual shadow map id changes keeps its pages,
// VirtualShadowMapCacheManager remaps the previous id's physical pages to the new one). Ours: a VSM view is a light's
// shadow SLOT, and a slot can change when the shadowed set does (LightManager's sticky slots move at most the last
// light into a leaver's hole). One thread per PHYSICAL page, two passes, before the touch pass:
//   pass 0 -- a page whose view moves: clear its old page-table entry; a page of a view another light moves INTO (its
//             own light left): free it -- it would share a virtual page with an arrival;
//   pass 1 -- a moving page: owned under its new view, its entry written there (resident, as the allocate pass writes
//             it), and its last-frame owner moved along, so the page cache sees it as cached, not new.
// The page table is updated only for pages that move; every other view keeps its pages, entries and cache untouched.
#include "vsm_page_alloc_common.hlsli"

#define VSM_PAGE_ALLOC_REMAP_RS \
    "CBV(b0), " \
    "DescriptorTable(UAV(u0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

static const uint kRemapKeep = 0xFFFFFFFFu;
static const uint kRemapDrop = 0xFFFFFFFEu;

cbuffer VsmRemapCB : register(b0)
{
    uint gRemapNumPages; // kPoolPageCount
    uint gRemapPass;     // 0 = clear the moving entries + free the overrun views, 1 = re-own the moving pages
    uint2 gRemapPad;
    uint4 gViewRemap[(VSM_MAX_VIEWS + 3u) / 4u]; // per OLD view: kRemapKeep, kRemapDrop, or its new view
};

RWStructuredBuffer<uint> PhysOwner     : register(u0); // physical -> virtual owner / VSM_INVALID (rw)
RWStructuredBuffer<uint> PageTable     : register(u1); // virtual -> packed entry (rw)
RWStructuredBuffer<uint> PhysOwnerPrev : register(u2); // physical -> last frame's owner (the page cache's isNew)

[numthreads(8, 8, 1)]
[RootSignature(VSM_PAGE_ALLOC_REMAP_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.y != 0u) { return; }
    const uint p = dtid.x;
    if (p >= gRemapNumPages) { return; }

    const uint owner = PhysOwner[p];
    if (owner == VSM_INVALID) { return; }
    const uint view = owner / VSM_PAGES_PER_VIEW;
    if (view >= VSM_MAX_VIEWS) { return; }
    const uint remap = gViewRemap[view >> 2u][view & 3u];
    if (remap == kRemapKeep) { return; }

    if (gRemapPass == 0u)
    {
        PageTable[owner] = 0u;
        if (remap == kRemapDrop) { PhysOwner[p] = VSM_INVALID; } // the free-list pass hands it out again
        return;
    }
    if (remap == kRemapDrop || remap >= VSM_MAX_VIEWS) { return; }
    const uint moved = remap * VSM_PAGES_PER_VIEW + (owner - view * VSM_PAGES_PER_VIEW);
    PhysOwner[p] = moved;
    PageTable[moved] = VSM_RESIDENT_BIT | VSM_ANY_LOD_BIT | (p & VSM_PHYS_MASK);
    if (PhysOwnerPrev[p] == owner) { PhysOwnerPrev[p] = moved; }
}
