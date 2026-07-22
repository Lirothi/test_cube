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

## Engine state as it was BEFORE W1 (snapshot, 2026-07-22)

> Kept as the record of what the plan was written against. **It is stale now** — W1-W6 plus the
> post-W6 rewrite changed most of it (the instance struct is 224 bytes with FOUR mirrors, the PerView
> CB carries wind, the shadow VS sways). Read "Post-W6 — what actually shipped" and the Risks section
> for the current picture before touching anything.


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

## Post-W6 — what actually shipped (2026-07-22, committed as `improving wind on trees WIP`)

W1-W6 landed as written. Then a review of the result forced a rewrite of the sway math itself, plus
two real bug fixes. Recorded here because the steps below build directly on it:

- **Sway rewritten (the "Tier 0" pass).** The original `saturate(y * 1/3)^2` height weight flattened
  partway up the trunk and its derivative discontinuity was a visible KINK; above it the whole crown
  translated rigidly and traced a Lissajous figure (two incommensurate frequencies on perpendicular
  axes). Replaced with the GPU Gems 3 ch.16 profile `((1+u)^4 - (1+u)^2)/12` normalised by the
  ACTUAL mesh height, a length-preserving arc (`normalize(rel)*len`, built in WORLD space about the
  object origin — doing it in object space silently divides the lean by the object scale), and a
  steady downwind lean `bend = (1 + 0.35*osc) * gustMul` so the wind direction reads at all.
- **Wind direction was 180 deg off the water.** The ocean spectrum peaks for wave vectors k along
  `+(cos,sin)` but evolves with Tessendorf's `exp(+i*omega*t)`, so those waves travel toward `-k`:
  the ocean's `directionDeg` visually means "where the wind comes FROM". `WindState::Tick` now
  negates `windDirXZ`. The ocean is untouched.
- **Per-slot foliage + per-asset tuning.** `windFoliage` is a weight PER MATERIAL SLOT and
  `windTrunkStiffness` a per-object divisor of the main bend, both authored in `models/*.mesh.json`
  and editable in the Mesh Editor. They ride the existing 224-byte `InstancePerObject` (the
  per-slot weight is free because ShadowGpuData already allocates one caster id per (object, slot)).

**The open problem this leaves.** Foliage is still located by geometry heuristics — a leaf weight
built from distance to a single assumed crown point at `(0, 0.70*H, 0)`. That works for a
single-trunk palm and breaks on anything else. Measured on `coconut_palm`, a ONE-clump TWO-trunk
asset (trunk bases 0.24 m apart, but the trunks lean apart so the crowns end up ~2.4 m apart):
**36 % of frond vertices saturate at weight 1**, i.e. a third of the canopy has no base-to-tip
gradient and streams at full strength. No `objPos`-only formulation can fix this — one object space,
two crowns, any single reference point is wrong for at least one of them. W7 retires the heuristics.

## W7 — Per-vertex wind bake (retires the geometry heuristics)

*(exec: Fable for W7.1 — vertex format + input layouts touch the shadow mega-buffer and RT BLAS;
Opus 4.8 for W7.2/W7.3)*

The fix is baked per-vertex data instead of guessed geometry. **Validated on all three palm assets
before writing this** (prototype in the session scratchpad): geodesic distance along the mesh
surface from the plant's ground contact, normalised, gives a monotone trunk-base -> trunk-top ->
frond-base -> frond-tip gradient with **0 % saturation on every asset**, including the two-trunk
coconut, with no tuning constants at all.

| asset | trunk | collar | fronds | saturated |
|---|---|---|---|---|
| date_palm | 0.00-0.61 | 0.57-0.78 | 0.65-1.00 | 0 % |
| coconut_palm | 0.00-0.61 | 0.54-0.68 | 0.63-1.00 | 0 % |
| curly_palm | 0.00-0.32 | 0.25-0.87 | 0.33-1.00 | 1 % |

### W7.1 — Vertex channel  — **DONE (2026-07-23), uncommitted**

Landed: `VertexPNTUV` grew 48 -> 52 (`uint32_t color = 0` at offset 48, `static_assert(52)`); `COLOR_0`
R8G8B8A8_UNORM added to the `PosNormTanUV`, `PosOnly_InstCasterId`, and `PosUV_InstCasterId` presets
(explicit offset 48). Everything else auto-propagated via `sizeof(VertexPNTUV)` / `GetVertexStride()`
(mega buffer `MegaStride`, RT BLAS `stride`, VB views). No VS reads the channel yet, so unbaked meshes
are byte-identical. EditorPreviewRenderer needed no change (its explicit UV offset 40 stays valid).
Verified: Debug+Release clean, atoll `--shot` byte-identical (palms/shadows/RT intact), `--scene-stress=30`
verdict CLEAN.

