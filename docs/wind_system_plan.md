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

### W7.1b — Binary mesh: our `.mesh.bin` becomes the committed geometry asset  *(exec: Opus 4.8)*

**REFINED 2026-07-23 (supersedes the earlier gitignored-cache decision).** Since we have our own
binary, the glTF + its `.bin` should NOT live in `models/` — they stay in `import_staging/` (source,
gitignored) and our `.mesh.bin` takes their place in `models/` (COMMITTED — a fresh clone has no
staging, so the committed `.bin` is the only geometry) with `mesh.json` referencing it. Net pipeline:
- `import_staging/<name>/` : raw glTF + its `.bin` + source textures (gitignored, kept for re-import).
- `models/<name>.mesh.bin` : our baked geometry (verts incl. wind color + normals/tangents + LODs), COMMITTED.
- `models/<name>.mesh.json` : `"geometry": "models/<name>.mesh.bin"` (+ `"source": "import_staging/<name>/scene.gltf"` for re-import) + materials/windFoliage/etc. COMMITTED.
- `models/<name>/*.dds` : converted textures, still emitted to `models/` (needed at runtime).
- Runtime: `mesh.json.geometry` → `.mesh.bin` → `MeshManager::Load` reads it directly (no cgltf ever). No glTF in the shipped tree, so **no runtime fallback** for a `.mesh.bin` reference (a missing `.bin` is a content error, not a soft-fall to glTF).

**DONE (keystone, builds clean):** `MeshManager::Load` handles a `.mesh.bin` geometry path directly
(`LoadBinaryDirect` — version-checked read, no source hash, no fallback); the binary format +
`BuildLodsCpu` + `BakeToBinary` + `--bake-meshes` all in place from the first pass. `ReadMeshBinary`
freshness checks are now optional (null for a directly-referenced committed `.bin`).

**Final decisions (user, 2026-07-23):** `.bin` **gitignored** (already covered by `/models/**` — only `.mesh.json` is tracked; nothing to commit). Don't copy glTF back to staging — the source already lives in `import_staging/<name>/`. `mesh.json` gains `"source"` → the staging glTF (for re-import). The earlier hashed `cache/` path + `--bake-meshes` bake-mode were removed; a headless CPU-only `--reimport-src=<glTF> --reimport-out=<.bin> [--reimport-recompute=n,n]` CLI (no device/window) bakes explicitly.

**DONE + verified (3 palms migrated):** `date_palm`/`coconut_palm`/`curly_palm`.mesh.json now `"geometry":"models/<name>.mesh.bin"` + `"source":"import_staging/<name>/scene.gltf"`; `.bin`s baked via `--reimport` (coconut `--reimport-recompute=2,3,4`, curly `=1` — MUST match the mesh.json recomputeNormalSlots or the baked normals differ from the runtime look). wind_test + atoll render correctly with **0 palm glTF loads** (all from `.bin`); Debug+Release clean.

**FIXES (2026-07-23, after user testing):**
- **Winding bug**: the runtime loads glTF with `wantCW=false` (`StaticMesh.cpp:57`, `TransparentStaticMesh.cpp:156`); the bake used the default `wantCW=true` → flipped triangles → backfaces. Fix: `--reimport` sets `opt.wantCW=false`. Re-baked all assets.
- **Location**: `.bin` now lives in `models/<name>/<name>.mesh.bin` (same folder as the glTF was), not `models/` root. mesh.json `geometry` updated.

**DONE + verified:**
- **Item 3** — removed dead hashed-cache methods (`BinCachePath`/`TryLoadBinary`/old `BakeToBinary(path,opt)`) + `/cache/` gitignore line. Builds clean.
- **Item 2** — ALL glTF assets migrated to `.bin` + glTF+`scene.bin` **pruned** from `models/` (DDS kept): 3 palms + 2 boulders + rocks (2 `#node` sub-meshes). mesh.json `geometry`→`.bin` + `source`→staging glTF. atoll renders all of them from `.bin` with correct winding, glTF absent from `models/`.

