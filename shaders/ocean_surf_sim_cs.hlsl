// Nearshore surf simulation - compute kernels (docs/ocean_surf_sim_plan.md).
//
// S1: the wave equation. `Update` advances the classic five-point-Laplacian height/velocity
// pair (the numerics of Crest's UpdateDynWaves, see docs/ref/crest/) one FIXED substep at a
// time, with the wave speed taken from the shore depth map: c^2 = g*depth, the shallow-water
// celerity, which buys REFRACTION for free - fronts slow over the shallows and bend parallel
// to the shoreline. Land (depth <= 0), the window border and open water outside the shore map
// absorb instead of reflecting. A Poke (PokeAmp != 0) injects a Gaussian hump at the window
// centre for the S1 gate. `Relocate` re-anchors the window by whole texels (S0).
//
// The window is square, world-axis-aligned: worldXZ = center + (coord + 0.5) * texel - halfExtent.
//
// Registers: RenderContext::kMaxBindings is 4 and Material::Bind silently skips a table whose
// base register is >= 4, so the four sim surfaces ride ONE UAV table at u0..u3 and the shore
// depth map one SRV table at t0. The sampler is static - no sampler table needed.
#define OCEAN_SURF_SIM_RS "RootConstants(num32BitConstants=16, b0), DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP)"

cbuffer SurfSimParams : register(b0)
{
    uint  Resolution;      // texels per side
    float TexelWorldSize;  // metres per texel
    float CenterX;         // window centre, world XZ (also the poke position)
    float CenterZ;
    float Time;            // seconds (the ocean's simulation clock)
    float DeltaTime;       // the FIXED substep, seconds
    int   ShiftX;          // Relocate only: texel shift, src = dst + shift
    int   ShiftY;
    float ShoreCenterX;    // shore depth map window (matches ShoreDepthUV in the surface shader)
    float ShoreCenterZ;
    float ShoreInvExtent;  // shoreViewParams.w: uv = offset * invExtent + 0.5
    float ShoreZNear;      // depth decode: viewDepth = lerp(zNear, zFar, raw)
    float ShoreZFar;
    float ShoreCamHeight;  // ortho camera height; waterDepth = viewDepth - camHeight
    float PokeAmp;         // metres; != 0 on the substep that injects the test hump
    float Pad0;
};

Texture2D<float>    ShoreDepthTex : register(t0);
SamplerState        ClampSampler : register(s0);

// x: height (metres), y: vertical velocity (m/s).
RWTexture2D<float2> WaveRead  : register(u0);
RWTexture2D<float2> WaveWrite : register(u1);
RWTexture2D<float>  FoamRead  : register(u2);
RWTexture2D<float>  FoamWrite : register(u3);

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
    // and the window border eats it so a wave leaving the domain never echoes back.
    const float landFade = saturate(depth / 0.2f);
    const int borderTexels = min(min(c.x, c.y), min(maxC - c.x, maxC - c.y));
    const float borderFade = saturate((float)borderTexels / 24.0f);
    const float fade = min(landFade, borderFade);
    const float kBaseDamping = 0.15f;   // 1/s, open-water settle rate
    const float kAbsorbDamping = 10.0f; // 1/s inside an absorber
    v *= max(0.0f, 1.0f - lerp(kAbsorbDamping, kBaseDamping, fade) * DeltaTime);

    h += v * DeltaTime;
    // Heights decay too where absorbed, so land never accumulates a standing sheet.
    h *= lerp(max(0.0f, 1.0f - 8.0f * DeltaTime), 1.0f, fade);

    // Poke (S1 gate): a Gaussian hump at the window centre, injected on exactly one substep.
    if (PokeAmp != 0.0f)
    {
        const float2 d = world - float2(CenterX, CenterZ);
        const float kSigma = 3.0f; // metres
        h += PokeAmp * exp(-dot(d, d) / (2.0f * kSigma * kSigma));
    }

    WaveWrite[coord] = float2(h, v);

    // Foam: still just a decaying carrier until S3 deposits into it.
    FoamWrite[coord] = FoamRead[coord] * max(0.0f, 1.0f - 0.5f * DeltaTime);
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
    const int2 src = int2(coord) + int2(ShiftX, ShiftY);
    const bool inside =
        src.x >= 0 && src.y >= 0 && src.x < (int)Resolution && src.y < (int)Resolution;
    WaveWrite[coord] = inside ? WaveRead[uint2(src)] : float2(0.0f, 0.0f);
    FoamWrite[coord] = inside ? FoamRead[uint2(src)] : 0.0f;
}
