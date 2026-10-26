// RT sky visibility (interior lighting plan S) -- the SPATIAL half of its denoise. rt_sky_visibility_cs.hlsl leaves
// the temporally accumulated bent normal in `skyVisAccum` (next frame's history, never blurred: a blurred history
// feeds the blur back into itself and widens it every frame); this pass gathers it over a 5x5 half-resolution
// footprint (a 10x10 full-resolution one) into `skyVis`, the target lighting, compose and glass read.
//
// Why it exists: a point that sees the sky through a small window gets it from a few of its four rays now and
// then, and the history's ~19 frames are ~76 samples -- 40 % noise on a wall lit by a window that fills 5 % of its
// hemisphere (measured on wind_test's second room at 24:00: 1.4-3.6 % frame to frame over a 7-pixel window, the
// GI and the moonlit sky off it). UE's DFAO meets the same problem with the same tool: its bent normal is filtered
// in space (DistanceFieldAOSpatialFilter / the 5x5 "AO upsample" gather) after the temporal blend, weighted by
// depth and normal so it does not cross an edge.
//
// Weights, per tap: a Gaussian over the 5x5 (sigma 1.5 texels); the relative depth step (reverse-Z device depth is
// 1/distance, so |z_i - z| / z is the relative distance step) against `depthTolerance`; the normals' agreement
// raised to `normalPower`. A tap on the sky (.w 0) has none. The centre always counts, so an isolated texel keeps
// its own value.
#define RT_SKYVIS_FILTER_CS_RS \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)," \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)"

#pragma pack_matrix(row_major)

cbuffer SkyVisibilityFilter : register(b0)
{
    uint accumIndex;     uint outUavIndex;   uint gb1Index;       uint filterEnabled; // 0 = copy (skyVis.filter)
    uint outWidth;       uint outHeight;     float depthTolerance; float normalPower;
}

SamplerState gSmpPoint : register(s0);

static const int kSkyVisFilterRadius = 2;

[numthreads(8, 8, 1)]
[RootSignature(RT_SKYVIS_FILTER_CS_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= outWidth || dtid.y >= outHeight)
    {
        return;
    }
    Texture2D<float4> accum = ResourceDescriptorHeap[accumIndex];
    Texture2D gb1 = ResourceDescriptorHeap[gb1Index];
    RWTexture2D<float4> outTex = ResourceDescriptorHeap[outUavIndex];

    const float4 centre = accum[dtid.xy];
    if (centre.w <= 0.0f || filterEnabled == 0u)
    {
        outTex[dtid.xy] = centre; // the sky, or a texel the trace cleared: as it is
        return;
    }
    const float2 texel = 1.0f / float2(outWidth, outHeight);
    // The trace's own normal for each texel: the full-resolution pixel under its centre (GTAO's convention).
    const float3 N = normalize(gb1.SampleLevel(gSmpPoint, (float2(dtid.xy) + 0.5f) * texel, 0).rgb * 2.0f - 1.0f);

    float4 sum = float4(centre.xyz, 1.0f);
    [unroll]
    for (int y = -kSkyVisFilterRadius; y <= kSkyVisFilterRadius; ++y)
    {
        [unroll]
        for (int x = -kSkyVisFilterRadius; x <= kSkyVisFilterRadius; ++x)
        {
            if (x == 0 && y == 0) { continue; }
            const int2 p = int2(dtid.xy) + int2(x, y);
            if (any(p < 0) || p.x >= int(outWidth) || p.y >= int(outHeight)) { continue; }
            const float4 s = accum[p];
            if (s.w <= 0.0f) { continue; }
            const float3 Ns = normalize(gb1.SampleLevel(gSmpPoint, (float2(p) + 0.5f) * texel, 0).rgb * 2.0f - 1.0f);
            const float spatial = exp(-float(x * x + y * y) * (1.0f / (2.0f * 1.5f * 1.5f)));
            const float depthStep = abs(s.w - centre.w) / centre.w;
            const float wDepth = saturate(1.0f - depthStep / max(depthTolerance, 1.0e-4f));
            const float wNormal = pow(saturate(dot(N, Ns)), normalPower);
            const float w = spatial * wDepth * wNormal;
            sum += float4(s.xyz * w, w);
        }
    }
    outTex[dtid.xy] = float4(sum.xyz / sum.w, centre.w);
}
