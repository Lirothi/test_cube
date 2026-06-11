# TaskSystemLockFree — remaining fixes & speedups (rev 2)

Working prompt for follow-up work on `sources/core/task/TaskSystemLockFree.{h,cpp}`,
from the 2026-06 review. Revised after a second-opinion review (Codex) caught three
errors in rev 1; steps are reordered by priority and renumbered (old numbers in
parentheses).

Already DONE and verified:

- ✅ `WaitForAll` lost wakeup: `FinishTask` notifies under `waitMutex_`, only when
  `outstandingTasks_` hits zero.
- ✅ Completion via `std::atomic<uint32_t> completed_` + C++20 atomic wait, replacing
  `std::promise`/`shared_future` (removed a heap allocation per task recycle and the
  ~1ms-granularity polling in `Wait`).
- ✅ Freelist ABA: pool heads are tagged pointers (48-bit ptr + 16-bit pop counter);
  CAS failure ordering is `acquire`.
- ✅ Step 1: `dependents_` overflow fails fast — `AddDependent` is capacity-checked and
  returns bool; `SetDependencies` aborts on overflow in all build configs.
- ✅ Step 2: `Wait()` branches on `workerIndex_` — main thread helps then blocks on the
  futex; worker context never parks (help, 64x `_mm_pause`, yield, re-check loop).
- ✅ Step 3: `availableTasks_` increments BEFORE the queue push (Schedule and Stop), so
  the counter stays >= the item count and the unsigned wrap is gone.
- ✅ Step 4: `activeTasks_` removed; `WaitForAll` predicate is `outstandingTasks_ == 0`.
  (The inactive TBB backend keeps its own counter — out of scope.)
- ✅ Step 5: `SetDependencies` documents the auto-submit side effect and asserts the
  setup contract; `ParallelForNoHelp` deleted from all three backends. NOTE deviation:
  the dep-side assert is `!scheduled_`, not `!submitted_` — a dep with its own deps is
  legally auto-submitted by its own SetDependencies call before later tasks register
  on it; the race only exists once it can run.
- ✅ Step 6: stress harness in `sources/core/task/TaskSystemStress.{h,cpp}`, run via
  `test_cube.exe --tasksystem-stress` (exit code = failures, details in
  `tasksystem_stress.log`; ~90s Debug / ~50s Release, 50 rounds x 10 scenarios).
  Overflow death test via `--tasksystem-stress --stress-overflow` — the process is
  EXPECTED to abort (exit 3); a 0/100 exit means the fail-fast regressed.
  Baseline: 0 failures in Debug and Release; death test aborts in both.

## How to use

Pick ONE step, implement, build (test_cube.sln, x64, Debug + Release), and verify with:
(1) the stress harness — `test_cube.exe --tasksystem-stress` in Debug AND Release must
exit 0, and `--tasksystem-stress --stress-overflow` must abort (exit 3); (2) a normal
app run ~10s closed via WM_CLOSE with exit code 0 (exercises `Stop()`/`WaitForAll`
from the real frame loop).

Invariants to preserve:

- A waiter must hold a handle reference for the duration of `Wait` — recycling is
  gated on refcount, which is what makes waiting on task members safe.
- `Wait()` runs on the main thread AND on workers: `Pass_CSM` and `Pass_SpotShadows`
  call `DispatchWait` from render-graph pass bodies executing on workers. (Rev 1
  wrongly claimed workers never call `Wait()` — see Step 2.)
- Range tasks waiting for their chunks use the separate `chunkCv_` path with inline
  helping; that path is independent of `Wait()`.
- Tasks scheduled while `running_ == false` execute inline on the caller; `completed_`
  must be set on every path.

## ✅ DONE — Step 1 (was 4) — `dependents_` overflow must fail fast

`tc::inl_vector<TaskHandle, 4>` asserts in Debug only; a 5th dependent in Release is an
out-of-bounds write.

Constraints discovered in review — read before choosing a fix:

- The relevant bound is OUTBOUND fan-out: how many passes name this task in their
  `mtDeps`. It is NOT bounded by `kPassDependencyCapacity` (that caps inbound deps per
  task), and `CreateTask(..., mtDeps.size())` passes the inbound count — the
  `depCapacity` parameter is currently meaningless for `dependents_`. Do not
  static-assert the two capacities against each other; they measure different things.
- "Clamp + log" on overflow is UNSAFE: `SetDependencies` increments the dependent's
  `pendingDeps_` BEFORE `dep->AddDependent(...)`. Dropping the entry leaves a
  dependency that never notifies — the dependent never schedules and its waiter hangs.

Acceptable fixes: (a) fail fast — `AddDependent` reports failure, `SetDependencies`
treats it as a fatal error in all build configs (abort/log-and-terminate; corrupting
memory or hanging are both worse); or (b) a safe container with inline capacity and
heap fallback for the overflow case. Current max fan-out in the render graph is ~2,
so this is a latent footgun, not a live bug — but the graph grows.

## ✅ DONE — Step 2 (new) — worker-context `Wait()` must not block indefinitely

