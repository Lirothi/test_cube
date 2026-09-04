// Occlusion plan S3b (docs/occlusion_culling_plan.md): FHZBOcclusionTester's test as one compute
// dispatch. Transcription of HZBOcclusion.usf:30-41 (HZBTestPS): per box, BoxCullFrustum ->
// crossing the near plane is visible without asking -> GetScreenRect(footprint 4) -> IsVisibleHZB.
// The library is hzb_cull.hlsli (S2), the same code the self-test holds equal to its CPU mirror.
//
// Deltas from UE, both shapes rather than arithmetic:
//   * a structured buffer of boxes instead of two 256x256 float4 textures updated in 8x8 blocks
//     (SceneOcclusion.cpp:960-1010) -- the block/Morton addressing existed to feed a pixel
//     shader; a compute thread per box needs none of it;
//   * one uint per box instead of a BGRA8 pixel (IsVisible reads channel 0, :882-905).
// The boxes are WORLD-space centre/extent (the history expanded them by OCCLUSION_SLOP), so
// localToWorld is the identity -- UE's HZBTestPS calls the world-space BoxCullFrustum the same way.
//
// t0: this frame's boxes      t1: the FURTHEST HZB, all mips      u0: one uint per box, 1 = visible
// The consumer is vis::OcclusionHistory (the same history as the S3a queries), which reads the
// results `vis.queryLatency` frames later.

#include "hzb_cull.hlsli"

#define VIS_TEST_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

// Mirrors vis::HzbTestConstants. row_major spelled out for tools/check_shaders.py (no -Zpr there).
cbuffer VisTestCB : register(b0)
{
    row_major float4x4 worldToClip;   // the camera's JITTERED view-projection: the HZB is the jittered depth
    row_major float4x4 viewToClip;    // its projection, for the analytic near/far verdicts
    int4  viewRect;                   // (0, 0, renderWidth, renderHeight)
    uint2 hzbSize;                    // mip 0 of the pyramid
    uint  boxCount;
    uint  pad0;
};

struct TestBox
{
    float4 center;                    // xyz, world space
    float4 extent;                    // xyz half-extents
};

StructuredBuffer<TestBox> Boxes   : register(t0);
Texture2D<float>          Hzb     : register(t1);
RWStructuredBuffer<uint>  Results : register(u0);

static const float4x4 kIdentity = float4x4(1, 0, 0, 0,
                                           0, 1, 0, 0,
                                           0, 0, 1, 0,
                                           0, 0, 0, 1);

[numthreads(64, 1, 1)]
[RootSignature(VIS_TEST_RS)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= boxCount) { return; }

    const TestBox box = Boxes[i];
    HzbFrustumCull cull = HzbBoxCullFrustumPerspective(box.center.xyz, box.extent.xyz, kIdentity,
                                                       worldToClip, viewToClip, false);

    // HZBOcclusion.usf:36-40 -- only a box entirely past the near plane consults the pyramid.
    // No overlapsPixelCenter test: HZBTestPS has none (that rule belongs to the Nanite cull
    // shaders, and to the self-test's consumer model of them). A rect between pixel centres keeps
    // a valid one-texel footprint (HzbGetScreenRect) and takes the depth test like any other; a
    // primitive the history culled on a zero-area rect would otherwise pop when it grew to a pixel.
    [branch]
    if (cull.isVisible && !cull.crossesNearPlane)
    {
        const HzbScreenRect rect = HzbGetScreenRect(viewRect, cull.rectMin, cull.rectMax, 4);
        cull.isVisible = HzbIsVisible(Hzb, hzbSize, rect);
    }

    Results[i] = cull.isVisible ? 1u : 0u;
}
