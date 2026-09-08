// B1 diagnostic composition after forward draws, before the existing display transform.
// Top = ground, bottom = atmosphere top. T: x zenith->horizon; MS: x sun below->above.
#define SKY_DEBUG_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
#pragma pack_matrix(row_major)
#include "sky_aerial_common.hlsli"
cbuffer DebugCB : register(b0)
{
    uint2 size; uint view; float startDepthMetres;
    float4x4 invProj, projNoJitter;
};
Texture2D<float4> Transmittance : register(t0);
Texture2D<float4> MultiScatter : register(t1);
Texture3D<float4> AerialVolume : register(t2);
Texture2D<float> Depth : register(t3); // opaque snapshot before forward depth writes
RWTexture2D<float4> Scene : register(u0);
SamplerState LinearClamp : register(s0);
[numthreads(8,8,1)]
[RootSignature(SKY_DEBUG_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= size)) return;
    float2 uv = (id.xy + 0.5f) / size;
    if (view >= 3u)
    {
        float3 color = 0;
        float z = Depth.Load(int3(id.xy, 0));
        if (z > 1.e-6f)
        {
            float4 pos = mul(float4(uv * float2(2, -2) + float2(-1, 1), z, 1), invProj);
            pos /= pos.w;
            if (pos.z > startDepthMetres)
            {
                float distanceKm = length(pos.xyz) / 1000.0f;
                float startKm = startDepthMetres * distanceKm / pos.z;
                float4 clip = mul(pos, projNoJitter);
                float2 apUv = (clip.xy / clip.w) * float2(.5f, -.5f) + .5f;
                // L is already pre-exposed; transmittance/slices are display-linear debug values.
                float4 ap = SampleSkyAerial(AerialVolume, LinearClamp, apUv, distanceKm, startKm, 1.0f);
                if (view == 3u) color = ap.aaa;
                else if (view == 4u) color = ap.rgb;
                else color = (.25f + .5f*frac(sqrt(max(0.0f,distanceKm-startKm)/SkyAerialDepthKm)*SkyAerialSlices)).xxx;
            }
        }
        Scene[id.xy] = float4(color, 1);
        return;
    }
    float3 color = view == 1 ? Transmittance.SampleLevel(LinearClamp, uv, 0).rgb
        : 10.0f * MultiScatter.SampleLevel(LinearClamp, uv, 0).rgb;
    Scene[id.xy] = float4(color, 1);
}
