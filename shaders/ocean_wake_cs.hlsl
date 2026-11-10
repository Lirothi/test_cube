// A boat's wake -- compute kernels (ocean/OceanWake.h says why the field is spectral and what is Crest's).
//
// One step: Pack (space) -> the forward FFT (ocean_fft.hlsl) -> Step (spectrum) -> two inverse FFTs -> Compose (space).
//   Pack:    last step's field (x height, y vertical velocity), moved by the window's re-anchor, absorbed toward the
//            border, and this step's hull pressure in z. The FFT turns x + iy = h + iv and z + iw = pressure + 0i together.
//   Step:    every Fourier mode of (h, v) turned through omega * dt about the equilibrium the pressure sets (h = -head:
//            the water under a hull at rest sinks by its draft), damped as k^2 -- once for each of four depths,
//            omega^2 = g k tanh(k d), d = 0.3, 1.5, 7.5 m and deep, two depths to a texture (xy, zw): 0.3 and 1.5 m into Field
//            itself (its last state is in Work by then), 7.5 m and deep into Spec. And the free height's Jacobian terms
//            into Jac (a third inverse FFT), for the crest foam.
//   Compose: each texel takes the two depths its own water lies between (the shore depth map, log-interpolated) -- the
//            spectrum has one depth for the whole window, the water does not: a wave slows over the shallows -- and land
//            holds the field at 0, so a wave meets the coast instead of crossing the island. It reads and writes its
//            own texel of Field only. It also writes the FREE height, h + head (0 under a hull at rest), for the CPU's
//            copy (the buoyancy).
// The FOAM (2026-11-07, owner: "both" -- white water at the hull and its trail, foam on the steep crests) is a field of its
// own in the same window, two textures in turn (Pack reads last step's moved by the re-anchor, which one texture could not
// give it): Pack moves, spreads and runs it down and lays the white water a moving hull leaves, where the head under it
// changes (its bow arriving, its transom leaving), and the two arms of its breaking bow wave (the fronts: lines through
// points the CPU runs outward from the bow's shoulders); Compose adds Crest's wave foam where the displaced surface pinches.
// The surface tears it per pixel as it does the surf sim's (ocean_surface_surf_sim.hlsli WakeFoamCoverage).
// The window is square, world-axis-aligned, no v flip: worldXZ = centre + (coord + 0.5 - N/2) * texel.
#define OCEAN_WAKE_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, numDescriptors=7, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP)"

#define MAX_HULLS 16
// The breaking bow waves' fronts: OceanWake::kMaxFronts / kMaxFrontPoints -- MUST MATCH.
#define MAX_FRONTS 8
#define MAX_FRONT_POINTS 128
#define FRONT_PIECE 16 // segments a piece of a front, each with its own box (OceanWake::kFrontPiece -- MUST MATCH)
#define FRONT_PIECES (MAX_FRONT_POINTS / FRONT_PIECE)
// The breaker's band across its front, in half widths: its toe ahead of the line (outward, sharp) and its tail behind
// (OceanWake.cpp kFrontToe / kFrontTail -- MUST MATCH).
#define FRONT_TOE 0.6f
#define FRONT_TAIL 1.6f

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
    float FoamFade;       // 1/s the foam runs down at
    float FoamDiffusion;  // nu dt / texel^2: the foam's spreading a step (a 4-neighbour Laplacian)

    float WashStrength;   // foam a metre of head change under a moving hull leaves (0 on a step that may not wash)
    float WashThreshold;  // m/s of head change below which a hull leaves no white water
    float CrestStrength;  // Crest's Wave Foam Strength: 5 dt x this x saturate(coverage - det J) a step
    float CrestCoverage;  // Crest's Wave Foam Coverage (0.55: det J of a Gerstner wave at the Stokes limit, a k = 0.44)

    float FoamFadeDense;  // 1/s per unit of foam: dense white water runs down fast, the lace it leaves slowly
    uint  FrontCount;     // the breaking bow waves' fronts this step (OceanWake::StepFronts), two a boat
    float WashBow;        // the share of that foam a head RISING lays (a hull's bow arriving), of what a head falling does
    float Pad3;

    // Per hull, three float4: (posX, posZ, fwdX, fwdZ), (scale, u0, v0, 1/cellU), (1/cellV, along, across, offset).
    float4 Hulls[MAX_HULLS * 3];
    float4 FrontBounds[MAX_FRONTS * FRONT_PIECES]; // (min x, min z, max x, max z) world of each piece of a front: its
                                        // FRONT_PIECE segments, the band's reach in
    float4 FrontInfo[MAX_FRONTS];       // (points, which side of the line is outward: +1 its left normal, -1 its right,
                                        // the residue's rate: x foam / half width a second, the points over which the
                                        // residue comes in from the hull: 0 = whole from the first)
    float4 FrontPoints[MAX_FRONTS * MAX_FRONT_POINTS]; // (x, z, the band's foam, half width m), newest (on the hull) first
};

