#define BLOOM_FFT_CS_RS "CBV(b0), DescriptorTable(UAV(u0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

// P8C -- one axis of the 2D Fourier transform, plus the spectral multiply that turns two transforms
// into a convolution.
//
// WHY A TRANSFORM AT ALL. Convolution bloom replaces every bright pixel with a copy of a kernel
// image scaled by that pixel's brightness -- which is where streaks, starbursts and ghosts all come
// from at once, instead of three separate approximations. Done directly that is ~3.4e12 multiply-adds
// per channel at this resolution. The convolution theorem makes it
//     conv(frame, kernel) = IFFT( FFT(frame) * FFT(kernel) )
// i.e. O(N log N) plus one elementwise complex multiply. The kernel's own transform is computed once
// and cached, so a frame costs two transforms and a multiply.
//
// THE ALGORITHM is the Stockham auto-sort FFT, radix 2, the same one `shaders/ocean_fft.hlsl` runs
// for the wave spectrum and the same one UE use (`GPUFastFourierTransformCore.ush`). Stockham rather
// than Cooley-Tukey because it needs no bit-reversal permutation pass: the data is re-ordered by the
// transpose that each stage performs anyway.
//
// TWO REAL CHANNELS PER COMPLEX TRANSFORM, AND WHY IT IS LEGAL HERE. A float4 lane carries two
// complex numbers: `.xy` and `.zw`. This packs (R + iG) into the first and (B + i0) into the second,
// so all three colour channels ride one transform pair. That works because CONVOLUTION IS LINEAR and
// the kernel is REAL: conv(R + iG, k) = conv(R, k) + i*conv(G, k), so R comes back in the real part
// and G in the imaginary part with no unpacking step. UE need their "two for one" gymnastics because
// they transform real signals in isolation; we never look at the spectrum of R alone, only at the
// product, so the cheap packing is sound. (If a COMPLEX kernel is ever wanted -- a phase plate --
// this stops being true and the channels have to be separated.)
//
// SIZE. One scan line lives in group shared memory and one thread owns RADIX=2 of its elements, so
// `numthreads` is length/2 and the practical ceiling is 2048 elements (1024 threads, D3D12's limit)
// at 16 KB of LDS. The bloom transform runs on a downscaled, zero-padded frame, so 1024x512 covers
// this project's display resolution with room to spare -- see BloomFftConstants for the padding rule.

#ifndef BLOOM_FFT_MAX_LENGTH
#define BLOOM_FFT_MAX_LENGTH 2048
#endif

// Two complex numbers per texel: .xy = R + iG, .zw = B + i0.
//
// ALL THREE ARE UAVs, INCLUDING THE TWO THAT ARE ONLY READ. The convolution holds its grids in
// UNORDERED_ACCESS for its whole duration -- exactly as the HZB and bloom pyramids do, because this
// engine's barrier layer transitions whole resources and cannot hold one in SHADER_RESOURCE while a
// sibling is being written. Declaring the inputs as `Texture2D` compiled and ran, and GPU-based
// validation caught it as what it was: an SRV binding pointed at a resource in UAV layout
// (id=1358). A RWTexture2D is readable; the flip side is that there is no hardware filtering here,
// which this transform does not want anyway.
RWTexture2D<float4> SrcSpectrum : register(u0);
// The kernel's transform, only read by the multiply stage.
RWTexture2D<float4> KernelSpectrum : register(u1);
RWTexture2D<float4> DstSpectrum : register(u2);
SamplerState gSmp : register(s0);

cbuffer BloomFftCB : register(b0)
{
    uint2 transformSize;  // the padded, power-of-two grid this transform runs on
    uint  isVertical;     // 0 = transform rows (along X), 1 = transform columns (along Y)
    uint  isInverse;      // 0 = forward, 1 = inverse (conjugate twiddles + 1/N normalisation)
    // 1 = multiply by the kernel's spectrum on the way out. Folded into the transform rather than
    // given a pass of its own: the data is already in registers, and a separate pass would be a
    // full round trip through memory for one complex multiply per texel.
    uint  multiplyByKernel;
    uint  fftPad0, fftPad1, fftPad2;
};

groupshared float4 FftShared[BLOOM_FFT_MAX_LENGTH];

