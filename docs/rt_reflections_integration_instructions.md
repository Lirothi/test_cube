# Hardware Ray-Traced Reflections — Integration Plan (for AI executors)

This is an execution plan for adding **hardware ray-traced (DXR) reflections** to the
renderer. It is written so that an AI agent can pick up any **single step**, implement it
against the stated **interface contract**, and verify it in isolation. Steps declare their
dependencies explicitly; steps with no shared dependency may be executed in parallel.

---

## 1. Goal & non-goals

**Goal:** Replace/augment the existing screen-space reflections (SSR) with DXR reflections
that can reflect off-screen geometry, integrated behind a runtime toggle, degrading cleanly
to SSR when RT is unavailable.

**Recommended architecture (decision record):** **DXR 1.1 inline ray tracing (`RayQuery`)
inside a compute shader.** Rationale:
- Mirrors the existing SSR compute pass (`Pass_SSR` in `sources/app/scene/SceneRenderer.cpp`),
  so it slots into the `RenderGraph` the same way.
- Needs only the **TLAS as an SRV** plus the **GBuffer** (already present); **no** state
  objects, **no** `DispatchRays`, **no** shader binding table.
- Tier-1 (sharp reflections sampling existing HDR scene color) avoids the renderer's biggest
  gap — it is **not bindless** — entirely. Bindless is only required for Tier-2 hit shading.

**Non-goals (initially):** reflective ocean/water (`OceanRenderable`, GPU-displaced) and
refractive glass (`TransparentStaticMesh`) — these stay on their current paths. Multi-bounce
and RT global illumination are out of scope.

**Requires at runtime:** `D3D12_RAYTRACING_TIER_1_1` and Shader Model 6.5. Everything must be
runtime-gated; when unsupported, the SSR path remains the active fallback.

---

## 2. Conventions for the executing AI

- **Build (Release x64):**
  `vcvars64.bat` → `msbuild test_cube.sln /p:Configuration=Release /p:Platform=x64 /m`.
  (VS path: `C:\Program Files\Microsoft Visual Studio\18\Professional\...`.)
- **Headless harnesses** are dispatched from `sources/app/main.cpp` by substring-matching
  `lpCmdLine` (see `tasksystem-stress`, `renderer-submission-stress`, `textmanager-benchmark`).
  The exe is **Windows-subsystem**, so run with `Start-Process -Wait` and report via exit code
  / a file written to CWD (do not rely on stdout).
- **Shaders compile at runtime** from `shaders/*.hlsl` via DXC, with a file-watcher hot-reload
  (`sources/materials/Material.cpp`, `HotReloadIfPending`). Root signatures are declared
  **inline** in HLSL as `[RootSignature("...")]`.
- **Commit policy:** one logical step per commit, and **only commit when the human asks**.
  Branch off `master` first if work lands on it.
- **Visual claims:** never conclude a visual feature is correct/broken from a screenshot
  without the human confirming the capture analysis. Prefer objective signals (debug viewer
  readouts, GPU validation layer, exit codes) where possible.
- **Code style:** match the surrounding file (naming, comment density, `Microsoft::WRL::ComPtr`,
  `ThrowIfFailed`). Keep the non-RT build path byte-identical when the RT toggle is off.
- **Global invariant (every step must preserve):** with the RT toggle OFF *or* on
  RT-incapable hardware, the frame is identical to today and there are zero new per-frame
  allocations or barriers on the hot path.

---

## 3. Dependency graph

```
S1 caps/device5/cmdlist4 ─┬─> S3 BLAS ──> S4 TLAS ──> S5 AS build pass ─┬─> S6 RT debug view ─> S7 RT reflect (Tier1) ─> S8 toggle/fallback
S2 DXC cs_6_5/RayQuery ───┘                                             │                              │
                                                                        │                              ├─> S11 denoise/temporal
S1 ─> S9 bindless geo/material table ─────────────────────────────────────────> S10 hit shading (Tier2)│
                                                                                       (needs S7,S9)    ├─> S12 perf (half-res/checkerboard)
S5,S7 ─> S13 robustness/exclusions/VRAM                                                                 ┘
```

