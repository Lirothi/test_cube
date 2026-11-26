// WaterMaskLidCB's fields ONCE (architecture plan R5.3): HLSL's `cbuffer WaterMaskLidCB` (water_mask_lid.hlsl) and
// C++'s LidCB (ocean/OceanRenderable.cpp) spell WATER_MASK_LID_FIELDS. A field list, not a struct: HLSL reads a cbuffer
// as globals; TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a // inside a continued
// line would swallow the next field). Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_WATER_MASK_LID_H
#define TC_SHADERS_SHARED_WATER_MASK_LID_H

#define WATER_MASK_LID_FIELDS \
    TC_ROW_MAJOR float4x4 modelViewProj; /* the hull's model space -> the water's clip space */                       \
    float4 modelViewZ; /* the view depth: dot(float4(p, 1), modelViewZ), as the surface's viewDepth */

#endif
