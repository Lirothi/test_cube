#define SKYBOX_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), DescriptorTable(SRV(t0, flags=DATA_VOLATILE)), DescriptorTable(Sampler(s0))"
#pragma pack_matrix(row_major)
#include "utils.hlsl"

cbuffer PerFrame : register(b0)
{
    float4x4 view;      // regular view matrix
    float4x4 proj;
    float4x4 prevView;
    float4x4 prevProj;
    float4x4 projNoJitter;
    float4x4 prevProjNoJitter;
    float exposure;
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
    //c = SRGBToLinear(c);
    float2 currUv = ClipToUV(i.clipPos);
    float2 prevUv = ClipToUV(i.prevPos);
    float2 motion = currUv - prevUv;

    PSOut o;
    o.color = float4(c, 1.0);
    o.velocity = motion;
    return o;
}
