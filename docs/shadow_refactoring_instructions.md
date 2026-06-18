# Shadow system refactoring & improvement — correctness, perf, culling (working prompt, rev 4)

Companion to `docs/shadow_review_instructions.md` (the diagnosis brief). That file
says *find the problems*; THIS file says *fix them*, ONE change per commit, with a
measurement or a user visual-confirmation between every change. It encodes the
review's prioritization: the **correctness** bugs first (tile-edge seams, edge
shimmer, split seams), then the `Pass_CSM` **perf** wins, then **culling**, then
**cleanup**.

Self-contained: read the whole brief before touching code. Do ONE step, build
(`test_cube.sln`, x64, Debug + Release), verify, **stop**. All file:line refs below
were VERIFIED against the working tree on 2026-06-15 — but line numbers drift, so
re-check before relying on them. Repo root `D:\Programming\test_cube`, C++ under
`sources/`, shaders under `shaders/`. Windows + PowerShell; MSBuild at
`C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe`.

Rev 2 adds Step 2 (cascade fit & stabilization — the `min(radius, rLS)` line defeats
the stable radius → edge shimmer; plus near-plane pancaking) and reframes the culling
step (Step 6) to DELETE the wrong-axis depth clamp rather than repair it.
Rev 3 corrects the atlas-sizing rationale: the shadow map is WORLD-space and not
mechanically tied to screen/render resolution. Step 5 is rebased on quality-matching
(shadow-texel vs screen-pixel density) and gated on first proving `Pass_CSM` is
fill-bound, not draw-bound.
Rev 4 splits Step 6 into separable sub-steps 6a–6e (delete wrong-axis clamp; far-cascade
LOD; swept-shadow-volume cull; contribution cull; bucketize hoist), with a measure-first
gate choosing between them and the long-shadow correctness trap called out for the
near-object culls.

## Why this matters (measured context — from the review brief)

At the demo settings (render Scale 0.58, DLSS on, kFrameCount=3) the scene is
**GPU-bound**: CPU ~0.45 ms/frame, GPU frame ~1.37 ms, the CPU otherwise blocked in
`Renderer::WaitForFrame`. Because it is GPU-bound, **only GPU-side wins move FPS**.

Per the F9 GPU profiler, **`Pass_CSM` is the single largest GPU pass: ~0.39 ms,
~28% of the GPU frame** (5 invocations: 1 atlas clear + 4 cascades). `Pass_SpotShadows`
~0.08 ms. Shadows are the #1 GPU cost AND carry the known cascade-border visual bug —
highest value to fix.

Demo camera (`data/levels/demo.json`): `hfov 90°`, `zNear 0.01`, `zFar 10000`; shadows
capped at `maxDistance 300`. The wide 90° FOV matters for Step 2 — it hides a latent
coverage bug but makes the stabilization defect fire every frame.

Note — there are TWO separate shadow costs, and only one tracks screen resolution:
- `Pass_CSM` is the atlas RENDER — WORLD-space rasterization of casters into the fixed
  4096² atlas (`RenderTargetManager.cpp:436`). Its cost is geometry/atlas-resolution
  bound and does NOT change with the 0.58 render scale, so it holds constant while DLSS
  shrinks the main passes — which is why it sits at ~28% of the frame.
- Shadow SAMPLING (the PCF in `lighting_cs.hlsl`) is dispatched per pixel of the
  lighting pass, which runs at render resolution, so it DOES scale with render scale.
The shadow-map resolution is world-space and NOT mechanically tied to screen resolution
(it sets world-space texel density, not screen detail). The only real link is perceptual
quality — see Step 5 for the actual basis to resize it.

## Architecture map (verified 2026-06-15)

Data flow: **cascade setup (CPU, per frame) → atlas render (GPU) → sample in the
lighting compute shader (GPU)**.

1. **Cascade setup** — `Scene::UpdateCascades` (`sources/app/scene/Scene.cpp:94-229`),
   called from `Scene::PrepareViews` (`Scene.cpp:316`). Fills
   `SceneFrameData::CascadeData` (`sources/app/scene/SceneFrameData.h:42-50`,
   `kCascades = 4`): per-cascade `lightView/lightProj`, `atlasScale/atlasBias`
   (2×2 tile packing, `Scene.cpp:208-211`), `splitsVS[kCascades+1]`, `normalBiasWS`,
   `depthBiasNDC`.
   - **Split scheme**: fixed view-space distances `CascadeShadowConfig::sliceDistances`
     `{15, 40, 100, 300}` m, capped at `maxDistance` — `BuildSplitScheme`
     (`sources/app/scene/SceneRenderConfig.cpp:5-23`). NOT a log/lambda fit to camera
     near/far.
   - **Fit**: per-cascade bounding radius from the rotation-INVARIANT `radiusFor`
     (far-face corner distance, `Scene.cpp:147-157`), then **overridden** by the
     rotation-DEPENDENT corner extent `radius = std::min(radius, rLS)`
     (`Scene.cpp:183`) — this is the Step 2a defect. Ortho center placed on the slice
     FAR plane (`forwardOffset = 1.0` ⇒ `center = camPos + camDir*sliceFar`,
     `Scene.cpp:158`). Light near plane fitted to slice corners + `zPadding = 25`
     (`nearLS`, `Scene.cpp:197-198`) — the Step 2b defect.
   - **Stabilization**: world-space center snap (`stabilizationStepFraction` 0.1,
     `Scene.cpp:159-166`) then per-texel light-space snap (`Scene.cpp:185-190`); ortho
     fit via `OrthoOffCenterLH` (`Scene.cpp:201`). The texel snap is only effective if
     `unitsPerTexel` is stable frame-to-frame (see Step 2a).
   - **Bias**: `normalBiasInTexels` 0.75, `depthBiasInTexels` 2.0
     (`SceneRenderConfig.h:9-10`) → `normalBiasWS`/`depthBiasNDC` in texel units
     (`Scene.cpp:205-206`); these scale with `unitsPerTexel`, so they swim when it does.

