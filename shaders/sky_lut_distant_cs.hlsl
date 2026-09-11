#define SKY_VIEW
#include "sky_atmosphere.hlsli"
#define DISTANT_SKY_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
Texture2D<float4> Transmittance : register(t0);
Texture2D<float4> MultiScatter : register(t1);
RWTexture2D<float4> DistantSky : register(u0);
SamplerState LinearClamp : register(s0);
// UE SkyAtmosphereRendering.cpp:1102-1120 / RandomStream.h:115-123,342.
// The fixed 8x8 stratified sphere samples, FRandomStream seed 0xDE4DC0DE.
// Delta: constants instead of an uploaded buffer; identical sample distribution and ordering.
static const float3 SphereDirections[64] = {
    float3(0.553352535f, 0.149271294f, 0.819462657f),
    float3(0.655382097f, 0.522989631f, 0.544936895f),
    float3(0.955122292f, 0.136374697f, 0.262951314f),
    float3(0.903731883f, 0.393119782f, 0.169485986f),
    float3(0.731092334f, 0.6774261f, -0.0812270641f),
    float3(0.951815605f, 0.150274187f, -0.267328858f),
    float3(0.62674588f, 0.244235799f, -0.739958405f),
    float3(0.56821841f, 0.121369138f, -0.81387794f),
    float3(0.0179671254f, 0.314223886f, 0.949178874f),
    float3(0.415900856f, 0.578565478f, 0.701632738f),
    float3(0.205371886f, 0.843880117f, 0.495669961f),
    float3(0.295728266f, 0.953949511f, 0.050250411f),
    float3(0.385188818f, 0.903913558f, -0.185929894f),
    float3(0.412229985f, 0.860249043f, -0.300063372f),
    float3(0.282735199f, 0.675644338f, -0.680856466f),
    float3(0.0195403639f, 0.620258987f, -0.7841537f),
    float3(-0.26341176f, 0.539948404f, 0.799418509f),
    float3(-0.46830079f, 0.600220263f, 0.64840579f),
    float3(-0.194046393f, 0.884011149f, 0.425288498f),
    float3(-0.0780521631f, 0.974233449f, 0.211605668f),
    float3(-0.541702807f, 0.836584568f, -0.0817577839f),
    float3(-0.400613785f, 0.844597042f, -0.355196238f),
    float3(-0.414023966f, 0.568375766f, -0.711008549f),
    float3(-0.110317551f, 0.524838865f, -0.844022632f),
    float3(-0.375222772f, 0.0253295079f, 0.926588535f),
    float3(-0.633319139f, 0.485579163f, 0.602594197f),
    float3(-0.733251929f, 0.472709179f, 0.488761306f),
    float3(-0.7805112f, 0.58718586f, 0.214511216f),
    float3(-0.970575452f, 0.120193057f, -0.208654881f),
    float3(-0.853066742f, 0.331730336f, -0.402780533f),
    float3(-0.592894673f, 0.489952683f, -0.639079213f),
    float3(-0.472486854f, 0.219527572f, -0.853559494f),
    float3(-0.151823595f, -0.101272866f, 0.983205676f),
    float3(-0.690598428f, -0.0348385386f, 0.722398877f),
    float3(-0.676667452f, -0.627924442f, 0.384489298f),
    float3(-0.784233928f, -0.598899841f, 0.162160993f),
    float3(-0.929999888f, -0.279279858f, -0.238962293f),
    float3(-0.84410429f, -0.215553507f, -0.490942597f),
    float3(-0.822698176f, -0.194548368f, -0.534152269f),
    float3(-0.221907228f, -0.11801634f, -0.967899442f),
    float3(-0.0207274351f, -0.622306287f, 0.782499373f),
    float3(-0.0281751733f, -0.770271182f, 0.637093782f),
    float3(-0.019924365f, -0.893311918f, 0.448995411f),
    float3(-0.399852246f, -0.898934126f, 0.178985298f),
    float3(-0.487302721f, -0.865596771f, -0.115231633f),
    float3(-0.509785771f, -0.808269083f, -0.294651628f),
    float3(-0.523834884f, -0.545068383f, -0.654597163f),
    float3(-0.0765670836f, -0.331504077f, -0.940341711f),
    float3(0.0655848086f, -0.515917957f, 0.854123712f),
    float3(0.314944625f, -0.636074841f, 0.704427898f),
    float3(0.325434089f, -0.831619203f, 0.450002313f),
    float3(0.0359972827f, -0.979462504f, 0.198387027f),
    float3(0.448308915f, -0.874768674f, -0.183844686f),
    float3(0.236158565f, -0.855015755f, -0.461711168f),
    float3(0.102363154f, -0.733031988f, -0.672447681f),
    float3(0.262432426f, -0.586661696f, -0.766131401f),
    float3(0.468570799f, -0.266134202f, 0.842385888f),
    float3(0.849686086f, -0.084606193f, 0.520456851f),
    float3(0.915618181f, -0.157357186f, 0.369975865f),
    float3(0.976836741f, -0.206224635f, 0.0571073294f),
    float3(0.727279067f, -0.674354196f, -0.12771678f),
    float3(0.682471633f, -0.672879577f, -0.285421848f),
    float3(0.592569768f, -0.441246569f, -0.673915863f),
    float3(0.222896725f, -0.0468816161f, -0.973714113f)
};
groupshared float3 SphereLuminance[64];
// UE SkyAtmosphere.usf:1360-1449, integrator :590-604,628-633,738-758.
// 64 rays, 10 uniform steps at offset .3, 6km, isotropic phase, no ground bounce or disk.
// One sun and no opaque/cloud shadows (same delta as the other atmosphere LUTs).
[numthreads(8, 8, 1)]
[RootSignature(DISTANT_SKY_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint lane = id.y * 8u + id.x;
    float3 dir = SphereDirections[lane];
    float3 p = float3(0, 0, SkyPlanet.x);
    float bottom;
    float lengthKm = SkyRayLength(p, dir, bottom);
    float dt = lengthKm / 10.0f;
    float3 L = 0, throughput = 1;
    [loop] for (uint step = 0; step < 10; ++step)
    {
        float3 q = p + dir * ((step + 0.3f) * dt);
        float height = length(q);
        float3 up = q / height;
        MediumSampleRGB m = SampleAtmosphereMediumRGB(q);
        float3 tr = exp(-m.Extinction * dt);
        float mu = dot(SkySunDirection.xyz, up);
        float2 uv;
        getTransmittanceLutUvs(height, mu, AtmosphereRadii.x, AtmosphereRadii.y, uv);
        float3 lightT = Transmittance.SampleLevel(LinearClamp, uv, 0).rgb;
        float planet = SkyRaySphereNearest(q, SkySunDirection.xyz, PlanetRadiusOffset * up, AtmosphereRadii.x);
        float3 multi = MultiScatter.SampleLevel(LinearClamp,
            saturate(float2(mu * 0.5f + 0.5f, (height - AtmosphereRadii.x) / (AtmosphereRadii.y - AtmosphereRadii.x))), 0).rgb;
        float3 source = ((planet >= 0 ? 0.0f : 1.0f) * lightT / (4.0f * SkyPi) + multi) * m.Scattering;
        L += throughput * (source - source * tr) / max(m.Extinction, 1.e-9f);
        throughput *= tr;
    }
    // SkyIlluminance.w = SkyAndAerialPerspectiveLuminanceFactor (usf:1393). UE also multiply SkyLuminanceFactor
    // in here (usf:1397); ours is a picture-only knob and stays out of the fog's light, deliberately.
    SphereLuminance[lane] = L * SkyIlluminance.rgb * SkyIlluminance.w;
    GroupMemoryBarrierWithGroupSync();
    // Full barriers at every level: do not assume UE's final lanes are implicitly lockstep.
    [unroll] for (uint stride = 32; stride > 0; stride >>= 1)
    {
        if (lane < stride) SphereLuminance[lane] += SphereLuminance[lane + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    // Sphere solid angle 4PI/64 times uniform medium phase 1/(4PI) = mean radiance.
    // Raw FP32, NOT pre-exposed; consumers apply their own camera exposure exactly once.
    if (lane == 0) DistantSky[uint2(0,0)] = float4(SphereLuminance[0] / 64.0f, 0);
}
