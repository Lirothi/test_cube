// The per-view cbuffer PerView (b1) -- the fields ONCE (architecture plan R5.3): HLSL's `cbuffer PerView`
// (gbuffer_common.hlsli: both lists; shadow_indirect_csm.hlsl's CB path: the first) and C++'s PerViewCB
// (app/scene/SceneRenderInternal.h; the SDSM analyze pass writes the same 256 bytes) spell PER_VIEW_FIELDS and
// PER_VIEW_GBUFFER_FIELDS. A field list, not a struct: HLSL reads a cbuffer as globals. TC_INIT is the C++ mirror's
// initial value (nothing in HLSL). Comments are /* */ (a // inside a continued line would swallow the next field).
// Include shared/shader_types.h first. New fields are APPENDED. The VSM page path reads its per-view data out of the
// 256-byte PageProj slot instead (vsm_page_setup_cs.hlsl stores the wind at po+192): not this list.
#ifndef TC_SHADERS_SHARED_PER_VIEW_H
#define TC_SHADERS_SHARED_PER_VIEW_H

#define PER_VIEW_FIELDS \
    float4x4 viewProj;                                                                                                \
    float4x4 viewProjNoJitter;                                                                                        \
    float4x4 prevViewProjNoJitter;                                                                                    \
    /* W3: global wind, filled from WindState by BuildGBufferViewCB. The gbuffer VS (W4) applies the */               \
    /* sway offset from these; the depth-only shadow shaders declare a shorter PerView and ignore */                  \
    /* this tail (W5 adds the matching fields to the shadow CB). windGustMul defaults to 1. */                        \
    float  windTime;                                                                                                  \
    float  windPrevTime;                                                                                              \
    float2 windDirXZ TC_INIT(float2(1.0f, 0.0f));                                                                     \
    float  windSwayAmp;                                                                                               \
    float  windSwayFreq;                                                                                              \
    float  windGustMul TC_INIT(1.0f);                                                                                 \
    float  windPrevGustMul TC_INIT(1.0f);                                                                             \
    /* S6: shadow-depth bias params at 224..239. The GBUFFER VS never reads them; the three */                        \
    /* CPU-fallback shadow VS in this header do. Declared here because they share this cbuffer. */                    \
    float  shadowConstBias; /* 224 */                                                                                 \
    float  shadowSlopeBias; /* 228 */                                                                                 \
    float  shadowMaxSlope; /* 232 */                                                                                  \
    float  shadowClampNear; /* 236 (S7) */

#define PER_VIEW_GBUFFER_FIELDS \
    /* 240: the frame's directional light (the sun or the moon) for the G-buffer's GB1.a, xyz TOWARD it, w = 1 */     \
    /* when set */                                                                                                    \
    /* (FinalizeGBuffer). The shadow views leave it 0. */                                                             \
    float4 gbLightToward;

#endif
