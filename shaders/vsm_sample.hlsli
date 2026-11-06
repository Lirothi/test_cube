// Rung 2 / Step 21: virtual->physical shadow sampling for the LOCAL lights (spot + point-face).
// Projects a receiver into a light's virtual shadow space, picks the mip level by camera distance
// (mirrors the request/setup passes), looks the virtual page up in the page table, and — if the
// page is resident — SampleCmp's the physical page in the pool with a 3x3 PCF clamped to the page
// (so PCF never bleeds into a neighbouring page). A not-resident page falls back to lit (1.0).
// Directional/CSM is NOT handled here (it stays on the cascade path until Step 24).
#ifndef VSM_SAMPLE_HLSLI
#define VSM_SAMPLE_HLSLI
#include "vsm_addressing.hlsli"
#include "vsm_smrt.hlsli"

static const uint VSM_SAMPLE_RESIDENT_BIT = VSM_RESIDENT_BIT_C; // see vsm_addressing.hlsli
static const uint VSM_SAMPLE_PHYS_MASK    = VSM_PHYS_MASK_C;

// P16.16. UE clamp the receiver-plane bias to `abs(100 * ShadowViewToClipMatrix._33)` -- a hundred
// world units expressed in the level's own NDC. It exists only to stop a near-grazing receiver,
// where the plane gradient diverges, from flinging its shadow away; with the gradient clamped below
// it does not bind on a page's texels.
static const float VSM_MAX_SLOPE_BIAS_NDC = 0.01f;
// UE's clamp on the gradient itself (ComputeDepthSlopeDirectionalUV): "Clamp to avoid excessive
// degenerate slope biases causing flickering lit pixels". Their number: a level's depth range and
// its side are UE's now (2000 radii against 4), so a gradient means what it means there.
static const float VSM_MAX_DEPTH_SLOPE_UV = 0.05f;

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
// Plan S10e: from a given mip on (the local lights' pixel-footprint mip); VsmSampleNDC below picks it by distance.
float VsmSampleNDCFrom(uint view, uint startLevel, float3 ndc, float2 uv, float depthBias, float2 depthSlopeUV,
                       StructuredBuffer<uint> PageTable, Texture2D Pool, SamplerComparisonState cmp)
{
    const float2 invPool = 1.0f / float2((float)VSM_POOL_PAGES_X, (float)VSM_POOL_PAGES_Y);
    const float2 texel = invPool / (float)VSM_PAGE_SIZE;
    const uint lastLevel = VsmIsClipmapView(view) ? 0u : VSM_MAX_LEVEL; // a clipmap level has no mips

    for (uint level = startLevel; level <= lastLevel; ++level)
    {
        const uint axis = VsmViewAxis(view, level);
        const uint px = min((uint)(uv.x * axis), axis - 1u);
        const uint py = min((uint)(uv.y * axis), axis - 1u);
        const uint entry = PageTable[VsmPageId(view, level, px, py)];
        if ((entry & VSM_SAMPLE_RESIDENT_BIT) == 0u) { continue; } // try a coarser level

        const uint phys = entry & VSM_SAMPLE_PHYS_MASK;
        const float gx = (float)(phys % VSM_POOL_PAGES_X);
        const float gy = (float)(phys / VSM_POOL_PAGES_X);
        const float2 uvInPage = saturate(uv * axis - float2(px, py)); // position within the page [0,1]
        const float2 poolUV = (float2(gx, gy) + uvInPage) * invPool;

        // 3x3 PCF, one pool texel, clamped to this page's pool region (no neighbour bleed).
        const float2 pmin = float2(gx, gy) * invPool + 0.5f * texel;
        const float2 pmax = (float2(gx, gy) + 1.0f) * invPool - 0.5f * texel;

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
                // SIGN: the tap that needs covering is the one whose texel centre lies on the
                // SHALLOW side of the receiver plane (stored depth closer to the light than the
                // receiver), i.e. dot(g, offset) NEGATIVE in this engine's z-forward convention --
                // UE's identical-looking max(0, +dot) is written against their REVERSED shadow
                // depth. Transcribing the formula verbatim biased exactly the taps that never
                // needed it and left the needy half of the PCF kernel in acne.
                const float slopeBias = min(2.0f * max(0.0f, -dot(depthSlopeUV, offsetUV)),
                                            VSM_MAX_SLOPE_BIAS_NDC);
                sh += Pool.SampleCmpLevelZero(cmp, s, ndc.z - (depthBias + slopeBias));
            }
        }
        return sh / 9.0f;
    }
    return 1.0f; // no resident level -> lit fallback
}

