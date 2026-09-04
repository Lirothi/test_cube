// Occlusion plan S5b: the POST pass of the cascades' light-space occlusion cull -- Nanite's
// CULLING_PASS_OCCLUSION_POST (NaniteCullingCommon.ush:483-497) on the deferred list
// shadow_cull_cs.hlsl wrote: every caster that last frame's pyramid hid is retested against THIS
// frame's pyramid (built from the tile after pass A drew everything the main cull kept) with this
// frame's light matrices. Still hidden -> not drawn at all; visible -> appended to the pass-B args
// and visible list, drawn into the same tile by Main_CSMPost. Zero latency by construction: a
// caster that opened up this frame is drawn this frame.
//
// Thread (x, y) = (deferred index, cascade). Same bucketing as the main cull: real group ->
// virtual group (group * kMaxShadowLods + receiver LOD), the arg row is the cascade's.
//
// b0: CullParams (numCasters = the deferred list's per-cascade stride, numGroups = virtual groups)
// b1: CascadeHzbCB       t0: Bounds  t1: CasterGroup  t2: PerGroup (virtual)  t3: CasterLod
// t4..t7: the cascade pyramids   u0: pass-B args   u1: pass-B visible list   u2: DeferredList
// u3: DeferredCount ([v] read: how many were deferred; [4 + v] written: how many pass B draws)
#include "hzb_cull.hlsli"

#define SHADOW_CULL_POST_RS \
    "CBV(b0), CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors=8, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer CullParams : register(b0)
{
    uint gNumCasters;
    uint gNumViews;      // = kHzbCascades
    uint gNumGroups;
    uint gPad;
};

static const uint kHzbCascades = 4u;
cbuffer CascadeHzbCB : register(b1)
{
    row_major float4x4 gHzbPrevViewProj[4];
    row_major float4x4 gHzbViewProj[4];
    uint4 gHzbPrevValid;
    int4  gHzbViewRect;
    uint2 gHzbSize;
    uint  gHzbOn;
    uint  gHzbPad;
};

struct CasterBounds
{
    float4 center;
    float4 halfExtents;
};

StructuredBuffer<CasterBounds> Bounds      : register(t0);
StructuredBuffer<uint>         CasterGroup : register(t1);
StructuredBuffer<uint4>        PerGroup    : register(t2);
StructuredBuffer<uint>         CasterLod   : register(t3);
Texture2D<float>               CsmHzb[4]   : register(t4);

static const uint kMaxShadowLods = 4u;
static const uint kCasterLodMask = 0x0Fu;
static const uint kArgStride = 20u;

RWByteAddressBuffer      ArgsB         : register(u0);
RWStructuredBuffer<uint> VisibleListB  : register(u1);
RWStructuredBuffer<uint> DeferredList  : register(u2);
RWStructuredBuffer<uint> DeferredCount : register(u3);

static const float4x4 kIdentity = float4x4(1, 0, 0, 0,
                                           0, 1, 0, 0,
                                           0, 0, 1, 0,
                                           0, 0, 0, 1);

[numthreads(8, 8, 1)]
[RootSignature(SHADOW_CULL_POST_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint v = dtid.y;
    if (v >= kHzbCascades || v >= gNumViews) { return; }
    const uint i = dtid.x;
    if (i >= DeferredCount[v]) { return; }
    const uint caster = DeferredList[v * gNumCasters + i];
    if (caster >= gNumCasters) { return; }

    const CasterBounds b = Bounds[caster];
    const float3 c = b.center.xyz;
    const float3 e = b.halfExtents.xyz;

    // Retest what was occluded last frame against the current frame's pyramid: the frustum
    // (the S14 volume) passed this caster in the main cull; here only the far cap and the depth
    // test decide, and a caster crossing the near plane is visible (pancaked).
    const HzbFrustumCull cur = HzbBoxCullFrustumOrtho(c, e, kIdentity, gHzbViewProj[v], false, false);
    if (!cur.isVisible) { return; }
    if (!cur.crossesNearPlane)
    {
        const HzbScreenRect rect = HzbGetScreenRect(gHzbViewRect, cur.rectMin, cur.rectMax, 4);
        if (!HzbIsVisible(CsmHzb[v], gHzbSize, rect)) { return; }
    }

    uint lod = CasterLod[caster] & kCasterLodMask;
    if (lod >= kMaxShadowLods) { lod = kMaxShadowLods - 1u; }
    const uint g = CasterGroup[caster] * kMaxShadowLods + lod;
    const uint base = PerGroup[g].x;

    uint slot;
    ArgsB.InterlockedAdd((v * gNumGroups + g) * kArgStride + 4u, 1u, slot);
    VisibleListB[v * gNumCasters + base + slot] = caster;
    uint drawn;
    InterlockedAdd(DeferredCount[kHzbCascades + v], 1u, drawn);
}
