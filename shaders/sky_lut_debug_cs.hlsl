// B1 diagnostic composition after forward draws, before the existing display transform.
// Top = ground, bottom = atmosphere top. T: x zenith->horizon; MS: x sun below->above.
#define SKY_DEBUG_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
cbuffer DebugCB : register(b0) { uint2 size; uint view; uint pad; };
Texture2D<float4> Transmittance : register(t0);
Texture2D<float4> MultiScatter : register(t1);
RWTexture2D<float4> Scene : register(u0);
SamplerState LinearClamp : register(s0);
[numthreads(8,8,1)]
[RootSignature(SKY_DEBUG_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= size)) return;
    float2 uv = (id.xy + 0.5f) / size;
    float3 color = view == 1 ? Transmittance.SampleLevel(LinearClamp, uv, 0).rgb
        : 10.0f * MultiScatter.SampleLevel(LinearClamp, uv, 0).rgb;
    Scene[id.xy] = float4(color, 1);
}