2. **Atlas render** — `SceneRenderer::Pass_CSM`
   (`sources/app/scene/SceneRenderer.cpp:540-645`): one clear list
   (`BindShadowTarget(d.cl, 0, clear=true)`, localOrder 0, `:553`) then per-cascade
   lists (localOrder `idx+1`, `:601` / `:642`), each
   `BindShadowTarget(cl, idx, clear=false)` into its 2048² tile (`:580` / `:622`),
   drawing the cascade's culled opaque casters via `obj->RenderShadow(...)`. Atlas
   `shadowRes = 4096` (`RenderTargetManager.cpp:436`), depth format **D16_UNORM**
   (`RenderTargetManager.cpp:330-410` — low precision, interacts with bias/acne).
   Tile layout / viewports hardcoded in `Renderer::BindShadowTarget`
   (`Renderer.cpp:1030-1056`; clear = full-atlas `ClearDepthStencilView` at `:1054`).
   - **Spot shadows** — `Pass_SpotShadows` (`SceneRenderer.cpp:649-...`): per-light
     slices of a `spotShadowRes = 512` array (`RenderTargetManager.cpp:439`).
   - **Shadow draw** — `RenderObjectBase::RenderShadow`
     (`sources/rendering/renderables/RenderableObject.cpp`); depth-only shaders
     `shaders/gbuffer_csm.hlsl`, `shaders/gbuffer_inst_csm.hlsl`.

3. **Sampling** — `shaders/lighting_cs.hlsl`: `ChooseCascadeIndex` (`:49-54`),
   `SampleShadowCSM` (`:77-107`), `ShadowPCF` / `ShadowPCF3x3` (`:56-75`). Comparison
   sampler `gSmpLinear`. Spot sampling in `shaders/spotlight_cs.hlsl`.

4. **Culling** — per-cascade `SceneView`s (`Scene::cascadeViews_`,
   `spotShadowViews_`), each its own frustum (`Frustum::FromOrthoBounds`,
   `Scene.cpp:220`) + `SceneRenderQueue`, classified by `Bucketize`
   (`SceneRenderQueue.cpp:47`, re-run per view) and frustum-tested by `Cull`
   (`SceneRenderQueue.cpp:136`). The per-cascade depth-range clamp is DISABLED and
   uses the wrong axis (camera distance vs camera-space slice near/far): `Scene.cpp:357`
   `clampDepthRange = false; //doesnt work well, fix in future`,
   `SceneRenderQueue.cpp:174-190`.

Shaders are runtime-compiled from `shaders/*.hlsl` (`Material.cpp` D3DCompile/DXC path)
with a material hot-reload (`Scene.cpp:420`, `MaterialManager::ApplyPendingHotReloads`).
**The sampling-shader steps (1, 3, 4 — `lighting_cs.hlsl`) need no C++ rebuild** —
relaunch (or hot-reload) picks up the edited `.hlsl`. The cascade-setup / atlas /
culling / cleanup steps (2, 5, 6, 7) touch C++ and need a rebuild.

## AI execution protocol

1. Execute only the step the user names. If none named, inspect code and RECOMMEND
   the next step; do not edit.
2. Stop after the requested step. Never start the next one automatically.
3. Before editing, inspect current code and `git status`; work with pre-existing
   changes, never revert unrelated work.
4. Keep edits scoped to the requested step (and, for a multi-part step, the ONE
   sub-part requested) plus its tests/instrumentation.
5. No commits, branches, or flag removals unless the user explicitly asks.
6. Never claim a check passed unless it was run and observed. Report every check as
   `PASS`, `FAIL`, or `NOT RUN`.
7. Visual correctness is the whole point for the correctness steps (1–4) and a
   constraint for the perf/culling steps (5–6), and it CANNOT be self-verified here
   (see Verification + project memory `screenshot-verification`). Present
   analysis/captures to the USER; never conclude a visual fix works or a regression
   exists from a capture alone.
8. When the code contradicts this doc's description, STOP and report the conflict;
   do not silently invent a replacement design.