float VsmSampleNDC(uint view, float3 ndc, float2 uv, float distCam, float refDist, float depthBias,
                   float2 depthSlopeUV,
                   StructuredBuffer<uint> PageTable, Texture2D Pool, SamplerComparisonState cmp)
{
    return VsmSampleNDCFrom(view, VsmSelectLevel(distCam, refDist, VSM_MAX_LEVEL), ndc, uv, depthBias, depthSlopeUV,
                            PageTable, Pool, cmp);
}

// Spot: `view` = the spot's VSM view (plan S9e: SpotLightGpu::shadowParams4.z, vsm::SpotView of its slot -- the light
// carries it, the receiver knows no layout). Pbiased = P + N*normalBias. Plan S10e: from mip `startLevel` (the
// request's; VsmSpotShadow picks it by distance).
float VsmSpotShadowFrom(uint view, uint startLevel, float4x4 viewProj, float3 Pbiased, float depthBias,
                        StructuredBuffer<uint> PageTable, Texture2D Pool, SamplerComparisonState cmp)
{
    float4 clip = mul(float4(Pbiased, 1.0f), viewProj);
    if (clip.w <= 0.0f) { return 1.0f; }
    float3 ndc = clip.xyz / clip.w;
    if (any(abs(ndc.xy) > 1.0f) || ndc.z < 0.0f || ndc.z > 1.0f) { return 1.0f; }
    float2 uv = float2(0.5f * ndc.x + 0.5f, 0.5f - 0.5f * ndc.y);
    // Spot/point keep their own tuned constant bias (P16.16 changed the DIRECTIONAL clipmap
    // only); a zero gradient makes the receiver-plane term vanish, so this is an exact no-op.
    return VsmSampleNDCFrom(view, startLevel, ndc, uv, depthBias, float2(0.0f, 0.0f), PageTable, Pool, cmp);
}
float VsmSpotShadow(uint view, float4x4 viewProj, float3 Pbiased, float3 camPos, float refDist,
                    float depthBias, StructuredBuffer<uint> PageTable, Texture2D Pool,
                    SamplerComparisonState cmp)
{
    return VsmSpotShadowFrom(view, VsmSelectLevel(length(Pbiased - camPos), refDist, VSM_MAX_LEVEL), viewProj, Pbiased,
                             depthBias, PageTable, Pool, cmp);
}

// Point: `firstView` = the VSM view of the light's +X face (plan S9e: PointLightGpu::shadowParams4.z, vsm::PointFaceView
// of slot * 6); its six faces are the views firstView + face (a light never straddles the layout's prefix: it holds
// whole lights). They are rendered with LookAtLH(lightPos, lightPos+kCubeDir[face], kCubeUp[face]) * PerspectiveFovLH(90,
// 1, near, far) — the exact D3D face order (+X,-X,+Y,-Y,+Z,-Z). The receiver's direction major axis
// picks the one face whose 90° cone contains it (the faces tile the sphere exactly, no overlap), so
// we reconstruct just that face's view-proj here (no per-face buffer needed) and sample it.
// Plan S10b: P in its cube face's VSM view -- the face's view, NDC and UV; false outside it.
bool VsmPointFaceProject(uint firstView, float3 Pbiased, float3 lightPos, float nearP, float farP,
                         out uint vsmView, out float3 ndc, out float2 uv)
{
    vsmView = 0u; ndc = float3(0.0f, 0.0f, 0.0f); uv = float2(0.0f, 0.0f);
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
    if (clip.w <= 0.0f) { return false; }
    ndc = clip.xyz / clip.w;
    if (any(abs(ndc.xy) > 1.0f) || ndc.z < 0.0f || ndc.z > 1.0f) { return false; }
    uv = float2(0.5f * ndc.x + 0.5f, 0.5f - 0.5f * ndc.y);
    vsmView = firstView + face;
    return true;
}

