// Rung 2 VSM virtual-page addressing shared by the request / setup / sampling shaders. Mirrors
// the vsm:: constants in VirtualShadowMap.h. A virtual page id is view*kPagesPerView + the
// per-mip-level offset + py*axis + px; a resident page maps (via the page table) to a physical
// page in the pool (VSM_POOL_PAGES_AXIS² pages of VSM_PAGE_SIZE² texels).
#ifndef VSM_ADDRESSING_HLSLI
#define VSM_ADDRESSING_HLSLI

static const uint VSM_MAX_VIEWS      = 42u;  // kMaxVirtualViews (32 local spots+point faces + 10 clipmap levels)
// ---- PAGE TABLE ENTRY FORMAT (the one definition; vsm_page_alloc_common.hlsli includes this) ----
//
//   bit 31      : MAPPED HERE. A physical page is allocated for this exact virtual page, and the
//                 page render writes into it. Every pass that RENDERS reads only this bit.
//   bit 30      : RESOLVABLE. This lookup can be served, possibly by a COARSER clipmap level whose
//                 page covers the same world area. Set on mapped pages too (mapped implies
//                 resolvable), so a sampler tests one bit.
//   bits 16..19 : LOD OFFSET. 0 when mapped here; k>0 means "the physical page below belongs to
//                 clipmap level (this level + k)". Only ever non-zero when bit 31 is clear.
//   bits 0..15  : physical page index (0..kPoolPageCount-1)
//
// The split into two bits is Unreal's (`bThisLODValidForRendering` / `bAnyLODValid`,
// VirtualShadowMapPageAccessCommon.ush) and it is what makes the fallback additive: nothing that
// existed before this chain looks at bit 30, so nothing that renders changed behaviour.
static const uint VSM_RESIDENT_BIT_C = 0x80000000u; // mapped at THIS level (render + legacy sample)
static const uint VSM_ANY_LOD_BIT    = 0x40000000u; // resolvable here or at a coarser clipmap level
static const uint VSM_LOD_SHIFT      = 16u;
static const uint VSM_LOD_MASK       = 0x000F0000u; // 4 bits: offsets 0..15 (10 clipmap levels)
static const uint VSM_PHYS_MASK_C    = 0x0000FFFFu;

static const uint VSM_NUM_LOCAL_VIEWS = 32u; // kNumLocalVirtualViews — views [0,32) local; [32,40) directional clipmap
static const uint VSM_NUM_CLIPMAP_LEVELS = 10u; // kNumClipmapLevels (VSM_MAX_VIEWS - VSM_NUM_LOCAL_VIEWS)
static const float VSM_VIRTUAL_RES = 2048.0f;  // kVirtualRes — texels per clipmap level edge (for texel-scaled bias)
static const uint VSM_L0_AXIS        = 16u;  // kVirtualPagesL0Axis (kVirtualRes / kPageSize)
static const uint VSM_MAX_LEVEL      = 4u;   // kMaxMipLevel
static const uint VSM_PAGES_PER_VIEW = 341u; // kPagesPerView (16²+8²+4²+2²+1)
static const uint VSM_LEVEL_OFFSET[6] = { 0u, 256u, 320u, 336u, 340u, 341u };
static const uint VSM_POOL_PAGES_AXIS = 32u; // kPoolPagesPerAxis (kPoolTexels / kPageSize)
static const uint VSM_PAGE_SIZE       = 128u;

// Distance-based mip select (mirrors vsm_page_request_cs): coarser level with camera distance.
uint VsmSelectLevel(float distCam, float refDist, uint maxLevel)
{
    int raw = (int)floor(log2(max(distCam, 1e-3f) / max(refDist, 1e-3f)));
    return (uint)clamp(raw, 0, (int)maxLevel);
}

// (view, level, px, py) -> virtual page id.
uint VsmPageId(uint view, uint level, uint px, uint py)
{
    uint axis = VSM_L0_AXIS >> level;
    return view * VSM_PAGES_PER_VIEW + VSM_LEVEL_OFFSET[level] + py * axis + px;
}

// Virtual page id -> (view, level, px, py).
void VsmDecodePage(uint pageId, out uint view, out uint level, out uint px, out uint py)
{
    view = pageId / VSM_PAGES_PER_VIEW;
    uint inView = pageId - view * VSM_PAGES_PER_VIEW;
    level = 0u;
    [unroll] for (uint L = 0u; L < 5u; ++L)
    {
        if (inView >= VSM_LEVEL_OFFSET[L] && inView < VSM_LEVEL_OFFSET[L + 1u]) { level = L; }
    }
    uint local = inView - VSM_LEVEL_OFFSET[level];
    uint axis = VSM_L0_AXIS >> level;
    px = local % axis;
    py = local / axis;
}

#endif // VSM_ADDRESSING_HLSLI
