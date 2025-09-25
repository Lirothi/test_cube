// RootSignature: CONSTANTS(b0,count=4) TABLE(SRV(t0,t1)) TABLE(UAV(u0))

cbuffer SpectrumParams : register(b0)
{
    uint Resolution;
    uint Cascades;
    float Time;
    uint CascadeStride;
};

StructuredBuffer<float4> H0 : register(t0);
StructuredBuffer<float4> Waves : register(t1);
RWTexture2DArray<float4> Spectrum : register(u0);

float2 ComplexMul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

float2 ComplexConj(float2 a)
{
    return float2(a.x, -a.y);
}

[numthreads(8, 8, 1)]
void CalculateAmplitudes(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Resolution || dispatchThreadId.y >= Resolution || dispatchThreadId.z >= Cascades)
    {
        return;
    }

    const uint baseIndex = dispatchThreadId.z * CascadeStride
        + dispatchThreadId.y * Resolution + dispatchThreadId.x;

    float4 wave = Waves[baseIndex];
    float4 h0 = H0[baseIndex];

    if (wave.w == 0.0f)
    {
        Spectrum[uint3(dispatchThreadId.xy, dispatchThreadId.z * 2)] = 0;
        Spectrum[uint3(dispatchThreadId.xy, dispatchThreadId.z * 2 + 1)] = 0;
        return;
    }

    float phase = wave.w * Time;
    float s, c;
    sincos(phase, s, c);
    float2 exponent = float2(c, s);

    float2 h = ComplexMul(h0.xy, exponent) + ComplexMul(float2(h0.z, h0.w), ComplexConj(exponent));
    float2 ih = float2(-h.y, h.x);

    float kLen = length(wave.xz);
    float invKLen = (kLen > 1e-4) ? rcp(kLen) : 0.0f;

    float lambda = wave.y;

    float2 displacementX = lambda * ih * wave.x * invKLen;
    float2 displacementY = h;
    float2 displacementZ = lambda * ih * wave.z * invKLen;

    float2 displacementX_dx = -lambda * h * wave.x * wave.x * invKLen;
    float2 displacementY_dx = ih * wave.x;
    float2 displacementZ_dx = -lambda * h * wave.x * wave.z * invKLen;

    float2 displacementY_dz = ih * wave.z;
    float2 displacementZ_dz = -lambda * h * wave.z * wave.z * invKLen;

    float4 packed0 = float4(
        displacementX.x - displacementY.y,
        displacementX.y + displacementY.x,
        displacementZ.x - displacementZ_dx.y,
        displacementZ.y + displacementZ_dx.x);

    float4 packed1 = float4(
        displacementY_dx.x - displacementY_dz.y,
        displacementY_dx.y + displacementY_dz.x,
        displacementX_dx.x - displacementZ_dz.y,
        displacementX_dx.y + displacementZ_dz.x);

    Spectrum[uint3(dispatchThreadId.xy, dispatchThreadId.z * 2)] = packed0;
    Spectrum[uint3(dispatchThreadId.xy, dispatchThreadId.z * 2 + 1)] = packed1;
}
