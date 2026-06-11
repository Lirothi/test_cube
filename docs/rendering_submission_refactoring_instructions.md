# Renderer Submission and Resource-State Refactoring

> PRECEDENCE: `docs/renderer_submission_instructions.md` is the operative plan.
> Iterations 0-4 of THIS document are superseded by its steps 1-4 and must not be
> executed from here. Iterations 5-9 (graph-compiled barrier plans, CommandContext,
> tracker removal) remain valid as the spec for the PARKED opt-in program described
> there, and require an explicit user decision to start. Rule 11 below applies only
> within that parked program.

This document is a step-by-step implementation plan for improving parallel
D3D12 command-list recording, submission ordering, and resource-state handling.

The intended destination is:

- Render passes and expensive pass chunks record directly into native D3D12
  command lists in parallel.
- Submission order is deterministic and derived from the render graph, not from
  worker completion order.
- The render graph compiles cross-pass transitions before recording begins.
- A small per-command-list context handles transitions that occur inside a pass.
- The current lane/TLS-based `ResourceStateTracker` is removed after migration.

A full custom command stream that replays all rendering calls into one D3D12
command list is not part of this plan. It would serialize driver calls, duplicate
the D3D12 API surface, complicate third-party integrations such as Streamline,
and remove much of the benefit of parallel recording.

## Scope

Primary files:

- `sources/rendering/core/RenderGraph.h`
- `sources/rendering/core/Renderer.h`
- `sources/rendering/core/Renderer.cpp`
- `sources/rendering/core/ResourceStateTracker.h`
- `sources/rendering/core/ResourceStateTracker.cpp`
- `sources/rendering/core/FrameResource.h`
- `sources/app/scene/SceneRenderer.cpp`

Related files may be changed when migrating resource declarations or raw command
list use, but each iteration must remain narrowly scoped.

Do not combine this work with task-system refactoring. The renderer depends on
the task system, so scheduler changes make failures harder to attribute.

Out of scope for Iterations 0-9:

- eager GPU submission before `ExecuteTimelineAndPresent`;
- changing the number or placement of `ExecuteCommandLists` queue calls except
  as required to submit the ordered acquire/work/epilogue list sequence;
- merging pass command lists;
- replacing native command lists with a custom replay stream;
- adding compute or copy queue execution;
- changing render-pass algorithms, shaders, or visual output.

## AI Execution Protocol

This document is intended to be executable by a coding agent. Follow these rules
exactly:

1. Execute only the iteration explicitly requested by the user.
2. If the user does not name an iteration, inspect the code and recommend the
   next iteration; do not edit files.
3. Stop after the requested iteration. Never begin the following iteration
   automatically.
4. Before editing, inspect the current implementation and `git status`. Work
   with pre-existing changes and never revert unrelated changes.
5. Keep edits limited to the requested iteration and its required tests,
   instrumentation, or documentation.
6. Do not make commits, create branches, or remove feature flags unless the user
   explicitly requests it.
7. Do not claim a check passed unless it was actually run and its result was
   observed. Report every check as `PASS`, `FAIL`, or `NOT RUN`.
8. If a required automated check fails, diagnose and fix it within the requested
   iteration. Do not proceed to later iterations.
9. If a manual or interactive check cannot be performed, report it as
   `NOT RUN`; do not treat it as passing.
10. When architecture described here conflicts with the current code, stop and
    report the conflict rather than silently inventing a replacement design.
11. Treat this file as authoritative for its iterations. Do not combine steps
    from another planning document unless the user explicitly requests it.

At the end of an iteration, report:

- iteration completed;
- changed files and behavior;
- automated checks with exact commands and results;
- manual checks as `PASS`, `FAIL`, or `NOT RUN`;
- before/after measurements, or `NOT RUN`;
- remaining risks and feature flags left in place.

Iteration prerequisites:

- Before implementing Iteration N, verify that every earlier iteration's
  implementation exists and its required automated checks most recently passed.
- Missing earlier work is a blocker. Report it and stop; do not silently
  backfill multiple iterations.
- Earlier manual or performance checks reported as `NOT RUN` do not block
  implementation of the next iteration, but all behavior-changing flags remain
  disabled by default.
- Iteration 9 additionally requires every earlier manual and performance check
  to report `PASS`.

## Failure Semantics

Use these failure rules consistently:

- When first needed, add one shared
  `[[noreturn]] RendererInvariantFailure(const char* message)` helper under
  `sources/rendering/core/`. It must call `OutputDebugStringA(message)` and then
  `std::abort()`. Reuse it; do not create iteration-specific fatal helpers.
- Renderer invariant violations that would otherwise lose, duplicate, corrupt,
  or incorrectly order GPU work must write a diagnostic with
  `RendererInvariantFailure` in every build configuration.
- Do not use `assert` as the only protection for a runtime invariant.
- Do not throw exceptions from render-pass worker tasks. The active lock-free
  task system swallows task exceptions.
