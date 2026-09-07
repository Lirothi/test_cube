#include "sky_atmosphere.hlsli"
#define SKY_TRANS_RS "CBV(b0), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"
RWTexture2D<float4> TransmittanceLutUAV : register(u0);

// UE SkyAtmosphere.usf:1100-1137. Same dimensions, 10 samples, offset 0.3.
// DELTA: RGBA16F instead of R11G11B10F, alpha=0; no unused light evaluation.
// Earth reference table (scalar double transcription, texel centres, RGB linear):
// texel (0,0):   (0.9320433, 0.8504903, 0.7291999), height 3.679 m, mu .9736727.
// texel (255,0): (0.0840152, 0.0063381, 0.0000220), same height, mu -.0008248.
// These are LUT texel centres, NOT exact sea-level zenith/horizon ray directions.
// --set=sky.lutValidate:1 checks every texel against the scalar reference in the session log.
[numthreads(8, 8, 1)]
[RootSignature(SKY_TRANS_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= 256 || id.y >= 64) return;
    float h, mu;
    fromTransmittanceLutUVs(h, mu, AtmosphereRadii.x, AtmosphereRadii.y, (id.xy + 0.5f) / float2(256, 64));
    float3 p = float3(0, 0, h);
    float3 dir = float3(0, sqrt(1.0f - mu * mu), mu);
    float bottom;
    float tMax = SkyRayLength(p, dir, bottom);
    float3 opticalDepth = 0;
    if (dot(p, p) > AtmosphereRadii.x * AtmosphereRadii.x)
    {
        [loop] for (uint i = 0; i < 10; ++i)
            opticalDepth += SampleAtmosphereMediumRGB(p + dir * (tMax * (i + 0.3f) / 10.0f)).Extinction * (tMax / 10.0f);
    }
    TransmittanceLutUAV[id.xy] = float4(exp(-opticalDepth), 0);
}