**Parallelizable at start:** `S1`, `S2`, and `S9` (design). After `S1`: `S3` and `S9`
implementation. `S4` can be written against `S3`'s contract before `S3` lands.
**Critical path to first visible result:** S1 → S3 → S4 → S5 → S6 → S7.

**Suggested milestones:**
- **M-Smoke** = S1 + S2 (+ optional S0 harness): RT path detected, AS builds validate headless.
- **M1 (Tier-1, ship-able prototype):** S1–S8. Sharp RT reflections reusing SSR buffer+compose.
- **M2 (Tier-2, production):** + S9–S13. Shaded off-screen hits, perf-tuned, hardened.
- **M3 (glossy):** + S14. Clean roughness-driven glossy via DLSS Ray Reconstruction (the
  hand-rolled S11(a) denoiser proved inadequate; DLSS-RR is the real fix).

---

## 4. Step template

Each step below provides: **Depends**, **Goal**, **Touch** (files), **Implement**,
**Interface contract** (what to expose so dependents can proceed in parallel), **Done-when**
(objective acceptance), **Verify** (how to check in isolation).

---

### S0 — (Optional) Headless RT smoke harness
- **Depends:** none (but most useful after S1/S3/S4 land; build it early as a stub).
- **Goal:** A `--rt-smoke` headless harness that creates a device, reports RT support, and
  (once S3/S4 exist) builds a BLAS+TLAS over one triangle and validates no device-removal.
- **Touch:** `sources/app/main.cpp` (dispatch), new `sources/rendering/rt/RtSmoke.cpp`.
- **Implement:** Follow `RunRendererSubmissionStress` for device/queue/cmdlist bring-up. Print
  `RaytracingTier`, build trivial AS, execute, wait on fence, write `rt_smoke.txt` with PASS/FAIL.
- **Interface contract:** `int RunRtSmoke(const char* outPath);`
- **Done-when:** Exit code 0 + `rt_smoke.txt: PASS` on RT HW; graceful `SKIP` (exit 0) when
  `RaytracingTier == 0`.
- **Verify:** `Start-Process test_cube.exe "--rt-smoke" -Wait`; read file.

---

### S1 — RT capability detection + device/command-list upgrade
- **Depends:** none.
- **Goal:** Detect RT support and expose the `Device5`/`CommandList4` interfaces DXR needs.
- **Touch:** `sources/rendering/core/GraphicsDevice.cpp/.h` (device creation at
  `GraphicsDevice.cpp:20`), `sources/rendering/core/Renderer.h/.cpp`.
- **Implement:**
  - After `D3D12CreateDevice`, `QueryInterface` for `ID3D12Device5` (store; null if absent).
  - `CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, ...)`; record
    `options5.RaytracingTier`. Treat `>= D3D12_RAYTRACING_TIER_1_1` as supported.
  - Provide a helper to obtain `ID3D12GraphicsCommandList4` from a recorded command list
    (QI once and cache, or QI per-pass — measure; QI is cheap).
- **Interface contract (exposed on `Renderer`):**
  ```cpp
  bool                       IsRaytracingSupported() const;     // tier >= 1_1
  ID3D12Device5*             GetDevice5() const;                // null if unsupported
  ID3D12GraphicsCommandList4* AsCmdList4(ID3D12GraphicsCommandList*) const;
  ```
- **Done-when:** Builds; `IsRaytracingSupported()` returns true on RT HW, false otherwise;
  no behavior change anywhere yet.
- **Verify:** Build Release x64. Optionally log the tier at startup; confirm via S0 harness.

---

### S2 — DXC RayQuery / SM6.5 compute compile path
- **Depends:** none.
- **Goal:** Ensure a compute shader using `RayQuery` compiles at `cs_6_5` through the existing
  DXC path.
- **Touch:** `sources/materials/Material.cpp` (`CompileDXC`, `BuildProfile`, `QueryMaxShaderModel`).
- **Implement:**
  - Confirm `BuildProfile("cs", sm)` selects `cs_6_5` (or higher) when the device reports
    `>= 6_5`; clamp to `6_5` if RT is requested. No `lib_*` profile is needed for the inline
    path.
  - Add a tiny `shaders/rt_compile_probe.hlsl` (a CS that declares
    `RaytracingAccelerationStructure` + `RayQuery<RAY_FLAG_FORCE_OPAQUE>` and calls
    `TraceRayInline`/`Proceed`) used only to validate compilation.
