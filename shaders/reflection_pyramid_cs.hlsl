#define REFLECTION_PYRAMID_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

// One level of the two pyramids the glossy filter (reflection_glossy_cs.hlsl) reads its taps from:
//  * the REFLECTION's: mip 0 a copy of this frame's reflection (the temporal resolve's output when it ran), every
//    further mip the mean of 2x2 of the one before. Premultiplied (rgb, coverage), so a plain mean is the right
//    reduction -- a hole (coverage 0) dilutes its neighbours' coverage, not their colour;
//  * the GEOMETRY's, on the same grid: mip 0 the world normal (xyz) and device depth (w) under each reflection texel's
//    centre (the gather's own convention), further mips their means. A mip that mixes two surfaces averages them into
//    a position and a normal that fit neither, which the gather's same-surface test then rejects.
//
// Both chains live in UNORDERED_ACCESS while they are built and mip N reads mip N-1 through its UAV, the HZB's rule
// (hzb_build_cs.hlsl): this engine's barrier layer transitions whole resources. Levels are separated by UAV barriers.
//
// t0: the reflection, t1: depth, t2: GB1 (all three only read for mip 0)
// u0/u1: the reflection's source (N-1, unused for mip 0) and destination (N) mips
// u2/u3: the same pair of the geometry's

Texture2D<float4> ReflectionIn : register(t0);
Texture2D DepthTex : register(t1);
Texture2D GB1 : register(t2);
RWTexture2D<float4> SrcMip : register(u0);
RWTexture2D<float4> DstMip : register(u1);
RWTexture2D<float4> SrcGeom : register(u2);
RWTexture2D<float4> DstGeom : register(u3);
SamplerState gSmpPoint : register(s0);

cbuffer ReflectionPyramidCB : register(b0)
{
    uint2 dstSize;   // destination mip dimensions, texels
    uint2 srcSize;   // source mip dimensions (the reflection's for mip 0)
    uint fromInput;  // 1 = mip 0: copy the reflection, sample the G-buffer
    uint3 _pyramidPad;
}

[numthreads(8, 8, 1)]
[RootSignature(REFLECTION_PYRAMID_CS_RS)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid.xy >= dstSize))
    {
        return;
    }
    if (fromInput != 0u)
    {
        DstMip[tid.xy] = ReflectionIn[min(tid.xy, srcSize - 1u)];
        const float2 uv = (float2(tid.xy) + 0.5f) / float2(dstSize);
        const float z = DepthTex.SampleLevel(gSmpPoint, uv, 0.0f).r;
        const float3 n = z > 0.0f ? normalize(GB1.SampleLevel(gSmpPoint, uv, 0.0f).rgb * 2.0f - 1.0f) : 0.0f.xxx;
        DstGeom[tid.xy] = float4(n, z);
        return;
    }
    // An odd source folds its last row/column into the last destination texel's quad.
    const uint2 s0 = min(tid.xy * 2u, srcSize - 1u);
    const uint2 s1 = min(tid.xy * 2u + 1u, srcSize - 1u);
    DstMip[tid.xy] = 0.25f * (SrcMip[s0] + SrcMip[uint2(s1.x, s0.y)] + SrcMip[uint2(s0.x, s1.y)] + SrcMip[s1]);
    DstGeom[tid.xy] = 0.25f * (SrcGeom[s0] + SrcGeom[uint2(s1.x, s0.y)] + SrcGeom[uint2(s0.x, s1.y)] + SrcGeom[s1]);
}
