#define SKY_MULTISCATTER
#include "sky_atmosphere.hlsli"
#define SKY_MULTI_RS "CBV(b0), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
RWTexture2D<float4> MultiScatteredLuminanceLutUAV : register(u0);

// UE SkyAtmosphere.usf:1156-1268, default low-quality two-direction quadrature.
// Unit-white transfer, independent of sun direction/intensity and pre-exposure.
[numthreads(8, 8, 1)]
[RootSignature(SKY_MULTI_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= 32 || id.y >= 32) return;
    float2 uv = (id.xy + 0.5f) / 32.0f;
    float mu = uv.x * 2.0f - 1.0f;
    float3 lightDir = float3(0, sqrt(saturate(1.0f - mu * mu)), mu);
    float3 p = float3(0, 0, lerp(AtmosphereRadii.x, AtmosphereRadii.y, uv.y));
    float3 L0, L1, r0, r1;
    IntegrateSkyMulti(p, float3(0, 0, 1), lightDir, L0, r0);
    IntegrateSkyMulti(p, float3(0, 0, -1), lightDir, L1, r1);
    float3 inScattered = 0.5f * (L0 + L1); // 4pi/2 integral times 1/4pi phase
    float3 r = 0.5f * (r0 + r1);
    float3 r2 = r * r;
    float3 L = inScattered * (1.0f + r + r2 + r * r2 + r2 * r2); // :1251
    MultiScatteredLuminanceLutUAV[id.xy] = float4(L * GroundAlbedo.w, 0);
}
