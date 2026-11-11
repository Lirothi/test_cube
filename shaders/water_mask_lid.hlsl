// The water masks' lids (ocean/WaterMask.h), Main_WaterMask: every hull's caps and curtains, drawn before the water into the
// TWO NEAREST CROSSINGS of each pixel -- a Texture2DArray<uint> of two slices, the view depth's bits (positive floats order as
// their bits do), cleared to 0xFFFFFFFF. The water's surface (ocean_surface_surf_sim.hlsli InsideWaterMask) counts the ones
// in front of its own point: the cap and the curtains close a basin with the hull's opaque walls, so a ray that reached the
// water through an odd number of them reached it inside.
//
// Both faces, no depth: a crossing counts wherever it is, hidden or not (what hides it hides the water behind it too). No
// near plane either: z is half of w, so only what is behind the eye is clipped -- a crossing a hand's breadth in front of a
// camera standing at the cap's height still counts. The projection is the water's own (the jittered one: the same pixels).
#define WATER_MASK_LID_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer WaterMaskLidCB : register(b0)
{
    row_major float4x4 modelViewProj; // the hull's model space -> the water's clip space
    float4 modelViewZ;                // the view depth: dot(float4(p, 1), modelViewZ), as the surface's viewDepth
};

RWTexture2DArray<uint> Crossings : register(u0);

struct VSIn
{
    float3 position : POSITION;
};

struct VSOut
{
    float4 position : SV_POSITION;
    float viewDepth : TEXCOORD0;
};

[RootSignature(WATER_MASK_LID_RS)]
VSOut VSMain(VSIn input)
{
    VSOut o;
    o.position = mul(float4(input.position, 1.0f), modelViewProj);
    o.position.z = 0.5f * o.position.w;
    o.viewDepth = dot(float4(input.position, 1.0f), modelViewZ);
    return o;
}

// The two smallest of every value a pixel gets, in order (an atomic k-buffer of two: the first slice keeps the least, what it
// displaced goes on to the second).
[RootSignature(WATER_MASK_LID_RS)]
void PSMain(VSOut input)
{
    const uint2 pixel = uint2(input.position.xy);
    const uint depth = asuint(max(input.viewDepth, 0.0f));
    uint displaced;
    InterlockedMin(Crossings[uint3(pixel, 0u)], depth, displaced);
    InterlockedMin(Crossings[uint3(pixel, 1u)], max(depth, displaced));
}
