// RT sky visibility (interior lighting plan S; photographic plan P9 item 2) -- the PRODUCER.
//
// UE's Distance Field AO bent normal (DistanceFieldAOShared.ush ComputeBentNormal): the mean of the
// unoccluded directions over the hemisphere, normalized so an open sky has length 1 -- length =
// how much sky the point sees, direction = where it sees it from. UE cones through mesh distance
// fields; this engine has no SDFs but has the TLAS, so it is traced with inline RayQuery: `rayCount`
// cosine-distributed rays per HALF-resolution pixel, each a shadow-style "anything in the way"
// query against the SHADOW casters (rt::kInstanceMaskShadow -- what casts no shadow hides no sky)
// out to `maxDistance`, a leaf card solid (no RT ray but the reflections' alpha-tests: rt_geometry.hlsli).
//
// The few rays are accumulated over frames IN THE SAME DISPATCH: the previous frame's result,
// reprojected through the G-buffer velocity, is blended in with `historyWeight` where it saw the
// same surface (device depth within 10 %). A texel that has NO history -- disoccluded, or come in
// over the screen's edge -- is seeded from the neighbours that kept theirs (UE DFAO FilterHistoryPS,
// below): left as this frame's few rays, that one noisy estimate carried `historyWeight` into every
// later blend (0.98^n -- still 13 % of it after 100 frames), and a camera flying backwards, with the
// floor coming in under the bottom edge every frame, wore a band of grain ~50 texels deep (2026-10-26).
// The consumers upsample it with UE's bilateral 2x2 (sky_visibility.hlsli), so .w carries the device
// depth the trace saw -- NEGATIVE in the accumulation where the texel had no history this frame (UE's
// "sign bit of alpha", DistanceFieldLightingPost.usf:288-290): the filter shows it (skyVis.debug:1) and
// writes the target's .w positive. 0 = the sky.
#define RT_SKYVIS_CS_RS \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)," \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)," \
    "StaticSampler(s1, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)"

#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "rt_geometry.hlsli"

cbuffer SkyVisibility : register(b0)
{
    float4x4 invView;
    float4x4 invProj;
    uint tlasIndex;     uint gb1Index;      uint depthIndex;    uint velocityIndex;
    uint outUavIndex;   uint historyIndex;  uint _skyVisPad0;   uint _skyVisPad1;
    uint outWidth;      uint outHeight;     uint rayCount;      uint frameIndex;
    float maxDistance;  float historyWeight; uint historyValid; uint seedEnabled; // skyVis.seed (0 = start from own rays)
    float seedDepthTolerance; float seedNormalPower; uint2 _skyVisPad2; // the spatial filter's own (skyVis.filter*)
}

SamplerState gSmp      : register(s0); // linear clamp
SamplerState gSmpPoint : register(s1); // point clamp

static const uint kSkyVisMaskShadow = 0x02u; // rt::kInstanceMaskShadow (rt_reflect_common.hlsli RT_MASK_SHADOW)

// Anything in the way between `origin` and `maxDistance` along `dir` -- the geometry, a leaf card solid
// (rt_geometry.hlsli RtVisibilityBlocked).
bool SkyRayBlocked(RaytracingAccelerationStructure tlas, float3 origin, float3 dir)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 1.0e-3f;
    ray.TMax = maxDistance;
    return RtVisibilityBlocked(tlas, ray, kSkyVisMaskShadow);
}

// The previous accumulation where the surface under `uvAt` (device depth `zAt`) was last frame -- if it is on the
// screen and saw the same surface (relative step in reverse-Z depth = relative step in distance).
bool SkyVisHistoryAt(Texture2D velocity, Texture2D<float4> history, float2 uvAt, float zAt, out float3 value)
{
    value = 0.0f.xxx;
    const float2 prevUv = uvAt - velocity.SampleLevel(gSmpPoint, uvAt, 0).rg; // motion = currUv - prevUv
    if (any(prevUv < 0.0f) || any(prevUv > 1.0f))
    {
        return false;
    }
    const float4 h = history.SampleLevel(gSmpPoint, prevUv, 0);
    const float hz = abs(h.w); // negative = that texel had no history itself; its value is still last frame's
    if (hz <= 0.0f || abs(hz - zAt) > 0.1f * zAt)
    {
        return false;
    }
    value = h.xyz;
    return true;
}

// A texel with no history is seeded from those around it within this many texels that kept theirs. UE's search is
// 2 (HALF_HISTORY_FILL_KERNEL_SIZE); a camera flying backwards at 60 fps brings ~3 half-resolution rows of floor in
// under the bottom edge a frame (3.8 m/s, 2.3 m up), and the rows past the reach of the search would start raw.
static const int kSkyVisSeedRadius = 4;

