// RootSignature: CONSTANTS(b0,count=4) TABLE(UAV(u0))

#ifndef FFT_SIZE
#define FFT_SIZE 256
#endif

#ifndef FFT_LOG_SIZE
#define FFT_LOG_SIZE 8
#endif

static const uint Size = FFT_SIZE;

RWTexture2DArray<float4> Target : register(u0);

cbuffer FFTParams : register(b0)
{
    uint TargetsCount;
    uint Direction;
    uint Inverse;
    uint Flags;
};

static const uint FFT_FLAG_SCALE = 1u << 0;
static const uint FFT_FLAG_PERMUTE = 1u << 1;

groupshared float4 FftBuffer[2][FFT_SIZE];

float2 ComplexMult(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

void ButterflyValues(uint step, uint index, out uint2 indices, out float2 twiddle)
{
    const float twoPi = 6.28318530718f;
    uint b = Size >> (step + 1);
    uint w = b * (index / b);
    uint i = (w + index) % Size;
    sincos(-twoPi / Size * w, twiddle.y, twiddle.x);
    if (Inverse != 0)
    {
        twiddle.y = -twiddle.y;
    }
    indices = uint2(i, i + b);
}

float4 DoFft(uint threadIndex, float4 input)
{
	FftBuffer[0][threadIndex] = input;
	GroupMemoryBarrierWithGroupSync();

	uint ping = 0;

	[unroll(FFT_LOG_SIZE)]
	for (uint step = 0; step < FFT_LOG_SIZE; ++step)
	{
		uint2 inputsIndices;
		float2 twiddle;
		ButterflyValues(step, threadIndex, inputsIndices, twiddle);

		const uint readIndex = ping;
		const uint writeIndex = 1 - ping;

		float4 lhs = FftBuffer[readIndex][inputsIndices.x];
		float4 v = FftBuffer[readIndex][inputsIndices.y];
		float4 res = lhs + float4(ComplexMult(twiddle, v.xy), ComplexMult(twiddle, v.zw));

		FftBuffer[writeIndex][threadIndex] = res;

		ping = writeIndex;
		GroupMemoryBarrierWithGroupSync();
	}

	return FftBuffer[ping][threadIndex];
}

[numthreads(FFT_SIZE, 1, 1)]
void Fft(uint3 id : SV_DispatchThreadID)
{
    uint threadIndex = id.x;
    uint2 targetIndex = (Direction == 0) ? id.xy : id.yx;

    for (uint k = 0; k < TargetsCount; ++k)
    {
        Target[uint3(targetIndex, k)] = DoFft(threadIndex, Target[uint3(targetIndex, k)]);
    }
}

float4 DoPostProcess(float4 input, uint2 coord)
{
    if ((Flags & FFT_FLAG_SCALE) != 0)
    {
        input /= (Size * Size);
    }
    if ((Flags & FFT_FLAG_PERMUTE) != 0)
    {
        input *= 1.0f - 2.0f * ((coord.x + coord.y) & 1);
    }
    return input;
}

[numthreads(8, 8, 1)]
void PostProcess(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Size || id.y >= Size)
    {
        return;
    }

    for (uint k = 0; k < TargetsCount; ++k)
    {
        Target[uint3(id.xy, k)] = DoPostProcess(Target[uint3(id.xy, k)], id.xy);
    }
}