9. Sub-parts and perf/culling changes are SEPARATELY MEASURED — never land two between
   measurements (Step 2's 2a vs 2b, Step 5's resolution change, Step 6's 6a–6e); a
   visual/perf regression must be attributable to ONE change.

At the end of a step report: step completed; changed files & behavior; automated checks
with exact commands & results; manual/visual checks as `PASS`/`FAIL`/`NOT RUN`;
before/after `Pass_CSM` & `GPU.Frame` (or `NOT RUN`); remaining risks.

## Correctness invariants (preserve throughout)

1. A lit pixel inside the directional shadow range samples the depth of **its own
   cascade's tile** — never a neighboring tile's depth, and never "fully lit by
   default" when a valid coarser cascade covers it.
2. The shadow factor stays in `[0,1]`; cascade transitions are continuous (no hard
   step in shadow value, bias, or filter width across a split or a tile edge).
3. PCF taps never read across a tile boundary into a neighbor cascade's texels (the
   atlas has NO gutter between the four packed tiles).
4. Shadow edges stay **stable under pure camera rotation** — the per-cascade
   `unitsPerTexel` must not change frame-to-frame from camera orientation, or the
   light-space texel snap is defeated and edges crawl.
5. A caster **between the light and a cascade's slice still casts** into that cascade
   (the light ortho near-plane must reach toward the light far enough to include it).
6. When touching `Pass_CSM` command-list recording, preserve the submission contract
   from `docs/renderer_submission_instructions.md`: the clear list is `localOrder 0`,
   cascades follow as `idx+1`, each CL closed exactly once via `EndThreadCommandList`.
   Do not reorder the clear after a cascade draw.
7. Debug and Release render identically.

## Verification recipe (every step)

- Build Debug + Release (x64). The Debug D3D12 debug layer breaks on errors — keep it
  clean. (Shader-only steps: still launch both configs to exercise the debug layer.)
- **Perf is GPU-side**: read per-pass GPU times from the **F9 profiler overlay**
  (`Pass_CSM`, `GPU.Frame`). The overlay can't be read headlessly — either ask the
  USER for before/after numbers, or add TEMPORARY `OutputDebugStringA` instrumentation
  logging the GPU scope's ms and capture it with a DBWIN listener (pattern from prior
  work; remove after). Treat **>3% GPU-frame regression as a failure**. Numbers, or
  report `NOT RUN` — no vague claims.
- **Visual correctness cannot be self-verified**: capture tooling on this machine is
  unreliable and the cascade-border / shimmer bugs need camera movement (interactive
  input the assistant can't drive). Present analysis/captures to the USER and ask them
  to confirm before concluding. Never burn iterations debugging off a self-read capture.
- Toggles to exercise: F4 DebugTex, F5 SSR, F6 DLSS, F7 FXAA, F9 profiler. DLSS mode +
  render Scale change shadow appearance — pin identical settings when comparing.
- Runtime smoke: drive `D3D12WindowClass` / title "D3D12 Multi-Mesh Renderer" headlessly
  (PostMessage F-keys, SetWindowPos resize, WM_CLOSE), expect exit 0, for crash /
  debug-layer checks — NOT for visual or GPU-time reads. Report camera-movement and
  hot-reload checks as `NOT RUN` if the environment can't drive them.

---

## Step 1 — out-of-tile: sample the correct tile, fall back to a coarser cascade

**Category: correctness. Risk: low (shader-only). The tile-edge seam bug + a latent
wrong-cascade-sampling bug.** Lead with this.

`SampleShadowCSM` (`lighting_cs.hlsl:77-107`) picks a cascade by depth
(`ChooseCascadeIndex`), projects `Poff = Pws + Nws*normalBiasWS[idx]` into that
cascade, maps to atlas UV (`uv = uv*scale + bias`, `:91`), then:

```hlsl
if (any(uv < 0.0) || any(uv > 1.0))  // :93
    return 1.0;                        // :95  -> fully lit
```

Two defects:

- **Wrong-tile sampling (latent, subtle).** The bounds test is against the WHOLE atlas
  `[0,1]`, not cascade `idx`'s tile sub-rect `[bias, bias+scale]`. A point that
  projects outside cascade `idx`'s own frustum but lands in a NEIGHBORING tile passes
  the test and samples the wrong cascade's depth → incorrect shadow, not just a seam.
- **Out-of-range → fully lit (the visible seam).** When the projected sample genuinely
  leaves cascade `idx` (tight ortho fit, or pushed out by the normal-bias offset),
  `return 1.0` paints a hard UNSHADOWED seam at the tile edge instead of falling back
  to a coarser cascade that still covers the pixel.

**Fix:**
- Test the cascade's OWN normalized UV (the pre-`scale+bias` value, i.e. cascade NDC
  mapped to `[0,1]`) against `[0,1]`, NOT the atlas-space UV. This makes "outside this
  cascade" exact and removes the wrong-tile read.
- On a miss, advance `idx` to the next coarser cascade (`idx+1 … 3`), re-project, and
  retry. Only `return 1.0` if even cascade 3 misses — there, the pixel is genuinely
  beyond the directional shadow range and "lit" is correct. A small fixed-trip loop
  (≤4 iterations) keeps it branch-friendly.
