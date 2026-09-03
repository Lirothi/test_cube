# Logging system — execution plan

**Status: ALL STEPS L1-L9 COMPLETE, 2026-09-02 (L1-L8 committed, L9 uncommitted).** L1 was
delivered by a first pass and then corrected (see its "Review findings"); L2-L9 followed in the
same session. Gate results and measured numbers are recorded under each step; the final file
list and overhead summary are at the end. The owner's decision on the artifacts that duplicated
session-log lines: **retired** (see "Retired artifacts" under L9).

This document is written as an execution contract for an AI working in this repository. Each step
must leave the tree buildable and independently verifiable. Do not silently combine steps, change
the logging contract while migrating call sites, or convert diagnostic artifacts into ordinary log
lines.

## Goal

Introduce one project-wide event logging system that:

- works in the normal Windows-subsystem application, headless runs and stress harnesses;
- accepts records concurrently from the main thread, renderer workers and D3D callbacks;
- performs no file I/O, filesystem calls, locks or heap allocation on the normal producer path;
- does not block a render/task thread when the log consumer falls behind;
- writes one readable UTF-8 session log and mirrors selected records to DBWIN;
- exposes recent records to a future ImGui log viewer without adding closed-window frame cost;
- preserves crash-critical evidence when the asynchronous path is unavailable;
- keeps profiler traces, benchmark output, DRED reports and other structured dumps as artifacts.

The primary result is diagnosability, not telemetry. No network sink, remote upload or analytics is
part of this plan.

## Verified baseline — do not re-discover before L1

Audit performed on 2026-09-02:

- `OutputDebugStringA/W` appears in **27 unique source files**.
- There are **35 literal `diag::LogPath(...)` destinations** in source.
- The working `logs/` directory currently contains **32 files / 598,193 bytes**.
- `DiagPaths.h::LogPath` calls `std::filesystem::create_directories` on every invocation.
- Several D3D callback paths open, append and close a file for each message.
- `Renderer::DiagLog` serializes producers with a mutex, writes synchronously and calls `fflush`
  per line.
- `Renderer::DiagLogOnce` additionally owns an `unordered_set<string>` keyed by formatted text.
- Multiple files use their own one-time flags, mutexes and per-file append/truncate conventions.
- Some files append between process runs (`ibl.log`, `bloom_kernel.log`, `unbound_root.log`), so a
  line has no reliable session identity.
- The main application configurations use the Windows subsystem. stdout/stderr cannot be the only
  sink even though a few profiler messages use `std::printf`.
- `TaskSystem` starts after window/system initialization and stops before renderer teardown.
  Logging therefore must own an independent writer thread and must not submit writer work to
  `TaskSystem`.
- `Profiler` already owns CPU/GPU timelines and Chrome trace JSON. Logging must not duplicate it.
- `BootProfile`, stress verdicts, benchmarks, DRED, barrier dumps and cache summaries have
  structured formats consumed by humans or scripts. They are artifacts, not event-log sinks.

Important current locations:

- `sources/core/diagnostics/DiagPaths.h` — only shared path helper.
- `sources/rendering/core/Renderer.cpp` — `DiagLog`, `DiagLogOnce`, terminate/device-removal paths.
- `sources/rendering/core/GraphicsDevice.cpp` — D3D12/GBV callbacks doing direct file I/O.
- `sources/rendering/core/RendererInvariantFailure.cpp` — last-chance renderer failure.
- `sources/assets/AssetImporter.cpp` — a separate mutexed file + DBWIN logger.
- `sources/app/App.cpp` — application/task/renderer lifetime and per-frame insertion point.
- `sources/app/main.cpp` — every early-return headless harness; logging must initialize before them.

## Non-goals

- Do not replace `Profiler`, `BOOT_SCOPE`, Chrome traces or profiler overlay dumps.
- Do not serialize arbitrary structured payloads through the ordinary text queue.
- Do not move existing fixed-name artifacts until every script/document consumer is audited.
- Do not add per-frame informational logging. A value sampled every frame belongs in a counter,
  profiler track or explicitly enabled diagnostic artifact.
- Do not add a third-party logging dependency in the foundation steps. The required core is small,
  and the D3D callback/emergency constraints still need project-owned code around a library.
- Do not make logging failure fatal. Failure to create `logs/` degrades to DBWIN/emergency output.
- Do not use logging to replace return values, HRESULT checks, assertions or harness exit codes.

## Rules for every implementation step

- New `.cpp/.h` files must be added to both `test_cube.vcxproj` and
  `test_cube.vcxproj.filters`.
- Preserve CRLF in C++ and project files. Verify every touched text file has zero lone LF/CR.
- Build all THREE configurations after every step: Debug x64, Release x64, Release_Editor x64.
  `WITH_EDITOR` is defined in Debug and Release_Editor only; Release compiles every editor TU
  empty, so a Release-only build verifies nothing about editor code.
- One logical step may become one commit, but create commits only when the human explicitly asks.
- A migration step may change only logging side effects. Rendering, asset resolution, fallback
  decisions and harness exit codes must remain unchanged.
- No log call may be added to a hot loop without an explicit rate/once gate and a measured disabled
  cost.
- Failure paths must not log the same event twice at two abstraction layers. The layer that owns
  the recovery decision owns the record.
- Every asynchronous record is best-effort. Correctness must never depend on its delivery.

## Target contract

### Levels

```cpp
enum class LogLevel : std::uint8_t
{
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Fatal
};
```

Semantics:

- `Trace` — explicitly enabled high-detail diagnostics; compiled out of normal Release builds.
- `Debug` — developer reasoning/state transitions, normally disabled in Release.
- `Info` — lifecycle and completed user-visible/headless operations.
- `Warning` — recoverable fallback or degraded quality/capability.
- `Error` — requested operation failed but the process can continue.
- `Fatal` — invariant is broken and the process will terminate after emergency output.

### Categories

Use a closed enum and a static name table. Do not use heap-owned category strings or runtime hashes
on the producer path.

```text
Core
App
Scene
Asset
Editor
Task
Render
RenderRhi
RenderGraph
RenderShadow
RenderRt
RenderValidation
Vfx
Ocean
Profiling
```

The text names are hierarchical (`render.rhi`, `render.graph`, etc.) even though storage is an enum.
An atomic minimum-level array indexed by category provides runtime filtering before formatting.

### Public API shape

The exact macro spelling may change in L1, but these capabilities and evaluation rules may not:

```cpp
LOG_TRACE(logging::LogCategory::RenderGraph, "Pass {} skipped", passName);
LOG_DEBUG(logging::LogCategory::Asset, "Resolved {} -> {}", source, dds);
LOG_INFO(logging::LogCategory::Scene, "Loaded level {}", path);
LOG_WARNING(logging::LogCategory::RenderRt, "RT allocation failed; switching to SSR");
LOG_ERROR(logging::LogCategory::Asset, "Could not import {}: {}", path, error);
LOG_FATAL(logging::LogCategory::Render, "Invariant failed: {}", reason);

LOG_WARNING_ONCE(logging::LogCategory::RenderRt, "...");
LOG_DEBUG_EVERY_N(120, logging::LogCategory::RenderShadow, "...");
LOG_INFO_THROTTLED(std::chrono::seconds(2), logging::LogCategory::Vfx, "...");
```

Requirements:

- arguments are not evaluated when compile-time or runtime filtering rejects the record;
- source location is captured at the call site;
- literal/scalar formatting writes directly into the record's fixed buffer;
- `WriteRaw` accepts already formatted SDK/allocator callback text without another allocation;
- wide strings are converted to internal UTF-8 by an explicit helper;
- `Fatal` is `[[noreturn]]` only when the macro owns termination. If a caller must dump DRED first,
  it uses `EmergencyWrite` and retains control of termination.

The formatting backend is isolated behind one header. `std::format_to_n` is acceptable only if the
L1 allocation test proves zero allocations for the common literal/scalar path on this MSVC. If it
allocates or throws in the measured path, replace only that backend before proceeding.

### Record layout

`LogRecord` is trivially movable/copyable and contains no owning pointers to dynamic data:

```text
QPC timestamp
global sequence number
frame number, or invalid before the first frame
process/thread id
LogLevel + LogCategory
source file/function static pointers + line
message byte count + flags (truncated/emergency)
fixed UTF-8 message buffer (target 768–1024 bytes)
```

The file sink renders one line in this stable shape:

```text
[01:14:22.381][WARN][render.rt][1842] RT allocation failed; switching to SSR (SceneRenderer.cpp:217)
```

