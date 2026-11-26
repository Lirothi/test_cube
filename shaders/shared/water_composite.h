// WaterCompositeCB's fields ONCE (architecture plan R5.3): HLSL's `cbuffer WaterCompositeCB` (water_composite_cs.hlsl)
// and C++'s WaterCompositeConstants (app/scene/SceneRenderInternal.h) spell WATER_COMPOSITE_FIELDS. A field list, not a
// struct: HLSL reads a cbuffer as globals; TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /*
// */ (a // inside a continued line would swallow the next field). Include shared/shader_types.h first. New fields are
// APPENDED.
#ifndef TC_SHADERS_SHARED_WATER_COMPOSITE_H
#define TC_SHADERS_SHARED_WATER_COMPOSITE_H

#define WATER_COMPOSITE_FIELDS \
    float4x4 invViewProj; /* clip -> world (the jittered projection the depth was drawn with) */                      \
    float3   camPos;                                                                                                  \
    /* the ocean preset's roughnessDistance (the water's specularParams.z) */                                         \
    float    roughnessDistance TC_INIT(1.0f);                                                                         \
    float2   invRenderSize;                                                                                           \
    float    preExposure TC_INIT(1.0f);                                                                               \
    /* 1 = the flat mirror read at the UV offset; 0 = the trace already took the wave normal (UE's way), read at */   \
    /* the pixel itself */                                                                                            \
    uint     uvBreakup TC_INIT(1u);                                                                                   \
    /* The prefilter (below): x, y = its Gaussian sigma across / along the screen's columns (render px), 0 = off; */  \
    /* zw unused. */                                                                                                  \
    float4   prefilter;

#endif
