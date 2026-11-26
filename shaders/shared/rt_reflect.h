// Probe's fields ONCE (architecture plan R5.3): HLSL's `cbuffer Probe` (rt_reflect_common.hlsli) and C++'s
// RtReflectConstants (app/scene/SceneRenderer_Reflections.cpp) spell RT_REFLECT_FIELDS. A field list, not a struct:
// HLSL reads a cbuffer as globals; TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a
// // inside a continued line would swallow the next field). Include shared/shader_types.h first. New fields are
// APPENDED.
#ifndef TC_SHADERS_SHARED_RT_REFLECT_H
#define TC_SHADERS_SHARED_RT_REFLECT_H

#define RT_REFLECT_FIELDS \
    float4x4 view;                                                                                                    \
    float4x4 proj;                                                                                                    \
    float4x4 invView;                                                                                                 \
    float4x4 invProj;                                                                                                 \
    float3 sunDirWS;     float ambientIntensity;                                                                      \
    float3 lightRgb;     float exposure TC_INIT(1.0f);                                                                \
    float depthA;        float depthB;        uint outWidth;       uint outHeight;                                    \
    uint tlasIndex;      uint lightIndex;     uint gb1Index;        uint depthIndex;                                  \
    uint reflectionUavIndex;    uint geomInfoIndex;  uint skyboxIndex;     float skyboxIntensity TC_INIT(1.0f);       \
    /* P16.9: the sky's cosine-convolved irradiance and the scale lighting_cs applies to it. */                       \
    /* `skyIrradianceIndex == 0` means this sky has no F7 derivatives and the flat fallback stands. */                \
    /* lightInvPreExposure: the light target (lightIndex) is PRE-EXPOSED (lighting_cb.hlsli */                        \
    /* lightPreExposure); a reused on-screen hit is brought back to radiance with this. */                            \
    /* sunAngularRadius: the sun as UE's sphere light in the off-screen re-shade, as lighting_cs has it. */           \
    uint skyIrradianceIndex;                                                                                          \
    float skyIrradianceScale TC_INIT(1.0f);                                                                           \
    float lightInvPreExposure TC_INIT(1.0f);                                                                          \
    float sunAngularRadius;                                                                                           \
    /* P16.12: the ground's diffuse reflectance, mirroring lighting_cs. An off-screen hit is */                       \
    /* re-shaded HERE, so leaving it out would make every reflection of a shaded surface about a */                   \
    /* stop darker than the same surface seen directly -- the same half-a-pair defect P16.9 fixed */                  \
    /* for the sky fill itself. */                                                                                    \
    /* sunSoftRadius: UE's SoftSourceRadius of the sun, as lighting_cs has it (EvalBRDF). */                          \
    float3 groundAlbedoRgb TC_INIT(float3(0.25f, 0.25f, 0.25f)); float sunSoftRadius;                                 \
    uint spotLightIndex; uint spotCount;      uint pointLightIndex; uint pointCount;                                  \
    /* depthIndex reconstructs the PRIMARY surface (the reflector). screenDepthIndex is */                            \
    /* the on-screen opaque depth used only for the fast-path visibility/depth-match -- the */                        \
    /* two differ for the glass reflection dispatch (primary = glass depth, screen = opaque */                        \
    /* depth); for opaque reflections screenDepthIndex == depthIndex. */                                              \
    /* alphaMissKeep: stochastic coverage inflation for the foliage alpha test (see */                                \
    /* RtAlphaCandidatePasses); frameSeed is FROZEN by the CPU (see the fill site). */                                \
    /* alphaMode -- the REFLECTION ray's only (its shadow rays, the sky visibility and the GI see */                  \
    /* the geometry, a leaf card solid: rt_geometry.hlsli RtVisibilityBlocked): */                                    \
    /* 0 = OFF (FORCE_OPAQUE traversal, foliage = solid cards, cheapest); */                                          \
    /* 1 = FIRST HIT (FORCE_OPAQUE traversal, then the COMMITTED hit is alpha-tested with the */                      \
    /* albedo sample the shading already fetches -- a transparent texel becomes a miss, so */                         \
    /* crowns get holes showing the sky fallback instead of what is truly behind them; the */                         \
    /* cost is the opaque path plus nothing); */                                                                      \
    /* 2 = FULL (non-opaque candidates alpha-test during traversal -- exact, the expensive one). */                   \
    uint screenDepthIndex; float alphaMissKeep; uint frameSeed; uint alphaMode TC_INIT(2);                            \
    /* Gather-then-shade payload slots. For the TRACE dispatch these are the payload UAV heap */                      \
    /* indices; for the RESOLVE dispatch the payload SRV heap indices. The monolithic (glass) */                      \
    /* dispatch leaves them 0 and never reads them. payloadSkyIndex: the off-screen hit's SKY */                      \
    /* lighting, apart (RtTraceResult.skyPart). skyVisIndex (resolve only, 0 = none this frame): */                   \
    /* the RT sky visibility the resolve scales that part by, at skyVisStrength (skyVis.diffuse). */                  \
    uint payloadRadianceIndex; uint payloadUvIndex; uint payloadSkyIndex; uint skyVisIndex;                           \
    /* rayGeometricNormal: the mirror ray is kept above, and clears, the reflector's GEOMETRIC plane rebuilt from */  \
    /* the depth (RtSetupReflectionRay); 0 = the shading normal alone, the A/B. */                                    \
    float skyVisStrength TC_INIT(1.0f); uint rayGeometricNormal; uint _rtPad6; uint _rtPad7;

#endif
