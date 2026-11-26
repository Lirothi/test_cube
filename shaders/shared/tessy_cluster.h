// TessyCluster's fields ONCE (architecture plan R5.3): HLSL's `struct TessyCluster` (tessy_common.hlsli) and C++'s
// TessyCluster (rendering/shadows/ShadowGpuData.h) spell TESSY_CLUSTER_FIELDS, each inside its own struct. TC_INIT is
// the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a // inside a continued line would swallow the
// next field). Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_TESSY_CLUSTER_H
#define TC_SHADERS_SHARED_TESSY_CLUSTER_H

#define TESSY_CLUSTER_FIELDS \
    uint   firstIndex; /* mega-absolute, into ShadowGpuData::MegaIndexBuffer() */                                     \
    uint   triCount; /* <= 128 */                                                                                     \
    float  windAlongMax; /* Mesh::Cluster's wind maxima (the cull's sway bound) */                                    \
    float  windLeafMax;                                                                                               \
    float3 center; /* object-space box */                                                                             \
    float  radius; /* meshopt sphere radius (N7's LOD) */                                                             \
    float3 extent;                                                                                                    \
    uint   firstMeshlet; /* N6: its meshlets in TessyMeshlets (Mesh::ClusterLocalTables, the mesh shader's) */        \
    uint   meshletCount; /* N6: 1 (a regular mesh's cluster) to ~5 (a leaf card's) */                                 \
    uint3  pad;

#endif
