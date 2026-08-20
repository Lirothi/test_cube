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

// P16.16. UE clamp the receiver-plane bias to `abs(100 * ShadowViewToClipMatrix._33)` -- a hundred
// world units expressed in the level's own NDC. Ours is already NDC, and a clipmap level's depth
// range is 6x its extent, so the equivalent bound is a fixed slice of that range. It exists only to
// stop a near-grazing receiver, where the plane gradient diverges, from flinging its shadow away;
// it should never bind on anything shaded normally.
static const float VSM_MAX_SLOPE_BIAS_NDC = 0.01f;
// UE's clamp on the gradient itself (ComputeDepthSlopeDirectionalUV): "Clamp to avoid excessive
// degenerate slope biases causing flickering lit pixels". THEIR NUMBER IS 0.05 AND IT DOES NOT
// TRANSFER -- it is expressed in a depth range that is not ours. UE's clipmap level spans
// `ZRangeScale` (1000) times its radius; ours spans 6x its extent, i.e. 12x its radius. Our NDC
// per world unit is therefore ~83x theirs and so are our gradients: for flat ground under this
// level's 28-degree sun the real gradient is tan(theta)/6 = 0.31, which their 0.05 would crush
// SIXFOLD -- measured, and it produced less bias than the flat constant it replaced.
//
// So the bound is derived here instead of copied. `DepthSlopeUV` works out to tan(theta)/6 for this
// projection, and bounding tan(theta) at 8 (about 83 degrees off the light, past which a receiver
// contributes nothing worth defending) gives 8/6.
static const float VSM_MAX_DEPTH_SLOPE_UV = 1.34f;

// Sample a resident/virtual page given the receiver's light-space NDC + full-view shadow UV.
// Starts at the distance-selected mip level and walks to COARSER levels until it finds a resident
// page (the request marks the whole chain from the selected level up, so a coarser page is almost
// always resident even when the exact level isn't — this is what stops shadows popping in/out as
// the camera distance nudges the selected level across a threshold). Falls back to lit only if no
// level is resident.
// P16.16 -- RECEIVER-PLANE DEPTH BIAS, per tap. Transcribed from Unreal's
// `ComputeVirtualShadowMapOptimalSlopeBias` (VirtualShadowMapProjectionCommon.ush).
//
// A constant push cannot win here, and the reason is not that it is badly tuned. The error a shadow
// lookup makes is the depth difference between where the receiver ACTUALLY is and the centre of the
// texel its sample snapped to. That difference depends on the receiver's slope, on the texel size,
// and on where inside the texel the sample happened to land -- so a constant is simultaneously too
// large for a sample sitting on a texel centre (which needs none, and gets peter-panning) and too
// small for one at a corner of a tilted texel (which gets acne).
//
// `depthSlopeUV` is the receiver plane's depth gradient per unit shadow UV, built by the caller.
// Dotted with the offset from the sample to the texel being compared, it IS that difference -- not
// an estimate of it. Zero on a texel centre, maximal at a corner. UE's 2x factor is kept, with
// their comment: "2x factor due to lack of precision (probably)".
//
// TWO offsets go into it, and only the first is UE's, because their SMRT walks single texels while
// this samples a 3x3 PCF: the sub-texel offset to the nearest texel centre, plus the tap's own
// offset in texels. A corner tap of a 3x3 therefore biases about 1.4x the centre one, which is
// correct and was not happening when one bias served all nine.
//
// UE ALSO scale by `1 << (sampledLevel - requestedLevel)` when the sampler falls back to a coarser
// level. That is not a separate step here: the offset is converted to UV using the dimensions of
// the level ACTUALLY sampled, inside the fallback loop, so a coarser level's larger texel widens
// the offset by exactly the same factor. Same result, one fewer thing to keep in step.
float VsmSampleNDC(uint view, float3 ndc, float2 uv, float distCam, float refDist, float depthBias,
                   float2 depthSlopeUV,
                   StructuredBuffer<uint> PageTable, Texture2D Pool, SamplerComparisonState cmp)
{
    const uint startLevel = VsmSelectLevel(distCam, refDist, VSM_MAX_LEVEL);
    const float invPoolAxis = 1.0f / (float)VSM_POOL_PAGES_AXIS;
    const float texel = 1.0f / (float)(VSM_POOL_PAGES_AXIS * VSM_PAGE_SIZE); // 1/4096

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

        // The receiver-plane bias is built in THIS level's units: `levelDims` is how many texels
        // the view spans at the level that actually turned out to be resident, which is where the
        // per-level scaling comes from (see the note above).
        const float levelDims = (float)(axis * VSM_PAGE_SIZE);
        const float2 exactTexel = uv * levelDims;
        const float2 toTexelCentre = (floor(exactTexel) + 0.5f) - exactTexel;
        const float invLevelDims = 1.0f / levelDims;

        float sh = 0.0f;
        [unroll] for (int y = -1; y <= 1; ++y)
        {
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                const float2 s = clamp(poolUV + float2(x, y) * texel, pmin, pmax);
                const float2 offsetUV = (toTexelCentre + float2(x, y)) * invLevelDims;
                // UE clamp the result against the projection's depth scale; the equivalent bound
                // here is a slice of this level's own NDC range, which `depthBias` is expressed in.
                const float slopeBias = min(2.0f * max(0.0f, dot(depthSlopeUV, offsetUV)),
                                            VSM_MAX_SLOPE_BIAS_NDC);
                sh += Pool.SampleCmpLevelZero(cmp, s, ndc.z - (depthBias + slopeBias));
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
    // Spot/point keep their own tuned constant bias (P16.16 changed the DIRECTIONAL clipmap
    // only); a zero gradient makes the receiver-plane term vanish, so this is an exact no-op.
    return VsmSampleNDC(view, ndc, uv, distCam, refDist, depthBias, float2(0.0f, 0.0f),
                        PageTable, Pool, cmp);
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
                        float2(0.0f, 0.0f), PageTable, Pool, cmp);
}

