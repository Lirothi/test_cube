// Nearshore surf simulation - compute kernels (docs/ocean_surf_sim_plan.md).
//
// S1: `Update` advances the classic five-point-Laplacian height/velocity wave equation one
// FIXED substep at a time (Crest's UpdateDynWaves numerics, docs/ref/crest/), with the wave
// speed from the shore depth map: c^2 = g*depth, the shallow-water celerity - fronts slow over
// the shallows and bend parallel to the shoreline for free. Land, the window border and open
// water absorb instead of reflecting.
//
// S2: `Spawn` places a disturbance SEGMENT via the shore SDF: the CPU throws a random candidate
// point into the window, this kernel walks it to the waterline along the SDF gradient, backs off
// seaward by the spawn distance and orients the segment ALONG the shore. `Update` integrates
// every live slot as a capsule-Gaussian forcing with a sin^1 envelope whose time integral is
// exactly the authored amplitude - the hump inflates smoothly and departs as a wave. Discrete,
// local, randomized events: this is what keeps the surf from ever being an isobath ring
// (the plan's invariant 1).
//
// The window is square, world-axis-aligned: worldXZ = center + (coord + 0.5) * texel - halfExtent.
// Spawner slots hold WORLD positions, so a window re-anchor does not disturb them.
#define OCEAN_SURF_SIM_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, numDescriptors=5, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP), StaticSampler(s1, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_WRAP)"

#define MAX_SPAWNERS 8u

cbuffer SurfSimCB : register(b0)
{
    uint  Resolution;      // texels per side
    float TexelWorldSize;  // metres per texel
    float CenterX;         // window centre, world XZ (also the poke position)
    float CenterZ;

    float Time;            // seconds (the ocean's simulation clock)
    float DeltaTime;       // the FIXED substep, seconds (0 for Spawn/Relocate)
    int   ShiftX;          // Relocate only: texel shift, src = dst + shift
    int   ShiftY;

    float ShoreCenterX;    // shore depth map window (matches ShoreDepthUV in the surface shader)
    float ShoreCenterZ;
    float ShoreInvExtent;  // uv = offset * invExtent + 0.5
    float ShoreZNear;      // depth decode: viewDepth = lerp(zNear, zFar, raw)

    float ShoreZFar;
    float ShoreCamHeight;  // ortho camera height; waterDepth = viewDepth - camHeight
    float PokeAmp;         // metres; != 0 on the substep that injects the test hump
    float SdfCenterX;      // shore SDF placement (matches ShoreSdfUV in the modern surface)

    float SdfCenterZ;
    float SdfInvExtent;
    float SpawnCandX;      // Spawn only: the CPU's random candidate point
    float SpawnCandZ;

    uint  SpawnSlot;       // Spawn only: round-robin slot to (over)write
    float SpawnDistance;   // metres seaward of the waterline
    float SegmentHalfLen;  // metres, along-shore half length of the disturbance
    float SpawnAmp;        // metres of total injected height

    float SpawnDuration;   // seconds of forcing (the sin envelope's period)
    float SpawnSigma;      // metres, across-segment Gaussian width
    float DepositStrength; // S3: peak foam a breaking crest stamps (FFT-foam max() semantics)
    float BreakerGamma;    // S3: surf breaker index H/d (~0.78, McCowan)

    float FoamFadeRate;    // S3: foam/s LINEAR decay behind the crest (the FFT-foam pattern)
    float FrontBreakup;    // S3: 0..1 tear of the foam at CONSUMPTION (S4) — never in the sim,
                           // whose ~1 m texels alias the pattern into per-texel noise
    float Pad0;
    float Pad1;
};

Texture2D<float>    ShoreDepthTex : register(t0);
Texture2D<float2>   ShoreSdfTex : register(t1); // x: metres to the waterline (negative inland)
Texture2D<float4>   BreakupTex : register(t2);  // S3: the shared ContactFoam breakup pattern
SamplerState        ClampSampler : register(s0);
SamplerState        WrapSampler : register(s1);

