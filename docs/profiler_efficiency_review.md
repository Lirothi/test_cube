# Profiler efficiency review

Implementation: `fcbb93e0`. Regression build integration with memory tracking: `edc6dc5a`.
The 2026-10-22 application/test builds below used that committed snapshot, before the concurrent memory-trace work.

## Findings and fixes

| Problem | Result |
|---|---|
| The shared sample freelist could race when nodes were reused. | CPU producers keep reusable per-thread buffers; frame epochs reject late samples. |
| Resetting maxima raced with the asynchronous statistics reducer. | Reset is requested atomically and applied by the reducer that owns the statistics. |
| Hidden HUD rows were formatted every frame. | Numeric snapshots retain their storage; visible HUD rows are formatted on demand, at most 10 times per second. Snapshot names remain complete UTF-8 strings. |
| Trace export shared the task that the next frame had to join; stopping a capture drained GPU work synchronously. | Completed captures go to a separate writer. Stop retires GPU batches as fences complete; only shutdown joins file I/O. |
| Several GPU readbacks arriving together were combined into one frame. | Every batch retains its original frame number and is committed separately. An exhausted eight-slot ring skips new samples instead of overwriting unread results. |
| Maximum cost measured one invocation while average cost measured the whole frame. | Both use the sum of invocations in a frame; reset starts the maximum from zero. |
| GPU scopes contended on a shared lock and resolved every nested scope separately. | Query pairs use one atomic reservation; adjacent completed pairs on the same command list are resolved together. Clock calibration runs only for trace capture. |

## Measurement

A/B/A/B microbenchmark on 2026-10-21, before and after `fcbb93e0`: eight producers,
384 CPU scopes per frame, 192 scope names, hidden HUD, no GPU. Each run warms up for
500 frames and measures the next 2,000, including producer dispatch and frame reduction.

| Run | Before, us/frame | After, us/frame |
|---|---:|---:|
| 1 | 136.44 | 16.25 |
| 2 | 136.79 | 16.62 |

The measured overhead fell about 8.2 times in this workload. The reducer's EMA fell
from about 122 us to 5.6-5.8 us. These are isolated profiler measurements, not game FPS
gains; they predate the subsequent memory-tracking work.

## Regression gate

From a Visual Studio developer shell at the repository root:

```powershell
msbuild tools/profiler_regression.vcxproj /p:Configuration=Debug /p:Platform=x64
./x64/ProfilerRegression/Debug/profiler_regression.exe
msbuild tools/profiler_regression.vcxproj /p:Configuration=Release /p:Platform=x64
./x64/ProfilerRegression/Release/profiler_regression.exe --memtrack
```

Both configurations passed all 15 checks on 2026-10-22. The harness exercises 1.28
million CPU samples with concurrent resets and snapshots, frame-total maxima,
stale epochs, delayed GPU frames, UTF-8 names, asynchronous trace stop/export,
readback-ring and query exhaustion, and disabled profiling. Direct and compute
queues run on WARP with the D3D12 debug layer and GPU-based validation; no validation
warnings or errors were reported. The harness links the real allocator and GPU
memory census. `python tools/check_logging.py` reported zero findings.

Application integration: Debug and Release_Editor builds passed on 2026-10-22.

Release_Editor integration (`wind_test`, fixed 1280x720, `--trace=120`, no Streamline)
exited with code 0 and a clean session footer. JSON validation found exactly 120
CPU frame markers and 120 matching GPU frame markers, 6,849 direct-queue and 840
compute-queue events, and 240 counter samples. Timestamps/durations were nonnegative;
all GPU events belonged to the captured frame range. The trace is
`traces/trace_20261022_153737_release_16284_000.json` (generated, not versioned).

The first long full-GBV integration run is **not a pass**. It rendered 89 frames,
then terminated with `DescriptorHeapGPU overflow`, after repeatedly recreating
render targets (generation 14). No `render.validation` warnings/errors preceded
that exception, but the capture did not finish. The descriptor-heap failure was left
open at that point; the 2026-10-23 follow-up below identifies and fixes its cause.
Session: `logs/session_20261022_152439_12152_debug.log` (subject to normal log rotation).

