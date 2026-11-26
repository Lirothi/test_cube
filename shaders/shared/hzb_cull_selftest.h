// SelfTestCB's fields ONCE (architecture plan R5.3): HLSL's `cbuffer SelfTestCB` (hzb_cull_selftest_cs.hlsl) and C++'s
// SelfTestConstants (rendering/visibility/HzbCullSelfTest.cpp) spell HZB_SELFTEST_FIELDS. A field list, not a struct:
// HLSL reads a cbuffer as globals; TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a
// // inside a continued line would swallow the next field). Include shared/shader_types.h first. New fields are
// APPENDED.
#ifndef TC_SHADERS_SHARED_HZB_CULL_SELFTEST_H
#define TC_SHADERS_SHARED_HZB_CULL_SELFTEST_H

#define HZB_SELFTEST_FIELDS \
    TC_ROW_MAJOR float4x4 localToWorld;                                                                               \
    TC_ROW_MAJOR float4x4 worldToClip;                                                                                \
    TC_ROW_MAJOR float4x4 viewToClip;                                                                                 \
    int4  viewRect; /* (0, 0, width, height) */                                                                       \
    uint2 hzbSize; /* mip 0 of the pyramid */                                                                         \
    uint  boxCount;                                                                                                   \
    uint  footprint; /* 4 */                                                                                          \
    /* S5b: the ortho cases' projection (reverse-Z); C++: S5b: the ortho cases' projection (reverse-Z), own */        \
    /* pyramid in t2 */                                                                                               \
    TC_ROW_MAJOR float4x4 orthoWorldToClip;

#endif
