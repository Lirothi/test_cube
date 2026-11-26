// ScatterCB's fields ONCE (architecture plan R5.3): HLSL's `cbuffer ScatterCB` (vsm_page_scatter_cs.hlsl) and C++'s
// ScatterCB (rendering/shadows/VirtualShadowMap.cpp) spell VSM_SCATTER_FIELDS. A field list, not a struct: HLSL reads a
// cbuffer as globals; TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a // inside a
// continued line would swallow the next field). Include shared/shader_types.h first. New fields are APPENDED.
// CLIPMAP_LEVELS: the directional clipmap's level count (HLSL TC_VSM_CLIPMAP_LEVELS, C++ vsm::kNumClipmapLevels).
#ifndef TC_SHADERS_SHARED_VSM_SCATTER_H
#define TC_SHADERS_SHARED_VSM_SCATTER_H

#define VSM_SCATTER_FIELDS(CLIPMAP_LEVELS) \
    uint gNumCasters; /* active caster SLOT count (also the per-page slice stride) */                                 \
    uint gNumGroups; /* mesh-group count */                                                                           \
    uint gNumLevels; /* active clipmap levels (VSM_NUM_CLIPMAP_LEVELS) */                                             \
    /* Single-draw page render: see the identically-named field in vsm_page_setup_cs.hlsl's SetupCB. */               \
    /* Both writers of PageVisibleList must pack identically, so this is fed from the same CPU value. */              \
    uint gPageIdShift;                                                                                                \
    uint gNumLocalViews; /* local (spot/point) views scattered after the clipmap levels; 0 = none */                  \
    /* 1 = bucket FLOOR casters by their receiver LOD; 0 = old per-view (A/B); C++: A/B lever; mirrors */             \
    /* gPerInstanceLod */                                                                                             \
    uint gPerInstanceLod;                                                                                             \
    uint2 _pad0;                                                                                                      \
    /* S5b.2: x = the HZB test is on, y = last frame's pyramid + PrevPageTable are valid (a render */                 \
    /* with a build happened last frame, same level), z = DeferredPairs capacity, w unused. */                        \
    uint4 gHzbParams;                                                                                                 \
    /* UE's r.Shadow.Virtual.CoarsePagePixelThresholdStatic / Dynamic: an instance under this many texels stays */    \
    /* out of a page only the coarse */                                                                               \
    /* marks asked for. gCoarseBase = their bitfield in Request; 0 = no such pages (vsm::g_coarseCacheDynamic). */    \
    uint  gCoarseBase; /* mirrors gCoarseBase */                                                                      \
    float gCoarsePixelsStatic;                                                                                        \
    float gCoarsePixelsDynamic;                                                                                       \
    /* The wind's page marks (vsm.windPageMarks): a caster that sways marks its pages for the setup's wind dirt */    \
    /* only where the */                                                                                              \
    /* shadow VS sways it -- a vertex's sway fades to 0 at gWindReach (m) from the camera in xz */                    \
    /* (shadow_indirect_csm.hlsl */                                                                                   \
    /* WindOffset's w2), so a box wholly past it, this frame and last, draws the same rigid pose both frames. 0 = */  \
    /* no marks */                                                                                                    \
    /* (the wind holds still: the same clock and gusts, the camera where it was). */                                  \
    float gWindReach; /* mirrors gWindReach (vsm.windPageMarks) */                                                    \
    /* xy = this frame's camera xz, zw = last frame's (the fade's centre): a local view's; C++: mirrors gWindCams */  \
    float4 gWindCams TC_INIT(float4(0.0f, 0.0f, 0.0f, 0.0f));                                                         \
    /* A clipmap level's (vsm.windSliceTexels): xy = the camera its pose was taken at, zw = the one at its take */    \
    /* before -- a */                                                                                                 \
    /* level redraws its swaying pages only when it takes a pose, and then from the last one to this. */              \
    float4 gWindLevelCams[CLIPMAP_LEVELS]; /* mirrors gWindLevelCams */

#endif