// x: height (metres), y: vertical velocity (m/s).
RWTexture2D<float2> WaveRead  : register(u0);
RWTexture2D<float2> WaveWrite : register(u1);
RWTexture2D<float>  FoamRead  : register(u2);
RWTexture2D<float>  FoamWrite : register(u3);

// posDir: xy = segment centre (world), zw = SHOREWARD normal (unit; the along-shore direction
// is its perpendicular). params: x = half length (m), y = amplitude (m), z = birth time (s),
// w = duration (s; <= 0 = free).
struct SpawnerSlot { float4 posDir; float4 params; };
RWStructuredBuffer<SpawnerSlot> Spawners : register(u4);

float2 WindowWorldPos(uint2 coord)
{
    const float halfExtent = 0.5f * TexelWorldSize * (float)Resolution;
    return float2(CenterX, CenterZ) +
        (float2(coord) + 0.5f) * TexelWorldSize - halfExtent;
}

// Vertical water depth (metres) at a world position, from the shore depth map. Negative on
// land. Outside the map's window, or where the top-down pass drew nothing, the water counts as
// deep - the sim only cares about the nearshore.
float WaterDepthAt(float2 world)
{
    const float kDeep = 20.0f;
    float2 offset = world - float2(ShoreCenterX, ShoreCenterZ);
    float2 uv = float2(offset.x * ShoreInvExtent + 0.5f, 0.5f - offset.y * ShoreInvExtent);
    if (any(uv < 0.0f) || any(uv > 1.0f))
    {
        return kDeep;
    }
    float raw = ShoreDepthTex.SampleLevel(ClampSampler, uv, 0);
    if (raw >= 1.0f - 1e-6f)
    {
        return kDeep; // far plane: the terrain pass drew nothing here - open water
    }
    float viewDepth = lerp(ShoreZNear, ShoreZFar, raw);
    return viewDepth - ShoreCamHeight;
}

float2 SdfUV(float2 world)
{
    float2 offset = world - float2(SdfCenterX, SdfCenterZ);
    return float2(offset.x * SdfInvExtent + 0.5f, 0.5f - offset.y * SdfInvExtent);
}

