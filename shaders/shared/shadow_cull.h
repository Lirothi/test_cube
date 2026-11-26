// shadow_cull_cs.hlsl's CullParams -- the fields ONCE (architecture plan R5.3): HLSL's `cbuffer CullParams`
// (shadow_cull_cs.hlsl) and C++'s ShadowCullCB (rendering/shadows/ShadowGpuData.cpp) spell SHADOW_CULL_FIELDS. A field
// list, not a struct: HLSL reads a cbuffer as globals. TC_INIT is the C++ mirror's initial value (nothing in HLSL).
// Comments are /* */ (a // inside a continued line would swallow the next field). Include shared/shader_types.h first.
// New fields are APPENDED. WIND_RIGID_ROWS: the uint4 rows of a ShadowGpuData::ViewMask (HLSL (TC_SHADOW_VIEW_CEILING +
// 127) / 128, C++ (render::kShadowViewCeiling + 127u) / 128u).
#ifndef TC_SHADERS_SHARED_SHADOW_CULL_H
#define TC_SHADERS_SHARED_SHADOW_CULL_H

#define SHADOW_CULL_FIELDS(WIND_RIGID_ROWS) \
    uint gNumCasters;                                                                                                 \
    uint gNumViews;                                                                                                   \
    uint gNumGroups;                                                                                                  \
    uint gCamView TC_INIT(0xFFFFFFFFu); /* S4: the camera's frustum slot, or 0xFFFFFFFF when the camera row is off */ \
    uint gLocalSplit; /* S7: 1 = the local views' static casters into StaticArgs / StaticVisibleList */               \
    /* S9e: the clipmap slots [gClipFirst, gClipEnd) -- every other view past the cascades is local */                \
    uint gClipFirst TC_INIT(render::ShadowViewClipmapSlot(0u));                                                       \
    uint gClipEnd TC_INIT(render::ShadowViewClipmapSlot(render::kShadowViewClipmapLevels));                           \
    uint gGiFirst; /* S9e: casters from here on are GI (exempt from the radius threshold) */                          \
    float4 gLocalCam; /* S9e: xyz the camera, w (RadiusThreshold x LODDistanceFactor)^2, 0 = off */                   \
    /* render::g_localShadowWindRange: one bit per view slot -- a local view whose light is past the range, whose */  \
    /* wind-only casters are static (ShadowGpuData::ViewMask). Zero on a frame without the split. */                  \
    uint4 gWindRigid[WIND_RIGID_ROWS];

#endif