Fields: every service field in its own `[]`, no spaces between them — time of day, level,
category, frame (`-` before the first frame) — then one space, the message, and for Warning+ the
`(File.cpp:line)` suffix. The pre-init DBWIN fallback uses the same run minus clock and frame:
`[LEVEL][category] message`. **Amended three times after L8 at the owner's request** (the third,
2026-09-03: brackets around every field, spaces removed — the previous shape was
`01:14:22.381 WARN [render.rt] 1842 ...`) — the original shape also carried the sequence number,
the full date, the elapsed clock and the thread name, padded into columns (85 characters of
prefix per line, then ~50); now ~30. The date lives only in the session header and file name,
the sequence and the thread only in the viewer, and elapsed time is what the viewer's time
column shows. Session files are UTF-8 without BOM. The debugger sink converts UTF-8 to UTF-16
and calls `OutputDebugStringW`.

### Producer/consumer model

- One bounded MPSC queue, one dedicated writer thread.
- Capacity is fixed at initialization; start with 8192 records and measure memory/catch-up time.
- Producers check category level before timestamping or formatting.
- Enqueue is non-blocking. No producer waits for disk or the consumer.
- Writer wakes through a semaphore/event and drains batches, rather than waking once per record.
- Writer assigns wall-clock text from the stored QPC/session epoch; producers do not format dates.
- Writer owns file handles and sink state.
- Queue overflow increments per-level atomic drop counters. The next successful drain emits one
  synthetic warning with the accumulated counts.
- Error records that fail to enqueue are mirrored immediately to DBWIN (raw text, stack buffer).
  Warning records are deliberately NOT (amended from "Warning/Error" in L2): OutputDebugString
  serialises every caller on a process-wide mutex and a hung DBWIN listener stalls each call for
  seconds, so a warning storm on a full ring would block exactly the render/task threads this
  section promises never to block. Dropped warnings are still counted and named by the next
  drain's synthetic report.
- Fatal is queued in order like any record and then followed by a synchronous `Flush` (2 s) so
  the caller may abort; if the push or the flush fails the line is appended through the
  emergency handle instead (marked `[emergency]`).

### Emergency path

The emergency path is separate from the queue and normal sinks:

- no heap allocation, `std::filesystem`, normal logger mutex or dependency on the writer thread;
- fixed-buffer message only;
- writes DBWIN immediately;
- best-effort append to a pre-established emergency destination using Win32 primitives;
- cannot spin forever if another crashed thread held a lock;
- `Flush(timeout)` is attempted before a controlled abort, but timeout expiry never prevents exit.

DRED, crash stack and device-removal structured reports continue to use their specialized writers.
The emergency log records where those artifacts were requested/written.

### Default sinks and filtering

- File: everything the producer-side category threshold accepts (Debug+ in Debug, Info+ in
  Release by default; `--log-level`/`--log-category` move that threshold and the file follows).
- DBWIN: Debug+ in Debug, Warning+ in Release (`LogConfig::debuggerMinimum`).
- Memory ring: same as the file; fixed at the latest 4096 records (4.3 MiB).
- Console: only when a console is actually attached or explicitly requested.
- Optional JSONL is deferred; it is not needed for the first complete implementation.

Session path:

```text
logs/session_YYYYMMDD_HHMMSS_PID_debug.log
```

Use a unique filename so simultaneous headless processes cannot truncate each other. Maintain a
small `logs/latest.txt` containing the newest session path; it is a hint, not a sink. Retention may
delete only `session_*.log` files owned by this system, never fixed-name artifacts.

### Events versus artifacts

General event examples:

- selected GPU/feature mode;
- level load begin/end/failure;
- fallback from RT to SSR;
- missing texture using WIC fallback;
- screenshot/trace/import completion;
- user-triggered save/reload failure.

Artifacts that remain separate:

- profiler Chrome trace JSON;
- `boot_profile.log`;
- DRED/crash stack/device reports;
- benchmark CSV/TXT;
- barrier transition/census dumps;
- VSM page samples;
- cache summaries and import reports with a stable external format.

An artifact-producing subsystem writes a normal event containing the artifact path and verdict.
It does not stream every artifact row through the event queue.

## Separable implementation steps

### L1 — Logging types, filtering and fixed-buffer frontend

**Status: COMPLETE (2026-09-02).**

**Goal:** land the public contract without changing any existing call site.

**Touch:**

- add `sources/core/logging/LogLevel.h`;
- add `sources/core/logging/LogCategory.h`;
- add `sources/core/logging/LogRecord.h`;
- add `sources/core/logging/Log.h/.cpp`;
- add all files to both project files.

**Work:**

- implement static categories and atomic per-category thresholds;
- implement source-location capture, fixed-buffer formatting and truncation flag;
- implement `WriteRaw` and UTF-16-to-UTF-8 helper;
- before initialization, route records to a minimal DBWIN fallback;
- add callsite-local `ONCE`, `EVERY_N` and time-throttle primitives using atomics;
- do not add the asynchronous queue or session file yet;
- add focused self-tests callable from code: filtering must skip argument evaluation; common
  formatting must not allocate; oversized messages must truncate and remain terminated.

**Gate:** Debug/Release x64 build; frontend self-tests pass; zero production call sites migrated;
zero lone LF in new C++ files.

**Implemented result:**

- Public API lives in namespace `logging`; call sites use the `LOG_*` macros so rejected format
  arguments are not evaluated.
- The closed category table and per-category atomic thresholds are implemented. Defaults are
  `Debug` in Debug and `Info` in Release; `Trace` macros compile out under `NDEBUG`.
- `LogRecord` is trivially copyable, carries QPC/sequence/frame/PID/TID/source metadata and owns a
  1024-byte UTF-8 message buffer. The frame remains invalid until L3 publishes it.
- Formatting is a project-owned, `noexcept`, fixed-buffer backend rather than `std::format`. It
  supports common strings, UTF-16 strings, scalar values, integer hex/width and fixed floating
  precision. It has no heap fallback; bad formats are flagged and oversized output is terminated
  and flagged as truncated.
- `WriteRaw`, `WriteRawWide`, explicit UTF-16-to-UTF-8 conversion and callsite-local `ONCE`,
  `EVERY_N`, and duration-based `THROTTLED` macros are implemented.
- Until L2 installs the queue and sinks, accepted records use the minimal UTF-16 DBWIN fallback.
  No session file is expected at L1.
- `RunFrontendSelfTests(const AllocationProbe*)` is callable without modifying process lifetime
  and is run by `--log-stress` (L3) with a mimalloc-backed probe. No existing production log
  call was migrated.

**Review findings (second pass, 2026-09-02) — all fixed:**

- `LOG_INFO(cat, "{}", nullptr)` crashed (0xC0000005): in C++20
  `is_convertible_v<nullptr_t, string_view>` is true, so the dedicated `nullptr_t` branch of
  `AppendArgument` was dead code and a bare `nullptr` reached `string_view(nullptr)`. The branch
  now comes first; the self-test has a regression check.
- The in-tree "does not allocate" check was a tautology (`check(!kFormatterUsesHeap)` on a
  hard-coded `false`), and the standalone harness that had really measured it was not kept.
  The self-test now takes an allocation-count probe, calibrates it against a real `new`, and
  reports the check as *skipped* when the allocator cannot count (mimalloc's release build,
  `MI_STAT 0`). In Debug the probe is live and the format+submit path is measured at zero
  allocations.
- `ThrottleAllows` divided by a dynamically initialised global QPC frequency: a throttled call
  from another TU's static initializer would have divided by zero. It is a function-local
  static now; every other namespace-scope object in `Log.cpp` is constant-initialised.
- `LogRecord{}` zeroed the 1 KiB message buffer on every accepted call; the producer path now
  default-initialises the record and only `messageByteCount` bytes are ever read or copied.
- The build rule named two configurations; there are three (Release_Editor added above).

### L2 — Bounded queue, writer thread and sinks

**Status: COMPLETE (2026-09-02).** See "Implemented result" after the gate.

**Depends on:** L1.

**Goal:** implement the final asynchronous backend before real subsystems start using the API.

**Touch:**

- add `sources/core/logging/LogQueue.h/.cpp`;
- add `sources/core/logging/LogSinks.h/.cpp`;
- extend `Log.h/.cpp` with `Initialize`, `Flush`, `Shutdown` and sink configuration.

**Work:**

- implement the fixed-capacity MPSC queue and single consumer;
- use an independent writer thread, not `TaskSystem`;
- add unique session file, DBWIN and bounded memory sinks;
- batch file writes; flush periodically and immediately for Error/Fatal;
- add overflow counters and one synthetic drop record per drain cycle;
- make repeated `Initialize/Shutdown` calls safe for harness processes/tests;
- session header records build configuration, PID, command line, working directory and executable;
- session footer records clean/unclean shutdown and dropped counts.

**Gate:** multi-producer test with more producers than worker count; exact delivered+dropped accounting;
per-thread sequence order preserved; shutdown drains queued records; a deliberately unwritable log
directory still produces DBWIN and does not fail the run; both builds pass.

