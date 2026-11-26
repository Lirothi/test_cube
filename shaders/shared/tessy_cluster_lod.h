// TessyClusterLod's fields ONCE (architecture plan R5.3): HLSL's `struct TessyClusterLod` (tessy_common.hlsli) and
// C++'s TessyClusterLod (rendering/shadows/ShadowGpuData.h) spell TESSY_CLUSTER_LOD_FIELDS, each inside its own struct.
// TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a // inside a continued line would
// swallow the next field). Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_TESSY_CLUSTER_LOD_H
#define TC_SHADERS_SHARED_TESSY_CLUSTER_LOD_H

#define TESSY_CLUSTER_LOD_FIELDS \
    float4 lodSphere;                                                                                                 \
    float4 parentSphere;                                                                                              \
    float lodError;                                                                                                   \
    float parentError;                                                                                                \
    uint level;                                                                                                       \
    uint pad;

#endif
