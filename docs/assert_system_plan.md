# Assert system (check / ensure) — execution plan

**Status: PLANNED, nothing started. No source file has been touched.** This document is the
contract for the work; the owner's answers to "Open decisions" are required before S2 lands.

This document is written as an execution contract for an AI working in this repository. Each step
must leave the tree buildable and independently verifiable. Do not silently combine steps, and do
not migrate call sites while the macro contract is still moving.

---

## 0. Why

### 0.1 The inventory

`sources/` holds **94 runtime `assert(...)`** across 20 files, plus 42 `static_assert` (compile
time — those stay exactly as they are; they are not part of this plan). Shaders hold none.

### 0.2 Two of three configs do not check anything

| Config | Defines | `assert` |
|---|---|---|
| `Debug\|x64` | `_DEBUG`, `WITH_EDITOR=1` | active |
| `Release\|x64` | `NDEBUG` | **compiled out** |
| `Release_Editor\|x64` | `NDEBUG`, `WITH_EDITOR=1` | **compiled out** |

So a capacity overrun, a pool overflow, or a descriptor-table overrun — the failures that must
never be a silent drop — are checked in the one config nobody ships or profiles in.

### 0.3 The dialog is suppressed by a workaround, not by a contract

`sources/app/main.cpp:215-236` installs `_CrtSetReportHookW2` (the wide path — `assert` reports
through `_wassert` → `_CrtDbgReportW`, which the narrow hook never sees). It logs the CRT text as
`[FATAL] CRT assert: ...`, calls `logging::Shutdown()`, and `TerminateProcess(…, 3)` when no
debugger is attached; under a debugger it returns `FALSE` so the default break still happens.

That works, and the behaviour it produces is exactly what is wanted here. What it cannot do:

- the record carries CRT text (`expr`, file, line) only — **no category, no formatted values**, so
  "which cascade / which pass / what was the count" is never in the log;
- it is installed **only under `#if defined(_DEBUG)`**, and only after `LogSession` is constructed
  (the `--log-stress` early return at `main.cpp:192` runs before it exists);
- it depends on a CRT implementation detail (the wide report path) rather than on our own code;
- in `Release` / `Release_Editor` there is nothing to intercept, because there is no check.

### 0.4 The engine already has the right shape — twice, hand-rolled

`RendererInvariantFailure(msg)` (`sources/rendering/core/RendererInvariantFailure.{h,cpp}`) is
already: artifact write → `logging::EmergencyWrite(Fatal, …)` → `logging::Flush(2000)` →
`std::abort()`, active in **every** config, used at ~30 sites in `Renderer.cpp`, `RenderGraph.h`,
`SubmitTimeline.cpp`, `SceneRenderer_Graph.cpp`. The task system has a second, independent one
(`TaskSystemLockFree.cpp:688`: `assert(false && …); std::abort();`). `Renderer.cpp:443` has a
third (`ReportOnTerminate`). The engine needs **one** primitive, and these become callers of it.

---

## 1. The contract

| Macro | Active in | On failure |
|---|---|---|
| `CHECK(cond)` | all three configs | fatal sequence (§1.1) |
| `CHECKF(cat, cond, fmt, …)` | all three configs | fatal sequence, with a `{}`-formatted message and a log category |
| `CHECK_SLOW(cond)` / `CHECK_SLOWF(…)` | `_DEBUG` only | fatal sequence; expands to `((void)0)` under `NDEBUG` |
| `ENSURE(cond)` / `ENSUREF(cat, cond, fmt, …)` | all three configs | **returns `bool`**, logs `Error` **once per call site**, breaks under a debugger, execution continues |
| `ENSURE_ALWAYS(cond)` / `ENSURE_ALWAYSF(…)` | all three configs | as `ENSURE`, but logs every time (through `LOG_ERROR_THROTTLED`) |
| `CHECK_NO_ENTRY()` | all three configs | fatal sequence, `[[noreturn]]` — for a branch that must be unreachable |

`ENSURE` is an expression: `if (!ENSURE(ptr != nullptr)) { return; }` — the UE idiom, and the one
that lets the 11 editor/data call sites keep their existing fallback path.

### 1.1 The fatal sequence (exact order, and why)

1. **One record at `Fatal`** with category, the expression text, the formatted message and
   `file:line`. `Fatal` is already special-cased in `Log.cpp:415`: it is pushed through the queue
   so it lands *after* everything logged before the failure, then flushed synchronously, with
   `EmergencyRecord` as the fallback if either step fails. Nothing new is needed for durability.