**Implemented result:**

- `LogQueue` (`LogQueue.h/.cpp`): Vyukov bounded MPMC ring used single-consumer. A slot is
  claimed with a seq_cst CAS and published with a release store on its sequence; a full ring
  fails the push. Only `UsedRecordBytes(record)` (header + message + terminator) are copied in
  and out, so an 80-byte message moves ~130 bytes, not the 1 KiB slot. Capacity is rounded to a
  power of two; the default 8192 slots cost 8.8 MiB (slot = 1072 B).
- `LogSinks` (`LogSinks.h/.cpp`): `SessionClock` (QPC epoch + `GetSystemTimePreciseAsFileTime`,
  overflow-safe split for multi-day uptimes), `ThreadNameTable` (128 fixed entries, SRW lock,
  try-lock on read so the emergency path cannot block behind a dead thread), `FormatLine` (the
  one-line shape from "Record layout"; Warning+ get a ` (File.cpp:line)` suffix; flags render as
  ` [truncated]`, ` [format-error]`, ` [emergency]`; trailing CR/LF stripped), `FileSink`
  (64 KiB buffer; `FILE_APPEND_DATA` handles so the emergency handle interleaves whole lines
  with the buffered writer; `Commit` = WriteFile, `Sync` = + best-effort FlushFileBuffers),
  `WriteDebugger` (UTF-8 -> UTF-16 on the stack), `ConsoleSink` (stdout when it is a console,
  pipe or file — a plain Windows-subsystem launch has none), `MemoryRing` (latest N records,
  writer-only producer, cursor = running write count so "newer than cursor" is exact).
- Writer thread is a raw `_beginthreadex` thread, not `std::thread`: `--scene-stress` exits
  through `TerminateProcess`, and a joinable `std::thread` global reached by `ExitProcess`'s
  static destructors is `std::terminate`. Idle handshake: the writer publishes `writerIdle`
  (seq_cst), re-checks the ring, then waits on an auto-reset event with `flushIntervalMs`
  (250 ms) timeout; a producer signals the event only when it sees the idle flag, so the
  common case costs no syscall. Batches commit every 256 records and at the end of each
  drain; Error/Fatal commit immediately.
- Session state lives in one heap object behind `std::atomic<LoggerState*>`; producers wrap
  every access in a counter (`g_activeProducers`, two seq_cst RMWs per accepted record) so
  `Shutdown` can null the pointer, wait for in-flight producers, stop the writer, write the
  footer and free the state without a use-after-free. If either wait times out the state is
  abandoned (leaked) and an emergency line says so.
- Records before `Initialize`/after `Shutdown` go to the L1 DBWIN fallback. `Initialize`
  twice returns false; `Shutdown` twice is a no-op; a file that cannot be created degrades to
  DBWIN with one warning in the session (DBWIN-only) and `Initialize` still returns true.
- Session header (4 Info records: build/pid/exe, cwd, command line, file/queue/ring/sync) goes
  through the normal path so it is ordered; the footer
  (`session end: clean shutdown; submitted= written= dropped= fileBytes= elapsed=`) is written
  by `Shutdown` after the writer stopped. A missing footer is the unclean-end marker.
- `EmergencyWrite`: thread-local recursion guard (one attempt), stack-only formatting, DBWIN
  first, then an unbuffered append through the second handle. `Flush(timeout)` never waits
  past its deadline; called from the writer thread itself it drains inline.
- `--log-sync` mode: the calling thread renders under an SRW lock and commits per record; the
  writer thread is not created.

### L3 — Process lifecycle, CLI controls and stress harness

**Status: COMPLETE (2026-09-02).** See "Implemented result" after the gate.

**Depends on:** L2.

**Goal:** make every normal and early-return executable mode own a complete log session.

**Touch:**

- `sources/app/main.cpp`;
- `sources/app/App.cpp`;
- task-system backends only where thread names can be added without changing scheduling;
- add `sources/core/logging/diagnostics/LogStress.h/.cpp`.

**Work:**

- initialize logging at the top of `WinMain`, before any harness branch;
- own shutdown with an RAII session so every early return flushes;
- parse `--log-level=<level>`, repeated `--log-category=<name>:<level>`, `--log-sync`,
  `--log-no-file` and optional `--log-file=<path>`;
- publish the main thread name and frame number; frame is invalid during boot;
- keep the logger alive until after `TaskSystem::Stop`, renderer shutdown and system teardown;
- add `--log-stress`: concurrent known-count production, oversized record, overflow mode and clean
  shutdown; return non-zero on any accounting/format failure;
- optional `--log-fatal-smoke` runs only as a child process and proves the emergency tail survives.

**Gate:** all existing early-return harnesses still return their old codes; `--log-stress` passes
repeatedly; unique files from two concurrent processes do not collide; Debug/Release build.

**Implemented result:**

