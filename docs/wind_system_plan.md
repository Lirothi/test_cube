# Wind System — Implementation Plan

Goal: a **global wind** that drives the ocean and animated foliage from one source, so waves and
tree sway share direction, strength, and timing. Palms (and other flagged foliage) bend in the
wind with **matching shadows**, all **submeshes of a tree stay in lockstep**, and the wind
**gusts** — periodic stronger bends layered over a steady sway. Motion-vector correct (no DLSS/TAA
smear). Authored via a **`wind` level entity** (like `ocean`/`skybox`), editable in the editor.

## Decisions locked with the user

- **One wind source of truth.** A `wind` level entity holds direction + strength + gust + sway
  params. It drives BOTH the ocean (`OceanSimulation::SetSceneVariables`) and the tree-sway CBs.
  Changing the wind object rotates the waves and the sway together.
- **Full sway with shadows**, not a main-view-only cut. Palm shadows sway in sync (CSM + VSM).
- **All submeshes of a tree move together.** A palm is one multi-slot glTF object (trunk +
  frond submeshes, B2/B2b). The sway must be identical across every submesh/slot AND between the
  gbuffer and shadow passes.
- **Gusts on trees.** A slow gust envelope modulates the sway amplitude (and slightly the
  direction), coherent across all trees. The steady wind direction/force is shared with the ocean;
  the fast gust layer is primarily a foliage effect (the ocean FFT responds slowly).
- **Realistic, not procedural-robotic:** layered sines (not a single sine), height-weighted so
  the trunk base is planted and the canopy is floppy.

## The two sync mechanisms (read before coding — these are the whole design)