[numthreads(8, 8, 1)]
[RootSignature(OCEAN_SURF_SIM_RS)]
void Update(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 coord = dispatchThreadId.xy;
    if (coord.x >= Resolution || coord.y >= Resolution)
    {
        return;
    }

    const float2 wave = WaveRead[coord];
    float h = wave.x;
    float v = wave.y;

    // Five-point Laplacian, edge-clamped (the border absorber below owns the boundary).
    const int2 c = int2(coord);
    const int maxC = (int)Resolution - 1;
    const float hxm = WaveRead[uint2(max(c.x - 1, 0), c.y)].x;
    const float hxp = WaveRead[uint2(min(c.x + 1, maxC), c.y)].x;
    const float hym = WaveRead[uint2(c.x, max(c.y - 1, 0))].x;
    const float hyp = WaveRead[uint2(c.x, min(c.y + 1, maxC))].x;
    const float laplacian = hxm + hxp + hym + hyp - 4.0f * h;

    const float2 world = WindowWorldPos(coord);
    const float depth = WaterDepthAt(world);

    // Shallow-water celerity c^2 = g*depth, clamped by the CFL bound so an outlier depth can
    // never blow the integration up (0.7 * dx / dt; generous at our texel/substep, kept anyway).
    const float cfl = 0.7f * TexelWorldSize / max(DeltaTime, 1e-4f);
    float c2 = 9.81f * max(depth, 0.0f);
    c2 = min(c2, cfl * cfl);

    v += c2 * laplacian / (TexelWorldSize * TexelWorldSize) * DeltaTime;

    // Absorbers, not reflectors: land eats the wave (the run-up is S3's foam, not a bounce),
    // and the window border eats it so a wave leaving the domain never echoes back. The land
    // absorber hugs the WATERLINE (8 cm) — its first cut at 20 cm swallowed the whole breaking
    // zone (h > gamma*depth is reachable at 15..40 cm for our wave heights), which is why S3's
    // first gate produced ZERO foam.
    const float landFade = saturate(depth / 0.08f);
    const int borderTexels = min(min(c.x, c.y), min(maxC - c.x, maxC - c.y));
    const float borderFade = saturate((float)borderTexels / 24.0f);
    const float fade = min(landFade, borderFade);
    const float kBaseDamping = 0.15f;   // 1/s, open-water settle rate
    const float kAbsorbDamping = 10.0f; // 1/s inside an absorber
    v *= max(0.0f, 1.0f - lerp(kAbsorbDamping, kBaseDamping, fade) * DeltaTime);

    h += v * DeltaTime;
    // Heights decay too where absorbed, so land never accumulates a standing sheet.
    h *= lerp(max(0.0f, 1.0f - 8.0f * DeltaTime), 1.0f, fade);

    // S2: spawner forcing. Each live segment is a capsule Gaussian with a sin envelope whose
    // TIME INTEGRAL equals the authored amplitude (int sin(pi*tau) dtau * pi/2 = 1), so the
    // hump inflates smoothly over the duration and departs as a wave. DIRECTED: injecting
    // height alone splits d'Alembert-style into equal shoreward and seaward waves, so the
    // matched velocity pair v = -c * dh/dn is injected with it — the seaward half cancels and
    // the packet runs at the beach (exactly for constant c; near-exactly over our depths).
    [loop]
    for (uint s = 0; s < MAX_SPAWNERS; ++s)
    {
        const SpawnerSlot slot = Spawners[s];
        if (slot.params.w <= 0.0f)
        {
            continue;
        }
        const float tau = (Time - slot.params.z) / slot.params.w;
        if (tau <= 0.0f || tau >= 1.0f)
        {
            continue;
        }
        const float2 toShore = slot.posDir.zw;
        const float2 alongDir = float2(-toShore.y, toShore.x);
        const float2 rel = world - slot.posDir.xy;
        const float along = clamp(dot(rel, alongDir), -slot.params.x, slot.params.x);
        const float2 closest = slot.posDir.xy + alongDir * along;
        const float2 d = world - closest;
        const float sAcross = dot(d, toShore); // signed metres shoreward of the segment
        const float sigma2 = SpawnSigma * SpawnSigma;
        const float gauss = exp(-dot(d, d) / (2.0f * sigma2));
        const float forcing =
            slot.params.y * sin(3.14159265f * tau) * (3.14159265f / (2.0f * slot.params.w));
        const float cLocal = sqrt(9.81f * max(depth, 0.1f));
        h += forcing * gauss * DeltaTime;
        v += forcing * cLocal * (sAcross / sigma2) * gauss * DeltaTime;
    }

    // Poke (S1 gate): a Gaussian hump at the window centre, injected on exactly one substep.
    if (PokeAmp != 0.0f)
    {
        const float2 d = world - float2(CenterX, CenterZ);
        const float kSigma = 3.0f; // metres
        h += PokeAmp * exp(-dot(d, d) / (2.0f * kSigma * kSigma));
    }

    WaveWrite[coord] = float2(h, v);

    // S3: crest-provoked foam, mirroring the FFT whitecap sim (ocean_foam_simulation.hlsl):
    // the crest's instantaneous breaking activity STAMPS the field through max() and the trail
    // it leaves decays LINEARLY behind it — foam follows every wave shoreward and dissipates,
    // no matter how few substeps the front spends over a texel (the previous additive
    // deposit-per-second left only faint stamps behind a fast front). Activity ramps in as the
    // wave shoals toward the breaker index (H/d ~ gamma, McCowan) and squares so the stamp
    // peaks at the actual break — still an event tied to THIS wave at THIS spot, never a
    // function of depth alone (the plan's invariant 1).
    // The field stays a SMOOTH physical quantity: the FrontBreakup tear is applied at
    // consumption (S4), per PIXEL — sampled here at the sim's ~1 m texel the pattern aliases
    // into per-texel noise, and bilinear magnification renders every noisy texel as a soft
    // axis-aligned square.
    const float breakDepth = max(depth, 0.1f); // the waterline would divide by zero otherwise
    const float overload = h / (BreakerGamma * breakDepth);
    const float kOnset = 0.5f; // fraction of the breaker index where whitecapping starts
    float current = saturate((overload - kOnset) / (1.0f - kOnset));
    current *= current * saturate(h / 0.02f) * DepositStrength; // ripples never foam
    const float foam = max(current, FoamRead[coord] - FoamFadeRate * DeltaTime);
    FoamWrite[coord] = min(foam, 1.5f);
}

