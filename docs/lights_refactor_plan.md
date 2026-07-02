# Spot & Point Light Shadow Refactor — Executor Plan

This document is written to be executed by an AI coding agent. Read the
**Executor Guide** first, then do the parts/steps in order. Each step must leave
**both** build configurations green and, unless a step is explicitly the behavioral
flip, must leave runtime behavior unchanged.

- **Part A** — Spot lights: remove the total cap; cap only *shadowed* spots per frame
  (`kMaxShadowedSpotLights`, default **8**), chosen by distance from camera.
- **Part B** — Point lights: add omnidirectional shadows (they currently have none),
  capped at *shadowed* count per frame (`kMaxShadowedPointLights`, default **4**),
  chosen by distance from camera.

Parts A and B are independent (different light types) and share the same
"distance-selected, slot-mapped, sentinel-in-buffer" pattern. Do Part A first — its
selection/slot machinery is the template Part B mirrors.

---

## Executor Guide (read first)

**Repo:** `D:\Programming\test_cube`. Shared conventions live in
`docs/level_editor_HANDOFF.md` — read it for the gating/CRLF/build rules; the essentials
are repeated here.

**Build (run BOTH after every step):** use the PowerShell tool (not bash — bash mangles
MSBuild `/t:` `/p:` switches).
```
& "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe" `
    "D:\Programming\test_cube\test_cube.sln" /t:Build /p:Configuration=Debug   /p:Platform=x64 /m /nologo "/clp:ErrorsOnly;Summary"
& "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe" `
    "D:\Programming\test_cube\test_cube.sln" /t:Build /p:Configuration=Release /p:Platform=x64 /m /nologo "/clp:ErrorsOnly;Summary"
```
`Debug|x64` compiles with `/WX` (warnings = errors) and `WITH_EDITOR=1`. `Release|x64`
is the non-editor build. Both must report `0 Warning(s) 0 Error(s)`.

**This is all ENGINE code — NOT `WITH_EDITOR`-gated.** LightManager, Scene,
SceneRenderer, RenderTargetManager, Renderer and the shaders ship in Release. Behavior
must be identical in both configs; the editor only benefits indirectly (it can spawn
any number of lights). Do not wrap any of this in `#if WITH_EDITOR`.

**Line endings:** all `.cpp/.h/.hlsl` in this repo are **CRLF**; markdown is LF. The
Write tool creates new files as **LF**, so any new source/shader file must be converted
to CRLF. Verify every touched file with a lone-LF check (must be 0):
```powershell
$b=[IO.File]::ReadAllBytes($path); $n=0; for($i=0;$i -lt $b.Length;$i++){ if($b[$i]-eq 10 -and ($i-eq 0 -or $b[$i-1]-ne 13)){$n++} }; "$path loneLF=$n"
```

**Shaders compile at RUNTIME** (`sources/materials/Material.cpp` →
`CompileWithIncludes` / `D3DCompileFromFile`, with hot-reload). The C++ build will
**not** catch a shader syntax error, a wrong `StructureByteStride`, or a root-signature
mismatch. Any step that edits an `.hlsl` file, changes an SRV-table `numDescriptors`, or
changes a GPU struct size **must be validated by running the app**, not just by
building. Shader edits hot-reload in a running session; struct/table/root-signature
changes generally need a relaunch.

**Verification per step:** (1) both builds `0/0`; (2) CRLF check on every touched file;
(3) for steps touching shaders / GPU structs / render passes, **run the app** and
confirm the acceptance criteria visually; (4) run `x64\Debug\test_cube.exe --scene-stress`
(exit 0 = clean) after render-path changes, to catch device-removal regressions.

**Do NOT commit.** The user commits. Leave the working tree with your changes staged for
their review. Update the memory file `spot-lights-refactor-plan.md` as you complete
steps.

**Non-negotiable safety rule (learned the hard way):** any code that frees/reallocates a
GPU resource (light buffers, shadow atlases) must run only when the GPU is idle
(`Renderer::WaitForPreviousFrame()` == full `WaitForGpuIdle`). Reallocating a resource
while a frame that references it is in flight causes `DXGI_ERROR_DEVICE_HUNG`. The
editor light-mutation path (`EnvironmentRuntime::RebuildLights`) already idles + pre-grows
the light buffers for this reason; keep that invariant.

---

## Current State (verified)

- `LightManager::kMaxSpotLights = 4` (`sources/rendering/lighting/LightManager.h:17`) is
  **both** the total spot cap and the shadow-atlas size — the two concepts are fused.
