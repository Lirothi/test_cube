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
// SIZE. A line lives in group shared memory and one thread owns RADIX=2 of its elements: the group is
// BLOOM_FFT_MAX_LENGTH / 2 threads and 16 KB x 2 of LDS, and it transforms as many lines as fill it
// (2026-11-09: one line a group left half the threads of a 1024 row and three quarters of a 512 column
// idle at every barrier -- the transform cost what its group count did).
//
// THE FRAME'S ORDER (BloomRenderer::Convolve, bloom.convFftFused): the image fills the grid's top-left
// `imageSize`, the rest is the zero pad. (1) Rows forward, only the image's rows: the pad's rows are
// zero and so are their transforms -- the column pass reads them as zero (`loadLimit`) instead of
// transforming zeroes. (2) Columns in PAIRS, x and N - x, forward, times the kernel's spectrum, and
// back, in one dispatch (mode 3): the multiply needs the spectrum at -k, which lives in the partner
// column -- in the same group's LDS now, where it used to need a pass of its own between two full
// column passes. Only the image's rows are written back (`writeLimit`): nothing reads the pad's. (3)
// Rows back, only the image's. The resolve reads the image's rectangle and nothing else.

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
// The kernel's transform, only read by the multiply.
RWTexture2D<float4> KernelSpectrum : register(u1);
RWTexture2D<float4> DstSpectrum : register(u2);
SamplerState gSmp : register(s0);

cbuffer BloomFftCB : register(b0)
{
    uint2 transformSize;  // the padded, power-of-two grid this transform runs on
    uint  isVertical;     // 0 = transform rows (along X), 1 = transform columns (along Y)
    uint  isInverse;      // 0 = forward, 1 = inverse (conjugate twiddles + 1/N normalisation)
    // 0 = one axis of the transform. 1 = the Hermitian spectral multiply, a full-grid pass between the
    // forward and inverse transforms (bloom.convFftFused 0). 3 = the columns fused: forward, multiply,
    // inverse, a column pair a time (see THE FRAME'S ORDER).
    uint  mode;
    uint  lineCount;      // mode 0: the lines (rows or columns) transformed, from the first; 0 = all of them
    uint  loadLimit;      // modes 0, 3: elements of a line at/after this read as zero (rows never transformed); 0 = none
    uint  writeLimit;     // modes 0, 3: elements of a line at/after this are not written (nothing reads them); 0 = all
};

static const uint kThreads = BLOOM_FFT_MAX_LENGTH / 2u;

groupshared float4 FftShared[BLOOM_FFT_MAX_LENGTH];

float2 ComplexMul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// exp(-2*pi*i * k / n), conjugated for the inverse transform.
float2 Twiddle(float k, float n, bool inverse)
{
    const float kTwoPi = 6.28318530717958647692f;
    float s, c;
    sincos(-kTwoPi * k / n, s, c);
    if (inverse) { s = -s; }
    return float2(c, s);
}