float2 ComplexMul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// exp(-2*pi*i * k / n), conjugated for the inverse transform.
float2 Twiddle(float k, float n)
{
    const float kTwoPi = 6.28318530717958647692f;
    float s, c;
    sincos(-kTwoPi * k / n, s, c);
    if (isInverse != 0u) { s = -s; }
    return float2(c, s);
}

float4 LoadElement(uint2 coord)
{
    return SrcSpectrum[coord];
}

// One thread transforms RADIX=2 elements of one scan line; the whole line lives in LDS.
[numthreads(BLOOM_FFT_MAX_LENGTH / 2, 1, 1)]
[RootSignature(BLOOM_FFT_CS_RS)]
void CSMain(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    const uint length = (isVertical != 0u) ? transformSize.y : transformSize.x;
    const uint half_ = length / 2u;
    const uint thread = groupThreadId.x;
    const uint line_ = groupId.x; // which row (or column) this group owns

    // Threads past the transform's own half-length have nothing to own. They must still reach every
    // barrier below, so they are not returned early -- they simply carry zeroes.
    const bool active = (thread < half_);

    // ---- load: element `thread` and element `thread + half`, which is what a radix-2 Stockham
    // stage consumes at Ns = 1 ----
    float4 a = 0.0f.xxxx;
    float4 b = 0.0f.xxxx;
    if (active)
    {
        const uint2 ca = (isVertical != 0u) ? uint2(line_, thread) : uint2(thread, line_);
        const uint2 cb = (isVertical != 0u) ? uint2(line_, thread + half_) : uint2(thread + half_, line_);
        a = LoadElement(ca);
        b = LoadElement(cb);
    }

    // Stockham: at every stage the pair (a, b) is butterflied and written back INTERLEAVED at a
    // stride that doubles, which is what removes the need for a bit-reversal pass.
    uint ns = 1u;
    [loop]
    for (uint stage = 0u; ns < length; ns *= 2u, ++stage)
    {
        if (active)
        {
            const uint j = thread & (ns - 1u);           // position within the current block
            const uint base = ((thread - j) << 1u) + j;  // where this pair lands after the stage
            const float2 w = Twiddle((float)j, (float)(ns * 2u));

            const float4 bw = float4(ComplexMul(b.xy, w), ComplexMul(b.zw, w));
            FftShared[base] = a + bw;
            FftShared[base + ns] = a - bw;
        }
        GroupMemoryBarrierWithGroupSync();
        if (active)
        {
            a = FftShared[thread];
            b = FftShared[thread + half_];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (!active)
    {
        return;
    }

    // ---- the spectral multiply, and the normalisation ----
    //
    // 1/N is applied on the INVERSE pass only, once per axis, which together give the 1/(Nx*Ny) a
    // round trip needs. Splitting it across the two inverse passes rather than applying it all at
    // the end keeps the intermediate magnitudes in range for a 16-bit float target.
    const float norm = (isInverse != 0u) ? (1.0f / (float)length) : 1.0f;

    const uint2 outA = (isVertical != 0u) ? uint2(line_, thread) : uint2(thread, line_);
    const uint2 outB = (isVertical != 0u) ? uint2(line_, thread + half_) : uint2(thread + half_, line_);

    if (multiplyByKernel != 0u)
    {
        // ENERGY NORMALISATION, FOR FREE. The transform's DC term is by definition the SUM of the
        // kernel, so dividing by it makes the convolution preserve total energy no matter what the
        // kernel radius, blade count or falloff are set to. Without this, widening the aperture
        // would brighten the whole bloom -- the failure the plan warns about, and the reason UE
        // spend four shaders on an energy survey. Reading one texel per thread is a broadcast the
        // hardware handles well.
        const float kernelSum = max(KernelSpectrum[uint2(0u, 0u)].x, 1.0e-6f);
        const float invSum = 1.0f / kernelSum;
        const float4 ka = KernelSpectrum[outA] * invSum;
        const float4 kb = KernelSpectrum[outB] * invSum;
        a = float4(ComplexMul(a.xy, ka.xy), ComplexMul(a.zw, ka.zw));
        b = float4(ComplexMul(b.xy, kb.xy), ComplexMul(b.zw, kb.zw));
    }

    DstSpectrum[outA] = a * norm;
    DstSpectrum[outB] = b * norm;
}
