// SkyVisibility's fields ONCE (architecture plan R5.3): HLSL's `cbuffer SkyVisibility` (rt_sky_visibility_cs.hlsl) and
// C++'s SkyVisConstants (app/scene/SceneRenderer_Reflections.cpp) spell SKY_VISIBILITY_FIELDS. A field list, not a
// struct: HLSL reads a cbuffer as globals; TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /*
// */ (a // inside a continued line would swallow the next field). Include shared/shader_types.h first. New fields are
// APPENDED.
#ifndef TC_SHADERS_SHARED_SKY_VISIBILITY_H
#define TC_SHADERS_SHARED_SKY_VISIBILITY_H

#define SKY_VISIBILITY_FIELDS \
    float4x4 invView;                                                                                                 \
    float4x4 invProj;                                                                                                 \
    /* last frame's clip -> world: where the history's depths put a point (UE PrevScreenToTranslatedWorld); C++: */   \
    /* last frame's clip -> world (the history's reprojection test) */                                                \
    float4x4 prevInvViewProj;                                                                                         \
    uint tlasIndex;     uint gb1Index;      uint depthIndex;    uint velocityIndex;                                   \
    /* counts: R8_UNORM, n / 255 */                                                                                   \
    uint accumUavIndex; uint historyIndex;  uint countUavIndex; uint countHistoryIndex;                               \
    uint outWidth;      uint outHeight;     uint rayCount TC_INIT(4);      uint frameIndex;                           \
    /* skyVis.seed (0 = start from own rays) */                                                                       \
    float maxDistance TC_INIT(30.0f);                                                                                 \
    float historyWeight TC_INIT(0.9f);                                                                                \
    uint historyValid;                                                                                                \
    uint seedEnabled TC_INIT(1);                                                                                      \
    /* skyVis.filterDepth / filterNormalPower: the gather's and the seed's */                                         \
    float filterDepthTolerance TC_INIT(0.05f); float filterNormalPower TC_INIT(8.0f);                                 \
    /* skyVis.sampling: 0 white noise, 1 interleaved gradient noise; rt.meshOpacity */                                \
    uint samplingMode TC_INIT(1); uint meshOpacity TC_INIT(1);                                                        \
    /* skyVisRaw (CSTrace's UAV, CSAccumulate's SRV); the target */                                                   \
    uint rawUavIndex;   uint rawIndex;      uint targetUavIndex; uint filterRadius TC_INIT(4);                        \
    /* skyVis.filter, skyVis.debug 1; this */                                                                         \
    uint filterEnabled TC_INIT(1); uint debugView;     uint accumIndex;    uint filterFirst TC_INIT(1);               \
    /* frame's accumulation (SRV); skyVis.filterFirst */

#endif