- `LightManager::UpdateSpotLightCache()` clamps: `cachedSpotLightCount_ =
  min(spotLights_.size(), kMaxSpotLights)`. Extra spots are silently dropped.
- The spot structured buffer holds `GetSpotLightCount()` entries and grows on demand
  (`EnsureSpotLightBuffer`). No fixed-size *total* array exists — only the *shadow*
  arrays are fixed-size, so uncapping the total is safe.
- **Shadow slice == light index**: `Pass_SpotLights` writes
  `spotLightBufferCPU[i].shadowParams.y = (float)i` (`SceneRenderer.cpp:1387`). This
  single line is the fusion point; every spot has a shadow because count ≤ 4 ≤ atlas.
- Spot shadow atlas = `Texture2DArray` depth, `kMaxSpotLights` slices
  (`RenderTargetManager.cpp:430/449/455`), one DSV per slice (`.h:65`), DSV heap
  reservation `kDeferredDsvPerFrame` (`.h:115`). `Renderer::BindSpotShadowTarget` clamps
  to `kMaxSpotLights` (`Renderer.cpp:1340`).
- Spot shadow views = `spotShadowViews_[kMaxSpotLights]` (`Scene.h:117`,
  `SceneFrameData.h:89`), built 1:1 in `Scene::PrepareViews` (`Scene.cpp:578-599,674-677`).
  `Pass_SpotShadows` renders each into slice `lightIndex` (`SceneRenderer.cpp:1085-1178`).
- **Two** shaders sample the spot atlas via `light.shadowParams.y`:
  `shaders/spotlight_cs.hlsl` (`ComputeSpotShadow`, ~line 81) and `shaders/glass.hlsl`
  (`SampleSpotShadow`, ~line 208). `TransparentStaticMesh` reuses the buffer
  `Pass_SpotLights` fills, so it inherits whatever `shadowParams.y` was written.
- **Point lights have NO shadows today.** `PointLightGpu` =
  `{float3 position; float radius; float3 color; float intensity;}`
  (`LightManager.h:19-25`), 32 bytes. `pointlight_cs.hlsl` (deferred) and the point-light
  block in `glass.hlsl` (~line 320) light with distance attenuation only — no shadow
  term, no shadow atlas, no shadow views, no shadow pass.
- `ProfilerScopes` uses single scope keys for spot shadows (`kPassSpotShadow`,
  `kSpotShadowPerLight`), not per-slot arrays — no per-slot changes needed there.

---

## Part A — Spot Lights: Unlimited Lights, Capped Shadow Casters

### Design

Split the fused constant: delete `kMaxSpotLights` (total cap) by the end; add
`kMaxShadowedSpotLights = 8` for shadow-atlas size / shadow-view count / DSV reservation
/ `BindSpotShadowTarget` clamp.

Each frame `LightManager` selects the closest ≤ `kMaxShadowedSpotLights` spots to the
camera and assigns each a **shadow slot** in `[0, N)`, exposing:
- `GetSpotLightCount()` — all spots (drives lighting + buffer size), no longer capped.
- `GetShadowedSpotCount()` — N ≤ 8 (drives shadow views + atlas slices rendered).
- `GetSpotShadowSlot(lightIndex)` — atlas slice for that light, or `-1` if unshadowed.
- `GetShadowedSpotLightIndex(slot)` — inverse (which light owns a slot), so the shadow
  view for slot `s` uses that light's matrices.

`Pass_SpotLights` writes `shadowParams.y = (float)GetSpotShadowSlot(i)` (`-1` for
unshadowed). Both spot shaders early-out (`return 1.0`) when `shadowParams.y < 0`.
Slot/view/buffer stay coherent per frame even though the slot↔light mapping changes as
the camera moves.

**Selection metric:** distance² from camera position to spot-light position, ascending.

### Step A1 — Add `kMaxShadowedSpotLights`; resize the spot shadow atlas to it

Zero behavior change (total still capped at 4; atlas grows to 8 slices, 4 used).

Modify:
- `LightManager.h` — add `static constexpr std::uint32_t kMaxShadowedSpotLights = 8;`
  (keep `kMaxSpotLights` for now).
- `RenderTargetManager.{h,cpp}` — spot atlas `DepthOrArraySize`, SRV
  `Texture2DArray.ArraySize`, the per-slice DSV loop, `spotShadowDSV` array size (`.h:65`),
  and `kDeferredDsvPerFrame` (`.h:115`): `kMaxSpotLights` → `kMaxShadowedSpotLights`.