- Re-evaluate `Poff` per cascade (each has its own `normalBiasWS[idx]`).

**Landmines:**
- The four tiles are packed adjacently with NO gutter. A 3×3 PCF tap near a tile edge
  reads texels from the neighbor tile. INSET the in-tile acceptance test by the PCF
  radius expressed in normalized-tile units (`pcfRadius * texel / scale`), or clamp the
  sample UV to the tile rect, so taps never bleed across the border. State this in a
  comment (invariant 3).
- Keep the `lc.w` guard (`max(1e-6, lc.w)`, `:88-89`).

**Acceptance:** tile-edge unshadowed seams gone and no wrong-cascade shadows
(USER visual confirm, with camera movement across the range); `Pass_CSM` /
`lighting_cs` cost essentially unchanged (the fallback loop runs only at tile
boundaries) — measure and report; debug layer clean.

## Step 2 — cascade fit & stabilization (stop defeating the stable radius; pancake the near plane)

**Category: correctness. Risk: medium (C++ math in `UpdateCascades`; visual).
Independent of the shader steps.** Shimmer is CONSTANT (every frame the camera turns),
whereas the split seam is intermittent — so for visible quality this ranks right after
Step 1. Two independently-verifiable sub-changes; land and confirm them SEPARATELY
(protocol 9).

### 2a — Stable per-cascade radius (fixes edge shimmer on camera rotation)

`UpdateCascades` computes a rotation-INVARIANT bounding radius (`radiusFor`, depends
only on split distances + FOV, `Scene.cpp:147-157`) and then DISCARDS it:

```cpp
radius = std::min(radius, rLS);                 // Scene.cpp:183  (rLS rotates with camera)
const float unitsPerTexel = (2.0f * radius) / static_cast<float>(tileRes);  // :185
... centerLS.x = std::floor(centerLS.x / unitsPerTexel) * unitsPerTexel; ... // :188-189
```

`rLS` is the light-space corner extent, which rotates with the camera. With the demo's
90° FOV `rLS` is always the smaller, so `radius` — and therefore `unitsPerTexel` — change
every frame as the camera turns. The texel-snap grid (`:188-189`) is then a DIFFERENT
grid each frame → the snap is defeated → shadow edges crawl/shimmer on rotation.
`normalBiasWS`/`depthBiasNDC` (`:205-206`) scale with `unitsPerTexel` too, so the bias
swims as well.

