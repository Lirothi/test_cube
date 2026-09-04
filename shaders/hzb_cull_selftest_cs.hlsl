// Occlusion plan S2: the GPU half of `--hzb-cull-selftest`. One thread per test box: frustum cull,
// screen rect, level, footprint minimum, verdict -- every intermediate written out so the CPU
// mirror (sources/rendering/visibility/HzbCull.h) can be held equal field by field, not just on the
// final bit. Nothing in the renderer runs this; it exists to be compared.
//
// t0: the test boxes (local-space centre/extent; center.w = 1 -> an ORTHO case, S5b)
// t1: the synthetic pyramid, R32_FLOAT with the full mip chain (the FURTHEST convention)
// t2: the same scene's pyramid under the ortho projection (its device depths differ)
// u0: one result record per box

#include "hzb_cull.hlsli"

#define HZB_CULL_SELFTEST_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

// Mirrors hzb::SelfTestConstants. row_major is what -Zpr gives every engine shader; spelled out
// so tools/check_shaders.py (which runs dxc without -Zpr) compiles the same layout.
cbuffer SelfTestCB : register(b0)
{
    row_major float4x4 localToWorld;
    row_major float4x4 worldToClip;
    row_major float4x4 viewToClip;
    int4  viewRect;     // (0, 0, width, height)
    uint2 hzbSize;      // mip 0 of the pyramid
    uint  boxCount;
    uint  footprint;    // 4
    row_major float4x4 orthoWorldToClip; // S5b: the ortho cases' projection (reverse-Z)
};

struct TestBox
{
    float4 center;      // xyz; w = 1 for an ortho case
    float4 extent;      // xyz half-extents
};

// Mirrors hzb::SelfTestResult (64 bytes).
struct TestResult
{
    int4  pixels;
    int4  hzbTexels;
    int   level;        // -1 when the HZB was not consulted
    float minDepth;     // 0 when not consulted
    uint  flags;        // kFlag* below
    uint  visible;
    float depth;        // rect.depth (nearest device depth)
    float rectMinZ;
    float2 pad;
};

static const uint kFlagCrossesNear   = 1u;
static const uint kFlagCrossesFar    = 2u;
static const uint kFlagSideCulled    = 4u;
static const uint kFlagFrustumVisible = 8u;
static const uint kFlagOverlapsPixel = 16u;

StructuredBuffer<TestBox>     Boxes    : register(t0);
Texture2D<float>              Hzb      : register(t1);
Texture2D<float>              HzbOrtho : register(t2);
RWStructuredBuffer<TestResult> Results : register(u0);

[numthreads(64, 1, 1)]
[RootSignature(HZB_CULL_SELFTEST_RS)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= boxCount) { return; }

    const TestBox box = Boxes[i];
    TestResult r = (TestResult)0;
    r.level = -1;

    const bool ortho = box.center.w != 0.0f;
    HzbFrustumCull cull;
    if (ortho)
    {
        cull = HzbBoxCullFrustumOrtho(box.center.xyz, box.extent.xyz, localToWorld, orthoWorldToClip, false, false);
    }
    else
    {
        cull = HzbBoxCullFrustumPerspective(box.center.xyz, box.extent.xyz, localToWorld, worldToClip, viewToClip, false);
    }
    r.flags |= cull.crossesNearPlane  ? kFlagCrossesNear    : 0u;
    r.flags |= cull.crossesFarPlane   ? kFlagCrossesFar     : 0u;
    r.flags |= cull.frustumSideCulled ? kFlagSideCulled     : 0u;
    r.flags |= cull.isVisible         ? kFlagFrustumVisible : 0u;
    r.depth = cull.rectMax.z;
    r.rectMinZ = cull.rectMin.z;

    // The consumer's rules (S3b/S5, same as every NaniteHZBCull caller): frustum-culled = out;
    // crossing the near plane = in without asking the HZB; otherwise pixel-centre overlap AND the
    // depth test.
    if (!cull.isVisible)
    {
        r.visible = 0u;
    }
    else if (cull.crossesNearPlane)
    {
        r.visible = 1u;
    }
    else
    {
        const HzbScreenRect rect = HzbGetScreenRect(viewRect, cull.rectMin, cull.rectMax, (int)footprint);
        r.pixels = rect.pixels;
        r.hzbTexels = rect.hzbTexels;
        r.level = rect.hzbLevel;
        r.flags |= rect.overlapsPixelCenter ? kFlagOverlapsPixel : 0u;
        r.minDepth = ortho ? HzbGetMinDepth(HzbOrtho, hzbSize, rect) : HzbGetMinDepth(Hzb, hzbSize, rect);
        // == HzbIsVisible on the same pyramid; spelled on the value so the texture is chosen once.
        r.visible = (rect.overlapsPixelCenter && rect.depth >= r.minDepth) ? 1u : 0u;
    }
    Results[i] = r;
}
