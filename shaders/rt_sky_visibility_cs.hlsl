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
// Two entry points, in UE DFAO's order. CSTrace writes this frame's rays alone into `skyVisRaw` (.w = the device
// depth it traced at, 0 = the sky). CSAccumulate filters THOSE in space (the 9x9 depth- and normal-weighted gather,
// UE's GeometryAwareUpsample step, on the NEW value), and only then blends the result into the history: the previous
// frame's accumulation, reprojected through the G-buffer velocity and read bilinearly corner by corner (UE's manual
// resample, SkyVisHistoryAt), where it saw the same surface (off this point's plane by under 3 % of the view depth),
// at 1 / (1 + the frames it has accumulated) up to `historyWeight`'s share (Lumen's count, below). UE's own
// UpdateHistoryDepthRejectionPS calls GeometryAwareUpsample BEFORE it blends (DistanceFieldLightingPost.usf:161-240).
//
// Until 2026-10-27 the spatial filter ran AFTER the accumulation, on its output (SVGF's order), weighted by this
// frame's G-buffer under each texel -- which the DLSS jitter moves every frame. Where two surfaces meet at the same
// depth (the tent's fabric folded on its floor, no sky in the fold, the wall's bright bottom above it) the filter's
// verdict on which taps are "this surface" changed from frame to frame, and nothing averaged it: 4-7 % frame to frame
// on the hem, 0.03 levels with DLSS off (owner: "what trembles here?"). Filtered before the history, those per-frame
// weights are averaged like the rays are; and a texel the history rejected starts from a filtered estimate, not from
// its own eight rays.
//
// A texel that has NO history -- disoccluded, or come in over the screen's edge -- is seeded from the neighbours
// that kept theirs (UE DFAO FilterHistoryPS, below): left as this frame's estimate, it carried `historyWeight` into
// every later blend (0.98^n -- still 13 % of it after 100 frames), and a camera flying backwards, with the floor
// coming in under the bottom edge every frame, wore a band of grain ~50 texels deep (2026-10-26). The consumers
// upsample the target with UE's bilateral 2x2 (sky_visibility.hlsli), so .w carries the device depth -- NEGATIVE in
// the accumulation where the texel had no history this frame (UE's "sign bit of alpha",
// DistanceFieldLightingPost.usf:288-290); the target's is always positive (`debugView` 1, skyVis.debug, paints those
// texels as the open sky). 0 = the sky.
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
    float4x4 prevInvViewProj; // last frame's clip -> world: where the history's depths put a point (UE PrevScreenToTranslatedWorld)
    uint tlasIndex;     uint gb1Index;      uint depthIndex;    uint velocityIndex;
    uint accumUavIndex; uint historyIndex;  uint countUavIndex; uint countHistoryIndex; // counts: R8_UNORM, n / 255
    uint outWidth;      uint outHeight;     uint rayCount;      uint frameIndex;
    float maxDistance;  float historyWeight; uint historyValid; uint seedEnabled; // skyVis.seed (0 = start from own rays)
    float filterDepthTolerance; float filterNormalPower; // skyVis.filterDepth / filterNormalPower: the gather's and the seed's
    uint samplingMode; uint meshOpacity;              // skyVis.sampling: 0 white noise, 1 interleaved gradient noise; rt.meshOpacity
    uint rawUavIndex;   uint rawIndex;      uint targetUavIndex; uint filterRadius; // skyVisRaw (CSTrace's UAV, CSAccumulate's SRV); the target
    uint filterEnabled; uint debugView;     uint accumIndex;    uint filterFirst; // skyVis.filter, skyVis.debug 1; this
                                                                    // frame's accumulation (SRV); skyVis.filterFirst
}

SamplerState gSmp      : register(s0); // linear clamp
SamplerState gSmpPoint : register(s1); // point clamp

static const uint kSkyVisMaskShadow = kRtMaskShadow; // rt::kInstanceMaskShadow (rt_reflect_common.hlsli RT_MASK_SHADOW)