- **Interface contract:** none new; existing compute `Material` builds a `cs_6_5` RayQuery shader.
- **Done-when:** `rt_compile_probe.hlsl` compiles via DXC (no fallback to D3DCompile SM5).
- **Verify:** Offline `dxc -T cs_6_5 -E CSMain shaders/rt_compile_probe.hlsl` returns 0; and the
  runtime material build logs no DXC failure.

---

### S3 — BLAS builder (per mesh)
- **Depends:** S1.
- **Goal:** Build a bottom-level acceleration structure for each `Mesh` from its resident VB/IB.
- **Touch:** new `sources/rendering/rt/AccelerationStructure.{h,cpp}`; read
  `sources/rendering/meshes/Mesh.h` (`GetVertexBufferResource`, `GetIndexBufferResource`,
  `GetVertexStride`, `GetIndexFormat`, `GetIndexCount`).
- **Implement:**
  - `D3D12_RAYTRACING_GEOMETRY_DESC` (triangles): `VertexBuffer.StartAddress = VB VA + 0`
    (position is at offset 0 of `VertexPNTUV`), `StrideInBytes = GetVertexStride()`,
    `VertexFormat = R32G32B32_FLOAT`, `VertexCount`, `IndexBuffer = IB VA`,
    `IndexFormat = GetIndexFormat()`, `IndexCount = GetIndexCount()`,
    `Flags = OPAQUE`.
  - `GetRaytracingAccelerationStructurePrebuildInfo` (on `Device5`) → allocate result buffer
    (`D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE`, `ALLOW_UNORDERED_ACCESS`) and a
    scratch buffer.
  - `BuildRaytracingAccelerationStructure` on `CmdList4` + UAV barrier. Use
    `PREFER_FAST_TRACE`; since geometry is static, build once and **cache** on the mesh.
    (Optional: compaction.)
- **Interface contract:**
  ```cpp
  struct Blas { ComPtr<ID3D12Resource> result; D3D12_GPU_VIRTUAL_ADDRESS Address() const; };
  // Builds (or returns cached) BLAS for a mesh on the given cmdlist4.
  const Blas& AccelerationStructureManager::GetOrBuildBlas(Mesh* mesh, ID3D12GraphicsCommandList4*);
  ```
- **Done-when:** BLAS builds without GPU validation errors; `Address()` non-zero; result
  survives past the build (kept alive).
- **Verify:** Enable the D3D12 debug layer + GPU-based validation; build a BLAS via S0 and
  confirm no errors. Scratch is freed/recycled after the build fence.

---

### S4 — TLAS builder (per frame, from scene instances)
- **Depends:** S3 (interface only — can be coded against the `Blas` contract in parallel).
- **Goal:** Each frame, assemble a top-level AS from all reflective instances.
- **Touch:** `sources/rendering/rt/AccelerationStructure.{h,cpp}`; read instance transforms in
  `sources/rendering/meshes/InstanceBuffer.h` (`InstanceData.world`) and the scene enumeration
  in `GpuInstancedModels`/`SceneRenderer`.
- **Implement:**
  - For each instance: one `D3D12_RAYTRACING_INSTANCE_DESC` — `Transform` = 3×4 row-major from
    `InstanceData.world` (the project is `#pragma pack_matrix(row_major)`; verify handedness),
    `InstanceMask = 0xFF`, `AccelerationStructure = GetOrBuildBlas(mesh).Address()`,
    `InstanceID`/`InstanceContributionToHitGroupIndex` = an index you can map back to the mesh
    (needed later by S10).
  - Upload instance descs to a per-frame UPLOAD buffer; build TLAS (prebuild info → result +
    scratch) on `CmdList4`; UAV barrier.
  - Create a TLAS **SRV** (`ViewDimension = RAYTRACING_ACCELERATION_STRUCTURE`,
    `RaytracingAccelerationStructure.Location = TLAS VA`) in the renderer's CPU SRV heap so it
    can be staged into a descriptor table like any other SRV.
