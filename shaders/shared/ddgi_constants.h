// DdgiConstants's fields ONCE (architecture plan R5.3): HLSL's `cbuffer DdgiConstants` (ddgi_constants.hlsli) and C++'s
// DdgiConstants (app/scene/SceneRenderer_Gi.cpp) spell DDGI_CONSTANTS_FIELDS. A field list, not a struct: HLSL reads a
// cbuffer as globals; TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a // inside a
// continued line would swallow the next field). Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_DDGI_CONSTANTS_H
#define TC_SHADERS_SHARED_DDGI_CONSTANTS_H

#define DDGI_CONSTANTS_FIELDS \
    float4x4 rayRotation; /* this update's random rotation of the Fibonacci directions */                             \
    int4 volumeBase; /* xyz the volume's first world cell, w its generation */                                        \
    /* x spacing (m), y hysteresis, z ray reach (m), w depth clamp (m) */                                             \
    float4 volumeParams TC_INIT(float4(0.5f, 0.9f, 30.0f, 1.0f));                                                     \
    /* x 1/N of the probes per frame, y this frame's phase, z probes this frame, w frame seed */                      \
    uint4 schedule TC_INIT(uint4(1, 0, 0, 0));                                                                        \
    float3 sunDirWS;  float ambientIntensity;                                                                         \
    float3 lightRgb;  float skyIrradianceScale TC_INIT(1.0f);                                                         \
    uint tlasIndex;   uint geomInfoIndex;    uint rayUavIndex;        uint irradianceIndex;                           \
    uint depthIndex;  uint stateIndex;       uint skyIrradianceIndex; uint spotLightIndex;                            \
    /* gi.twoSidedSheets: a two-sided material's back is its other face */                                            \
    uint spotCount;   uint pointLightIndex;  uint pointCount;         uint twoSidedSheets TC_INIT(1);                 \
    /* rt.meshOpacity: the meshes' cutout opacity; kDdgiTrace* */                                                     \
    uint bounceOn;    float skyRayDistance TC_INIT(15.0f);  uint meshOpacity TC_INIT(1);        uint traceFlags;      \
    uint blendRays TC_INIT(64); /* the blended rays this update traces (gi.rays: 16-64, a multiple of 16) */          \
    /* a probe traces its fixed rays every this many of its updates (gi.fixedRayPeriod; 1 = every one) */             \
    uint fixedRayPeriod TC_INIT(1);                                                                                   \
    uint updateIndex; /* the volume's update cycle (frames / divisor): each probe sees it rise by one per update */   \
    uint usageIndex; /* the trace's bindless index of this cascade's usage stamps (ddgi_usage_cs.hlsl) */             \
    uint frameStamp; /* this frame's stamp, as ddgi_usage_cs wrote it */                                              \
    uint sleepPeriod TC_INIT(1); /* an idle probe updates one turn in this many (gi.sleepPeriod; 1 = every turn) */   \
    uint _ddgiPad1;                                                                                                   \
    uint _ddgiPad2;

#endif
