// WaterMaskCB's fields ONCE (architecture plan R5.3): HLSL's `cbuffer WaterMaskCB` (ocean_surface_surf_sim.hlsli) and
// C++'s WaterMaskCB (ocean/OceanRenderable.cpp) spell OCEAN_WATER_MASK_FIELDS. A field list, not a struct: HLSL reads a
// cbuffer as globals; TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a // inside a
// continued line would swallow the next field). Include shared/shader_types.h first. New fields are APPENDED.
// MAX_HULLS: the hulls the surface keeps out (HLSL WATER_MASK_MAX_HULLS, C++ ocean::kWaterMaskMaxHulls).
#ifndef TC_SHADERS_SHARED_OCEAN_WATER_MASK_H
#define TC_SHADERS_SHARED_OCEAN_WATER_MASK_H

#define OCEAN_WATER_MASK_FIELDS(MAX_HULLS) \
    /* x hulls this frame, y 1 = discard, 2 = paint the kept-out water magenta (ocean.waterMask:2), 3 = discard, */   \
    /* and paint red the water drawn OVER a hull's plan (a leak, a wave over the deck); z the boats whose bow wave */ \
    /* breakers come in over a gate (wakeBowGate); w bit 1: the lids were drawn this frame (t30 holds them), bit */   \
    /* 0: the camera is inside a basin */                                                                             \
    uint4 waterMaskCount;                                                                                             \
    float4 waterMaskHull[MAX_HULLS * 5];                                                                              \
    /* OceanWake::BowGates, two a boat: (the bow wave's roots' midpoint x z, the hull's forward x z), (the gate's */  \
    /* length m, half its lane m, 0, 0). */                                                                           \
    float4 wakeBowGate[MAX_HULLS * 2];

#endif
