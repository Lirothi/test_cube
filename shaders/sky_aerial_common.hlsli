#ifndef SKY_AERIAL_COMMON_HLSLI
#define SKY_AERIAL_COMMON_HLSLI
// UE SkyAtmosphereRendering.cpp:120-137. Distances are km, not world metres.
static const uint SkyAerialWidth = 32;
static const uint SkyAerialSlices = 16;
static const float SkyAerialDepthKm = 96.0f;

// UE SkyAtmosphereCommon.ush:62-81,103-115: squared depth distribution and near fade.
float4 SampleSkyAerial(Texture3D<float4> volume, SamplerState linearClamp,
    float2 screenUv, float distanceKm, float startKm, float oneOverPreExposure)
{
    float depth = max(0.0f, distanceKm - startKm);
    float w = sqrt(depth / SkyAerialDepthKm);
    float slice = w * SkyAerialSlices;
    float weight = slice < 0.7071067811865475f ? saturate(slice * slice * 2.0f) : 1.0f;
    weight *= saturate(depth / 0.00001f); // UE: one-centimetre start fade
    float4 ap = volume.SampleLevel(linearClamp, float3(screenUv, w), 0);
    ap.rgb *= weight * oneOverPreExposure;
    ap.a = 1.0f - weight * (1.0f - ap.a);
    return ap;
}
#endif
