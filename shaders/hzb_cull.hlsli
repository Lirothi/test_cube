// Occlusion plan S2 (docs/occlusion_culling_plan.md): box -> screen rect -> HZB level -> visible.
// Transcription of ue_strip/Shaders/Private/Nanite/NaniteHZBCull.ush: MipLevelForRect (:40),
// GetScreenRect (:71), GetMinDepthFromHZB (:135, the SampleLevel fallback path), IsVisibleHZB
// (:195), BoxCullFrustumPerspective (:444). Names keep UE's so a diff against the source reads.
//
// Conventions of THIS engine that the transcription is bound to:
//   * row vectors: mul(p, M), matrices as the CPU's Math::mat4 (the compiler runs with -Zpr);
//   * reverse-Z: near = 1, far = 0 (PerspectiveFovLHReverseZ), so "nearer" == "larger";
//   * the FURTHEST chain of hzb_build_cs.hlsl: min device-Z per texel, mip 0 = HALF the view rect
//     rounded UP, coarser mips floor-halved with the odd tail FOLDED into the last texel.
//
// Two deltas from UE, both because our pyramid is not a power of two:
//   1. texels are read by integer Load at the level -- there is no texel size to derive from a
//      float exponent, and nothing to go wrong when the size is 617;
//   2. level coordinates are clamped to (max(1, size >> level) - 1): a mip-0 texel past the
//      floor-halved edge lives in the LAST texel of the coarser level, where the build folded it.
// Not transcribed: RoundUpF16 (BuildInstanceDrawCommands.usf:200). It compensates the f16
// quantisation of UE's HZB; ours is R32_FLOAT, the stored minimum is exact.
//
// The CPU mirror is sources/rendering/visibility/HzbCull.h -- the same functions on the same
// floats -- and `--hzb-cull-selftest` holds the two equal (hzb_cull_selftest_cs.hlsl).

#ifndef HZB_CULL_HLSLI
#define HZB_CULL_HLSLI

// UE's INFINITE_FLOAT. A finite sentinel, so min/max never meet an actual infinity.
static const float kHzbInfiniteFloat = 3.402823466e+38f;

struct HzbFrustumCull
{
    float3 rectMin;             // xy: NDC in [-1, 1] (y up); z: device depth (reverse-Z)
    float3 rectMax;
    bool   crossesNearPlane;    // some corner is at or before the near plane -> caller treats as VISIBLE
    bool   crossesFarPlane;
    bool   frustumSideCulled;
    bool   isVisible;           // survives the near/far/side frustum tests
};

struct HzbScreenRect
{
    int4  pixels;               // inclusive [xy, zw], full-resolution pixels of the view rect
    bool  overlapsPixelCenter;  // false: the rect falls between pixel centres and rasterises nothing
    int4  hzbTexels;            // inclusive, in the texels of `hzbLevel`
    int   hzbLevel;
    float depth;                // the box's NEAREST device depth (rectMax.z)
};

float HzbMin3(float a, float b, float c) { return min(min(a, b), c); }
float2 HzbMin3(float2 a, float2 b, float2 c) { return min(min(a, b), c); }
float4 HzbMin3(float4 a, float4 b, float4 c) { return min(min(a, b), c); }
float HzbMax3(float a, float b, float c) { return max(max(a, b), c); }
float2 HzbMax3(float2 a, float2 b, float2 c) { return max(max(a, b), c); }

// Rect is inclusive [xy, zw]. The lowest level whose 4x4 footprint (desiredFootprintPixels = 4)
// covers the rect: two texels of level k cover 2^(k+1) texels of level 0, and the alignment of the
// rect to the level's grid may cost one more level (UE's comment block explains the arithmetic).
int HzbMipLevelForRect(int4 rectPixels, int desiredFootprintPixels)
{
    const int maxPixelOffset = desiredFootprintPixels - 1;
    const int mipOffset = (int)log2((float)desiredFootprintPixels) - 1;
    // firstbithigh(0) = -1, so the max with 0 also handles a one-texel rect.
    const int2 mipLevelXY = firstbithigh(rectPixels.zw - rectPixels.xy);
    int mipLevel = max(max(mipLevelXY.x, mipLevelXY.y) - mipOffset, 0);
    mipLevel += any((rectPixels.zw >> mipLevel) - (rectPixels.xy >> mipLevel) > maxPixelOffset) ? 1 : 0;
    return mipLevel;
}

