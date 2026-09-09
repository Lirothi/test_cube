#include "sky_ibl_common.hlsli"
#include "ibl_common.hlsli"
#define SKY_IBL_RS "CBV(b0), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
cbuffer Filter : register(b0) { uint size; uint mip; uint mipCount; uint diffuse; }
TextureCube<float4> Source : register(t0);
RWTexture2DArray<float4> Output : register(u0);
SamplerState LinearClamp : register(s0);
// UE ReflectionEnvironmentShaders.usf:537-647, MonteCarlo.ush:58-63,248-261,347-363.
// Delta: source is a smooth, disk-free SkyView capture; sample mip 0, no source mip pyramid.
// Diffuse is E/PI in a cube (our existing consumers), rather than UE's SH coefficients.
[numthreads(8, 8, 1)]
[RootSignature(SKY_IBL_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint face = id.y / size;
    uint2 pixel = uint2(id.x, id.y % size);
    if (pixel.x >= size || face >= 6) return;
    float3 N = SkyCubeDirection((pixel + 0.5f) / size, face);
    float roughness = saturate(IblRoughnessFromMip(mip, mipCount));
    if (!diffuse && mip == 0)
    {
        Output[uint3(pixel, face)] = float4(Source.SampleLevel(LinearClamp, N, 0).rgb, 1);
        return;
    }
    // Orthonormal basis in our world axes; isotropic integral is independent of its rotation.
    float3 T = normalize(cross(abs(N.z) < 0.999f ? float3(0,0,1) : float3(1,0,0), N));
    float3 B = cross(N, T);
    const uint samples = 64;
    float3 sum = 0;
    float weight = 0;
    [loop] for (uint i = 0; i < samples; ++i)
    {
        float2 E = float2(float(i) / samples, float(reversebits(i)) * 2.3283064365386963e-10f);
        float phi = 2.0f * 3.14159265358979323846f * E.x;
        bool cosine = diffuse != 0 || roughness > 0.99f;
        E.y *= cosine ? 1.0f : 0.995f;
        float a2 = pow(roughness, 4.0f);
        float c = cosine ? sqrt(E.y) : sqrt((1.0f - E.y) / (1.0f + (a2 - 1.0f) * E.y));
        float s = sqrt(max(0.0f, 1.0f - c*c));
        float3 H = float3(s*cos(phi), s*sin(phi), c);
        float3 L = cosine ? H : 2.0f * H.z * H - float3(0,0,1);
        if (L.z > 0 || cosine)
        {
            float w = cosine ? 1.0f : L.z;
            sum += Source.SampleLevel(LinearClamp, L.x*T + L.y*B + L.z*N, 0).rgb * w;
            weight += w;
        }
    }
    Output[uint3(pixel, face)] = float4(max(sum / max(weight, 1.e-6f), 0.0f), 1);
}