[numthreads(8, 8, 1)]
[RootSignature(RT_SKYVIS_CS_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= outWidth || dtid.y >= outHeight)
    {
        return;
    }
    RWTexture2D<float4> outTex = ResourceDescriptorHeap[outUavIndex];
    Texture2D depthT = ResourceDescriptorHeap[depthIndex];
    Texture2D gb1 = ResourceDescriptorHeap[gb1Index];

    // The full-resolution pixel under this half-resolution texel's centre (GTAO's convention).
    const float2 uv = (float2(dtid.xy) + 0.5f) / float2(outWidth, outHeight);
    const float z = depthT.SampleLevel(gSmpPoint, uv, 0).r;
    if (z <= 1.0e-6f)
    {
        outTex[dtid.xy] = 0.0f.xxxx; // the sky itself: .w 0, never read by the upsample
        return;
    }
    const float3 N = normalize(gb1.SampleLevel(gSmpPoint, uv, 0).rgb * 2.0f - 1.0f);
    const float3 P = ReconstructPosWS(uv, z, invProj, invView);
    const float3 camPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), invView).xyz;
    // RtSetupReflectionRay's offset: off the surface by a share of the view distance, so a far
    // texel's depth error does not start the ray inside its own surface.
    const float3 origin = P + N * max(0.02f, length(P - camPos) * 0.002f);

    // Tangent frame (UE FindBestAxisVectors).
    const float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 T = normalize(cross(up, N));
    const float3 B = cross(N, T);

    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[tlasIndex];
    // Per-pixel, per-frame rotation of an R2 sequence: the rays of successive frames fill the
    // hemisphere between each other, which is what the accumulation below integrates.
    const uint seed = RtWangHash(dtid.x * 1973u + dtid.y * 9277u + frameIndex * 26699u);
    const float2 rotation = float2(float(seed & 0xFFFFu), float(seed >> 16)) * (1.0f / 65536.0f);
    const uint rays = clamp(rayCount, 1u, 16u);
    float3 unoccluded = 0.0f.xxx;
    for (uint k = 0; k < rays; ++k)
    {
        const float u1 = frac(rotation.x + float(k) * 0.7548776662f);
        const float u2 = frac(rotation.y + float(k) * 0.5698402910f);
        // Cosine-distributed: the sky light's own weighting, so a plain average is its integral.
        const float r = sqrt(u1);
        const float phi = 2.0f * kPi * u2;
        const float3 d = normalize(T * (r * cos(phi)) + B * (r * sin(phi)) + N * sqrt(saturate(1.0f - u1)));
        if (!SkyRayBlocked(tlas, origin, d))
        {
            unoccluded += d;
        }
    }
    // UE's BentNormalNormalizeFactor: 1 / |mean direction of an open hemisphere|, which for
    // cosine-distributed directions is 1 / (2/3).
    float3 bent = unoccluded * (1.5f / float(rays));
    const float bentLength = length(bent);
    if (bentLength > 1.0f) { bent /= bentLength; }

    bool kept = false;
    if (historyValid != 0u)
    {
        Texture2D velocity = ResourceDescriptorHeap[velocityIndex];
        Texture2D<float4> history = ResourceDescriptorHeap[historyIndex];
        float3 h;
        if (SkyVisHistoryAt(velocity, history, uv, z, h))
        {
            bent = lerp(bent, h, saturate(historyWeight));
            kept = true;
        }
        else if (seedEnabled != 0u)
        {
            // UE DFAO FilterHistoryPS (DistanceFieldLightingPost.usf:297-361): "seeds newly rejected history values
            // (which are sources of temporal instability) with the results of a spatial search from stable history
            // values", as if that search were the history and these rays the new value (line 354). UE searches the
            // updated buffer in a pass of its own; here each neighbour's own reprojected history is read -- the same
            // values one blend earlier, and no texel reads another's output. The weights are the spatial filter's
            // (rt_sky_visibility_filter_cs.hlsl), so the seed comes from what the filter calls the same surface:
            // UE's exp2(-1000 x relative depth step) rejects the next row of a floor seen at 30 degrees.
            float4 acc = 0.0f.xxxx;
            [loop]
            for (int y = -kSkyVisSeedRadius; y <= kSkyVisSeedRadius; ++y)
            {
                [loop]
                for (int x = -kSkyVisSeedRadius; x <= kSkyVisSeedRadius; ++x)
                {
                    const int2 p = int2(dtid.xy) + int2(x, y);
                    if ((x == 0 && y == 0) || any(p < 0) || p.x >= int(outWidth) || p.y >= int(outHeight)) { continue; }
                    const float2 uvP = (float2(p) + 0.5f) / float2(outWidth, outHeight);
                    const float zP = depthT.SampleLevel(gSmpPoint, uvP, 0).r;
                    if (zP <= 1.0e-6f) { continue; }
                    const float wDepth = saturate(1.0f - (abs(zP - z) / z) / max(seedDepthTolerance, 1.0e-4f));
                    if (wDepth <= 0.0f) { continue; }
                    float3 hP;
                    if (!SkyVisHistoryAt(velocity, history, uvP, zP, hP)) { continue; }
                    const float3 NP = normalize(gb1.SampleLevel(gSmpPoint, uvP, 0).rgb * 2.0f - 1.0f);
                    const float w = exp(-float(x * x + y * y) * (1.0f / (2.0f * 1.5f * 1.5f))) * wDepth *
                                    pow(saturate(dot(N, NP)), seedNormalPower);
                    acc += float4(hP * w, w);
                }
            }
            if (acc.w > 1.0e-4f)
            {
                bent = lerp(bent, acc.xyz / acc.w, saturate(historyWeight));
            }
        }
    }
    outTex[dtid.xy] = float4(bent, kept ? z : -z);
}