- Graph compilation and declaration-validation failures occur before recording
  tasks are submitted. During migration they return a structured error, log it,
  and select the legacy path for that frame.
- A structured graph error must contain an error code, pass identity, resource
  pointer when applicable, and diagnostic message.
- After the legacy path is removed, a graph compilation failure must log and
  abort before any recording task is submitted.
- HRESULT failures continue to use the project's existing `ThrowIfFailed`
  behavior only from contexts where the exception cannot be swallowed by a
  render worker task.

## Initial Supported D3D12 Model

Do not expand the synchronization model while implementing Iterations 0-9.
The initial graph compiler supports:

- the direct queue only;
- legacy `D3D12_RESOURCE_BARRIER` barriers only;
- whole-resource tracking with
  `D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES` only;
- exact declared D3D12 states, without automatically combining compatible
  read-only state bitmasks;
- transition barriers between submission batches;
- a UAV barrier when the previous access to a resource exits in
  `UNORDERED_ACCESS`, the next access enters in `UNORDERED_ACCESS`, and the
  previous access declares `ReadWrite`;
- no aliasing barriers, split barriers, enhanced barriers, cross-queue
  ownership, or automatic subresource hazard resolution.

A submission batch is one synchronization unit. The graph compiler does not
insert barriers between lists inside a batch. A multi-list pass must guarantee
that every list can execute in deterministic local order without an inter-list
transition. Internal transitions and UAV ordering inside the batch remain the
pass's responsibility. Do not add inputs for unsupported cases during
Iterations 0-9. Detectable violations of this model must fail graph validation
before recording begins; do not guess.

This batch-unit restriction applies to graph-compiled barriers. Iteration 4's
temporary legacy-compatible path still preserves per-list acquire barriers.

## Feature Flag Policy

Place renderer migration flags in one file:

`sources/rendering/core/RendererFeatureFlags.h`

Create this header in Iteration 3 with all of the following compile-time
`inline constexpr bool` flags set to `false`. Later iterations reuse it:

- `kRendererUseDeterministicSubmissionOrder`
- `kRendererUseDedicatedAcquireLists`
- `kRendererValidateCompiledBarrierPlans`
- `kRendererUseCompiledBarrierPlans`
- `kRendererUseCommandContext`

All flags default to `false` when introduced. Do not add per-pass preprocessor
macros. During Iteration 7, use one centralized `RenderPass` allowlist for
compiled-barrier migration. Remove a flag only after its replacement path passes
the full validation loop and the user explicitly requests removal.

Add compile-time dependency checks in the same header:

```cpp
static_assert(!kRendererUseDedicatedAcquireLists ||
              kRendererUseDeterministicSubmissionOrder);
static_assert(!kRendererValidateCompiledBarrierPlans ||
              kRendererUseDeterministicSubmissionOrder);
static_assert(!kRendererUseCompiledBarrierPlans ||
              kRendererValidateCompiledBarrierPlans);
static_assert(!kRendererUseCommandContext ||
              kRendererUseCompiledBarrierPlans);
```

The relevant new path must be exercised by automated tests even while its flag
defaults to `false`. For manual runtime validation, enable the requested
iteration's flag and only the prerequisite flags required by that path; do not
enable flags from later iterations. Leave a flag enabled by default only when
every required automated, manual, and performance check passes; otherwise leave
it disabled and report the failure or unavailable checks.

## Current Design Facts

- `RenderGraph::ExecuteParallel` creates one ordered submission batch per graph
  pass.
- Passes record native D3D12 command lists on task-system workers.
- Expensive pass bodies may add multiple direct lists or bundles to one batch.
- Lists inside a batch are currently stored in worker completion order.
- `ResourceStateTracker` records each command list's first-use and final states.
- At submit time, transitions are appended between native command lists.
- `PassBatch_::directs` and `PassBatch_::bundles` currently have a fixed capacity
  of eight, but runtime chunk counts can exceed eight.
- `ResourceStateTracker::ResetLanesForFrame()` clears entries while worker TLS
  can retain pointers to those entries.
- Lane allocation clamps every thread after lane 63 onto the same unsynchronized
  lane.

## Invariants

Preserve these invariants throughout the refactor:

1. A command allocator is not reset until the GPU has completed every command
   list recorded with it.
2. Every submitted command list is closed exactly once before
   `ExecuteCommandLists`.
3. A resource transition must execute before the first command that requires
   the destination state.
4. Internal pass transitions, UAV barriers, copies, and read/write ordering must
   remain in their original command order.
5. Throughout Iterations 0-9, every batch uses deterministic local list order.
   Any future arbitrary-order policy is outside this plan and requires an
   explicit user request.
6. Resource declarations describe GPU access and cannot depend on worker
   completion order.
7. Raw resource pointers, descriptor handles, upload allocations, and captured
   objects must remain alive until the GPU has consumed the submitted lists.