// One line of `length` elements at LDS `base`, this thread owning elements i and i + length/2: (a, b) in,
// the transform's (a, b) out, in natural order -- and the whole transform left in LDS. Stockham: at every
// stage the pair is butterflied and written back INTERLEAVED at a stride that doubles, which is what
// removes the bit-reversal pass. EVERY thread of the group calls it (the barriers); `active` says whether
// this one owns a line.
void StockhamLine(inout float4 a, inout float4 b, uint base, uint i, uint length, bool active, bool inverse)
{
    const uint half_ = length / 2u;
    [loop]
    for (uint ns = 1u; ns < length; ns *= 2u)
    {
        if (active)
        {
            const uint j = i & (ns - 1u);           // position within the current block
            const uint dst = ((i - j) << 1u) + j;   // where this pair lands after the stage
            const float2 w = Twiddle((float)j, (float)(ns * 2u), inverse);
            const float4 bw = float4(ComplexMul(b.xy, w), ComplexMul(b.zw, w));
            FftShared[base + dst] = a + bw;
            FftShared[base + dst + ns] = a - bw;
        }
        GroupMemoryBarrierWithGroupSync();
        if (active)
        {
            a = FftShared[base + i];
            b = FftShared[base + i + half_];
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

// THE COLOURED-KERNEL MULTIPLY (P8C-2). The packing carries (R + iG) in .xy and (B + i0) in .zw.
// While R and G shared one REAL kernel the packed product was the packed convolution; a coloured
// kernel's R and G differ, so both spectra are unpacked at (k, -k) by Hermitian symmetry, multiplied
// per channel, and repacked:
//     Fr(k) = (Z(k) + Z*(-k)) / 2      Fg(k) = (Z(k) - Z*(-k)) / (2i)
//     Zout(k) = Fr*Kr + i * (Fg*Kg)
// -k is ((N-x)%N, (M-y)%M) -- the %N is what makes k = 0 and Nyquist their own mirrors, and it is the
// classic off-by-one if written as N-x alone. Verified against per-channel numpy convolution to float
// rounding (fft_verify_hermitian.py) BEFORE this was written, mirrored edge rows included.
//
// The B lane needs no unpack: B and its kernel lane are both real signals, so their packed spectra ARE
// the per-channel spectra and the direct product is exact.
//
// ENERGY NORMALISATION, PER CHANNEL, as UE's GetKernelSum does (GPUFastFourierTransform.usf, "Normalize
// R & G" / "Normalize B & A"): the kernel spectrum's DC texel holds the per-channel sums -- (Sr, Sg)
// packed in .xy of lane 1, Sb in .z of lane 2 -- and each channel is divided by its own (`invNorm`). The
// kernel's colour balance comes back afterwards, once, through the Tint the resolve applies.
float4 SpectralProduct(float4 z, float4 zm, float4 w, float4 wm, float3 invNorm)
{
    const float2 zc = float2(zm.x, -zm.y);   // Z*(-k), lane 1
    const float2 wc = float2(wm.x, -wm.y);   // W*(-k), lane 1

    const float2 fr = (z.xy + zc) * 0.5f;
    const float2 fg = float2(z.y - zc.y, zc.x - z.x) * 0.5f;   // (Z - Z*(-k)) / (2i)
    const float2 kr = (w.xy + wc) * 0.5f;
    const float2 kg = float2(w.y - wc.y, wc.x - w.x) * 0.5f;

    const float2 pr = ComplexMul(fr, kr) * invNorm.x;
    const float2 pg = ComplexMul(fg, kg) * invNorm.y;

    float4 outV;
    outV.xy = float2(pr.x - pg.y, pr.y + pg.x);        // Fr*Kr + i*(Fg*Kg)
    outV.zw = ComplexMul(z.zw, w.zw) * invNorm.z;
    return outV;
}

float3 KernelInvNorm()
{
    const float4 dc = KernelSpectrum[uint2(0u, 0u)];
    return 1.0f / max(dc.xyz, 1.0e-6f.xxx);
}

[numthreads(BLOOM_FFT_MAX_LENGTH / 2, 1, 1)]
[RootSignature(BLOOM_FFT_CS_RS)]
void CSMain(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    const uint thread = groupThreadId.x;

    // ---- mode 1: the Hermitian spectral multiply, one group per ROW, each thread two columns ----
    //
    // Uniform early path: EVERY thread of the group takes it (mode is a CB constant), so nobody
    // misses the barriers of the transforms below.
    if (mode == 1u)
    {
        const uint width = transformSize.x;
        const uint height = transformSize.y;
        const uint halfW = width / 2u;
        if (thread >= halfW) { return; }
        const uint y = groupId.x;
        const float3 invNorm = KernelInvNorm();
        [unroll]
        for (uint e = 0u; e < 2u; ++e)
        {
            const uint x = (e == 0u) ? thread : (thread + halfW);
            const uint2 p = uint2(x, y);
            const uint2 m = uint2((width - x) % width, (height - y) % height);
            DstSpectrum[p] = SpectralProduct(SrcSpectrum[p], SrcSpectrum[m], KernelSpectrum[p], KernelSpectrum[m], invNorm);
        }
        return;
    }

    // ---- mode 3: the columns fused -- forward, times the kernel's spectrum, back -- a pair x, N - x a time ----
    //
    // A pair is two columns of L in LDS and L threads, half a column each; a group holds kThreads / L
    // pairs (BloomRenderer falls back to the separate passes when a column is longer than kThreads). Pair 0
    // is columns 0 and N/2, each its own mirror; pair p the columns p and N - p, each the other's.
    if (mode == 3u)
    {
        const uint W = transformSize.x;
        const uint L = transformSize.y;
        const uint halfL = L / 2u;
        const uint pairsPerGroup = max(kThreads / L, 1u);
        const uint pairLocal = thread / L;
        const uint t2 = thread - pairLocal * L;
        const uint slot = t2 / halfL; // 0 or 1: which column of the pair
        const uint i = t2 - slot * halfL;
        const uint p = groupId.x * pairsPerGroup + pairLocal;
        const bool active = pairLocal < pairsPerGroup && p < W / 2u;
        const uint col = (p == 0u) ? ((slot == 0u) ? 0u : W / 2u) : ((slot == 0u) ? p : W - p);
        const uint partnerSlot = (p == 0u) ? slot : (1u - slot);
        const uint base = (pairLocal * 2u + slot) * L;
        const uint pbase = (pairLocal * 2u + partnerSlot) * L;
        const uint pcol = (W - col) % W;

        float4 a = 0.0f.xxxx;
        float4 b = 0.0f.xxxx;
        if (active)
        {
            if (loadLimit == 0u || i < loadLimit) { a = SrcSpectrum[uint2(col, i)]; }
            if (loadLimit == 0u || i + halfL < loadLimit) { b = SrcSpectrum[uint2(col, i + halfL)]; }
        }
        StockhamLine(a, b, base, i, L, active, false);

        // Both columns' spectra are in LDS in natural order (the last stage's writes, its reads fenced).
        float4 pa = 0.0f.xxxx;
        float4 pb = 0.0f.xxxx;
        if (active)
        {
            const float3 invNorm = KernelInvNorm();
            const uint ya = i;
            const uint yb = i + halfL;
            const uint ma = (L - ya) % L;
            const uint mb = (L - yb) % L;
            pa = SpectralProduct(a, FftShared[pbase + ma], KernelSpectrum[uint2(col, ya)], KernelSpectrum[uint2(pcol, ma)], invNorm);
            pb = SpectralProduct(b, FftShared[pbase + mb], KernelSpectrum[uint2(col, yb)], KernelSpectrum[uint2(pcol, mb)], invNorm);
        }
        GroupMemoryBarrierWithGroupSync(); // every partner read done before the inverse writes over the spectra
        StockhamLine(pa, pb, base, i, L, active, true);
        if (!active) { return; }
        const float norm = 1.0f / (float)L;
        if (writeLimit == 0u || i < writeLimit) { DstSpectrum[uint2(col, i)] = pa * norm; }
        if (writeLimit == 0u || i + halfL < writeLimit) { DstSpectrum[uint2(col, i + halfL)] = pb * norm; }
        return;
    }

    // ---- mode 0: one axis, as many lines a group as fill it ----
    const uint length = (isVertical != 0u) ? transformSize.y : transformSize.x;
    const uint half_ = length / 2u;
    const uint linesPerGroup = max(kThreads / half_, 1u);
    const uint lineLocal = thread / half_;
    const uint i = thread - lineLocal * half_;
    const uint allLines = (isVertical != 0u) ? transformSize.x : transformSize.y;
    const uint lines = (lineCount != 0u) ? min(lineCount, allLines) : allLines;
    const uint line_ = groupId.x * linesPerGroup + lineLocal; // which row (or column) these threads own
    // Threads past their line's half-length, or past the last line, have nothing to own. They must still
    // reach every barrier, so they are not returned early -- they simply carry zeroes.
    const bool active = lineLocal < linesPerGroup && line_ < lines;
    const uint base = lineLocal * length;

    // ---- load: element i and element i + half, which is what a radix-2 Stockham stage consumes at Ns = 1 ----
    float4 a = 0.0f.xxxx;
    float4 b = 0.0f.xxxx;
    if (active)
    {
        const uint2 ca = (isVertical != 0u) ? uint2(line_, i) : uint2(i, line_);
        const uint2 cb = (isVertical != 0u) ? uint2(line_, i + half_) : uint2(i + half_, line_);
        if (loadLimit == 0u || i < loadLimit) { a = SrcSpectrum[ca]; }
        if (loadLimit == 0u || i + half_ < loadLimit) { b = SrcSpectrum[cb]; }
    }
    StockhamLine(a, b, base, i, length, active, isInverse != 0u);
    if (!active)
    {
        return;
    }

    // 1/N is applied on the INVERSE pass only, once per axis, which together give the 1/(Nx*Ny) a
    // round trip needs. Splitting it across the two inverse passes rather than applying it all at
    // the end keeps the intermediate magnitudes in range for a 16-bit float target.
    const float norm = (isInverse != 0u) ? (1.0f / (float)length) : 1.0f;

    const uint2 outA = (isVertical != 0u) ? uint2(line_, i) : uint2(i, line_);
    const uint2 outB = (isVertical != 0u) ? uint2(line_, i + half_) : uint2(i + half_, line_);
    if (writeLimit == 0u || i < writeLimit) { DstSpectrum[outA] = a * norm; }
    if (writeLimit == 0u || i + half_ < writeLimit) { DstSpectrum[outB] = b * norm; }
}
