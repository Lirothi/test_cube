#pragma once

// Headless DXR smoke harness, run via "test_cube.exe --rt-smoke" instead of the
// app. Creates a D3D12 device, queries ray-tracing support, and — when RT is
// available — builds a trivial BLAS over one triangle plus a single-instance
// TLAS, executes them on a direct CommandList4, and waits on a fence. It then
// checks the device was not removed.
//
// Self-contained: it brings up its own device/queue/command-list (it does not
// touch the live Renderer), so it validates the DXR build path in isolation.
//
// Writes the verdict to `outPath` ("PASS", "SKIP", or "FAIL ...") and a step log
// to rt_smoke.log in the working directory. Return value (process exit code):
//   0 = PASS (RT HW, AS built cleanly) or SKIP (no RT support — graceful)
//   1 = FAIL (device bring-up or AS build/exec failed unexpectedly)
int RunRtSmoke(const char* outPath);