8. Debug and Release builds must behave consistently. Fixed-capacity overflow
   must never depend only on `assert`.

## Validation Loop For Every Iteration

Implement only one iteration at a time. Do not begin the next iteration until
the current one passes all applicable checks.

Required automated checks:

```powershell
git diff --check
msbuild test_cube.sln /m /p:Configuration=Debug /p:Platform=x64
msbuild test_cube.sln /m /p:Configuration=Release /p:Platform=x64
x64\Debug\test_cube.exe --tasksystem-stress
x64\Release\test_cube.exe --tasksystem-stress
```

For Iterations 1-9, also run:

```powershell
x64\Debug\test_cube.exe --renderer-submission-stress
x64\Release\test_cube.exe --renderer-submission-stress
```

Expected result:

- `git diff --check` produces no output and exits with code 0.
- Both builds exit with code 0. Record warnings in the final report and fail the
  check on any new warning attributable to the iteration.
- Both task-system stress runs exit with code 0. These runs do not validate
  rendering, but they detect accidental damage to the worker execution used by
  parallel recording.
- For Iterations 1-9, both renderer-submission stress runs exit with code 0.

When an iteration introduces CPU-only planning or validation logic, add focused
automated tests for it in the same iteration. The tests must cover successful
plans and every newly introduced rejection path. Use a command-line stress or
validation mode that returns 0 on success and nonzero on failure.

Manual or interactive runtime checks:

1. Run the Debug executable with the D3D12 debug layer enabled.
2. Render for at least 60 seconds while moving the camera through the demo
   scene.
3. Resize the window repeatedly.
4. Toggle DLSS with `F6`, FXAA with `F7`, and the profiler overlay with `F9`.
5. Exercise material hot reload, render-resolution changes, debug drawing,
   transparent objects, and shadows. Report any unavailable operation as a
   separate `NOT RUN` manual subcheck.
6. Inspect debugger output for new D3D12 errors, corruption messages, and
   resource-state warnings.
7. Close through the normal window-close path and confirm exit code 0.
8. Repeat the start, render, resize, and close cycle three times in Debug and
   three times in Release.

These checks are manual unless the active environment can control the native
window, capture debugger output, and close it through `WM_CLOSE`. Killing the
process does not count as a passing shutdown test.

Reject the iteration on any new D3D12 debug error, resource-state warning,
device removal, deadlock, missing pass, flicker, or shutdown failure.

An iteration may be reported as implementation-complete when all required
automated checks pass and unavailable manual or performance checks are reported
as `NOT RUN`. It is not fully validated, and its behavior-changing feature flag
must remain disabled by default, until the manual and performance checks pass.

Performance procedure:

1. For Iteration 0, record only the baseline. For later iterations, use the same
   executable configuration, scene, camera position, window size,
   render-resolution scale, DLSS mode, and profiler settings for baseline and
   candidate.
2. Warm up for 300 frames.
3. Capture the following 1,000 frames.
4. Repeat five times.
5. Record the median of run medians and the worst run p95 in
   `docs/rendering_submission_baseline.md`.
6. Record:
   - total CPU frame time;
   - `Renderer::BeginThreadCommandList`;
   - `Renderer::EndThreadCommandList`;
   - `Renderer::ExecuteTimelineAndPresent`;
   - submission gathering, barrier planning, queue submission, and present;
   - direct-list, bundle, acquire-list, and transition counts;
   - GPU frame time and pass timing.
7. Treat a greater than 3% median CPU-frame regression, greater than 5% p95
   CPU-frame regression, or greater than 3% median GPU-frame regression as a
   failed performance check unless the user explicitly accepts it.

If this procedure cannot be performed, report performance as `NOT RUN` and make
no performance claim.

Required final validation report:

```text
Automated:
- git diff --check: PASS|FAIL
- Debug x64 build: PASS|FAIL
- Release x64 build: PASS|FAIL
- Debug task-system stress: PASS|FAIL|NOT RUN
- Release task-system stress: PASS|FAIL|NOT RUN
- Debug renderer-submission stress: PASS|FAIL|NOT RUN
- Release renderer-submission stress: PASS|FAIL|NOT RUN
- other iteration-specific tests: PASS|FAIL|NOT RUN

Manual:
- Debug runtime and D3D12 validation: PASS|FAIL|NOT RUN
- Release runtime: PASS|FAIL|NOT RUN
- repeated WM_CLOSE shutdown: PASS|FAIL|NOT RUN

Performance:
- baseline/candidate comparison: PASS|FAIL|NOT RUN
```

Legacy wording such as "representative," "measurement noise," or "appears
stable" is not an acceptance result.

Use a feature flag for every behavior-changing migration. Keep the old path
available until the replacement has passed the full validation loop.

## Iteration 0: Establish a Baseline

Goal: make later changes measurable and attributable.

Steps:

