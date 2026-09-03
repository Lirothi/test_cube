#pragma once

// Occlusion plan S2: headless self-test of the box -> HZB visibility library, run via
// "test_cube.exe --hzb-cull-selftest" instead of the app (the same shape as --rt-smoke: its own
// device, no window, no renderer). Builds a synthetic pyramid on the CPU -- an occluder plane
// with a hole, reduced exactly like hzb_build_cs.hlsl, odd tails folded -- uploads it with its
// full mip chain, runs hzb_cull_selftest_cs.hlsl over a set of boxes whose verdicts are set by
// hand, then computes the same boxes through the CPU mirror (HzbCull.h) and holds every field
// equal: pixel rect, texel rect, level, footprint minimum (bit-exact), flags, verdict.
//
// Everything it says goes to the session log (category RenderValidation): one Info line per case,
// the verdict line "hzb cull self-test: PASS ..." / "FAIL ..." (Error). The return value is the
// number of failed checks (0 = PASS). A missing shader or device is a failure, not a skip: every
// machine this runs on has the pyramid this library is for.
int RunHzbCullSelfTest();
