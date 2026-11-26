// cloud_common.hlsli's CloudCB -- the fields ONCE (architecture plan R5.3): HLSL's `cbuffer CloudCB`
// (cloud_common.hlsli) and C++'s VolumetricCloudConstants (rendering/lighting/VolumetricCloudSettings.h; uploaded as
// one blob) spell CLOUD_FIELDS. A field list, not a struct: HLSL reads a cbuffer as globals. TC_INIT is the C++
// mirror's initial value (nothing in HLSL). Comments are /* */ (a // inside a continued line would swallow the next
// field). Include shared/shader_types.h first. New fields are APPENDED. Under CLOUD_WITH_SKY_CB the cbuffer appends the
// sky's SKY_ATMOSPHERE_FIELDS and SKY_VIEW_FIELDS (shared/sky_atmosphere.h): the C++ blob is VolumetricCloudConstants +
// SkyAtmosphereParameters + SkyViewFrameData.
#ifndef TC_SHADERS_SHARED_CLOUD_H
#define TC_SHADERS_SHARED_CLOUD_H

#define CLOUD_FIELDS \
    float4x4 cloudInvViewProjNoJitter; /* clip -> world, this frame's UNJITTERED camera */                            \
    float4x4 cloudViewProjNoJitter; /* world -> clip (w = view depth) */                                              \
    float4x4 cloudPrevViewProjNoJitter; /* world -> last frame's unjittered clip (history UV) */                      \
    float4x4 cloudShadowViewProj; /* world -> cloud shadow map clip (plan C3) */                                      \
    float4x4 cloudShadowInvViewProj; /* cloud shadow map clip -> world */                                             \
    float4 cloudCamera; /* xyz: camera position (world m), w: planet bottom radius (km) */                            \
    /* x: bottom radius (km), y: top radius (km), z: 1 / layer height (km), w: layer bottom altitude (km) */          \
    float4 cloudLayer;                                                                                                \
    float4 cloudSun; /* xyz: direction TO the sun (world), w: phase g (textbook sign: + forward) */                   \
    float4 cloudSunColor; /* rgb: the sun's effective illuminance (post transmittance), w: second phase g */          \
    float4 cloudPhase; /* x: phase blend, y: ms contribution, z: ms occlusion, w: ms eccentricity */                  \
    /* x: view sample count max, y: min, z: 1 / distance to max count (1/km), w: stop transmittance */                \
    float4 cloudTrace;                                                                                                \
    /* x: shadow sample count, y: shadow tracing distance (km), z: sky light bottom visibility, w: tracing max */     \
    /* distance (km) */                                                                                               \
    float4 cloudShadowTrace;                                                                                          \
    /* x: extinction at density 1 (1/m), y: albedo, z: coverage, w: cloud type bias (the profile column's offset) */  \
    float4 cloudMedium;                                                                                               \
    /* x: 1 / formation tile (1/km), y: the formation's time coordinate (0..1, wraps), z: macro variation */          \
    /* (coverage +-), w: 1 / macro tile (1/km) */                                                                     \
    float4 cloudFormation;                                                                                            \
    float4 cloudWind; /* xyz: world drift offset (m) added to every sample, w: tracing start max distance (km) */     \
    /* x: trace width, y: trace height (half the resolved size), z: preExposure, w: 1 / preExposure */                \
    float4 cloudOutput;                                                                                               \
    /* x: depth width, y: depth height (the render size), z: proj._33, w: proj._43 (reverse-Z: viewZ = w / (z - */    \
    /* z33)) */                                                                                                       \
    float4 cloudDepth;                                                                                                \
    /* x: aerial perspective on, y: aerial start depth (km), z: distant sky light on, w: debug view */                \
    float4 cloudAerial;                                                                                               \
    /* x: history valid, y: history weight, z: preExposure / previous preExposure, w: frame index */                  \
    float4 cloudTemporal;                                                                                             \
    float4 cloudShadowMap; /* x: resolution, y: 1 / resolution, z: far depth (km), w: strength */                     \
    /* x: sample count, y: depth bias (km), z: the air's sun under the deck (the capture's aerial perspective), w: */ \
    /* unused */                                                                                                      \
    float4 cloudShadowMap2;                                                                                           \
    /* x: shadow-map texel stride this frame (1 = every texel), yz: the traced texel's offset in its stride x */      \
    /* stride cell, w: cheap-density bits (1 bilinear macro variation, 2 mean erosion) */                             \
    float4 cloudShadowMap3;                                                                                           \
    /* x: the overcast's evenness (overcast x overcastEvenness), y: 1 / LUT width, z: 1 / LUT height, w: unused */    \
    float4 cloudProfile;                                                                                              \
    /* x: lit extra multi-scattering octaves (0..2, UE's count), y: sky light intensity, z: light diffused through */ \
    /* the cloud (strength), w: overcast (0..1) */                                                                    \
    float4 cloudLighting;                                                                                             \
    /* x erosion strength, y erosion power, z 1 / erosion tile (1/km), w high-frequency strength */                   \
    float4 cloudErosion;                                                                                              \
    /* x floor variation clear, y cloudy, z 1 / high-frequency distance (1/km), w the overcast's lift of the */       \
    /* threshold's density */                                                                                         \
    float4 cloudFloorShape;                                                                                           \
    /* N3: x atmosphere top radius (km), y 1 = per-sample transmittance to the light, z body (0 sun, 1 moon), w */    \
    /* sin(the capture's horizon fade) */                                                                             \
    float4 cloudLight;                                                                                                \
    float4 cloudShadowSun; /* N3: xyz direction TO the ground's light (the shadow map's), w unused */                 \
    /* xy: the resolved clouds' size, z: render pixels per resolved pixel (1; 2 in trace mode 0, */                   \
    /* VolumetricCloudSettings.h), w: 1 = the start offset from the blue-noise mask (cloud_trace_cs.hlsl) */          \
    float4 cloudResolve;

#endif
