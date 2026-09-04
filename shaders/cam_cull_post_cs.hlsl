// Occlusion plan S5: the POST pass of the camera's two-pass HZB occlusion (Nanite's
// CULLING_PASS_OCCLUSION_POST, NaniteCullingCommon.ush:483-497) on the deferred list the main
// cull wrote (shadow_cull_cs.hlsl, camera branch): every indirect G-buffer candidate that LAST
// frame's pyramid hid is retested against THIS frame's pyramid -- built from the depth after
// pass A drew everything the main cull kept -- with this frame's (jittered) matrices. Still
// hidden -> not drawn at all this frame; visible -> the pass-B args + visible list, drawn into the
// same G-buffer by Main_GBufferB. Zero latency by construction: whatever is visible this frame is
// drawn this frame, the bad guesses of pass 1 just arrive one pass later.
//
// b0: CullParams (numCasters, numGroups)   b1: CameraHzbCB
// t0: Bounds  t1: CasterGroup  t2: PerGroupCam (the camera's bases)  t3: CasterLod  t4: CasterFade
// t5: the current pyramid (after pass A)
// u0: pass-B args   u1: pass-B visible list   u2: CamDeferred   u3: DeferredCount
//     ([8] read: the deferred list's length; [10] written: pass-B entries, a fading caster twice)
#include "hzb_cull.hlsli"

#define CAM_CULL_POST_RS \
    "CBV(b0), CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors=6, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer CullParams : register(b0)
{
    uint gNumCasters;
    uint gNumViews;
    uint gNumGroups;
    uint gCamView;
};

// Mirrors ShadowGpuData::CameraHzbParams.
cbuffer CameraHzbCB : register(b1)
{
    row_major float4x4 gCamPrevViewProj;
    row_major float4x4 gCamPrevViewToClip;
    row_major float4x4 gCamViewProj;
    row_major float4x4 gCamViewToClip;
    int4  gCamViewRect;
    uint2 gCamHzbSize;
    uint  gCamHzbPrevValid;
    uint  gCamHzbOn;
};

struct CasterBounds
{
    float4 center;
    float4 halfExtents;
};

StructuredBuffer<CasterBounds> Bounds      : register(t0);
StructuredBuffer<uint>         CasterGroup : register(t1);
StructuredBuffer<uint4>        PerGroupCam : register(t2);
StructuredBuffer<uint>         CasterLod   : register(t3);
StructuredBuffer<float>        CasterFade  : register(t4);
Texture2D<float>               CamHzb      : register(t5);

static const uint kMaxShadowLods = 4u;
static const uint kCasterLodMask = 0x0Fu;
static const uint kArgStride = 20u;
static const uint kCamDeferredCount = 8u;   // DeferredCount[] slot: the list's length
static const uint kCamDrawnB = 10u;         // DeferredCount[] slot: pass-B entries

RWByteAddressBuffer      CamArgsB        : register(u0);
RWStructuredBuffer<uint> CamVisibleListB : register(u1);
RWStructuredBuffer<uint> CamDeferred     : register(u2);
RWStructuredBuffer<uint> DeferredCount   : register(u3);

static const float4x4 kIdentity = float4x4(1, 0, 0, 0,
                                           0, 1, 0, 0,
                                           0, 0, 1, 0,
                                           0, 0, 0, 1);

void EmitB(uint vg, uint caster)
{
    const uint base = PerGroupCam[vg].x;
    uint slot;
    CamArgsB.InterlockedAdd(vg * kArgStride + 4u, 1u, slot);
    CamVisibleListB[base + slot] = caster;
}

[numthreads(8, 8, 1)]
[RootSignature(CAM_CULL_POST_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.y != 0u) { return; }
    const uint i = dtid.x;
    if (i >= DeferredCount[kCamDeferredCount]) { return; }
    const uint caster = CamDeferred[i];
    if (caster >= gNumCasters) { return; }

    const CasterBounds b = Bounds[caster];
    const float3 c = b.center.xyz;
    const float3 e = b.halfExtents.xyz;

    // Retest against the current frame's pyramid: the frustum was passed in the main cull; a box
    // crossing the near plane is visible without asking.
    const HzbFrustumCull cur = HzbBoxCullFrustumPerspective(c, e, kIdentity, gCamViewProj, gCamViewToClip, false);
    if (!cur.isVisible) { return; }
    if (!cur.crossesNearPlane)
    {
        const HzbScreenRect rect = HzbGetScreenRect(gCamViewRect, cur.rectMin, cur.rectMax, 4);
        if (!HzbIsVisible(CamHzb, gCamHzbSize, rect)) { return; }
    }

    uint lod = CasterLod[caster] & kCasterLodMask;
    if (lod >= kMaxShadowLods) { lod = kMaxShadowLods - 1u; }
    const uint g = CasterGroup[caster] * kMaxShadowLods + lod;
    EmitB(g, caster);
    uint weight = 1u;
    if (CasterFade[caster] > 0.0f && (lod + 1u) < kMaxShadowLods)
    {
        EmitB(g + 1u, caster);
        weight = 2u;
    }
    uint drawn;
    InterlockedAdd(DeferredCount[kCamDrawnB], weight, drawn);
}