- `WinMain` dispatches `--log-stress` first (the harness owns the logger's lifetime), then
  creates a `LogSession` RAII object whose constructor runs `ApplyCommandLine` + `Initialize` +
  `SetCurrentThreadName("Main")` and whose destructor runs `Shutdown`; every early-return
  harness below it flushes on return. `App::RunSceneStress` ends in `TerminateProcess` on both
  of its paths, so `SceneStress.cpp` calls `logging::Shutdown()` before each.
- Frame number: published from the frame loop in `App::Run` and in the scene-stress loop
  (`logging::SetFrameNumber(renderer.GetTotalFrameNumber())` next to `Profiler::BeginFrame`).
  `[frame=-]` during boot, numeric afterwards (verified: the footer of a `--shot` run reads
  `[frame=991]`).
- Thread names: `Main`, `LogWriter`, and `Worker<N>` from `TaskSystem::WorkerLoop`
  (`TaskSystemLockFree.cpp`, the active backend; one call at thread start, scheduling
  untouched). Also pushed to `SetThreadDescription` (resolved dynamically) for the debugger.
- CLI (`logging::ApplyCommandLine`, whole-token matching so `--log-syncless` does not parse as
  `--log-sync`): `--log-level=<level>`, repeated `--log-category=<name>:<level>`, `--log-sync`,
  `--log-no-file`, `--log-file=<path>` (quoted paths accepted).
- `--log-stress` (`sources/core/logging/diagnostics/LogStress.h/.cpp`, verdict in
  `logs/log_stress.log`, exit code = failed checks) runs: frontend self-tests with the mimalloc
  probe; command-line parsing; accounting with `hardware_concurrency + 4` producers and a
  ring that holds every record (exact delivery, zero drops, per-thread order, thread names in
  the file); overflow with a 256-slot ring under 240 000 records (drops > 0,
  delivered + dropped == produced exactly, order preserved among survivors, synthetic drop
  report present); oversized record (cut at 1023 bytes, `[truncated]`); unwritable session
  path (degraded, no crash, clean shutdown); 20 Initialize/Shutdown cycles; synchronous mode
  (record visible in the file before any Flush); memory ring (exactly capacity, newest
  records, cursor semantics); a child process that logs `LOG_FATAL` and dies through
  `TerminateProcess` without Shutdown (the fatal line and everything queued before it are in
  the file, no footer); two children opening auto-named sessions at the same instant
  (distinct files, both complete); and a microbenchmark.
- `--log-fatal-smoke` from the plan is folded into `--log-stress` as the fatal-child scenario
  (internal flags `--log-stress-fatal-child` / `--log-stress-session-child`).

**Gate results (2026-09-02, 32 hardware threads):**

- Debug x64, Release x64, Release_Editor x64 build.
- `--log-stress`: **0 failed checks in Debug and in Release**; self-tests 10 passed; the
  allocation check is measured in Debug (mimalloc `MI_STAT 2`) and reported skipped in Release.
- `--tasksystem-stress` still returns 0 and leaves `logs/session_<stamp>_<pid>_release.log`
  with header + footer. A `--level=data/levels/wind_test.json --shot=... --shot-delay=3`
  run boots the full renderer, writes the PNG, exits 0 and its session footer carries
  `[frame=991]`.
- Microbenchmark (Release, two runs — the second while a build was running): filtered call
  0.2-0.7 ns (hoisted threshold check), accepted call 113-174 ns on the producer (format +
  stamp + push), consumer 0.9-3.3 M records/s to file with DBWIN off. Debug: 26 ns / 2.5 us /
  234 k records/s (unoptimised STL `to_chars`). Re-measure on an idle machine before quoting.
- Two harness bugs found while writing it, worth remembering: `_wfopen_s` opens exclusively
  and fails with a sharing violation while the sink holds the file (`_wfsopen(_SH_DENYNO)`),
  and "a ring large enough" must be sized against `threads x records`, not assumed.

### L4 — Critical renderer, D3D callback and termination paths

**Status: COMPLETE (2026-09-02).** Gate results after "Implemented result".

**Depends on:** L3.

**Goal:** remove filesystem/file-I/O work from callbacks and centralize process-ending messages.

**Touch:**

- `sources/rendering/core/GraphicsDevice.cpp`;
- `sources/rendering/core/Renderer.cpp`;
- `sources/rendering/core/RendererInvariantFailure.cpp`;
- `sources/app/diagnostics/SceneStress.cpp` only where it mirrors crash verdicts.

**Work:**

- D3D12/GBV callbacks use `WriteRaw`; no `fopen`, `filesystem` or heap formatting in callbacks;
- map D3D severity to logging level without weakening current filtering/deduplication;
- invariant/terminate/device-removal paths emit one central Fatal/Error record;
- preserve and reference existing `invariant_failure.log`, `device_removed.log`, DRED and crash-stack
  artifacts until L7; do not squeeze their multi-line content into `LogRecord`;
- ensure a failure on the writer thread itself uses the emergency path and cannot recurse;
- do not change when the process aborts or which renderer fallback is selected.

**Gate:** controlled fatal child writes a readable final record; `--scene-stress-gbv=20` remains
CLEAN; callback flood cannot deadlock and reports dropped counts; renderer invariants still abort;
Debug/Release build.

**Implemented result:**

- `GraphicsDevice.cpp` (Debug-only callbacks): `GbvMessageCallback` and `BarrierMessageCallback`
  no longer `fopen`/append/close per message. Each artifact (`gbv.log`,
  `barrier_msg_trace.log`) is opened ONCE at callback registration as an append-only handle
  (`CREATE_ALWAYS`, so "fresh per run" is unchanged) and the callback does one `WriteFile`;
  the handles live for the process because a callback can still fire during device teardown.
  `gbv.log` is kept because "gbv.log is empty" is a documented verdict
  (`docs/gbv_startup_profile.md`); the multi-line module backtrace is kept because it IS the
  barrier-trace artifact. The one-line event goes to the session log through `WriteRaw`
  under `render.validation`: GBV WARNING -> Warning, ERROR/CORRUPTION -> Error; barrier trace
  -> Warning with the first 400 bytes of the description and a pointer to the artifact. The
  severity filter and both dedupe tables are untouched. Callbacks never emit Fatal (Fatal
  flushes synchronously, and the callback runs under its own spin lock inside a D3D call).
- Boot-time `[caps]` lines (`InitDevice`, `InitQueue`) and the `[gbv] requested patch mode`
  line: `OutputDebugStringA` replaced by `WriteRaw(Info, render.rhi)`; "async compute queue
  NOT created" is a Warning. `device_caps.log` stays the artifact other probes append to.
- `Renderer.cpp`: the Streamline log callback maps `sl::LogType` eInfo/eWarn/eError to
  Debug/Warning/Error under `render.rhi` via `WriteRaw` (its Info chatter is ~220 lines per
  Debug boot and is dropped by the Release threshold). `ReportOnTerminate` emits one Fatal
  through `EmergencyWrite` after the device-removal report; `ReportDeviceRemovalOnce` emits
  one Error (`device removed: reason=... frame=... ; breadcrumbs in logs/device_removed.log`)
  through `EmergencyWrite` because it runs from the terminate handler as often as from
  BeginFrame; `DumpDebugLayerMessages` emits one Warning naming how many messages it drained
  into `invariant_failure.log`. When the process aborts and which fallback is selected are
  unchanged.
- `RendererInvariantFailure.cpp`: artifact append first (unchanged), then one central Fatal
  via `EmergencyWrite` (DBWIN + unbuffered session append, stack buffer, no writer-thread
  dependency), then `std::abort()` exactly as before. The three raw `OutputDebugStringA`
  calls are gone (the emergency path writes DBWIN itself).
- `SceneStress.cpp` (verdict mirrors only): `StressCrashFilter` emits one Fatal via
  `EmergencyWrite` BEFORE the dbghelp stack walk (which can itself fault) pointing at
  `crash_stack.txt`; `verdict: CLEAN` mirrors as `LOG_INFO(app, ...)`, `verdict: FAULT` as
  `LOG_ERROR(app, ...)` naming `scene_stress.log` and `dred_dump.txt`. Both paths reach the
  `logging::Shutdown()` added in L3 before `TerminateProcess`.
- Writer-thread failure: the writer never logs through the queue and no sink calls back into
  the frontend; a crash on it reaches the process crash filter / terminate handler on THAT
  thread, whose `EmergencyWrite` uses its own file handle, takes no logger lock
  (`ThreadNameTable::Get` is a try-lock) and is guarded by a thread-local depth counter, so
  it cannot recurse or deadlock behind the dead writer.
- Untouched on purpose (later steps): `Renderer::DiagLog/DiagLogOnce` and their callers (L6),
  `texcache.log`, `submit_order.log`, `device_caps.log` writers and `DiagPaths` (L7).

**Gate results (2026-09-02):**

- Debug x64, Release x64, Release_Editor x64 build.
- `--scene-stress-gbv=20` (Debug, guarded validation): `verdict: CLEAN after 20 iterations
  (total 171.7 s)`, exit code 0; `logs/gbv.log` holds only its mode header (zero GBV
  messages, i.e. zero `render.validation` records — consistent with the pre-L4 verdict rule);
  the session log `session_l4_gbv.log` ends with the clean-shutdown footer written by the
  `logging::Shutdown()` before `TerminateProcess` (390 records, 0 dropped) and carries the
  `scene-stress verdict: CLEAN` mirror and both `[caps]` lines.
- Controlled fatal child: covered by `--log-stress` (L3), still 0 failed checks.
- Callback flood: cannot deadlock by construction — the callback's own spin lock wraps only
  `WriteFile` on a pre-opened handle plus `WriteRaw`, which never blocks (a full ring drops,
  counts, and the next drain reports the count; proven under 240 000 records by the L3
  overflow scenario). Not separately reproduced with a real GBV flood: this scene is clean.
- Renderer invariants still abort: `RendererInvariantFailure` ends in `std::abort()` exactly
  as before (by inspection; no invariant can be provoked from the command line).
- Debug `--shot` run (`session_l4_debug.log`): 227 records, Streamline chatter at Debug under
  `render.rhi` (its warning about a duplicated plugin id carries `(Renderer.cpp:50)`),
  `[caps]` at Info, footer `[frame=520]`, 0 dropped.

### L5 — Migrate ordinary event logging by domain

**Depends on:** L4.

**Goal:** eliminate direct DBWIN/printf event emission while preserving specialized artifacts.

This step is intentionally divisible. Complete and gate one domain at a time:

- **L5a App/Core:** `App.cpp`, `main.cpp`, profiler status messages, allocator callback.
- **L5b Scene/Render:** scene renderer, RT fallback, VSM mode/fallback, shadows, instancing.
- **L5c Assets/Materials:** texture resolution, materials, mesh manager, font manager, importer status.
- **L5d Editor/VFX/Ocean:** editor panels, thumbnail status, particles and ocean diagnostics.

**Rules:**

- map each message to a real severity and category; do not mechanically mark everything Info;
- preserve one-time/per-path semantics explicitly;
- allocator and SDK callbacks must use the raw no-allocation frontend;
- an importer progress report may keep `asset_import.log` as an artifact, but important begin/end/
  failure events also enter the session log;
- leave structured file writers in place for L7;
- after each substep, search the touched domain for direct `OutputDebugString` and unowned
  `printf`; remaining occurrences need an explanatory comment or migration.

**Gate per substep:** builds; representative headless path; no duplicate lines in DBWIN; expected
artifact still produced; no render/image behavior change.

**L5a App/Core — COMPLETE (2026-09-02).** Domain = `sources/app/App.cpp`, `sources/app/main.cpp`,
`sources/core/**` (of which only `profiling/Profiler.cpp` emitted anything). `sources/app/scene/*`
is L5b, `sources/app/diagnostics/SceneStress.cpp` writes its own artifact and was handled in L4.

- `App.cpp`: mimalloc output callback `MiOut` -> `WriteRaw` (allocator callback = raw frontend).
  The statistics printer emits one table row as several fragments through separate calls, so
  the callback accumulates per thread (thread_local, 1 KiB, no heap) until a newline; level by
  prefix: `mimalloc: error` -> Error, `mimalloc: warning` -> Warning, else Info under `core`.
  The exit statistics are printed explicitly at the end of `App::Run` through
  `mi_stats_print_out(MiOut, ...)` instead of `mi_option_show_stats` at process exit, because
  by process exit the log session is closed and the table only reached DBWIN (headless: nowhere).
- `App.cpp` events: `--set <name> = <v>` Info / `... UNKNOWN SETTING (ignored)` Warning;
  `--sweep <name>: unknown setting` Warning, still once per run via the existing flag (the
  name does not change between shots, so `LOG_WARNING_ONCE` would have been the wrong tool
  only by accident); `--sweep shot <i>: <name> = <v>` Info; `screenshot saved: <path> (frame N)`
  Info / `screenshot FAILED: <path>` Error; `profiler overlay dump saved/FAILED: <path>` under
  `profiling`.
- `main.cpp`: `--reimport-manifest: could not read <path>` Error under `asset` (exit code 3
  unchanged; the RAII session flushes it).
- `Profiler.cpp`: five `std::printf` (invisible in a Windows-subsystem process) -> `profiling`
  records: trace capture requested/stop requested/canceled (Info), `trace capture FAILED:
  cannot write <path>` (Error), `trace saved: <path> (<n> events)` (Info — the
  artifact-producing event; the Chrome JSON stays the artifact).
- Direct-output audit of the domain after the step: zero `OutputDebugString` / `printf`.

L5a gate (2026-09-02): Debug/Release/Release_Editor build. Debug run with
`--sweep=exposure.lowPercentile:0.02,0.35 --set=exposure.compensationEv:-0.15;bogus.setting:1
--trace=30`: both `--set` records (the unknown one as a Warning with `(App.cpp:1270)`), both
`--sweep shot` records, `screenshot saved: ... (frame 577)`, `trace capture requested: 30
frames`, `trace saved: traces/trace_..._debug_000.json (7028 events)` emitted from `Worker14`,
0 dropped. A second Debug run without `--trace` produced both PNGs, two `screenshot saved`
records and the mimalloc statistics table assembled row-by-row at the end of the session.
`--reimport-manifest=nonexistent.mesh.json` still exits 3 and leaves `[ERROR] [asset]
--reimport-manifest: could not read nonexistent.mesh.json (main.cpp:813)` in its session.
Release run: 34 records, zero Streamline/Debug lines, `[caps]` + `--set` + `screenshot saved` +
footer. (Note for anyone combining flags: `--trace=N` ends the run N+30 frames after its
request regardless of a pending `--shot-count`; pre-existing, not a logging change.)

**L5b Scene/Render — COMPLETE (2026-09-02).** 27 sites in 12 files; every artifact writer next to
them (`shadow_casters.log`, `vsm.log`, `vsm_pages.log`, `ibl.log`, `texcache.log`,
`texcreate.log`, `device_caps.log`) is untouched for L7. Mapping:

- `render.shadow`: `ShadowGpuData` rebuilt summary / `VSM allocated` / `shaders ready` Info
  (`WriteRaw` where the same text also goes to the artifact); the seven PSO-creation failures
  Error; masked-group overflow Warning; `frame update: n/N casters re-uploaded` Debug behind
  the existing five-frames-after-rebuild counter; cull validation verdict Info on PASS,
  Warning on MISMATCH; `single-draw page render OFF: <reason>` Warning (still once per reason
  via `singleDrawFallbackLogged_`); the page-stats sample Debug behind `vsm::g_logPageStats`
  and `kDbwinLogPeriod` (the constant keeps its name; the comment now says what it throttles).
- `render.rt`: AS VRAM report Info (once per level via `asVramLogged_`); "AS/bindless
  allocation failed; disabling RT, falling back to SSR" Warning once per scene via
  `rtFailureLogged_` (narrower than ONCE-per-process on purpose); bindless descriptor
  exhaustion Error **on the transition only** (`if (!buildFailed_)`) — the old line repeated on
  every registration after exhaustion.
- `render`: IBL path (`split-sum ON` Info / any fallback to the legacy sky mip chain Warning;
  `LogIbl` now takes the level), multi-slot instancing batch active `LOG_INFO_ONCE` (replaces
  the local `loggedOnce` static), gbuffer slot material unusable Warning (per object slot, at
  material resolution, not per frame), texcache summary Info at shutdown.
- `render.rhi`: compute-lane probe Info/Warning by verdict; `CreateCommittedResource3`
  refusal Warning (per call, as before: "a refusal is rare and worth every line").
- `vfx`: the W1/W2 wind self-check Debug, still exactly the first 8 frames.
- `asset`: instanced model failed to load; object disabled — Error.
- Left with a comment: `Renderer::DiagLog` (L6). Left for L5c: `MeshManager.cpp` (5).

L5b gate: Debug/Release/Release_Editor build. Debug `--shot` run: `render.shadow` 9,
`vfx` 8, `render` 3, `render.rt` 1, `app` 1 records, all with the expected levels; the five
`frame update` Debug lines stop after frame 4; `shadow_casters.log`, `ibl.log`,
`texcache.log`, `device_caps.log` updated by the run (`vsm.log`/`vsm_pages.log` are gated on
conditions this run does not hit, `texcreate.log` absent = no refusals). Release run: 41
records, zero Debug-level, same Info/Warning set. Surfaced, not caused: `[ShadowGpuData] cull
validation MISMATCH` reports 5/46 views in Debug and 15/46 in Release — the validation is
non-deterministic and was previously readable only in `shadow_casters.log`.

**L5c Assets/Materials — COMPLETE (2026-09-02).** 27 sites in 7 files + the importer's logger.

- `Material.cpp` (`render`): shader-compiler output goes through a file-local
  `LogShaderCompilerOutput` that emits one record per line (DXC and D3DCompile output is
  multi-line); DXC output is Warning when the compile succeeded (warnings) and Error when it
  failed, D3DCompile output is only ever seen on failure (Error). `DXC <stage> compile failed
  for <file> (<entry>); falling back to D3DCompile SM5` Warning; `missing/invalid
  [RootSignature] in <file>` Error; `material CreateGraphics/CreateCompute failed: <file>` Error
  (the old lines named no file at all). Shader-cache and PSO-cache summaries Info at shutdown
  (their artifact files unchanged).
- `Texture2D.cpp` (`asset`): `<png> -> DDS sibling <dds>` Debug, `unmipped texture (no DDS
  sibling, WIC fallback)` Warning — both still behind the per-path dedupe set in
  `LogTextureResolveOnce` (L6 decides its fate); DDS/WIC load failures Error with the path.
- `TextureCube.cpp`, `MaterialDataManager.cpp`, `FontManager.cpp` (`asset`): read/parse
  failures Error (the "not a cubemap" line now carries the array size and the path), unknown
  `shadingModel` Warning, font atlas generation failure Error.
- `MeshManager.cpp` (`asset`): `[meshbake]` prune/chunk/bake summaries Info (chunk-grid
  REJECTED Warning, bake FAILED Error) via `WriteRaw`; `failed to read mesh binary` Error;
  `GltfLog` takes a level: parse/load_buffers/resolve failures Error, group-index fallback
  Warning, per-file "loaded" and per-material summaries Debug (a level with many un-baked
  glTF meshes would otherwise print one Info line per mesh per boot).
- `AssetImporter.cpp` (`asset`): the importer's `Log::Line` keeps `asset_import.log` as the
  artifact and mirrors each line to the session at a level read off the importer's OWN
  markers (they are its severity convention): `FAIL`/`FATAL` Error, `SKIPPED`/`WARN`
  Warning, `=== ... ===` banners Info, everything else Debug. So a Release session keeps the
  begin/end banners plus failures, exactly the "important begin/end/failure events" the plan
  asks for, and the DBWIN mirror is gone. Not exercised at runtime here: `--import` writes
  into the content tree and is the user's call (compile-verified only).

L5c gate: Debug/Release/Release_Editor build. Debug `--shot` run: 61 `asset` records (the
per-texture DDS-sibling resolutions at Debug, deduped per path as before), `render` 5 including
the shader-cache/PSO-cache summaries at shutdown. Release run: 49 records, zero Debug-level,
six `[WARN ] [asset] unmipped texture (no DDS sibling, WIC fallback): textures/ocean/*.png`
(real, previously DBWIN-only), cache summaries at Info, clean footer. Domain audit after the
step: zero direct output left in `materials/`, `text/`, `assets/`, `rendering/meshes/`.

**L5d Editor/VFX/Ocean — COMPLETE (2026-09-02).** Five sites in three files; `ocean/` had no
direct event output (its diagnostics are artifacts).

- Core addition used here and by L5c: `logging::WriteRawLines(level, category, text)` — one
  record per line of an already-formatted multi-line blob (trailing CR/space trimmed, empty
  lines skipped). `Material.cpp`'s file-local splitter was replaced by it.
- `EditorPreviewRenderer.cpp` (`editor`, WITH_EDITOR builds): D3DCompile and root-signature
  serialisation errors -> `WriteRawLines(Error)`.
- `ContentBrowserPanel.cpp` (`editor`): "material file <path> was created but could not be
  registered at runtime" Error (the path was missing from the old line).
- `ParticleEmitterObject.cpp` (`vfx`): "emitter maxParticles N > sort cap 1024; back-to-front
  sort disabled" Warning at emitter init; the per-emitter `alive=n/N (rate)` sample Debug,
  still behind BOTH existing gates (`vfx::g_debugAliveLog` opt-in and the one-second
  `logAccum_`).

L5d gate: Debug/Release/Release_Editor build (the editor TUs compile in the first and last).
Debug `--shot` run 392 records / Release 49 records, zero Debug-level in Release, clean
footers; `--log-stress` re-run in Debug after the core addition: 0 failed checks.

**L5 as a whole — direct-output audit of `sources/` after the step:** exactly ONE
`OutputDebugString` call site remains, `Renderer::DiagLog` (`Renderer.cpp`), carrying the L6
comment; zero `printf`-family event output. Every artifact writer next to a migrated line is
untouched (L7).

### L6 — Replace ad-hoc dedupe/throttle and remove hot-path logging hazards

**Depends on:** relevant L5 domains.

**Goal:** delete local logging infrastructure that the central frontend supersedes.

**Touch examples:**

- `Renderer::DiagLog` / `Renderer::DiagLogOnce`;
- `Texture2D::LogTextureResolveOnce`;
- static `loggedOnce` flags used only for output suppression;
- periodic VSM/VFX DBWIN emission;
- renderer state diagnostics evaluated every frame.

**Work:**

- use callsite `ONCE`, explicit subsystem state-change logging or throttling as appropriate;
- do not replace a per-key bounded subsystem cache with one global unbounded dedupe table;
- remove synchronous per-line `fflush` from runtime paths;
- add a microbenchmark to `--log-stress`: filtered call, accepted enqueue and consumer throughput;
- measure with the dev window closed and no log categories raised above defaults.

**Performance gate:** filtered calls perform no allocation/format/timestamp operation; no logging
scope appears as a meaningful `AppController::Tick` or renderer hot-path cost; a 120-frame trace is
within the established run-to-run CPU noise floor. Record measured numbers in this document when
the step is completed.

**Status: COMPLETE (2026-09-02).**

**Implemented result:**

- `Renderer::DiagLog(level, line)` is now only the `barrier_diag.log` artifact writer plus a
  session-log mirror under `render.graph`; the direct `OutputDebugStringA` is gone — the LAST
  one in `sources/`. The per-line `fflush` is KEPT on purpose and documented at the function:
  `--scene-stress` exits through `TerminateProcess`, which discards a buffered CRT tail, and
  "no barrier_diag.log produced" is a documented verdict; the paths only run under their
  diagnostic flags, so the cost is opt-in. Levels: `[flip]` Info, `[flip-miss]` Warning,
  `[barrier-compile]` Info, `[barrier-cache] STALE` Error, `[barrier-cmp]` Warning.
- `Renderer::DiagLogOnce` (a global text-keyed dedupe set) is deleted. Its callers got the
  semantics they actually needed:
  - `--canonical-check`: `Renderer::CanonicalLogOnce` — the same bounded (8192) text-keyed
    cache, but a member of the one subsystem that reports state every frame; no lock because
    only the frame-end thread calls it. The change-gated frame summary no longer needs it.
  - `--barrier-cmp` (`RenderGraph::LogComparator`): the comparator's own bounded set behind
    a mutex (pass bodies run on workers). A per-key bounded subsystem cache, not a global one.
  - The two `[p16]` state lines (`SceneRenderer_Reflections/_Post`): a NEW callsite primitive
    `LOG_<LEVEL>_ONCE_PER_MESSAGE` — emits unless this callsite already emitted the same text
    among its last 16 distinct messages (`detail::MessageHashRing`: 16 atomic FNV-1a hashes,
    no heap). "Once per change since last" was tried first and was WRONG: the scene-peak
    line flips between two stops on an unsettled scene and printed 404 lines in a 700-frame
    run; the ring gives exactly 2. These lines move from `barrier_diag.log` (a sink they were
    only borrowing) to the session log — `grep "\[p16\]" logs/session_*.log`.
- `DlssHandler`: "evaluate failed" no longer borrows the barrier sink; `LOG_WARNING(render)`
  once per backoff window (the existing guard).
- `Texture2D::LogTextureResolveOnce` and its `unordered_set<wstring>` + mutex are deleted. The
  DDS-sibling / unmipped line is emitted where the texture is actually LOADED (the shared-cache
  miss), so a cache hit is silent by construction and a reload after `ClearCache` is a new
  event. `App.cpp`'s `sweepNameReported` flag became `LOG_WARNING_ONCE` (identical semantics:
  the sweep name never changes within a process).
