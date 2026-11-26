// hzb_build_cs.hlsl's HzbCB -- the fields ONCE (architecture plan R5.3): HLSL's `cbuffer HzbCB` (hzb_build_cs.hlsl) and
// C++'s HzbLightCB (rendering/shadows/CascadeHzb.cpp; the camera's pyramid writes it by its reflected names) spell
// HZB_BUILD_FIELDS and, under HZB_LIGHT, HZB_BUILD_LIGHT_FIELDS. A field list, not a struct: HLSL reads a cbuffer as
// globals. TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a // inside a continued
// line would swallow the next field). Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_HZB_BUILD_H
#define TC_SHADERS_SHARED_HZB_BUILD_H

#define HZB_BUILD_FIELDS \
    uint2 dstSize; /* destination mip dimensions, texels */                                                           \
    uint2 srcSize; /* source dimensions: the depth buffer for mip 0, else mip N-1 */                                  \
    uint  fromDepth; /* 1 = reduce the render-res depth buffer into mip 0 */                                          \
    /* P6C step 6: 0 = skip the closest chain entirely. Nothing reads it unless a screen-space march is the active */ \
    /* reflection source, and a pyramid nobody reads is pure bandwidth. */                                            \
    uint  writeClosest;                                                                                               \
    uint  pad1;                                                                                                       \
    uint  pad2;

#define HZB_BUILD_LIGHT_FIELDS \
    /* Occlusion plan S5b (HZB_LIGHT=1, the CASCADE pyramids): mip 0 reduces one cascade's tile of the shadow */      \
    /* atlas, so the source is a RECT of DepthTex -- `srcSize` is the tile's content size and `srcOffset` its */      \
    /* origin in the atlas; the clamp to srcSize keeps the reads inside the tile (the S5 gutter and the */            \
    /* neighbouring tiles never fold in). The atlas is FORWARD-Z (LESS_EQUAL, clear 1.0) and hzb_cull.hlsli is */     \
    /* reverse-Z, so the pyramid stores 1 - z: the furthest surface is still the MINIMUM, and the cull tests a box */ \
    /* whose light projection has its z flipped the same way (CascadeHzb.cpp). One min chain; the closest UAVs are */ \
    /* bound to placeholders and writeClosest is 0. */                                                                \
    uint2 srcOffset;                                                                                                  \
    uint2 pad3;

#endif
