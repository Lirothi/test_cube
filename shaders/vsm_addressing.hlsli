// Rung 2 VSM virtual-page addressing shared by the request / setup / sampling shaders. Mirrors
// the vsm:: constants in VirtualShadowMap.h. A virtual page id is VsmViewBase(view) + the
// per-mip-level offset + py*axis + px; a resident page maps (via the page table) to a physical
// page in the pool (VSM_POOL_PAGES_X x VSM_POOL_PAGES_Y pages of VSM_PAGE_SIZE² texels).
#ifndef VSM_ADDRESSING_HLSLI
#define VSM_ADDRESSING_HLSLI

// Local-light plan S10a: the view counts come from C++ (Material.cpp EngineShaderDefines, from render::kShadowView*),
// never as literals here -- a shader compiled without them fails loudly instead of addressing a stale layout.
#if !defined(TC_VSM_PREFIX_SPOTS) || !defined(TC_VSM_CLIPMAP_FIRST) || !defined(TC_VSM_CLIPMAP_LEVELS) || \
    !defined(TC_SHADOW_VIEW_CEILING) || !defined(TC_SHADOW_CASCADE_VIEWS)
#error "TC_VSM_* layout defines missing: compile through Material (EngineShaderDefines)"
#endif
// Plan S10b: VSM's views are the cull layout minus its cascades (vsm::SpotView / PointFaceView / ClipmapView):
// [prefix spots | prefix point faces | clipmap levels | the other spots | the other point faces].
static const uint VSM_PREFIX_SPOTS       = TC_VSM_PREFIX_SPOTS;               // render::kShadowViewVsmSpots
static const uint VSM_CLIPMAP_FIRST      = TC_VSM_CLIPMAP_FIRST;              // vsm::kClipmapFirstView
static const uint VSM_NUM_CLIPMAP_LEVELS = TC_VSM_CLIPMAP_LEVELS;             // vsm::kNumClipmapLevels
static const uint VSM_CLIPMAP_END        = VSM_CLIPMAP_FIRST + VSM_NUM_CLIPMAP_LEVELS;
// Plan S9e: the view count grows at run time (vsm::MaxVirtualViews, the shadow capacity) -- a light's view comes with
// the light (SpotLightGpu / PointLightGpu shadowParams4.z, vsm::SpotView / PointFaceView), never from a count here. This
// is only the literal every loop over views is bounded by (vsm::kVirtualViewCeiling); the CB's count stops it.
static const uint VSM_VIEW_CEILING       = TC_SHADOW_VIEW_CEILING - TC_SHADOW_CASCADE_VIEWS;
bool VsmIsClipmapView(uint view) { return view >= VSM_CLIPMAP_FIRST && view < VSM_CLIPMAP_END; }
uint VsmLocalView(uint i) { return (i < VSM_CLIPMAP_FIRST) ? i : i + VSM_NUM_CLIPMAP_LEVELS; } // the i-th local view

// Plan S9e: one record per VSM view (vsm::ViewGpuData, 192 bytes), in a StructuredBuffer the CPU uploads twice a frame
// -- the request pass's and the render pass's -- instead of CB arrays a view count could outgrow (a CB holds 64 KB).
// Explicitly row_major: the files that include this differ in their pack_matrix.
static const uint VSM_VIEW_DIRTY          = 1u; // bits.x: the view's matrix changed this frame (the page cache)
static const uint VSM_VIEW_DISTANT        = 2u; // plan S10d: a distant light's view (rigid, round-robin redraw)
static const uint VSM_VIEW_DISTANT_UPDATE = 4u; // ...the round robin redraws it this frame
static const uint VSM_VIEW_WIND_RIGID     = 8u; // render::g_localShadowWindRange: a local light past it -- no sway, no wind dirt
// The per-caster meta word (ShadowGpuData::kCasterMeta*): bit 0 = dynamic, bit 1 = the object sways in the wind, bits 2.. =
// the object's slot count, on its first slot only (0 = a continuation slot).
static const uint VSM_CASTER_META_DYNAMIC    = 1u;
static const uint VSM_CASTER_META_WIND       = 2u;
static const uint VSM_CASTER_META_SLOT_SHIFT = 2u;
struct VsmViewData
{
    row_major float4x4 viewProj;     // this frame's
    row_major float4x4 prevViewProj; // render slot: last frame's (the scatter's prev-pyramid test)
    float4 params; // x = valid (0/1), y = a point face's cube face (0..5; -1 = a spot / clipmap), z = zFar,
                   // w = the projection's x scale (S10e: the pixel footprint)
    float4 cull;   // plan S10e: the light's position + reach (w < 0 = none)
    float4 mip;    // plan S10e: x = the light's mip bias on top of the frame's (UE's ResolutionLodBiasLocalMoving)
    uint4  bits;   // render slot: x = VSM_VIEW_* flags, y = the view's shadow LOD (its cull view's tier curve);
                   // request slot: w = the page remap (S10c: keep / drop / the new view)
};
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
static const uint VSM_LOD_MASK       = 0x001F0000u; // 5 bits: offsets 0..16 (17 clipmap levels)
static const uint VSM_PHYS_MASK_C    = 0x0000FFFFu;

