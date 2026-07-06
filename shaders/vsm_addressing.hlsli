// Rung 2 VSM virtual-page addressing shared by the request / setup / sampling shaders. Mirrors
// the vsm:: constants in VirtualShadowMap.h. A virtual page id is view*kPagesPerView + the
// per-mip-level offset + py*axis + px; a resident page maps (via the page table) to a physical
// page in the pool (VSM_POOL_PAGES_AXIS² pages of VSM_PAGE_SIZE² texels).
#ifndef VSM_ADDRESSING_HLSLI
#define VSM_ADDRESSING_HLSLI

static const uint VSM_MAX_VIEWS      = 40u;  // kMaxVirtualViews (32 local spots+point faces + 8 clipmap levels, Step 24d)
static const uint VSM_NUM_LOCAL_VIEWS = 32u; // kNumLocalVirtualViews — views [0,32) local; [32,40) directional clipmap
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
