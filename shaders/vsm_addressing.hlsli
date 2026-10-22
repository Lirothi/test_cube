// Rung 2 VSM virtual-page addressing shared by the request / setup / sampling shaders. Mirrors
// the vsm:: constants in VirtualShadowMap.h. A virtual page id is view*kPagesPerView + the
// per-mip-level offset + py*axis + px; a resident page maps (via the page table) to a physical
// page in the pool (VSM_POOL_PAGES_AXIS² pages of VSM_PAGE_SIZE² texels).
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
static const uint VSM_LOD_MASK       = 0x000F0000u; // 4 bits: offsets 0..15 (10 clipmap levels)
static const uint VSM_PHYS_MASK_C    = 0x0000FFFFu;

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
