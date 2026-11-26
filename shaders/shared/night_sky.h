// the night sky's rows -- the fields ONCE (architecture plan R5.3): HLSL's skybox.hlsl's and sky_ibl_capture_cs.hlsl's
// cbuffers (each its own, these rows inside) and C++'s NightSkyFrameData (rendering/lighting/SkyAtmosphereSettings.h)
// spell NIGHT_SKY_FIELDS. A field list, not a struct: HLSL reads a cbuffer as globals. TC_INIT is the C++ mirror's
// initial value (nothing in HLSL). Comments are /* */ (a // inside a continued line would swallow the next field).
// Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_NIGHT_SKY_H
#define TC_SHADERS_SHARED_NIGHT_SKY_H

#define NIGHT_SKY_FIELDS \
    float4 nightGlow; /* rgb the airglow's zenith radiance (cd/m2); w the stars' + Milky Way's mean */                \
    float4 nightLayers; /* x stars, y Milky Way (0 = its map not ready), z twinkle 0..1, w time (s) */                \
    float4 nightRow0; /* world -> equatorial by rows; w 1 = the night sky is drawn */                                 \
    float4 nightRow1;                                                                                                 \
    float4 nightRow2;

#endif