- **Interface contract:**
  ```cpp
  void  AccelerationStructureManager::BuildTlas(span<InstanceEntry>, ID3D12GraphicsCommandList4*, UINT frameIndex);
  D3D12_CPU_DESCRIPTOR_HANDLE AccelerationStructureManager::TlasSrvCpu(UINT frameIndex) const;
  ```
- **Done-when:** TLAS builds each frame with N instances; SRV is valid; no validation errors.
- **Verify:** S0 harness builds a 2-instance TLAS; debug-layer clean. Instance count matches scene.

---

### S5 — Acceleration-structure RenderGraph pass + lifetime/scratch management
- **Depends:** S3, S4.
- **Goal:** A `RenderGraph` pass that (re)builds AS before reflections, with correct barriers
  and resource lifetimes.
- **Touch:** `sources/app/scene/SceneRenderer.cpp` (add a pass, mirror `Pass_SSR` registration
  at `SceneRenderer.cpp:342`), `sources/rendering/core/RenderPass.h` (add
  `RenderPass::Main_BuildAS`), `AccelerationStructureManager`.
- **Implement:**
  - Add `Main_BuildAS` as the first pass when RT is enabled (BLAS first-frame builds + per-frame
    TLAS). Acquire `CmdList4` via `Renderer::AsCmdList4`.
  - **Gotcha:** AS buffers must stay in `RAYTRACING_ACCELERATION_STRUCTURE` state and must not be
    routed through the normal `ResourceStateTracker` transition logic — exclude them or mark
    them so the RenderGraph never transitions them. Document this in the pass.
  - Scratch buffers: pool and keep alive until the build's fence; recycle (see retired-buffer
    pattern used elsewhere, e.g. TextManager's retired index buffers).
- **Interface contract:** the pass produces a valid TLAS SRV for the frame; downstream passes
  depend on `Main_BuildAS`.
- **Done-when:** Frame renders unchanged visually (no consumer yet), debug layer clean, no
  per-frame leak (VRAM stable over time).
- **Verify:** Run the app a few minutes with RT on; confirm steady VRAM (debug layer / PIX) and
  no validation spew. Confirm the toggle-off path skips the pass entirely.

---

### S6 — RT hit/visibility debug visualization
- **Depends:** S2, S4, S5.
- **Goal:** De-risk RayQuery + AS by visualizing ray hits before building real reflections.
- **Touch:** new `shaders/rt_debug_cs.hlsl`, a debug pass in `SceneRenderer.cpp`, reuse
  `sources/rendering/debug/TextureDebugViewer`.
- **Implement:** A `cs_6_5` shader: reconstruct world pos from depth (reuse `ReconstructPosWS`
  in `shaders/utils.hlsl`), shoot a ray along the **reflection vector** (`reflect(-V, N)` using
  `gb1` normal as in `compose_cs.hlsl`), `RayQuery<RAY_FLAG_FORCE_OPAQUE>` against the TLAS,
  write hit distance / hit-normal / miss to a debug UAV. Bind TLAS SRV + GBuffer SRVs via the
  staged descriptor table (follow `RecordComputeDispatch` usage in `Pass_SSR`).
- **Interface contract:** none new (debug-only).
- **Done-when:** The debug target shows plausible hit distances on reflective surfaces and
  "miss" toward sky.
- **Verify:** View via `TextureDebugViewer`. **Confirm the visual interpretation with the human
  before declaring success.**

---

### S7 — RT reflection pass, Tier-1 (no bindless)
- **Depends:** S2, S5, S6.
- **Goal:** Produce a usable reflection result and feed it into the **existing** compose.
- **Touch:** new `shaders/rt_reflections_cs.hlsl`, `SceneRenderer.cpp` (new
  `RenderPass::Main_RTReflections`), reuse the `ssr`/`ssrBlur` targets in
  `sources/rendering/core/RenderTargetManager.h`.