// A LOCAL view (a spot, a point face): UE's 16384 texels at mip 0 (VSM_VIRTUAL_MAX_RESOLUTION_XY), eight mips.
static const float VSM_VIRTUAL_RES = 16384.0f; // kVirtualRes -- texels per local view edge (for texel-scaled bias)
static const uint VSM_L0_AXIS        = 128u;   // kVirtualPagesL0Axis (kVirtualRes / kPageSize)
static const uint VSM_MAX_LEVEL      = 7u;     // kMaxMipLevel
static const uint VSM_PAGES_PER_VIEW = 21845u; // kPagesPerView (128^2 + 64^2 + ... + 1)
static const uint VSM_LEVEL_OFFSET[9] = { 0u, 16384u, 20480u, 21504u, 21760u, 21824u, 21840u, 21844u, 21845u };
// A clipmap LEVEL: UE's 16384 texels (VSM_VIRTUAL_MAX_RESOLUTION_XY), 128 x 128 pages, no mips: the level IS the LOD.
static const float VSM_CLIPMAP_VIRTUAL_RES = 16384.0f; // kClipmapVirtualRes
static const uint VSM_CLIPMAP_AXIS   = 128u;   // kClipmapPagesAxis
static const uint VSM_CLIPMAP_PAGES  = 16384u; // kClipmapPages
// The pool: UE's layout, its row a power of two (VirtualShadowMapArray.cpp:1006-1044).
static const uint VSM_POOL_PAGES_X    = 128u;  // kPoolPagesX
static const uint VSM_POOL_PAGES_Y    = 16u;   // kPoolPagesY
static const uint VSM_POOL_PAGES      = 2048u; // kPoolPageCount
static const uint VSM_PAGE_SIZE       = 128u;
// The water's caster mask: a level's sixteenth a bit (vsm_water_mask_cs.hlsl); 8 words a level. vsm::kWaterMaskAxis.
static const uint VSM_WATER_MASK_AXIS = 16u;
// One row of a two-dimensional dispatch that clears something longer than 65535 groups of 64 reach. vsm::kClearRowGroups x 64.
static const uint VSM_CLEAR_ROW_THREADS = 262144u;

// Where a view has pages (vsm_page_bounds_cs.hlsl): per (view, mip) four words, [min x, min y, max x, max y] (nothing mapped: min past max);
// and per view one entry more, its first word not 0 when the view has a page. A clipmap level has its mip 0. vsm::LevelBoundsWords.
static const uint VSM_BOUNDS_ENTRIES_PER_VIEW = VSM_MAX_LEVEL + 2u;
static const uint VSM_BOUNDS_ANY = VSM_MAX_LEVEL + 1u;
uint VsmBoundsIndex(uint view, uint mip) { return (view * VSM_BOUNDS_ENTRIES_PER_VIEW + mip) * 4u; }

// Where a READ starts by the camera's distance (the fog, glass: no pixel footprint to go by): it walks to coarser mips from there, so
// starting finer than anyone asked costs table reads and nothing else.
uint VsmSelectLevel(float distCam, float refDist, uint maxLevel)
{
    int raw = (int)floor(log2(max(distCam, 1e-3f) / max(refDist, 1e-3f)));
    return (uint)clamp(raw, 0, (int)maxLevel);
}
// The same distance as a MARK (vsm.localMipFromPixel 0, the A/B): the law was written for 2048 texels at mip 0, which is mip 3 now.
static const uint VSM_DISTANCE_MARK_SHIFT = 3u; // vsm::kDistanceMarkShift
uint VsmSelectMarkLevel(float distCam, float refDist)
{
    const int raw = (int)floor(log2(max(distCam, 1e-3f) / max(refDist, 1e-3f))) + (int)VSM_DISTANCE_MARK_SHIFT;
    return (uint)clamp(raw, 0, (int)VSM_MAX_LEVEL);
}