1. Add per-frame counters for:
   - graph pass count;
   - submission batch count;
   - direct-list and bundle count per batch;
   - maximum direct-list and bundle count in any batch;
   - transition and UAV barrier count;
   - lists that have no tracked state;
   - state-tracker lane count.
2. Display or log the counters through the existing profiler infrastructure.
3. Capture Debug and Release measurements using the Performance procedure.
4. After the 300-frame warmup, capture one GPU debugging frame of the demo scene
   with the profiler overlay disabled if PIX or an equivalent tool is available.
   Otherwise report this check as `NOT RUN`.
5. Document the observed maximum list count per batch. Do not assume eight is
   sufficient.

Acceptance criteria:

- Instrumentation does not change submission order or resource states.
- Every required automated check reports `PASS`.
- Manual checks are reported as `PASS`, `FAIL`, or `NOT RUN`.
- Baseline numbers and exact capture settings are saved in
  `docs/rendering_submission_baseline.md`.

## Iteration 1: Remove Fixed-Capacity Submission Corruption

Goal: make pass fan-out safe before changing the architecture.

Steps:

1. Replace `PassBatch_::directs` and `PassBatch_::bundles` with `std::vector`.
2. Reserve a measured typical capacity when creating a batch to avoid repeated
   allocations.
3. Add explicit invariant checks for:
   - a null list;
   - a list registered more than once;
   - a batch index outside `submitTimeline_`.
4. On violation, log the offending batch/list and call
   `RendererInvariantFailure`. Do not throw, return success, or silently discard
   a command list.
5. Add `test_cube.exe --renderer-submission-stress`, a CPU-only command-line
   mode that verifies:
   - more than eight ordered list registrations are retained;
   - duplicate registration is detected;
   - an invalid batch index is detected;
   - deterministic gathering retains every registered item.
   Death-test cases must run in a child process so the parent returns 0 only
   when the expected abort occurs.
6. Add counters for batch growth and maximum batch size.

Acceptance criteria:

- `--renderer-submission-stress` exits with code 0 in Debug and Release.
- A runtime scene producing more than eight lists in one batch does not corrupt
  memory or drop work.
- The full validation report contains no `FAIL`.

Rollback rule:

- If list ownership remains ambiguous or a command list can be registered in
  multiple batches, leave the new path disabled and report the blocker. Do not
  use destructive git commands or revert unrelated changes.

## Iteration 2: Make The Existing State Tracker Memory-Safe

Goal: stabilize the current tracker before using it as a temporary differential
comparison signal.

Steps:

1. Remove raw `CLStateEntry*` caching from TLS entirely. A pointer into
   `robin_hood::unordered_flat_map` can be invalidated by insertion or rehash
   during the same epoch, not only by frame reset.
2. TLS may cache only stable values: lane identity, current command-list key,
   and observed lane epoch. Resolve the entry by key after validating the epoch.
   Do not cache pointers or references into the flat map.
3. Remove lane-63 clamping. Multiple threads must not mutate the same lane
   without synchronization.
4. Replace the fixed lane array with
   `std::vector<std::unique_ptr<CLStateLane>>`. Protect lane creation with one
   mutex. After assignment, one recording thread exclusively owns that lane and
   records without the creation mutex. TLS may store the tracker owner and the
   stable heap-allocated `CLStateLane*`; it must never store a pointer or
   reference into a lane's flat map.
5. If lane allocation fails, call `RendererInvariantFailure`; do not share a
   lane.
6. Add Debug validation that a command list is only recorded by one thread at a
   time.
7. Add Debug validation that a tracked command list belongs to the current
   frame epoch.
8. Track active recording scopes in Debug and require zero active recorders
   before `ResetLanesForFrame` clears entries.
9. Extend `--renderer-submission-stress` to cover:
   - repeated lane resets followed by reuse;
   - enough inserts to force flat-map rehash;
   - more than 64 participating threads;
   - repeated registration of different command-list keys on one thread.
10. Keep submit-phase state access single-threaded and state that contract in the
   interface.

Acceptance criteria:

- Repeated frame resets cannot dereference cleared map entries.
- High-thread-count stress cannot cause two recording threads to mutate one lane
  unsafely.
- `--renderer-submission-stress` exits with code 0 in Debug and Release.
- Existing rendering and transition behavior remains unchanged.

## Iteration 3: Make Submission Order Deterministic

Goal: stop using worker completion order as GPU execution order.

Steps:

1. Add `kRendererUseDeterministicSubmissionOrder`.
2. Introduce a submission token created before recording begins:

   ```cpp
   struct SubmissionToken {
       size_t batchIndex;
       uint32_t localOrder;
   };
   ```

3. Change driver, direct-list, and bundle registration to include a token.
4. Use two independent local-order namespaces per batch:
   - work-list order contains the driver and ordinary direct lists;
   - bundle order contains bundles executed inside the driver.