- **Implement:**
  - Spawn the reflection ray from GBuffer surface (world pos from depth, normal+roughness from
    `gb1`). For roughness, Tier-1 may trace a single mirror ray (defer glossy to S10/S12).
  - On **hit**: compute hit world pos from ray T, reproject to screen; if visible in this
    frame's depth, sample the HDR `light`/`scene` buffer for radiance; otherwise fall back to
    skybox (same fallback `compose_cs.hlsl` already uses). On **miss**: skybox.
  - Write **premultiplied** reflection (rgb, coverage) into the `ssr` UAV so the existing
    `Pass_SSR_Blur` + `Pass_Compose` (`compose_cs.hlsl` t6 = `SSRBlur`) consume it unchanged.
  - Register the pass to run **instead of** `Pass_SSR` when RT reflections are enabled; keep the
    blur + compose passes as-is.
- **Interface contract:** writes the same buffer SSR writes; downstream unchanged.
- **Done-when:** With RT on, reflections show off-screen geometry that SSR cannot (e.g., objects
  behind the camera reflected in a floor); with RT off, output is byte-identical to today.
- **Verify:** A/B the toggle in a scene with a known off-screen occluder. **Confirm visually
  with the human.** Add `CPU_SCOPE`/`GPU_SCOPE` (`ProfilerScopes`) like `Pass_SSR`.

---

### S8 — Toggle, fallback, and dev UI
- **Depends:** S7.
- **Goal:** Make RT reflections a first-class, safe-to-ship option.
- **Touch:** settings struct used by `SceneRenderer` (`frame_->settings`, see
  `ssrTechnique` usage at `SceneRenderer.cpp:1143`), `sources/app/DeveloperWindow.cpp`.
- **Implement:** A reflection-source enum `{ Off, SSR, RT }`. Selecting `RT` requires
  `IsRaytracingSupported()`; otherwise force-fall back to `SSR` and surface why in the dev UI.
- **Interface contract:** `settings.reflectionSource`.
- **Done-when:** Toggling RT↔SSR↔Off works live; on non-RT HW the RT option is disabled/greyed.
- **Verify:** Manual toggle; confirm fallback on a machine/driver without RT (or by forcing
  `IsRaytracingSupported()` false).

---

### S9 — Bindless geometry + material table  *(Tier-2 enabler; heaviest infra)*
- **Depends:** S1. Parallel to S3–S8.
- **Goal:** Give RT shaders indexed access to per-hit geometry attributes and materials —
  the project is **not** bindless today, so this is net-new.
- **Touch:** `sources/rendering/descriptors/DescriptorAllocator.h` and the descriptor/heap
  system; `sources/materials/MaterialData.cpp`; mesh registration.
- **Implement:** A persistent, growable SRV array (a bindless heap segment) holding each mesh's
  VB and IB as `ByteAddressBuffer`/structured SRVs, plus a material/texture table, indexed by
  the `InstanceID`/contribution index set in S4. Expose root-signature access via
  `SM6.6 ResourceDescriptorHeap[]` (if `ResourceBindingTier`/SM permit) or an unbounded
  descriptor-table array (`SRV(t0, numDescriptors=unbounded)`).
- **Interface contract:** `uint geometryIndex` per TLAS instance → `{VB srv, IB srv, materialIndex}`.
- **Done-when:** A CS can fetch hit-triangle normal/UV and the hit material from indices only.
- **Verify:** Extend the S6 debug shader to output interpolated hit normal/albedo; confirm it
  matches the reflected surface. Debug layer clean.

---

### S10 — Tier-2 hit shading (glossy, lit)
- **Depends:** S7, S9.
- **Goal:** Shade ray hits properly instead of sampling screen color.
- **Touch:** `shaders/rt_reflections_cs.hlsl`.
- **Implement:** At hit, fetch attributes (S9), evaluate material BRDF with scene lights
  (reuse lighting helpers; optional shadow ray via a second `RayQuery`). Importance-sample the
  reflection lobe by roughness (multiple rays or a single jittered ray + denoise). Keep the
  screen-color sample as a cheap fast path / first-bounce radiance where valid.
- **Done-when:** Hidden surfaces reflect with correct lighting (not just screen color); glossy
  surfaces show roughness-appropriate blur.
