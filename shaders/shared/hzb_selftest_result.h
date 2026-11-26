// TestResult's fields ONCE (architecture plan R5.3): HLSL's `struct TestResult` (hzb_cull_selftest_cs.hlsl) and C++'s
// TestResult (rendering/visibility/HzbCullSelfTest.cpp) spell HZB_SELFTEST_RESULT_FIELDS, each inside its own struct.
// TC_INIT is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a // inside a continued line would
// swallow the next field). Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_HZB_SELFTEST_RESULT_H
#define TC_SHADERS_SHARED_HZB_SELFTEST_RESULT_H

#define HZB_SELFTEST_RESULT_FIELDS \
    int4  pixels;                                                                                                     \
    int4  hzbTexels;                                                                                                  \
    int   level; /* -1 when the HZB was not consulted */                                                              \
    float minDepth; /* 0 when not consulted */                                                                        \
    uint  flags; /* kFlag* below */                                                                                   \
    uint  visible;                                                                                                    \
    float depth; /* rect.depth (nearest device depth) */                                                              \
    float rectMinZ;                                                                                                   \
    float2 pad;

#endif
