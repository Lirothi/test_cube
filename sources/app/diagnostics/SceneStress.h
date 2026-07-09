#pragma once

// Scene-lifecycle stress driver, run via "test_cube.exe --scene-stress" (or
// "--scene-stress=<iterations>") instead of interactive use. It boots the REAL
// renderer/device/swapchain/scene/level (the same App bootstrap as a normal
// launch) and then, instead of waiting for user input, autonomously hammers the
// scene-lifetime churn operations that the intermittent launch/render crash is
// believed to live in: level reload, live level switching, window resize (which
// recreates the deferred render targets — the prime suspect for a transient
// null SRV), DLSS mode changes, render-resolution / reflection-scale changes,
// and (WITH_EDITOR only) editor object spawn/delete. Each step performs ONE
// churn op, renders a few real frames to pump the pipeline, then checks for a
// fault.
//
// Fault detection after every step:
//   * ID3D12Device::GetDeviceRemovedReason() != S_OK  -> device removed
//   * a std::runtime_error from the frame render (e.g. the FrameResource
//     "CommandList Reset failed" path)
//   * (Debug) any ID3D12InfoQueue ERROR/CORRUPTION message (the driver disables
//     break-on-severity and polls the queue itself, so a debug-layer error is
//     captured and reported rather than hard-breaking the process)
//
// Writes a running log to scene_stress.log (iteration, op, result) plus a final
// verdict line. Return value (process exit code):
//   0 = clean through all iterations
//   nonzero = a fault was caught (the log names the op + iteration + HRESULT)
//
// iterations <= 0 selects the default iteration count. When gbvContinue is true
// (diagnostics), InfoQueue ERROR/CORRUPTION messages (e.g. GPU-based-validation
// findings) are logged but NOT treated as fatal, so the run proceeds through the
// churn to the actual device-removal — letting GBV annotate the frames leading
// up to the hang. Device-removed and exceptions remain fatal.
int RunSceneStress(struct HINSTANCE__* hInstance, int nCmdShow, int iterations, bool gbvContinue);
