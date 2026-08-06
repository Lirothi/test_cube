// Shore SDF: plan-view distance from every point of the level to the waterline.
//
// WHY IT EXISTS. The ocean has to take the wave's vertical motion out as it approaches land, or a
// swell drives straight through a beach. Doing that from the shore DEPTH map has two problems: the
// map is a window around the camera, so anything further away has no data at all, and depth is the
// wrong quantity anyway — it says how much water is under a point, not how far the shore is. Off a
// steep bank the water is deep right up against the rock. Distance-to-land answers the actual
// question, and it answers it for the whole level at once.
//
// HOW. Jump flooding. Seed every land texel with its own coordinate, then repeatedly let each texel
// look at neighbours k texels away and keep the nearest seed it has heard of, halving k each pass.
// After log2(N) passes every texel holds (approximately, and in practice exactly for our purposes)
// the nearest land texel, and the distance falls out of the coordinate difference.
//
// The source is a depth render of the terrain from directly above, the same ortho projection the
// shore depth map uses, so the decode below matches ShoreViewDepth in ocean_surface.hlsl.
//
// Registers: `RenderContext::kMaxBindings` is 4 and Material::Bind silently skips a table whose
// base register is >= 4, so the two jump buffers share ONE table at u0..u1.
#define OCEAN_SHORE_SDF_RS "RootConstants(num32BitConstants=8, b0), DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer SdfParams : register(b0)
{
    uint  Resolution;      // texels per side
    uint  JumpStep;        // current jump distance, in texels
    float TexelWorldSize;  // metres per texel
    float ViewHeight;      // world height the ortho camera sits at
    float ZNear;
    float ZFar;
    float SeaLevel;        // world Y the waterline sits at
    float Pad;
};

Texture2D<float> SourceDepth : register(t0);
// u0: read side, u1: write side. The Seed pass writes u0 and ignores u1; Resolve writes the SDF
// through u1. Ping-pong is done on the CPU by swapping which resource lands in which slot.
RWTexture2D<float2> JumpRead : register(u0);
RWTexture2D<float2> JumpWrite : register(u1);

static const float kNoSeed = -1.0f;

// Height of the solid surface above sea level, or a large negative number where nothing was drawn.
float SolidHeightAbove(uint2 coord)
{
    const float d = SourceDepth[coord];
    if (d >= 1.0f - 1e-6f)
    {
        return -1000.0f; // open water: the terrain pass drew nothing here
    }
    const float viewDepth = lerp(ZNear, ZFar, d);
    return (ViewHeight - viewDepth) - SeaLevel;
}

[numthreads(8, 8, 1)]
[RootSignature(OCEAN_SHORE_SDF_RS)]
void Seed(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 coord = dispatchThreadId.xy;
    if (coord.x >= Resolution || coord.y >= Resolution)
    {
        return;
    }

    // Land seeds itself. Everything else starts with no seed and gets one flooded in.
    const bool isLand = SolidHeightAbove(coord) > 0.0f;
    JumpRead[coord] = isLand ? float2(coord) : float2(kNoSeed, kNoSeed);
}

[numthreads(8, 8, 1)]
[RootSignature(OCEAN_SHORE_SDF_RS)]
void Jump(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 coord = dispatchThreadId.xy;
    if (coord.x >= Resolution || coord.y >= Resolution)
    {
        return;
    }

    const float2 self = float2(coord);
    float2 best = JumpRead[coord];
    float bestDistSq = best.x < 0.0f ? 1e30f : dot(best - self, best - self);

    const int step = int(JumpStep);
    [unroll]
    for (int oy = -1; oy <= 1; ++oy)
    {
        [unroll]
        for (int ox = -1; ox <= 1; ++ox)
        {
            const int2 sampleCoord = int2(coord) + int2(ox, oy) * step;
            if (sampleCoord.x < 0 || sampleCoord.y < 0 ||
                sampleCoord.x >= int(Resolution) || sampleCoord.y >= int(Resolution))
            {
                continue;
            }

            const float2 candidate = JumpRead[uint2(sampleCoord)];
            if (candidate.x < 0.0f)
            {
                continue;
            }

            const float2 delta = candidate - self;
            const float distSq = dot(delta, delta);
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                best = candidate;
            }
        }
    }

    JumpWrite[coord] = best;
}

[numthreads(8, 8, 1)]
[RootSignature(OCEAN_SHORE_SDF_RS)]
void Resolve(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 coord = dispatchThreadId.xy;
    if (coord.x >= Resolution || coord.y >= Resolution)
    {
        return;
    }

    const float2 seed = JumpRead[coord];
    // No land anywhere in the map: report "very far", which reads as open ocean everywhere.
    float distance = 4000.0f;
    if (seed.x >= 0.0f)
    {
        distance = length(seed - float2(coord)) * TexelWorldSize;
    }

    // Signed: negative on land. The consumer only needs the sign to know it is inland, and a
    // metre-accurate interior distance is not worth a second flood over the inverted mask.
    const float solidHeight = SolidHeightAbove(coord);
    if (solidHeight > 0.0f)
    {
        distance = -max(distance, TexelWorldSize);
    }

    // Second channel: the bed's height relative to sea level, i.e. minus the water depth. Distance
    // alone is not enough to quiet a wave — the middle of a wide lagoon is far from any shore and
    // still only knee deep, and a swell there looks absurd. This is the same rasterization the
    // flood ran on, so it costs nothing extra to keep.
    JumpWrite[coord] = float2(distance, clamp(solidHeight, -1000.0f, 1000.0f));
}
