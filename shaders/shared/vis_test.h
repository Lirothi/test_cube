// VisTestCB's fields ONCE (architecture plan R5.3): HLSL's `cbuffer VisTestCB` (vis_test_cs.hlsl) and C++'s
// HzbTestConstants (rendering/visibility/HzbOcclusionTester.h) spell VIS_TEST_FIELDS. A field list, not a struct: HLSL
// reads a cbuffer as globals; TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a //
// inside a continued line would swallow the next field). Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_VIS_TEST_H
#define TC_SHADERS_SHARED_VIS_TEST_H

#define VIS_TEST_FIELDS \
    TC_ROW_MAJOR float4x4 worldToClip; /* the camera's JITTERED view-projection: the HZB is the jittered depth */     \
    TC_ROW_MAJOR float4x4 viewToClip; /* its projection, for the analytic near/far verdicts */                        \
    int4  viewRect; /* (0, 0, renderWidth, renderHeight) */                                                           \
    uint2 hzbSize; /* mip 0 of the pyramid */                                                                         \
    uint  boxCount;                                                                                                   \
    uint  pad0;

#endif