// How much of the sky along `dir` gets through to `origin` within `maxDistance`: 0 behind the geometry, 1 - opacity
// through a tree's leaf card (rt_geometry.hlsli RtVisibilityTransmittance), 1 open.
float SkyRayTransmittance(RaytracingAccelerationStructure tlas, float3 origin, float3 dir)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 1.0e-3f;
    ray.TMax = maxDistance;
    return RtVisibilityTransmittance(tlas, ray, kSkyVisMaskShadow, meshOpacity);
}

// UE r.AOHistoryDistanceThreshold 30 over its 1000 (ComputeHistoryWeightBasedOnPosition): a history corner whose
// surface lies farther than this share of the view depth off this frame's point's plane is another surface.
static const float kSkyVisHistoryDistance = 0.03f;

// The previous accumulation where the surface at `posAt` (screen `uvAt`, view depth `viewDepthAt`, normal `normalAt`)
// was last frame: UE UpdateHistoryDepthRejectionPS's manual resample with a position weight per corner
// (DistanceFieldLightingPost.usf:203-240, COMPUTE_SEPARATE_POSITION_WEIGHTS). The four texels around the reprojected
// point, each by its bilinear weight, each kept only if the point its OWN depth puts at that screen position last frame
// lies on this point's surface: its distance from this point's PLANE over the view depth, against UE's 3 % -- the
// plane test of UE's own GeometryAwareUpsample (ComputeSampleWeightBasedOnPosition), not the history's 3D distance.
// The two depths were rendered under different DLSS jitters, so the two points sit up to a pixel's footprint apart
// ALONG the surface, and on a wall seen at a grazing angle that footprint is more than 3 % of the depth: the 3D test
// threw the history away there now and then -- streaks of noise down the tunnel's walls, rings in the light by its
// mouth, the camera standing still. Off the plane is what another surface is. Returns (sum of weight x value, sum of weight);
// a corner left out -- off the screen, the sky, another surface -- leaves its weight to the caller's new value.
// `countSum` is the same sum of the corners' accumulated-frame counts plus one (Lumen's WeightedAverage(count + 1)).
// It was ONE point sample at the reprojected position: the nearest texel, whose centre is up to half a texel off, and
// in a steady turn off by the SAME fraction every frame -- the history slid that far each frame and 0.98 of it
// carried 50 frames of slides, a trail behind every edge of the sky's light (2026-10-26, owner: "SkyVis is noisy and
// trails when the camera moves or turns fast"). And its test was the device depth within 10 %: a palm trunk 2 m in
// front of a wall 20 m away passed it.
float4 SkyVisHistoryAt(Texture2D velocity, Texture2D<float4> history, Texture2D<float> counts, float2 uvAt, float3 posAt,
                       float3 normalAt, float viewDepthAt, out float countSum)
{
    countSum = 0.0f;
    const float2 prevUv = uvAt - velocity.SampleLevel(gSmpPoint, uvAt, 0).rg; // motion = currUv - prevUv
    if (any(prevUv < 0.0f) || any(prevUv > 1.0f))
    {
        return 0.0f.xxxx;
    }
    const float2 prevNdc = (prevUv * 2.0f - 1.0f) * float2(1.0f, -1.0f);
    const float2 t = prevUv * float2(outWidth, outHeight) - 0.5f;
    const int2 corner00 = int2(floor(t));
    const float2 f = t - float2(corner00);
    float4 acc = 0.0f.xxxx;
    [unroll]
    for (uint i = 0u; i < 4u; ++i)
    {
        const int2 o = int2(i & 1u, i >> 1u);
        const int2 p = clamp(corner00 + o, int2(0, 0), int2(outWidth, outHeight) - 1);
        const float4 h = history.Load(int3(p, 0));
        const float hz = abs(h.w); // negative = that texel had no history itself; its value is still last frame's
        if (hz <= 0.0f) { continue; } // the sky
        const float4 prevClip = mul(float4(prevNdc, hz, 1.0f), prevInvViewProj);
        const float3 prevPos = prevClip.xyz / prevClip.w;
        if (abs(dot(normalAt, prevPos - posAt)) > kSkyVisHistoryDistance * viewDepthAt) { continue; }
        const float bilinear = (o.x != 0 ? f.x : 1.0f - f.x) * (o.y != 0 ? f.y : 1.0f - f.y);
        acc += float4(h.xyz, 1.0f) * bilinear;
        countSum += (counts.Load(int3(p, 0)) * 255.0f + 1.0f) * bilinear;
    }
    return acc;
}

