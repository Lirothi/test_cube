// A boat's wake -- compute kernels (ocean/OceanWake.h says why the field is spectral and what is Crest's).
//
// One step: Pack (space) -> the forward FFT (ocean_fft.hlsl) -> Step (spectrum) -> two inverse FFTs -> Compose (space).
//   Pack:    last step's field (x height, y vertical velocity), moved by the window's re-anchor, absorbed toward the
//            border, and this step's hull pressure in z. The FFT turns x + iy = h + iv and z + iw = pressure + 0i together.
//   Step:    every Fourier mode of (h, v) turned through omega * dt about the equilibrium the pressure sets (h = -head:
//            the water under a hull at rest sinks by its draft), damped as k^2 -- once for each of four depths,
//            omega^2 = g k tanh(k d), d = 0.3, 1.5, 7.5 m and deep, two depths to a texture (xy, zw): 0.3 and 1.5 m into Field
//            itself (its last state is in Work by then), 9 m and deep into Spec.
//   Compose: each texel takes the two depths its own water lies between (the shore depth map, log-interpolated) -- the
//            spectrum has one depth for the whole window, the water does not: a wave slows over the shallows -- and land
//            holds the field at 0, so a wave meets the coast instead of crossing the island. It reads and writes its
//            own texel of Field only. It also writes the FREE height, h + head (0 under a hull at rest), for the CPU's
//            copy (the buoyancy).
// The window is square, world-axis-aligned, no v flip: worldXZ = centre + (coord + 0.5 - N/2) * texel.
#define OCEAN_WAKE_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP)"

#define MAX_HULLS 16

cbuffer WakeCB : register(b0)
{
    uint  Resolution;     // texels a side (the FFT's size)
    float Texel;          // metres a texel
    float CenterX;        // window centre, world XZ
    float CenterZ;

    int   ShiftX;         // the re-anchor: this texel was (coord + shift) last step
    int   ShiftY;
    float DeltaTime;      // seconds this step
    uint  Clear;          // 1 = last step's field is not read (the first step, a teleport, a new texel size)

    float Gravity;        // m/s2
    float DampingK2;      // 1/s per (rad/m)^2: the shortest wave's damping over its k^2
    float Strength;       // the hull pressure's scale (1 = the head its draft stands for)
    uint  HullCount;

    float EdgeTexels;     // the absorber's width
    float EdgeRate;       // 1/s at the border, falling as the square of the distance in
    uint  ShoreValid;     // 0 = no shore map this frame: deep water everywhere
    float ShoreCamHeight; // the shore depth map: waterDepth = lerp(zNear, zFar, raw) - camHeight

    float ShoreCenterX;   // shore depth map window (the surf sim's, ShoreDepthUV's): v flipped
    float ShoreCenterZ;
    float ShoreInvExtent; // uv = offset * invExtent + 0.5
    float ShoreZNear;

    float ShoreZFar;
    float BreakRate;      // 1/s: a long wave in the shallows (d < wavelength / 20) runs down at it as d goes to 0; 0 = none
    float Pad1;
    float Pad2;

    // Per hull, three float4: (posX, posZ, fwdX, fwdZ), (scale, u0, v0, 1/cellU), (1/cellV, along, across, offset).
    float4 Hulls[MAX_HULLS * 3];
};

// The four depths the spectrum steps at (m); the last is deep water (tanh = 1). A texel deeper than kDeepFrom takes it alone.
static const float kDepths[3] = { 0.3f, 1.5f, 7.5f };
static const float kDeepFrom = 37.5f;

StructuredBuffer<float> Footprints : register(t0); // the hulls' depth grids (buoyancy::HullFootprint), metres
Texture2D<float> ShoreDepthTex : register(t1);     // the ocean's shore depth map
SamplerState ClampSampler : register(s0);
RWTexture2D<float4> Work : register(u0);
RWTexture2D<float4> Field : register(u1);  // x height, y vertical velocity, z free height (h + head), w head;
                                           // between Step and Compose: depths 1 and 3 m, (h + iv) each
RWTexture2D<float4> Spec : register(u2);   // 9 m and deep
RWTexture2D<float> Free : register(u3);    // the free height alone, for the CPU's copy (R16F)

float2 WorldOf(uint2 coord)
{
    return float2(CenterX, CenterZ) + (float2(coord) + 0.5f - 0.5f * (float)Resolution) * Texel;
}