// The four depths the spectrum steps at (m); the last is deep water (tanh = 1). A texel deeper than kDeepFrom takes it alone.
static const float kDepths[3] = { 0.3f, 1.5f, 7.5f };
static const float kDeepFrom = 37.5f;

StructuredBuffer<float> Footprints : register(t0); // the hulls' depth grids (buoyancy::HullFootprint), metres
Texture2D<float> ShoreDepthTex : register(t1);     // the ocean's shore depth map
SamplerState ClampSampler : register(s0);
RWTexture2D<float4> Work : register(u0);
RWTexture2D<float4> Field : register(u1);  // x height, y vertical velocity, z free height (h + head), w head;
                                           // between Step and Compose: depths 0.3 and 1.5 m, (h + iv) each
RWTexture2D<float4> Spec : register(u2);   // 7.5 m and deep
RWTexture2D<float> Free : register(u3);    // the free height alone, for the CPU's copy (R16F)
RWTexture2D<float4> Jac : register(u4);    // the free height's Jacobian terms: xy k_x^2/|k| + i k_z^2/|k|, z k_x k_z/|k|
RWTexture2D<float4> FoamPrev : register(u5); // last step's foam (RGBA16F: x the foam that stays, yz the way it was laid
                                             // times it, w the bow waves' breakers as they stand this step: not kept)
RWTexture2D<float4> FoamNext : register(u6); // this step's, which the surface reads

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

// Metres of water head the hulls press down at `xz`, and the way the hulls pressing there head (weighted by their head).
float HullPressure(float2 xz, out float2 way)
{
    float head = 0.0f;
    way = float2(0.0f, 0.0f);
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
        const float d = FootprintAt((uint)c.w, (uint)c.y, (uint)c.z, gu, gv) * b.x;
        head += d;
        way += d * a.zw;
    }
    return head * Strength;
}