- Crash paths now DRAIN the ring: `StressCrashFilter`, `RendererInvariantFailure` and
  `ReportOnTerminate` call `logging::Flush(2000)` after their emergency record — the writer
  thread is normally still alive, so the frames leading up to the fault reach the file instead
  of dying in the queue. The synchronous-mode branch of `Flush` uses a try-lock so a handler
  running on a thread that died inside the sync render path cannot self-deadlock.
- Kept, as state that gates behaviour rather than output: `rtFailureLogged_` (per scene),
  `asVramLogged_` (per level), `singleDrawFallbackLogged_` (per reason), `s_deviceRemovalReported`
  (also gates the artifact), `g_unboundReported` (a bounded per-slot bitmask), the wind
  8-frame and shadow 5-frame counters.

**Measured (Release x64, wind_test, `--wind-freeze --no-hud --profdump`, 2 consecutive runs
of ~2140 frames each, machine otherwise idle):**

- Session records per run: **50 and 50** — all boot/shutdown; zero records are produced per
  frame in steady state at default thresholds. The only per-frame logging work in the frame
  loop is `SetFrameNumber` (one relaxed atomic store) plus the threshold checks of flag-gated
  diagnostics (0.2-0.7 ns each, L3 microbenchmark).
- `Whole Cycle` avg 1.575 / 1.766 ms, `CPU.Frame` avg 2.847 / 3.336 ms, `Scene::Render`
  1.402 / 1.563 ms — the spread between two identical binaries IS the run-to-run noise floor
  here (~10%); the profiler has no logging scope at all, so nothing to attribute.