5. Preserve the current driver-first behavior by assigning the driver work-list
   order 0 and assigning ordinary direct lists from 1 upward in a batch that has
   a driver. If bundles exist without a registered driver, the submit-time
   fallback driver receives work-list order 0 and ordinary direct lists start at
   1. In a batch with neither driver nor bundles, direct-list order starts at 0.
6. Assign bundle order from chunk or input order before jobs are dispatched.
   Assign ordinary direct-list order from a fixed pass slot or chunk index
   before dispatch. Do not derive any order from registration timing or an
   atomic completion counter.
7. Sort work lists and bundles by their respective `localOrder` before
   submission/finalization.
8. Duplicate local-order values within either namespace call
   `RendererInvariantFailure`.
9. Preserve input order for transparent rendering and any pass where draw order
   affects blending or correctness.
10. Use stable local order for every batch in the initial implementation. Do not
   add an unordered or reorderable path in this iteration.
11. Extend `--renderer-submission-stress` to register drivers, directs, and
    bundles from randomized thread completion order and verify identical driver,
    work-list, and bundle order across runs.

Acceptance criteria:

- The same scene produces the same command-list order across repeated runs.
- `--renderer-submission-stress` exits with code 0 in Debug and Release.
- Transparent and shadow rendering remain correct.
- Parallel recording remains enabled.

## Iteration 4: Close Native Command Lists At Recording Completion

Goal: give command-list ownership a clear lifecycle before changing barrier
insertion.

The current path leaves direct command lists open so submit code can append the
next list's acquire barriers to the preceding list. The temporary migration path
must preserve those per-list barrier positions while allowing ordinary recorded
work lists to close immediately.

Do not aggregate all first-use transitions into one batch prologue. A later list
inside the same batch may require a transition from an earlier list's final
state. One batch prologue cannot represent that transition.

Bundle drivers are a deliberate exception during this iteration. They currently
remain open until submit time so sorted bundles can be appended with
`ExecuteBundle`. Give them an explicit driver lifecycle rather than treating
them as ordinary work lists.

Steps:

1. Add `kRendererUseDedicatedAcquireLists`.
2. Under the new path, close every ordinary direct command list in
   `EndThreadCommandList`. Because this can run inside a worker task, check the
   `Close()` HRESULT explicitly and call `RendererInvariantFailure` on failure;
   do not throw.
3. Store closed lists in deterministic batch order.
4. Keep bundles closed at recording completion. Check bundle `Close()` failures
   with `RendererInvariantFailure` because bundle recording also runs on workers.
5. At submit time, append sorted bundles to each bundle driver, close the driver
   exactly once, and insert the closed driver into the deterministic work-list
   sequence for its batch.
6. Walk the now-closed work lists in deterministic submission order and
   preserve the existing
   state-tracker algorithm:
   - apply the previous work list's final states;
   - calculate the current work list's acquire barriers;
   - when barriers exist, record them into a dedicated acquire command list;
   - submit that acquire list immediately before the current work list.
7. Never move a current work list's acquire barriers earlier than the final
   state of the preceding work list.
8. Apply tracker-observed final states in deterministic work-list order.
9. Avoid creating an acquire list when the current work list needs no barriers.
10. Record the backbuffer present transition and GPU-frame-end commands into one
    dedicated frame-epilogue direct list submitted after every work list. Do not
    reopen or append to the final work list. Commit the backbuffer's known state
    as `PRESENT` after planning the epilogue.
11. Extend `--renderer-submission-stress` with:
    - a pure planning sequence
    `UAV -> SRV -> UAV`. Verify that the second UAV acquire barrier is placed
      between the SRV-ending list and the later UAV list, not at batch start;
    - bundle-driver finalization order;
    - frame-epilogue placement after the final work list.
12. Do not remove the old path yet.

Acceptance criteria:

- Every ordinary work list has one clear owner and is closed exactly once at
  recording completion.
- Every bundle driver has one clear owner and is closed exactly once after its
  bundles are appended.
- No code appends commands to a closed work list.
- Acquire barriers execute immediately before the work list that requires them.
- The dedicated frame epilogue executes last and contains the present transition
  and GPU-frame-end commands.
- `--renderer-submission-stress` exits with code 0 in Debug and Release.
- Extra acquire-list CPU/GPU cost is measured and documented.

Rollback rule:

- Keep the old append-to-previous-list path if dedicated acquire lists fail the
  Performance procedure. The later graph compiler can still emit barriers
  through a different native-list strategy. Leave
  `kRendererUseDedicatedAcquireLists` false and report the measurements; do not
  remove the implementation or revert unrelated changes.

## Iteration 5: Define Render-Graph Resource Usage

Goal: describe cross-pass synchronization independently of command-list
recording.

Introduce exactly this initial usage description:

```cpp
enum class ResourceAccess {
    Read,
    Write,
    ReadWrite
};

struct PassResourceUsage {
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES entryState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES exitState = D3D12_RESOURCE_STATE_COMMON;
    ResourceAccess access = ResourceAccess::Read;
};
```