float HullPressure(float2 xz)
{
    float2 way;
    return HullPressure(xz, way);
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
    float2 way;
    const float head = HullPressure(WorldOf(id.xy), way);
    float2 hv = float2(0.0f, 0.0f);
    float headBefore = head;
    float4 foam = float4(0.0f, 0.0f, 0.0f, 0.0f); // x the foam, yz the way it was laid times it (its streaks' direction)
    if (Clear == 0u)
    {
        const int2 src = int2(id.xy) + int2(ShiftX, ShiftY);
        if (all(src >= 0) && all(src < (int)Resolution))
        {
            const float4 last = Field[uint2(src)];
            hv = last.xy;
            headBefore = last.w;
            // The foam spreads (nu dt / texel^2 of the 4-neighbour Laplacian, its neighbours clamped to the window).
            const int hi = (int)Resolution - 1;
            const float4 f = FoamPrev[uint2(src)];
            const float4 around = FoamPrev[uint2(clamp(src + int2(1, 0), 0, hi))] + FoamPrev[uint2(clamp(src - int2(1, 0), 0, hi))] +
                                  FoamPrev[uint2(clamp(src + int2(0, 1), 0, hi))] + FoamPrev[uint2(clamp(src - int2(0, 1), 0, hi))];
            foam = f + FoamDiffusion * (around - 4.0f * f);
        }
    }
    // The absorber: the spectrum is periodic, so what reaches the border must be gone before it wraps round. A rate,
    // not Crest's per-step factor (lerp(0.95, 1, edge) a step), so it absorbs the same at any frame time.
    const float edge = (float)min(min(id.x, id.y), min(Resolution - 1u - id.x, Resolution - 1u - id.y));
    const float inside = saturate(edge / EdgeTexels);
    hv *= exp(-EdgeRate * DeltaTime * (1.0f - inside) * (1.0f - inside));
    // The white water a moving hull leaves: the head under it FALLING faster than WashThreshold -- its transom leaving (a
    // side, in a slide), where the water closes in behind it. A texel the whole hull passes over takes twice the depth
    // under it, less the threshold over the time it took: the trail is the hull's width, whitest on its keel. The head
    // RISING (the bow arriving) lays WashBow of that, 0 by default: it used to lay as much, a white collar round the stem
    // that stood still on the water between two steps of the field and jumped a hull's step forward at each (owner: "the
    // foam there appears abruptly, it shows even at speed") -- and a planing hull's stem is in the air; the bow's white
    // water is the arms', which come in along their first seconds.
    // Two stages, as a breaking crest's foam has (whitecap stage A, seconds; the residual patch, tens of seconds): the
    // denser the white water, the faster it goes -- a trail is solid only just behind the transom and dissolves into
    // the lace that lingers.
    // The way it was laid rides with it (yz, times the foam), so the surface draws the trail's streaks along the hull's
    // path, through a turn too.
    foam *= exp(-(FoamFade + FoamFadeDense * max(foam.x, 0.0f)) * DeltaTime);
    float breaker = 0.0f; // the bow waves' breakers at this texel, this step
    if (DeltaTime > 0.0f)
    {
        const float rise = head - headBefore;
        const float still = WashThreshold * DeltaTime;
        const float wash = 2.0f * WashStrength * (max(-rise - still, 0.0f) + WashBow * max(rise - still, 0.0f));
        const float wayLength = length(way);
        foam.x += wash;
        foam.yz += wayLength > 1e-6f ? wash * (way / wayLength) : float2(0.0f, 0.0f);
        // The breaking bow waves (OceanWake::StepFronts): each front is a line through its points, from the hull's
        // waterline out along the arm. The breaker stands ON it: a band with a sharp toe ahead of the line (outward) and a
        // tail behind, of the foam and width its points carry (both run down along the arm: the wave breaks less the
        // further it has gone) -- this step's alone (w). Under it a thinner residue goes into the foam that stays, streaked
        // along the front: the lace between the arm and the trail. The nearest piece of a front draws it -- summed, two
        // pieces would both draw their joint.
        const float2 p = WorldOf(id.xy);
        const uint fronts = min(FrontCount, (uint)MAX_FRONTS);
        [loop] for (uint f = 0u; f < (uint)MAX_FRONTS; ++f)
        {
            if (f >= fronts)
            {
                break;
            }
            const float4 info = FrontInfo[f];
            const uint count = min((uint)info.x, (uint)MAX_FRONT_POINTS);
            float best = 0.0f;
            float bestRate = 0.0f;
            float2 bestWay = float2(0.0f, 0.0f);
            // By pieces: a texel walks only the segments of the pieces whose box it lies in.
            [loop] for (uint i = 0u; i + 1u < (uint)MAX_FRONT_POINTS; ++i)
            {
                if (i + 1u >= count)
                {
                    break;
                }
                if ((i % (uint)FRONT_PIECE) == 0u)
                {
                    const float4 box = FrontBounds[f * (uint)FRONT_PIECES + i / (uint)FRONT_PIECE];
                    if (p.x < box.x || p.y < box.y || p.x > box.z || p.y > box.w)
                    {
                        i += (uint)FRONT_PIECE - 1u; // the next piece
                        continue;
                    }
                }
                const float4 a = FrontPoints[f * (uint)MAX_FRONT_POINTS + i];
                const float4 b = FrontPoints[f * (uint)MAX_FRONT_POINTS + i + 1u];
                const float2 ab = b.xy - a.xy;
                const float abLength2 = dot(ab, ab);
                const float2 way = abLength2 > 1e-6f ? ab * rsqrt(abLength2) : float2(0.0f, 0.0f);
                const float t = abLength2 > 1e-6f ? saturate(dot(p - a.xy, ab) / abLength2) : 0.0f;
                const float2 off = p - (a.xy + ab * t);
                const float halfWidth = max(lerp(a.w, b.w, t), 1e-3f);
                const float ahead = dot(off, float2(-way.y, way.x)) * info.y; // > 0: outside the arm
                const float near = saturate(1.0f - length(off) / (halfWidth * (ahead >= 0.0f ? FRONT_TOE : FRONT_TAIL)));
                const float band = lerp(a.z, b.z, t) * near * near;
                if (band > best)
                {
                    best = band;
                    // The residue comes in along the front's first points (the breaker itself is laid whole: the surface
                    // lets it in over a gate that rides with the hull, every frame).
                    const float come = info.w > 0.0f ? smoothstep(0.0f, info.w, (float)i + t - 0.5f) : 1.0f;
                    bestRate = band * come * info.z / halfWidth;
                    bestWay = way;
                }
            }
            breaker = max(breaker, best);
            foam.x += bestRate * DeltaTime;
            foam.yz += bestRate * DeltaTime * bestWay;
        }
    }
    foam.w = breaker;
    if (edge < 2.0f || any(!isfinite(hv))) // Crest's NaN guard: one bad value would spread over the whole spectrum
    {
        hv = float2(0.0f, 0.0f);
    }
    if (edge < 2.0f || any(!isfinite(foam)))
    {
        foam = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    Work[id.xy] = float4(hv, head, 0.0f);
    // Over 2 the white water is scaled down whole, its way with it (the breakers, w, are this step's own).
    const float3 kept = foam.x > 2.0f ? foam.xyz * (2.0f / foam.x) : max(foam.xyz, float3(0.0f, -2.0f, -2.0f));
    FoamNext[id.xy] = float4(kept, foam.w);
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
    // The Jacobian of the free height's choppy displacement (Tessendorf's D = i k / |k| F with lambda 1: a Gerstner wave's
    // particle paths, crowding into its crests), for Compose's crest foam: J = I - [[jxx, jxz], [jxz, jzz]] with
    // j_ab = k_a k_b / |k| F. The free height, so a hull at rest wears no ring of foam round its own hollow.
    const float2 F = h + head;
    const float invK = kl > 1e-6f ? 1.0f / kl : 0.0f;
    const float2 jxx = F * (k.x * k.x * invK);
    const float2 jzz = F * (k.y * k.y * invK);
    const float2 jxz = F * (k.x * k.y * invK);
    Jac[n] = float4(jxx.x - jzz.y, jxx.y + jzz.x, jxz) * scale; // two real fields as one complex: jxx + i jzz
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

    // Crest's wave foam (UpdateFoam.compute:95-102): where the displaced surface pinches, det J under the coverage --
    // 0.55 is a Gerstner wave at the Stokes limit, a k = 0.44 -- the crest breaks. Land holds none.
    const float4 j = Jac[id.xy];
    const float det = (1.0f - j.x) * (1.0f - j.y) - j.z * j.z;
    // The crest's foam keeps the way the trail's was laid (it adds none: away from a hull nothing says which way).
    float4 foam = FoamNext[id.xy];
    foam.x += 5.0f * DeltaTime * CrestStrength * saturate(CrestCoverage - det);
    foam *= saturate(depth / 0.1f);
    FoamNext[id.xy] = all(isfinite(foam)) ? float4(foam.x > 2.0f ? foam.xyz * (2.0f / foam.x) : foam.xyz, foam.w)
                                          : float4(0.0f, 0.0f, 0.0f, 0.0f);
}