// S2: refine the CPU's random candidate against the shore SDF and (over)write one spawner slot.
// One thread does the work; the kernel keeps the shared numthreads(8,8,1) contract of
// RecordComputeDispatch.
[numthreads(8, 8, 1)]
[RootSignature(OCEAN_SURF_SIM_RS)]
void Spawn(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x != 0 || dispatchThreadId.y != 0)
    {
        return;
    }

    const float2 cand = float2(SpawnCandX, SpawnCandZ);
    const float2 uv = SdfUV(cand);
    if (any(uv < 0.02f) || any(uv > 0.98f))
    {
        return; // candidate outside the SDF - no coast information, skip this beat
    }
    const float dist = ShoreSdfTex.SampleLevel(ClampSampler, uv, 0).x;
    if (dist < -20.0f || dist > 400.0f)
    {
        return; // deep inland or far offshore - no surf to seed here
    }

    // SDF gradient by central differences (2 texels apart in UV; only the direction matters).
    // uv.y runs against worldZ, so the Z component flips sign.
    const float kEps = 2.0f / 1024.0f;
    const float dxp = ShoreSdfTex.SampleLevel(ClampSampler, uv + float2(kEps, 0.0f), 0).x;
    const float dxm = ShoreSdfTex.SampleLevel(ClampSampler, uv - float2(kEps, 0.0f), 0).x;
    const float dyp = ShoreSdfTex.SampleLevel(ClampSampler, uv + float2(0.0f, kEps), 0).x;
    const float dym = ShoreSdfTex.SampleLevel(ClampSampler, uv - float2(0.0f, kEps), 0).x;
    float2 grad = float2(dxp - dxm, -(dyp - dym)); // points SEAWARD (distance grows offshore)
    const float gradLen = length(grad);
    if (gradLen < 1e-4f)
    {
        return; // flat field (window border padding) - no reliable shore direction
    }
    grad /= gradLen;

    const float2 shorePoint = cand - grad * dist;
    const float2 spawnPos = shorePoint + grad * SpawnDistance;
    const float2 toShore = -grad; // the direction the injected packet travels

    SpawnerSlot slot;
    slot.posDir = float4(spawnPos, toShore);
    slot.params = float4(SegmentHalfLen, SpawnAmp, Time, SpawnDuration);
    Spawners[SpawnSlot] = slot;
}

[numthreads(8, 8, 1)]
[RootSignature(OCEAN_SURF_SIM_RS)]
void Relocate(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 coord = dispatchThreadId.xy;
    if (coord.x >= Resolution || coord.y >= Resolution)
    {
        return;
    }

    // worldPos(dst, newCenter) == worldPos(src, oldCenter)  =>  src = dst + (new - old) / texel.
    // Spawner slots hold world positions and are untouched by a NORMAL re-anchor.
    const int2 src = int2(coord) + int2(ShiftX, ShiftY);
    const bool inside =
        src.x >= 0 && src.y >= 0 && src.x < (int)Resolution && src.y < (int)Resolution;
    WaveWrite[coord] = inside ? WaveRead[uint2(src)] : float2(0.0f, 0.0f);
    FoamWrite[coord] = inside ? FoamRead[uint2(src)] : 0.0f;

    // A shift >= Resolution is the FULL-CLEAR marker (first frame after creation): committed
    // heaps are NOT guaranteed zeroed and the foam pair proved it the hard way — garbage/NaN
    // survived every `read * fade + deposit` frame because NaN eats arithmetic, and
    // saturate(NaN) = 0 hid it as permanent blackness. The spawner slots come from the same
    // heap, so they are scrubbed here too.
    if (ShiftX >= (int)Resolution && coord.y == 0 && coord.x < MAX_SPAWNERS)
    {
        SpawnerSlot empty = (SpawnerSlot)0;
        Spawners[coord.x] = empty;
    }
}