Steps:

1. Replace or extend `ResourceStateDecl` with explicit access information.
2. Track whole resources only. Do not add subresource fields in this iteration.
3. Require an explicit exit state. For most uses it equals the entry state, but
   a pass with internal transitions must declare the state left for the next
   pass.
4. Distinguish pass-entry and pass-exit states from transitions that occur
   inside a pass.
5. Use exact state equality in the initial implementation. Do not combine
   read-only bitmasks or add enhanced-barrier semantics.
6. Validate `entryState` access using these rules:
   - `Read`: the state is nonzero and contains only `COPY_SOURCE`, `DEPTH_READ`,
     `GENERIC_READ`, `INDEX_BUFFER`, `NON_PIXEL_SHADER_RESOURCE`,
     `PIXEL_SHADER_RESOURCE`, or `VERTEX_AND_CONSTANT_BUFFER` bits;
   - `Write`: the state is exactly `COPY_DEST`, `DEPTH_WRITE`, or
     `RENDER_TARGET`;
   - `ReadWrite`: the state is exactly `UNORDERED_ACCESS`.
   `entryState` may not be `COMMON` or `PRESENT`.
   `exitState` must be one of: `COMMON`, `PRESENT`, an allowed `Read` state or
   explicitly declared combination of allowed read bits, an allowed `Write`
   state, or `UNORDERED_ACCESS`.
7. For resource-conflict validation, graph ordering means a path in the
   `prereqs` DAG, not merely deterministic insertion order and not `mtDeps`.
8. Add validation for:
   - a null declared resource;
   - duplicate declarations for one resource in one pass: collapse them only
     when `entryState`, `exitState`, and `access` are identical; otherwise
     return a structured error;
   - two passes that share a resource: when either access is `Write` or
     `ReadWrite`, require a `prereqs` path in one direction; two `Read` accesses
     require no path;
   - states incompatible with the declared access.
9. Return a structured validation error before tasks are submitted. During
   migration, log the error and execute the legacy path for that frame.
10. Extend `--renderer-submission-stress` with CPU-only declaration tests for
   every acceptance and rejection rule.
11. In the initial Iteration 5 request, migrate exactly one simple named pass to
    prove the declaration API without changing execution. Treat every remaining
    pass declaration migration as a separately requested Iteration 5
    sub-iteration.

Important:

- Pass-level usage applies to the whole submission batch, including chunk lists
  and bundles created by that pass.
- Internal ping-pong transitions, copies, and UAV barriers remain explicit
  commands inside the pass.
- Third-party work that requires a real command list remains native D3D12 work.
- Because the initial model tracks whole resources, a pass that requires
  subresource-specific synchronization is unsupported until Iteration 10.
- Iteration 5 cannot prove that lists inside a multi-list batch are internally
  barrier-independent. Treat that as a pass-author contract here; detect
  mismatches through shadow comparison in Iteration 6 and enforce final local
  states with `CommandContext` in Iteration 8.

Acceptance criteria:

- `--renderer-submission-stress` exits with code 0 in Debug and Release.
- Invalid resource relationships produce a structured error before recording
  begins and select the legacy path.
- Existing rendering still uses the old tracker for actual barriers.

## Iteration 6: Compile Barrier Plans In Shadow Mode

Goal: prove the graph has enough information before it controls execution.

Prerequisite: every parallel render-graph pass has a complete Iteration 5 usage
declaration. If any pass is incomplete, report the pass names and stop.

Steps:

1. Add a graph compilation phase before recording tasks are submitted, guarded
   by `kRendererValidateCompiledBarrierPlans`.
2. Walk passes in final deterministic submission order.
3. Starting from the global known state, calculate each batch's required
   prologue transitions, UAV ordering barriers, and final states.
4. Emit barriers in pass resource-declaration order. Do not expose unordered-map
   iteration order in the compiled plan.
5. Record the compiled plan without executing it.
6. Compare the compiled plan against the transitions inferred by the current
   `ResourceStateTracker` as a differential signal only. Compare batch-entry
   acquire requirements and batch-exit states after folding every list in the
   batch in deterministic local order; do not compare graph plans against
   individual internal-list transitions.
7. If the legacy tracker requires a non-empty acquire transition between two
   work lists inside one batch, mark that batch unsupported for compiled
   barriers and return a structured validation error. Do not move that
   transition to batch entry.
8. Log mismatches with:
   - pass name;
   - resource name or pointer;
   - expected before/after state;
   - current tracker before/after state.
9. Extend `--renderer-submission-stress` with deterministic planner cases:
   - no transition when exact states match;
   - transition when exact states differ;
   - UAV barrier for consecutive same-state UAV accesses after `ReadWrite`;
   - no UAV barrier between consecutive same-state read-only SRV accesses;
   - structured failure when a legacy per-list acquire barrier is required
     inside a batch;
   - compile the same input 100 times and verify identical ordered plan entries
     regardless of randomized worker completion order.
