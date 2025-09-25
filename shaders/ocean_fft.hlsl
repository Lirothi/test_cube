// RootSignature: CONSTANTS(b0,count=3) TABLE(SRV(t0)) TABLE(UAV(u4) UAV(u5) UAV(u6))
static const float PI = 3.14159265358979323846f;

cbuffer FFTParams : register(b0)
{
    uint Size;
    uint Step;
    uint PingPong;
};

RWTexture2D<float4> PrecomputeBuffer : register(u4);
RWTexture2D<float2> Buffer0 : register(u5);
RWTexture2D<float2> Buffer1 : register(u6);
Texture2D<float4> PrecomputedData : register(t0);

float2 ComplexMult(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

float2 ComplexExp(float2 a)
{
    return float2(cos(a.y), sin(a.y)) * exp(a.x);
}

[numthreads(1, 8, 1)]
void PrecomputeTwiddleFactorsAndInputIndices(uint3 id : SV_DispatchThreadID)
{
    uint b = Size >> (id.x + 1);
    float2 mult = 2.0f * PI * float2(0.0f, 1.0f) / Size;
    uint i = (2 * b * (id.y / b) + id.y % b) % Size;
    float2 twiddle = ComplexExp(-mult * ((id.y / b) * b));
    PrecomputeBuffer[id.xy] = float4(twiddle.x, twiddle.y, i, i + b);
    PrecomputeBuffer[uint2(id.x, id.y + Size / 2)] = float4(-twiddle.x, -twiddle.y, i, i + b);
}

[numthreads(8, 8, 1)]
void HorizontalStepFFT(uint3 id : SV_DispatchThreadID)
{
    float4 data = PrecomputedData[uint2(Step, id.x)];
    uint2 inputsIndices = (uint2)data.ba;
    if (PingPong != 0)
    {
        Buffer1[id.xy] = Buffer0[uint2(inputsIndices.x, id.y)] + ComplexMult(data.rg, Buffer0[uint2(inputsIndices.y, id.y)]);
    }
    else
    {
        Buffer0[id.xy] = Buffer1[uint2(inputsIndices.x, id.y)] + ComplexMult(data.rg, Buffer1[uint2(inputsIndices.y, id.y)]);
    }
}

[numthreads(8, 8, 1)]
void VerticalStepFFT(uint3 id : SV_DispatchThreadID)
{
    float4 data = PrecomputedData[uint2(Step, id.y)];
    uint2 inputsIndices = (uint2)data.ba;
    if (PingPong != 0)
    {
        Buffer1[id.xy] = Buffer0[uint2(id.x, inputsIndices.x)] + ComplexMult(data.rg, Buffer0[uint2(id.x, inputsIndices.y)]);
    }
    else
    {
        Buffer0[id.xy] = Buffer1[uint2(id.x, inputsIndices.x)] + ComplexMult(data.rg, Buffer1[uint2(id.x, inputsIndices.y)]);
    }
}

[numthreads(8, 8, 1)]
void HorizontalStepInverseFFT(uint3 id : SV_DispatchThreadID)
{
    float4 data = PrecomputedData[uint2(Step, id.x)];
    uint2 inputsIndices = (uint2)data.ba;
    if (PingPong != 0)
    {
        Buffer1[id.xy] = Buffer0[uint2(inputsIndices.x, id.y)] + ComplexMult(float2(data.r, -data.g), Buffer0[uint2(inputsIndices.y, id.y)]);
    }
    else
    {
        Buffer0[id.xy] = Buffer1[uint2(inputsIndices.x, id.y)] + ComplexMult(float2(data.r, -data.g), Buffer1[uint2(inputsIndices.y, id.y)]);
    }
}

[numthreads(8, 8, 1)]
void VerticalStepInverseFFT(uint3 id : SV_DispatchThreadID)
{
    float4 data = PrecomputedData[uint2(Step, id.y)];
    uint2 inputsIndices = (uint2)data.ba;
    if (PingPong != 0)
    {
        Buffer1[id.xy] = Buffer0[uint2(id.x, inputsIndices.x)] + ComplexMult(float2(data.r, -data.g), Buffer0[uint2(id.x, inputsIndices.y)]);
    }
    else
    {
        Buffer0[id.xy] = Buffer1[uint2(id.x, inputsIndices.x)] + ComplexMult(float2(data.r, -data.g), Buffer1[uint2(id.x, inputsIndices.y)]);
    }
}

[numthreads(8, 8, 1)]
void Scale(uint3 id : SV_DispatchThreadID)
{
    float inv = 1.0f / (float(Size) * float(Size));
    Buffer0[id.xy] = Buffer0[id.xy] * inv;
}

[numthreads(8, 8, 1)]
void Permute(uint3 id : SV_DispatchThreadID)
{
    Buffer0[id.xy] = Buffer0[id.xy] * (1.0f - 2.0f * float((id.x + id.y) % 2));
}