// Directional clipmap (Step 24f): the receiver picks the FINEST level whose ortho extent contains it
// (levels ordered finest->coarsest in slots [VSM_NUM_LOCAL_VIEWS, +VSM_NUM_CLIPMAP_LEVELS)); the
// clipmap LEVEL is the LOD, so we sample page-level 0 (distCam 0 forces VsmSampleNDC's start level 0).
// `clipVP[i]` = clipmap level i's camera-centered ortho viewProj (built CPU-side, texel-snapped).
// Not resident / outside every level -> lit (1.0). Mirrors VsmSpotShadow but with level selection.
// P16.16 -- P = un-offset receiver, N = the SHADING normal (which is what UE use here too:
// `GetEstimatedGeoWorldNormal` sits beside their projection marked "not currently used").
//
// TWO biases, matching Unreal's split:
//
//  * a NORMAL OFFSET, `VirtualShadowMapGetNormalBiasLength`:
//        P += N * max(0.02cm, NormalBias * distanceToCamera / cot(hFov/2))
//    Note what it is referred to -- the world size of a SCREEN PIXEL (distance x tan(hFov/2)), not
//    the shadow texel. `normalBias` carries UE's own units (their CVar default 0.5, divided by 1000
//    on the CPU exactly as `GetNormalBiasForShader` does), so the number here is directly
//    comparable to `r.Shadow.Virtual.NormalBias`. The old form was `texels * dist/1024`, which at
//    its shipped 2.0 was 3.9x this and ignored the field of view entirely.
//
//  * the RECEIVER-PLANE bias, per tap, inside VsmSampleNDC -- the part that actually does the work.
//    `uvNormalMatrix` is the inverse transpose of world -> shadow UVZ, built CPU-side the way UE
//    build `TranslatedWorldToShadowUVNormalMatrix`. ONE matrix serves every level: the gradient it
//    produces is a ratio of xy to z, and both scale with the level's extent, so the extent cancels.
//    (`CalcClipmapUvNormalMatrix` on the CPU asserts that proportionality holds.)
// Constant depth bias per level: `depthBias` is NDC, and a level's NDC range is 6x its extent, so
// a constant NDC value is a constant bias in TEXELS (ndc * 6 * 2048) whose WORLD size doubles per
// level -- which is what detaches thin far shadows when the base value is raised. `depthBiasDecay`
// shrinks it per level (0.5 = a constant WORLD-size bias instead), and `depthBiasFloorNdc` is the
// lower bound that keeps the far levels above the D16 pool's quantization (6*2048/65536 = 0.19
// texel = the hard floor; the CPU authors it in texels). decay 1 + floor 0 = the legacy constant.
float VsmClipmapShadow(float3 P, float3 N, float3 camPos, float normalBias, float depthBias,
                       float depthBiasDecay, float depthBiasFloorNdc,
                       float tanHalfFovX, float4x4 uvNormalMatrix,
                       float4x4 clipVP[VSM_NUM_CLIPMAP_LEVELS],
                       StructuredBuffer<uint> PageTable, Texture2D Pool, SamplerComparisonState cmp)
{
    const float dist = length(P - camPos);
    // 0.0002 m is UE's 0.02 cm floor converted; their world is centimetres, the dimensionless
    // NormalBias factor transfers unchanged.
    const float3 Poff = P + N * max(0.0002f, normalBias * dist * tanHalfFovX);

    // The receiver plane, taken to shadow UV space. Only .xyz of the result is used, and for an
    // affine transform that part does not depend on the plane's distance term -- the `-dot(N, P)`
    // is carried because UE carry it and because it costs nothing.
    const float4 planeUV = mul(float4(N, -dot(N, P)), uvNormalMatrix);
    const float planeZ = (abs(planeUV.z) < 1e-8f) ? 1e-8f : planeUV.z;
    const float2 depthSlopeUV = clamp(-planeUV.xy / planeZ,
                                      -VSM_MAX_DEPTH_SLOPE_UV, VSM_MAX_DEPTH_SLOPE_UV);

    for (uint i = 0u; i < VSM_NUM_CLIPMAP_LEVELS; ++i)
    {
        float4 clip = mul(float4(Poff, 1.0f), clipVP[i]);
        if (clip.w <= 0.0f) { continue; }
        float3 ndc = clip.xyz / clip.w;
        if (any(abs(ndc.xy) > 1.0f) || ndc.z < 0.0f || ndc.z > 1.0f) { continue; } // outside this level
        const float2 uv = float2(0.5f * ndc.x + 0.5f, 0.5f - 0.5f * ndc.y);
        // Residency pre-check: if this level's page isn't resident (a level-boundary strip the
        // sub-sampled request missed), fall through to the NEXT, coarser clipmap level — which covers
        // the same spot and is resident there — instead of returning lit. Kills the bright seam.
        const uint px = min((uint)(uv.x * (float)VSM_L0_AXIS), VSM_L0_AXIS - 1u);
        const uint py = min((uint)(uv.y * (float)VSM_L0_AXIS), VSM_L0_AXIS - 1u);
        const uint entry = PageTable[VsmPageId(VSM_NUM_LOCAL_VIEWS + i, 0u, px, py)];
        if ((entry & VSM_SAMPLE_RESIDENT_BIT) == 0u) { continue; } // not resident -> coarser level
        // Bias of the level we LANDED on (matches how the receiver-plane bias scales: a residency
        // fallback to a coarser level uses that level's own values).
        const float levelDepthBias = max(depthBias * pow(depthBiasDecay, (float)i), depthBiasFloorNdc);
        return VsmSampleNDC(VSM_NUM_LOCAL_VIEWS + i, ndc, uv, 0.0f, 1.0f, levelDepthBias, depthSlopeUV,
                            PageTable, Pool, cmp);
    }
    return 1.0f; // outside all clipmap levels
}

#endif // VSM_SAMPLE_HLSLI