2. **`logging::Flush(2000)`** — bounded, never holds up the death.
3. **`if (IsDebuggerPresent()) __debugbreak();`** — you stand at the failing frame, with the call
   stack intact. Continuing (F5) falls through to step 4, which is the UE behaviour.
4. **`logging::Shutdown()`**, then **`TerminateProcess(GetCurrentProcess(), 3)`**.

Step 4 is deliberately **not `std::abort()`**. `abort()` goes through the CRT's
"abort() has been called" path and WER, and `_set_abort_behavior(0, …)` is currently called only
inside `#if defined(_DEBUG)` (`main.cpp:219`) and inside two harnesses — so a `Release` fatal today
can still raise a window. `TerminateProcess` has no dialog in any config, runs no `atexit`
handlers on a process we have already decided is broken, and keeps **exit code 3**, which is what
the CRT hook and `_set_abort_behavior` already produce. **No headless gate's expected exit code
changes** — including `--tasksystem-stress --stress-overflow`, the death test at
`TaskSystemStress.cpp:196-222` that expects the 5th dependent to kill the process.

No message box exists anywhere on this path **by construction**: our code never enters
`_wassert` / `_CrtDbgReport`. The CRT hook in `main.cpp` stays (Debug only) because
`third_party/` still contains ~1093 of its own assertions, which we do not own.

---

## 2. Header layout, and the include-cost problem

- **`sources/core/diagnostics/Assert.h`** — deliberately light. **No `Log.h`, no `<string>`,
  no `<source_location>`.** Declares only:

  ```cpp
  namespace tc::assertion
  {
      [[noreturn]] void Fail(const char* expr, const char* msg,
                             const char* file, const char* func, int line,
                             unsigned char category) noexcept;
      bool EnsureFail(const char* expr, const char* msg,
                      const char* file, const char* func, int line,
                      unsigned char category) noexcept; // always returns false
  }
  ```

  and the non-formatting `CHECK`, `CHECK_SLOW`, `ENSURE`, `ENSURE_ALWAYS`, `CHECK_NO_ENTRY`.
  This is what `core/containers/inl_vector.h` includes — one declaration, no transitive weight.

- **`sources/core/diagnostics/AssertF.h`** — includes `core/logging/Log.h`, adds `CHECKF`,
  `CHECK_SLOWF`, `ENSUREF`, `ENSURE_ALWAYSF` (`{}` formatting, category as the first argument to
  match every `LOG_*` in the repo). 46 of 348 source files already include `Log.h`, so for the
  call sites that want a formatted message this is not new weight.

- **`sources/core/diagnostics/Assert.cpp`** — includes `Log.h` and `<windows.h>`, owns §1.1.

### 2.1 Blocking detail found while planning

`logging::WriteRaw` and `logging::EmergencyWrite` both take a
`std::source_location` — and **`std::source_location` cannot be constructed from `__FILE__` /
`__LINE__`**. An out-of-line handler that receives file/func/line as plain arguments therefore has
no way to reach the existing frontend with the *call site's* location; it would stamp
`Assert.cpp:NN` on every failure. `LogRecord` itself (`LogRecord.h`) stores
`sourceFile` / `sourceFunction` / `sourceLine` as plain fields, so the fix is small but it is a
real prerequisite → **S1**.

---

## 3. Steps

Each step is one commit, buildable and verifiable on its own.

### S1 — location-explicit logging entry point
Add to `core/logging/Log.{h,cpp}` a frontend that takes the location as `(file, func, line)`
instead of `std::source_location`, alongside the existing ones (`WriteRawAt`, `EmergencyWriteAt`,
or an internal `detail::` pair — the public surface should not grow more than it must).
No call-site changes.
**Gate:** `--log-stress` exit 0 in Debug and Release; `python tools/check_logging.py` = 0.

### S2 — the primitive
`Assert.h` / `AssertF.h` / `Assert.cpp` per §1 and §2. **Zero call sites migrated.** Register the
three files in **both** `test_cube.vcxproj` *and* `test_cube.vcxproj.filters` (forward slashes for
C++ — repo rule).
**Gate:** all three configs build (`WITH_EDITOR` is Debug + Release_Editor only, so a
Debug-only build proves nothing about `Release`).

### S3 — make dialog suppression config-independent
Move `_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT)` and
`SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX)` in
`main.cpp` **out of** `#if defined(_DEBUG)`. The `_CrtSetReportHookW2` block stays Debug-only
(it exists for `third_party`). This closes the residual Release window that the two stress
harnesses currently each suppress by hand.