// viewRect = (x0, y0, x1, y1) exclusive on the far side, e.g. (0, 0, width, height).
HzbScreenRect HzbGetScreenRect(int4 viewRect, float3 cullRectMin, float3 cullRectMax, int desiredFootprintPixels)
{
    HzbScreenRect rect;
    rect.depth = cullRectMax.z;

    // NDC [-1,1] -> texture UV [0,1]: x straight, y flipped. After the flip the min corner's v is
    // the LARGER one, so .xwzy re-sorts to (uMin, vMin, uMax, vMax).
    const float4 rectUV = saturate(float4(cullRectMin.xy, cullRectMax.xy) * float2(0.5f, -0.5f).xyxy + 0.5f).xwzy;

    // Pixel footprint at full resolution: a pixel belongs to the rect only when its CENTRE does --
    // that is the only pixel anything inside the rect can rasterise into (one centred sample).
    const float2 viewSize = viewRect.zw - viewRect.xy;
    rect.pixels = int4(rectUV * viewSize.xyxy + viewRect.xyxy + float4(0.5f, 0.5f, -0.5f, -0.5f));
    rect.pixels.xy = max(rect.pixels.xy, viewRect.xy);
    rect.pixels.zw = min(rect.pixels.zw, viewRect.zw - 1);

    // Otherwise the rect has zero area or sits between pixel centres.
    rect.overlapsPixelCenter = all(rect.pixels.zw >= rect.pixels.xy);

    // Keep the texel rect valid even when !overlapsPixelCenter.
    rect.hzbTexels = int4(rect.pixels.xy, max(rect.pixels.xy, rect.pixels.zw));

    // Mip 0 is half resolution: texel (x, y) holds pixels (2x..2x+1, 2y..2y+1) -- and, at an odd
    // edge, the folded leftover column/row (hzb_build_cs.hlsl), which x >> 1 lands on as well.
    rect.hzbTexels = rect.hzbTexels >> 1;

    rect.hzbLevel = HzbMipLevelForRect(rect.hzbTexels, desiredFootprintPixels);

    // Mip-0 texel coordinates -> the selected level's.
    rect.hzbTexels >>= rect.hzbLevel;
    return rect;
}

// One texel of `level`. Delta 2 lives here: a coordinate past the floor-halved edge is clamped to
// the last texel, which is where the build folded that column/row.
float HzbLoad(Texture2D<float> hzb, uint2 hzbMip0Size, int2 texel, int level)
{
    const int2 levelSize = max(int2(1, 1), int2(hzbMip0Size) >> level);
    const int2 c = clamp(texel, int2(0, 0), levelSize - int2(1, 1));
    return hzb.Load(int3(c, level));
}

// Minimum (= furthest, reverse-Z) depth over the rect's 4x4 footprint at its level. Offsets past
// the rect's far texel repeat that texel -- neutral for min. (UE's gather path masks them with
// 1.0 instead, its SampleLevel fallback repeats exactly like this; the result is the same.)
float HzbGetMinDepth(Texture2D<float> hzb, uint2 hzbMip0Size, HzbScreenRect rect)
{
    const int4 xs = min(rect.hzbTexels.x + int4(0, 1, 2, 3), rect.hzbTexels.z);
    const int4 ys = min(rect.hzbTexels.y + int4(0, 1, 2, 3), rect.hzbTexels.w);
    float minDepth = 1.0f;
    [unroll]
    for (int j = 0; j < 4; ++j)
    {
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            minDepth = min(minDepth, HzbLoad(hzb, hzbMip0Size, int2(xs[i], ys[j]), rect.hzbLevel));
        }
    }
    return minDepth;
}

// Reverse-Z: the box is visible when its nearest depth is at or in front of the furthest depth
// any texel of its footprint holds.
bool HzbIsVisible(Texture2D<float> hzb, uint2 hzbMip0Size, HzbScreenRect rect)
{
    return rect.depth >= HzbGetMinDepth(hzb, hzbMip0Size, rect);
}

