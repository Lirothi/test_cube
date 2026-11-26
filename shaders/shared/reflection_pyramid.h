// ReflectionPyramidCB's fields ONCE (architecture plan R5.3): HLSL's `cbuffer ReflectionPyramidCB`
// (reflection_pyramid_cs.hlsl) and C++'s ReflectionPyramidConstants (app/scene/SceneRenderer_Reflections.cpp) spell
// REFLECTION_PYRAMID_FIELDS. A field list, not a struct: HLSL reads a cbuffer as globals; TC_INIT is the C++ mirror's
// initial value (nothing in HLSL). Comments are /* */ (a // inside a continued line would swallow the next field).
// Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_REFLECTION_PYRAMID_H
#define TC_SHADERS_SHARED_REFLECTION_PYRAMID_H

#define REFLECTION_PYRAMID_FIELDS \
    uint2 dstSize TC_INIT(uint2(1, 1)); /* destination mip dimensions, texels */                                      \
    uint2 srcSize TC_INIT(uint2(1, 1)); /* source mip dimensions (the reflection's for mip 0) */                      \
    uint fromInput; /* 1 = mip 0: copy the reflection, sample the G-buffer */                                         \
    uint3 _pyramidPad;

#endif