### S4 — `--assert-stress` self-test harness
Modelled on `RunLogStress`, including its fatal-child technique: the parent spawns itself with a
sub-switch, the child trips a `CHECK`, and the parent asserts on the **artefact**, not on a claim:
- exit code is exactly `3`;
- the child's session log contains a `][FATAL][` line carrying the category, the expression and
  the formatted values (the `][FATAL][` substring is the same match `LogStress` already uses);
- **no window appeared** — the child terminated without input inside the timeout;
- `ENSURE` logs once per call site over N iterations, returns `false`, and the process survives;
- `CHECK_SLOW` in a `Release` child does not fire.

**Verdict is the exit code plus lines in the session log. Do NOT add `logs/assert_stress.log`** —
the artifact set is closed (AGENTS.md), and diagnostics are events.

### S5 — migrate the 94 call sites, one commit per owner
Order chosen so the risky, behaviour-changing batch (S5e) lands last, on top of a primitive that
three earlier batches have already exercised. §4 has the per-file mapping.

- **S5a** containers + text — `inl_vector.h` (12), `TextManager.h` (5), `TextManager.cpp` (8)
- **S5b** render graph + renderer — `RenderGraph.h` (14), `Renderer.cpp` (1),
  `SceneRenderer_Geometry.cpp` (1); **fold `RendererInvariantFailure` onto the new core** (keep
  the function and its name — ~30 call sites — reimplement its body as a `CHECKF`-equivalent)
- **S5c** scene + shadow validators — `Scene.cpp` (14), `RenderableObject.h` (2),
  `RenderableObjectBase.h` (1)
- **S5d** platform + UI + materials + task system — `ImGuiLayer.cpp` (11), `Material.cpp` (4),
  `TaskSystemLockFree.cpp` (4), `Systems.cpp` (2), `OceanRenderable.cpp` (1),
  `DeleteObjectCommand.cpp` (1)
- **S5e** boot and level data — `App.cpp` (6), `JsonLevel.cpp` (3), `SceneObjectRegistry.cpp` (2),
  `LevelManager.cpp` (1), `OceanSimulation.cpp` (1). **This is the batch that changes shipped
  behaviour** (§5, decision 2) and it needs the owner's answer before it is written.

### S6 — lock the door
Extend `tools/check_logging.py` with a `raw assert(` pattern and an allowlist
(`core/diagnostics/Assert.cpp` and whatever shim genuinely needs the CRT), so a new
`assert(` cannot come back. `static_assert` must not match the pattern.
**Gate:** `python tools/check_logging.py` = 0.

### S7 — write it down
A short section in `AGENTS.md` next to "Session Log" (which macro, when, and that a failure is a
log event with exit code 3, not a window), and a cross-reference from
`docs/logging_system_plan.md`.

---

## 4. Call-site mapping (provisional; finalized per site inside each S5 batch)

| File | Sites | Proposed |
|---|---|---|
| `core/containers/inl_vector.h` | 12 | 3 capacity (`:31`, `:81`, `:212`) → **`CHECK`**; 9 element access → **`CHECK_SLOW`** |
| `text/TextManager.h` | 5 | all **`CHECK_SLOW`** (per-glyph) |
| `text/TextManager.cpp` | 8 | `:651` pool overflow → **`CHECK`**; rest **`CHECK_SLOW`** |
| `app/scene/Scene.cpp` | 14 | 6 cascade-index + 6 S14 cull validators + `:1736` → **`CHECK_SLOW`**; `:1230` (non-zero editor id) → **`CHECK`** |
| `rendering/core/RenderGraph.h` | 14 | all **`CHECK`** — every one is once per pass registration or per compile, never per element, so the cost is nil and these are exactly the "silently corrupt the frame" class |
| `ui/ImGuiLayer.cpp` | 11 | all **`CHECK`** (null arguments + descriptor-handle range at an SDK boundary) |
| `app/App.cpp` | 6 | `:1032`, `:1099` (`systems_`) → **`CHECK`**; 4 boot-data → decision 2 |
| `materials/Material.cpp` | 4 | all **`CHECK`** (unsupported root-signature shapes; currently silent in Release) |
| `core/task/TaskSystemLockFree.cpp` | 4 | `:143`, `:688` → **`CHECK`** (`:688` is already always-fatal via `abort`); `:669`, `:681` race guards → **`CHECK`** (one relaxed atomic load) |
| `app/levels/JsonLevel.cpp` | 3 | decision 2 |
| `rendering/renderables/RenderableObject*.h` | 3 | S6 shadow-view guards → **`CHECK_SLOW`** (read inside the cull loop) |
| `app/scene/SceneObjectRegistry.cpp` | 2 | decision 2 |
| `app/Systems.cpp` | 2 | **`CHECK`** |
| `rendering/core/Renderer.cpp` | 1 | **`CHECK`** |
| `ocean/OceanRenderable.cpp` | 1 | **`CHECK`** (SRV table overrun vs `OCEAN_SURFACE_RS`) |
| `ocean/OceanSimulation.cpp` | 1 | decision 2 |
| `editor/commands/DeleteObjectCommand.cpp` | 1 | **`ENSURE`** — a broken undo must not kill the editor |
| `app/scene/SceneRenderer_Geometry.cpp` | 1 | **`CHECK`** |
| `app/levels/LevelManager.cpp` | 1 | decision 2 |

