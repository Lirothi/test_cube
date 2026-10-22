# Profiler efficiency review

Implementation: `fcbb93e0`. Regression build integration with memory tracking: `edc6dc5a`.

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
that exception, but the capture did not finish. The descriptor-heap failure is an
outstanding renderer finding; its cause has not been established by this review.
Session: `logs/session_20261022_152439_12152_debug.log` (subject to normal log rotation).
