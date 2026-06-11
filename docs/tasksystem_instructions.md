# TaskSystemLockFree — remaining fixes & speedups

Working prompt for follow-up work on `sources/core/task/TaskSystemLockFree.{h,cpp}`,
from the 2026-06 review. The three priority items are already DONE and verified:

- ✅ `WaitForAll` lost wakeup: `FinishTask` now notifies under `waitMutex_`, only when
  `outstandingTasks_` hits zero.
- ✅ Completion via `std::atomic<uint32_t> completed_` + C++20 atomic wait, replacing
  `std::promise`/`shared_future` (removed a heap allocation per task recycle and the
  ~1ms-granularity polling in `Wait`).
- ✅ Freelist ABA: pool heads are tagged pointers (48-bit ptr + 16-bit pop counter);
  CAS failure ordering is now `acquire`.

## How to use

Pick ONE step, implement, build (test_cube.sln, x64, Debug + Release), and verify with:
run ~10s (the task system is exercised every frame by the render graph, culling, and
object ticks), then close the window via WM_CLOSE and confirm exit code 0 — this
exercises `Stop()`/`WaitForAll`. Repeat the close cycle 3x; the shutdown races are rare.

Invariants to preserve:
- A waiter must hold a handle reference for the duration of `Wait` — recycling is
  gated on refcount, which is what makes waiting on task members safe.
- `Wait()` is called from the main thread (render graph, tracked frame tasks); range
  tasks waiting for their chunks use the separate `chunkCv_` path. Workers never call
  `Wait()` — keep it that way, or re-audit the blocking wait for worker starvation.
- Tasks scheduled while `running_ == false` execute inline on the caller; `completed_`
  must be set on every path.

## Step 1 — Remove the redundant `activeTasks_` counter

A task is "outstanding" from `Submit` until `FinishTask`; the active window is strictly
inside that, and each task decrements `activeTasks_` before `outstandingTasks_`. So the
`WaitForAll` predicate only needs `outstandingTasks_ == 0`. Delete `activeTasks_` and
its two RMWs per task in `RunTask`/`FinishTask`.

## Step 2 — Cache-line padding (false sharing)

- `LockFreeQueue`: `head_` and `tail_` share a cache line, so producers and consumers
  ping-pong it on every operation. `alignas(64)` on each (and keep them away from
  `buffer_`'s control fields). This is the standard refinement of the Vyukov queue.
- `TaskSystem`: `outstandingTasks_` / `availableTasks_` (and the pool heads) likely sit
  on one line; every completion hammers it from all workers. Pad similarly.
- Measure with the profiler overlay (F9) before/after; wins show up under contention
  (many small dispatches: PrepareViews culling, CSM cascades).

## Step 3 — `availableTasks_` transient underflow

A worker can pop a task before the producer's `fetch_add` lands (`Schedule` pushes,
then increments), wrapping the `size_t` counter to huge values briefly. Self-corrects,
but sleeping workers that observe "nonzero" busy-spin through failed pops until it does.
Fix options: make the counter signed (`std::atomic<std::ptrdiff_t>`, wait on <= 0), or
increment BEFORE the queue push and notify after (then a spurious wake just re-checks).

## Step 4 — `dependents_` overflow must fail loudly in Release

`tc::inl_vector<TaskHandle, 4>` asserts in Debug only; a 5th dependent in Release is an
out-of-bounds write. Current render graph peaks at ~2 dependents per task, but the graph
grows. Either bump capacity with a static_assert-style guard at the RenderGraph call
site (`kPassDependencyCapacity` must stay <= dependents capacity), or make
`AddDependent` clamp + log in Release instead of corrupting memory.

## Step 5 — Optional: bounded spin before blocking in `Wait`

`Wait` now blocks on the futex-backed atomic immediately when no inline work exists.
For very short tasks (CSM cascade lists) a short `_mm_pause` spin (~200-400 iterations)
before `completed_.wait(0)` can shave wakeup latency. Only do this with profiler
numbers showing `Wait` wakeup latency matters — the earlier commented-out experiments
in this function suggest results were inconclusive.

## Step 6 — API cleanups (no behavior change)

- Delete `ParallelForNoHelp` or implement it honestly — it is currently byte-identical
  to `ParallelFor` (both help via `DispatchWait`).
- Comment `SetDependencies`' hidden side effect: it auto-submits the handle when any
  dependency registers, which is why RenderGraph only explicitly submits root passes.
- `TaskWithDeps::AddDependent` is only safe before the dependency is submitted
  (`dependents_` is unsynchronized; `NotifyDependents` iterates it on completion).
  True in current usage (graph setup is single-threaded, pre-submit) — assert
  `!submitted_` in `AddDependent` to lock the contract in.

## Known design tradeoffs (documented, no action planned)

- Help-while-waiting can pick up an unrelated long task and delay the waited-on
  dispatch (priority inversion). Unfixable without task priorities in a single queue;
  acceptable at current scale.
- Nested range tasks recurse `Execute -> RunInlineTask -> Execute`, deepening the
  worker stack. Bounded by queue contents in practice.
- Task exceptions are swallowed (the old promise path stored them but no caller ever
  rethrew). If a debugging story is ever needed, add a debug-only log in `RunTask`'s
  catch block.
