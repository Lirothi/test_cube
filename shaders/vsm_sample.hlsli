// Rung 2 / Step 21: virtual->physical shadow sampling for the LOCAL lights (spot + point-face).
// Projects a receiver into a light's virtual shadow space, picks the mip level by camera distance
// (mirrors the request/setup passes), looks the virtual page up in the page table, and — if the
// page is resident — SampleCmp's the physical page in the pool with a 3x3 PCF clamped to the page
// (so PCF never bleeds into a neighbouring page). A not-resident page falls back to lit (1.0).
// Directional/CSM is NOT handled here (it stays on the cascade path until Step 24).
#ifndef VSM_SAMPLE_HLSLI
#define VSM_SAMPLE_HLSLI
#include "vsm_addressing.hlsli"

static const uint VSM_SAMPLE_RESIDENT_BIT = 0x80000000u;
static const uint VSM_SAMPLE_PHYS_MASK    = 0x0000FFFFu;

// Sample a resident/virtual page given the receiver's light-space NDC + full-view shadow UV.
// Starts at the distance-selected mip level and walks to COARSER levels until it finds a resident
// page (the request marks the whole chain from the selected level up, so a coarser page is almost
// always resident even when the exact level isn't — this is what stops shadows popping in/out as
// the camera distance nudges the selected level across a threshold). Falls back to lit only if no
// level is resident.
float VsmSampleNDC(uint view, float3 ndc, float2 uv, float distCam, float refDist, float depthBias,
                   StructuredBuffer<uint> PageTable, Texture2D Pool, SamplerComparisonState cmp)
{
    const uint startLevel = VsmSelectLevel(distCam, refDist, VSM_MAX_LEVEL);
    const float invPoolAxis = 1.0f / (float)VSM_POOL_PAGES_AXIS;
    const float texel = 1.0f / (float)(VSM_POOL_PAGES_AXIS * VSM_PAGE_SIZE); // 1/4096
    const float depth = ndc.z - depthBias;

    for (uint level = startLevel; level <= VSM_MAX_LEVEL; ++level)
    {
        const uint axis = VSM_L0_AXIS >> level;
        const uint px = min((uint)(uv.x * axis), axis - 1u);
        const uint py = min((uint)(uv.y * axis), axis - 1u);
        const uint entry = PageTable[VsmPageId(view, level, px, py)];
        if ((entry & VSM_SAMPLE_RESIDENT_BIT) == 0u) { continue; } // try a coarser level

        const uint phys = entry & VSM_SAMPLE_PHYS_MASK;
        const float gx = (float)(phys % VSM_POOL_PAGES_AXIS);
        const float gy = (float)(phys / VSM_POOL_PAGES_AXIS);
        const float2 uvInPage = saturate(uv * axis - float2(px, py)); // position within the page [0,1]
        const float2 poolUV = (float2(gx, gy) + uvInPage) * invPoolAxis;

        // 3x3 PCF, one pool texel, clamped to this page's pool region (no neighbour bleed).
        const float2 pmin = float2(gx, gy) * invPoolAxis + 0.5f * texel;
        const float2 pmax = (float2(gx, gy) + 1.0f) * invPoolAxis - 0.5f * texel;
        float sh = 0.0f;
        [unroll] for (int y = -1; y <= 1; ++y)
        {
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                float2 s = clamp(poolUV + float2(x, y) * texel, pmin, pmax);
                sh += Pool.SampleCmpLevelZero(cmp, s, depth);
            }
        }
        return sh / 9.0f;
    }
    return 1.0f; // no resident level -> lit fallback
}

// Spot: `view` = the spot's shadow slot (0..kMaxShadowedSpotLights-1). Pbiased = P + N*normalBias.
float VsmSpotShadow(uint view, float4x4 viewProj, float3 Pbiased, float3 camPos, float refDist,
                    float depthBias, StructuredBuffer<uint> PageTable, Texture2D Pool,
                    SamplerComparisonState cmp)
{
    float4 clip = mul(float4(Pbiased, 1.0f), viewProj);
    if (clip.w <= 0.0f) { return 1.0f; }
    float3 ndc = clip.xyz / clip.w;
    if (any(abs(ndc.xy) > 1.0f) || ndc.z < 0.0f || ndc.z > 1.0f) { return 1.0f; }
    float2 uv = float2(0.5f * ndc.x + 0.5f, 0.5f - 0.5f * ndc.y);
    float distCam = length(Pbiased - camPos);
    return VsmSampleNDC(view, ndc, uv, distCam, refDist, depthBias, PageTable, Pool, cmp);
}