// A seeded texel counts as UE DFAO's own history: r.AOHistoryWeight 0.85 is a blend of 0.15 = 1 / (1 + 5.67). The seed
// is a spatial estimate from the neighbours, near an edge a biased one; at 0.98's 49 frames it outlived the motion that
// made it by a second.
static const float kSkyVisSeedFrames = 0.85f / 0.15f;

// A texel with no history is seeded from those around it within this many texels that kept theirs. UE's search is
// 2 (HALF_HISTORY_FILL_KERNEL_SIZE); a camera flying backwards at 60 fps brings ~3 half-resolution rows of floor in
// under the bottom edge a frame (3.8 m/s, 2.3 m up), and the rows past the reach of the search would start raw.
static const int kSkyVisSeedRadius = 4;

// The spatial gather's reach in half-resolution texels: the loops' literal bound, `filterRadius` (skyVis.filterRadius,
// 1-4) only shortens them.
static const int kSkyVisFilterMaxRadius = 4;

// The normals' agreement raised to `filterNormalPower`; power 0 = not weighed (pow(0, 0) is NaN on the GPU).
float SkyVisNormalWeight(float3 a, float3 b)
{
    return filterNormalPower > 0.0f ? pow(saturate(dot(a, b)), filterNormalPower) : 1.0f;
}

[numthreads(8, 8, 1)]
[RootSignature(RT_SKYVIS_CS_RS)]
void CSTrace(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= outWidth || dtid.y >= outHeight)
    {
        return;
    }
    RWTexture2D<float4> rawOut = ResourceDescriptorHeap[rawUavIndex];
    Texture2D depthT = ResourceDescriptorHeap[depthIndex];
    Texture2D gb1 = ResourceDescriptorHeap[gb1Index];

    // The full-resolution pixel under this half-resolution texel's centre (GTAO's convention).
    const float2 uv = (float2(dtid.xy) + 0.5f) / float2(outWidth, outHeight);
    const float z = depthT.SampleLevel(gSmpPoint, uv, 0).r;
    if (z <= 1.0e-6f)
    {
        rawOut[dtid.xy] = 0.0f.xxxx; // the sky itself: .w 0
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
    //
    // ...and the rotations of NEIGHBOURING texels fill it between each other too: UE's InterleavedGradientNoise
    // (Random.ush), offset per frame as UE offsets it (5.588238 x frame % 64). Any 3x3 of texels holds nearly every
    // rotation, so the spatial filter's 5x5 averages rays that cover the hemisphere instead of independent draws
    // that may all miss a small opening. A ray is a 0/1 point sample: where the sky is a small share p of the
    // hemisphere (the walls by a tunnel's mouth) its estimate carries 1 / sqrt(p x rays) of noise, which a
    // distance-field cone (UE DFAO) does not -- it measures the share of its own section the geometry closes. The
    // stratification is what rays can do about it for free. Owner's tunnel (cam -57.89,2.27,65.68 yaw 57.8, 15:00),
    // the visibility alone (skyVis.debug:2), 24 still frames, white noise -> interleaved: grain (|mean - 8 px blur| /
    // blur) median 6.7 -> 4.7 %, p90 20.5 -> 16.7 %; flicker p90 9.0 -> 5.9 %; the mean unchanged (35.2 / 35.1).
    // 16 rays would take it to 3.7 % / 3.7 % at twice the cost. Tried and dropped: half the rays into a cone around
    // the history's bent normal (Lumen-style guiding, balance-heuristic weights) -- grainier (7.7 %) and, with the
    // interleaving, 12 % darker. `skyVis.sampling:0` = the white noise, the A/B. Tried with the gather moved before the
    // history (2026-10-27) and dropped: the IGN unshifted plus each texel's R2 sequence in time -- the tunnel's visibility
    // grain 3.7 % / p90 11.8 % against UE's shift's 2.3 % / 6.7 %.
    float2 rotation;
    if (samplingMode == 0u)
    {
        const uint seed = RtWangHash(dtid.x * 1973u + dtid.y * 9277u + frameIndex * 26699u);
        rotation = float2(float(seed & 0xFFFFu), float(seed >> 16)) * (1.0f / 65536.0f);
    }
    else
    {
        const float2 px = float2(dtid.xy) + 5.588238f * float(frameIndex & 63u);
        rotation = float2(frac(52.9829189f * frac(dot(px, float2(0.06711056f, 0.00583715f)))),
                          frac(52.9829189f * frac(dot(px.yx + 17.0f, float2(0.06711056f, 0.00583715f)))));
    }
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
        unoccluded += d * SkyRayTransmittance(tlas, origin, d);
    }
    // UE's BentNormalNormalizeFactor: 1 / |mean direction of an open hemisphere|, which for
    // cosine-distributed directions is 1 / (2/3).
    float3 bent = unoccluded * (1.5f / float(rays));
    const float bentLength = length(bent);
    if (bentLength > 1.0f) { bent /= bentLength; }
    rawOut[dtid.xy] = float4(bent, z);
}

