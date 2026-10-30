// Tessy (docs/tessy_plan.md): what the cluster passes share -- the cluster record and the visible list's layout.
#ifndef TESSY_COMMON_HLSLI
#define TESSY_COMMON_HLSLI

// Matches ShadowGpuData::TessyCluster (64 bytes). One per (virtual group x cluster): the classic index range it covers
// in the mega IB (the VS path and the validator) and, since N6, its meshlets (TessyMeshlets) the mesh shader emits.
struct TessyCluster
{
    uint   firstIndex;   // mega-absolute
    uint   triCount;     // <= 128
    float  windAlongMax;
    float  windLeafMax;
    float3 center;       // object space
    float  radius;
    float3 extent;
    uint   firstMeshlet; // N6
    uint   meshletCount; // N6: 1 (a regular mesh's cluster) to ~5 (a leaf card's)
    uint3  pad;
};
// N6, matches ShadowGpuData::TessyMeshlet: {vertex list offset in TessyClusterVerts, vertices (<= kTessyMeshletMaxVerts),
// triangle offset in TessyClusterTris (three 8-bit places in that list each), triangles}.
static const uint kTessyMeshletMaxVerts = 64u; // Mesh::kMeshletMaxVerts
static const uint kTessyMeshletMaxTris  = 128u; // Mesh::kClusterMaxTris

// TessyVisible: one uint4 per surviving (caster, cluster) on a page -- {caster id | page << 22, the cluster's index
// (the VS path) or ONE of its meshlets' (the mesh shader path: the cull appends a record per meshlet), the mesh's base
// vertex in the mega VB, the mesh shader's culls (below; the cull pass copies tessy.msCull in)}. A record whose .x is
// kTessyDeadRecord draws nothing: tessy_ms_args_cs fills the mesh shader dispatch's last row past the count with it.
static const uint kTessyDeadRecord = 0xFFFFFFFFu;
// .w bit 0: cull a triangle whose screen box holds no pixel centre (it would raster no sample) -- exact, conservative
// by 1/64 px against the rasterizer's snapping. The page-border cull (all three corners outside one border) is always on.
static const uint kTessyMsCullSmall = 1u;

// The CAMERA row's records (tessy_atlas_cull_cs kFlagClusterId: the cluster view, N5's G-buffer) carry in .w their
// virtual group (bits 0-15: the G-buffer's material, its LOD = vg % 4) and the cluster's index in that group's range
// (bits 16-31) -- together an id unique per cluster.
static const uint kTessyCamVgMask = 0xFFFFu;

// The mesh shader dispatch: DispatchMesh(min(N, row), ceil(N / row), 1) -- a dimension holds at most 65535 groups.
static const uint kTessyMsRow = 65535u;

#endif // TESSY_COMMON_HLSLI
