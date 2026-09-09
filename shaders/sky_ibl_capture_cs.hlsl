#include "sky_view_mapping.hlsli"
#include "sky_ibl_common.hlsli"
#define SKY_IBL_RS "CBV(b0), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
cbuffer Capture : register(b0) { float4 planet; } // height, bottom, 1/fixed preExposure, size
Texture2D<float4> SkyView : register(t0);
RWTexture2DArray<float4> Output : register(u0);
SamplerState LinearClamp : register(s0);
// UE SkyAtmosphere.usf:956-974; :314-317 omits the sun disk during capture to avoid
// double specular (the directional light already supplies it). Fixed sea-level probe.
[numthreads(8, 8, 1)]
[RootSignature(SKY_IBL_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint size = (uint)planet.w, face = id.y / size;
    uint2 pixel = uint2(id.x, id.y % size);
    if (pixel.x >= size || face >= 6) return;
    float3 dir = SkyCubeDirection((pixel + 0.5f) / size, face).xzy;
    bool ground = SkyViewIntersectsGround(dir, planet.x, planet.y);
    float3 L = SkyView.SampleLevel(LinearClamp, SkyViewDirToUv(dir, planet.x, planet.y, ground), 0).rgb;
    // Persistent RAW radiance: camera exposure must not dirty or rescale the environment.
    Output[uint3(pixel, face)] = float4(min(max(L * planet.z, 0.0f), 65504.0f), 1);
}