10. Fix missing or inaccurate pass declarations until the automated planner
   cases pass and runtime differential logs contain zero cross-pass mismatches.
   Do not add a mismatch allowlist without explicit user approval.

Do not switch execution to the compiled plans during this iteration.

The current tracker is not a correctness oracle. It observes only explicit
`Renderer::Transition` calls, so a missing transition can be absent from both
the tracker and the comparison. Correctness also requires focused planner tests,
the D3D12 debug layer, and manual visual validation.

Acceptance criteria:

- The deterministic planner stress case produces identical ordered entries in
  all 100 runs.
- `--renderer-submission-stress` exits with code 0 in Debug and Release.
- Runtime differential logs contain zero cross-pass mismatches.

## Iteration 7: Execute Compiled Barriers For Selected Passes

Goal: migrate incrementally from inferred cross-list states to graph-compiled
batch states.

Steps:

1. Add `kRendererUseCompiledBarrierPlans` and one centralized `RenderPass`
   allowlist. Do not add per-pass macros.
2. Start with simple passes that:
   - record one direct list;
   - have complete resource declarations;
   - contain no complex internal state sequence;
   - do not call Streamline or other third-party command recording.
3. For a migrated pass, execute the compiled prologue barriers before its work
   lists.
4. Do not also emit legacy acquire barriers for a migrated pass. Keep the old
   tracker active in validation-only mode for that pass.
5. Maintain one submit-time global state cursor. Migrated and legacy batches
   both consume and commit states through that cursor in deterministic
   submission order.
6. Compare compiled final states against tracker-observed final states.
7. Treat each pass migration as one explicitly requested sub-iteration. Migrate
   exactly one pass, run the full validation loop, report, and stop.
8. Migrate multi-list passes only after deterministic ordering and batch-level
   usage validation are proven.
9. Migrate copy-heavy, ping-pong, DLSS, transparent, and nested-driver passes
   last.

Acceptance criteria for each migrated pass:

- Debug layer reports no state errors.
- Shadow comparison reports zero cross-pass mismatches.
- Manual visual checks report `PASS`, or are explicitly reported as `NOT RUN`.
- Performance comparison reports `PASS`, or is explicitly reported as
  `NOT RUN`.
- Disabling the migration flag restores the old behavior.

## Iteration 8: Introduce A Lightweight Native Command Context

Goal: remove global lane/TLS tracking from intra-command-list transitions
without creating a custom replay stream.

The context records directly into a native D3D12 command list. Use this initial
public behavior; adapt constructor ownership only when required by existing
renderer ownership:

```cpp
class CommandContext {
public:
    ID3D12GraphicsCommandList* Native() const;
    void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES after);
    void UAVBarrier(ID3D12Resource* resource);
    void ReconcileExternalState(ID3D12Resource* resource,
                                D3D12_RESOURCE_STATES after);
    void Close();

private:
    ID3D12GraphicsCommandList* list_ = nullptr;
    robin_hood::unordered_flat_map<ID3D12Resource*,
                                   D3D12_RESOURCE_STATES> current_;
    bool closed_ = false;
};
```

Steps:

1. Add `kRendererUseCommandContext`.
2. Apply `CommandContext` to direct command lists only in the initial
   implementation. Bundles remain raw, must not issue transitions or UAV
   barriers, and inherit state from their direct-list driver.
3. Store per-list current states in the context rather than global TLS lanes.
4. Seed the context with the graph-compiled entry states for its batch.
5. Emit internal transitions directly to the native list.
   - `Native()` on a closed context calls `RendererInvariantFailure`.
   - `Transition` on a closed context calls `RendererInvariantFailure`.
   - `Transition` on an unseeded resource calls `RendererInvariantFailure`.
   - Equal before/after states emit no barrier.
   - Different states emit one legacy whole-resource transition and update
     `current_`.
   - `UAVBarrier` on a closed context or unseeded resource calls
     `RendererInvariantFailure`; otherwise it emits one legacy UAV barrier and
     does not change `current_`.
6. Preserve access to `Native()` for existing rendering code and third-party
   APIs during migration.
7. Code recording through a `CommandContext` must not call raw
   `Renderer::Transition` or `Renderer::UAVBarrier` for that list. Use the
   context methods so local state remains authoritative.
8. After an external API can change resource state through `Native()`, require
   explicit `ReconcileExternalState` calls for every affected resource. Do not
   guess external final states. Reconciliation updates `current_` without
   emitting a barrier and fails on a closed context or unseeded resource.
9. The initial Iteration 8 request adds the context, tests, and no broad renderer
   migration. Migrate one named helper family or one named pass from
   `(ID3D12GraphicsCommandList*)` to `(CommandContext&)` per separately requested
   Iteration 8 sub-iteration.
10. Keep raw-list overloads only where external APIs require them.
11. Validate that no list is used after `CommandContext::Close`.
12. Validate each list's final local states against the batch's declared exit
   states. Multi-list passes must either agree on shared final states or declare
   explicit intra-batch ordering.