- **Verify:** Compare a hidden-geometry reflection vs. ground truth offline render. Confirm
  visually with the human.

---

### S11 — Temporal denoise
- **Depends:** S7 (full quality needs S10).
- **Goal:** Stabilize the noisy RT result.
- **Touch:** new denoise CS or Streamline integration; reuse `gbVelocity` (motion vectors).
- **Implement (pick one):** (a) hand-rolled temporal accumulation reprojected via `gbVelocity`
  + variance-guided spatial blur (extend `ssr_blur_cs.hlsl`); or (b) **Streamline DLSS Ray
  Reconstruction** — Streamline is already vendored (`third_party/streamline`) and the guide
  buffers it needs (normals, roughness, depth, motion) all exist. Note DLSS-RR replaces the
  separate denoiser and interacts with the existing DLSS upscale ordering.
- **Done-when:** Reflections are temporally stable under camera motion without excessive ghosting.
- **Verify:** Motion sequence; confirm noise/ghosting with the human. Watch perf cost.
- **STATUS (attempted):** Option (a) was implemented (ping-pong history + motion reprojection +
  3×3 neighbourhood clamp, with a per-frame-jittered single glossy ray) and proved **insufficient**:
  at 1 sample/pixel the result is too noisy for a simple accumulator, and DLSS jitter (always on
  here) drives constant sub-pixel motion that keeps partially resetting the history → visible
  "dancing"/noise. The infra (history textures, `Main_RTDenoise` pass, `JitterReflection`/
  `frameSeed` in `rt_reflections_cs.hlsl`) is left in place but **inert** (glossy jitter disabled →
  sharp mirror ray; denoise `alpha=1` → pass-through), so RT reflections are clean+sharp. Clean
  glossy is deferred to **S14 (DLSS Ray Reconstruction)** — option (b) done as a dedicated step.

---

### S12 — Performance (resolution + ray budget)
- **Depends:** S7.
- **Goal:** Make the pass affordable.
- **Touch:** `rt_reflections_cs.hlsl`, pass dispatch, `RenderTargetManager` (reuse the existing
  SSR-resolution targets — `ssrWidth/ssrHeight` already separate from render res).
- **Implement:** Half-res or checkerboard tracing + upscale; skip rays on near-mirror-irrelevant
  pixels (low gloss); cap rays per pixel; thread-group tuning. The SSR targets are already
  lower-res, so reuse that plumbing.
- **Done-when:** Pass cost within an agreed GPU budget at target resolution; documented.
- **Verify:** GPU timing via `GPU_SCOPE`/PIX; record before/after.

---

### S13 — Robustness, exclusions, VRAM
- **Depends:** S5, S7.
- **Goal:** Production hardening.
- **Touch:** AS manager, scene integration, docs.
- **Implement:** Exclude/define behavior for ocean + transparent/glass (kept on existing paths);
  handle scene add/remove (BLAS cache invalidation, TLAS instance churn); VRAM budgeting +
  graceful disable if AS allocation fails; device-removed handling.
- **Done-when:** Stable over long sessions and scene changes; clean disable under memory pressure.
- **Verify:** Soak test; force AS alloc failure path; confirm fallback to SSR.

---

### S14 — Glossy reflections via DLSS Ray Reconstruction *(the real denoiser; replaces S11(a))*
- **Depends:** S10 (Tier-2 hit shading), S11 (supersedes its hand-rolled option (a)).
- **Why this step exists:** S11(a) — hand-rolled temporal accumulation over a single roughness-
  jittered ray — was implemented and is **visually inadequate**: 1 sample/pixel is too noisy for a
  simple history+clamp accumulator, and the always-on DLSS jitter drives constant sub-pixel motion
  that keeps partially resetting the history → "dancing"/noise. Clean glossy from a sparse RT
  signal is exactly what **DLSS Ray Reconstruction (DLSS-RR / DLSS-D)** is built for. Until this
  lands, ship the **sharp** mirror reflection (clean + stable), not the noisy accumulator.
- **Goal:** Clean, temporally-stable, roughness-appropriate **glossy** reflections by feeding the
  (re-enabled) noisy jittered RT reflection through DLSS Ray Reconstruction.