// Perspective box cull: 8 corners built incrementally from one transformed corner and the three
// transformed edge vectors; MinW/MaxW give the near/far verdicts analytically from the projection
// (clip.z = view.z * P[2][2] + P[3][2] with w = view.z); the xy rect is the NDC bound of the corners.
// A box touching or crossing the near plane gets the WHOLE screen as its rect: its projection is
// unbounded, and every caller treats crossesNearPlane as "visible, skip the HZB".
HzbFrustumCull HzbBoxCullFrustumPerspective(float3 center, float3 extent, float4x4 localToWorld,
                                            float4x4 worldToClip, float4x4 viewToClip, bool skipFrustumCull)
{
    HzbFrustumCull cull;

    const float4 dx = (2.0f * extent.x) * mul(localToWorld[0], worldToClip);
    const float4 dy = (2.0f * extent.y) * mul(localToWorld[1], worldToClip);
    const float4 dz = (2.0f * extent.z) * mul(localToWorld[2], worldToClip);

    float  minW = +kHzbInfiniteFloat;
    float  maxW = -kHzbInfiniteFloat;
    float4 planesMin = 1.0f;

    cull.rectMin = float3(+1, +1, +1);
    cull.rectMax = float3(-1, -1, -1);

    // UE evaluates two corners per isolated pass to bound VGPRs; the arithmetic is the same.
#define HZB_EVAL_POINTS(PC0, PC1) \
    minW = HzbMin3(minW, PC0.w, PC1.w); \
    maxW = HzbMax3(maxW, PC0.w, PC1.w); \
    planesMin = HzbMin3(planesMin, float4(PC0.xy, -PC0.xy) - PC0.w, float4(PC1.xy, -PC1.xy) - PC1.w); \
    { \
        const float2 ps0 = PC0.xy / PC0.w; \
        const float2 ps1 = PC1.xy / PC1.w; \
        cull.rectMin.xy = HzbMin3(cull.rectMin.xy, ps0, ps1); \
        cull.rectMax.xy = HzbMax3(cull.rectMax.xy, ps0, ps1); \
    }

    const float4 pc000 = mul(mul(float4(center - extent, 1.0f), localToWorld), worldToClip);
    const float4 pc100 = pc000 + dz;
    HZB_EVAL_POINTS(pc000, pc100);
    const float4 pc001 = pc000 + dx;
    const float4 pc101 = pc100 + dx;
    HZB_EVAL_POINTS(pc001, pc101);
    const float4 pc011 = pc001 + dy;
    const float4 pc111 = pc101 + dy;
    HZB_EVAL_POINTS(pc011, pc111);
    const float4 pc010 = pc011 - dx;
    const float4 pc110 = pc111 - dx;
    HZB_EVAL_POINTS(pc010, pc110);
#undef HZB_EVAL_POINTS

    const float minZ = maxW * viewToClip[2][2] + viewToClip[3][2];
    const float maxZ = minW * viewToClip[2][2] + viewToClip[3][2];

    // Near is z = 1 (clip z == w there), far is z = 0.
    const bool inFrontNearPlane = minW <= maxZ;
    const bool behindNearPlane  = maxW >  minZ;
    const bool inFrontFarPlane  = 0.0f <  maxZ;
    const bool behindFarPlane   = 0.0f >= minZ;

    cull.crossesNearPlane = inFrontNearPlane;
    cull.crossesFarPlane  = behindFarPlane;
    cull.isVisible        = behindNearPlane && inFrontFarPlane;

    if (minW <= 0.0f && maxW > 0.0f)
    {
        cull.rectMin = float3(-1, -1, -1);
        cull.rectMax = float3(+1, +1, +1);
    }
    else
    {
        cull.rectMin.z = minZ / maxW;
        cull.rectMax.z = maxZ / minW;
    }

    cull.frustumSideCulled = false;
    if (!skipFrustumCull)
    {
        const bool frustumCull = any(planesMin > 0.0f);
        cull.frustumSideCulled = cull.isVisible && frustumCull;
        cull.isVisible = cull.isVisible && !frustumCull;
    }
    return cull;
}

#endif // HZB_CULL_HLSLI
