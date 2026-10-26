// RT sky visibility (interior lighting plan S) -- the SPATIAL half of its denoise. rt_sky_visibility_cs.hlsl leaves
// the temporally accumulated bent normal in `skyVisAccum` (next frame's history, never blurred: a blurred history
// feeds the blur back into itself and widens it every frame); this pass gathers it over a (2 radius + 1)^2
// half-resolution footprint (skyVis.filterRadius, 4 = 9x9, an 18x18 full-resolution one) into `skyVis`, the target
// lighting, compose and glass read.
//
// 4, not UE's 5x5, since 2026-10-26 (owner: "SkyVis noises like a bastard" in his tunnel): a ray is a 0/1 sample, and
// where the sky is a few per cent of the hemisphere -- the walls by the tunnel's mouth -- 8 rays x the history leave a
// texel tens of per cent off; with the rays interleaved across texels (rt_sky_visibility_cs.hlsl) a wider footprint
// averages rays that cover the hemisphere between each other. Owner's tunnel, the visibility alone (skyVis.debug:2),
// 24 still frames: grain (|mean - 8 px blur| / blur) median 4.7 / 3.1 / 2.3 % at radius 2 / 3 / 4, p90 17.0 / 11.5 /
// 8.5 %, flicker p90 5.8 / 4.3 / 3.5 %, the mean unchanged (-0.4 %); in flight at 4 m/s the radius-2 walls wore a
// diagonal hatching radius 4 smooths out. 16 rays at radius 2 reach 3.8 % for +0.12-0.27 ms; radius 4 costs
// +0.01-0.04 ms (Pass_SkyVis, tunnel / grove / bathroom). The sky's contact occlusion on the grove and the bathroom's
// shelves looked the same: the depth and normal weights stop it at every edge.
//
// Why it exists: a point that sees the sky through a small window gets it from a few of its four rays now and
// then, and the history's ~19 frames are ~76 samples -- 40 % noise on a wall lit by a window that fills 5 % of its
// hemisphere (measured on wind_test's second room at 24:00: 1.4-3.6 % frame to frame over a 7-pixel window, the
// GI and the moonlit sky off it). UE's DFAO meets the same problem with the same tool: its bent normal is filtered
// in space (DistanceFieldAOSpatialFilter / the 5x5 "AO upsample" gather) after the temporal blend, weighted by
// depth and normal so it does not cross an edge.
//
// Weights, per tap: a Gaussian over the footprint (sigma 0.75 x the radius: 1.5 texels at UE's 5x5); the relative depth step (reverse-Z device depth is
// 1/distance, so |z_i - z| / z is the relative distance step) against `depthTolerance`; the normals' agreement
// raised to `normalPower`. A tap on the sky (.w 0) has none. The centre always counts, so an isolated texel keeps
// its own value. The accumulation's .w is the device depth, NEGATIVE where the texel had no history this frame
// (rt_sky_visibility_cs.hlsl); the target's is always positive. `debugView` 1 (skyVis.debug) paints those texels as
// the open sky -- bright wherever the sky is otherwise shut out.
#define RT_SKYVIS_FILTER_CS_RS \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)," \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)"

#pragma pack_matrix(row_major)

cbuffer SkyVisibilityFilter : register(b0)
{
    uint accumIndex;     uint outUavIndex;   uint gb1Index;       uint filterEnabled; // 0 = copy (skyVis.filter)
    uint outWidth;       uint outHeight;     float depthTolerance; float normalPower;
    uint debugView;      uint radius;        uint2 _filterPad; // radius: skyVis.filterRadius, 1-4 texels
}

SamplerState gSmpPoint : register(s0);

static const int kSkyVisFilterMaxRadius = 4; // the loops' literal bound; `radius` only shortens them

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
    const float centreZ = abs(centre.w);
    if (centreZ <= 0.0f)
    {
        outTex[dtid.xy] = 0.0f.xxxx; // the sky, or a texel the trace cleared: as it is
        return;
    }
    const float2 texel = 1.0f / float2(outWidth, outHeight);
    // The trace's own normal for each texel: the full-resolution pixel under its centre (GTAO's convention).
    const float3 N = normalize(gb1.SampleLevel(gSmpPoint, (float2(dtid.xy) + 0.5f) * texel, 0).rgb * 2.0f - 1.0f);
    if (debugView == 1u && centre.w < 0.0f)
    {
        outTex[dtid.xy] = float4(N, centreZ); // no history this frame: shown as the whole sky
        return;
    }
    if (filterEnabled == 0u)
    {
        outTex[dtid.xy] = float4(centre.xyz, centreZ);
        return;
    }

    float4 sum = float4(centre.xyz, 1.0f);
    const int reach = clamp(int(radius), 1, kSkyVisFilterMaxRadius);
    const float sigma = 0.75f * float(reach);
    [loop]
    for (int y = -kSkyVisFilterMaxRadius; y <= kSkyVisFilterMaxRadius; ++y)
    {
        if (abs(y) > reach) { continue; }
        [loop]
        for (int x = -kSkyVisFilterMaxRadius; x <= kSkyVisFilterMaxRadius; ++x)
        {
            if (abs(x) > reach || (x == 0 && y == 0)) { continue; }
            const int2 p = int2(dtid.xy) + int2(x, y);
            if (any(p < 0) || p.x >= int(outWidth) || p.y >= int(outHeight)) { continue; }
            const float4 s = accum[p];
            const float sZ = abs(s.w);
            if (sZ <= 0.0f) { continue; }
            const float3 Ns = normalize(gb1.SampleLevel(gSmpPoint, (float2(p) + 0.5f) * texel, 0).rgb * 2.0f - 1.0f);
            const float spatial = exp(-float(x * x + y * y) / (2.0f * sigma * sigma));
            const float depthStep = abs(sZ - centreZ) / centreZ;
            const float wDepth = saturate(1.0f - depthStep / max(depthTolerance, 1.0e-4f));
            const float wNormal = pow(saturate(dot(N, Ns)), normalPower);
            const float w = spatial * wDepth * wNormal;
            sum += float4(s.xyz * w, w);
        }
    }
    outTex[dtid.xy] = float4(sum.xyz / sum.w, centreZ);
}