[numthreads(8, 8, 1)]
[RootSignature(RT_SKYVIS_CS_RS)]
void CSAccumulate(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= outWidth || dtid.y >= outHeight)
    {
        return;
    }
    Texture2D<float4> rawIn = ResourceDescriptorHeap[rawIndex];
    RWTexture2D<float4> accumOut = ResourceDescriptorHeap[accumUavIndex];
    RWTexture2D<float> countOut = ResourceDescriptorHeap[countUavIndex];
    RWTexture2D<float4> targetOut = ResourceDescriptorHeap[targetUavIndex];
    Texture2D depthT = ResourceDescriptorHeap[depthIndex];
    Texture2D gb1 = ResourceDescriptorHeap[gb1Index];

    const float4 centre = rawIn[dtid.xy];
    const float z = centre.w; // the device depth CSTrace saw at this texel
    if (z <= 1.0e-6f)
    {
        accumOut[dtid.xy] = 0.0f.xxxx; // the sky itself: .w 0, never read by the upsample
        countOut[dtid.xy] = 0.0f;
        targetOut[dtid.xy] = 0.0f.xxxx;
        return;
    }
    const float2 texel = 1.0f / float2(outWidth, outHeight);
    const float2 uv = (float2(dtid.xy) + 0.5f) * texel;
    const float3 N = normalize(gb1.SampleLevel(gSmpPoint, uv, 0).rgb * 2.0f - 1.0f);
    const float3 P = ReconstructPosWS(uv, z, invProj, invView);
    const float3 camPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), invView).xyz;

    // The spatial step, on THIS frame's rays (UE DFAO's spatial filter, applied where UE applies its
    // GeometryAwareUpsample): a (2 radius + 1)^2 gather, each tap by a Gaussian over the footprint (sigma 0.75 x the
    // radius), its relative depth step against `filterDepthTolerance` (reverse-Z device depth is ~1/distance, so the
    // relative step is a relative distance step) and its normal's agreement. The centre always counts. With the rays
    // interleaved across texels (CSTrace's rotation) the gather averages rays that cover the hemisphere between each
    // other; the history then averages the gathers, and with them the weights' frame-to-frame changes under the jitter.
    //
    // Why it exists: a point that sees the sky through a small window gets it from a few of its rays now and then -- 40 %
    // noise on a wall lit by a window that fills 5 % of its hemisphere (wind_test's second room at 24:00, 2026-10-24).
    // Radius 4 (9x9, an 18x18 full-resolution footprint), not UE's 5x5, since 2026-10-26 (owner: "SkyVis noises like a
    // bastard" in his tunnel), measured as the filter after the accumulation it then was: the visibility alone
    // (skyVis.debug:2), 24 still frames, grain (|mean - 8 px blur| / blur) median 4.7 / 3.1 / 2.3 % at radius 2 / 3 / 4,
    // p90 17.0 / 11.5 / 8.5 %, flicker p90 5.8 / 4.3 / 3.5 %, the mean unchanged; in flight at 4 m/s radius 2 wore a
    // diagonal hatching radius 4 smooths out; +0.01-0.04 ms. The depth and normal weights stop it at every edge: the
    // sky's contact occlusion on the grove and the bathroom's shelves looked the same.
    float3 bent = centre.xyz;
    if (filterEnabled != 0u && filterFirst != 0u)
    {
        float4 sum = float4(centre.xyz, 1.0f);
        const int reach = clamp(int(filterRadius), 1, kSkyVisFilterMaxRadius);
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
                const float4 s = rawIn.Load(int3(p, 0));
                if (s.w <= 0.0f) { continue; } // the sky
                const float3 Ns = normalize(gb1.SampleLevel(gSmpPoint, (float2(p) + 0.5f) * texel, 0).rgb * 2.0f - 1.0f);
                const float spatial = exp(-float(x * x + y * y) / (2.0f * sigma * sigma));
                const float wDepth = saturate(1.0f - (abs(s.w - z) / z) / max(filterDepthTolerance, 1.0e-4f));
                const float w = spatial * wDepth * SkyVisNormalWeight(N, Ns);
                sum += float4(s.xyz * w, w);
            }
        }
        bent = sum.xyz / sum.w;
    }

    // How much of the past a texel takes: Lumen's ScreenProbeGather temporal (LumenScreenProbeGatherTemporal.usf:481-527)
    // -- each texel counts the frames it has accumulated (the history corners' counts plus one, capped), and blends the
    // new rays at 1 / (1 + count): a running MEAN while it is young, the history weight's own share (0.98 = 49 frames)
    // once it is not. A fixed 0.98 kept a texel's FIRST frame at 0.98^n -- 82 % of it after 10 frames: a camera turning
    // 90 degrees a second at 120 fps brings ~10 new half-resolution columns in a frame, past the seed's reach, and each
    // dragged its eight rays' noise across the screen for half a second (2026-10-26, owner: "SkyVis is very noisy and
    // trails when the camera moves or turns fast").
    const float maxFrames = saturate(historyWeight) / max(1.0f - saturate(historyWeight), 1.0e-3f);
    float frames = 0.0f;
    bool kept = false;
    if (historyValid != 0u)
    {
        Texture2D velocity = ResourceDescriptorHeap[velocityIndex];
        Texture2D<float4> history = ResourceDescriptorHeap[historyIndex];
        Texture2D<float> counts = ResourceDescriptorHeap[countHistoryIndex];
        const float3 viewForward = mul(float4(0.0f, 0.0f, 1.0f, 0.0f), invView).xyz;
        float countSum;
        const float4 h = SkyVisHistoryAt(velocity, history, counts, uv, P, N, dot(P - camPos, viewForward), countSum);
        if (h.w > 0.0f)
        {
            // UE: a corner that is not this surface lends its bilinear weight to the new value
            // (lerp(NewValue, Corner, PositionWeight)); any corner that is keeps the texel's history.
            frames = min(countSum / h.w, maxFrames);
            bent = lerp(bent, h.xyz + bent * (1.0f - h.w), frames / (frames + 1.0f));
            kept = true;
        }
        else if (seedEnabled != 0u)
        {
            // UE DFAO FilterHistoryPS (DistanceFieldLightingPost.usf:297-361): "seeds newly rejected history values
            // (which are sources of temporal instability) with the results of a spatial search from stable history
            // values", as if that search were the history and these rays the new value (line 354). UE searches the
            // updated buffer in a pass of its own; here each neighbour's own reprojected history is read -- the same
            // values one blend earlier, and no texel reads another's output. The weights are the spatial step's
            // (above), so the seed comes from what the gather calls the same surface:
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
                    const float wDepth = saturate(1.0f - (abs(zP - z) / z) / max(filterDepthTolerance, 1.0e-4f));
                    if (wDepth <= 0.0f) { continue; }
                    const float3 PP = ReconstructPosWS(uvP, zP, invProj, invView);
                    const float3 NP = normalize(gb1.SampleLevel(gSmpPoint, uvP, 0).rgb * 2.0f - 1.0f);
                    float unusedCount;
                    const float4 hP = SkyVisHistoryAt(velocity, history, counts, uvP, PP, NP, dot(PP - camPos, viewForward),
                                                      unusedCount);
                    if (hP.w <= 1.0e-4f) { continue; }
                    const float w = exp(-float(x * x + y * y) * (1.0f / (2.0f * 1.5f * 1.5f))) * wDepth *
                                    SkyVisNormalWeight(N, NP);
                    acc += float4(hP.xyz / hP.w * w, w); // the corners that are this neighbour's surface, renormalised
                }
            }
            if (acc.w > 1.0e-4f)
            {
                frames = min(kSkyVisSeedFrames, maxFrames);
                bent = lerp(bent, acc.xyz / acc.w, frames / (frames + 1.0f));
            }
        }
    }
    accumOut[dtid.xy] = float4(bent, kept ? z : -z);
    countOut[dtid.xy] = frames * (1.0f / 255.0f);
    if (filterFirst != 0u)
    {
        targetOut[dtid.xy] = (debugView == 1u && !kept) ? float4(N, z) : float4(bent, z); // debug: no history = the open sky
    }
}

