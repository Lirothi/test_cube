// SkyOcclusionCB's fields ONCE (architecture plan R5.3): HLSL's `cbuffer SkyOcclusionCB` (sky_occlusion_cs.hlsl) and
// C++'s SkyOcclusionConstants (app/scene/SceneRenderer_SkyOcclusion.cpp) spell SKY_OCCLUSION_FIELDS. A field list, not
// a struct: HLSL reads a cbuffer as globals; TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are
// /* */ (a // inside a continued line would swallow the next field). Include shared/shader_types.h first. New fields
// are APPENDED.
#ifndef TC_SHADERS_SHARED_SKY_OCCLUSION_H
#define TC_SHADERS_SHARED_SKY_OCCLUSION_H

#define SKY_OCCLUSION_FIELDS \
    float4x4 invView; /* CSTrace: the camera (jittered, as the depth it reads) */                                     \
    float4x4 invProj;                                                                                                 \
    float4x4 topViewProj; /* CSReduce: world -> the top-down renders' clip (the objects' top and the terrain) */      \
    float4x4 bottomViewProj; /* ...and the bottom-up one */                                                           \
    /* x the map's min world x, y its min world z, z 1 / its width (m), w the near level's texel (m) */               \
    float4 mapParams;                                                                                                 \
    float4 heightParams; /* x the top camera's height, y the bottom camera's, z their near plane, w far - near */     \
    uint4 sizes; /* x, y CSTrace's output (half resolution); z the depth renders' size; w the level written */        \
    /* x reach (m), y opacity (a slab's coverage -> its opacity), z where the slabs start (m), w the receiver's */    \
    /* lift off its surface (m) */                                                                                    \
    float4 traceParams;                                                                                               \
    /* the map's edge (SkyOcclusionMap::FadeParams): x, y the camera's world x, z; z the horizontal distance from */  \
    /* it where the fade starts (m); w 1 / the fade's width (m) */                                                    \
    float4 fadeParams;

#endif