**Observed (confirmed with user 2026-06-15):** in the demo, a thin line on FLAT LIT ground
at each split (~15/40/100 m) CRAWLS/SHIMMERS as the camera rotates — that motion is this
bias/texel-grid swim. After 2a the line must stop moving (the remaining static line is
Step 3's job). Verify by panning the camera and watching a split line.

**Implemented 2026-06-15 — corrected mechanism (the prescription below was wrong).** The
naive "drop the `min(., rLS)` and use `radiusFor` directly" is INSUFFICIENT: `radiusFor`
is the far-corner distance from the far-PLANE center, not a true bounding radius, and the
coarse world-space snap (`stabilizationStepFraction`, the old `Scene.cpp:159-166`) shifted
the center by up to ~0.1·radius BEFORE the corners were measured — so `radiusFor` failed to
enclose the near corners. The `min` was clamping to `rLS` precisely to absorb that shift,
which is exactly what made the radius rotation-dependent. Flying the camera with the naive
fix tripped the coverage assert at once.

**Actual fix applied (two parts — second was needed after the first still danced):**
1. **Bounding-sphere fit** — `sphereCenter` = centroid of the 8 slice corners, `radius` =
   max corner distance + `overlap`. The sphere radius depends only on slice shape + FOV →
   `unitsPerTexel` constant; a sphere projects to a same-radius circle in any light
   orientation, so the extent never changes with sun/camera angle. Coverage is guaranteed
   BY CONSTRUCTION (`radius ≥ sphereRadius ≥ |corner−center| ≥ rLS`), closing the
   narrow-FOV under-coverage hole.
2. **Real texel snap** — the old `centerLS = lightView * center` snap was a NO-OP: `center`
   is the `LookAt` target, so a view matrix maps it to light-space (0,0,dist) → XY always
   (0,0) → `floor(0/upt)*upt = 0`. (The deleted coarse world snap was the only thing ever
   stabilizing anything, crudely.) Fix: snap `center` along the FIXED light right/up axes
   (`up.Cross(fwd)`, `fwd.Cross(right)`) to whole-`unitsPerTexel` steps in WORLD space,
   BEFORE building `lightView`. Because `radius` is constant, `unitsPerTexel` is constant,
   and `radius` is an integer multiple of it, so the covered world region shifts in exact
   whole-texel steps as the camera moves → texels pinned to fixed world cells → no crawl.

`forwardOffset` and `stabilizationStepFraction` (`SceneRenderConfig.h`) are now UNUSED
config — remove in Step 7 cleanup.

**Landmines:**
- The DEBUG `assert(radius + 1e-3f ≥ rLS)` is kept as a tripwire; it now holds by
  construction and would fire only if someone reintroduces a center offset or a coarse
  pre-measure snap.
- The centroid sphere is a valid (not minimal) enclosing sphere — slightly larger than the
  old tight `rLS` fit, so shadows are marginally softer. If too soft, the minimal on-axis
  sphere or per-cascade resolution (Step 5) is the lever — do NOT re-add the `min`.

**Acceptance (build + headless run PASS 2026-06-15; visual pending USER):** shadow edges
stable under pure camera ROTATION (USER confirm — pan and watch a split line: before =
crawl, after = stable); coverage assert did NOT fire while flying (translation tested
headless; rotation covered by the construction proof); `Pass_CSM` draw work unchanged.

### 2b — Pancake the light near-plane (fixes missing shadows from off-slice casters)

`nearLS = std::max(0.001f, minZ - zPad)` with `zPad = 25` (`Scene.cpp:197-198`) fits the
ortho near plane to the slice corners + 25 units. A caster more than ~25 units TOWARD the
sun from the slice falls outside the ortho near plane → not rendered → casts no shadow
into the cascade.

**Implemented 2026-06-16.** Added `CascadeShadowConfig::casterReachWS` (default 150 m) and
set `nearLS = max(0.001f, minZ - casterReachWS)` (far side keeps `zPadding`). Pulls the
near plane ~150 m toward the light so casters that far above/behind a slice still render.

Note — the precision landmine below is OVERCAUTIOUS for this codebase: the WORLD-space
depth bias is `depthBiasInTexels · unitsPerTexel`, which is INDEPENDENT of the depth
range (the NDC bias and the per-texel NDC slope both scale as 1/range, so their ratio is
constant), and D16 quantization only causes acne past ~2500 m of range. So 150 m is
acne-safe; it is bounded well under `maxDistance` (300) per the landmine. The fully robust
alternative — depth-clamp pancaking via `DepthClipEnable=FALSE` on the shadow PSO, which
captures casters of ANY height with zero range cost — is deferred (it's a material/PSO
change; `Material.h` sets `raster.DepthClipEnable = TRUE` per material).

**Landmines:**
- Do NOT pull `nearLS` all the way to the light eye (`lightDistance`); keep it bounded.
- This changes which casters draw → re-run the Step 1 / Step 4 visual checks afterward.

**Acceptance (build + headless fly PASS 2026-06-16; visual pending USER):** for the demo
this is a NO-OP visually (small teapots on flat ground — nothing sits 25–150 m toward the
sun), so the USER check is NO REGRESSION (shadows unchanged, no new acne, no missing
shadows). The "tall off-slice caster now casts" acceptance needs a scene with such a
caster. Coverage assert did not fire while flying; `Pass_CSM` draw work unchanged for the
demo.

## Step 3 — cascade blend band across splits

**Category: correctness. Risk: medium (visual + small perf). Depends on Step 1.**

`ChooseCascadeIndex` (`lighting_cs.hlsl:49-54`) returns a single cascade by view-space
depth. At a split the cascade flips discontinuously, and bias / texel density / PCF
radius all change with it → a visible seam at the split distance (15 / 40 / 100 m).

**Observed (confirmed with user 2026-06-15):** the seam shows on FLAT LIT ground (not only
on shadowed surfaces) as a thin self-shadow/bias step at each split — each cascade biases
its own acne differently. The blend band must fade this static line into a gradient. (Its
crawl/shimmer is a separate symptom, fixed by Step 2a — do 2a first.) Residual within-a-
cascade acne, if any remains, is bias tuning (Step 4 / parked D32 precision).

**Implemented 2026-06-16 (`lighting_cs.hlsl`).** Refactored the Step 1 fallback walk into
`SampleCascadeChain(start, …)`. `SampleShadowCSM` picks `idx` by depth, samples
`chain(idx)`, and in a band just before idx's far split also samples `chain(idx+1)` and
lerps. Band width = `splitNext * kBlendFraction` (`kBlendFraction = 0.1`, in-shader — no
new cbuffer constant needed; `splitNext` read from `cascadeSplitsVS.y/z/w` via a ternary
to avoid dynamic vector indexing). `t = saturate((zView − (splitNext − band)) / band)` so
`t=0` at band start (pure idx) → `t=1` at the split (pure idx+1), continuous with the
`idx+1` region just past the split. The second sample runs ONLY inside the band.

**Landmines (handled):**
- Step 1's fallback makes the `idx+1` sample valid even at idx+1's tile edge.
- Cascade 3 has no `idx+1` → guarded by `if (idx < 3)`, never blends.
- Doubles samples in-band only. If the band cost shows up, switch to a dithered pick — not
  done (band is narrow; measure first).

**Acceptance (build + headless run PASS 2026-06-16; visual pending USER):** the split line
should now fade into a gradient instead of a hard seam — USER confirm by moving across the
~15 / 40 / 100 m boundaries (do this AFTER 2a so the line is also static). `lighting_cs`
GPU cost — NOT measured (read F9 `Pass_CSM`/lighting if you want the in-band overhead
number; expected small). Shadow factor stays in `[0,1]` (lerp of two `[0,1]` values).
`kBlendFraction` is the tuning knob if the band looks too wide/narrow.

## Step 4 — consistent filtering & bias across cascades

**Category: correctness/quality. Risk: low (shader-only). Small.**

`SampleShadowCSM` uses 3×3 PCF for cascades 0–2 but 1×1 for cascade 3
(`lighting_cs.hlsl:102-106`) → a filtering discontinuity (sharp vs soft) at the 100 m
boundary, on top of any residual split seam.

**Implemented 2026-06-16 (`lighting_cs.hlsl`, in `SampleCascadeChain`).** Replaced the
`if (c < 3) 3x3 else 1x1` with an unconditional `ShadowPCF3x3` — every cascade now filters
3×3. Deleted the dead `static const float shadowBias = 0.0015f`. The live bias model
(`bBase = shadowBiasNDC[c]` + grazing term) is unchanged. The `ShadowPCF` (1×1) helper is
now UNUSED → Step 7 cleanup candidate (left in place to keep this change surgical).

**Follow-up (same day) — per-cascade PCF radius.** A uniform `pcfRadius` of 1 *texel* made
the far cascade a blurry mess (user-reported): its texels are ~10–16× larger in world
space, so a 1-texel kernel = a ~0.7 m penumbra there vs ~0.02 m near. Fix — scale the
texel radius by the per-cascade world-texel-size ratio (`normalBiasWS[c]` is proportional
to that size, so the `normalBiasInTexels` factor cancels):

```hlsl
const float pcfR = pcfRadius * pow((normalBiasWS[0] / max(1e-6, normalBiasWS[c])), 0.25);
```

The **exponent is the look knob**: `1.0` = constant world penumbra (far cascades nearly as
crisp as near — over-sharp); `0.0` = the original fixed-texel mush. The user tuned it to
**`0.25`** (world penumbra ∝ `unitsPerTexel^0.75` — grows with distance but sublinearly,
so far shadows stay soft without smearing); committed and visually preferred. `pcfRadius`
(1.0) is the global softness multiplier. Cleaner alternative if ever wanted: a dedicated
per-cascade radius / `unitsPerTexel` cbuffer field instead of the `normalBiasWS` proxy.

**Landmines (watch):** confirm cascade 3 with 3×3 didn't reintroduce acne at distance
(D16 precision is lowest in the far cascade); if it does, nudge `depthBiasInTexels` rather
than reverting to 1×1.

**Acceptance (build + headless run PASS 2026-06-16; visual pending USER):** no sharp→soft
seam at the cascade 2→3 (~100 m) boundary (USER confirm); acne/peter-panning unchanged
elsewhere. `Pass_CSM`/lighting cost delta — NOT measured (cascade 3 gains ~8 taps/pixel;
expected negligible, read F9 if you want the number).

## Step 5 — atlas resolution / per-cascade tile sizing

**Category: perf (potentially the biggest direct `Pass_CSM` win — but verify the bound
first). Risk: medium (quality tradeoff — USER must confirm sharpness).**

`shadowRes = 4096` is hardcoded (`RenderTargetManager.cpp:436`), giving 2048²/cascade.
The shadow map is WORLD-space: its resolution sets world-space texel density (~1.5 cm/
texel near, ~0.24 m/texel far), NOT screen detail — do NOT think of it as "matching the
render target." The basis for resizing is QUALITY-matching: a shadow looks right when one
shadow texel ≈ the world footprint of one screen pixel where the cascade is sampled. At
0.58 render scale each screen pixel covers more world, so 4096 is LIKELY oversized — that
is the argument for shrinking it, not any cost-vs-resolution coupling.

CAVEAT — measure the bound BEFORE committing to this step: shrinking the atlas only saves
`Pass_CSM` ms if the pass is FILL/raster-bound (depth-write over texels). With 4 cascades
drawing many casters it may instead be DRAW/vertex-bound, in which case resolution barely
moves the time and culling (Step 6) is the real lever. Use the F9 before/after (or a
quick experiment: drop to 2048² and see if `Pass_CSM` drops ~4× or hardly at all) to
decide which way it leans first.

**5a (do this first — one knob, measurable):** make `shadowRes` config-driven (thread it
from `CascadeShadowConfig` / a render setting instead of the literal `4096`) and measure
3072 and 2048 at the user's settings. Everything downstream already derives from
`shadowRes`, so this is safe to change in one place:
- `Scene.cpp:185` (`unitsPerTexel`), `:205-206` (bias in texels), `:208-209`
  (`atlasScale`/`atlasBias` from `tileRes = shadowRes/2`);
- `Renderer::BindShadowTarget` tile size `shadowRes*0.5` and the four origins
  (`Renderer.cpp:1039-1048`);
- `shadowAtlasSize` shader constant feeding `texel = 1/shadowAtlasSize`
  (`lighting_cs.hlsl:98`).

**5b (defer — bigger refactor):** non-uniform per-cascade tiles (big near, small far).
This breaks the uniform-`tileRes` assumption in all three places above — `atlasScale`,
the hardcoded 2×2 layout, and a single `shadowAtlasSize`. Only attempt after 5a if the
uniform shrink isn't enough; treat as its own multi-step effort.

**Landmines:** the clear list clears the WHOLE atlas (`Renderer.cpp:1054`) — fine, just
re-check after a size change. Pin DLSS mode + render Scale + camera start when comparing
(they alter shadow appearance, `Verification`).

**Acceptance:** `Pass_CSM` ms drops with the tile area IF the pass was fill-bound — if it
barely moves, the pass is draw-bound, so STOP and pivot to Step 6 rather than degrading
shadow quality for no gain (report the before/after either way); shadow sharpness
acceptable to the USER at the chosen size; no other pass regresses >3% GPU frame; debug
layer clean.

## Step 6 — shadow-caster culling & far-cascade cost (multi-part)

**Category: culling/perf. Risk: ranges per sub-step (6a none → 6c correctness-sensitive).
Builds on Step 2b.** Five separable commits; land and measure ONE at a time (protocol 9).
Read this shared preamble before any of them.

**Why plain frustum culling does NOT remove near objects from far cascades.** Cascade 3's
ortho box is huge: at 90° FOV its radius is `sqrt((300·tanH)²+(300·tanV)²) ≈ 344 m`,
centered ~300 m ahead — a ~690 m square that SPATIALLY ENGULFS the near-camera region
(laterally, and in light-depth for a high sun on flat terrain). So a 5 m-away prop is
inside cascade 3's frustum and `frustum.Intersects` keeps it. Tightening the box can't
fix this — the box is large because the cascade genuinely covers a huge area.

**The correctness trap (do NOT violate, applies to 6c/6d).** You cannot "draw each object
only in its smallest containing cascade." A near caster can throw a LONG shadow onto far
ground that samples cascade 3 (low sun, tall object); removing it makes that shadow
vanish. Any near-object cull MUST be sun-angle aware. The correct test is not "is the
object in the box" but **"can the object's shadow land on a receiver that samples THIS
cascade?"** (receivers sampling cascade N are at view-depth `[splitN, splitN+1]`).

**Measure first to pick the lever (gate for 6b vs 6c/6d).** Do 6a, then instrument
per-cascade caster AND triangle counts (and recall Step 5's fill-vs-draw-bound result).
If the far-cascade cost is a few LARGE meshes / draw-bound → prioritize 6b (LOD). If it's
MANY small near objects → prioritize 6c + 6d. Usually both apply; let the counts order
them. Do not implement 6b–6d blind.

### 6a — Delete the wrong-axis depth clamp (baseline)

**Risk: none (runtime no-op — the clamp is already `= false`).** Do this first; it clears
dead code and establishes the correct baseline cull the rest build on.

`SceneRenderQueue::Cull` (`SceneRenderQueue.cpp:174-190`) would cull by EUCLIDEAN distance
from the camera (`toCenter.Length()`, `:180`) against the cascade's CAMERA-space slice
near/far (`view.zNear/zFar`, `Scene.cpp:358-359`) — a convoluted heuristic on the wrong
axis (`Scene.cpp:357` `//doesnt work well`). Do NOT repair it: after Step 2b the
light-space ortho frustum (`Scene.cpp:220`) is the correct and sufficient baseline, so
plain `frustum.Intersects` is enough. Remove the dead `clampDepthRange`/`minDepth`/
`maxDepth` plumbing (`Scene.cpp:357-360`, `SceneRenderQueue.cpp:174-191`).

**Acceptance:** identical visuals and identical per-cascade draw counts (it was off);
both configs build; debug layer clean. (This sub-step deliberately does NOT remove near
objects from far cascades — that's 6b–6d.)

### 6b — Shadow-caster LOD in cascades 2–3

**Risk: low–medium (visual: proxy must not change the silhouette enough to matter at
0.24 m/texel). Likely the biggest far-cascade win if Step 5 showed `Pass_CSM` is
DRAW/vertex-bound.**

Render low-poly proxies (or a coarser mesh LOD) into the far cascades — at ~0.24 m/texel
fine geometry can't be resolved anyway. This is the only lever that also cuts the
IRREDUCIBLE cost of large casters (terrain, big meshes) that legitimately appear in every
cascade and can never be culled. Drive LOD selection off the cascade index (or its
unitsPerTexel) in `RenderObjectBase::RenderShadow` / the per-cascade draw loop.

**Landmines:** a proxy whose silhouette differs from the real mesh shifts shadow edges —
verify the far shadow still matches. Keep cascade 0–1 at full detail.

**Acceptance:** far-cascade triangle count drops; `Pass_CSM` ms down (most if draw-bound);
far shadows visually unchanged to the USER; no >3% GPU regression elsewhere; debug clean.

### 6c — Swept-shadow-volume cull (the sun-aware near-object cull)

**Risk: medium–high (CORRECTNESS — too-short an assumed reach drops real long shadows).**
The principled answer to "cull near objects from far cascades."

Extrude the caster AABB along the light direction by its max shadow reach
(≈ `height / tan(sunElevation)`) and cull it from cascade N when that swept volume does
not intersect cascade N's RECEIVER band (`[splitN, splitN+1]` in view-depth, within the
cascade footprint). High sun → near objects cast short shadows that never reach the
100 m+ band → correctly culled from 2–3; low sun → long shadow reaches → correctly kept.

**Landmines:** derive the reach from a real sun-elevation + scene-height bound and keep it
CONSERVATIVE (over-cull = missing shadows). Verify across sun angles AND camera movement.

**Acceptance:** far-cascade caster count drops; `Pass_CSM` ms down; **NO shadow disappears
at any sun angle** (USER confirm, sweep the sun + camera); no >3% GPU regression; debug
clean.

### 6d — Contribution / size cull (cheap complement to 6c)

**Risk: low–medium (popping as objects cross the threshold).**

Reject a caster from a cascade when its shadow footprint there is below ~1 texel. Reliably
drops tiny near clutter; keeps normal-sized props (a 1 m object is ~4 texels in cascade
3), so it is a partial win on its own — best stacked on 6c.

**Landmines:** thresholds too aggressive = popping on camera move. Tie to texel size and
add hysteresis.

**Acceptance:** small-object caster count drops on far cascades; `Pass_CSM` ms down; no
visible popping (USER confirm + camera movement); debug clean.

### 6e — Hoist the redundant shadow bucketization (CPU cleanup)

**Risk: none. CPU only — will NOT move GPU-bound FPS; flag honestly as cleanup.**
Order-independent.

`Bucketize` (`SceneRenderQueue.cpp:47`) re-walks ALL objects for every view, and the 4
cascades + spot views pass an identical `(cameraLayerMask, filterShadowCaster=true)` — the
shadow-caster set is rebuilt ~5× identically per frame. Compute it once and reuse for each
per-view `Cull`.

**Acceptance:** identical visuals; per-frame `Bucketize` work drops (instrument or reason
it); both configs build; debug clean.

## Step 7 — cleanup: remove dead shadow code

**Category: cleanup. Risk: none (no behavior change). Order-independent — can be taken
any time.**

- `SceneRenderer::RenderShadowBatch` (`SceneRenderer.cpp:372-423`, decl
  `SceneRenderer.h:56`) is **dead**: `Pass_CSM` inlines the per-cascade draw loop and
  nothing calls `RenderShadowBatch` (grep-confirmed). Remove the definition + decl, and
  its now-orphaned profiler scopes `kRenderShadowBatchAsync` / `kRenderShadowBatchGpu`
  (`ProfilerScopes.cpp:42-45`, `ProfilerScopes.h:48-51`) — but re-grep first; remove a
  scope only if nothing else references it.
- Remove the dead `static const float shadowBias` (`lighting_cs.hlsl:46`) if Step 4
  didn't already.

**Acceptance:** both configs build; `grep` shows no dangling references; visuals
identical; debug layer clean.

---

## Parked — larger programs (explicit user go-ahead required)

- **Static/dynamic shadow caching.** Shadows are re-rendered in FULL every frame — no
  temporal reuse. Largest structural GPU opportunity if most geometry is static: classify
  casters static/dynamic, cache the static cascade depth, re-render a cascade only when
  its stabilized center snaps to a new texel grid cell (the snap already exists,
  `Scene.cpp:159-190`; Step 2a makes the snap actually stable, which this depends on),
  and composite dynamic casters each frame. Multi-session, cache-invalidation-sensitive
  (light/camera move, hot-reload). Do NOT start without an explicit decision.
- **Adaptive split scheme.** Current splits are fixed view-space `{15,40,100,300}`
  (`SceneRenderConfig.h:8`), capped at `maxDistance` — not a log/lambda fit. Reasonable
  for the demo's fixed 90° FOV camera; the bigger quality levers are Steps 2–4, not the
  distances. Revisit only if the camera's usable range becomes dynamic. (If near shadows
  ever look soft, pulling split 0 from 15 m toward ~10 m is the first knob.)
- **D16 → D32 shadow depth.** `D16_UNORM` (`RenderTargetManager.cpp:330-410`) is low
  precision and forces larger bias (more acne/peter-panning pressure — and it interacts
  with Step 2b's near-plane extension). D32 eases that at 2× shadow bandwidth/memory — a
  measured tradeoff, only if bias tuning hits a wall.
- **Spot-shadow culling / sizing.** Skip off-screen or zero-contribution spot lights;
  `spotShadowRes = 512`. `Pass_SpotShadows` is ~0.08 ms — low priority vs CSM.
- **Atlas clear micro-opt.** Full 4096² `ClearDepthStencilView` each frame
  (`Renderer.cpp:1054`). Likely already cheap (one op) — quantify before touching;
  per-tile clears are usually NOT a win.

## Explicitly out of scope

- Ray-traced shadows, virtual/sparse shadow maps (engine-v2 scope).
- Moment / variance / exponential shadow maps (different algorithm + artifact class).
- Lighting model / BRDF changes.
- Command-list lifecycle / submission-ordering changes — owned by
  `docs/renderer_submission_instructions.md`; this work only preserves that contract
  (invariant 6), never modifies it.