- `Renderer.cpp` — `BindSpotShadowTarget` clamp (`:1343-1345`) → `kMaxShadowedSpotLights`.
- `Scene.h:117`, `SceneFrameData.h:89` — `spotShadowViews_` array size.
- `SceneRenderer.{h,cpp}` — `Pass_SpotShadows` `std::array<..., kMaxSpotLights>` param
  (`.h:79`, `.cpp:1086`).

Acceptance: both builds `0/0`; run — scene renders identically (4 spots, 4 shadows).
**VRAM:** the atlas doubles (4→8 slices × `spotShadowRes²` × D32); confirm it fits at the
configured resolution.

### Step A2 — Per-frame shadow selection + slot mapping + shader sentinel

Installs the machinery while still capped at 4, so behavior stays identical (≤4 spots ⇒
all selected ⇒ slot == index) but every path Step A3 flips is in place.

Modify `LightManager.{h,cpp}`:
- Add `std::vector<int> spotShadowSlot_` (parallel to `spotLights_`, `-1` = unshadowed)
  and `std::vector<std::uint32_t> shadowedSpotLightIndices_` (slot → light index).
- `void SelectShadowedSpots(const Math::float3& cameraPos)`: reset all slots to `-1`;
  compute distance² camera→spot; pick the closest `min(count, kMaxShadowedSpotLights)`;
  fill both arrays (slot order = ascending distance).
- Accessors `GetShadowedSpotCount()`, `GetSpotShadowSlot(size_t)`,
  `GetShadowedSpotLightIndex(size_t)`.

Modify `Scene.cpp` (`PrepareViews`):
- Call `lightManager_.SelectShadowedSpots(camera_.GetPosition())` **before** building
  spot shadow views (see ordering note).
- Build `spotShadowViews_[s]` for `s` in `[0, GetShadowedSpotCount())` from
  `spotLights_[GetShadowedSpotLightIndex(s)]` (replace the 1:1 loop at `:578-599`).
- Enqueue `GetShadowedSpotCount()` shadow views (replace `:674-677`).

Modify `SceneRenderer.cpp`:
- `Pass_SpotShadows`: `viewCount = min(spotViews.size(), GetShadowedSpotCount())`.
- `Pass_SpotLights` fill (`:1387`): `shadowParams.y =
  (float)lightManager.GetSpotShadowSlot(i)`.

Modify shaders (sentinel at top of the shadow function):
- `spotlight_cs.hlsl` `ComputeSpotShadow`: `if (light.shadowParams.y < 0.0f) return 1.0f;`
- `glass.hlsl` `SampleSpotShadow`: same guard.

**Ordering note:** `UpdateSpotLightCache` is called in `Scene::Render` (`:693`), after
`PrepareViews`. `SelectShadowedSpots` + slot assignment must run **before** the shadow
views are built and **before** `Pass_SpotLights` fills the buffer. Cleanest: fold the
selection into `PrepareViews` right after the spot `UpdateCachedData`, and verify no pass
reads a stale `cachedSpotLightCount_`. Spot views must still bucketize/cull through the
shared `shadowCasterSource_`.

Acceptance: both builds `0/0`; run with ≤4 spots — identical render (all shadowed,
`shadowParams.y ≥ 0`). To pre-validate the flip: temporarily set `kMaxSpotLights` large,
add >4 spots, confirm the closest ≤8 shadow and distant ones are shadowless; **revert the
bump before finishing** (that flip is Step A3).

### Step A3 — Uncap the total spot count (behavioral flip)

`LightManager.cpp` `UpdateSpotLightCache`: `cachedSpotLightCount_ = spotLights_.size();`
(drop the `min`), and run `UpdateCachedData()` on all spots.

Acceptance: run; add >8 spots (editor spawn/duplicate) — all light the scene, the 8
closest cast shadows, the shadowed set updates as the camera moves; deferred and glass
paths agree. `--scene-stress` exit 0 (no `DEVICE_HUNG` on buffer growth — already guarded
by `EnvironmentRuntime::RebuildLights`; verify non-editor spawn growth is also idle-safe).

### Step A4 — Remove `kMaxSpotLights`, update editor/docs

Delete `kMaxSpotLights` from `LightManager.h`; grep the tree and repoint/remove any
residue (all shadow uses are `kMaxShadowedSpotLights` after A1). Update the
`DuplicateObjectCommand`/memory note that said "spot lights cap at 4" — it no longer
applies. Acceptance: clean grep for `kMaxSpotLights`; both builds `0/0`; behavior
unchanged from A3.

