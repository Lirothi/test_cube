// sky_lut_debug_cs.hlsl's DebugCB -- the fields ONCE (architecture plan R5.3): HLSL's `cbuffer DebugCB`
// (sky_lut_debug_cs.hlsl) and C++'s SkyAtmosphere::...'s Constants (rendering/lighting/SkyAtmosphere.cpp) spell
// SKY_LUT_DEBUG_FIELDS. A field list, not a struct: HLSL reads a cbuffer as globals. TC_INIT is the C++ mirror's
// initial value (nothing in HLSL). Comments are /* */ (a // inside a continued line would swallow the next field).
// Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_SKY_LUT_DEBUG_H
#define TC_SHADERS_SHARED_SKY_LUT_DEBUG_H

#define SKY_LUT_DEBUG_FIELDS \
    uint2 size; uint view; float startDepthMetres;                                                                    \
    float4x4 invProj, projNoJitter;

#endif
