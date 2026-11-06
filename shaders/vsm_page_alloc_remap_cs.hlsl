// Local-light shadow plan S10c (UE's per-light VSM cache: a light whose virtual shadow map id changes keeps its pages,
// VirtualShadowMapCacheManager remaps the previous id's physical pages to the new one). Ours: a VSM view is a light's
// shadow SLOT, and a slot can change when the shadowed set does (LightManager's sticky slots move at most the last
// light into a leaver's hole). One thread per PHYSICAL page, two passes, before the touch pass:
//   pass 0 -- a page whose view moves: clear its old page-table entry; a page of a view another light moves INTO (its
//             own light left): free it -- it would share a virtual page with an arrival;
//   pass 1 -- a moving page: owned under its new view, its entry written there (resident, as the allocate pass writes
//             it), and its last-frame owner moved along, so the page cache sees it as cached, not new.
// The page table is updated only for pages that move; every other view keeps its pages, entries and cache untouched.
//
// A clipmap level's pan goes the same way (UE's VSM_NEXT_FLAG_KEEP_PAGE, PageAddressOffset: VirtualShadowMapCacheManager.cpp:297-303).
// A level's centre snaps to multiples of its radius, 32 pages, so its texel grid keeps its world place: a page keeps
// its depth and only changes its address. bits.z of a panned level holds the move, kPanNone otherwise; a page that leaves the grid goes.
#include "vsm_page_alloc_common.hlsli"

#define VSM_PAGE_ALLOC_REMAP_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

static const uint kRemapKeep = 0xFFFFFFFFu;
static const uint kRemapDrop = 0xFFFFFFFEu;
static const uint kPanNone = 0u;    // bits.z: not panned
static const uint kPanFlag = 1u << 31u; // bits.z: (signed 15-bit dx + 16384) | (dy + 16384) << 15 | flag

cbuffer VsmRemapCB : register(b0)
{
    uint gRemapNumPages; // kPoolPageCount
    uint gRemapPass;     // 0 = clear the moving entries + free the overrun views, 1 = re-own the moving pages
    uint gRemapNumViews; // plan S9e: the views the page table holds (VirtualShadowMap's view count)
    uint gRemapPad;
};

// Plan S9e: the request slot's records; bits.w = per OLD view: kRemapKeep, kRemapDrop, or its new view.
StructuredBuffer<VsmViewData> Views    : register(t0);
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
    const uint view = VsmViewOfPage(owner);
    if (view >= gRemapNumViews) { return; }
    if (VsmIsClipmapView(view))
    {
        const uint pan = Views[view].bits.z;
        if ((pan & kPanFlag) == 0u) { return; }
        if (gRemapPass == 0u)
        {
            PageTable[owner] = 0u; // every old entry first, so no move can land on a page not moved yet
            return;
        }
        const uint inLevel = owner - VsmViewBase(view);
        const int x = (int)(inLevel % VSM_CLIPMAP_AXIS) + (int)(pan & 0x7FFFu) - 16384;
        const int y = (int)(inLevel / VSM_CLIPMAP_AXIS) + (int)((pan >> 15u) & 0x7FFFu) - 16384;
        if (x < 0 || y < 0 || x >= (int)VSM_CLIPMAP_AXIS || y >= (int)VSM_CLIPMAP_AXIS)
        {
            PhysOwner[p] = VSM_INVALID; // left the grid: the free-list pass hands it out again
            return;
        }
        const uint moved = VsmViewBase(view) + (uint)y * VSM_CLIPMAP_AXIS + (uint)x;
        PhysOwner[p] = moved;
        PageTable[moved] = VSM_RESIDENT_BIT | VSM_ANY_LOD_BIT | (p & VSM_PHYS_MASK);
        if (PhysOwnerPrev[p] == owner) { PhysOwnerPrev[p] = moved; }
        return;
    }
    const uint remap = Views[view].bits.w;
    if (remap == kRemapKeep) { return; }

    if (gRemapPass == 0u)
    {
        PageTable[owner] = 0u;
        if (remap == kRemapDrop) { PhysOwner[p] = VSM_INVALID; } // the free-list pass hands it out again
        return;
    }
    if (remap == kRemapDrop || remap >= gRemapNumViews) { return; }
    const uint moved = VsmViewBase(remap) + (owner - VsmViewBase(view));
    PhysOwner[p] = moved;
    PageTable[moved] = VSM_RESIDENT_BIT | VSM_ANY_LOD_BIT | (p & VSM_PHYS_MASK);
    if (PhysOwnerPrev[p] == owner) { PhysOwnerPrev[p] = moved; }
}
