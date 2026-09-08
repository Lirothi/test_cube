#define SKYBOX_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "sky_view_mapping.hlsli"

cbuffer PerFrame : register(b0)
{
    float4x4 view;      // regular view matrix
    float4x4 proj;
    float4x4 prevView;
    float4x4 prevProj;
    float4x4 projNoJitter;
    float4x4 prevProjNoJitter;
    float exposure;
    float4 skySunDirection;
    float4 skyIlluminance;
    float4 skyExposure;
    float4 skyPlanet;
}

struct VSIn {
    float3 pos : POSITION;
};

struct VSOut {
    float4 pos : SV_Position;
    float4 prevPos : TEXCOORD1;
    float3 dir : TEXCOORD0; // sampling direction
    float4 clipPos : TEXCOORD2;
};

[RootSignature(SKYBOX_RS)]
VSOut VSMain(VSIn i)
{
    // 1) Position: remove translation from view, apply proj, and push to the far plane
    float4x4 v = view;
    v._41 = 0.0;
    v._42 = 0.0;
    v._43 = 0.0; // remove translation (HLSL uses _41.._43)
    VSOut o;
    float4 viewPos = mul(float4(i.pos, 1.0), v);
    o.pos = mul(viewPos, proj);
    o.pos.z = 0.0f;
    float4 clipNoJitter = mul(viewPos, projNoJitter);
    clipNoJitter.z = 0.0f;
    o.clipPos = clipNoJitter;

    float4x4 pv = prevView;
    pv._41 = 0.0;
    pv._42 = 0.0;
    pv._43 = 0.0;
    float4 prevViewPos = mul(float4(i.pos, 1.0), pv);
    o.prevPos = mul(prevViewPos, prevProjNoJitter);
    o.prevPos.z = 0.0f;

    //float3 dirWS = mul(viewPos.xyz, (float3x3) invView).xyz;
    float3 dirWS = i.pos;
    o.dir = normalize(dirWS);
    
    return o;
}

TextureCube sky : register(t0);
Texture2D<float4> skyViewLut : register(t1);
Texture2D<float4> transmittanceLut : register(t2);
SamplerState samLinear : register(s0);

struct PSOut
{
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
};

[RootSignature(SKYBOX_RS)]
PSOut PSMain(VSOut i)
{
    float3 c = sky.Sample(samLinear, i.dir).rgb * exposure;
    if (skyPlanet.w != 0.0f)
    {
        float3 dir = normalize(i.dir.xzy);
        float height = skyPlanet.x, bottom = skyPlanet.y, top = skyPlanet.z;
        bool ground = SkyViewIntersectsGround(dir, height, bottom);
        // The LUT is pre-exposed for storage; LightTarget is RAW radiance for HDRI
        // and surface lighting alike. Decode here so compose uses one exposure path.
        c = skyViewLut.SampleLevel(samLinear, SkyViewDirToUv(dir, height, bottom, ground), 0).rgb
            / max(skyExposure.x, 1.e-8f);
        // UE SkyAtmosphereCommon.ush:256-279, Rendering.cpp:445-446.
        // sunAngularSize is interpreted as angular RADIUS in radians for the disk.
        float radius = skySunDirection.w;
        float cosHalf = cos(radius);
        float viewDotLight = dot(dir, skySunDirection.xyz);
        if (!ground && radius > 0.0f && viewDotLight > cosHalf)
        {
            float H = sqrt(top * top - bottom * bottom);
            float h = min(height, top);
            float rho = sqrt(max(0.0f, h * h - bottom * bottom));
            float d = max(0.0f, -h * dir.z + sqrt(max(0.0f, h*h*(dir.z*dir.z-1.0f)+top*top)));
            float2 uv = float2((d - (top-h)) / max(1.e-6f, rho+H-(top-h)), rho/H);
            float3 tr = height >= top ? 1.0f : transmittanceLut.SampleLevel(samLinear, uv, 0).rgb;
            // 2*sin(radius/2)^2 avoids cancellation for very small disks.
            float oneMinusCos = 2.0f * pow(sin(0.5f * radius), 2.0f);
            float softEdge = saturate(2.0f * (viewDotLight - cosHalf) / max(oneMinusCos, 1.e-12f));
            // UE applies SkyAndAerialPerspectiveLuminanceFactor to scattering, not the disk.
            float3 disk = tr * skyIlluminance.rgb / max(2.0f * SkyViewPi * oneMinusCos, 1.e-12f);
            // Match the current HDRI/surface-lighting FP16 radiance range BEFORE exposure.
            // A procedural-only pre-exposure bypass gave bloom thousands of times more
            // energy than an equally bright HDRI sun or specular highlight. Keep coverage
            // outside the radiance limit, so the soft disk edge remains antialiased.
            c += min(disk, 65504.0f) * softEdge;
        }
        c = min(c, 65504.0f);
    }
    float2 currUv = ClipToUV(i.clipPos);
    float2 prevUv = ClipToUV(i.prevPos);
    float2 motion = currUv - prevUv;

    PSOut o;
    o.color = float4(c, 1.0);
    o.velocity = motion;
    return o;
}
