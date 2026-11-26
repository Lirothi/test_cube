// CascadeHzbCB's fields ONCE (architecture plan R5.3): HLSL's `cbuffer CascadeHzbCB` (shadow_cull_cs.hlsl) and C++'s
// GpuParams (rendering/shadows/CascadeHzb.h) spell CASCADE_HZB_FIELDS. A field list, not a struct: HLSL reads a cbuffer
// as globals; TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a // inside a continued
// line would swallow the next field). Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_CASCADE_HZB_H
#define TC_SHADERS_SHARED_CASCADE_HZB_H

#define CASCADE_HZB_FIELDS \
    TC_ROW_MAJOR float4x4 gHzbPrevViewProj[4]; /* last frame's light view-projection, z flipped */                    \
    TC_ROW_MAJOR float4x4 gHzbViewProj[4]; /* this frame's, z flipped */                                              \
    uint4 gHzbPrevValid; /* per cascade: 1 = the pyramid holds last frame's tile */                                   \
    int4  gHzbViewRect; /* (0, 0, content, content) -- the tile's content rect, full resolution */                    \
    uint2 gHzbSize; /* mip 0 of the pyramids */                                                                       \
    uint  gHzbOn; /* 0 = no test this frame (placeholders bound in t5..t8) */                                         \
    uint  gHzbPad;

#endif
