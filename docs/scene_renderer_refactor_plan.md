# SceneRenderer refactor plan (6.7k lines into files with one subject each)

## What is actually in there

Measured, not estimated (`SceneRenderer.cpp`, 6703 lines):

| region | lines | share |
|---|---|---|
| `Render()` — per-frame decisions + graph construction + execute | **1799** | 27 % |
| bloom (downsample / build / kernel survey / flares / convolve / bokeh bake) | **1100** | 16 % |
| reflections (SSR, RT, glass ×2, blur, temporal, RT debug, parked denoise) | 615 | 9 % |
| compose + transparent + ocean reflection | 491 | 7 % |
| shadows (shore depth, cull, CSM, spot, point, VSM request/render) | 465 | 7 % |
| lighting (directional, spot, point, skybox) | 442 | 7 % |
| file-local helpers in one anonymous namespace (lines 51-385) | 335 | 5 % |
| tonemap + DLSS + debug + overlay | 264 | 4 % |
| exposure metering | 216 | 3 % |
| HZB + GTAO | 208 | 3 % |
| RT acceleration structure + bindless | 182 | 3 % |
| G-buffer (+ its inner graph) | 146 | 2 % |
| lifecycle (Init / FinalizeLevelLoad / Reset / EnsureFrameResources) | ~110 | 2 % |

`SceneRenderer.h` is 562 lines with **57 member declarations**: 12 SSR, 11 bloom, 8 flare, 7
acceleration-structure, 4 GTAO, 3 reflection, 3 RT, 2 exposure, 2 VSM, plus the frame pointer.

Two things this census settles before any work starts:

- **This is NOT a build-time problem.** `SceneRenderer.h` is included by exactly two files — `Scene.h`
  and its own `.cpp`. Splitting it will not speed up a rebuild in any meaningful way, and a plan
  sold on compile times here would be selling something it cannot deliver. The problem is
  navigation and ownership: one file holds thirteen subjects, and the thing you open to change the
  bloom kernel is the same thing you open to change a shadow cascade.
- **21 % of the file is comments** (26 % inside `Render()`), and those comments are where the
  barrier-plan and pass-flow lessons live — every "P8C-2l: THE POINT ITSELF IS NOT GATED" that a
  future reader needs. **They move with their code, verbatim.** No step in this plan is allowed to
  shrink the file by deleting them; the target is fewer subjects per file, not fewer lines.

## Non-goals

- **No behaviour change at any step.** Every step is a move or a mechanical re-shape; the barrier
  program, the pass order and the recorded commands stay byte-identical unless a step says
  otherwise in its own text.
- **No re-litigating the pass-flow architecture.** S1-S9 just made every pass one builder that
  decides, declares and returns its record. This plan moves that code; it does not restyle it.
- **No "one class per domain".** A `ShadowRenderer` / `LightingRenderer` / `PostRenderer` split
  reads well on a slide and buys nothing here: the pass bodies are stateless functions over
  `frame_`, `resources_` and the deferred targets, so each new class would exist only to carry the
  same three pointers around. What deserves a class is state — which is why exactly two subsystems
  get extracted (bloom, RT acceleration structures) and the rest is split by FILE.
- **`Pass_RTDenoise` stays.** It is parked, not dead (SceneRenderer.cpp ~739 says so); it travels
  with the reflections.

## Rules that apply to every step

- **New files go in BOTH `test_cube.vcxproj` AND `test_cube.vcxproj.filters`** — C++ entries use
  forward slashes, filter entries use backslashes. A file that compiles but is missing from the
  filters is invisible in the IDE tree, which is how a "where did that function go" hunt starts.
- **CRLF.** Every `.cpp/.h` in this repo is CRLF; the Write tool creates LF. Check every new file
  (lone-LF count must be 0) before calling a step done.