// Local-light plan S10e: UE's local-light mip (VirtualShadowMapProjectionCommon.ush:96-118, VirtualShadowMapDefinitions.h
// GetMipLevelLocal). `pixelRadiusWorld` = the camera pixel's radius at the receiver (view depth x
// min(1 / (W P00), 1 / (H P11)), the RENDER resolution); projected into the light's view by its projection's x scale
// over the receiver's depth there, in texels x 2 (UE's 2 x virtual resolution over NDC); mip = floor(log2 + bias),
// clamped to the chain. `lodBias` = r.Shadow.Virtual.ResolutionLodBiasLocal + the dynamic-resolution bias.
uint VsmLocalMipFromPixel(float pixelRadiusWorld, float lightXScale, float lightDepth, float lodBias)
{
    const float footprint = abs(pixelRadiusWorld * lightXScale / max(lightDepth, 1e-4f)) * (2.0f * VSM_VIRTUAL_RES);
    const float mipLevel = log2(max(footprint, 1e-8f)) + lodBias;
    return (uint)min(max(floor(mipLevel), 0.0f), (float)VSM_MAX_LEVEL);
}

// The D3D cube face (+X,-X,+Y,-Y,+Z,-Z) a direction from a point light falls in -- its major axis, ties to x then y
// (VsmPointFaceProject samples by the same rule, plan S10e's request marks by it).
uint VsmCubeFace(float3 d)
{
    const float3 ad = abs(d);
    if (ad.x >= ad.y && ad.x >= ad.z) { return (d.x >= 0.0f) ? 0u : 1u; }
    if (ad.y >= ad.z) { return (d.y >= 0.0f) ? 2u : 3u; }
    return (d.z >= 0.0f) ? 4u : 5u;
}

// The page table: [prefix local views | clipmap levels | the other local views], so the first id of a view is arithmetic
// (vsm::ViewBase mirrors it). THE one place that knows.
uint VsmClipmapBase() { return VSM_CLIPMAP_FIRST * VSM_PAGES_PER_VIEW; }
uint VsmExtraBase() { return VsmClipmapBase() + VSM_NUM_CLIPMAP_LEVELS * VSM_CLIPMAP_PAGES; }
uint VsmViewBase(uint view)
{
    if (view < VSM_CLIPMAP_FIRST) { return view * VSM_PAGES_PER_VIEW; }
    if (view < VSM_CLIPMAP_END) { return VsmClipmapBase() + (view - VSM_CLIPMAP_FIRST) * VSM_CLIPMAP_PAGES; }
    return VsmExtraBase() + (view - VSM_CLIPMAP_END) * VSM_PAGES_PER_VIEW;
}
// Pages per axis of a view at a mip.
uint VsmViewAxis(uint view, uint level) { return VsmIsClipmapView(view) ? VSM_CLIPMAP_AXIS : (VSM_L0_AXIS >> level); }

// (view, level, px, py) -> virtual page id.
uint VsmPageId(uint view, uint level, uint px, uint py)
{
    if (VsmIsClipmapView(view)) { return VsmViewBase(view) + py * VSM_CLIPMAP_AXIS + px; }
    uint axis = VSM_L0_AXIS >> level;
    return VsmViewBase(view) + VSM_LEVEL_OFFSET[level] + py * axis + px;
}

// Virtual page id -> its view.
uint VsmViewOfPage(uint pageId)
{
    if (pageId < VsmClipmapBase()) { return pageId / VSM_PAGES_PER_VIEW; }
    if (pageId < VsmExtraBase()) { return VSM_CLIPMAP_FIRST + (pageId - VsmClipmapBase()) / VSM_CLIPMAP_PAGES; }
    return VSM_CLIPMAP_END + (pageId - VsmExtraBase()) / VSM_PAGES_PER_VIEW;
}

// Virtual page id -> (view, level, px, py).
void VsmDecodePage(uint pageId, out uint view, out uint level, out uint px, out uint py)
{
    view = VsmViewOfPage(pageId);
    uint inView = pageId - VsmViewBase(view);
    level = 0u;
    if (VsmIsClipmapView(view))
    {
        px = inView % VSM_CLIPMAP_AXIS;
        py = inView / VSM_CLIPMAP_AXIS;
        return;
    }
    [unroll] for (uint L = 0u; L <= VSM_MAX_LEVEL; ++L)
    {
        if (inView >= VSM_LEVEL_OFFSET[L] && inView < VSM_LEVEL_OFFSET[L + 1u]) { level = L; }
    }
    uint local = inView - VSM_LEVEL_OFFSET[level];
    uint axis = VSM_L0_AXIS >> level;
    px = local % axis;
    py = local / axis;
}

#endif // VSM_ADDRESSING_HLSLI