Append `COLOR_0` as `R8G8B8A8_UNORM` to `VertexPNTUV` (48 -> 52 bytes). **Append at the END, offset
48**: the hardcoded UV offset 40 in `InputLayoutManager.cpp` / `EditorPreviewRenderer.cpp` and the
`PosUV_InstCasterId` preset then stay valid, and every other element already uses
`D3D12_APPEND_ALIGNED_ELEMENT`. Add the element to `PosNormTanUV` (gbuffer) and to BOTH shadow
presets (`PosOnly_InstCasterId`, `PosUV_InstCasterId`) — the depth-only shadow VS needs the same
weights or the shadow drifts off the tree. `sizeof(VertexPNTUV)` propagates the new stride to the
mega buffer and the RT BLAS automatically.

Channel budget: **R** = geodesic weight (W7.2), **G** = per-limb id for phase, **B** = along-limb
edge weight (the Crysis "edge stiffness" for leaf-edge flutter), **A** = spare.

**Why a vertex attribute and not a parallel `StructuredBuffer` indexed by `SV_VertexID`:** the
gbuffer draws from the per-mesh VB while the indirect shadow path draws from the MEGA buffer with
`BaseVertexLocation` rebasing — two different vertex index spaces for the same data, so the buffer
would have to be mega-consolidated and kept in sync. That is the same dual-source-of-truth shape
that produced the four-mirror `InstancePerObject` bugs. A vertex attribute rides the same VB, so
gbuffer and shadow agree for free, rebasing included.

**Verify:** build clean; `sizeof` assertions; a level with no baked assets renders byte-identically;
`--scene-stress=30`.

### W7.2 — Importer bake

At import (cached — do not recompute per load):
1. Weld vertices by position (UV seams would otherwise cut the graph).
2. Build the edge graph from triangles.
3. Seed from the ground-contact vertices (lowest Y band); multi-source Dijkstra -> geodesic distance
   along the surface. **Multi-trunk falls out for free** — each trunk seeds from its own contact.
4. Bridge disconnected islands (foliage is usually separate cards) to the nearest reachable vertex,
   cheapest gap first, then keep relaxing. This is what makes a frond hanging DOWN the trunk measure
   "up the trunk, then out along the frond" instead of reading as trunk.
5. Normalise by the global max -> channel R.
6. Per-limb id: walk back k metres along the parent chain to a "limb anchor" and hash its position
   -> channel G. Works on welded meshes too, where connected components would give one island.
7. Within a limb, distance from its principal axis -> channel B.

**Known failure modes — handle explicitly, do not pretend they do not exist:** a plant not touching
the ground (seed heuristic misses -> fall back to the lowest-Y band, expose a manual pivot in the
Mesh Editor); far-away junk geometry attaching via a long bridge and inheriting a high weight (cap
the bridge length, leave unreachable geometry at 0 = rigid); assets whose stiff end is at the top
(hanging vine — needs an invert flag). The prototype's brute-force nearest-neighbour bridging is
instant at ~5k verts but needs a spatial grid for 100k-vert trees.

**Verify:** dump the baked weights per material slot and assert the monotone ordering above; 0 %
saturation on all three palms.

### W7.3 — Switch the shader over and DELETE the heuristics

`wind.hlsli` reads the baked weight instead of computing one. **Remove** `kWindCrownHeightFrac`,
`kWindFrondSpanFrac` and the whole radial/crown-distance mask — the point is to retire the
heuristics, not to stack another layer on them. The per-slot `windFoliage` stays as the artistic
multiplier on top of the baked weight (existing hand-tuned values keep working). Bake always, so
there is no "unbaked" branch to maintain.

**Verify:** coconut palm canopy shows a proper base-to-tip gradient (the reported bug); date palm
unchanged or better; shadow still tracks (overhead-sun canopy-vs-shadow centroid test); motion
vectors clean at high `swayFrequency`; `--scene-stress=30`.

## W8 — Polish (was W7; revised 2026-07-23 now that most of the original list has shipped)

*(exec: Opus 4.8 / GPT terra)*

Reassessed against what actually landed:

- ~~Per-tree stiffness variation~~ — **delivered** as per-asset `windTrunkStiffness` plus a per-object
  level override. What is genuinely still missing is variation WITHOUT hand-authoring: a grove of
  identical palms currently differs only in phase, so amplitude and stiffness are uniform. Derive a
  small jitter from the same world-origin hash the phase already uses.
- ~~Secondary frond flutter~~ — **superseded by W7.2 channel B**; do not implement separately.
- **Wind-driven particle drift** — still wanted, still untouched. Smoke/embers bend downwind using
  the same `windDirXZ` (which is now the visible-travel direction, so it matches the water).
- **`vfx::g_windFreeze` debug toggle — promote this, it is not cosmetic.** Verification of this
  system has repeatedly been the hard part, and the one real trap was using `swayFrequency: 0` for
  deterministic screenshots: a static lean and a FROZEN shadow are pixel-identical, so that test was
  structurally blind to the VSM freeze bug that shipped. A freeze toggle plus "advance time by
  exactly dt" gives deterministic, diffable frames WITHOUT changing the wind parameters, and would
  have caught it. Build it before the next visual change, not after.