// Point: `slot` = the light's cube slot. The 6 cube faces (VSM local views 8 + slot*6 + face) are
// rendered with LookAtLH(lightPos, lightPos+kCubeDir[face], kCubeUp[face]) * PerspectiveFovLH(90,
// 1, near, far) — the exact D3D face order (+X,-X,+Y,-Y,+Z,-Z). The receiver's direction major axis
// picks the one face whose 90° cone contains it (the faces tile the sphere exactly, no overlap), so
// we reconstruct just that face's view-proj here (no per-face buffer needed) and sample it.
float VsmPointShadow(uint slot, float3 Pbiased, float3 lightPos, float nearP, float farP,
                     float3 camPos, float refDist, float depthBias,
                     StructuredBuffer<uint> PageTable, Texture2D Pool, SamplerComparisonState cmp)
{
    const uint kSpotViews = 8u; // kMaxShadowedSpotLights (point face views start after the spots)
    float3 d = Pbiased - lightPos;
    float3 ad = abs(d);

    uint face; float3 fwd; float3 up; // fwd = kCubeDir[face], up = kCubeUp[face]
    if (ad.x >= ad.y && ad.x >= ad.z)
    {
        face = (d.x >= 0.0f) ? 0u : 1u; fwd = float3((d.x >= 0.0f) ? 1.0f : -1.0f, 0, 0); up = float3(0, 1, 0);
    }
    else if (ad.y >= ad.z)
    {
        face = (d.y >= 0.0f) ? 2u : 3u; fwd = float3(0, (d.y >= 0.0f) ? 1.0f : -1.0f, 0); up = float3(0, 0, (d.y >= 0.0f) ? -1.0f : 1.0f);
    }
    else
    {
        face = (d.z >= 0.0f) ? 4u : 5u; fwd = float3(0, 0, (d.z >= 0.0f) ? 1.0f : -1.0f); up = float3(0, 1, 0);
    }

    // LookAtLH (row-major, row-vector): right/up/fwd basis + translation.
    float3 right = normalize(cross(up, fwd));
    float3 upN = cross(fwd, right);
    float4x4 view = float4x4(right.x, upN.x, fwd.x, 0.0f,
                             right.y, upN.y, fwd.y, 0.0f,
                             right.z, upN.z, fwd.z, 0.0f,
                             -dot(right, lightPos), -dot(upN, lightPos), -dot(fwd, lightPos), 1.0f);
    // PerspectiveFovLH(90,1,near,far): xScale=yScale=cot(45)=1.
    float fRange = farP / (farP - nearP);
    float4x4 proj = float4x4(1.0f, 0.0f, 0.0f, 0.0f,
                             0.0f, 1.0f, 0.0f, 0.0f,
                             0.0f, 0.0f, fRange, 1.0f,
                             0.0f, 0.0f, -fRange * nearP, 0.0f);
    float4x4 vp = mul(view, proj);

    float4 clip = mul(float4(Pbiased, 1.0f), vp);
    if (clip.w <= 0.0f) { return 1.0f; }
    float3 ndc = clip.xyz / clip.w;
    if (any(abs(ndc.xy) > 1.0f) || ndc.z < 0.0f || ndc.z > 1.0f) { return 1.0f; }
    float2 uv = float2(0.5f * ndc.x + 0.5f, 0.5f - 0.5f * ndc.y);
    float distCam = length(Pbiased - camPos);
    return VsmSampleNDC(kSpotViews + slot * 6u + face, ndc, uv, distCam, refDist, depthBias,
                        PageTable, Pool, cmp);
}

#endif // VSM_SAMPLE_HLSLI
