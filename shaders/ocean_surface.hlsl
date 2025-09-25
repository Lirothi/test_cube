// RootSignature: CBV(b0) TABLE(SRV(t0)) TABLE(SAMPLER(s0))
#pragma pack_matrix(row_major)

cbuffer OceanCB : register(b0)
{
    float4x4 model;
    float4x4 view;
    float4x4 proj;
    float4 clipData[4]; // xy: offset, z: half extent, w: step
    float4 simulationParams;           // x: patch length, y: inv patch length, z: time, w: clip level count
    float4 viewerParams;               // x: viewer x, y: viewer z, z: amplitude, w: unused
    float4 cascadeLengthScales;        // length scales per cascade
    float4 inverseCascadeLengthScales; // inv length scales per cascade
};

Texture2DArray<float4> DisplacementDerivatives : register(t0);
SamplerState LinearWrapSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normalWS : TEXCOORD1;
    float3 displacementUVW : TEXCOORD2;
};

static const float3 kDeepColor = float3(0.01f, 0.09f, 0.18f);
static const float3 kShallowColor = float3(0.06f, 0.25f, 0.35f);
static const float3 kLightDir = normalize(float3(0.4f, 1.0f, 0.2f));

float3 SampleDisplacement(float2 worldXZ, uint level, float amplitude)
{
    float lengthScale = max(cascadeLengthScales[level], 1e-3);
    float3 uvw = float3(worldXZ / lengthScale, level * 2);
    float4 sample = DisplacementDerivatives.SampleLevel(LinearWrapSampler, uvw, 0);
    return float3(sample.x, sample.y, sample.z) * amplitude;
}

float4 SampleDerivatives(float2 worldXZ, uint level, float amplitude)
{
    float lengthScale = max(cascadeLengthScales[level], 1e-3);
    float3 uvw = float3(worldXZ / lengthScale, level * 2 + 1);
    float4 sample = DisplacementDerivatives.SampleLevel(LinearWrapSampler, uvw, 0);
    return sample * amplitude;
}

VSOutput VSMain(VSInput input)
{
    VSOutput output;

    uint clipCount = max((uint)simulationParams.w, 1u);
    uint level = min((uint)input.position.z, clipCount - 1u);

    float4 clip = clipData[level];
    float2 worldXZ = input.position.xy * clip.z + clip.xy;

    float amplitude = viewerParams.z;

    float3 displacement = SampleDisplacement(worldXZ, level, amplitude);
    float4 deriv = SampleDerivatives(worldXZ, level, amplitude);

    float3 world = float3(worldXZ.x + displacement.x, displacement.y, worldXZ.y + displacement.z);
    output.worldPos = world;

    float denomX = max(1e-3f, 1.0f + deriv.z);
    float denomZ = max(1e-3f, 1.0f + deriv.w);
    float2 slope = float2(deriv.x / denomX, deriv.y / denomZ);
    float3 normal = normalize(float3(-slope.x, 1.0f, -slope.y));
    output.normalWS = normal;
    output.displacementUVW = float3(worldXZ, level);

    float4 local = float4(world, 1.0f);
    float4 worldH = mul(local, model);
    float4 viewPos = mul(worldH, view);
    output.position = mul(viewPos, proj);
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float heightFactor = saturate(input.worldPos.y * 0.5f + 0.5f);
    float3 baseColor = lerp(kDeepColor, kShallowColor, heightFactor);
    float3 normal = normalize(input.normalWS);
    float lighting = saturate(dot(normal, kLightDir)) * 0.7f + 0.3f;
    float3 color = baseColor * lighting;
    return float4(color, 1.0f);
}
