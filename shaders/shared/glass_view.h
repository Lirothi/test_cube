// The transparent pass's per-view constant buffer (b1), its fields ONCE (architecture plan R5.3): HLSL's cbuffer GlassView
// and struct GlassViewData (glass_view_cb.hlsli; glass, particles, the ocean) and C++'s GlassViewCB
// (app/scene/SceneRenderInternal.h, uploaded by memcpy; its static_assert on the size stays). A field list, not a
// struct, because HLSL reads the cbuffer as globals: GLASS_VIEW_FIELDS(levels) is spelled inside `cbuffer GlassView
// {...}` and inside each struct. `levels` = the directional clipmap's level count (HLSL VSM_NUM_CLIPMAP_LEVELS, C++
// vsm::kNumClipmapLevels). Comments are /* */: a // inside a continued line would swallow the next field. Include
// shared/shader_types.h first (the C++ type names). New fields are APPENDED at the tail -- an insertion shifts every
// offset after it.
#ifndef TC_SHADERS_SHARED_GLASS_VIEW_H
#define TC_SHADERS_SHARED_GLASS_VIEW_H

#define GLASS_VIEW_FIELDS(CLIPMAP_LEVELS) \
    float4x4 view;                                                                                                   \
    float4x4 proj;                                                                                                   \
    float4x4 viewProj;                                                                                               \
    float4x4 viewProjNoJitter;                                                                                       \
    float4x4 prevViewProjNoJitter;                                                                                   \
    float4x4 invView;                                                                                                \
    float4x4 invProj;                                                                                                \
    float4 camPosSky; /* xyz = camera position, w = sky intensity */                                                 \
    float4 sunDirAmbient; /* xyz = sun direction, w = ambient intensity */                                           \
    float4 sunColorExposure; /* xyz = sun color, w = sun exposure */                                                 \
    float4 camDirWS; /* xyz = camera forward, w = P5 prefiltered sky mip count (0 = none) */                         \
    float4 screenSizeInv; /* xy = screen size, zw = inverse screen size */                                           \
    float4 shadowAtlasSizeInv; /* xy = atlas size, zw = inverse atlas size */                                        \
    float4 shadowBiasNDC; /* cascade depth bias */                                                                   \
    float4 cascadeTexelWS; /* world size of one cascade texel */                                                     \
    float4 cascadeSplitsVS; /* cascade splits in view space */                                                       \
    float4 cascadeScaleBias[4]; /* xy = scale, zw = bias per cascade */                                              \
    float4 spotShadowInfo; /* xy = spot shadow size, zw = inverse size */                                            \
    float4 lightCounts; /* x = point lights, y = spot lights, z = traced reflections, w = sky reflection enabled */  \
    float4x4 lightViewProj[4];                                                                                       \
    float4 vsmParams; /* x = useVsm, y = refDist, z = depth-bias floor, w = clip blend width */                      \
    /* Step 24f: x = the first level's number, y = normalBias (UE units), z = depthBias (NDC), w = depth-bias */     \
    /* decay/level */                                                                                                \
    float4 clipmapParams;                                                                                            \
    float4x4 clipmapViewProj[CLIPMAP_LEVELS]; /* Step 24f: directional clipmap level viewProjs */                    \
    float4x4 clipmapUvNormal; /* P16.16: receiver-plane transform, must match lighting_cs */                         \
    /* P16.1: x = the pre-exposure every writer of scene colour applies. Glass writes in the transparent pass, */    \
    /* which runs AFTER compose, so compose's own scaling never reaches it. y = the sun disc's angular radius */     \
    /* (rad): the sun as UE's sphere light (utils.hlsli EvalBRDF); z = its soft radius (UE SoftSourceRadius). */     \
    float4 preExposureParams;                                                                                        \
    /* S8: x = 0 legacy 3x3 SampleCmp box, 1 = soft-occlusion ramp + 4x4 Gather tent. APPENDED at the tail on */     \
    /* purpose -- inserting anywhere else shifts every offset after it. */                                           \
    float4 csmFilterMode; /* x = kernel mode, y = receiver bias, z = sharpen, w = over-blur */                       \
    float4 csmFilterParams; /* S10 x = far split, y = blend, z = distance fade; w = normal bias */                   \
    /* SMRT (docs/vsm_smrt_plan.md). x = ray count (0 = single-tap path), y = samples per ray, z = ray length */     \
    /* scale, w = extrapolate max slope. APPENDED at the tail, same rule as above. Glass samples the SAME */         \
    /* clipmap lighting_cs does, so it has to get the same numbers or a window shades against a differently- */      \
    /* sampled shadow than the ground under it. */                                                                   \
    float4 smrtParams;                                                                                               \
    /* x = source radius (sin), y = texel dither, z = the clipmap's resolution LOD bias, w = frame */                \
    float4 smrtParams2;                                                                                              \
    /* Volumetric fog plan A5 (APPENDED at the tail, same rule as above): the volume's lookup parameters and the */  \
    /* analytic medium (PackHeightFog), so glass and particles sit in the air the sand is in. */                     \
    float4 fogVolumeParams; /* (on, far view depth, 1/preExposure, slice count) */                                   \
    float4 fogVolumeZParams; /* (B, O, S, the volume's start) */                                                     \
    float4 fogParams0; /* density, height falloff, reference height, start distance */                               \
    float4 fogParams1; /* max opacity, sun scatter strength, sun scatter exponent, sun scatter start */              \
    float4 fogParams2; /* sky blur, sky back-scatter, zw reserved */                                                 \
    /* S15: x = active SDSM partitions, 0 = not SDSM mode (the gate on reading SdsmPartitions). APPENDED at the */   \
    /* tail, the same rule the fog block above followed. */                                                          \
    float4 csmSdsmParams;                                                                                            \
    /* Glass refraction (glass.hlsl GlassRefractedUv): x = the shell's wall (m), 0 = straight through, >= the */     \
    /* shape's radius = solid glass (the default, glass.wall 0.5). Appended at the tail, the same rule. */           \
    float4 glassRefraction;                                                                                          \
    /* Plan S on glass slots: lighting_cb.hlsli's skyVisParams (x 1 = SkyVisTex is real this frame, y diffuse, z */  \
    /* specular strength). Appended at the tail. */                                                                  \
    float4 glassSkyVis;                                                                                              \
    /* PackHeightFog's UE fog colour (raw), w = 1 = use it instead of the sky along the view. Appended at the */     \
    /* tail. */                                                                                                      \
    float4 fogInscatter;                                                                                             \
    /* PackHeightFog's sun lobe own coloured term (DirectionalInscatteringLuminance) / 4 pi, cd/m2. Appended at */   \
    /* the tail. */                                                                                                  \
    float4 fogSunDirectional;

#endif
