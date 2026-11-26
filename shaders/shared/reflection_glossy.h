// ReflectionGlossyCB's fields ONCE (architecture plan R5.3): HLSL's `cbuffer ReflectionGlossyCB`
// (reflection_glossy_cs.hlsl) and C++'s ReflectionGlossyConstants (app/scene/SceneRenderer_Reflections.cpp) spell
// REFLECTION_GLOSSY_FIELDS. A field list, not a struct: HLSL reads a cbuffer as globals; TC_INIT is the C++ mirror's
// initial value (nothing in HLSL). Comments are /* */ (a // inside a continued line would swallow the next field).
// Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_REFLECTION_GLOSSY_H
#define TC_SHADERS_SHARED_REFLECTION_GLOSSY_H

#define REFLECTION_GLOSSY_FIELDS \
    float4x4 invView;                                                                                                 \
    float4x4 invProj;                                                                                                 \
    uint2 outSize TC_INIT(uint2(1, 1)); /* the reflection target, texels */                                           \
    float2 invOutSize TC_INIT(float2(1.0f, 1.0f));                                                                    \
    float pixelsPerRadian TC_INIT(1.0f); /* reflection texels per radian of view angle: 0.5 x height x P[1][1] */     \
    /* a tap off the centre's plane by more than this share of the view distance is not it */                         \
    float planeTolerance TC_INIT(0.02f);                                                                              \
    float normalPower TC_INIT(8.0f); /* ...and its normals' agreement raised to this */                               \
    uint mipCount TC_INIT(1); /* the pyramids' levels */

#endif