The atomic-wait rewrite made `Wait()` fully blocking once the queue looks empty. For
the main thread that is correct. A WORKER blocked in `completed_.wait()` stops watching
`availableTasks_`, so work pushed after its last failed pop is invisible to it. With
every worker simultaneously blocked in `Wait()` on tasks whose remaining work sits in
the queue, nothing drains it — a deadlock the old 10µs-polling code could not produce.
Today this cannot trigger (CSM and SpotShadows waits are serialized by the graph, so at
most one worker waits at a time), but it is one graph restructure away.

Fix: branch on `workerIndex_` in `Wait()` —

- main thread (`workerIndex_ == invalid`): keep the current help-then-block-on-futex.
- worker context: never fully block; loop { `RunInlineTask()`; if no work, bounded
  `_mm_pause` spin, then a short `completed_.wait`-with-timeout substitute — e.g.
  Windows `WaitOnAddress` with a ~100µs timeout, or a brief `std::this_thread::yield`
  — and re-check the queue }.

## ✅ DONE — Step 3 — `availableTasks_` transient underflow

A worker can pop a task before the producer's `fetch_add` lands (`Schedule` pushes,
then increments), wrapping the `size_t` counter to huge values briefly. Self-corrects,
but sleeping workers that observe "nonzero" busy-spin through failed pops until it does.

Fix options (equivalent strength, pick the cleaner diff):

- producer-side: increment BEFORE the queue push, notify after — a spurious wake just
  re-checks and sleeps again;
- consumer-side: treat the counter as permits — acquire (CAS down, only if > 0) before
  popping, with the producer incrementing after a successful push.

## ✅ DONE — Step 4 (was 1) — remove the redundant `activeTasks_` counter

A task is "outstanding" from `Submit` until `FinishTask`; the active window is strictly
inside that, and each task decrements `activeTasks_` before `outstandingTasks_`. So the
`WaitForAll` predicate only needs `outstandingTasks_ == 0`. Delete `activeTasks_` and
its two RMWs per task in `RunTask`/`FinishTask`.

## ✅ DONE — Step 5 (was part of 6) — contract assertions & documentation

- Comment `SetDependencies`' hidden side effect: it auto-submits the handle when any
  dependency registers, which is why RenderGraph only explicitly submits root passes.
- Assert the contract on BOTH sides during dependency setup: the target handle and
  every dependency must be unsubmitted (`!submitted_`). `dependents_` is
  unsynchronized and `NotifyDependents` iterates it on completion, so registering on a
  submitted (possibly executing) dependency is a data race. True in current usage —
  graph setup is single-threaded and pre-submit — assert it to lock it in.
- `ParallelForNoHelp` is byte-identical to `ParallelFor`. If deleting it, delete it
  from ALL backends (`TaskSystemEnki`, `TaskSystemTBB`, lock-free) in one change.

## ✅ DONE — Step 6 (new) — stress harness

Smoke runs validate "didn't obviously break", not concurrency-correctness; the races
fixed so far live in interleavings a normal frame never produces. Add a small test
target (or a debug-only key-triggered routine) that hammers:

- nested worker-context waits (range tasks dispatched from inside range tasks),
- dependency fan-out at and above the `dependents_` capacity (validates Step 1),
- concurrent create/submit/recycle from many threads (validates the tagged freelists),
- repeated `Start`/`Stop` cycles with in-flight work (validates shutdown paths),
- tiny worker counts (1-2 threads), where help-loops carry all the load.

Run it under the Debug build (assertions live) for minutes, not seconds. Do this
BEFORE the performance steps below — it is the safety net for them.

## Step 7 (was 2) — cache-line padding, benchmark-gated

- `LockFreeQueue`: `head_` and `tail_` share a cache line; producers and consumers
  ping-pong it on every operation. `alignas(64)` each. This is the standard refinement
  of the Vyukov queue and the most likely win.
- `TaskSystem` members: rev 1 claimed `outstandingTasks_`/`availableTasks_` share a
  line — WRONG, they are ~200 bytes apart (mutex/cv/vector between them). The real
  adjacent hot cluster is `workerCount_` / `availableTasks_` / `lambdaPool_` /
  `rangePool_` — four consecutive atomics on one line taking push/pop-notify and
  pool-recycle traffic from all workers. Pad that cluster if measurements justify it.
- Measure with the profiler overlay (F9) before/after; wins show up under contention
  (many small dispatches: PrepareViews culling, CSM cascades).

## Skipped (was 5) — bounded spin before blocking in main-thread `Wait`

Do not add without profiler numbers showing wakeup latency is material. (Worker-context
spinning is covered by Step 2, where it is a correctness measure, not a tuning knob.)

## Known design tradeoffs (documented, no action planned)

- Help-while-waiting can pick up an unrelated long task and delay the waited-on
  dispatch (priority inversion). Unfixable without task priorities in a single queue;
  acceptable at current scale.
- Nested range tasks recurse `Execute -> RunInlineTask -> Execute`, deepening the
  worker stack. Bounded by queue contents in practice.
- The 16-bit freelist tag wraps at 65,536 pops; ABA returns only if exactly that many
  pops occur inside one thread's load-to-CAS window (~11 minutes of stall at current
  rates). Only realistic with a thread frozen in a debugger while the app runs.
  Accepted; if it ever matters, a pool spinlock is the cheap fully-proof fallback.
- Task exceptions are swallowed (the old promise path stored them but no caller ever
  rethrew). If a debugging story is ever needed, add a debug-only log in `RunTask`'s
  catch block.
