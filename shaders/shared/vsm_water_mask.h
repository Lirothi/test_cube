// WaterMaskCB's fields ONCE (architecture plan R5.3): HLSL's `cbuffer WaterMaskCB` (vsm_water_mask_cs.hlsl) and C++'s
// WaterMaskCB (rendering/shadows/VirtualShadowMap.cpp) spell VSM_WATER_MASK_FIELDS. A field list, not a struct: HLSL
// reads a cbuffer as globals; TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a //
// inside a continued line would swallow the next field). Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_VSM_WATER_MASK_H
#define TC_SHADERS_SHARED_VSM_WATER_MASK_H

#define VSM_WATER_MASK_FIELDS \
    uint  gNumCasters;                                                                                                \
    uint  gNumViews;                                                                                                  \
    uint  gMaskBase; /* the mask's first word: the bitfield's length */                                               \
    float gMargin; /* m round every box (vsm::kWaterMaskMargin) */

#endif