// `skyVis.filterFirst:0`, the A/B: the order before 2026-10-27 -- CSAccumulate blends the rays unfiltered and this
// gathers the ACCUMULATION into the target, with the same weights (SVGF's order; the jitter's per-frame weights are
// then never averaged, the tent's hem trembled 4-7 %).
[numthreads(8, 8, 1)]
[RootSignature(RT_SKYVIS_CS_RS)]
void CSFilterHistory(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= outWidth || dtid.y >= outHeight)
    {
        return;
    }
    Texture2D<float4> accum = ResourceDescriptorHeap[accumIndex];
    RWTexture2D<float4> targetOut = ResourceDescriptorHeap[targetUavIndex];
    Texture2D gb1 = ResourceDescriptorHeap[gb1Index];
    const float4 centre = accum[dtid.xy];
    const float centreZ = abs(centre.w);
    if (centreZ <= 0.0f)
    {
        targetOut[dtid.xy] = 0.0f.xxxx;
        return;
    }
    const float2 texel = 1.0f / float2(outWidth, outHeight);
    const float3 N = normalize(gb1.SampleLevel(gSmpPoint, (float2(dtid.xy) + 0.5f) * texel, 0).rgb * 2.0f - 1.0f);
    if (debugView == 1u && centre.w < 0.0f)
    {
        targetOut[dtid.xy] = float4(N, centreZ);
        return;
    }
    if (filterEnabled == 0u)
    {
        targetOut[dtid.xy] = float4(centre.xyz, centreZ);
        return;
    }
    float4 sum = float4(centre.xyz, 1.0f);
    const int reach = clamp(int(filterRadius), 1, kSkyVisFilterMaxRadius);
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
            const float4 s = accum.Load(int3(p, 0));
            const float sZ = abs(s.w);
            if (sZ <= 0.0f) { continue; }
            const float3 Ns = normalize(gb1.SampleLevel(gSmpPoint, (float2(p) + 0.5f) * texel, 0).rgb * 2.0f - 1.0f);
            const float spatial = exp(-float(x * x + y * y) / (2.0f * sigma * sigma));
            const float wDepth = saturate(1.0f - (abs(sZ - centreZ) / centreZ) / max(filterDepthTolerance, 1.0e-4f));
            const float w = spatial * wDepth * SkyVisNormalWeight(N, Ns);
            sum += float4(s.xyz * w, w);
        }
    }
    targetOut[dtid.xy] = float4(sum.xyz / sum.w, centreZ);
}
