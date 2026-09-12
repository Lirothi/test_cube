// Volumetric clouds, plan C3: the cloud shadow map -- an orthographic view from the sun over the
// ground under the camera, each texel a march of the density along the light through the layer.
// Transcription of UE VolumetricCloud.usf MainPS under SHADER_SHADOW_PS (:2067-2219) and the
// spatial filter MainShadowFilterCS (:2333-2348). What it writes and how it is read is in
// cloud_shadow_common.hlsli. The matrix, the snapping and the sample count are built on the CPU
// (VolumetricCloud.cpp BuildShadowMatrix, UE VolumetricCloudRendering.cpp:1725-1810).
//
// Deltas: UE trace at TWICE the map's resolution and their filter is the 2x2 reduction with the
// "mean minus deviation" front depth; ours traces at the map's resolution and filters 3x3 in place
// with the same depth rule -- a quarter of the density samples for a map that is read bilinearly
// anyway. UE's optional temporal filter (off by default, NewFrameWeight 1) is not carried.
//
// Two entry points: CSTrace (b0 CloudCB, t0..t2 the noise set, u0 the raw map) and
// CSFilter (t3 the raw map, u0 the filtered map).
#pragma pack_matrix(row_major)
#define CLOUD_DENSITY
#define CLOUD_T_BASE t0
#define CLOUD_T_DETAIL t1
#define CLOUD_T_WEATHER t2
#define CLOUD_S_WRAP s0
#include "cloud_common.hlsli"

#define CLOUD_SHADOW_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(Sampler(s0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE))"

Texture2D<float4> RawShadow : register(t3);
RWTexture2D<float4> OutShadow : register(u0);

[numthreads(8, 8, 1)]
[RootSignature(CLOUD_SHADOW_RS)]
void CSTrace(uint3 id : SV_DispatchThreadID)
{
    const uint resolution = (uint)cloudShadowMap.x;
    if (any(id.xy >= resolution)) { return; }
    const float2 uv = (float2(id.xy) + 0.5f) * cloudShadowMap.y;
    const float farDepthKm = cloudShadowMap.z;
    const float strength = cloudShadowMap.w;

    // usf:2113-2115: the texel's point on the map's NEAR plane (our ortho: clip z 0), and the light
    // direction it marches along -- from the light to the ground.
    const float4 nearClip = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    const float4 nearH = mul(nearClip, cloudShadowInvViewProj);
    const float3 nearWorld = nearH.xyz / nearH.w;
    const float3 lightDir = -cloudSun.xyz;

    // usf:2119-2165: the segment of the light ray inside the layer.
    const float3 originKm = CloudWorldToPlanetKm(nearWorld);
    float2 tTop, tBottom;
    float tMin, tMax;
    if (!CloudRaySphere(originKm, lightDir, 0.0f.xxx, cloudLayer.y, tTop))
    {
        OutShadow[id.xy] = float4(farDepthKm, 0.0f, 0.0f, 0.0f); // no intersection with the top of the layer
        return;
    }
    if (CloudRaySphere(originKm, lightDir, 0.0f.xxx, cloudLayer.x, tBottom))
    {
        float tempTop = all(tTop > 0.0f) ? min(tTop.x, tTop.y) : max(tTop.x, tTop.y);
        const float tempBottom = all(tBottom > 0.0f) ? min(tBottom.x, tBottom.y) : max(tBottom.x, tBottom.y);
        if (all(tBottom > 0.0f))
        {
            tempTop = max(0.0f, min(tTop.x, tTop.y));
        }
        else
        {
            // Under the layer already: nothing above this texel to shadow it (usf:2142).
            OutShadow[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
            return;
        }
        tMin = min(tempBottom, tempTop);
        tMax = max(tempBottom, tempTop);
    }
    else
    {
        tMin = tTop.x;
        tMax = tTop.y;
    }
    tMin = max(0.0f, tMin);
    tMax = max(0.0f, tMax);

    // usf:2167-2207: linear samples, the mean extinction of the medium met, the optical depth of
    // the whole column, the depth of the first medium.
    const float lengthKm = tMax - tMin;
    const float steps = max(cloudShadowMap2.x, 1.0f);
    const float dtMetres = lengthKm * CloudMetresPerKm / steps;
    float extinctionAcc = 0.0f, extinctionCount = 0.0f, maxOpticalDepth = 0.0f;
    float nearDepthKm = farDepthKm;
    const float invLayerHeight = cloudLayer.z;
    [loop] for (float st = 0.5f; st < steps; st += 1.0f)
    {
        const float sampleKm = lengthKm * (st / steps);
        const float3 P = nearWorld + lightDir * ((tMin + sampleKm) * CloudMetresPerKm);
        const float normAlt = (length(CloudWorldToPlanetKm(P)) - cloudLayer.x) * invLayerHeight;
        if (normAlt <= 0.0f || normAlt >= 1.0f) { continue; }
        const CloudSample s = CloudSampleAt(P, normAlt);
        const bool present = s.extinction > 0.0f;
        nearDepthKm = present ? min(nearDepthKm, sampleKm) : nearDepthKm;
        extinctionAcc += s.extinction;
        maxOpticalDepth += s.extinction * dtMetres;
        extinctionCount += present ? 1.0f : 0.0f;
    }
    const float meanExtinction = strength * extinctionAcc / max(1.0f, extinctionCount);
    const float maxGreyOpticalDepth = strength * maxOpticalDepth;
    const bool noHit = nearDepthKm == farDepthKm;
    const float frontDepthKm = noHit ? tMax : (tMin + nearDepthKm);
    OutShadow[id.xy] = float4(max(0.0f, frontDepthKm + cloudShadowMap2.y), meanExtinction, maxGreyOpticalDepth, 0.0f);
}

[numthreads(8, 8, 1)]
[RootSignature(CLOUD_SHADOW_RS)]
void CSFilter(uint3 id : SV_DispatchThreadID)
{
    const int resolution = (int)cloudShadowMap.x;
    if (any(int2(id.xy) >= resolution)) { return; }
    // usf:2333-2348 on a 3x3 footprint: the front depth is the mean MINUS the mean absolute
    // deviation (the shadow starts where the nearest cloud in the footprint starts, not halfway
    // down it), the two extinction terms are plain means.
    float depthSum = 0.0f, extinction = 0.0f, opticalDepth = 0.0f;
    float depths[9];
    int n = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        const int2 c = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(resolution - 1, resolution - 1));
        const float3 d = RawShadow.Load(int3(c, 0)).rgb;
        depths[n++] = d.r;
        depthSum += d.r;
        extinction += d.g;
        opticalDepth += d.b;
    }
    const float mean = depthSum / 9.0f;
    float deviation = 0.0f;
    [unroll] for (int k = 0; k < 9; ++k) { deviation += abs(depths[k] - mean); }
    deviation /= 9.0f;
    OutShadow[id.xy] = float4(max(0.0f, mean - deviation), extinction / 9.0f, opticalDepth / 9.0f, 0.0f);
}
