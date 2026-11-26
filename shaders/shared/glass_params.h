// GlassParams's fields ONCE (architecture plan R5.3): HLSL's `cbuffer GlassParams` (glass.hlsl) and C++'s
// GlassMaterialCB (rendering/renderables/GBufferRenderable.cpp) spell GLASS_PARAMS_FIELDS. A field list, not a struct:
// HLSL reads a cbuffer as globals; TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a
// // inside a continued line would swallow the next field). Include shared/shader_types.h first. New fields are
// APPENDED.
#ifndef TC_SHADERS_SHARED_GLASS_PARAMS_H
#define TC_SHADERS_SHARED_GLASS_PARAMS_H

#define GLASS_PARAMS_FIELDS \
    float4x4 world;                                                                                                   \
    float4x4 prevWorld;                                                                                               \
    /* xyz = absorption, w = thickness; C++: xyz unused (no absorption in the thin model), w = thickness (m) */       \
    float4 absorptionThickness;                                                                                       \
    float4 tintRoughness; /* xyz = tint color, w = roughness; C++: unread by GLASS_MATERIAL */                        \
    /* x = IOR, y = reflection strength, z = refraction distortion, w = normal map enabled (0=disabled, 1=enabled) */ \
    float4 matExtra;                                                                                                  \
    uint objectId;                                                                                                    \
    uint3 _objectIdPad;                                                                                               \
    /* GLASS_MATERIAL only (GBufferRenderable::RenderTranslucent; zero from TransparentStaticMesh, unread there): */  \
    /* the slot's MaterialParams and glass values. */                                                                 \
    float4 matBaseColor; /* linear tint (rgb, multiplies the albedo map) + alpha factor (a) */                        \
    /* x = metallic, y = roughness, z = transmission factor, w = alpha cutoff (-1 = no split) */                      \
    float4 matSurface;                                                                                                \
    float4 matTexFlags; /* x = useAlbedo, y = useMR, z = useNormal, w = normal strength */                            \
    float4 matTexOffsScale; /* UV offset (xy), scale (zw) -- gbuffer_common.hlsli's tfUVp */                          \
    /* x = transmission map present, y = mrMultiply, z = the slot's glass wall (m; < 0 = the frame's), w = unused */  \
    float4 matGlass;

#endif
