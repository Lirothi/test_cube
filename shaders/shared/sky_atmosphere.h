// sky_atmosphere.hlsli's SkyAtmosphereCB -- the fields ONCE (architecture plan R5.3): HLSL's `cbuffer SkyAtmosphereCB`
// (sky_atmosphere.hlsli; cloud_common.hlsli's CloudCB appends the first two under CLOUD_WITH_SKY_CB) and C++'s
// SkyAtmosphereParameters, SkyViewFrameData (rendering/lighting/SkyAtmosphereSettings.h) and SkyAtmosphere's
// AerialConstants spell SKY_ATMOSPHERE_FIELDS (the medium; the C++ struct is the sky's settings too), SKY_VIEW_FIELDS
// (under SKY_VIEW) and SKY_AERIAL_FIELDS (under SKY_AERIAL). A field list, not a struct: HLSL reads a cbuffer as
// globals. TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a // inside a continued
// line would swallow the next field). Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_SKY_ATMOSPHERE_H
#define TC_SHADERS_SHARED_SKY_ATMOSPHERE_H

#define SKY_ATMOSPHERE_FIELDS \
    /* bottom, top, Rayleigh/Mie density exponential scales */                                                        \
    float4 AtmosphereRadii TC_INIT(shader_shared::FloatRow(6360.0f, 6420.0f, -1.0f / 8.0f, -1.0f / 1.2f));            \
    float4 RayleighScattering TC_INIT(shader_shared::FloatRow(0.005802f, 0.013558f, 0.033100f, 0.0f));                \
    /* w: g (isotropic in B1, used by B2) */                                                                          \
    float4 MieScattering TC_INIT(shader_shared::FloatRow(0.003996f, 0.003996f, 0.003996f, 0.8f));                     \
    float4 MieAbsorption TC_INIT(shader_shared::FloatRow(0.000444f, 0.000444f, 0.000444f, 0.0f));                     \
    /* w: ozone layer split altitude */                                                                               \
    float4 AbsorptionExtinction TC_INIT(shader_shared::FloatRow(0.000650f, 0.001881f, 0.000085f, 25.0f));             \
    /* ascending linear/constant, descending linear/constant */                                                       \
    float4 AbsorptionDensity                                                                                          \
        TC_INIT(shader_shared::FloatRow(1.0f / 15.0f, -2.0f / 3.0f, -1.0f / 15.0f, 8.0f / 3.0f));                     \
    /* GroundAlbedo's default: FColor(170) converted from sRGB to linear, NOT 170/255. */                             \
    /* w: multi-scattering factor */                                                                                  \
    float4 GroundAlbedo TC_INIT(shader_shared::FloatRow(0.40197778f, 0.40197778f, 0.40197778f, 1.0f));

#define SKY_VIEW_FIELDS \
    float4 SkySunDirection; /* local XYZ to the sun; w its angular radius (rad) */                                    \
    float4 SkyIlluminance; /* outer-space RGB lux; w the sky-only luminance scale */                                  \
    /* x the pre-exposure before FP16 storage; y UE's SkyLuminanceFactor (sky pixel, capture, distant light) */       \
    float4 SkyExposure TC_INIT(shader_shared::FloatRow(1.0f, 1.0f, 0.0f, 0.0f));                                      \
    float4 SkyPlanet; /* view height, bottom radius, top radius (km), enabled */                                      \
    /* Time-of-day plan N1: the atmosphere's SECOND light, the moon (UE NUM_ATMOSPHERE_LIGHTS 2, */                   \
    /* SkyAtmosphere.usf:1117 / 1403 / 1688 sum both). Zero illuminance = no second light; every LUT skips it. */     \
    float4 SkyLight1Direction; /* local XYZ to the moon; w its disc's angular radius (0 = no disc: the captures) */   \
    float4 SkyLight1Illuminance; /* outer-space RGB lux as lit now; w the earthshine fraction of the full disc */     \
    float4 SkyMoonDisc; /* the FULL moon's outer-space RGB lux (the disc's reference); w the phase angle (rad) */     \
    /* x the disc's lit scale (phase law / Lommel-Seeliger integral), y 1 = albedo/normal texture ready */            \
    float4 SkyMoonDisc2;                                                                                              \
    /* N2: the night sky's own light for what reads the SKY as a whole (the distant light): rgb the airglow's */      \
    /* zenith radiance (cd/m2), w the stars' + Milky Way's mean */                                                    \
    float4 SkyNightGlow;

#define SKY_AERIAL_FIELDS \
    float4x4 AerialInvView;                                                                                           \
    float4x4 AerialInvProj;                                                                                           \
    /* x: froxel far-plane view depth in km, y: view-distance scale (UE AerialPespectiveViewDistanceScale) */         \
    float4 AerialStart;

#endif