// The footprint between its cell centres, 0 off its grid (`gu`, `gv` in cells from its first edge).
float FootprintAt(uint offset, uint along, uint across, float gu, float gv)
{
    const float fu = gu - 0.5f;
    const float fv = gv - 0.5f;
    const int i0 = (int)floor(fu);
    const int j0 = (int)floor(fv);
    const float tu = fu - (float)i0;
    const float tv = fv - (float)j0;
    float sum = 0.0f;
    [unroll] for (int dj = 0; dj < 2; ++dj)
    {
        [unroll] for (int di = 0; di < 2; ++di)
        {
            const int i = i0 + di;
            const int j = j0 + dj;
            if (i < 0 || j < 0 || i >= (int)along || j >= (int)across)
            {
                continue;
            }
            const float w = (di != 0 ? tu : 1.0f - tu) * (dj != 0 ? tv : 1.0f - tv);
            sum += w * Footprints[offset + (uint)j * along + (uint)i];
        }
    }
    return sum;
}

// Metres of water head the hulls press down at `xz`.
float HullPressure(float2 xz)
{
    float head = 0.0f;
    for (uint h = 0; h < HullCount; ++h)
    {
        const float4 a = Hulls[h * 3u + 0u];
        const float4 b = Hulls[h * 3u + 1u];
        const float4 c = Hulls[h * 3u + 2u];
        const float2 rel = xz - a.xy;
        const float u = dot(rel, a.zw) / b.x;
        const float v = (-rel.x * a.w + rel.y * a.z) / b.x; // the footprint's across axis: (-fz, fx)
        const float gu = (u - b.y) * b.w;
        const float gv = (v - b.z) * c.x;
        if (gu < -1.0f || gv < -1.0f || gu > c.y + 1.0f || gv > c.z + 1.0f)
        {
            continue;
        }
        head += FootprintAt((uint)c.w, (uint)c.y, (uint)c.z, gu, gv) * b.x;
    }
    return head * Strength;
}

// Metres of water at `world` from the shore depth map (the surf sim's WaterDepthAt): deep off the map, on its far plane
// (the terrain pass drew nothing) or without a map this frame; negative on land.
float WaterDepthAt(float2 world)
{
    if (ShoreValid == 0u)
    {
        return 1.0e4f;
    }
    const float2 offset = world - float2(ShoreCenterX, ShoreCenterZ);
    const float2 uv = float2(offset.x * ShoreInvExtent + 0.5f, 0.5f - offset.y * ShoreInvExtent);
    if (any(uv < 0.0f) || any(uv > 1.0f))
    {
        return 1.0e4f;
    }
    const float raw = ShoreDepthTex.SampleLevel(ClampSampler, uv, 0);
    if (raw >= 1.0f - 1e-6f)
    {
        return 1.0e4f;
    }
    return lerp(ShoreZNear, ShoreZFar, raw) - ShoreCamHeight;
}

[numthreads(8, 8, 1)]
[RootSignature(OCEAN_WAKE_RS)]
void Pack(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Resolution || id.y >= Resolution)
    {
        return;
    }
    float2 hv = float2(0.0f, 0.0f);
    if (Clear == 0u)
    {
        const int2 src = int2(id.xy) + int2(ShiftX, ShiftY);
        if (all(src >= 0) && all(src < (int)Resolution))
        {
            hv = Field[uint2(src)].xy;
        }
    }
    // The absorber: the spectrum is periodic, so what reaches the border must be gone before it wraps round. A rate,
    // not Crest's per-step factor (lerp(0.95, 1, edge) a step), so it absorbs the same at any frame time.
    const float edge = (float)min(min(id.x, id.y), min(Resolution - 1u - id.x, Resolution - 1u - id.y));
    const float inside = saturate(edge / EdgeTexels);
    hv *= exp(-EdgeRate * DeltaTime * (1.0f - inside) * (1.0f - inside));
    if (edge < 2.0f || any(!isfinite(hv))) // Crest's NaN guard: one bad value would spread over the whole spectrum
    {
        hv = float2(0.0f, 0.0f);
    }
    Work[id.xy] = float4(hv, HullPressure(WorldOf(id.xy)), 0.0f);
}

// One mode turned through omega * dt about the equilibrium; returns h' + i v' as one complex number.
float2 Turn(float2 h, float2 v, float2 head, float omega, float decay)
{
    float s, c;
    sincos(omega * DeltaTime, s, c);
    const float sinc = omega > 1e-5f ? s / omega : DeltaTime;
    // u = h + head: the departure from the equilibrium h = -head. It turns as a harmonic oscillator of frequency omega;
    // the pressure, held over the step, only moves the point it turns about -- exact for any dt.
    const float2 u = h + head;
    const float2 u1 = decay * (u * c + v * sinc);
    const float2 v1 = decay * (v * c - u * (omega * s));
    const float2 h1 = u1 - head;
    return float2(h1.x - v1.y, h1.y + v1.x);
}

