// CameraHzbCB's fields ONCE (architecture plan R5.3): HLSL's `cbuffer CameraHzbCB` (cam_cull_post_cs.hlsl) and C++'s
// CameraHzbParams (rendering/shadows/ShadowGpuData.h) spell CAMERA_HZB_FIELDS. A field list, not a struct: HLSL reads a
// cbuffer as globals; TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a // inside a
// continued line would swallow the next field). Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_CAMERA_HZB_H
#define TC_SHADERS_SHARED_CAMERA_HZB_H

#define CAMERA_HZB_FIELDS \
    TC_ROW_MAJOR float4x4 gCamPrevViewProj; /* last frame's camera, no jitter */                                      \
    TC_ROW_MAJOR float4x4 gCamPrevViewToClip;                                                                         \
    TC_ROW_MAJOR float4x4 gCamViewProj; /* this frame's, jittered (the pyramid is the jittered depth) */              \
    TC_ROW_MAJOR float4x4 gCamViewToClip;                                                                             \
    int4  gCamViewRect; /* (0, 0, renderWidth, renderHeight) */                                                       \
    uint2 gCamHzbSize; /* mip 0 */                                                                                    \
    uint  gCamHzbPrevValid; /* last frame's pyramid: this camera's, same size, built last frame */                    \
    uint  gCamHzbOn;

#endif