// Plan S10e: from mip `startLevel` (the request's); VsmPointShadow picks it by distance.
float VsmPointShadowFrom(uint firstView, uint startLevel, float3 Pbiased, float3 lightPos, float nearP, float farP,
                         float depthBias, StructuredBuffer<uint> PageTable, Texture2D Pool, SamplerComparisonState cmp)
{
    uint view;
    float3 ndc;
    float2 uv;
    if (!VsmPointFaceProject(firstView, Pbiased, lightPos, nearP, farP, view, ndc, uv)) { return 1.0f; }
    return VsmSampleNDCFrom(view, startLevel, ndc, uv, depthBias, float2(0.0f, 0.0f), PageTable, Pool, cmp);
}
float VsmPointShadow(uint firstView, float3 Pbiased, float3 lightPos, float nearP, float farP,
                     float3 camPos, float refDist, float depthBias,
                     StructuredBuffer<uint> PageTable, Texture2D Pool, SamplerComparisonState cmp)
{
    return VsmPointShadowFrom(firstView, VsmSelectLevel(length(Pbiased - camPos), refDist, VSM_MAX_LEVEL), Pbiased,
                              lightPos, nearP, farP, depthBias, PageTable, Pool, cmp);
}

// Plan S10e: a point receiver's depth in its cube face's view -- the major axis of P - light (the face's view z).
float VsmPointFaceDepth(float3 P, float3 lightPos)
{
    const float3 a = abs(P - lightPos);
    return max(a.x, max(a.y, a.z));
}

// Plan S10b: the mip VsmSampleNDC will actually read for `uv` -- the first resident level from `startLevel` (the one
// the request asked for). A receiver whose page fell back to a coarser level sits in BIGGER texels than the requested
// level's; a push sized for the requested level leaves the whole fallen-back page in acne (the owner saw it as hatched
// quads with bright seams).
uint VsmResidentLevel(uint view, float2 uv, uint startLevel, StructuredBuffer<uint> PageTable)
{
    uint level = startLevel;
    for (; level < VSM_MAX_LEVEL; ++level)
    {
        const uint axis = VSM_L0_AXIS >> level;
        const uint px = min((uint)(uv.x * axis), axis - 1u);
        const uint py = min((uint)(uv.y * axis), axis - 1u);
        if ((PageTable[VsmPageId(view, level, px, py)] & VSM_SAMPLE_RESIDENT_BIT) != 0u) { break; }
    }
    return level;
}
uint VsmSpotSampledLevel(uint view, uint startLevel, float4x4 viewProj, float3 P, StructuredBuffer<uint> PageTable)
{
    const float4 clip = mul(float4(P, 1.0f), viewProj);
    if (clip.w <= 0.0f) { return startLevel; }
    const float2 ndc = clip.xy / clip.w;
    const float2 uv = saturate(float2(0.5f * ndc.x + 0.5f, 0.5f - 0.5f * ndc.y));
    return VsmResidentLevel(view, uv, startLevel, PageTable);
}
uint VsmPointSampledLevel(uint firstView, uint startLevel, float3 P, float3 lightPos, float nearP, float farP,
                          StructuredBuffer<uint> PageTable)
{
    uint view;
    float3 ndc;
    float2 uv;
    if (!VsmPointFaceProject(firstView, P, lightPos, nearP, farP, view, ndc, uv)) { return startLevel; }
    return VsmResidentLevel(view, uv, startLevel, PageTable);
}