Roughly: **~40 `CHECK`**, **~35 `CHECK_SLOW`**, **~1 `ENSURE`**, **~11 pending decision 2**.
The `CHECK_SLOW` half is a pure rename — same behaviour as today, better message. The `CHECK`
half is the win: those checks start existing in `Release` and `Release_Editor`.

---

## 5. Open decisions (owner)

1. **Names.** `CHECK` / `CHECKF` / `ENSURE` / `CHECK_SLOW` unprefixed. Verified free: no
   `#define` of any of them in `third_party/` or `deps/`, and no use of a bare `CHECK(`/`ENSURE(`
   anywhere in `sources/`. Alternative is a `TC_` prefix, at the cost of the UE muscle memory
   that motivated this. **Rec: unprefixed.**
2. **The 11 data/boot sites (S5e) — the only real behaviour change.** Today a missing
   `bindings.json` or a malformed level asserts in Debug and *silently continues* in Release.
   **Rec: split.** `App.cpp:1108/1119/1123/1159` (no bindings, no material presets, no upload
   batch, initial level failed to load) → **`CHECKF`**: there is nothing to run. `JsonLevel.cpp`,
   `LevelManager.cpp`, `SceneObjectRegistry.cpp`, `OceanSimulation.cpp` → **`ENSUREF`** on top of
   the fallback each of them already has: a broken level file must not kill an editor session,
   and you edit levels live.
3. **`CHECK` in `Release`?** There is no `Shipping` config in this project, so UE's
   check-off-in-Shipping tier has no counterpart. **Rec: `CHECK` on in all three configs**
   (which is also what `RendererInvariantFailure` already does).
4. **`RendererInvariantFailure` and `logs/invariant_failure.log`.** The function folds onto the
   new core (keep the name, ~30 call sites). The **artifact** is a separate question: it is also
   the drain target for the D3D12 debug layer (`Renderer.cpp:483`), so it is not a duplicate of a
   session-log line and retiring it is not free. **Rec: keep the artifact, keep the name, replace
   only the body.**
5. **Exit code.** Keep `3` (already produced by the CRT hook and by `_set_abort_behavior`), so no
   gate script changes. **Rec: keep.**
6. **`--assert-stress` verdict.** Exit code + session log only, no new `logs/*.log`.
   **Rec: as stated.**

---

## 6. Gates

This touches every subsystem, so the full set applies (not the shader-math short set):

- **all three configs build**: `Debug|x64`, `Release|x64`, `Release_Editor|x64` — `WITH_EDITOR`
  code only compiles in two of them, so one config proves nothing
- `python tools/check_logging.py` → 0 (and `python tools/check_shaders.py` unchanged)
- `--log-stress` → 0 in **Debug and Release**
- `--assert-stress` → 0 (new, S4)
- `--tasksystem-stress` → 0, and `--tasksystem-stress --stress-overflow` still dies with exit 3
  (S5d changes which code kills it, not the code it dies with)
- `--renderer-submission-stress` → 0
- `--scene-stress-gbv=20` under a **guarded** GBV mode — `--gbv-mode=state` returns a
  falsely-clean verdict
- one `--shot` A/B on the atoll level with `--wind-freeze=0`, plus the **same-build-twice**
  control run, to prove the migration moved no pixels (difference imaging: a real regression must
  clear the noise floor measured by the control, not by a single run)

## 7. What this plan does not do

- It does not touch the 42 `static_assert` — they already fail at compile time in every config.
- It does not touch `third_party/` (~1093 assertions). The Debug CRT hook stays for them.
- It does not introduce exceptions anywhere: `RendererInvariantFailure`'s header comment records
  why (render-pass worker tasks swallow task exceptions), and that constraint is inherited.
