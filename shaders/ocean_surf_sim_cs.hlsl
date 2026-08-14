// Nearshore surf simulation - compute kernels (docs/ocean_surf_sim_plan.md).
//
// S0: infrastructure only, NO wave math yet. `Update` stamps a world-anchored test pattern into
// the height field (proves the window transform: the pattern must stand still in the world while
// the window slides with the camera) and runs a placeholder decay on the foam ping-pong.
// `Relocate` re-anchors the window: it copies the previous frame's content shifted by a whole
// number of texels so the CONTENT keeps its world position when the window snaps. The wave
// equation replaces Update's stamp in S1.
//
// The window is square, world-axis-aligned: worldXZ = center + (coord + 0.5) * texel - halfExtent.
//
// Registers: RenderContext::kMaxBindings is 4 and Material::Bind silently skips a table whose
// base register is >= 4, so everything rides ONE UAV table at u0..u3. No SRVs yet (the shore
// depth map joins in S1).
#define OCEAN_SURF_SIM_RS "RootConstants(num32BitConstants=8, b0), DescriptorTable(UAV(u0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer SurfSimParams : register(b0)
{
    uint  Resolution;     // texels per side
    float TexelWorldSize; // metres per texel
    float CenterX;        // window centre, world XZ
    float CenterZ;
    int   ShiftX;         // Relocate only: texel shift, src = dst + shift
    int   ShiftY;
    float Time;           // seconds (the ocean's simulation clock)
    float DeltaTime;      // seconds since the previous sim step
};

// x: height (metres), y: vertical velocity (m/s) - the S1 wave equation's state pair.
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

[numthreads(8, 8, 1)]
[RootSignature(OCEAN_SURF_SIM_RS)]
void Update(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 coord = dispatchThreadId.xy;
    if (coord.x >= Resolution || coord.y >= Resolution)
    {
        return;
    }

    // S0 placeholder: a 4 m world checkerboard with a slow pulse. World-anchored on purpose -
    // if the pattern crawls when the camera moves, the window transform is wrong.
    const float2 world = WindowWorldPos(coord);
    const float checker =
        (fmod(floor(world.x * 0.25f) + floor(world.y * 0.25f) + 1000.0f, 2.0f) < 1.0f)
            ? 0.25f : -0.25f;
    const float pulse = 0.75f + 0.25f * sin(Time * 0.8f);
    WaveWrite[coord] = float2(checker * pulse, 0.0f);

    // Placeholder decay so the foam ping-pong is exercised end to end from day one.
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