- `--log-stress` after the core change (ONCE_PER_MESSAGE, try-lock Flush): 0 failed checks.
- Debug run with `--barrier-cmp --canonical-check`: 7 `render.graph` records, `barrier_diag.log`
  with the same 7 lines (comparator clean), `[p16]` 2 records, 0 dropped.
- Direct-output audit of `sources/`: **zero** `OutputDebugString`, zero `printf`-family event
  output outside `core/logging`.

### L7 — Explicit diagnostic artifact API

**Depends on:** L5; may run in parallel with L6 only if files do not overlap.

**Goal:** preserve machine/human diagnostic reports while removing ambiguous open modes and repeated
directory creation.

**Touch:**

- replace/extend `sources/core/diagnostics/DiagPaths.h`;
- add `ArtifactWriter.h/.cpp` if ownership cannot remain header-only;
- migrate the existing 35 literal destinations in domain-sized substeps.

**API modes:**

```text
PerRunTruncate — first open in this process truncates, later writes append
Append          — explicitly preserve history across sessions
UniqueSession   — unique filename for concurrent processes
AtomicReplace   — write temporary + replace for one complete report
```

**Rules:**

- directory creation is cached and occurs outside hot callbacks;
- keep current root names such as `logs/vsm_pages.log` where docs/scripts depend on them;
- every file declares its mode at the call site; no static local `firstLine` protocol;
- artifact writes may be synchronous because they are explicitly invoked diagnostics, but they
  must not run accidentally each frame;
- write one ordinary log event containing artifact path and success/failure.

**Gate:** run the harness/feature that owns each migrated artifact and compare its content shape;
two same-process writes obey the declared mode; concurrent unique artifacts do not collide;
existing documentation paths remain valid.

**Status: COMPLETE (2026-09-02).**

**Implemented result:**

- `sources/core/diagnostics/ArtifactWriter.h/.cpp`: `diag::ArtifactMode` (the four modes
  above), `diag::ArtifactFile` (RAII, Win32 handle — every `Write`/`Printf` reaches the OS
  immediately, so nothing is lost under `TerminateProcess` and no per-line flush protocol
  exists any more), and the one-shots `WriteArtifact` / `WriteArtifactf`. `EnsureLogDirectory`
  creates `logs/` once per process (function-local static); `diag::LogPath` now calls it
  instead of `std::filesystem::create_directories` on every invocation, and `DiagPaths.h` no
  longer pulls in `<filesystem>`.
