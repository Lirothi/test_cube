// TestBox's fields ONCE (architecture plan R5.3): HLSL's `struct TestBox` (hzb_cull_selftest_cs.hlsl) and C++'s TestBox
// (rendering/visibility/HzbCullSelfTest.cpp) spell HZB_SELFTEST_BOX_FIELDS, each inside its own struct. TC_INIT is the
// C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a // inside a continued line would swallow the next
// field). Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_HZB_SELFTEST_BOX_H
#define TC_SHADERS_SHARED_HZB_SELFTEST_BOX_H

#define HZB_SELFTEST_BOX_FIELDS \
    float4 center; /* xyz; w = 1 for an ortho case */                                                                 \
    float4 extent; /* xyz half-extents */

#endif