[numthreads(8, 8, 1)]
[RootSignature(OCEAN_WAKE_RS)]
void Step(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Resolution || id.y >= Resolution)
    {
        return;
    }
    const uint2 n = id.xy;
    const uint2 m = (Resolution.xx - n) % Resolution; // -k
    const float4 here = Work[n];
    const float2 zm = Work[m].xy;
    // Two real fields in one complex FFT: Z = FFT(h + i v), so H(k) = (Z(k) + conj Z(-k)) / 2, V(k) = (Z(k) - conj Z(-k)) / 2i.
    const float2 z = here.xy;
    const float2 head = here.zw; // the pressure head's spectrum (a real field: no unpacking)
    const float2 h = 0.5f * (z + float2(zm.x, -zm.y));
    const float2 d = z - float2(zm.x, -zm.y);
    const float2 v = float2(0.5f * d.y, -0.5f * d.x);

    const int2 kn = int2(n) - int2(n >= (Resolution / 2u).xx) * (int)Resolution;
    const float2 k = float2(kn) * (6.28318530718f / ((float)Resolution * Texel));
    const float kk = dot(k, k);
    const float kl = sqrt(kk);
    const float decay = exp(-DampingK2 * kk * DeltaTime);
    const float deep = sqrt(Gravity * kl);
    // Finite depth: omega^2 = g k tanh(k d). The same mode, turned four times; the inverse FFT's 1 / N^2 folded in.
    // The surf zone: Crest's Attenuation In Shallows (UpdateDynWaves.compute, 1 - (1 - saturate(2 d / wavelength)) dt 2 a
    // step) starts where ITS deep-water waves stop being right, d < wavelength / 2 -- over a 3 m shelf it took a 13 m wake
    // wave away in seconds, where these waves, finite-depth already, cross it as they should. Here it starts where a wave
    // is a long wave running up a beach, d < wavelength / 20 (k d < pi / 10), at Crest's rate as d goes to 0: what reaches
    // a beach breaks there instead of meeting the waterline whole and coming back off it.
    const float scale = 1.0f / ((float)Resolution * (float)Resolution);
    const float surf = BreakRate * DeltaTime;
    const float2 z0 = Turn(h, v, head, sqrt(Gravity * kl * tanh(kl * kDepths[0])),
                           decay * exp(-surf * (1.0f - saturate(kDepths[0] * kl * (10.0f / 3.14159265f)))));
    const float2 z1 = Turn(h, v, head, sqrt(Gravity * kl * tanh(kl * kDepths[1])),
                           decay * exp(-surf * (1.0f - saturate(kDepths[1] * kl * (10.0f / 3.14159265f)))));
    const float2 z2 = Turn(h, v, head, sqrt(Gravity * kl * tanh(kl * kDepths[2])), decay);
    const float2 z3 = Turn(h, v, head, deep, decay);
    Field[n] = float4(z0, z1) * scale;
    Spec[n] = float4(z2, z3) * scale;
}

[numthreads(8, 8, 1)]
[RootSignature(OCEAN_WAKE_RS)]
void Compose(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Resolution || id.y >= Resolution)
    {
        return;
    }
    const float2 xz = WorldOf(id.xy);
    const float depth = WaterDepthAt(xz);
    const float4 a = Field[id.xy];
    const float4 b = Spec[id.xy];
    // The two depths this water lies between, in log: 0.3 -> 1.5 -> 7.5 -> deep (37.5 m and over). Shallower takes 0.3 m's.
    float2 hv;
    if (depth >= kDeepFrom)
    {
        hv = b.zw;
    }
    else if (depth >= kDepths[2])
    {
        hv = lerp(b.xy, b.zw, saturate(log(depth / kDepths[2]) / log(kDeepFrom / kDepths[2])));
    }
    else if (depth >= kDepths[1])
    {
        hv = lerp(a.zw, b.xy, saturate(log(depth / kDepths[1]) / log(kDepths[2] / kDepths[1])));
    }
    else if (depth >= kDepths[0])
    {
        hv = lerp(a.xy, a.zw, saturate(log(depth / kDepths[0]) / log(kDepths[1] / kDepths[0])));
    }
    else
    {
        hv = a.xy;
    }
    // Land holds the field at 0: the coast meets a wave, the island does not let it through. The last 10 cm of water ramp
    // it, so the waterline is no step.
    hv *= saturate(depth / 0.1f);
    const float head = HullPressure(xz);
    if (any(!isfinite(hv)))
    {
        hv = float2(0.0f, 0.0f);
    }
    Field[id.xy] = float4(hv, hv.x + head, head);
    Free[id.xy] = hv.x + head;
}