A second **full guarded GBV** run, with a fixed 1280x720 window and an eight-frame
capture, passed: 40 rendered frames, exit code 0, a clean session footer, no
`render.validation` warnings/errors, and `live objects at exit: none`. Its trace
contains eight matching CPU/GPU frame markers, 471 direct-queue and 56 compute-queue
events, all inside the capture range with nonnegative timestamps/durations.
Session: `logs/session_20261022_153758_35164_debug.log`;
trace: `traces/trace_20261022_154430_debug_35164_000.json`.
This short pass alone did not close the long-run descriptor-heap finding above.

## Resize follow-up (2026-10-23)

Fix: `928ea104`.

Repeated WM_SIZE notifications reproduce the same failure without GBV: wind_test,
1280x720, no Streamline, 30 FPS, notifications spaced 120 ms apart. The old code
terminated on frame 17 (18 rendered frames), after target generation 10. Added
allocation diagnostics identified the CBV/SRV/UAV ring at base 0, with 4095 of 4096
entries used; the next five-descriptor allocation failed. This is slot reuse, not
an undersized heap or a persistent descriptor leak. Baseline session:
`logs/session_20261023_122736_38388_debug.log`.

App::Run pumps window messages after Renderer::BeginFrame. OnResize used to rebuild
the swapchain immediately, changing currentFrameIndex_ to 0 AFTER BeginFrame had
reset a different slot's command, descriptor, sampler and upload pools. Further
resizes repeatedly reused slot 0 without recycling it. OnResize now queues the
latest nonzero dimensions; BeginFrame applies them BEFORE selecting/waiting on
and resetting the frame slot. Identical dimensions do nothing, avoiding redundant
GPU-idle waits, target creation and history resets. Heap capacity is unchanged.

The existing scene-stress ResizeWindow operation now requests two sizes after
BeginFrame, verifies that the current frame remains unchanged, and checks that
the next frame adopts the latest request. Debug stress (17 iterations, 58 frames)
passed all three resize checks, exited 0 and logged a clean shutdown, with no
render.validation warnings/errors. Session:
`logs/session_20261023_123124_4356_debug.log` (subject to normal log rotation).

Debug and Release_Editor application builds passed. The profiler regression project
now links MemTrace.cpp from the concurrent memory-tracing implementation
(`fdd92be0`); all 15 profiler checks passed again in Debug and Release.

Release_Editor window/trace integration passed on wind_test at 1280x720, 30 FPS,
no Streamline: 100 duplicate WM_SIZE notifications caused **zero** target recreates;
40 alternating OS window-size requests were coalesced into 18 target recreates.
It rendered 241 frames, exited 0, and logged a clean shutdown. The 120-frame trace
contains matching CPU/GPU frame numbers 91-210, 6,946 direct-queue events, 840
compute-queue events and 240 memory counter records. All timestamps/durations are
nonnegative and GPU events stay inside the captured frame range. Session:
`logs/session_20261023_124124_54800_release_editor.log`; trace:
`traces/trace_20261023_124135_release_54800_000.json`.

Full guarded GBV also passed:
`test_cube.exe --scene-stress-gbv=3 --stress-level=data/levels/wind_test.json --no-streamline`.
The log confirms GUARDED_VALIDATION applied to the device, without a pass filter.
All 13 frames completed across reload, level switch and resize; the resize-boundary
regression passed, there were zero render.validation warnings/errors, and the exit
report says `live objects at exit: none`. Exit code 0, clean session footer.
Session: `logs/session_20261023_123222_18436_debug.log`.
The three iterations took 589 s and final debug-device teardown another 176 s;
this is GBV overhead, not an interactive resize measurement. Combined with the
reproducer and the 241-frame Release run, this closes the descriptor-heap finding.

Final checks: `python tools/check_logging.py` = 0 findings; all touched C++/project
files retain CRLF with no lone LF or CR. Concurrent memory-viewer edits were excluded
from these commits.
