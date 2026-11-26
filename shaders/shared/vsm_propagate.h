// vsm_page_propagate_cs.hlsl's VsmPropagateCB -- the fields ONCE (architecture plan R5.3): HLSL's `cbuffer
// VsmPropagateCB` (vsm_page_propagate_cs.hlsl) and C++'s PropagateCB (rendering/shadows/VirtualShadowMap.cpp) spell
// VSM_PROPAGATE_FIELDS. A field list, not a struct: HLSL reads a cbuffer as globals. TC_INIT is the C++ mirror's
// initial value (nothing in HLSL). Comments are /* */ (a // inside a continued line would swallow the next field).
// Include shared/shader_types.h first. New fields are APPENDED. CLIPMAP_LEVELS: the directional clipmap's level count
// (HLSL VSM_NUM_CLIPMAP_LEVELS, C++ vsm::kNumClipmapLevels).
#ifndef TC_SHADERS_SHARED_VSM_PROPAGATE_H
#define TC_SHADERS_SHARED_VSM_PROPAGATE_H

#define VSM_PROPAGATE_FIELDS(CLIPMAP_LEVELS) \
    uint  gNumClipLevels;                                                                                             \
    uint  gPagesPerAxis; /* VSM_L0_AXIS; clipmap levels use page level 0 (the LEVEL is the LOD) */                    \
    uint2 _padPropagate;                                                                                              \
    /* Per clipmap level, the square it covers in the FIXED light frame that Scene::UpdateClipmap snaps every */      \
    /* level in: xy = the level's texel-snapped centre projected on (right, trueUp), z = the level's full extent. */  \
    /* Levels share those axes, which is exactly what makes a level-to-level page mapping plain 2D arithmetic */      \
    /* rather than a matrix inverse. (C++: vsm::ClipmapLevelSquare's centreX, centreY, extent, _pad.) */              \
    float4 gLevelXform[CLIPMAP_LEVELS];

#endif