---

## Part B — Point Light Shadows (NEW feature)

Point lights are omnidirectional, so shadows need **cube maps**: 6 faces per shadowed
light. This is heavier than Part A — budget for it. Cap the shadowed set at
`kMaxShadowedPointLights` (default **4**) → up to 24 face-renders/frame and a 24-slice
shadow atlas. Non-shadowed point lights light the scene exactly as today.

### Approach decision (make this first)

Two ways to store/compare cube shadow depth. Pick one before Step B1:

- **(Recommended) Linear-distance cube.** Render each face storing the linear
  world-space distance from the light into an `R32_FLOAT` cube-array color target (tiny
  pixel shader: `output = length(worldPos - lightPos)`). Runtime sampling is trivial and
  reverse-Z-independent: sample the cube by direction `(P - lightPos)`, compare
  `length(P - lightPos) - bias > storedDist` ⇒ in shadow. Robust; costs one small new
  shadow shader + an R32 cube atlas.
- **(Alternative) Depth cube.** Reuse the existing depth `RenderShadow` for all 6 faces
  into a `TextureCubeArray` D32 atlas; sample with `SampleCmpLevelZero(float4(dir,slice),
  cmpDepth)`. Saves the new shadow shader but the runtime `cmpDepth` must be reconstructed
  from the major-axis magnitude through the **reverse-Z** perspective the engine uses —
  error-prone. Only choose this if you're confident with the reverse-Z depth math.

The steps below assume the **linear-distance** approach; adapt names if you choose depth.
All faces share one 90° FOV perspective (only orientation differs), near = small
constant, far = light radius.

### Step B1 — Constant, atlas, and point-light GPU struct extension

No shadows rendered or sampled yet (slots all `-1`), so behavior is identical.

- `LightManager.h` — add `static constexpr std::uint32_t kMaxShadowedPointLights = 4;`.
- Extend `PointLightGpu` (`LightManager.h:19-25`) with a shadow field, e.g. add
  `float4 shadowParams; // x = shadow slot (-1 = none), y = bias, z = nearPlane, w = farPlane`.
  Update the matching HLSL `PointLightData` struct in **both** `pointlight_cs.hlsl` and
  `glass.hlsl`, and update the `StructureByteStride` in `EnsurePointLightBuffer`
  (`LightManager.cpp` — it's `sizeof(PointLightGpu)` there and in the SRV desc, so it
  tracks automatically; confirm). Keep 16-byte alignment.
- `RenderTargetManager.{h,cpp}` — create the point shadow atlas: an `R32_FLOAT`
  `TextureCubeArray` (or `Texture2DArray` with `6 * kMaxShadowedPointLights` slices), an
  RTV per face (`6 * kMaxShadowedPointLights` RTVs) + a matching depth buffer for the
  render, and one SRV (`TextureCubeArray`). Add its handles to the `Deferred` struct
  alongside `spotShadow*`, and grow the RTV/DSV heap reservations. Mirror the spot
  atlas's lifecycle (created with the deferred targets, recreated on resize).
- `Renderer` — add `BindPointShadowTarget(UINT cubeSlot, UINT face, bool clear)` mirroring
  `BindSpotShadowTarget` (viewport = point shadow res, bind the face RTV + shared depth).

At this step, `Pass_PointLights` writes `shadowParams.x = -1` for every light. Acceptance:
both builds `0/0`; **run** (struct-size / SRV-table changes need a live check) — scene
renders identically to today.

### Step B2 — Selection + cube shadow views + shadow render pass

Renders shadows but nothing samples them yet → behavior identical.

- `LightManager.{h,cpp}` — mirror Part A: `SelectShadowedPoints(const Math::float3&
  cameraPos)` (closest ≤ `kMaxShadowedPointLights` by distance²), `pointShadowSlot_`
  (`-1` sentinel), `shadowedPointLightIndices_`, and accessors `GetShadowedPointCount()`,
  `GetPointShadowSlot(size_t)`, `GetShadowedPointLightIndex(size_t)`.
- `Scene.h` / `SceneFrameData.h` — add `pointShadowViews_[kMaxShadowedPointLights * 6]`
  (6 cube-face views per shadowed light).