**Item 1 — ImportPanel rework — DONE (2026-07-23, Debug-build-verified; in-editor import test pending):**
- `WriteImportedMeshAsset(path, binGeometry, sourceGltf, ...)`: bakes `sourceGltf → binGeometry` via `MeshManager::BakeToBinary` (wantCW=false + the mesh.json's authored `recomputeNormalSlots`, which survive re-import), writes `geometry`=`.bin` + `source`=staging glTF, and generates material files from the SOURCE glTF.
- `RecreateMeshAssets`: `binGeom = models/<name>/<name>.mesh.bin` (+ per-node for splits), `source = item.gltfFile` (staging, +`#node:` for splits).
- Copy loop: `.dds` only (glTF/glb/bin no longer copied into `models/`; the source stays in `import_staging/`).
- After `RecreateMeshAssets`, `RepointPresetPaths(import_staging/<name>/ → models/<name>/)` (material texture paths; mirrors the TextureSet repoint). No-op on re-import (material files preserved).
- Debug clean. Bake step = the same `BakeToBinary` the `--reimport` migration proved. **Still needs one real in-editor import of a NEW asset to confirm the full flow (copy/material-gen/repoint) end to end.**

**REMAINING:**
1. W7.2: fill `VertexPNTUV.color` (geodesic bake) inside `BakeToBinary` before serialize.

--- (original W7.1b spec below, now partly superseded by the above) ---

### W7.1b — Binary mesh cache (`.mesh.bin`) — the store the bake needs  *(exec: Opus 4.8)* — **CORE DONE (2026-07-23), uncommitted**

Landed + verified: format `cache/meshes/<hash>.mesh.bin` (header magic/version/sourceHash/optionsHash + `VertexPNTUV[]` incl. color + per-LOD index/submesh tables). `GenerateLods` split into CPU `BuildLodsCpu` (reused) + GPU `AddLod`. `MeshManager::BakeToBinary` (parse→regen normals/tangents→[W7.2 color TODO]→CPU LOD simplify→serialize), `TryLoadBinary` (fresh `.bin` → `CreateGPU_PNTUV`+`AddLod`, no cgltf), `Load` = in-mem cache → bake-mode → `TryLoadBinary` → **runtime fallback**. `--bake-meshes` CLI (`g_meshBakeMode`: every Load rebakes with its exact (path,opt), quits ~2s after boot). `cache/` gitignored. Verified: Debug+Release clean; fallback (no `.bin`) renders correctly; `--bake-meshes` wrote 5 valid `.bin`s (date_palm 7175v/4 LODs, etc.); baked load renders identical geometry; `--scene-stress=30` CLEAN. **STILL TODO: Mesh Editor "Rebake" button** (thin wrapper on BakeToBinary; the headless CLI is the verified mechanism).



Decided with the user 2026-07-23: `.mesh.json` is only a *reference* (geometry + materials), so today verts/indices are re-parsed from glTF and **normals/tangents regenerated + LODs simplified every load**, and there is **nowhere to persist a baked vertex color**. A geodesic Dijkstra bake (W7.2) per load is a non-starter. So introduce a baked binary mesh — the store for W7.2's color and the home for the (moved) import-time processing.

- **Format** `cache/meshes/<hash>.bin`: header `{magic 'MSHB', MESH_BIN_VERSION, source-glTF content hash, optionsHash(recomputeNormalSlots)}` + AABB + `VertexPNTUV[]` (52 B incl. baked `COLOR_0`) + LOD count, then per LOD `{index[] + submesh[]}`. LODs are reduced **index** buffers over the SAME verts (see `GenerateLods`), so verts are stored once.
- **Bake trigger = EXPLICIT (user's choice), not lazy-on-load.** A headless `--bake-meshes[=<level>]` CLI (bakes every mesh a level references; reusable for CI + `--shot` verification) plus a Mesh Editor "Reimport/Rebake" button. The bake: parse glTF → `DiscardNormalsForSlots`+`GenerateNormalsTangents` → [W7.2 geodesic color] → CPU LOD simplify → serialize.
- **Load** (`MeshManager::Load`): compute cache path from (path, opt); if a fresh `.bin` exists (version + source hash + optionsHash all match) → read → `CreateGPU_PNTUV`(LOD0) + `AddLod` per LOD (no cgltf, no normal regen, no simplify). **Else → today's runtime path** (parse + normal gen + `GenerateLods`), `color=0`. The **fallback is mandatory** because the cache is gitignored (user's choice) — fresh clone / headless / CI have no `.bin` until an asset is imported, and unbaked meshes must stay byte-identical.
- **Invalidation:** bump `MESH_BIN_VERSION` (algo/format change) or a source-glTF hash change → stale → fallback until re-baked. `windFoliage`/`windTrunkStiffness` are RUNTIME shader params → NOT in the hash (editing them never re-bakes).
- **Refactor:** split `GenerateLods` into a CPU part (produce LOD index+submesh arrays) reused by both the bake (serialize) and the runtime fallback (`AddLod`).
- Add `cache/` to `.gitignore`.

**Verify:** build clean; a level with no `.bin` renders byte-identically (fallback); `--bake-meshes` writes valid `.bin`s; loading the baked `.bin` renders identically to the fallback + skips cgltf/normal-gen/simplify; `--scene-stress=30`.

### W7.2 — Importer bake  — **DONE (2026-07-23), uncommitted**

Landed as `BakeWindWeightsCpu` in MeshManager.cpp, called from `BakeToBinary` before serialize.
Verified by reading the baked `.bin` back and comparing per material slot against the Python
prototype — identical within 8-bit quantisation, **0 % saturation on all three palms**:

| asset | slot | R range | distinct G |
|---|---|---|---|
| date_palm | 0 trunk | 0.00-0.61 | 1 |
| date_palm | 1 fronds | 0.64-1.00 | **35** (= its 35 frond islands) |
| coconut_palm | 0 trunk | 0.00-0.61 | **2** (= its two trunks) |
| coconut_palm | 2 fronds | 0.62-1.00 | **36** |
| curly_palm | 1 leaves | 0.33-1.00 | 14 |

Decisions made during the bake that differ from the spec below:
- **G is the hashed CONNECTED-COMPONENT id, not a parent-walk anchor.** The parent walk was tried
  first and measured 164 distinct anchors across 35 fronds — "walk back k metres from ME" is a
  per-VERTEX point, not a per-limb one, so a single leaf got ~5 phases and would tear. Game foliage
  is card-based (each frond is its own island), so the component IS the limb. A welded single-island
  mesh degrades to one phase for the whole plant: boring, not broken.
- **B is zeroed on any component that touches the ground** (the trunk): an "edge distance from the
  limb axis" is meaningless there and a non-zero value would invite a shader to flutter the trunk.
- **No `MESH_BIN_VERSION` bump.** `LoadBinaryDirect` has no fallback, so a bump would hard-fail every
  already-baked `.bin` at once. Instead **A = 255 marks "weights baked"**; legacy `.bin` have A = 0,
  which W7.3 uses to keep them on the old heuristic until re-baked.
- Bridging uses a uniform spatial grid (the prototype's brute-force NN is O(unreached x reached) and
  does not scale past a few thousand verts), with the bridge capped at 15 % of the bbox diagonal —
  anything further stays unreached and bakes rigid, which is the documented stray-geometry guard.

Re-bake command (must match the mesh.json `recomputeNormalSlots` or the normals drift):
`test_cube.exe --reimport-src=import_staging/<n>/scene.gltf --reimport-out=models/<n>/<n>.mesh.bin [--reimport-recompute=2,3,4]`

--- (original spec) ---

### W7.2 — Importer bake (spec)

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

### W7.3 — Switch the shader over and DELETE the heuristics — **DONE (2026-07-23), uncommitted**

`wind.hlsli` now reads the baked COLOR_0. **Deleted:** `kWindCrownHeightFrac`, `kWindFrondSpanFrac`,
the radial/crown-distance mask, and `windInvHeight` (the baked geodesic weight replaced the
height profile, so the per-object field became dead — renamed to `_windReserved` in all four mirrors
rather than removed, which keeps the 224-byte stride and every offset below it put).

Channel meanings as shipped: **r** = geodesic weight (drives `WindBendProfile`, replacing
`objPos.y * invHeight`), **g** = limb id -> per-frond phase, **b** = position ALONG the vertex's own
limb, **a** = baked marker.

**The one real trap, found by rendering it:** the foliage streaming term must use **b**, not **r**.
Driving it with r gives a coconut frond BASE 0.62 of the push (r is 0.62..1.00 across a frond), so the
whole leaf translates off the crown — the canopy visibly tore into thin streaks and the near palm was
left as a tuft. b is renormalised per limb (0 at that limb's own attachment), which also makes the
leaf term continuous with the trunk across the attachment instead of jumping 2x there. The B channel
originally held a leaf-edge weight; along-limb is strictly more important, so the edge weight is
deferred to W8.

**Verified:** all 9 VS variants compile standalone (dxc, incl. `SHADOW_MASKED=1` and
`INSTCB_SLOT_PARAMS=1`); coconut canopy intact with fronds streaming from their bases; shadow tracks
(overhead sun: canopy +24.8 px, shadow +19.2 px); motion vectors clean at `swayFrequency 10`;
atoll unregressed; `cull validation PASS`; 0 DXC failures; `--scene-stress=30` CLEAN; Debug+Release clean.

**NOT re-baked:** everything except the three palms still has a == 0 and is therefore rigid. That is
correct today (nothing else carries windStrength) but any NEW foliage asset must be baked or it will
simply not move.

### W7.4 — `b` becomes a distance from the wood surface — **DONE (2026-07-23), uncommitted**

Two user-reported artifacts, one root cause: **`windFoliage[slot] * b` multiplies a vertex
displacement, so it has to be continuous in SPACE, and both factors were discontinuous.**

- `windFoliage` is per-slot and authored, so it steps wherever two slots meet on coincident vertices.
  On coconut_palm slots 2 and 3 share 36 welded vertices (one per frond) — a **0.978 m tear** at
  swayAmp 4, i.e. every frond ripped off its base. ("отрывает листья при сильном ветре")
- `b` was renormalised per CONNECTED COMPONENT, and a component is a modelling accident rather than an
  anatomical limb. A coconut frond is a petiole (slot 3, 42 sticks) plus a blade (slot 2, 36 cards),
  and only *some* pairs are welded; on the rest `b` ran 0..1 along the petiole and **restarted at 0**
  on the blade, so the two sides of that junction differed by the whole streaming term — a leaf
  snapped in half a third of the way out. ("лист изломился")

The first fix attempt unified `windFoliage` across welded slot groups at runtime
(`Mesh::WeldedSlotGroups`). It removed the tear but silently overrode the artist's authoring, and
promoting the petioles to full foliage turned the hidden 0 m step at the petiole/blade junction into
**2.202 m**. That change is **reverted** — the accessor and its call site are gone.

**Shipped instead:** `b` = geodesic distance from the **wood surface** (the slots whose windFoliage is
0), normalised globally. `BakeWindWeightsCpu` takes the submesh table plus `MeshLoadOptions::
slotFoliage`; the bridges built for the island pass are added to the adjacency so the wood pass can
walk crown -> petiole -> blade. Being a distance FIELD is the whole point: it is continuous by
construction, cannot restart mid-leaf however the mesh was cut, and is exactly 0 where a leaf meets
wood, which pins `foliage * b` to 0 on *both* sides of a wood/leaf seam. No per-slot weight the artist
picks can tear or kink the mesh any more. A welded vertex shared by a wood and a foliage submesh
counts as wood. Absent classification -> falls back to the old per-component ramp.

Measured at swayAmp 4 (coconut; date_palm 0.238 -> 0.079 with 26 bad junctions -> 0, curly 0.503 ->
0.059 with 7 -> 0):

| | tear | worst junction elbow | elbows > 10 cm |
|---|---|---|---|
| old `b` + authored foliage | **0.978 m** | 0.033 m | 0/76 |
| old `b` + unified foliage (the first attempt) | 0.000 m | **2.202 m** | **41/76** |
| **new `b` + authored foliage** | **0.000 m** | **0.033 m** | **0/76** |

**Plumbing:** CLI `--reimport-foliage=0,0,1,0,0`; ImportPanel passes mesh.json's `windFoliage` into the
bake; `HashOptions` folds in only the zero/non-zero **pattern** (hashing the floats would invalidate
every `.bin` whenever a slider moves); `MeshManager::BinaryNeedsRebake` (32-byte header read) plus a
hook in `MeshEditorPanel::Save` re-bakes when that pattern changed — without it the next slider change
silently reintroduces the elbows. Weight *values* stay pure runtime.

**Known gap:** with no authored `windFoliage` the runtime defaults foliage to the slot's alphaMask
flag, but the bake has no material info and takes the fallback ramp. Author `windFoliage` explicitly
on new foliage assets.

**Measurement notes (two false trails burned first):** "distance from the trunk axis" is meaningless
on a two-trunk palm, and channel **G is an 8-bit hash** of the component id so it collides — recompute
real components in any analysis script. "Any two vertices within 2 cm" is not a junction either: it
flagged a frond passing near a **coconut** (slot 4 = 32 fruit) as a 1.26 m break. What actually works:
TEAR = displacement spread within exactly-coincident vertices; ELBOW = displacement difference at each
component's own closest approach to another component, which is exactly where the bake bridges it.
Both A/B off a single baked `.bin` (the old per-component `b` is recomputable from `r`), so comparing
the three states needs no rebuild.

**Verified:** Debug + Release clean; `--scene-stress=30` CLEAN; all three palms re-baked and measured.

### W7.5 — leaves stopped being stretched; bake made 330x faster — **DONE (2026-07-23), uncommitted**

Three separate defects, reported together ("ебически долго в дебаге", "листья как ломало так и ломает",
"у мелкой и у date чешуйки катаются относительно ствола"). Each was measured before it was touched.

**1. The bake took 132 s in a Debug build** (7.6 s Release). The island-attachment loop rescanned every
unreached vertex against a grid holding all (mostly unreached) vertices, once per attachment. A first
attempt at caching each island's best candidate made it *worse* (248 s): with a canopy full of islands
the invalidation predicate fires nearly every iteration, and the grid grows as islands attach.

The fix was to remove the need for the loop rather than speed it up: a **proximity pass** up front
wires each vertex to the nearest vertex in each of the **K nearest DISTINCT components**. Almost every
island is then reached by the plain Dijkstra and the loop has nothing left to do. **0.4 s in Debug.**
Distinct *components*, not simply K nearest vertices: a bark scale's two nearest vertices both lie on
the same neighbouring scale, so plain K-nearest just rebuilt the sibling chain it was meant to break.
K-nearest also replaced a fixed radius, which has to be guessed per asset and gets it wrong both ways
(0.5 % of the diagonal is 1.6 cm on curly_palm, whose trunk skin sits 4 cm off its core).

**2. Bark scales slid across the trunk.** date_palm carries 96 separate scales, curly_palm 100. Islands
were attached by the *cheapest gap*, so scales attached to each other and the chain accumulated
geodesic distance instead of taking it from the trunk 2 mm underneath: r drifted 28/255 (date) and
43/255 (curly) between a scale and the trunk vertex it rests on. Attachment cost is now
`dist[target] + gap` — the distance the island would INHERIT, i.e. plain Dijkstra with the bridge as an
edge, which cannot chain. Relative slide at swayAmp 0.98: **date 0.069 -> 0.008 m, curly 0.172 ->
0.050 m** (the proximity pass does most of this; the cost change stops it coming back).

**3. Leaves were being stretched, not bent.** The streaming displacement was `amp * bend * foliage *
0.9 * b` with **b a NORMALISED position along the limb**, so its gradient along a leaf was
`amp / leafLength` — completely decoupled from how long the leaf actually is. Measured as edge STRAIN
(`|offset_i - offset_j| / restLength`, 1.0 meaning an edge's endpoints separated by the edge's own
length), at the atoll's own swayAmp of 0.98: **coconut 1.02, date 0.99, curly 3.06**, with 26-57 % of
all edges past 0.3. It scales linearly with wind, so it never went away — it just got less obvious in
a light breeze. Note this was invisible to the earlier junction/tear metrics: it is not a seam coming
apart, it is the leaf's own interior being pulled apart.

Fixed by bounding the streaming to the leaf's own length. `COLOR_0.a` now carries `1 + leafScale/diag`
(it was a constant 255 marker; 0 still means "not baked"), `Mesh::GetWindLeafScale()` recovers the
metres, and the per-object `windLeafScale` — the slot freed in W7.3, so **the 224-byte stride and every
offset are unchanged** — carries it times the object's world scale through all four mirrors. The shader
then blends `want * ks / (ks + want)` with `ks = kWindLeafShear * b * windLeafScale`: equal to `want`
while small, saturating at `kWindLeafShear * s`, and with a derivative along the leaf that never
exceeds `kWindLeafShear`. A plain `min()` would bound it too but creases where the branches meet.
Recovered leaf scales are physically right: coconut 2.00 m, date 1.95 m, curly 0.70 m.

| swayAmp | | coconut | date | curly |
|---|---|---|---|---|
| 0.98 | unbounded | 1.02 | 0.99 | 3.06 |
| 0.98 | **bounded** | **0.72** | **0.66** | **2.13** |
| 4.00 | unbounded | 3.10 | 3.21 | 4.33 |
| 4.00 | **bounded** | **1.85** | **1.79** | **2.32** |

**KNOWN RESIDUAL, measured not guessed:** what is left is now dominated by the **main bend**, not the
streaming — at swayAmp 0.98 the bend alone accounts for 0.56 of coconut's 0.72, 0.36 of date's 0.66 and
1.88 of curly's 2.13. Same root cause one level up: `WindBendProfile(r)` uses r normalised by the
GLOBAL geodesic max, so its gradient in metres is unbounded too, and it bites hardest on a small plant
(curly is 2 m, so r sweeps its whole range over a short distance) and on canopies (r rises fastest
there). The clean fix is to freeze r at the leaf's ATTACHMENT for foliage vertices — the wood-distance
pass already knows which wood seed each vertex came from, so it costs no new channel — leaving the
leaf's own motion entirely to the bounded streaming. It is deliberately NOT done here: it cuts frond-tip
main bend from f(1.00)=1.00 to f(0.62)=0.36, a visible change in how the canopy reads, and the palms'
amplitudes are hand-tuned.

**Verified:** Debug + Release clean; all 9 wind VS variants plus the GI scatter CS compile standalone
under dxc (incl. `SHADOW_MASKED=1` / `INSTCB_SLOT_PARAMS=1`); `--scene-stress=30` CLEAN; Debug bake
0.44-0.48 s for both palms; all three palms re-baked.

--- (original spec) ---

### W7.3 — Switch the shader over and DELETE the heuristics (spec)

`wind.hlsli` reads the baked weight instead of computing one. **Remove** `kWindCrownHeightFrac`,
`kWindFrondSpanFrac` and the whole radial/crown-distance mask — the point is to retire the
heuristics, not to stack another layer on them. The per-slot `windFoliage` stays as the artistic
multiplier on top of the baked weight (existing hand-tuned values keep working). Bake always, so
there is no "unbaked" branch to maintain.

**Verify:** coconut palm canopy shows a proper base-to-tip gradient (the reported bug); date palm
unchanged or better; shadow still tracks (overhead-sun canopy-vs-shadow centroid test); motion
vectors clean at high `swayFrequency`; `--scene-stress=30`.

## W8 — Polish — **DONE except the gust->ocean item (2026-07-23), uncommitted**

Four of five shipped; the fifth is deliberately NOT built and the evidence is below. Built in the
plan's own order: the freeze toggle first, because it is the instrument the rest were verified with.

### W8.1 — `vfx::g_windFreeze` debug time freeze — DONE

Freezes the CLOCK, not the parameters, so the pose stays the real authored one. `--wind-freeze[=<s>]`
for headless shots; a Freeze / Step / step-size control in the Wind inspector (debug state, never
written into the level document or the undo stack).

**It also pins the OCEAN clock**, `OceanRenderable::Tick`. That is not scope creep, it is the same
clock: the wind derives its time from `elapsedTime_` precisely so waves and sway stay coherent, and
freezing one without the other both desyncs them and leaves the water animating under a frozen frame.
Measured, on `wind_test` — run-to-run noise vs the wind signal, as a fraction of changed pixels:

| | noise (same frozen time, two runs) | signal (freeze 0 vs 1.5) |
|---|---|---|
| wind clock only | 15.5 % | 18.2 % — **unusable** |
| + ocean clock pinned | **0.05 %** | **44.6 %** (~890x separation) |

The residual 0.05 % is DLSS jitter phase, which depends on the frame index at capture. On the
ocean-free `wind_shadow_test` the separation is 0.43 % vs 10.2 %.

Doing the job it was built for: freeze 0 -> 1.5 s on `wind_shadow_test` changes 296 319 px of canopy
AND 71 037 px of ground/shadow. A frozen or over-cached shadow shows ~0 in the second number while the
first still moves — the diff that `swayFrequency: 0` structurally cannot produce.

### W8.2 — Grove variation — DONE

One `WindHash2(worldOrigin.xz)` drives amplitude (+-25 %), trunk stiffness (+-20 %) and a phase
offset. All three come from the world origin — the anchor the phase already used — so they are free,
need no authoring, and cannot break submesh/shadow lockstep. Both jitters are symmetric, so a grove
still averages to the authored value.

The phase offset is the part that matters most: the old phase was `dot(origin.xz, k)`, LINEAR in
position, so evenly spaced trees got evenly spaced phases and a row swayed as one travelling wave.
Measured over the 12 palms of `atoll_a2_test` (ratio of largest to smallest phase gap, 1.0 = perfectly
even): **130.4 -> 17.7**. Amplitude now spans x0.750..x1.222.

`WindState::MaxSwayExtentMeters` mirrors `kWindGroveAmp` — the shadow caster bounds must cover the
LOUDEST tree in the stand, not the average, or the one hashed to +25 % clips at page edges.

### W8.3 — Wind-driven particle drift — DONE

`EmitterDesc::windInfluence` (m/s^2 at wind strength 1), folded on the CPU against the scene's
gust-modulated `vfx::g_windDriftXZ` into a finished acceleration, so `particle_update_cs` integrates
one vector and knows nothing about wind. Applied before drag, so drag still bounds terminal speed and
a gust visibly leans the plume further. `GpuEmitterParams` 80 -> 96 B (both mirrors + static_assert);
the emitter's swept culling AABB includes the drift, or a windblown plume drifts outside its own
bounds and pops.

Per emitter because the engine has no particle mass — smoke rides the wind, sparks do not — and
default 0, so enabling wind on a level cannot silently disturb its existing effects. Set to 3.0 on
`data/particles/smoke.json` so the feature is live rather than dormant.

**Verification note worth keeping:** the first measurement showed signal == noise and I nearly shipped
a no-op. The test level was wrong, not the code: `ResolveEmitterDesc` reads preset -> `overrides` ->
inline fields and never looks at `properties`. Re-tested through `overrides`, the plume visibly leans
while the fire at its base (no windInfluence) stays vertical — which is the per-emitter design working.
Particle RNG is per-frame seeded, so a pixel diff ALWAYS needs a same-settings control run; without one
the 0.9 % diff looks like a result.

### W8.4 — Distance fade — DONE, default OFF

`vfx::WindDistanceFade(objectOriginWS)` is a SINGLE definition scaling windStrength on both the
gbuffer and the shadow, from the CAMERA position (published by `Scene::Tick`). Never per-vertex (tears
the mesh) and never per-view (fading by distance from the LIGHT would detach the shadow).

The trap: `ShadowGpuData::UpdateForFrame` deliberately skips anything that has not moved, and a fading
caster changes without ever moving — so the tree would fade while its shadow kept swaying. Wind
casters are therefore re-checked per frame, quantised to 1/255 so a panning camera does not re-upload
every caster every frame. Guarded by one bool: with the fade off this is free and the O(movers)
property is untouched.

Default OFF because picking a distance blind is a worse regression than the hazard: the atoll vista
legitimately shows palms hundreds of metres out, and freezing those is far more visible than aliasing
nobody has reported. Sliders live in the Wind inspector.

### W8.5 — Gust -> ocean coupling — **NOT BUILT, and should not be as specified**

Every route from wind force to the water goes through `SetSceneVariables` ->
`ResetInitialSpectrum` -> `ResetGpuResources`, which retires every ocean GPU resource, releases the CPU
data and resets the descriptor heap; the editor path calls `WaitForPreviousFrame()` before it for
exactly that reason. `SetDisplayWindForce` is an input to the same spectrum, not a shortcut. And the
gust envelope changes EVERY FRAME by construction, so the coupling as specified is a full GPU idle
plus a resource rebuild per frame. That is categorically wrong, not a tuning question.

There is also no cheap per-frame lever to redirect it to: `ocean_surface.hlsl` declares
`viewerParams.z` (amplitude) but never uses it, and wave height comes from the FFT displacement
textures.

The correct implementation is an OCEAN-side feature: a per-frame displacement multiplier applied in
`SampleDisplacementTexture`, with the previous frame's gust for `SamplePreviousDisplacement` (same
prev-value discipline the foliage already uses for motion vectors) and the surface derivatives scaled
to match, or normals desync from the geometry. That is a change to a system under active refactor, so
it is proposed rather than smuggled in.

Worth noting the physics agrees: real gusts do not raise swell on a timescale of seconds — a sea has
enormous inertia, which is why windForce is a slow variable. Gusts show up as cat's paws, local
roughness patches. Coupling the envelope to the spectrum would look wrong even if it were cheap.

**Verified:** Debug + Release clean; all 9 wind VS variants + the GI scatter CS + the particle update
CS compile standalone under dxc; `--scene-stress=30` CLEAN; atoll renders with shadows tracking.

--- (original list) ---

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