13. A final-state mismatch discovered after recording has begun is a renderer
    invariant violation: log details and abort. Do not throw from the worker.
14. `Close()` succeeds exactly once, validates final states, closes the native
    command list, and marks the context closed. It checks the native `Close()`
    HRESULT and calls `RendererInvariantFailure` on failure; it does not throw.
15. Extend `--renderer-submission-stress` with context transition, close,
    use-after-close death-test, and external-state reconciliation cases.

Acceptance criteria:

- Intra-list transitions no longer require lane lookup or TLS entry caching.
- `--renderer-submission-stress` exits with code 0 in Debug and Release.
- Command recording still calls D3D12 directly and remains parallel.
- Existing raw-list integrations continue to function.

## Iteration 9: Remove Legacy Cross-List State Inference

Goal: make the graph compiler the single authority for cross-pass transitions.

Prerequisites:

- every parallel render-graph pass uses compiled barriers;
- every state-changing helper used by those passes goes through
  `CommandContext` or explicitly reconciles external state;
- every earlier manual and performance check reports `PASS`.

Steps:

1. Require complete usage declarations for every parallel render-graph pass.
2. Move persistent external state ownership into a small resource-state
   registry:
   - swapchain backbuffers;
   - upload/copy resources;
   - resources created or destroyed outside the graph;
   - resources shared with third-party APIs.
3. Commit graph final states to the registry after submission planning.
4. Remove `ResourceStateTracker` first-use/current maps and lane/TLS machinery.
5. Keep local `CommandContext` state tracking for transitions inside a list.
6. Fail graph compilation on unknown required state rather than assuming
   `COMMON`, except where `COMMON` is explicitly declared as the external state.
7. Update resource destruction and resize paths to remove registry entries.
8. Remove legacy feature flags and code only when the user explicitly requests
   Iteration 9 and every earlier migration check is `PASS`.
9. Extend `--renderer-submission-stress` to validate registry initialization,
   commit, resource removal, and unknown-state rejection.

Acceptance criteria:

- All cross-pass barriers come from compiled graph plans.
- No execution correctness depends on worker completion order.
- State-tracker lane counters and lookups are gone.
- `--renderer-submission-stress` exits with code 0 in Debug and Release.
- Full validation loop passes with the old path disabled.

## Iteration 10: Optimize Only After Correctness

Goal: reduce overhead without changing the model.

Each numbered item below is a separate optimization sub-iteration. The user
must explicitly request one item. If the user requests only "Iteration 10,"
inspect and recommend one item; do not edit files.

Available optimization sub-iterations:

1. Coalesce adjacent transition barriers in each batch prologue.
2. Avoid empty prologue lists.
3. Reuse dedicated prologue command allocators/lists.
4. Enhanced barriers: first add a capability query and retain a legacy-barrier
   fallback. If the required interface or capability is unavailable, report the
   optimization as `NOT RUN` and make no behavior change.
5. Merge very small passes only when profiling shows command-list overhead is
   material.
6. Tune object/chunk sizes based on measured recording time and list count.
7. Evaluate compute/copy queues only after queue ownership and fence edges are
   represented in graph usage declarations.
8. Add subresource-range declarations and hazard tracking as a separately
   requested model-expansion sub-iteration.

Keep an optimization only when:

- Debug and Release pass the full validation loop;
- output remains correct;
- the improvement repeats across multiple runs; and
- it passes the Performance procedure; and
- the user explicitly accepts any added complexity.

## Optional Experiment: Custom Command Stream

Do not begin this experiment as part of the main refactor.

Consider it only if profiling proves that native command-list creation/reset or
parallel D3D12 recording is a dominant CPU cost after the graph compiler is
complete.

If tested, limit the first prototype to a narrow, repetitive workload such as
static draw packets. Do not attempt to virtualize the entire
`ID3D12GraphicsCommandList` interface.

This experiment requires an explicit user request naming the workload. Otherwise
inspect and report only; do not implement it.

Required proof before adoption:

- Replay is faster than direct parallel native recording.
- The serial replay stage does not become a frame bottleneck.
- Resource and descriptor lifetimes are explicit and validated.
- Streamline, GPU profiling, debug markers, copies, dispatches, and unusual
  command-list calls remain on native paths.
- Capture/replay complexity provides value beyond transition insertion.

## Completion Criteria

The refactor is complete when:

- Native command lists are recorded in parallel.
- Submission order is deterministic.
- Runtime pass fan-out cannot overflow fixed-capacity containers.
- Cross-pass barriers are compiled from graph resource usage.
- Internal pass transitions are recorded through per-list command contexts.
- The lane/TLS-based `ResourceStateTracker` has been removed.
- Debug and Release pass the full validation loop.
- Performance passes the defined procedure, or the user explicitly accepts the
  measured regression.