- `Scene.cpp` (`PrepareViews`) — after `SelectShadowedPoints`, build the 6 face views per
  shadowed light: light position, 90° FOV, near/far, the 6 standard cube orientations
  (±X, ±Y, ±Z). Enqueue them for culling like spot views (each face frustum-culls the
  shared `shadowCasterSource_`). Reuse the same ordering discipline as Part A.
- `SceneRenderer.{h,cpp}` — add `Pass_PointShadows` mirroring `Pass_SpotShadows`: for each
  shadowed light × 6 faces, `BindPointShadowTarget(slot, face, clear)` then render the
  face's visible opaque buckets. Linear-distance approach: use the new distance-output
  shadow shader (a variant `RenderShadow` path, or a dedicated material) so faces store
  world distance; depth approach: reuse the existing depth `RenderShadow`. Register the
  pass in the render graph with correct resource-state transitions (the atlas → render
  target/depth-write during the pass, → SRV-read before the lighting passes), mirroring
  the spot atlas transitions.
- `Pass_PointLights` fill — set `shadowParams.x = (float)GetPointShadowSlot(i)`,
  `shadowParams.y = bias`, `.z = near`, `.w = radius` (far).

Acceptance: both builds `0/0`; `--scene-stress` exit 0; **run** — no visual change yet
(shadows render into the atlas but the lighting shaders ignore them). If you have a
GPU capture tool, confirm the atlas is populated.

### Step B3 — Sample point shadows in the lighting shaders (behavioral flip)

- Add the point shadow atlas SRV to the point-light SRV tables: `pointlight_cs.hlsl`
  root signature `numDescriptors` +1 and a new `TextureCubeArray` register; stage
  `D.pointShadowSRV` in the `Pass_PointLights` `RecordComputeDispatch` SRV list
  (`SceneRenderer.cpp:1490`). Do the same for `glass.hlsl` (its transparent SRV table in
  `TransparentStaticMesh.cpp:215-224`) and a comparison/linear sampler as needed.
- In both shaders' point-light loop, after computing `L`/`dist`, add a shadow term:
  `if (Ld.shadowParams.x < 0) shadow = 1;` else sample the cube by `(P - Ld.position)` and
  compare (linear-distance: `shadow = (dist - Ld.shadowParams.y) <= sampledDist ? 1 : 0`,
  with PCF if desired). Multiply the point light's contribution by `shadow`.

Acceptance: **run** — the closest ≤4 point lights now cast omnidirectional shadows;
distant point lights are shadowless; the shadowed set updates as the camera moves;
deferred and transparent (glass) point lighting agree. Tune bias to kill acne without
peter-panning. `--scene-stress` exit 0.

### Step B4 — Polish

PCF filtering on the cube sample; bias/near tuning; confirm `--scene-stress` and both
builds. Update the memory file.

---

## Cross-Cutting Notes

- **Config constants** (`kMaxShadowedSpotLights`, `kMaxShadowedPointLights`) live on
  `LightManager`, so every shadow-capacity site shares one source of truth. They are
  compile-time because the atlases/view arrays are fixed-size `std::array`. A runtime
  toggle would require dynamic atlas allocation — out of scope.
- **Shaders to touch:** spot = `spotlight_cs.hlsl` + `glass.hlsl`; point =
  `pointlight_cs.hlsl` + `glass.hlsl`. No other shader reads either atlas. All are
  runtime-compiled — validate by running.
- **Buffer/atlas growth safety:** the light *structured buffers* grow via
  `Ensure*Buffer`; the editor path pre-grows at GPU-idle. The shadow *atlases* are
  fixed-size (allocated with the deferred targets), so they never grow at runtime — no
  hang risk there. Keep the "free/realloc only at idle" rule for anything you add.
- **Serialization unchanged:** lights are JSON arrays; shadow-casting is a per-frame
  runtime decision, nothing new is persisted.
- **Both-config parity:** all engine code; verify Release each step.

## Non-Goals / Future Refinements

- **Better shadow-caster metric.** Distance-to-position ignores cone direction, range,
  intensity, and frustum visibility. Could score by screen footprint / distance-to-cone /
  frustum cull, and skip lights whose influence never intersects the view.
- **Temporal stability.** A light entering/leaving the shadowed set pops its shadow on/off
  as the camera moves; hysteresis or a fade could smooth it.
- **Runtime-adjustable shadow counts / per-light resolution / atlas packing.** Kept
  compile-time and uniform-resolution.
- **Point shadow perf.** 6 faces/light is expensive; future work could cull faces whose
  frustum is empty, use lower-res or dual-paraboloid, or cache static-scene faces.
