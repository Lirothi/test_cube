// Tessy N5 (docs/tessy_plan.md): the POST pass of the camera clusters' two-pass HZB occlusion (UE Nanite's
// CULLING_PASS_OCCLUSION_POST, NaniteClusterCulling.usf:941-962) -- cam_cull_post_cs.hlsl's twin for CLUSTERS. The camera
// row's cluster cull (tessy_atlas_cull_cs.hlsl, kFlagByRun) deferred every cluster LAST frame's pyramid hid; here each is
// retested against THIS frame's pyramid (built by Main_HzbA from pass A's depth) with this frame's matrices, and the
// visible ones' meshlets go to their PSO run's pass-B row of the camera row set, drawn into the same G-buffer by
// Main_GBufferB with the runs' mesh shader twins. Whatever is visible this frame is drawn this frame.
//
// One group per run (the dispatch has gNumRuns), two sweeps over the deferred list: count the run's visible meshlets,
// claim them in the camera row set (its cursor continues after pass A's), write them. A run that does not fit draws
// nothing of pass B -- a cluster lost for a frame, counted as a view that did not fit.
#pragma pack_matrix(row_major)
#include "wind.hlsli"
#include "tessy_common.hlsli"
#include "tessy_cull_common.hlsli"
#include "hzb_cull.hlsli"

#define TESSY_CAMERA_POST_RS \
    "CBV(b0), CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors=5, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer CameraPostCB : register(b0)
{
    uint gNumRuns;      // the groups
    uint gRowFirst;     // pass B's row of run 0 in TessyAtlasViewInfo / the args (row set 2, after pass A's runs)
    uint gRecordBase;   // the camera row set's part of TessyAtlasVisible
    uint gRecordCap;
    float4 gWind0;      // the frame's wind (the clusters' sway pad, as pass A padded them)
    float4 gWind1;
    uint gWindPad;
    uint gDeferCap;     // TessyCamDeferred's capacity
    uint2 gPad;
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

StructuredBuffer<InstancePerObject> Instances        : register(t0);
StructuredBuffer<TessyCluster>      Clusters         : register(t1);
StructuredBuffer<uint>              VgRun            : register(t2); // per virtual group: its PSO run (~0: classic)
Texture2D<float>                    CamHzb           : register(t3); // this frame's pyramid (after pass A)
StructuredBuffer<uint4>             TessyCamDeferred : register(t4); // {caster id, cluster, camId, base vertex} or dead

RWStructuredBuffer<uint4> TessyVisible       : register(u0);
RWStructuredBuffer<uint2> TessyAtlasViewInfo : register(u1);
RWByteAddressBuffer       TessyAtlasArgs     : register(u2);
RWByteAddressBuffer       TessyAtlasCounter  : register(u3); // [8] the camera row set's claims, [12] did not fit, [20]
                                                              // the deferred list's length (pass A's claims)

static const uint kThreads = 1024u;
static const float4x4 kIdentity = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);

groupshared uint gsRecords;
groupshared uint gsRunBase;
groupshared uint gsClaimed;
groupshared bool gsFits;

// Entry `e`, if it is this run's and this frame's pyramid shows it: its cluster (and the meshlet count), else none.
bool VisibleNow(uint4 e, uint run, out TessyCluster c)
{
    c = Clusters[e.x == kTessyDeadRecord ? 0u : e.y];
    if (e.x == kTessyDeadRecord || VgRun[e.z & kTessyCamVgMask] != run) { return false; }
    const InstancePerObject ip = Instances[e.x];
    float3 ctr = mul(float4(c.center, 1.0f), ip.world).xyz;
    float3 ext = abs(ip.world[0].xyz) * c.extent.x + abs(ip.world[1].xyz) * c.extent.y + abs(ip.world[2].xyz) * c.extent.z;
    if (gWindPad != 0u) { WindPadBox(ip, c, gWind0, gWind1, gWindPad, ctr, ext); }
    // cam_cull_post_cs.hlsl's test: the frustum was passed in pass A; a box crossing the near plane is visible.
    const HzbFrustumCull cur = HzbBoxCullFrustumPerspective(ctr, ext, kIdentity, gCamViewProj, gCamViewToClip, false);
    if (!cur.isVisible) { return false; }
    if (cur.crossesNearPlane) { return true; }
    const HzbScreenRect rect = HzbGetScreenRect(gCamViewRect, cur.rectMin, cur.rectMax, 4);
    return HzbIsVisible(CamHzb, gCamHzbSize, rect);
}

[numthreads(1024, 1, 1)]
[RootSignature(TESSY_CAMERA_POST_RS)]
void CSMain(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint run = gid.x;
    if (run >= gNumRuns) { return; } // uniform
    const uint entries = min(TessyAtlasCounter.Load(20u), gDeferCap);
    if (gi == 0u) { gsRecords = 0u; }
    GroupMemoryBarrierWithGroupSync();
    for (uint i = gi; i < entries; i += kThreads)
    {
        TessyCluster c;
        if (VisibleNow(TessyCamDeferred[i], run, c)) { InterlockedAdd(gsRecords, c.meshletCount); }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u)
    {
        const uint records = gsRecords;
        uint base = 0u;
        if (records != 0u) { TessyAtlasCounter.InterlockedAdd(8u, records, base); }
        gsFits = records == 0u || base + records <= gRecordCap;
        if (!gsFits) { TessyAtlasCounter.InterlockedAdd(12u, 1u); }
        gsRunBase = base;
        gsClaimed = records;
        gsRecords = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (gsFits)
    {
        for (uint i = gi; i < entries; i += kThreads)
        {
            const uint4 e = TessyCamDeferred[i];
            TessyCluster c;
            if (!VisibleNow(e, run, c)) { continue; }
            uint at;
            InterlockedAdd(gsRecords, c.meshletCount, at);
            for (uint s = 0u; s < c.meshletCount && at + s < gsClaimed; ++s)
            {
                TessyVisible[gRecordBase + gsRunBase + at + s] = uint4(e.x, c.firstMeshlet + s, e.w, e.z);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u)
    {
        const uint fitted = gsFits ? min(gsRecords, gsClaimed) : 0u;
        const uint info = gRowFirst + run;
        TessyAtlasViewInfo[info] = uint2(gRecordBase + gsRunBase, fitted);
        TessyAtlasArgs.Store3(info * 12u, fitted == 0u ? uint3(0u, 0u, 1u)
                                                      : uint3(min(fitted, kTessyMsRow), (fitted + kTessyMsRow - 1u) / kTessyMsRow, 1u));
    }
}
