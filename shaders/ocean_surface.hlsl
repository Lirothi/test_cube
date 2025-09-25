// RootSignature: CBV(b0) TABLE(SRV(t0)) TABLE(SAMPLER(s0))
#pragma pack_matrix(row_major)

cbuffer OceanCB : register(b0)
{
    float4x4 model;
    float4x4 view;
    float4x4 proj;
    float4 clipData[4]; // xy: offset, z: half extent, w: step
    float4 simulationParams;           // x: patch length, y: inv patch length, z: time, w: clip level count
    float4 viewerParams;               // x: viewer x, y: viewer z, z: amplitude, w: fade distance
    float4 cascadeLengthScales;        // length scales per cascade
    float4 inverseCascadeLengthScales; // inv length scales per cascade
    float4 clipMapParams;              // x: scale, y: level half size, z: vertex density, w: fade distance
    float4 clipMapViewer;              // xyz: viewer position
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

float ModifiedManhattanDistance(float3 a, float3 b)
{
    float3 v = a - b;
    return max(abs(v.x + v.z) + abs(v.x - v.z), abs(v.y)) * 0.5f;
}

float EaseInOutClamped(float x)
{
    x = saturate(x);
    return 3.0f * x * x - 2.0f * x * x * x;
}

float4 LodWeights(float viewDist, float lodScale)
{
    float4 scaledLength = max(cascadeLengthScales, float4(1e-3f, 1e-3f, 1e-3f, 1e-3f));
    float4 x = (viewDist - scaledLength * lodScale) / (scaledLength * lodScale);
    return float4(1.0f, 1.0f, 1.0f, 1.0f) - float4(
        EaseInOutClamped(x.x),
        EaseInOutClamped(x.y),
        EaseInOutClamped(x.z),
        EaseInOutClamped(x.w));
}

float3 ClipMapVertex(float3 positionOS, float2 uv)
{
    float clipScale = clipMapParams.x;
    float levelHalfSize = clipMapParams.y;
    float3 viewerPosition = clipMapViewer.xyz;

    float3 morphOffset = float3(uv.x, 0.0f, uv.y);
    positionOS *= clipScale;
    float meshScale = positionOS.y;
    float step = max(meshScale * 4.0f, 1e-3f);

    float snappedX = floor(viewerPosition.x / step) * step;
    float snappedZ = floor(viewerPosition.z / step) * step;
    float3 worldPos = float3(snappedX + positionOS.x, 0.0f, snappedZ + positionOS.z);

    float morphStart = ((levelHalfSize + 1.0f) * 0.5f + 8.0f) * meshScale;
    float morphEnd = (levelHalfSize - 2.0f) * meshScale;

    float denom = max(1e-3f, morphEnd - morphStart);
    float t = saturate((ModifiedManhattanDistance(worldPos, viewerPosition) - morphStart) / denom);
    worldPos += morphOffset * meshScale * t;
    return worldPos;
}

float3 SampleDisplacementCascade(float2 worldXZ, uint cascade, float amplitude)
{
    float lengthScale = max(cascadeLengthScales[cascade], 1e-3f);
    float3 uvw = float3(worldXZ / lengthScale, cascade * 2.0f);
    float4 sample = DisplacementDerivatives.SampleLevel(LinearWrapSampler, uvw, 0);
    return sample.xyz * amplitude;
}

float4 SampleDerivativesCascade(float2 worldXZ, uint cascade, float amplitude)
{
    float lengthScale = max(cascadeLengthScales[cascade], 1e-3f);
    float3 uvw = float3(worldXZ / lengthScale, cascade * 2.0f + 1.0f);
    float4 sample = DisplacementDerivatives.SampleLevel(LinearWrapSampler, uvw, 0);
    return sample * amplitude;
}

float3 SampleDisplacement(float2 worldXZ, float4 weights, uint clipCount, float amplitude)
{
    float3 displacement = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (uint cascade = 0; cascade < 4; ++cascade)
    {
        if (cascade >= clipCount)
        {
            break;
        }
        float w = weights[cascade];
        if (w > 1e-4f)
        {
            displacement += w * SampleDisplacementCascade(worldXZ, cascade, amplitude);
        }
    }
    return displacement;
}

float4 SampleDerivatives(float2 worldXZ, float4 weights, uint clipCount, float amplitude)
{
    float4 derivatives = float4(0.0f, 0.0f, 0.0f, 0.0f);
    [unroll]
    for (uint cascade = 0; cascade < 4; ++cascade)
    {
        if (cascade >= clipCount)
        {
            break;
        }
        float w = weights[cascade];
        if (w > 1e-4f)
        {
            derivatives += w * SampleDerivativesCascade(worldXZ, cascade, amplitude);
        }
    }
    return derivatives;
}

VSOutput VSMain(VSInput input)
{
    VSOutput output;

    uint clipCount = max((uint)simulationParams.w, 1u);

    float3 baseWorld = ClipMapVertex(input.position.xyz, input.uv);
    float3 viewVector = baseWorld - clipMapViewer.xyz;
    float viewDist = length(viewVector);

    float amplitude = viewerParams.z;
    float4 weights = saturate(LodWeights(viewDist, clipMapParams.w));

    float3 displacement = SampleDisplacement(baseWorld.xz, weights, clipCount, amplitude);
    float4 deriv = SampleDerivatives(baseWorld.xz, weights, clipCount, amplitude);

    float3 world = float3(baseWorld.x + displacement.x, displacement.y, baseWorld.z + displacement.z);
    output.worldPos = world;

    float denomX = max(1e-3f, 1.0f + deriv.z);
    float denomZ = max(1e-3f, 1.0f + deriv.w);
    float2 slope = float2(deriv.x / denomX, deriv.y / denomZ);
    float3 normal = normalize(float3(-slope.x, 1.0f, -slope.y));
    output.normalWS = normal;
    output.displacementUVW = float3(baseWorld.xz, viewDist);

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
