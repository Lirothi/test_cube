// The transparent pass's shared per-view constant buffer (b1): glass AND particles read it (the
// C++ mirror is GlassViewCB in SceneRenderInternal.h). Extracted from glass.hlsl verbatim for
// the volumetric fog (plan A5) -- particles used to declare a prefix of it under another name.
#ifndef GLASS_VIEW_CB_HLSLI
#define GLASS_VIEW_CB_HLSLI

// Needs VSM_NUM_CLIPMAP_LEVELS (a static const in vsm_addressing.hlsli): include it first.
//
// GLASS_VIEW_CB_STRUCT 1 declares the same layout as ConstantBuffer<GlassViewData> gGlassView instead of globals --
// the ocean's own b0 already names view, proj, sunDirAmbient and the fog block. GLASS_VIEW(x) reaches a field
// either way (forward_sun_shadow.hlsli reads it so).
#include "shared/shader_types.h"
#include "shared/glass_slots.h" // GLASS_B_VIEW
#include "shared/glass_view.h" // the fields, once for HLSL and C++ (architecture plan R5.3)

#ifndef GLASS_VIEW_CB_STRUCT
#define GLASS_VIEW_CB_STRUCT 0
#endif

// Per-view/per-frame data shared by every glass object in the pass. Filled once.
#if GLASS_VIEW_CB_STRUCT
struct GlassViewData
#else
cbuffer GlassView : TC_REGISTER(b, GLASS_B_VIEW)
#endif
{
    GLASS_VIEW_FIELDS(VSM_NUM_CLIPMAP_LEVELS)
};
#if GLASS_VIEW_CB_STRUCT
ConstantBuffer<GlassViewData> gGlassView : TC_REGISTER(b, GLASS_B_VIEW);
#define GLASS_VIEW(x) gGlassView.x
#else
#define GLASS_VIEW(x) x
#endif

#endif // GLASS_VIEW_CB_HLSLI