- Mode semantics: **PerRunTruncate** keeps a per-process set of opened names — the first open
  truncates, later ones open append-only. **Append** opens append-only and, on the first open
  per process, writes `---- session YYYY-MM-DD HH:MM:SS pid N (<build>) ----`, which answers
  the baseline's "a line has no reliable session identity" for `ibl.log`, `bloom_kernel.log`,
  `unbound_root.log`. **UniqueSession** generates `<stem>_<stamp>_<pid><ext>` with
  `CREATE_NEW` and a numbered retry, so two opens within one second (same process, or a child
  that reuses a pid) cannot clobber each other. **AtomicReplace** writes `<name>.tmp` and
  `MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)` over `<name>` on `Close`.
- One ordinary session event per artifact NAME per process: `artifact logs/<name> (<mode>)`
  at Info on the first successful open, Error on the first failed open (`--log-stress`'s
  session shows the pattern; a run that writes 200 `shadow_casters.log` lines logs it once).
- Migrated (32 of the 35 destinations; mode declared at every call site):
  - PerRunTruncate: `scene_stress.log`, `crash_stack.txt`, `dred_dump.txt`,
    `tasksystem_stress.log`, `renderer_submission_stress.log`, `rt_smoke.log`,
    `log_stress.log`, `boot_profile.log` (the `g_dumpCount` "w"/"a" switch is gone),
    `device_caps.log` (InitDevice truncates; InitQueue, the compute-lane probe and the RT AS
    report append), `material_rootparams.log` (static `first` deleted), `shadow_casters.log`
    (static `started` deleted), `vsm_pages.log` (static `firstLine` deleted),
    `barrier_dump.log` (was "a" across runs; one run's graphs now share one file),
    `submit_order.log`, `barrier_diag.log` (opened on first line, kept open),
    `thumbnail_profile.log` (kept open), `gbv.log`, `barrier_msg_trace.log` (the L4 raw
    handles are `ArtifactFile`s now; the callbacks call `Write`, still no CRT/heap/lock).
  - Append: `invariant_failure.log`, `device_removed.log` (`DumpDredBreadcrumbs` takes an
    `ArtifactFile&`), `fence_stall.log`, `missing_material.log`, `cb_field_missing.log`,
    `unbound_root.log`, `texcreate.log`, `vsm.log`, `ibl.log`, `bloom_kernel.log`,
    `asset_import.log` (the editor's invalidation verdict line).
  - AtomicReplace: `texcache.log`, `shader_cache.log`, `pso_cache.log`, `csm_readout.log`,
    `barrier_census.log`.
- Left on `diag::LogPath` (path only, directory creation cached): the three harness outputs
  that own their `FILE*` (`textmanager_benchmark.csv`, `cull_benchmark.txt`, `rt_smoke.txt`)
  and the importer's own `asset_import.log` stream (`AssetImporter.h` / `ImportPanel.cpp`).
  They truncate per run through their own `fopen`, which is the behaviour they had.
- `--log-stress` gained an `[artifact modes]` scenario (per-run truncate-then-append, append
  separator + lines, atomic replace with no `.tmp` left, two unique opens in one second) and
  the two concurrent session children each write a `UniqueSession` artifact the parent checks
  for distinctness.

**Gate results (2026-09-02):**

- Debug, Release, Release_Editor build (Debug clean on the first attempt, `<filesystem>` gone
  from `DiagPaths.h` without fallout).
- `--log-stress` Debug: 0 failed checks (7 artifact-mode checks + the unique-children check).
- `--tasksystem-stress` 0, `--renderer-submission-stress` 0, `--rt-smoke` 0 (`PASS tier=12`);
  Release `--scene-stress=3 --barrier-census`: `verdict: CLEAN`, exit 0 through
  `TerminateProcess`, `scene_stress.log` complete (14 lines) and `barrier_census.log` written
  (63 lines) — the two files whose tails the CRT buffer used to be able to lose.
- Debug `--shot` with `--dump-barriers --csm-readout --shadow-mode=legacy --barrier-cmp
  --canonical-check`: every artifact above was produced; the session carries 13
  `artifact ... (mode)` events, one per name.
- Content-shape comparison against a snapshot of `logs/` taken before the migration, for the
  20 artifacts present in both: line counts and first-line shape identical for every file
  whose owning run was repeated (differences are run-to-run numbers only; Append files grew by
  exactly the new session's lines plus one separator).

### L8 — ImGui log viewer

**Depends on:** L2 memory sink and L5 useful records.

**Goal:** expose recent logs without adding another permanent per-frame tax.

**Touch:**

- add a dedicated `LogWindow` or a `Log` tab under `DeveloperWindow`;
- core exposes a read-only snapshot/cursor API; core must not include ImGui.

**UI:**

- severity/category filters and text search;
- pause/autoscroll;
- warning/error/dropped counters;
- copy selected/all visible;
- clear view only (never silently delete the session file);
- show source location and exact session path;
- open-folder action may be added only through the existing platform/UI conventions.

**Performance rule:** when the log UI is closed, it performs no snapshot, string copy, filtering or
formatting. When open, it requests only records newer than its last sequence cursor.

**Gate:** Debug and Release_Editor build; 10k-record stress remains responsive; category/search
filters are correct; close-window 120-frame trace shows no `AppController` regression.

**Status: COMPLETE (2026-09-02).**

**Implemented result:**

- `sources/app/ui/LogWindow.h/.cpp`: a dedicated window ("Session Log", **F3**, also a checkbox
  in Developer Controls > Debug and `--log-window` at boot), owned by `DeveloperWindow` like the
  ocean controls and drawn from the same two sites. Core additions for it (no ImGui in core):
  `logging::GetSessionEpoch` (QPC origin + frequency, so the viewer renders session-relative
  time) and `logging::GetThreadName` (try-lock, never blocks).
- Cost rule, literally: `Draw()` is `if (!open_) return;` first — a closed viewer performs no
  snapshot, copy, filter or format. Open, it pulls only records past its cursor
  (`CopyRecentRecords`, 256 per batch), converts each to a compact `Entry` (header fields,
  thread name resolved once at pull, `std::string` message), keeps at most 8192 (twice the core
  ring; the oldest half is dropped in one move), and rebuilds the visible index only when the
  entry set or a filter changed. Rows render through `ImGuiListClipper`.
- UI: Pause (stops pulling; on resume the cursor picks up what the ring still holds and the
  status line says how many records the ring overwrote meanwhile), Auto-scroll, Details pane
  (source file:line, function, full text, flags), Clear view (view only — file and ring
  untouched), Copy visible / Copy selected (the same one-line shape as the session file),
  per-level toggles (coloured), a Categories popup with All/None, case-insensitive message
  search, counters (records/visible, warnings, errors — over all retained entries — and the
  logger's dropped total), the exact session path with Copy path, and "Emit 10k": 10 000 real
  records through the producer path across all levels/categories, the plan's responsiveness
  check reachable from the window itself. Rows and the selected row show the source location
  in a tooltip. No open-folder action: the codebase has no ShellExecute convention to follow.
- Levels colour the whole row (Warning amber, Error red, Fatal magenta, Debug/Trace grey);
  `[truncated]` is flagged inline.

**Gate results (2026-09-02):**

- Debug, Release, Release_Editor build.
- Rendering verified from a headless capture (`--log-window --shot=...`, Debug): the window
  shows toolbar, level toggles, path, counters (314 records, 17 warnings, 0 errors,
  0 dropped), the table with coloured rows, auto-scrolled to the newest records, details
  pane — see the run's PNG; the process exits 0 with a clean footer.
- Release `--profdump` (wind_test, `--wind-freeze --no-hud`, ~2000 frames each):
  - viewer closed: `AppController::Tick` **0.006 ms** avg (L6 baseline before the viewer
    existed: 0.010) — no regression; 59 session records.
  - viewer closed under `--barrier-flip-trace`: **118 777 records in 10 s (~12 000/s,
    ~59 per frame), 0 dropped**, `AppController::Tick` 0.006 ms — the flood itself adds
    ~0.17 ms to `Whole Cycle` (the trace's own formatting), the closed viewer nothing.
  - viewer open under the same flood: `AppController::Tick` **0.069 ms avg, 0.161 max** while
    pulling ~59 records/frame, filtering up to 8192 entries and rendering; 117 950 records,
    0 dropped. That is the "10k-record stress remains responsive" bar, exceeded ten-fold.
- Filter correctness: `Passes()` is one pure function (level toggle AND category toggle AND
  case-insensitive substring); the capture shows the default all-pass state. Toggling them
  interactively is the user's check — it is UI.

### L9 — Retention, enforcement and final cleanup

**Depends on:** L5–L8.

**Goal:** make the architecture difficult to bypass accidentally.

**Work:**

- retain the newest 10 session logs or 100 MiB, whichever limit is hit first;
- deletion targets only validated `logs/session_*.log` files;
- add a repository check that reports new direct `OutputDebugString`, event-style `printf/fprintf`
  and raw `fopen(diag::LogPath(...))` outside an allowlist of artifact/crash code;
- document severity/category rules and CLI switches in the normal developer documentation;
- update this plan with final file list, measured overhead, queue capacity and completed status;
- remove dead helpers/includes and verify no stale UI text points to old log locations.

**Final gate:**

- Debug, Release and editor configurations build;
- `--log-stress` clean under repeated and concurrent runs;
- representative import, screenshot, trace and scene-stress runs preserve exit codes/artifacts;
- forced fatal/device diagnostic leaves both the session tail and specialized artifact;
- no mixed line endings or `git diff --check` failures;
- direct-output audit is empty except documented allowlist;
- clean normal exit footer reports zero unexpected dropped records.

**Status: COMPLETE (2026-09-02). The plan is closed.**

**Implemented result:**

- Retention (`Log.cpp`, at `Initialize`, after the current session file exists): candidates are
  every `logs/session_*.log` (`IsSessionLogName`: the auto-named ones AND explicit `--log-file`
  names that keep the prefix — the owner's rule is "never more than 10 session logs on disk,
  whoever named them"; fixed-name artifacts use other prefixes and can never match). Newest
  first by last-write time (name as tie-break), keep `retainSessionCount` (10) and stop keeping
  once the cumulative size passes `retainSessionBytes` (100 MiB); the current file always
  survives. One Info record reports what was deleted. Verified on a real boot with 14 planted
  2020-stamped files: `deleted 5 older session logs (1000 bytes), keeping 10`; the
  `--log-stress` scenario back-dates its plants with `SetFileTime` and computes its expectations
  from whatever real session logs already sit in the folder.
- `tools/check_logging.py`: scans `sources/` for `OutputDebugString`, event-style
  `printf`/`fprintf(stderr)`/`std::cout|cerr`, `fopen(diag::LogPath(...))` and any
  `diag::LogPath(` outside an allowlist (the logging core, the three harness outputs that own
  their `FILE*`, the importer's stream), ignoring comment text; exit code = findings.
  **0 findings** on the final tree.
- Compact line shape (user request after L8): the sequence number, the date and the elapsed
  clock left the prefix (85 -> ~50 characters); see "Record layout".
- `--log-stress` gained a session-retention scenario (count and byte limits, computed against
  whatever real sessions already sit in `logs/` so a developer's files are neither deleted nor
  counted) and now deletes its own `log_stress_*` scratch files, leaving only the verdict.
- Rules and switches documented in `AGENTS.md` ("Session Log": line shape, levels, gates,
  artifacts, retention, the check script, F3 / `--log-window`).

**Retired artifacts (owner's decision, 2026-09-02).** Twelve fixed-name files folded into the
session log. Eight were pure duplicates once L5 landed (every line already a session record at
Info or above): `texcache.log`, `shader_cache.log`, `pso_cache.log`, `device_caps.log` (the
`[caps]` lines and the RT AS VRAM report), `shadow_casters.log` (the `[ShadowGpuData]` rebuild
summary and cull verdict), `vsm.log` (single-draw OFF), `texcreate.log` (legacy-creation
fallback), `ibl.log` (the `[ibl]` path line; the retained sessions are its history). Four
carried their own content and were translated: `bloom_kernel.log` -> `lens flare ghosts
DISABLED` (Warning) and the `[p8c-2o]` survey line (Info); `unbound_root.log` -> one Error per
(shader, root index), now formatted on the stack and pushed lock-free from the draw path the
old fopen sat on; `material_rootparams.log` -> one Debug record per shader
(`rootparams <file>:<entry>: [i]kind rN sM ...`) instead of a per-run table;
`boot_profile.log` -> `[profiling]` records: header, aggregated work and counters at Info, the
per-scope timeline at Debug (a Release session keeps the summary, a Debug session the whole
report). Writers gone, code comments and the two inspector help texts now point at the
session log, forward-looking docs (`terrain_shadow_chunking_plan.md`,
`vsm_group_cap_removal_plan.md`, `two_sided_foliage_and_ibl_plan.md`,
`photographic_rendering_improvement_plan.md`, `gbv_startup_profile.md`) say "session log
(formerly ...)"; closed historical plans keep their wording. Kept: dumps/tables/crash artifacts
that are not events (`vsm_pages`, `barrier_*`, `submit_order`, `csm_readout`, `dred_dump`,
`crash_stack`, `invariant_failure`, `device_removed`, `fence_stall`, `missing_material`,
`cb_field_missing`, `thumbnail_profile`, the harness verdicts) and the two documented verdict
files (`gbv.log`, `barrier_diag.log`).

**Final gate (2026-09-02):**

- Debug, Release, Release_Editor build.
- `--log-stress` (Debug): two sequential runs 0 failed; two CONCURRENT instances (separate
  working directories, i.e. separate `logs/` — the scratch-heavy scenarios are not meant to
  share one folder, sessions and `UniqueSession` artifacts are and were proven so in L3/L7)
  both 0 failed.
- Exit codes and artifacts preserved: `--import <missing dir>` exits 1 with `=== asset import
  ===` / `FATAL: staging dir does not exist` (Error) / `=== done ===` in the session and in
  `asset_import.log`; `--tasksystem-stress` 0, `--renderer-submission-stress` 0, `--rt-smoke` 0,
  `--scene-stress=3` CLEAN, `--shot` and `--trace` runs as in L5-L8.
- Forced fatal (the `--log-stress` fatal child) leaves the FATAL record, the records queued
  before it, and no footer; `RendererInvariantFailure` keeps its `invariant_failure.log` artifact
  beside the emergency record (L4).
- `tools/check_logging.py`: 0 findings. `git diff --check` clean, no mixed line endings.
- Every normal run's footer: `session end: clean shutdown; ... dropped=0`.

## Final file list

- `sources/core/logging/`: `LogLevel.h`, `LogCategory.h`, `LogRecord.h`, `Log.h/.cpp` (frontend,
  lifecycle, CLI, retention, self-tests), `LogQueue.h/.cpp` (bounded MPSC ring),
  `LogSinks.h/.cpp` (clock, thread names, line shape, file/DBWIN/console/memory sinks),
  `diagnostics/LogStress.h/.cpp` (`--log-stress`).
- `sources/core/diagnostics/`: `ArtifactWriter.h/.cpp` (L7), `DiagPaths.h` (path helper, cached
  directory), `BootProfile.cpp` (migrated writer).
- `sources/app/ui/LogWindow.h/.cpp` (L8 viewer); `tools/check_logging.py` (L9).
- Wiring: `main.cpp` (LogSession RAII, `--log-*`, `--log-stress`, `--log-window`), `App.cpp`,
  `AppController.cpp`, `DeveloperWindow.h/.cpp`, `TaskSystemLockFree.cpp`, `SceneStress.cpp`.
- Migrated call sites across `rendering/`, `materials/`, `assets/`, `editor/`, `vfx/`, `app/`
  (L4-L7; see each step).

## Measured overhead (Release, wind_test, from the gates above)

- Producer: filtered call 0.2-0.7 ns; accepted call 113-174 ns (format + stamp + push);
  0 records per frame at default thresholds in steady state (50 records per ~2100-frame run,
  all boot/shutdown).
- Consumer: 0.9-3.3 M records/s to file; 12 000 records/s sustained under `--barrier-flip-trace`
  with 0 dropped.
- Memory: 8192-slot ring = 8.8 MiB, 4096-record viewer ring = 4.3 MiB, viewer window up to
  8192 compact entries (only while open).
- Viewer: closed 0 (early return; `AppController::Tick` 0.006 ms vs 0.010 before it existed),
  open under 12 k records/s 0.069 ms avg / 0.161 ms max.

## Failure policies

- **Cannot create session file:** continue with DBWIN; one emergency warning, no retry every record.
- **Queue full:** drop without blocking, count by level, DBWIN fallback for Warning+.
- **Record too long:** truncate, set flag, preserve termination and metadata.
- **Formatter failure:** emit a fixed fallback containing category/source; logging cannot throw out.
- **Writer thread failure:** atomically disable asynchronous file sink and use emergency DBWIN.
- **Shutdown timeout:** emit emergency warning and continue process teardown; never wait forever on
  a logger after renderer/task failure.
- **Crash during logging:** recursion guard allows at most one emergency write attempt.

## Expected end state

- One searchable session event log per process.
- No ordinary subsystem opens its own event log or talks to DBWIN directly.
- Specialized diagnostic artifacts remain explicit, stable and script-compatible.
- Errors contain time, category, thread, frame and source context.
- Logging disabled by filters is effectively a branch, not hidden per-frame work.
- A closed log UI costs nothing measurable.
- A crash still leaves evidence even if the normal writer cannot drain.