1. **Per-tree phase from the world-space origin.** Compute the sway phase from the object's
   world-matrix translation (`world._41.._43` — the tree's base position), NOT from a per-object
   seed in a CB. Every submesh/slot of a tree shares the same object world matrix, and so does the
   shadow instance data — so **submesh-sync and shadow-match are automatic and free**. Per-tree
   variation (trees don't sway in unison) comes from the spatial phase term. Instances (auto-
   instancing / GpuInstancedModels) each carry their own world matrix → per-instance phase, also
   automatic.
2. **One shared wind function.** `shaders/wind.hlsli` defines the offset. The gbuffer VS and the
   shadow VS BOTH include it and call it identically. If the two diverge by even a constant, the
   shadow detaches from the tree. Treat wind.hlsli as the single source of the math.

## Current engine state (verified 2026-07-22)

- **Ocean wind:** `OceanSimulation::SetSceneVariables(renderer, localWindDirectionDegrees,
  swellDirectionDegrees, windForce01)` (`sources/ocean/OceanSimulation.cpp:234`). Level JSON
  `"ocean"` already accepts `windDirectionDeg`/`swellDirectionDeg`/`windForce`
  (`sources/app/levels/JsonLevel.cpp:371`). Ocean windForce is normalized **0..1** (`windForce01`).
- **Ocean clock:** `OceanRenderable::elapsedTime_` drives `simulation_->Update(..., elapsedTime_)`
  (`sources/ocean/OceanRenderable.cpp:529`). Reuse this exact accumulator for the wind time so
  waves/sway/gusts are phase-coherent.
- **gbuffer VS:** all variants share `BaseVS` in `shaders/gbuffer_common.hlsli` (builds `worldPos`,
  `clipH`, and `prevH` for motion vectors). The shared PerView CB (`b1`) has only viewProj /
  viewProjNoJitter / prevViewProjNoJitter — **no time**. Per-object params flow through the b0
  PerObject CB / instance array (baseColor, metalRough, alphaCutoff, texOffsScale, texFlags,
  objectId, emissive) — the emissive field (Part D) is the template for adding one more.
- **Shadows:** the GPU-driven indirect path `shaders/shadow_indirect_csm.hlsl` (VSMain does its own
  `TransformPositionH(i.P, world, viewProj)`, reading `Instances[casterId].world`) serves BOTH CSM
  per-view AND VSM per-page draws — one edit covers both. It has an opaque variant and a
  `SHADOW_MASKED` variant (fronds, `PosUV_InstCasterId` layout). The CPU-fallback per-object shadow
  is `shaders/gbuffer_csm.hlsl` (rarely used; the indirect path is default).
- **Shadow instance data:** `render::InstancePerObject` (`sources/rendering/renderables/
  InstanceTypes.h:26`) is 208 bytes and — as of 2026-07-22 — **fully packed, no free scalar left**.
  The `float _pad0` the earlier plan targeted (offset 156) is now `float mrMultiply` (the MR-texture
  blend flag), and `texFlags.w` (its comment still reads "reserved") is actually the normal-map
  strength (`gbuffer_common.hlsli:206,212`). Filled by `ShadowGpuData::FillInstance`. The
  StructuredBuffer stride is shared with every reader — the C++ struct, the HLSL `InstancePerObject`
  in `gbuffer_common.hlsli`, and the mirror in `shadow_indirect_csm.hlsl:35` (which aliases the
  gbuffer's `emissive` tail as unused `uint3 _instPad1`). B3 lesson: it silently breaks shadow reads
  unless ALL three move in lockstep. So `windStrength` **cannot slot into a free pad** — it forces a
  deliberate grow (208 → 224, 16-byte aligned) across those three mirrors + the `static_assert`
  (see W3/W5).
- **Per-material surface params (new since the SSS / "mats for palms" work):** a separate
  `SurfaceParams` cbuffer (`render::MaterialSurfaceParamsGpu`, 32 bytes, register **b2**,
  `InstanceTypes.h:10`) was added precisely so foliage/SSS controls do NOT grow the 208-byte
  per-instance payload. It is **gbuffer-only** (the shadow passes don't bind it), so it can hold
  gbuffer-side foliage knobs but **cannot carry `windStrength` for the shadow sway** — that still
  has to ride in `InstancePerObject`.
- **Caster bounds:** the Rung-0 / VSM per-page cull tests a static world AABB per caster
  (`ShadowGpuData::FillBounds`). Swaying vertices exceed it.
- **Verification tool:** `test_cube.exe --level=<lvl> --shot=<out.png> --shot-delay=<sec>` reads
  back the backbuffer to a PNG reliably (flip-model safe). USE IT to confirm each visual step —
  do not assume. For MOTION correctness, capture two frames a known dt apart and diff, or eyeball a
  short run (a swaying tree with wrong motion vectors smears under DLSS 2).

## Conventions for the executor

- Build from PowerShell (never bash): `msbuild test_cube.sln /p:Configuration=Release
  /p:Platform=x64 /m /v:m`; the editor build is `Release_Editor`. Every step ends with a Release
  build + `--shot` visual check on `data/levels/atoll_a2_test.json` (has palms) or a small wind
  test level (W2 adds one).
- **Executor model per step** (`exec:` tag): **Fable** = renderer/shadow/GPU-lifetime hazards +
  motion-vector correctness; **Opus 4.8** = contained shader/CB/editor with a clear spec;
  **GPT 5.6 terra** = JSON/preset/tuning + eyeball-verifiable. Escalate one tier after two failed
  attempts or an unexplained regression. Diffs touching `sources/rendering` / shadow paths get a
  Fable review before commit.
- After W5 and W6, run `--scene-stress=30` (a wind-object level in the churn cycle) — new global
  state must survive level switches with 0 device hangs.
- **No `dynamic_cast`** (engine rule); use the internal-RTTI accessor pattern.

---

## W1 — Global wind state + shared clock

*(exec: Opus 4.8 — contained CPU state)*

Define `sources/vfx/WindState.h` (or `sources/scene/`): a `WindState` struct holding the authored
params (direction degrees, strength 0..1, sway frequency, gust amplitude/frequency/seed, foliage
sway amplitude in metres) plus per-frame derived values (`time`, `prevTime`, `windDirXZ` unit
vector, `swayAmplitude`, `gustMul`). A single owner (Systems or Scene, mirroring how the ocean sim
is owned) updates it each frame from the **same elapsed-time accumulator the ocean uses**
(`OceanRenderable::elapsedTime_` — thread it in or hoist the accumulator so both read one clock).
`prevTime` = last frame's `time` (for motion vectors). No rendering yet.

**Verify:** unit-level — `time` advances monotonically, `windDirXZ` is unit-length, matches the
ocean's `elapsedTime_` exactly (log both for a few frames).

## W2 — `wind` level entity + ocean sync + editor entity

*(exec: Opus 4.8; escalate to Fable if the ocean-drive timing fights back)*

Parse a top-level `"wind"` level section (like `"ocean"`/`"skybox"`), e.g.
`{ "directionDeg": 40, "strength": 0.6, "swayFrequency": 0.9, "gust": { "amplitude": 0.5,
"frequencyHz": 0.15, "seed": 3 }, "foliageSwayMeters": 0.25 }` → populate `WindState`. Each frame,
push the wind's direction + force to the ocean:
`oceanSim->SetSceneVariables(renderer, windState.directionDeg, swellDeg, windState.strength)` (map
strength→`windForce01`). If no `wind` section exists, wind is disabled (strength 0 → everything
rigid, ocean keeps its own preset values — back-compat). Editor: register the wind as an
environment entity (mirror the directional-light/ocean entity in `EditorSceneDocument` +
`LevelDocumentSerializer`) so it round-trips; inspector editing lands in W6.

**Verify (`--shot`):** on a level with ocean, set the wind object `directionDeg` and confirm the
wave crests rotate to match; remove the `wind` section → ocean unchanged from its preset.

## W3 — Wind CB plumbing to the gbuffer VS + per-object windStrength

*(exec: Opus 4.8 — mirror the Part D emissive plumbing exactly)*

- Extend the PerView CB (`b1`, `PerViewCB`/`GlassViewCB` in `SceneRenderer.cpp` + the HLSL
  `cbuffer PerView`) with wind fields: `float time, prevTime; float2 windDirXZ; float swayAmp,
  swayFreq, gustMul, _pad;`. Fill from `WindState` in the per-view CB build.
- Add a per-object `windStrength` (0 = rigid) to `MaterialParams` + the b0 PerObject CB +
  `render::InstancePerObject` (gbuffer instanced) + `InstanceSlotParams` (multi-slot instanced) —
  the same set of touch-points Part D used for emissive.
- **Stride note (changed 2026-07-22):** `render::InstancePerObject` is now full at 208 bytes (the
  `_pad0` the plan assumed became `mrMultiply`), so this is the step that **grows it to 224** in
  lockstep — the C++ struct + `static_assert(224)`, the HLSL `InstancePerObject` in
  `gbuffer_common.hlsli`, AND the shadow mirror in `shadow_indirect_csm.hlsl:35`. Do it here (one
  struct feeds both the gbuffer and shadow paths); W5 then only reads the field. `InstanceSlotParams`
  still has a genuine free `float _pad1` at offset 76 — use it there (no grow needed on that struct).
- JSON: `"windStrength"` on a `staticMesh` object, applied to **every slot** (uniform across the
  tree — critical for submesh sync). glTF-auto palms: set it after the glTF slot seeding (like the
  alphaCutoff pass in `ResolveMaterialSlots`).

**Verify:** build clean; a temporary shader debug (tint by windStrength) shows only flagged trees
lit, all their submeshes uniformly.

## W4 — Shared wind function + gbuffer sway (main view, motion-vector correct)

*(exec: Fable — motion-vector correctness is the hazard)*

New `shaders/wind.hlsli`:
`float3 WindOffset(float3 objPos, float3 worldOrigin, float windStrength, float2 windDirXZ,
float swayAmp, float swayFreq, float gustMul, float t)`:
- height weight `h = saturate(objPos.y * kInvTreeHeight)` (palms base-at-0 per A2; expose the
  height scale as a constant or a per-object value), squared so the base is stiff and the canopy
  floppy;
- phase `p = t * swayFreq + dot(worldOrigin.xz, float2(0.13, 0.17))` (per-tree variation from the
  world origin — the sync anchor);
- `sway = sin(p) + 0.5*sin(p*2.3 + 1.7)` (layered) `* swayAmp * gustMul * windStrength * h`;
- offset horizontally along `windDirXZ` (+ a small perpendicular flutter term for leaves);
- return the world-space offset (added to worldPos).

In `BaseVS`: apply `WindOffset(pos, world._41_42_43, windStrength, windDirXZ, swayAmp, swayFreq,
gustMul, time)` to `worldPos`, AND apply it with `prevTime` (+ last frame's gustMul) to the prev
world position that feeds `o.prevH` — so motion vectors track the sway (else DLSS 2 smears the
leaves). All 3 gbuffer variants get it free (shared BaseVS). Non-tree objects have windStrength 0
→ offset 0 → byte-identical to today.

**Verify (`--shot` + short run):** palms sway; ground/box/rocks rigid; every submesh of a palm
moves in lockstep (trunk top meets fronds, no tearing); no smear/ghosting under DLSS (motion
vectors correct) — this is the pass/fail for prev-sway.

## W5 — Shadow sway (CSM + VSM) matching the gbuffer

*(exec: Fable — shadow instance data + cull bounds + exact match)*

- `float windStrength` is already on `render::InstancePerObject` from W3 (the struct grew 208 → 224
  there — see the W3 stride note; there is no `_pad0` to reuse anymore, and the shadow mirror in
  `shadow_indirect_csm.hlsl:35` grew with it). Here, just make the shadow path read it:
  `ShadowGpuData::FillInstance` sets `windStrength` from the object (mirror how it reads other
  per-object fields).
- Add `time` + wind params to the shadow view CB (`b1`) used by the indirect shadow passes (both
  the per-view CSM path and the VSM per-page path bind it).
- In `shadow_indirect_csm.hlsl` VSMain (opaque AND `SHADOW_MASKED` variants), include `wind.hlsli`
  and apply the SAME `WindOffset(i.P, world._41_42_43, inst.windStrength, ...)` before
  `TransformPositionH`. Because the phase comes from the world origin (shared) and the function is
  shared, the shadow bends identically to the gbuffer tree.
- **Pad tree caster bounds** by the max sway amplitude in `ShadowGpuData::FillBounds` (only for
  windStrength>0 casters) so the VSM per-page / Rung-0 cull doesn't clip a swaying frond tip.
- CPU-fallback `gbuffer_csm.hlsl` sway is OPTIONAL (indirect is default) — note it; do it only if a
  scene actually hits the fallback.

**Verify (`--shot`, VSM and Legacy via Ctrl+V):** palm shadows sway in sync with the trees; the
shadow silhouette tracks the frond motion; `cull validation PASS` still holds; no shadow popping at
page edges (bounds padding). `--scene-stress=30`.

## W6 — Gusts + wind-object editor UX + tuning

*(exec: GPT 5.6 terra for JSON/tuning; Opus 4.8 for the inspector)*

- **Gust envelope:** in `WindState` update, `gustMul = 1 + gustAmp * gustNoise(time*gustFreq,
  seed)` where `gustNoise` is layered value-noise / summed low-freq sines (NOT per-frame white
  noise — that strobes). Feed `gustMul` into the wind CB (already plumbed W3 — and, since W5, into
  the shadow PerView CB and the VSM per-page `pageProj` slot too, so gusts reach shadows for free).
  **Motion-vector note (W5):** `BaseVS` currently reuses the CURRENT `windGustMul` for the prev
  position because gustMul is pinned to 1.0. The moment it varies per frame, add a `prevGustMul` to
  the CB and feed it to the prev-position `WindOffset`, or DLSS/TAA smears the leaves again (re-run
  W4's A/B with `swayFrequency` ~10 to make the motion supra-pixel). Optionally feed a
  smoothed gust into the ocean `windForce01` so whitecaps swell during gusts (keep subtle — the
  FFT lags). All trees share `gustMul` (coherent gusts), each still varies by its world-origin
  phase.
- **Editor inspector** for the wind entity: direction, strength, sway frequency, gust
  amplitude/frequency, foliage sway metres — live-applied to `WindState` (mirror the ocean/light
  property drawers). Round-trips through the document (W2).
- **Tuning:** author a sensible default `wind` block in `data/levels/atoll.json` (breeze rippling
  the palms, gentle gusts); tune against the actual atoll lighting.

**Verify (short run):** visible gusts — palms periodically bend harder together, then relax; the
ocean direction matches the trees; editor edits apply live and survive save/reload.

## W7 (optional polish)

*(exec: Opus 4.8 / GPT terra)* Per-tree stiffness variation (a `windStiffness` per object);
secondary high-frequency low-amplitude frond flutter (a second height band or UV-driven term on
the frond slot only); wind-driven particle drift (smoke/embers bend downwind using the same
`windDirXZ`); a `vfx::g_windFreeze` debug toggle.

---

## Risks / gotchas summary

- **Submesh + shadow sync** rides entirely on deriving the phase from the **world-space origin**
  (shared by all slots + the shadow instance) and using **one shared `wind.hlsli`**. Any per-slot
  or per-pass divergence (a different seed, a constant offset, gbuffer vs shadow math drift) breaks
  lockstep or detaches the shadow. This is the #1 thing to get right.
- **Motion vectors:** the gbuffer VS MUST offset the prev-position with `prevTime` (and last
  frame's gust) or DLSS/TAA smears the moving leaves. This is the W4 pass/fail.
- **`InstancePerObject` stride is shared with every shadow reader** — as of 2026-07-22 it is full at
  208 bytes (no `_pad0` left; it became `mrMultiply`). Adding `windStrength` therefore **requires
  growing it to 224** (16-byte aligned) and updating ALL mirrors in lockstep: the C++ struct +
  `static_assert`, the HLSL `InstancePerObject` in `gbuffer_common.hlsli`, and the
  `shadow_indirect_csm.hlsl:35` copy. Miss one and shadow draws silently corrupt (the B3 stride
  lesson). Do the grow once in W3.
- **Caster bounds** must be padded by the sway amplitude for tree casters, or the VSM per-page /
  Rung-0 cull clips swaying tips → shadow flicker at page/cascade edges.
- **One clock:** wind time must equal the ocean's `elapsedTime_`, or waves and sway visibly drift
  apart and gusts desync.
- **Per-slot windStrength uniformity:** set it on ALL slots of a tree; a mismatched slot bends out
  of sync with the rest of the same tree.
- **windForce is 0..1** (`windForce01`) on the ocean side; map the wind object's strength into that
  range when calling `SetSceneVariables`.
- **Height weight assumes base-at-0** object space (true for the staged palms per A2). An asset
  authored off-origin would sway about the wrong pivot — expose the height scale / pivot if needed.
- **Verify with `--shot`, don't assume** — and for motion, actually check for DLSS smear. (The last
  render-order fix was declared done without confirming the submit order actually changed; don't
  repeat that. See memory [[transparent-pass-bundle-ordering]].)

## Suggested order

W1 → W2 (wind source + ocean sync, no foliage yet — already a visible, testable result) → W3 → W4
(trees sway in view) → W5 (shadows match) → W6 (gusts + editor). W7 as time allows. Each step is
independently buildable and `--shot`-verifiable.