- **Touch:** `sources/rendering/core/DlssHandler.*` + `third_party/streamline` (load the RR/DLSS-D
  feature, tag guide buffers); `rt_reflections_cs.hlsl` (re-enable the jittered glossy path — it is
  already present, gated off); `SceneRenderer` post chain (ordering vs the existing DLSS upscale +
  tonemap); retire/bypass the inert `Main_RTDenoise` + `ReflectionHistory`.
- **Implement:**
  - Load DLSS-RR via Streamline; gate on its availability **and** `IsRaytracingSupported()`.
  - Re-enable the per-frame roughness-jittered single glossy ray in `rt_reflections_cs.hlsl`
    (the `JitterReflection`/`frameSeed` path) so RR receives an unbiased noisy reflection. Mirror
    surfaces still trace sharp.
  - Tag the guide buffers RR needs through Streamline: world/view normals (`gb1`), roughness
    (`gb0.a`), depth, motion vectors (`gbVelocity`), the noisy reflection (color-in) and the
    denoised output; supply the existing camera **jitter offset** + matrices (reuse the
    `DlssHandler` constants — note `motion = currUv - prevUv`, `mvecScale=1`, jitter currently
    flagged not-jittered).
  - **Ordering (the plan's open question — resolve here):** start narrow — have RR denoise **only
    the reflection buffer** (SSR/render res), leaving compose → tonemap → existing DLSS upscale
    unchanged. Only escalate to a full DLSS-RR path (denoise+upscale replacing the separate DLSS
    upscale) if RR requires the whole-frame inputs. Document whichever lands.
  - Remove or setting-gate the hand-rolled denoiser once RR is the denoiser (keep as a non-RR
    fallback only if cheap to maintain).
  - **Fallback:** if DLSS-RR is unavailable (HW/driver), keep the **sharp** mirror reflection (no
    glossy) — never ship the noisy hand-rolled output.
- **Interface contract:** `settings.reflectionSource == RT` yields clean roughness-driven glossy
  when RR is available; sharp RT otherwise. No new toggle needed beyond a possible "RR on/off" dev flag.
- **Done-when:** Glossy/rough surfaces show clean, roughness-appropriate, temporally-stable
  reflections under **camera and object** motion — no dancing, no excessive ghosting; degrades to
  sharp RT (or SSR) when RR is unavailable.
- **Verify:** Motion sequence; A/B vs SSR and vs sharp RT; **confirm visually with the human**;
  Streamline log + debug layer clean; record perf delta vs sharp RT.
- **Risks:** DLSS-RR ↔ existing DLSS-upscale ordering/interaction; RR availability + driver
  version; guide-buffer correctness (motion-vector + jitter conventions most error-prone); RR's
  expected buffer formats/resolutions/scales.

---

## 5. Global acceptance criteria

1. RT toggle **off** or unsupported HW → frame is byte-identical to today; no new per-frame cost.
2. RT **on** → reflections include off-screen geometry SSR cannot reproduce.
3. Debug layer + GPU-based validation clean with RT on.
4. Stable VRAM and no validation errors over a multi-minute soak.
5. All shader changes compile via DXC at the intended SM (no SM5 fallback for RT shaders).

## 6. Key risks / open questions (resolve early)

- **Descriptor model is not bindless** — the single biggest risk; isolated to S9/S10. Tier-1
  (S1–S8) is designed to avoid it.
- **AS state handling in the RenderGraph** — AS resources must bypass the normal state tracker
  (S5). Get this right before building consumers.
- **Compute root-signature routing of the TLAS SRV** — confirm `RecordComputeDispatch` /
  `ComputeDispatch.h` can stage a `RAYTRACING_ACCELERATION_STRUCTURE` SRV into the table; if
  not, the RT pass needs a small bespoke dispatch path.
- **DLSS/RR ordering** — if S11 uses Streamline RR, reconcile with the existing DLSS upscale and
  tonemap ordering.
- **Transform handedness/row-major** — verify `InstanceData.world` → instance `Transform[3][4]`
  is correct (the codebase is row-major).