- **Gate by step type.**
  - A PURE MOVE (R1, R2) cannot change the barrier program: both builds 0/0, one
    `--scene-stress-gbv=20 --barrier-cmp --canonical-check` CLEAN, and a `--shot` A/B that is
    inside the same-binary noise floor. Measuring that floor first is not optional — on `wind_test`
    it spans 0-22k pixels >16 between two runs of ONE binary.
  - An EXTRACTION (R3, R4) or a RE-SHAPE (R5, R6) takes the full pass-flow gate: both builds 0/0;
    Debug `--scene-stress-gbv=20 --barrier-cmp --canonical-check` CLEAN in BOTH shadow modes;
    `--scene-stress=12` CLEAN; `--barrier-cache-verify` silent; zero flip-miss / MISSING /
    EmitPoint lines; the comparator silent (which it now is — anything it prints after a step here
    is that step's doing).
- **One commit per step.** R2 may be one commit per moved file.

## Steps

- **R1 — `SceneRenderInternal.h`: the file-local helpers get a home.** Lines 51-385 are one
  anonymous namespace holding what every pass body uses: `BucketIndex`, `UploadFrameCB`,
  `BuildGBufferViewCB` / `BuildShadowViewCB` / `BuildGlassViewCB`, `ApplyWind`,
  `FilterShadowCasters`, `ShouldRenderSelectionStencil`, `RtMaterialFingerprint`, the
  selection-stencil constants, plus the file-scope `SetCommandListName`. Nothing can be split out
  of the `.cpp` until these are reachable from another translation unit.
  Move them verbatim into `sources/app/scene/SceneRenderInternal.h` (an INTERNAL header — included
  only by the SceneRenderer translation units, stated in its first line so nobody grows a
  dependency on it). The `static` on `SetCommandListName` becomes `inline`; the anonymous namespace
  becomes a named one (`scene_internal`) so the same symbols are not silently duplicated per TU.
  GATE: move-gate. **-335 lines**, no new file in the build yet beyond the header.

- **R2 — split the pass bodies across translation units, class untouched.** No API change, no
  member moves: the same class, its methods defined in five more `.cpp` files.
  - `SceneRenderer_Geometry.cpp` — `RenderObjectBatch`, `Pass_GBuffer` + its inner graph,
    `Pass_Transparent` + its inner graph, `PrepareOpaqueDrawStates`, the selection-stencil draws
    (~500 lines).
  - `SceneRenderer_Shadows.cpp` — `Pass_ShoreDepth`, `Pass_ShadowCull`, `Pass_CSM`,
    `Pass_SpotShadows`, `Pass_PointShadows`, `Pass_VsmPageRequest`, `Pass_VsmPageRender`,
    `IndirectShadowDrawsActive` (~600).
  - `SceneRenderer_Lighting.cpp` — `Pass_Lighting`, `Pass_SpotLights`, `Pass_PointLights`,
    `Pass_Skybox`, `Pass_Hzb`, `Pass_Gtao`, `Pass_Compose` (~900).
  - `SceneRenderer_Reflections.cpp` — SSR, RT reflections, `Pass_RTDebug`, both glass passes,
    `Pass_ReflectionBlur`, `Pass_SsrTemporal`, `RecordOceanReflection`, the parked
    `Pass_RTDenoise`, `FillSsrHzbConstants` / `FillSsrUeConstants` (~700).
  - `SceneRenderer_Post.cpp` — `Pass_ExposureMetering`, `Pass_Dlss`, `Pass_Tonemap`, `Pass_Debug`,
    `Pass_DebugPreview`, `Pass_Overlay`, `PickDebugTexTarget` (~600).
  What stays in `SceneRenderer.cpp`: the lifecycle, `EnsureFrameResources`, and `Render()`.
  Watch for: file-scope constants that belong to ONE subject (`kStreakMaxLevels`,
  `kStreakMinLevelWidth` are bloom's) — they move with it, not into the shared header.
  GATE: move-gate, once after the last file. **The main file drops to ~2400.**

- **R3 — extract the bloom subsystem.** The biggest single subject (1100 lines) and the one with
  the most state: `bloomActive_`, `bloomConvolution_`, `flaresGhosts_`, `flaresStreak_`,
  `bloomKernelKeys_`, `bloomKernelTex_`, `bloomKernelReady_`, `bloomKernelLoadedPath_`,
  `bloomKernelPixelDim_`, `bloomKernelCenterEnergy_`, `bloomKernelScatterEnergy_`,
  `bloomSurveyRatio_`, `flareBokeh_[2]`, `flareBokehUpload_`, `flareBokehSafeFrame_`,
  `flareBokehReady_`, `flareBokehBlades_` — nineteen members, none of which any other pass reads.
  New `BloomRenderer` (`sources/rendering/post/BloomRenderer.{h,cpp}`) owning exactly that state,
  with three entry points shaped by what S8 already built:
  - `Decide(const SceneFrameData&, Renderer*)` → `{ active, convolution, flares }`, called from
    `EnsureFrameResources` where the flags are computed today.
  - `Declare(RenderGraphPassContext&, const Decisions&)` → the four points, called by the tonemap
    builder, which keeps ownership of the point INDICES (they are the tonemap's program).
  - `Record(Renderer*, ID3D12GraphicsCommandList*, hdrSource, const TonemapPoints&)` — the body,
    which emits the tonemap's markers exactly as it does now.
  The kernel load / survey / bokeh bake become private to it; `Bloom_ApplyConstants` and
  `Bloom_TonemapBloomScale` stay public because the tone curve reads them.
  RISK, stated plainly: this is the pass with the most hard-won barrier layout in the engine
  (P8C-2l/m). The extraction must not renumber or regroup a single point.
  GATE: full pass-flow gate PLUS both bloom methods flip-traced — and note that **no level in the
  repo enables the pyramid method** (`bloom.enabled` defaults false; only `wind_test` overrides it,
  with `method: 1`), so the pyramid path is exercised through a scratch copy of the level with
  `method: 0`, exactly as S8 did. The expected trace is the nine-point program with 6-barrier
  write/read points for the convolution and 4-barrier ones for the pyramid.

- **R4 — extract the RT acceleration structures.** `Pass_BuildAS` (179 lines),
  `InvalidateRaytracing`, `RtMaterialFingerprint`, `rtInstances_`, `rtBindlessObjectCache_`,
  `asManager_`, `bindless_`, `asScratchRetireFrame_`, `rtReflectActive_` → `RtSceneAs`
  (`sources/rendering/rt/RtSceneAs.{h,cpp}`). `GetTlasSrvCpu` / `IsRtReflectActive` stay on
  SceneRenderer as one-line forwards, because the transparent pass and the glass renderable call
  them. ~250 lines and 7 members out.
  GATE: full gate, plus an RT-on run (`Pass_BuildAS` only exists when RT is enabled, so a default
  stress may never build one — check the trace for the pass before believing the gate covered it).

- **R5 — `Render()` into phase builders.** After R1-R4 `Render()` is still ~1500 lines of graph
  construction, and it is the last big thing in the file. Split it into one method per phase, each
  taking a `GraphBuild&` — a struct holding the graph reference, this frame's deferred sets, the
  decisions from R6 and the pass indices produced so far:
  `BuildPrologue` (AS, clear, object compute, surf sim, wetness, terrain depth) →
  `BuildShadows` (cull, CSM, spot, point) →
  `BuildGBufferAndAo` (G-buffer, VSM request/render, HZB, GTAO) →
  `BuildLighting` (directional, spot, point, skybox) →
  `BuildReflections` (source variants, temporal, blur, compose, RT debug, glass) →
  `BuildForwardAndEditor` (transparent, object-id readback, debug draw, selection outline) →
  `BuildPost` (exposure, DLSS, tonemap, debug preview, debug).
  The pass INDICES are the interface between phases, so they are named fields on `GraphBuild`, not
  a bag of `size_t`. The CL-group brackets (`BeginCLGroup`/`EndCLGroup`) must not straddle a phase
  boundary — the compute group and the reflection group each sit wholly inside one phase, and the
  tonemap group inside `BuildPost`.
  GATE: full gate. This step changes no declaration, so `--barrier-cache-verify` plus a silent
  comparator is the proof that the program is identical.

- **R6 — per-frame decisions stop being members.** The flags computed at the top of `Render()` and
  in `EnsureFrameResources` — `clearReflections`, `ssrTemporalActive_`, `glassReflActive_`,
  `rtReflectActive_`, `bloomActive_`, `bloomConvolution_`, `flaresGhosts_`, `flaresStreak_`,
  `vsmSkipUpdate_`, `willDlss` — are decided once per frame and read by the builders. As members
  they are indistinguishable from state that legitimately CROSSES frames (`gtaoHistoryFrames_`,
  `vsmLastView_`, `vsmStillFrames_`, `bloomKernel*`, `flareBokeh*`, `lastExposureTimeSeconds_`),
  and that distinction is exactly what the pass-flow plan spent nine steps making visible.
  Introduce `FrameDecisions` (a plain struct, built by `DecideFrame()` right after
  `EnsureFrameResources`) and pass it to the phase builders. Every member that moves into it stops
  being state; what remains in the class is, by construction, cross-frame — and gets a one-line
  comment saying which frame it belongs to.
  GATE: full gate. Expect the header to lose ~10 members here and ~26 across R3/R4/R6 together.

- **R7 — the header, last.** With ~30 members gone, re-group what is left into three labelled
  blocks — cross-frame state, per-level caches, and sub-objects — and give each a one-line reason
  to exist. No code moves; this is the step that makes the class readable at a glance, and it is
  last because doing it earlier just means doing it twice.

## Where this lands

Nothing disappears — the same code, in files with one subject each:

| file | ~lines |
|---|---|
| `SceneRenderer.cpp` (lifecycle + `Render` orchestration + `DecideFrame`) | 500 |
| `SceneRenderer_Graph.cpp` (the seven phase builders) | 1500 |
| `SceneRenderer_Lighting.cpp` | 900 |
| `SceneRenderer_Reflections.cpp` | 700 |
| `SceneRenderer_Shadows.cpp` | 600 |
| `SceneRenderer_Post.cpp` | 600 |
| `SceneRenderer_Geometry.cpp` | 500 |
| `SceneRenderInternal.h` | 350 |
| `BloomRenderer.{h,cpp}` | 1100 |
| `RtSceneAs.{h,cpp}` | 250 |

## Detachability

R1 is a precondition for R2; R2 is a precondition for nothing (it is pure file surgery and can be
reverted with a single `git revert`). R3 and R4 are independent of each other and of R5/R6. R5
wants R6's `FrameDecisions` to exist first, or the phase builders will each take eight loose bools.
R7 is cosmetic and can be dropped without cost.

The order above is chosen so that the two riskiest steps (R3 bloom, R5 `Render()`) happen when the
file is already small enough to review a diff in one sitting.

## Status

- Written 2026-08-27, right after the pass-flow plan (S1-S9) closed; the line counts above are
  from that state of the tree.
- R1 DONE (uncommitted). `sources/app/scene/SceneRenderInternal.h` holds the block that was lines
  42-385: `SetCommandListName`, `RtMaterialFingerprint`, the selection-stencil constants and
  predicates, `BucketIndex`, `FilterShadowCasters`, the `PerViewCB` / `GlassViewCB` /
  `OceanReflectionConstants` layouts with their static_asserts, `UploadFrameCB`, `ApplyWind` and
  the three `Build*ViewCB` builders. The anonymous namespace became `scene_internal` and the free
  functions became `inline`; `SceneRenderer.cpp` gained the include plus one using-directive, so
  every call site is spelled exactly as before. Registered in BOTH `test_cube.vcxproj` and
  `.filters`; CRLF verified.
  **The move was proved verbatim, not asserted:** normalising both sides for the `inline` keyword
  and the namespace braces, the 261 statements of the old block and the 261 of the new header
  diff to ZERO semantic differences.
  Gate (move-gate): both builds 0/0 with the artefacts verified fresh; Debug
  `--scene-stress-gbv=20 --barrier-cmp --canonical-check` CLEAN with zero flip-miss / MISSING /
  EmitPoint / comparator lines; `--shot` A/B on `wind_test` inside the same-binary noise floor
  (A/B 1859-6101 pixels >16 against a 1803-12860 floor).
  `SceneRenderer.cpp`: **6703 -> 6364**.
- R2 DONE (uncommitted). The pass bodies moved into six translation units — one more than the plan
  listed, because the bloom got its own `SceneRenderer_Bloom.cpp` instead of riding in `_Post`: R3
  extracts it into a class, and a file it already owns turns that step into a rename plus a reshape
  instead of a second move.

  | file | lines |
  |---|---|
  | `SceneRenderer.cpp` (lifecycle + `Render` + `Pass_BuildAS` + `Pass_PrologueClear`) | **2142** (was 6365) |
  | `SceneRenderer_Bloom.cpp` | 1134 |
  | `SceneRenderer_Reflections.cpp` | 897 |
  | `SceneRenderer_Lighting.cpp` | 750 |
  | `SceneRenderer_Shadows.cpp` | 638 |
  | `SceneRenderer_Post.cpp` | 607 |
  | `SceneRenderer_Geometry.cpp` | 522 |

  Every new TU carries the original include block verbatim; trimming it per file is deliberately
  not part of this step. All six are registered in BOTH project files, CRLF verified.
  **Proved, not asserted:** counting every non-blank line across the seven files against the single
  file before the split — ZERO lines lost and ZERO invented; the only additions are the six copies
  of the include block and the section banners.
  Gate: both builds 0/0 with all six `.obj` verified fresh; Debug `--scene-stress-gbv=20` CLEAN in
  both shadow modes; `--scene-stress=12` CLEAN; zero flip-miss / MISSING / EmitPoint / comparator
  lines.

  **FINDING worth keeping — splitting translation units MOVES PIXELS in Release, and that is not a
  defect.** The Release `--shot` A/B against the pre-split binary came out at 11.5k-23k pixels
  differing by >16, against a same-binary noise floor of 1k-9.7k measured over ten pairs: above the
  noise, so it could not be waved away. The source is provably identical line for line, so the
  suspect was the optimizer — `WholeProgramOptimization` is on, and LTCG makes its inlining and
  float-contraction decisions per context, which the TU boundaries are part of. **Debug settles
  it**: with no optimisation and no LTCG, the same A/B is 878 and 1233 pixels against a 521-1147
  noise floor — indistinguishable. So the Release difference is codegen, the sources are
  semantically identical, and the lesson for the next file split is to reach for the Debug
  comparison rather than spend an hour hunting a semantic change that is not there.
- R3 DONE (uncommitted). `sources/rendering/post/BloomRenderer.{h,cpp}` owns the nineteen members,
  the per-frame decision and the four barrier points; `SceneRenderer_Bloom.cpp` (1134 lines, the
  sixth TU R2 made) is gone and the header lost its 73-line bloom block. The API is the one the
  plan called for, with the argument order settled by the call sites:
  `Initialize(SceneResourceBootstrapper*)`, `Decide(Renderer*, const SceneFrameData&)`,
  `Active()/Convolution()/Flares()`, `Declare(ctx, Points&)`, `Record(renderer, cl, hdrSource,
  Points)`, `ApplyConstants()`, `TonemapBloomScale()`. `TonemapPoints` now holds a
  `BloomRenderer::Points` instead of four loose indices, so the four points are named ONCE and the
  tonemap still owns the numbering — the risk the step called out (renumbering P8C-2l/m's layout)
  is structurally excluded rather than merely avoided.
  **Deliberately no `Reset()`.** Nothing in here is level state: the kernel image and the bokeh
  sprite are assets, `SceneRenderer::Reset` never cleared them, and `Decide()` re-checks every
  material and target each frame anyway. Adding one “for symmetry” would have been a level switch
  throwing away a kernel it is about to reload.
  The only non-mechanical edit inside the move: the tone-curve lambda used to capture `pts` for one
  bool, which a lambda nested in a builder cannot do — it now captures `bloomRan = pts.bloom`.

- R4 DONE (uncommitted). `sources/rendering/rt/RtSceneAs.{h,cpp}` owns `asManager_`, `bindless_`,
  `asManagerInited_`, `asScratchRetireFrame_`, `rtInstances_`, `rtBindlessObjectCache_` and
  `asVramLogged_`, plus the 178-line `Pass_BuildAS` body moved VERBATIM (`SceneRenderer::Pass_BuildAS`
  → `RtSceneAs::Build`, `frame_->` → `frame.`, nothing else). `RtMaterialFingerprint` went with it
  into a file-local anonymous namespace and left `SceneRenderInternal.h`, which had been its only
  home for one caller. `Reset()` / `Invalidate()` replace the two seven-line clear blocks that used
  to be copy-pasted in `SceneRenderer::Reset` and `InvalidateRaytracing`. `Manager()` and
  `Bindless()` are exposed on purpose — those two objects ARE what the RT passes bind from, and
  ~81 forwarders would have been a second name for the same thing.
  Two things the step had to decide, both recorded because they are not obvious from the plan text:
  1. **The lazy device init became `EnsureInit(Renderer*)`, not part of `Build()`.** It cannot move
     into the pass body: the RT passes' BUILDERS query the manager while the graph is being
     assembled, i.e. before any body records. It is called from the same frame gate as before.
  2. **`rtReflectActive_` stayed on SceneRenderer**, against this step's own bullet. It is a
     per-frame DECISION, and R6 lists it by name as one — moving it here would mean moving it
     again two steps later.
  `SetCommandListName` moved from `scene_internal` to `rendering/core/RenderGraph.h`. The AS build
  records a pass and now lives outside `sources/app/scene/`, and the alternative — a second copy of
  three lines — is the thing the R1 header exists to prevent. All 42 call sites are unchanged:
  unqualified lookup reaches the global definition exactly as it reached the namespaced one.
  SceneRenderer.cpp: 6703 → 1770 lines across R1-R4.

  Gate for R3+R4 together (run once, after both, as asked):
  - Both configs build 0 errors / 0 warnings; `BloomRenderer.obj`, `RtSceneAs.obj`,
    `SceneRenderer.obj`, `SceneRenderer_Post.obj`, `SceneRenderer_Reflections.obj` all verified
    fresh in BOTH.
  - Debug `--scene-stress-gbv=20 --barrier-cmp --canonical-check`: CLEAN, both shadow modes
    (`emit enhanced=7581` VSM / `6461` legacy, `as enhanced=104` in both).
  - Debug `--scene-stress=12`: CLEAN.
  - Debug `--barrier-cache-verify --barrier-cmp` on `wind_test`: SILENT (the recompile-every-frame
    diff produced not one line).
  - Debug `--barrier-flip-trace --barrier-cmp --canonical-check` on BOTH bloom methods
    (`method: 1` from the level, `method: 0` from a scratch copy OUTSIDE `data/`): zero MISSING,
    zero EmitPoint, zero comparator lines in either arm.
  - **The RT AS build was proved to RUN, not assumed**: `as enhanced=104` acceleration-structure
    barriers in every stress run, and the default `reflectionSource` is RT.
  - Visual: `wind_test` (convolution) and the pyramid scratch level both render correctly in Debug;
    `ssr_bronze_palms` renders correctly in Release.

  **Read the flip-trace before quoting it.** Both arms print ~26-33k `[flip-miss]` lines, which
  looks like a wall of failure and is not one: every one of them names `Ocean.Displacement` /
  `Ocean.PrevDisplacement` / the DLSS hand-off set (`Deferred[i].{Scene,Depth,GBVelocity,ObjectID,
  DlssOutput}`), i.e. ping-pong and Streamline-owned resources, and NOT ONE names a bloom or an AS
  resource. The control that settles it is free and already in hand: the two arms differ only in
  the bloom program, and the flip-miss population does not move between them. `--canonical-check`
  likewise reports the same 2-4 resting-state entries in both arms (`VSM.PerPageDirty`,
  `VSM.PageArgCount`, `Ocean.SurfSim{Wave,Foam}{A,B}`) — alternating ping-pong ends, none of them
  a resource either step declares.

- **ORDER CHANGED, on the plan's own advice.** Detachability already said "R5 wants R6's
  `FrameDecisions` to exist first, or the phase builders will each take eight loose bools", and
  that is exactly what happened: with the decisions still loose, `GraphBuild` would have carried
  rtSupported/rtDebugView/rtReflect/rtBuildAS/clearReflections/vsmActive/willDlss as fields and the
  phases would have been reading frame state out of a graph-construction struct. So R6 ran first,
  then R5, then R7.

- R6 DONE (uncommitted). `FrameDecisions` + `DecideFrame()`. Eleven fields: rtSupported,
  rtDebugView, rtReflect, rtBuildAS, clearReflections, glassRefl, ssrTemporal, ssrHiz, vsmActive,
  vsmSkipUpdate, willDlss. Five of them were MEMBERS (`rtReflectActive_`, `glassReflActive_`,
  `ssrTemporalActive_`, `ssrHizActive_`, `vsmSkipUpdate_`) sitting in the same list as the SSR and
  VSM histories, which is the confusion the step exists to end: a member's topic never told you
  whether clearing it on a level switch was required, forbidden or a bug — its lifetime tells you
  all three. The rest were locals threaded through 1400 lines of graph construction.
  It stays a MEMBER (not a `Render()` local) for a concrete reason: the AddPass2 builders read it,
  and they capture `this` and run later, from ExecuteParallel.
  Two findings worth keeping:
  1. **`vsmDirectional` and `vsmActive` were the SAME predicate** —
     `render::VsmActive() && vsm && vsm->IsAllocated()` — computed twice, ~180 lines apart, under
     two names. One field now.
  2. **Three blocks moved EARLIER as a consequence, and had to be checked for it**: the
     skip-when-still computation (and the cross-frame counters behind it), `vsmActive`, and
     `willDlss`. All three are safe because nothing between the old and new positions can move
     their inputs — the graph construction in between only REGISTERS passes; every builder runs
     afterwards, inside ExecuteParallel. That fact is what makes the whole of R5 safe too.

- R5 DONE (uncommitted). `sources/app/scene/SceneRenderer_Graph.cpp` (1477 lines) holds the seven
  phase builders exactly as the plan named them: BuildPrologue, BuildShadows, BuildGBufferAndAo,
  BuildLighting, BuildReflections, BuildForwardAndEditor, BuildPost. `Render()` opens a
  `GraphBuild`, calls them in schedule order and executes. **SceneRenderer.cpp: 1770 → 428.**
  `GraphBuild` carries the graph, the two deferred sets, and the FOURTEEN pass indices that
  actually cross a phase boundary (pBuildAS, pWetness, pShoreDepth, pShadow, pSpotShadow,
  pPointShadow, pGbuf, pVsmPageRender, pHzb, pGtao, pSky, pCompose, pGlassReflect,
  pSelectionOutline). The other twelve indices are used inside one phase and stayed locals — which
  is what makes the field list readable as the dependency graph BETWEEN phases instead of a bag of
  size_t. The CL-group constraint the plan called out holds: the compute group opens and closes
  inside BuildPrologue, the reflection group inside BuildReflections, the tonemap group inside
  BuildPost; no bracket straddles a boundary.
  **PROVED, not asserted — zero unexplained differences.** A checker reverses the three mechanical
  transforms (the `gb.` prefix, the declarations that became assignments, the alias lines) and
  diffs each phase against the pre-R5 ranges: 145 + 98 + 224 + 205 + 282 + 148 + 268 = 1370 lines
  moved, **0 diff lines in all seven**. (The checker's FIRST version reported 32 differences, every
  one of them its own fault — it was normalising phase-LOCAL indices too. Worth remembering: when
  a verification tool disagrees with a mechanical transform, suspect the tool first.)

- R7 DONE (uncommitted). The header's state is now three blocks sorted by LIFETIME, each with the
  reason it exists: SUB-OBJECTS (resources_, bloom_, rtAs_, reflectionHistory_, the two graphs),
  CROSS-FRAME STATE (pre-exposure pair, exposure stamp, both SSR histories, the GTAO history, the
  VSM stillness counters), PER-LEVEL/ONE-SHOT (rtFailureLogged_), and VALID-ONLY-DURING-Render
  (frame_, and decisions_ up with FrameDecisions). The four GTAO history members and `bloom_`,
  which were stranded mid-header among the pass declarations, moved into their block.
  Header 562 → 518 lines, and the member count is down from 57 to 24 across R3-R7.

  Gate for R5+R6+R7 together:
  - Both configs 0 errors / 0 warnings, `SceneRenderer_Graph.obj` fresh in both.
  - **The barrier program is IDENTICAL, and that is measured, not inferred**: the three stress runs
    emit exactly the counts the R3/R4 gate emitted — 7581 (VSM) / 6461 (legacy) / 4998 (plain), and
    104 / 104 / 71 acceleration-structure barriers. Not "similar": the same integers.
  - Debug `--scene-stress-gbv=20 --barrier-cmp --canonical-check` CLEAN in both shadow modes;
    `--scene-stress=12` CLEAN.
  - `--barrier-cache-verify` (recompiles every frame and diffs against the cache): SILENT. This is
    the step's real proof, because R5 changes no declaration.
  - `--barrier-flip-trace` on both bloom methods: zero MISSING / EmitPoint / comparator lines, and
    the flip-miss population is the same ocean-ping-pong + DLSS-handoff set as before.
  - Visual: `wind_test` (convolution), the pyramid scratch level, and Release `ssr_bronze_palms`
    all render correctly.
  - **Release A/B on `ssr_bronze_palms`: 351 pixels differing by >16 out of 3.69M, max delta 46,
    frame means equal to 0.01.** Set that against R2's TU split, which moved 11.5k-23k pixels on the
    same kind of comparison: three new phase functions in a new translation unit barely perturbed
    the optimiser at all. The Debug A/B on `wind_test` is inside its (enormous, 52k-119k) same-binary
    noise floor, as that level's pixel A/B always is — quoted only so the number is on record.

  **Where it landed vs the plan's table**: SceneRenderer.cpp 428 (planned 500), _Graph 1477 (1500),
  _Lighting 754 (900), _Reflections 901 (700), _Shadows 642 (600), _Post 606 (600), _Geometry 526
  (500), SceneRenderInternal.h 338 (350), BloomRenderer 1516 (1100), RtSceneAs 338 (250).
  The 6703-line file is now nine files, none over 1500, and the biggest one is a list of passes.