- **Gust -> ocean coupling** — W6 deliberately deferred pushing the gust envelope into the ocean
  `windForce01` (it rebuilds the ocean GPU resources; needs a GPU-idle + change gate). Still open.
- **Distance fade** — the per-vertex sway is full-rate ALU at any distance and can alias on distant
  foliage. Fade the amplitude out with distance / on coarse LODs.

---

## Risks / gotchas summary

- **Submesh + shadow sync** rides entirely on deriving the phase from the **world-space origin**
  (shared by all slots + the shadow instance) and using **one shared `wind.hlsli`**. Any per-slot
  or per-pass divergence (a different seed, a constant offset, gbuffer vs shadow math drift) breaks
  lockstep or detaches the shadow. This is the #1 thing to get right.
- **Motion vectors:** the gbuffer VS MUST offset the prev-position with `prevTime` (and last
  frame's gust) or DLSS/TAA smears the moving leaves. This is the W4 pass/fail.
- **`InstancePerObject` has FOUR mirrors, not three.** C++ `InstanceTypes.h` (+ `static_assert`), the
  HLSL `InstancePerObject` AND the `PerObject` cbuffer in `gbuffer_common.hlsli`,
  `shadow_indirect_csm.hlsl`, and **`shadow_gi_scatter_cs.hlsl`**. The scatter one was missed during
  the 208 -> 224 grow and left the GI ids' tail uninitialised; once the shadow VS started reading
  `windStrength` from there, every GPU-instanced shadow scrambled. It is currently full again at 224
  (windStrength / windInvHeight / windFoliage / windTrunkStiff).
- **Uninitialised instance data is nondeterministic — a plain A/B will not reproduce it.** Removing
  the GI scatter's write again showed almost no diff because that heap memory happened to read as
  zero. What reproduces it is INJECTING the garbage (write `1.0e18f`) on a level that has a `wind`
  section. Note `WindOffset` early-outs on `windStrength <= 0` and scales by `swayAmp`, so on a level
  with no `wind` block garbage is harmless unless it is NaN/Inf — hence "works on my machine".
- **`RenderableObject::RenderShadow` does NOT clear its dynamic b0 allocation**, and
  `GBufferRenderable::UpdateShadowCB` only writes what it is told to. Any field the CPU-fallback
  shadow VS reads must be written there explicitly or it gets recycled frame-ring garbage (seen as a
  stray, wildly displaced shadow).
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
- **A vertex-shader-animated caster is neither a "mover" nor a dynamic caster.** Its transform never
  changes, so every "nothing changed, reuse last frame" gate freezes its shadow while the gbuffer
  tree keeps swaying: the VSM still-frame skip in `SceneRenderer` and the VSM per-page dirty test
  both have to consult `ShadowGpuData::HasWindCasters()`.
- **`swayFrequency: 0` makes screenshots deterministic and is therefore a TRAP.** A static lean and a
  frozen shadow are pixel-identical, so that test cannot see a caching/skip bug — exactly how the
  VSM freeze shipped. Always pair it with a TIME-VARYING test (two shots N seconds apart). W8's
  freeze toggle is the proper fix.
- **Test the GI path separately.** GPU-instanced casters never touch `FillInstanceData`; they go
  through `shadow_gi_scatter_cs.hlsl`. Any new per-instance field must be verified on a level with
  `instancedModels` (demo.json), not just on palms.
- **A shader that fails to compile is nearly silent** — the engine falls back to D3DCompile SM5 and
  carries on. The DBWIN tell is `[Material] DXC VS failed, fallback to D3DCompile SM5` +
  `CreateGraphics failed`; grep for it on every run. To find the culprit fast, compile standalone:
  `dxc.exe -T vs_6_6 -E VSMain -I shaders shaders/<file>.hlsl` (repeat with `-D INSTCB_SLOT_PARAMS=1`
  for that file's second variant — `cbuffer SlotParams` only exists in the multi-slot build).
- **Verify with `--shot`, don't assume** — and for motion, actually check for DLSS smear. (The last
  render-order fix was declared done without confirming the submit order actually changed; don't
  repeat that. See memory [[transparent-pass-bundle-ordering]].)

## Suggested order

W1 → W2 (wind source + ocean sync, no foliage yet — already a visible, testable result) → W3 → W4
(trees sway in view) → W5 (shadows match) → W6 (gusts + editor). **All six shipped**, followed by the
unnumbered post-W6 rewrite recorded above.

Next: **W7.1 → W7.2 → W7.3** (vertex channel → importer bake → switch the shader over and delete the
heuristics). Do W7.3's deletion in the same pass as the switch — leaving the old crown/radial mask in
place "just in case" is how the heuristics accumulate.

Then **W8** as time allows, except the `g_windFreeze` toggle, which is worth pulling forward ahead of
W7 because every W7 verification step wants it. Each step is independently buildable and
`--shot`-verifiable.
