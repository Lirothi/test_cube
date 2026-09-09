#include "sky_view_mapping.hlsli"
#include "sky_ibl_common.hlsli"
#define SKY_IBL_RS "CBV(b0), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
cbuffer Capture : register(b0)
{
    float4 planet;     // height, bottom, 1/fixed preExposure, target mip size
    float4 captureMip; // x: supersample count per axis, yzw: unused
};
Texture2D<float4> SkyView : register(t0);
RWTexture2DArray<float4> Output : register(u0);
SamplerState LinearClamp : register(s0);
// UE SkyAtmosphere.usf:956-974; :314-317 omits the sun disk during capture to avoid
// double specular (the directional light already supplies it). Fixed sea-level probe.
//
// THIS CUBE IS A PICTURE OF THE SKY, NOT A LIGHTING PROBE. It is the whole sphere, ground term
// included, and it is the source the FOG samples along the view ray -- the direct analogue of UE's
// `InscatteringColorCubemap` on the height fog component ("useful to make distant, heavily fogged
// scene elements match the sky", ExponentialHeightFogComponent.h:50), which is likewise a full
// 4pi asset with no horizon policy on it.
//
// The lower-hemisphere policy that UE's SKYLIGHT capture carries (ReflectionEnvironmentShaders.usf
// :78-81, :205-211, :412-434 -- "no sky lighting is coming from below the horizon ... to avoid
// leaking from below since we are integrating incoming lighting and shadowing separately") belongs
// to the lighting probes, and lives in sky_ibl_filter_cs.hlsl, which builds them from this cube.
// Applying it HERE made the one texture serve both roles, and the fog -- whose view ray points
// DOWN at every pixel of ground and water it runs on -- read the policy value as if it were sky.
//
// The mip chain is UE's too: their fog blends the top mip (non-directional, the sphere average)
// into mip 0 (directional) by distance, so a short, steeply-downward ray never resolves a
// direction. Each mip is captured straight from the SkyView LUT with `captureMip.x` supersamples
// per axis rather than by downsampling the level above, so no mip depends on another and the whole
// chain is one barrier-free run of dispatches.
[numthreads(8, 8, 1)]
[RootSignature(SKY_IBL_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint size = (uint)planet.w, face = id.y / size;
    uint2 pixel = uint2(id.x, id.y % size);
    if (pixel.x >= size || face >= 6) return;
    const uint taps = max((uint)captureMip.x, 1u);
    const float invTaps = 1.0f / (float)taps;
    float3 sum = 0.0f.xxx;
    for (uint sy = 0; sy < taps; ++sy)
    {
        for (uint sx = 0; sx < taps; ++sx)
        {
            const float2 uv = (pixel + (float2(sx, sy) + 0.5f) * invTaps) / size;
            const float3 dir = SkyCubeDirection(uv, face).xzy; // local Z-up for the SkyView mapping
            const bool ground = SkyViewIntersectsGround(dir, planet.x, planet.y);
            sum += SkyView.SampleLevel(LinearClamp, SkyViewDirToUv(dir, planet.x, planet.y, ground), 0).rgb;
        }
    }
    const float3 L = sum * (invTaps * invTaps);
    // Persistent RAW radiance: camera exposure must not dirty or rescale the environment.
    Output[uint3(pixel, face)] = float4(min(max(L * planet.z, 0.0f), 65504.0f), 1);
}