// Directional clipmap, UE's GetMappedClipmap: the receiver starts at the level its distance picks with no bias (the most
// detailed one it is guaranteed a page-table entry for), and takes the first level there that has a page; the clipmap
// LEVEL is the LOD, so page-level 0. `clipVP[i]` = clipmap level i's viewProj. `clipFirstLevel` = level 0's absolute number.
// Not resident / outside every level -> lit (1.0).
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
// Constant depth bias per level: `depthBias` is NDC, and a level's NDC range is 500x its extent, so
// a constant NDC value is a constant bias in TEXELS whose WORLD size doubles per
// level -- which is what detaches thin far shadows when the base value is raised. `depthBiasDecay`
// shrinks it per level (0.5 = a constant WORLD-size bias instead) and `depthBiasFloorNdc` is a
// per-level lower bound (authored in texels on the CPU). With the D32 pool (2026-08-21) the
// quantization floor is ~2^-24 of the range -- effectively zero -- so the constant CAN sit at 0
// with the receiver-plane bias below doing all the work, which is UE's configuration.
//
// SMRT (`smrt.rayCount > 0`) replaces the single-tap SampleCmp for the level the receiver lands on
// with a ray march toward the light -- see vsm_smrt.hlsli. `dirToLight` is the unit direction
// TOWARDS the light (callers hold `sunDirWS`, the direction light TRAVELS, so they negate it).
// rayCount 0 leaves every instruction below untouched, which is what makes the A/B an A/B.
// `rayStartOffset` is where the SMRT ray begins, in world units along the light -- the result of
// the screen-space trace (vsm_screen_ray.hlsli). Computed by the CALLER because it needs the
// camera depth buffer, which is exactly how UE structure it (VirtualShadowMapProjection.usf
// computes SMRTRayOffset and hands it to TraceDirectional). 0 = no screen trace.
//
// `resident` says whether any level had a page there. The request marks pages under RECEIVERS only, so a point that is no
// receiver -- the air -- may have a page at no level at all, and its "lit" is then an unknown, not an answer.
float VsmClipmapShadowResident(float3 P, float3 N, float3 camPos, float normalBias, float depthBias,
                       float depthBiasDecay, float depthBiasFloorNdc, float clipFirstLevel,
                       float tanHalfFovX, float4x4 uvNormalMatrix, float3 dirToLight,
                       VsmSmrtParams smrt, float rayStartOffset,
                       float4x4 clipVP[VSM_NUM_CLIPMAP_LEVELS],
                       StructuredBuffer<uint> PageTable, Texture2D Pool, SamplerComparisonState cmp,
                       out bool resident)
{
    resident = false;
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

    // The level is the distance, with no bias: the request's bias marks a coarser level for the same point, and the
    // unmapped entries between point there (vsm_page_propagate_cs.hlsl), so a page is at most one more projection away.
    const int unbiasedLevel = (int)floor(log2(max(dist * 100.0f, 1e-4f)));
    uint i = (uint)max(0, unbiasedLevel - (int)clipFirstLevel);
    [loop] while (i < VSM_NUM_CLIPMAP_LEVELS)
    {
        const float4 clip = mul(float4(Poff, 1.0f), clipVP[i]);
        const float3 ndc = clip.xyz / clip.w;
        if (any(abs(ndc.xy) > 1.0f) || ndc.z < 0.0f || ndc.z > 1.0f) { ++i; continue; } // outside
        const float2 uv = float2(0.5f * ndc.x + 0.5f, 0.5f - 0.5f * ndc.y);
        const uint px = min((uint)(uv.x * (float)VSM_CLIPMAP_AXIS), VSM_CLIPMAP_AXIS - 1u);
        const uint py = min((uint)(uv.y * (float)VSM_CLIPMAP_AXIS), VSM_CLIPMAP_AXIS - 1u);
        const uint entry = PageTable[VsmPageId(VSM_CLIPMAP_FIRST + i, 0u, px, py)];
        if ((entry & VSM_SAMPLE_RESIDENT_BIT) == 0u)
        {
            i += max((entry & VSM_LOD_MASK) >> VSM_LOD_SHIFT, 1u); // the propagate's pointer, or the next level
            continue;
        }
        resident = true;
        // ---- SMRT ----------------------------------------------------------------------------
        // Marches this level instead of taking one biased comparison. Returns before the constant
        // depth bias is even computed: the march's own CompareTolerance is what replaces it, and
        // leaving the constant in would put both biases on the same sample.
        if (smrt.rayCount > 0u)
        {
            // The ray is held in WORLD space and projected per level, because it may cross into a
            // coarser clipmap level mid-march and has to be re-projected there (see
            // VsmSmrtRayState). `Poff` is the normal-offset receiver -- the offset is kept on the
            // SMRT path because it is already UE's formula and it is what stops the ray starting
            // inside the surface it is testing.
            const float rayLength = max(smrt.rayLengthScale * dist, 1e-4f);
            // One hash per pixel; each ray then walks the R2 sequence from it, which is UE's
            // arrangement (a per-pixel base plus a low-discrepancy per-ray offset) and is what
            // stops the N rays of one pixel from landing on top of each other.
            // Rotated once per frame when temporal dithering is on: the pixel keeps its own base,
            // but the whole set walks the R2 sequence so consecutive frames sample DIFFERENT points
            // of the light's disc. A still frame gets noisier; the temporal pass gets something to
            // average, which is the trade UE make with View.StateFrameIndex.
            const float2 pixelHash = frac(VsmSmrtHash2(P) + VsmSmrtR2(smrt.frameIndex));

            // UE: `DitherScale = (0.5 / CalcLevelDimsTexels(0)) * TexelDitherScale *
            // DistanceFromViewOrigin / exp2(ClipmapLevel - ResolutionLodBias)`, in centimetres: half a
            // texel of the sampled level times the scale, 1-2 x that across the distances that level holds.
            const float ditherUV = (0.5f / VSM_CLIPMAP_VIRTUAL_RES) * smrt.texelDitherScale * (dist * 100.0f) /
                                   exp2(clipFirstLevel + (float)i - smrt.clipResBias);

            // Bounded by a LITERAL -- the CB count can only end the loop early. See the caps in
            // vsm_smrt.hlsli for why a CB-driven loop bound is a device-hang generator.
            const uint rays = min(smrt.rayCount, VSM_SMRT_MAX_RAYS);
            uint rayMissCount = 0u;
            uint r = 0u;
            [loop] for (; r < VSM_SMRT_MAX_RAYS; ++r)
            {
                if (r >= rays) { break; }

                const float2 e0 = frac(pixelHash + VsmSmrtR2(r));
                const float2 e1 = frac(pixelHash + VsmSmrtR2(r + rays));
                const float3 rayDir = VsmSmrtRayDir(dirToLight, e0, smrt.sourceRadius);

                VsmSmrtRayState st;
                // UE: `RayStart = RayOrigin + RayDir * RayStartOffset`. Per RAY, because the
                // direction differs per ray, and the offset is along that direction.
                st.originWS = Poff + rayDir * rayStartOffset;
                st.rayLength = rayLength;
                st.vecWS = rayDir * rayLength;
                st.extrapolateMaxSlopeWS = smrt.extrapolateMaxSlope;
                st.level = i;
                st.startUVZ = float3(0.0f, 0.0f, 0.0f);
                st.stepUVZ = float3(0.0f, 0.0f, 0.0f);
                st.extrapolateSlope = 0.0f;
                // Built per ray, not once: each ray has its own direction, so its projection into
                // the level differs.
                VsmSmrtSetLevel(st, i, clipVP);

                // Texel dither on the ray's start, with the receiver-plane bias that offset earns
                // -- UE apply exactly this pair (SMRTClipmapRayInitialize), and applying the offset
                // without the bias is what turns a dither into acne.
                const float2 texelOffset = (e1 - 0.5f) * ditherUV;
                st.startUVZ.xy += texelOffset;
                // SUBTRACTED -- the FOURTH direct-Z flip, and the one that cost a measurement.
                // UE write `RayStartUVZ.z += OptimalBias` because larger depth is NEARER the light
                // for them. Adding it here pushes the ray's start AWAY from the light, i.e. BELOW
                // the surface it stands on, so every ray immediately finds that surface as its own
                // occluder. Measured with the sign wrong: shadow coverage 54.96% -> 73.70%, a scene
                // drowning in self-shadow that looks superficially like "softer shadows".
                // UE subtract the part of the bias the ray start has ALREADY covered:
                // `OptimalBias = max(0, OptimalBias - abs(RayStartOffset * ShadowViewToClip._33))`.
                // Our NDC-per-world along the light is the ray step's own z over its length.
                const float ndcPerWorld = abs(st.stepUVZ.z) / max(st.rayLength, 1e-6f);
                const float covered = abs(rayStartOffset) * ndcPerWorld;
                const float slopeBias = max(0.0f,
                    min(2.0f * max(0.0f, -dot(depthSlopeUV, texelOffset)), VSM_MAX_SLOPE_BIAS_NDC)
                        - covered);
                st.startUVZ.z -= slopeBias;

                const VsmSmrtResult hit = VsmSmrtRayCast(st, (int)smrt.samplesPerRay, 0.0f,
                                                         clipVP, PageTable, Pool);
                if (!hit.validHit) { ++rayMissCount; }

                // ADAPTIVE RAY COUNT, transcribed from UE (VirtualShadowMapProjectionDirectional):
                // stop early where the whole wave agrees, which is most of the screen -- fully lit
                // and fully umbral regions need one or two rays, only penumbrae need seven.
                //
                // Guarded by VSM_SMRT_COMPUTE for the same reason UE guard it with #if COMPUTESHADER:
                // WaveActiveAllTrue over a pixel shader's helper lanes does not mean what it means
                // here. Glass therefore always shoots the full count.
#if defined(VSM_SMRT_COMPUTE)
                if (smrt.adaptiveRayCount > 0u)
                {
                    if (r == 0u)
                    {
                        if (WaveActiveAllTrue(!hit.validHit)) { break; }   // every lane missed
                    }
                    else if (r >= smrt.adaptiveRayCount)
                    {
                        if (WaveActiveAllTrue(rayMissCount == 0u)) { break; } // every lane in umbra
                    }
                }
#endif
            }
            // UE divide by the rays ACTUALLY SHOT, not by the maximum: `RayCount = min(i+1, Max)`.
            // Dividing by the maximum after an early break would report a fully occluded pixel as
            // partially lit -- the adaptive path would darken nothing and lighten everything.
            const uint raysShot = min(r + 1u, rays);
            // UE's ShadowFactor: the fraction of rays that reached the light. THIS is where a
            // penumbra comes from -- a partially occluded light disc gives a partial count.
            return (float)rayMissCount / (float)max(raysShot, 1u);
        }
        // ---- end SMRT ------------------------------------------------------------------------

        // Bias of the level we LANDED on (matches how the receiver-plane bias scales: a residency
        // fallback to a coarser level uses that level's own values).
        const float levelDepthBias = max(depthBias * pow(depthBiasDecay, (float)i), depthBiasFloorNdc);
        return VsmSampleNDC(VSM_CLIPMAP_FIRST + i, ndc, uv, 0.0f, 1.0f, levelDepthBias, depthSlopeUV, PageTable, Pool, cmp);
    }
    return 1.0f; // outside all clipmap levels
}

float VsmClipmapShadow(float3 P, float3 N, float3 camPos, float normalBias, float depthBias,
                       float depthBiasDecay, float depthBiasFloorNdc, float clipFirstLevel,
                       float tanHalfFovX, float4x4 uvNormalMatrix, float3 dirToLight,
                       VsmSmrtParams smrt, float rayStartOffset,
                       float4x4 clipVP[VSM_NUM_CLIPMAP_LEVELS],
                       StructuredBuffer<uint> PageTable, Texture2D Pool, SamplerComparisonState cmp)
{
    bool resident;
    return VsmClipmapShadowResident(P, N, camPos, normalBias, depthBias, depthBiasDecay, depthBiasFloorNdc, clipFirstLevel,
                                    tanHalfFovX, uvNormalMatrix, dirToLight, smrt, rayStartOffset, clipVP, PageTable, Pool, cmp,
                                    resident);
}

#endif // VSM_SAMPLE_HLSLI
