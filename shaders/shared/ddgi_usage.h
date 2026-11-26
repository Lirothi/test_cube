// DdgiUsageCB's fields ONCE (architecture plan R5.3): HLSL's `cbuffer DdgiUsageCB` (ddgi_usage_cs.hlsl) and C++'s
// DdgiUsageConstants (app/scene/SceneRenderer_Gi.cpp) spell DDGI_USAGE_FIELDS. A field list, not a struct: HLSL reads a
// cbuffer as globals; TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a // inside a
// continued line would swallow the next field). Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_DDGI_USAGE_H
#define TC_SHADERS_SHARED_DDGI_USAGE_H

#define DDGI_USAGE_FIELDS \
    float4x4 invView; /* the camera (jittered, as the depth it reads) */                                              \
    float4x4 invProj;                                                                                                 \
    float4 cameraPos; /* xyz */                                                                                       \
    int4 nearBase; /* xyz the near cascade's first cell */                                                            \
    float4 nearParams; /* x spacing, y 1 = stamp it */                                                                \
    int4 farBase;                                                                                                     \
    float4 farParams;                                                                                                 \
    uint4 grid; /* xy the sample grid, zw this frame's pixel in each 4x4 block */                                     \
    uint4 stamp; /* x this frame's stamp */

#endif
