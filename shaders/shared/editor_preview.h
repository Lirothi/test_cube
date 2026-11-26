// editor_preview.hlsl's PreviewCB -- the fields ONCE (architecture plan R5.3): HLSL's `cbuffer PreviewCB`
// (editor_preview.hlsl; compiled by FXC, vs_5_0 / ps_5_0) and C++'s PreviewConstants
// (editor/assets/EditorPreviewRenderer.cpp; its rows are DirectX's XMFLOAT4 / XMFLOAT4X4) spell EDITOR_PREVIEW_FIELDS.
// A field list, not a struct: HLSL reads a cbuffer as globals. TC_INIT is the C++ mirror's initial value (nothing in
// HLSL). Comments are /* */ (a // inside a continued line would swallow the next field). Include shared/shader_types.h
// first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_EDITOR_PREVIEW_H
#define TC_SHADERS_SHARED_EDITOR_PREVIEW_H

#define EDITOR_PREVIEW_FIELDS \
    TC_ROW_MAJOR float4x4 gMVP; /* model * view * proj (row-vector convention) */                                     \
    TC_ROW_MAJOR float4x4 gModel; /* object to preview-world */                                                       \
    float4 gLightDir; /* xyz = light ray direction, w = exposure */                                                   \
    float4 gEyePosition; /* xyz = preview camera position */                                                          \
    float4 gBaseColor; /* real material baseColor factor */                                                           \
    float4 gMetalRoughAlpha; /* xy = metal/rough, z = alpha cutoff, w = MR multiply */                                \
    float4 gTexOffsScale; /* xy = UV offset, zw = UV scale */                                                         \
    float4 gTexFlags; /* xyz = use albedo/MR/normal, w = normal strength */                                           \
    float4 gMaterialFlags; /* x = glTF MR, y = normal RG, z = double-sided, w = albedo exists */                      \
    float4 gSurfaceParams; /* rgb = subsurface color, w = transmission strength */                                    \
    /* x = shading model ID, y = albedo power, z = normal weight, w = highlight 0..1 (Mesh Editor hover over a */     \
    /* slot's controls) */                                                                                            \
    float4 gSurfaceFlags;                                                                                             \
    float4 gTerrainTiling; /* x = zone size, y = rotation radians, z = scale variance, w = blend */                   \
    float4 gTerrainEdgeParams; /* x = edge breakup, y = edge detail, zw reserved */                                   \
    float4 gAmbient; /* rgb = light color, w = ambient intensity */                                                   \
    float4 gEnvironmentParams; /* x = available, y = exposure, z = max mip, w reserved */                             \
    float4 gDebugParams; /* x = normal length, yzw = diagnostic line color */                                         \
    float4 gMarkerParams; /* x = light-position marker, yzw = marker color */                                         \
    float4 gSkyboxRight;                                                                                              \
    float4 gSkyboxUp;                                                                                                 \
    float4 gSkyboxForward;                                                                                            \
    float4 gSkyboxParams; /* xy = horizontal/vertical tan half-FOV, z = exposure */                                   \
    /* ---- the renderer's lighting (appended: every field above keeps its offset) ---- */                            \
    /* x = on, y = prefiltered specular mips (0 = none), z = sky intensity (Skybox::GetExposure), w = sky fill */     \
    /* strength */                                                                                                    \
    float4 gPhysical;                                                                                                 \
    /* rgb = lux (DirectionalLight::GetEffectiveColor), w = the flat ambient fraction, used only when there is no */  \
    /* irradiance cube */                                                                                             \
    float4 gSunIlluminance;                                                                                           \
    float4 gGroundAlbedo; /* rgb = ground albedo for the bounce, w = irradiance cube present */                       \
    /* x = sun half-apex (radians), y = the light's legacy exposure, z = its soft radius (UE SoftSourceRadius) */     \
    float4 gSunParams;                                                                                                \
    float4 gCamera; /* x = exposure multiplier, y = tone curve (0 ACES, 1 AgX, 2 film) */                             \
    float4 gGrade; /* saturation, contrast, gamma, gain */                                                            \
    float4 gGradeOffsetAgx; /* x = grade offset, yzw = AgX slope, power, saturation */                                \
    float4 gFilm; /* slope, toe, shoulder, black clip */                                                              \
    float4 gFilmWhiteClip; /* x = white clip */

#endif
