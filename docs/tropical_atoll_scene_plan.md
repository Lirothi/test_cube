# Tropical Atoll Scene — Implementation Plan

Goal: a new level `data/levels/atoll.json` — a tropical atoll (sandy island ring + turquoise
lagoon) with realistic palm trees and a rock cave containing a burning campfire (flames, smoke,
sparks, flickering light). Art style: **realistic PBR**. The scene doubles as the dogfooding
milestone for the level editor.

Decisions locked with the user:
- Realistic PBR assets (Poly Haven / Sketchfab / ambientCG), not stylized low-poly.
- **Full particle system, GPU-simulated** (compute spawn/update, instanced billboard draw) for
  the fire (flames + smoke + sparks) — not an emissive-only fake and not a CPU sim. Rationale:
  the engine is CPU-submission-bound and the codebase direction is GPU-driven; GPU headroom
  exists after the VSM work.
- **glTF import added to the engine** (cgltf), not manual Blender→OBJ conversion per asset.
- **True multi-material submeshes in the renderer** (user choice over editor-side grouping or
  a composite-of-N-objects workaround): one placed object = one glTF asset with per-slot
  materials. Note this is identity/UX work, not perf — the queue already auto-instances by
  collapsing contiguous (mesh, material) runs, so N objects and N submeshes cost the same draws.
- **Skybox stays a DDS cubemap** — the HDRI is converted offline (no runtime .hdr reader).
  Verified: `sources/materials/TextureCube.cpp` passes the DX10-header `dxgiFormat` straight
  through (line ~291) and uploads via `GetCopyableFootprints` (format-agnostic), so a
  BC6H_UF16 (or RGBA16F) cubemap with mips loads with **zero engine changes**. Hard
  requirement: the DDS must carry a DX10 header (legacy-header HDR formats are rejected).
- **In-editor asset importer owns all conversions** (user request): a content-browser Import
  flow (Part H) turns staged raw downloads into engine-ready content (PNG→mipped BC DDS,
  .hdr→BC6H cubemap, license→CREDITS.md). Neither the user nor the executor hand-runs
  converters once it lands; the same backend is exposed as a CLI for headless/batch use.
- **Simple in-editor material editor** (user request): parameter/texture-picker editing of
  materials with live scene preview. Explicitly NOT a node graph and NOT custom shader
  AUTHORING (a material may still SELECT a feature shader — I0 `"shader"` key). Since the
  2026-07-16 Part-I rewrite, materials are per-file assets `data/materials/<name>.json` (I0,
  done); the old monolithic data/materials.json is gone.

## Current engine state (verified 2026-07-13)

Ready to use:
- **Ocean**: full FFT clipmap ocean (`sources/ocean/`, presets `data/ocean/*.json`) with shore
  depth/foam, SSS, height fog, its own reflection path. Level JSON: `"ocean": { "enabled": true,
  "preset": "data/ocean/default.json" }` (+ `windForce`, `windDirectionDeg` overrides).
  This is the lagoon — it renders as an infinite camera-following clipmap; the island just rises
  above sea level.
- **Skybox**: DDS cubemap per level (`"skybox": {"texture": "textures/skybox.dds"}`,
  `sources/rendering/lighting/Skybox.cpp`). Also feeds ambient/specular fallback.
- **Lights**: directional (CSM+VSM), spot + point with shadows, authored in level JSON and
  spawnable in the editor (`CreateEnvironmentCommand`). Campfire = shadowed point light.
- **Materials**: albedo(sRGB) + `mr`(R=metal,G=rough) + normal, DDS (mipped, BC) or PNG (WIC,
  **no mips**). Presets in `data/materials.json`, per-object overrides in level JSON
  (`ApplyStaticMeshJsonProperties`, `sources/app/scene/SceneObjectFactory.cpp`).
- **Meshes**: OBJ + custom `.txt` only (`sources/rendering/meshes/MeshManager.cpp`), fixed
  `VertexPNTUV`, auto-LOD via meshoptimizer, one material per placed object, `.mtl` ignored.
- **Editor**: AssetRegistry scans `models/` (`.obj/.mesh.txt/.txt`), `textures/` (`.dds/.png`);
  SpawnMeshCommand + CompositeCommand exist.

Gaps this plan closes:
1. No glTF/GLB import (Part A).
2. One material per mesh everywhere — replaced by true multi-material submeshes (Part B).
3. No alpha-test (masked) or two-sided rendering — required for palm fronds (Part C).
4. Emissive G-buffer target exists (`RT2` in `shaders/gbuffer_common.hlsl`, composed in
   `shaders/compose_cs.hlsl`) but stock `shaders/gbuffer.hlsl` writes 0 (Part D).
5. No particle/billboard system at all (Part E).
6. No light flicker animation (Part F).
7. No asset conversion pipeline (raw PNG/HDR → mipped BC DDS) and no import UX (Part H).
8. Material presets are hand-edited JSON only — no in-editor authoring/tweaking (Part I).

**State refresh (2026-07-16):** gaps 1–5 and 7 are CLOSED (Parts A–E and H fully landed; see
per-step DONE markers). Gap 8 is in progress — I0 (per-file materials + schema v2 + material
shader override) is done; the editor window itself (I2) is pending. Materials now live in
`data/materials/<name>.json`; Part J (mesh asset format, `models/<name>.mesh.json`) is scheduled
next so level objects stop repeating inputLayout/shader/material per placement. Part F and
E4-tuning statuses: F not started; G0 done, G1 mostly done (skybox imported via the H pipeline).

Out of scope (explicitly): audio (no backend exists), global atmospheric fog (ocean height fog
only), bloom, DLSS-RR, RT handling of masked geometry (fronds will reflect as opaque in RT
reflections — acceptable; optionally exclude fronds from BLAS).

## Conventions for the executor

- One part = one or more commits; every step ends with a **Release x64 build + run** check.
  Build from PowerShell (never bash):
  `msbuild test_cube.sln /p:Configuration=Release /p:Platform=x64 /m /v:m`
  (match existing configuration names in the sln if they differ).
- **Executor model per step** — every step below carries an `exec:` tag:
  - **Fable** — cross-cutting renderer refactors, GPU lifetime/sync hazards, perf-regression-
    prone queue/shadow/RT work, gnarly debugging.
  - **Opus 4.8** — well-specified engineering with a contained blast radius: shader variants,
    tool backends, editor windows. The step text in this doc is the spec.
  - **GPT 5.6 terra** — self-contained scripts/data/config with build- or eyeball-verifiable
    output; no renderer internals.
  Escalation rule: two failed attempts or any unexplained regression → bump one tier.
  Alternative escalation for GPT-tier steps that balloon into long-horizon/terminal-heavy work:
  **GPT-5.6 Sol** instead of Opus (2.6× fewer output tokens than Terra → cheaper per solved
  long task; strong terminal agentics). Sol is NOT a substitute for Opus on engine
  implementation (no published SWE-bench numbers, pricier output than Opus) and NOT for code
  review (low comment precision). Verification gates are identical for every tier. Lower-tier
  diffs touching `sources/rendering` / `sources/app/scene` get a Fable review before commit.
- **No `dynamic_cast`** — engine forbids it. Use the internal-RTTI virtual accessor pattern
  (`AsRenderableObject()`-style, add `AsParticleEmitter()` etc.).
- New GPU buffers that grow or die with scene objects: remember the LightManager use-after-free
  lesson — never free/reallocate a buffer the in-flight frames still reference. Ring-buffer
  per-frame uploads (pattern already used for light buffers) and pre-size on load.
- After Parts B, E and G, run the level-switch stress harness: `--scene-stress=30` cycling
  `atoll` ↔ `demo` — new object types (particle emitters) must survive churn with 0 device hangs.
- Headless run/verify recipe (no user present): launch exe, `FindWindow` by class+title to
  confirm alive, capture DBWIN/OutputDebugString for log verdict, screenshot for visual checks.
  Trust the log verdict, not the process exit code (known DLSS teardown flake at exit).
- **Asset gate — stop early when inputs are missing.** Before starting any step that consumes
  user-supplied assets (HDRI, sand/rock textures, palms, boulders, campfire, fire/smoke
  flipbooks), check `import_staging/` for the required files. If they are missing — or unusable
  (wrong format, no license note, absurd poly count) — **end the step early and ask the user to
  download them**. Do NOT silently substitute placeholders and do NOT go hunting for assets
  yourself (license/quality choices are the user's). The request must be self-sufficient:
  restate the relevant instruction from the asset guide below (what to get, selection criteria,
  target folder `import_staging/<name>/` + `source.txt` with the license link) and include a
  direct link, e.g.:
  - HDRI: https://polyhaven.com/hdris (search `beach` / `sunny`, 4K `.hdr`)
  - Sand/rock textures: https://ambientcg.com/list?q=sand , https://polyhaven.com/textures
  - Palms/campfire: https://sketchfab.com/search?features=downloadable&type=models&q=coconut+palm
    (remind: set license filter to CC0/CC Attribution, download as glTF)
  - Boulders: https://polyhaven.com/models (search `rock`)
  - Flipbooks: https://opengameart.org/art-search-advanced?keys=fire+flipbook ,
    https://kenney.nl/assets/particle-pack
  While blocked on an asset, continue with whatever steps don't need it (engine parts A–F are
  never asset-blocked; G0 island is procedural). Staged as of 2026-07-13: 3 palms (evolveduk,
  CC-BY), 2 boulders (Pixel Life, CC-BY), rocks pack (Studio Lab — **NOT CC**, Sketchfab
  standard license, prefer the Pixel Life boulders), coast_sand_01 + marble_cliff_05 texture
  sets (Poly Haven, CC0), rustig_koppie_puresky_4k.hdr sky (CC0), campfire (filthycent, CC-BY,
  1.4k tris, MR B-channel=0 so the default metallicFactor=1 is harmless), Kenney Particle Pack
  (CC0 — covers fire/smoke/sparks sprites). **All asset gates are clear as of 2026-07-13**;
  optional later add: a Mantaflow-baked flame flipbook (self-made, see E4).

---

## Part A — glTF import

**STATUS: ALL DONE + committed** (A1 cgltf vendored, A2 loader + `#N`/`#node:` selectors,
A3 material import with factor-multiply fix, A4 editor spawn; A5 dropped — owned by Part H).

Rationale: nearly all free realistic assets ship as glTF/GLB. One-time engine work kills the
per-asset Blender routine.

**A1 — vendor cgltf.** *(exec: GPT 5.6 terra — header drop + vcxproj, build verifies)* Add `third_party/cgltf/cgltf.h` (single header, MIT). Wire include path
into `test_cube.vcxproj` (`third_party` is already on the include path — just add the file to the
project + filters). No behavior change; build-verify.

**A2 — geometry import.** *(exec: Opus 4.8; escalate to Fable if handedness/winding debugging
drags)* In `MeshManager::Load`, dispatch `.gltf`/`.glb` → `ParseGltfFile`:
- Walk the node hierarchy, bake node world transforms into vertices.
- For each mesh primitive: positions, normals, uv0, tangents (if absent → reuse the existing
  tangent generation used for OBJ); indices u16/u32 → engine format; build `VertexPNTUV`.
- **Primitive handling**: parse all primitives, grouped/merged by material. Plain
  `models/palm.glb` = the whole asset as ONE mesh carrying a submesh table (one index range per
  material group) — consumed by Part B; until B lands, plain-path load falls back to group 0
  with a warning. Keep `models/palm.glb#N` (one material group as a standalone mesh) as a
  low-level/debug path — it is also how Part A is verified before Part B exists.
- **Node-subtree filter**: also support `models/rocks.glb#node:Rock_1` — parse only that
  top-level node's subtree (then merge by material within it). Rationale: prop PACKS (e.g. a
  two-rocks Sketchfab asset) ship several independent objects in one file; merging by material
  across top-level nodes would weld them together and kill independent placement. Merging must
  never cross top-level node boundaries unless the whole-file path was explicitly requested.
- Reuse `GenerateLods` (meshopt) exactly as for OBJ.
- **Axis/winding gotcha**: glTF is right-handed Y-up; verify against engine handedness with a
  test asset that has readable chirality (text or an asymmetric prop). Expect a Z-flip +
  triangle-winding reverse; confirm visually before proceeding.
- Embedded GLB textures: decode via the existing WIC path from memory blob (WIC supports
  `IWICImagingFactory::CreateDecoderFromStream`); external URIs resolve relative to the file.

**A3 — material import.** *(exec: Opus 4.8)* From each primitive's material, extract into `MaterialParams`:
- `baseColorTexture`/`baseColorFactor` → albedo/tint.
- `metallicRoughnessTexture`: **glTF packs G=roughness, B=metallic; engine expects R=metal,
  G=rough.** Add a `mrLayout` flag (engine|gltf) to `MaterialParams` + a swizzle in
  `gbuffer_common.hlsl` sampling — do NOT re-encode pixels at load.
- **Factors MULTIPLY texture channels — never skip them** (glTF spec: metallic =
  tex.B × `metallicFactor`, roughness = tex.G × `roughnessFactor`, albedo =
  tex × `baseColorFactor`; absent factor = 1.0). Real-world proof already in
  `import_staging/coconut_palm/`: every MR texture has B=255 (and R=255) as filler with the
  actual roughness in G, and every material sets `metallicFactor: 0.0` — ignore the factor and
  the whole palm renders as polished metal. Use this asset as the A3 test case; it also
  exercises everything else: 5 materials/5 primitives (single object with 5 material slots once
  Part B lands), fronds material
  with `alphaMode: MASK` + `alphaCutoff: 0.258` + `doubleSided: true` (feeds Part C), Z-up
  root rotation + 0.01 FBX scale in node matrices (A2 transform bake), trunk UVs wrapping past
  1.0 (V up to 10 — needs REPEAT sampler), CC-BY license → CREDITS.md (author evolveduk).
- `normalTexture` (+ scale → `normalStrength`).
- `alphaMode`/`alphaCutoff` + `doubleSided` → recorded now, consumed by Part B.
- `emissiveFactor`/`emissiveTexture` → recorded now, consumed by Part C.
- Runtime-only auto-materials (no writes to `data/materials.json`); the editor may later save a
  preset explicitly.

**A4 — registry + verification.** *(exec: GPT 5.6 terra)* AssetRegistry: add `.gltf`/`.glb` to the models root
extension list so assets show in the content browser. Editor spawn UX intentionally waits for
Part B (one object with material slots — B4); do NOT build a composite-of-N-objects spawn
workaround. Verify Part A by hand-authoring `staticMesh` entries with
`models/coconut_palm/scene.gltf#N` in a test level: trunk vs fronds materials correct, factors
applied (palm must NOT look metallic), node transforms baked (upright, ~5–6 m tall).

**A5 — dropped.** Mip generation / DDS conversion is owned by the importer (Part H, see
H1/H2). Until H lands, WIC-loaded PNGs render unmipped — acceptable for A/B/C bring-up, not
for the final scene.

## Part B — multi-material submeshes (renderer + editor)

One placed object = one multi-material asset. Identity/UX work, not perf (draw calls end up
identical — see Decisions). This is the **widest-touch part of the plan**: stage the commits so
each one builds, runs the demo level unchanged, and keeps auto-instancing intact.

**B1 — submesh plumbing (behavioral no-op).** *(exec: Opus 4.8; DONE 2026-07-14)* `Mesh` gains a submesh table
`{indexOffset, indexCount, materialSlot}`; OBJ/`.txt` loaders emit exactly one submesh, so
nothing changes visually. **LOD gotcha**: `GenerateLods` simplifies the whole index buffer
today — it must simplify each submesh range independently and rebuild the table per LOD level,
or ranges go stale. Commit as a no-op; gate on demo level identical + instancing batch counts
identical.

**B2 — multi-material draw path.** *(exec: Fable; DONE 2026-07-14 — implemented at the OBJECT
level, not as queue draw items: reading the code showed SortOpaque already orders by BatchKey
(mesh, material, matData) and batching gates on AsInstanceable(), so restructuring buckets was
needless churn.)* As landed: `GBufferRenderable` grew per-slot arrays (matDatas_/matParamses_/
slotPresets_, slot 0 ≡ legacy single material — factory/editor accessors unchanged); a
multi-slot object overrides `Render()` and records per submesh (own b0 slice via AllocDynamic +
its slot's SRV table + `Mesh::DrawSubmesh` ranged draw); the uniform binder pulls
`CurrentDrawParams()`. glTF plain-path/#node loads now return the FULL multi-submesh mesh (all
material groups concatenated, per-group submesh table); `#N` still loads one group. Level JSON:
`"materials": [...]` per-slot array ("auto" = from glTF), scalar `"material"` = slot-0
back-compat. Shadows draw the whole buffer in one draw (fine, depth-only).
Culling stays per-object. Known limits: one PSO per object = slot-0 defines win (mixing engine
presets with glTF slots renders the glTF slots wrong — warned in debug output; use homogeneous
slots); RT instance uses slot-0 albedo.

**B2b — instanced multi-submesh batches.** *(exec: Fable; DONE 2026-07-14)* Multi-slot objects
now auto-instance: `gbuffer_instcb.hlsl` gained an `INSTCB_SLOT_PARAMS` variant (per-slot
material params in a b2 CB, mirrored by `render::InstanceSlotParams`; world/prevWorld/objectId
stay per-instance in b0), `BuildInstancedMaterials` builds it for multi-slot objects (instanced
CSM shadow PSO stays shared/define-free), and `InstancedDrawBatch` loops the LOD's submeshes —
one `DrawSubmeshInstanced` per range with that slot's SRVs + b2 slice; the instance array
uploads once per chunk. Batch identity: `IInstanceable::SameInstanceSlots` (all slots'
MaterialData pointers + params equal) guards the queue's run extension, since slot textures and
params bind once from the run's lead — BatchKey alone only carries slot 0. N palms = ~5
instanced draws total (camera path; shadows were already 1 draw). Verified: 10-palm grove in
`atoll_a2_test.json` logs `[instancing] multi-slot batch active: 10 instances x 5 submeshes`
(one-time debug line), renders per-slot correctly (user-confirmed); mixed-slot palm correctly
excluded (differing slot-0 matData ≠ BatchKey); demo level clean (no batch line, no errors,
user-confirmed visuals).

**B3 — downstream consumers.** *(exec: Fable; DONE 2026-07-14 — GI→VSM plan complete, so the
"keep ShadowGpuData single-submesh" caveat was lifted and the GPU-driven path went per-submesh
too.)* As landed:
- **ShadowGpuData** (the main path — Rung 0 indirect + VSM caster data): one caster SLOT per
  (object, submesh) and one mesh-group per (mesh, submesh) — a mesh's groups are contiguous
  (`group = meshToGroup[mesh] + ordinal`), all its slots share the object's instance/bounds
  (per-submesh AABBs = future cull refinement). `PerGroup` widened to uint4 `{visibleBase,
  indexCount, startIndex, 0}`; the cull-clear CS seeds the args' StartIndexLocation, so both
  ExecuteIndirect consumers (per-view CSM + VSM per-page draws) draw submesh ranges with ZERO
  changes to the VSM setup shader (its mega rebase ADDS the mesh's mega offsets on top). Mega
  buffer lays out per UNIQUE mesh (one VB/IB copy shared by its submesh groups — also dedupes
  a mesh used by several groups, which previously copied twice). UpdateForFrame counts/updates
  slots (movers duplicate across their slots). One-time warning when groups exceed the VSM
  setup cap (64). Verified: demo layout byte-identical (282 casters/6 groups), atoll goes
  18 objs/7 groups → 65 slots/14 groups with `cull validation PASS` (GPU matches CPU reference).
- **CPU fallback shadow paths**: `RenderableObject::RenderShadow` + `InstancedDrawBatch`
  (shadow side) loop `DrawSubmesh(Instanced)` for multi-submesh meshes — depth-only equivalent
  (ranges tile the buffer); this loop is where C2 binds per-slot masked materials.
- **RT**: BLAS = one geometry desc per submesh (LOD0 table); bindless geometry-info records are
  per (instance, submesh), contiguous, sharing one VB/IB/albedo/MR descriptor set, with a new
  `firstTri` field; hit shaders index `geom[InstanceID + GeometryIndex()]` and fetch triangles
  at `firstTri + PrimitiveIndex()`. Every record still carries the object's slot-0 material —
  per-slot RT materials (correct per-slot albedo in reflections) are now a small follow-up.
- Picking/selection stays per-object as planned.
- **Perf follow-up (same day):** the submesh split multiplied the VSM per-page cull's plane
  tests (2 loops × pages × casters; atoll 18→65 slots → a couple ms in Debug while moving).
  Fixed by a per-OBJECT dedupe: `casterDynamic_` became `casterMeta_` (bit0 = dynamic, bits 1+
  = the object's slot count on its FIRST slot), and `vsm_page_setup_cs` tests bounds once per
  object, applying the result to its consecutive slots — test count back to pre-B3. NOTE: this
  deliberately keeps slots sharing the OBJECT's bounds; switching to tight per-submesh AABBs
  (finer page culling) would trade the dedupe away — measure before doing it.

**B4 — editor UX + spawn.** *(exec: Opus 4.8; DONE 2026-07-14 — slot list + SetMaterialSlotCommand + re-fetch-runtime-after-respawn fix)* `SpawnMeshCommand` spawns ONE object per glTF asset with slots
auto-filled from A3 materials; `InspectorPanel` shows a material-slot list (per-slot preset
picker, per-slot undo via a `SetMaterialSlotCommand`); outliner shows one node per asset.
Verify with the coconut palm: spawn → one object with 5 slots; move/duplicate/undo/save/reload
round-trips; `--scene-stress` clean.

## Part C — masked + two-sided foliage

**C1 — masked G-buffer variant.** *(exec: Opus 4.8; DONE 2026-07-14)* As landed: `ALPHA_TEST`
define permutation + `AlphaTestClip` helper in `gbuffer_common.hlsl` (early clip of
`baseColor.a * albedo.a - cutoff`), per-slot `alphaCutoff` threaded through every CB via
existing padding (b0 `PerObject`, `InstancePerObject`, `SlotParams` b2) with **sentinel −1 =
slot never clips**. Because the object had ONE PSO, C1 shipped with a **union-of-flags interim**:
ALPHA_TEST/cull-NONE applied object-wide if ANY slot is masked/two-sided; correctness held via
the −1 sentinel, but slots 1+ still render with slot-0's sampling defines (mixed preset+glTF
objects shade fronds with the wrong MR/normal swizzle). glTF import (A3) fills
`alphaMask`/`alphaCutoff`/`doubleSided`. Verified: fronds are clean cutouts, two-sided, trunks
intact. Alpha shimmer under DLSS jitter accepted (hashed alpha = future item).

**Shimmer follow-up (Fable, DONE 2026-07-15):** the dominant shimmer cause was UNMIPPED WIC
PNGs (minification aliasing that jitter animates), not the alpha test itself. Landed: (1)
Texture2D builds a CPU box-filter mip chain for every WIC load (sRGB-aware averaging for
albedo; DDS keeps its file mips) with **alpha-test coverage preservation** per mip (Castano
percentile rescale, boost-only clamped [1,8] — plain box filtering erodes masked foliage with
distance); `CreateDesc.alphaCoverageCutoff`, wired from glTF alphaMask/alphaCutoff in
MaterialDataManager (set BEFORE LoadAlbedo). (2) NVIDIA-recommended DLSS texture mip bias —
`Renderer::GetDlssMipBias()` = clamp(log2(renderW/displayW), −2, 0) quantized to 0.25 steps —
on the gbuffer material samplers (MaterialData::StageGBufferBindings + GpuInstancedModels), so
the new mips don't go soft under upscaling. fwidth-sharpened alpha was considered and REJECTED
(mathematical no-op for a binary clip; only helps blended coverage). Residual in-motion edge
flicker is the DLSS-resolvable kind; hashed alpha stays the escalation. Superseded for imported
content once H1 ships BC DDS with offline mips. Also landed same day: **per-slot RT materials**
(B3 follow-up) — BindlessTable registers one record per submesh with THAT slot's albedo/MR/
params (`SlotMaterial` overload; Pass_BuildAS builds the slot array from GBufferRenderable),
so palms reflect bark + green fronds instead of slot-0 everywhere; hit shaders unchanged.

**C1b — per-slot pipeline materials.** *(exec: Fable — draw-loop/batching restructure; user
decision 2026-07-14: kill the one-PSO-per-object limitation before shadows build on it)*
Each material slot gets its OWN graphics `Material` (PSO): per-slot defines
(`NORMALMAP_IS_RG` / `USE_TBN` / `MR_LAYOUT_GLTF` / `ALPHA_TEST`) and per-slot raster state
(cull NONE only on genuinely two-sided slots) — replacing C1's union-of-flags. Scope:
- `GBufferRenderable`: `slotGraphicsMaterials_[i]` built from slot i's `MaterialData` + flags;
  keep the base-class `graphicsMaterial_` as the slot-0 alias so every single-slot object and
  non-GBuffer renderable path stays byte-identical. Same treatment for the instanced variants
  (per-slot `INSTCB_SLOT_PARAMS` materials). ONE shadow material for now (depth-only is
  slot-agnostic until C2). Drop the "slot 0's PSO wins" warning.
- Multi-slot `Render()` loop: bind the slot's material per submesh (bind cache already elides
  redundant PSO sets; optionally sort submeshes by material). UniformBinder CB handles come from
  slot-0 reflection — all gbuffer permutations share the PerObject layout; assert offsets match
  across slot materials rather than recomputing per slot.
- `InstancedDrawBatch` multi-slot: bind the lead's slot-i instanced material inside the existing
  submesh loop (batch compatibility already guarantees identical slot sets via
  `SameInstanceSlots`; PSO switches are per-slot-per-batch, not per-instance).
- MaterialManager desc-caching dedups identical slot PSOs across objects (all palms share the
  same 2 pipelines: opaque + masked).
- Gates: demo byte-identical (single-slot alias path untouched); palm grove still instances;
  **acceptance = `palm.mixed.slots` renders fronds with CORRECT glTF sampling while trunk uses
  the sandstone preset**; opaque-only glTF objects (boulder) lose the needless ALPHA_TEST/cull-
  NONE they inherited from the union.

**C2 — masked shadow passes.** *(exec: Fable — CSM + VSM page render + point paths, easy to
regress)* Depth-only shadow paths (CSM, VSM page render, point/spot) treat
everything as opaque today → fronds would cast solid-blob shadows. Builds on C1b: masked slots
get a per-slot masked SHADOW material (albedo SRV + `AlphaTestClip`), opaque slots keep the
shared depth-only pipeline — no union hacks. Do the **sun path (CSM + VSM) first** — palms are
sunlit; point/spot masked shadows can lag behind (campfire is inside a cave of solid rocks).
Watch VSM perf: fronds are static, so cached/per-page-culled pages keep the cost bounded. Also
covers the indirect-shadow path interplay (ShadowGpuData folded submeshes in B3). Acceptable
interim state after C1/C1b: solid shadows (visible but not blocking).

**C2 as landed *(Fable; DONE 2026-07-14)*:** ONE masked variant of `shadow_indirect_csm.hlsl`
(`SHADOW_MASKED=1`) covers **every GPU-driven shadow consumer** — the per-view CSM/spot/point
ExecuteIndirect path AND the VSM per-page draws — because both bind
`ShadowGpuData::IndirectShadowMaterial()`, which returns the masked PSO whenever the caster set
contains any alpha-masked group. It deliberately diverges from the per-slot-material sketch
above: the GPU-driven paths draw whole (mesh, submesh) GROUPS via ExecuteIndirect, so the mask
is per GROUP, and one PSO serves the WHOLE set (opaque groups carry texSlot=~0 and its PS
early-outs before sampling) — preserving the single-ExecuteIndirect structure with no per-class
arg partitioning and no second VSM page loop. Data: per-group `groupMask_` uint2 {albedo table
slot (~0=opaque), asuint(alphaCutoff)} built in Rebuild from the FIRST object's slot
MaterialData (shared-mesh semantics like the mega buffer / RT BLAS); masked albedos are a
bounded 16-entry descriptor table (t3.., overflow warns + those groups cast solid); the VS
(`PosUV_InstCasterId` layout, UV at offset 40 of VertexPNTUV) forwards UV + the group's mask
data, the PS clips; CULL_NONE (fronds are double-sided). Levels with no masked groups keep the
null-PS fast path (demo byte-identical). Legacy CPU fallback paths (per-object RenderShadow +
instanced batch) still cast solid — the B3 submesh loop is the hook if that ever matters (the
Rung0 indirect path is the default everywhere). Verified: atoll palm shadows = serrated
per-leaflet silhouettes in BOTH VSM and Legacy (scene luma 99.7→102.6 = less over-shadowed
area), cull validation PASS, demo unchanged, scene-stress=15 CLEAN.

## Part D — emissive meshes

**STATUS: DONE (Opus 4.8, 2026-07-15) — verified with a glowing ember box vs no-emissive reference.**

*(exec: Opus 4.8)* Extend `gbuffer.hlsl` (and the masked variant) with per-material-slot `emissiveColor` (rgb) ×
`emissiveStrength`, optional `emissiveTexture` (from A3). Write into the existing emissive
target (`RT2`); `compose_cs.hlsl` already adds it. JSON knobs on material entries (slot-level,
same shape as C1's flags): `"emissiveColor": [r,g,b]`, `"emissiveStrength": x`,
`"emissiveTexture": "..."`. Default 0 =
zero-cost for existing content. Used for: campfire embers/coals, flame cards inside the mesh
pile. Note: with no bloom pass the glow is subtle — the point light (Part F) carries the effect.

## Part E — particle system (GPU-simulated)

Scope: GPU sim (compute spawn + update) with an instanced billboard draw. Per-frame CPU cost per
emitter = one CB update + 2 dispatches + 1 draw, regardless of particle count — consistent with
the engine's GPU-driven direction and its known CPU-submission bottleneck. Budgets stay modest
for the campfire (≤ ~2K particles per emitter), but the design scales.

**E1 — GPU sim core.** *(exec: Fable; DONE 2026-07-15, committed)* `sources/vfx/ParticleEmitter.{h,cpp}` (+ `ParticleTypes.h`),
`shaders/particle_spawn_cs.hlsl`, `shaders/particle_update_cs.hlsl`:
- `EmitterDesc` (JSON-serializable, unchanged by the GPU choice): `maxParticles`, `spawnRate`,
  `lifetime` [min,max], `initialSpeed` [min,max] + cone (direction, angle), `gravity`
  (negative = buoyancy), `drag`, `sizeOverLife` (start→end), `colorOverLife` (≤4 gradient
  keys, RGBA — A drives fade), `rotation` [min,max] + `spin`, `flipbook` {cols, rows, fps,
  randomStart, frameBlend}, `blendMode` (additive|alpha), `texture`, `localSpace` (bool),
  `sortParticles` (bool).
- **Buffers per emitter** (DEFAULT heap, persistent): particle state buffer
  `Particle[maxParticles]` (UAV/SRV), dead-list `uint[maxParticles]` + atomic counter (small
  counter buffer). One-time init dispatch fills the dead list. **Slot-array + dead-list scheme;
  no alive-list and no indirect draw needed** (see E2's degenerate-quad trick) — upgrade to
  `ExecuteIndirect` later when GPU-driven submission infra (shadow Rung 0) lands.
- **Spawn CS**: CPU accumulates fractional `spawnRate*dt` and passes an integer spawn count via
  root constants; CS consumes slots from the dead list, initializes particles. GPU RNG =
  PCG/Wang hash of (slot, frameIndex, emitterSeed) — no CPU randomness.
- **Update CS**: integrate velocity/gravity/drag, age, kill (push slot back onto the dead list),
  evaluate nothing that the VS can evaluate later (keep state minimal: pos, vel, age, life,
  rot, spin, seed).
- Curves/gradients (`sizeOverLife`, `colorOverLife`) pack into the per-emitter CB and are
  evaluated in-shader from normalized age — editor tweaks = CB update only, no buffer rebuild.
- Sim driven from the object `Tick` (same hook as `RotatingObject`) recording dispatches;
  editor pause stops Ticks → sim freezes naturally. Add a `vfx::g_freeze` debug toggle and an
  optional alive-count readback (debug HUD) — GPU sims are otherwise opaque to debug.

**E2 — rendering.** *(exec: Fable — transparent-pass/queue integration)* New renderable `ParticleEmitterObject` (subclass `RenderableObjectBase`,
`IsTransparent()=true`, `RenderLayer::Transparent`) → lands in the sorted `TransparentSimple`
bucket and draws inside `Pass_Transparent` (`SceneRenderer.cpp`, `Main_Transparent` — blending
already runs in sorted-queue order there):
- `shaders/particles.hlsl`: VS reads the particle state buffer as SRV, indexed from
  `SV_VertexID/6`; **dead slots emit degenerate (zero-area) triangles** — this is what lets us
  draw `maxParticles` quads unconditionally without an indirect draw or CPU-visible count.
  Alive slots expand a camera-facing quad; PS samples the flipbook atlas (frame from
  normalized age; optional frame-blend between adjacent frames — cheap and hides low flipbook
  fps); depth-test ON, depth-write OFF; additive or premultiplied-alpha blend state per emitter.
- No per-frame CPU upload of particle data at all; only the emitter CB.
- Particles are absent from G-buffer/shadow/RT — intended (no reflected/shadow-casting fire).
- **E2b (optional polish)** *(exec: Opus 4.8; DONE 2026-07-15 — via the engine's existing depthCopy)*: soft-particle depth fade using the scene depth SRV already
  available to the transparent pass.
- **E2c — sorting for `alpha` emitters (smoke)** *(exec: Opus 4.8; DONE 2026-07-15 — prev-frame camera pos, 1-frame lag imperceptible)*: single-workgroup bitonic sort of alive slots
  by view depth into a small index buffer, VS indexes through it (fine up to ~1–2K particles —
  enforce `maxParticles` ≤ sort capacity when `sortParticles` is set). Additive emitters (fire,
  sparks) skip it. Interim state before D2c lands: keep smoke opacity low — premultiplied alpha
  at low opacity hides most order artifacts.
- **Hazards** (both bit us before — see scene-stress history): (1) per-emitter DEFAULT-heap
  UAV buffers die with the object → release must be deferred/GPU-idle-safe on level switch;
  (2) UAV↔SRV transitions each frame go through the ResourceStateTracker, and the sim writes
  while a *previous frame's* draw may still read → either double-buffer the state buffer or
  prove the render-graph ordering makes it safe. `--scene-stress=30` is the gate.

**E3 — authoring + editor.** *(exec: Opus 4.8; DONE 2026-07-15)* As landed:
- **Shared schema/parser** `sources/vfx/ParticlePresets.{h,cpp}`: `ApplyEmitterJson(json,
  EmitterDesc&)` (all sim+render fields) + `ResolveEmitterDesc(objectJson)` with precedence
  **defaults < `preset` file < `overrides` object < top-level inline** (inline = the pre-E3
  back-compat path). Alpha (non-additive) emitters still auto-`sort` unless `sort` is set
  explicitly. `position` is NOT read here — the runtime emitter reads its object position.
- **Shared factory** `SceneObjectFactory::CreateParticleEmitterFromJson` (resolve + set
  transform); `SceneObjectRegistry`'s creator now delegates to it, and the editor spawn
  (`SpawnMeshCommand::CreateRuntime`) dispatches `particleEmitter` to the same factory — one
  code path for level-load and editor-spawn.
- **Editor** (all generic mechanisms already type-agnostic — reused, not rebuilt): outliner
  listing, selection, gizmo move (`GetPosition()` is read every frame by the sim), and
  save/load round-trip (document stores `type`+`properties` verbatim) work with no
  particle-specific code. Added: **Create ▸ VFX ▸ Particle Emitter** (inline default puff via
  `BuildParticleEmitterObjectJson`), and a `ParticleEmitterPropertyDrawer` (via the extension
  registry) that live-edits the tuning fields through `DescRef()` — the sim refills its CB from
  the desc every frame, so edits are instant — and mirrors each change into `obj.properties`
  (into `overrides` for preset-based emitters, top-level for inline) so it round-trips.
  Structural fields (maxParticles / blend / sprite, baked at Init) are shown read-only.
- **`AsParticleEmitter()`** internal-RTTI accessor (no `dynamic_cast`); `data/particles/fire.json`
  seeds the preset path (E4 tunes the full fire/smoke/sparks set).
- Known limits: `coneDir` is a LOCAL axis not rotated by the object transform (gizmo rotation
  doesn't steer emission — position is what matters); structural field changes need a re-create.
  Verified: `e3_particle_test.json` loads a preset+overrides fire (rate 180→220) beside an inline
  sorted-alpha smoke — both simulate at the authored rates and render distinctly (additive orange
  flame vs grey alpha dome); Release + Release_Editor build clean; scene-stress=30 with the
  emitter level in the churn cycle CLEAN.

**E4 — presets + tuning.** *(exec: GPT 5.6 terra — JSON iteration, user judges visuals)*
`data/particles/fire.json`, `smoke.json`, `sparks.json`.
**Primary textures = the user's AI-generated 8-frame flipbooks** (verified visually, staged):
- `import_staging/white_flame_frames_separate_png/` — flame silhouette with licking tongues,
  grayscale/white, base anchored bottom-center. Additive, orange→deep-red tint via
  `colorOverLife`, ~12 fps + `frameBlend`. This is the fire emitter's flipbook.
- `import_staging/smoke_frames_separate_png/` — frames of a rising smoke COLUMN (not a puff):
  use 2–4 large, slow, low-alpha particles → an animated plume over the fire, cheap. Optional
  edge haze: a second emitter with Kenney `smoke_01..08` round puffs.
Frames are 444×444 RGBA → H1's frame-sequence importer resamples to 512 and packs a 4×2
2048×1024 atlas (premultiplied alpha). From Kenney (CC0) still in use: sparks/embers =
`circle_05` or `star_05` (additive, gravity-pulled); `scorch_01..03` = Part D emissive
ember-bed candidates. AVOID from Kenney: `flame_05/06` (candle flames), `spark_XX` (lightning).
Mantaflow-baked flipbook remains the escalation path if the flame set falls short in the cave:
- Fire: additive flame flipbook, buoyant (gravity ≈ −2..−4), lifetime 0.5–1.0 s, grows then
  shrinks, orange→deep-red gradient.
- Smoke: alpha-blend, slow, long lifetime (2–4 s), grows steadily, gray with low alpha, mild
  cone spread, sorted.
- Sparks: tiny additive dots (or 1×1 flipbook), high initial speed, real gravity, short life.
Verify inside the actual cave lighting, not in the void.

## Part F — flickering point light

*(exec: GPT 5.6 terra — tiny Tick + JSON knob)* `pointLights[]` JSON: `"flicker": {"amplitude": 0.35, "frequencyHz": 7, "seed": 3}` —
modulate intensity (and optionally radius ±10%) in a Tick using layered sines / value noise
(NOT white noise per frame — that strobes). Editor inspector support. Keep `shadowsEnabled:
true` for the campfire — pointlight shadows exist and are cheap for one light.

## Part G — scene assembly (`data/levels/atoll.json`)

Prefer assembling **in the editor** (this is the dogfooding goal), saving via the document
pipeline; hand-edit JSON only for things the editor can't author yet.

**G0 — procedural island mesh.** *(exec: GPT 5.6 terra; DONE — commit "Way to atoll level G0", models/atoll_island.obj + atoll_lagoon_floor.obj)* No good free atoll meshes exist; generate one:
`tools/gen_island.py` (pure Python, writes OBJ directly — no Blender dependency): a ring-shaped
heightfield (radial gaussian ring + low-frequency noise), gentle beach slope crossing y=0 (ocean
shore-depth params need real underwater geometry to fade against), a flattened area for the
cave/camp, planar top-down UVs sized so sand tiles via `texOffsScale`. ~50–150k tris. Also
generate a simple lagoon-floor disc (sand, slightly below sea level) so the lagoon reads
turquoise-over-sand rather than deep-ocean.
**Sea level convention: ocean plane sits at y=0; author everything against that.**

**G1 — environment.** *(exec: GPT 5.6 terra; MOSTLY DONE — skybox rustig_koppie converted+imported via the H pipeline (GPU BC6H); verify ocean preset tuning before calling it closed)*
Skybox conversion runs through the importer backend (H1 ".hdr → skybox"
path, via UI or CLI):
1. equirect `.hdr` → 6 cube faces (CPU projection, float precision preserved);
2. cubemap assembly + full mip chain (skybox doubles as ambient/specular source);
3. BC6H_UF16 encode with DX10 header (RGBA16F fallback if BC6H misbehaves, 4× size).
Smoke-test first: load the converted cubemap in place of `textures/skybox.dds` on the demo
level before building anything else on top. Cube face order/orientation mistakes are the
classic failure — verify sun position matches the HDRI.
Sun: warm, ~35–55° elevation, azimuth chosen so
palm shadows rake across the beach. Ocean: enable with a copied+tuned preset
`data/ocean/atoll.json` — lower wind, turquoise SSS/scatter tint, strong shore foam; check
`GetShoreDepthParams` behavior against the island slope.

**G2 — island dressing.** *(exec: user in the editor + GPT 5.6 terra for JSON chores)* Island mesh (`renderLayer: "Terrain"`, sand material from the asset
guide, tiled). Palms: imported GLB spawned via B4 (one object, 5 material slots), 8–15 around
the ring, varied yaw/scale; if count grows, switch repeated palms to `instancedModels`. Rocks: 5–10
photoscanned boulders composed into an outcrop + walk-in cave (assembling this in the editor is
the point); a dark interior "cap" rock kills skylight leaks.

**G3 — campfire.** *(exec: user in the editor + GPT 5.6 terra for JSON chores)* In the cave: logs+stones mesh (asset guide), embers with emissive material
(Part D), three emitters — fire, smoke (drifting toward the cave mouth via cone direction),
sparks — and the flickering shadowed point light (Part F) at flame height.

**G4 — polish + verification.** *(exec: Opus 4.8 — scene-stress verdicts and shadow/foam tuning
need judgment)* Camera start on the beach facing the cave. Tune: ocean foam at
the shoreline, palm shadow quality (C2), fire readability from the beach at dusk-ish exposure.
Screenshot set: beach wide shot / cave interior / fire close-up. Run `--scene-stress=30`
(atoll↔demo). Confirm visuals with the user before calling it done (per screenshot-verification
rule).

## Part H — smart asset importer (editor UI)

Owns every conversion from `import_staging/` raw downloads into engine-ready content — nobody
hand-runs texconv, ever. **Ordering**: needs Part A (glTF parsing, for asset inspection);
H1/H2 can run in parallel with B–F; must land before G2 (island dressing) so palms/rocks get
mipped BC textures instead of raw WIC PNGs. The doc's part order is not strict execution order
here.

**H1 — conversion backend (no UI yet). — DONE (Opus 4.8), verified in-engine.** DirectXTex
vendored (`third_party/DirectXTex`, pinned `99a6a50`, CPU units only, warnings-off in vcxproj);
`sources/assets/AssetImporter.{h,cpp}` + `--import <dir> [--skybox <file.hdr>] [--high|--fast]
[--flip-green] [--bc5-normal] [--max-size=N] [--sky-face=N]` CLI (main.cpp, headless, verdict in
`asset_import.log`). Implements: PNG/JPG→mipped BC7 DDS (sRGB albedo / linear normal·MR, role by
filename, 16-bit/BGRA canonicalize, max-size downscale, BC5 opt, green-flip opt) with tbb
cross-texture parallelism; texture-set import (Poly Haven `_diff`/`_rough`/`_nor` → engine-layout
MR synth R=metal(0)/G=rough + preset registered to `data/materials.json` via nlohmann, clean diff);
frame-sequence→flipbook atlas (`_frame_NN`/single-base `_NN`, wide grid 8→4×2, POT cell, premult
alpha, BC7, `<base>.flipbook.json` sidecar); equirect `.hdr`→6-face cube→BC6H_UF16 DX10 (parallel
per-subresource compress, RGBA16F fallback). Verified: palm (15 loose), coast_sand set, smoke+flame
flipbooks, 1024 skybox all render correctly in-engine; glTF palm not mis-detected. Perf: OpenMP
enabled per-TU on DirectXTex (vcxproj `OpenMPSupport`, adds vcomp140 dep) + `TEX_COMPRESS_PARALLEL`
→ block-parallel compression across cores. Measured on 32c: FAST BC7 1024² ~9s→<1s, `--high`
215s→21s (~10×), palm folder 13s→6s, 1024 BC6H skybox 141s→63s. Per-file loop is serial (each
Compress already saturates cores via OpenMP; a tbb fan-out would nest-oversubscribe). Still ~20-60×
slower than a GPU BC encoder — good-enough for offline; option (2) = wire DirectXTexCompressGPU/
BCDirectCompute (needs a D3D device) if sub-second high-quality ever needed. BC7 default = FAST
mode-6; `--high` = exhaustive. normal-Y convention left as `--flip-green` opt (sand `_nor_dx`
rendered fine unflipped — convention not yet pinned). *(original spec below)* Vendor **DirectXTex** (source, MIT) into
`third_party/` — prefer the library over shelling out to `texconv.exe` (in-process progress
reporting, no binaries in the repo); keep the texconv-CLI route as a documented fallback if
vcxproj integration fights back. Implement:
- Texture import: PNG/JPG → BC7 DDS (sRGB flag for albedo, linear for normal/MR) with a full
  mip chain; optional BC5 for RG normals. **No MR pixel repacking for glTF assets** — the
  `mrLayout` shader flag (A3) already handles glTF channel order. Handle 16-bit PNG sources
  (Poly Haven ships some; WIC converts). Downscale to the max-size option (staged 4K rock
  textures → 2K).
- **Texture-set import** (Poly Haven-style folders with no glTF): synthesize an engine-layout
  `mr` texture from the separate grayscale maps (R=metal — constant 0 when no metallic map
  exists, which is the normal case for sand/rock; G=rough) and register a material preset for
  the set.
- **Frame-sequence → flipbook atlas**: detect `*_frame_NN.png` / `*_NN.png` sequences,
  resample frames to a power-of-two cell (e.g. 444→512), pack a grid atlas (8 frames → 4×2),
  premultiply alpha, emit flipbook metadata (cols/rows/frames) alongside for the particle
  presets (E4). Staged ChatGPT-generated flame/smoke sets are the first consumers.
- **Normal-map Y convention**: pin down the engine's expected convention first (inspect how
  the existing `*_normal` textures + shaders behave), then normalize on import with a
  flip-green option. Staged reality: Poly Haven `_nor_dx` = DirectX-style (-Y), glTF normal
  maps = OpenGL-style (+Y) — mixing them unnormalized turns bumps into dents in raking light.
- HDRI import: equirect `.hdr` → 6 faces → cubemap + mips → **BC6H_UF16 with a DX10 header**
  (RGBA16F fallback), matching the verified TextureCube loader contract.
- Compression on background threads (tbb is already in `third_party/`) — the editor must not
  hitch; BC7 on a 2K texture costs seconds of CPU.
- CLI entry point (e.g. `test_cube.exe --import <staging-dir> [--skybox <file.hdr>]`) for
  headless/batch use and for the executor.

**H2 — DDS-sibling resolution. — DONE (Opus 4.8), verified in-engine.** `Texture2D::CreateFromFile`
(the single chokepoint for glTF-material AND preset texture loads via MaterialData::LoadAlbedo/MR/
Normal) now prefers a sibling `<name>.dds` next to the requested source; already-`.dds` requests
pass through untouched. Falls back to WIC with a one-time "unmipped texture" `OutputDebugString`
warning (`LogTextureResolveOnce`, per-path dedup). SRV sRGB-ness stays usage-driven (CreateFromDDS_
line ~349), so a BC7_UNORM_SRGB albedo sibling + `Usage::AlbedoSRGB` → sRGB SRV; BC7_UNORM MR/normal
+ linear usage → linear — correct color space regardless of the DDS's own flag. Loader already maps
BC7_UNORM/_SRGB/BC5 (the H1 outputs). Verified DECISIVELY: imported the coconut palm to `.dds`
siblings, renamed all 15 source `.png` away so ONLY `.dds` existed, booted the palm — rendered fully
textured (bark/masked fronds/coconuts/normals/MR/shadows), proving the glTF's `.png` URIs resolved
to siblings with the asset byte-identical. Uncommitted (`sources/materials/Texture2D.cpp`).
*(original spec below)* *(exec: GPT 5.6 terra — small resolver change; escalation rule
applies since it touches texture loading)* When resolving any texture path (glTF materials, material
presets), prefer `<name>.dds` sitting next to the source file; fall back to the original
PNG via WIC with a one-time "unmipped texture" log warning. This kills reference remapping
entirely: the imported glTF stays byte-identical, DDS files just appear beside its textures.

**H3 — importer UI. — DONE (Opus 4.8 2026-07-16 + heavy same-week rework: table UI, move-not-copy routed by type, texture-pick dialog, progress bar, CB Import button; the unit/scale normalizer landed as the spawn-scale manifest verdict and split-by-top-level-nodes landed via Codex as virtual node records).** *(original spec below)* Content browser "Import…" button → window that scans `import_staging/`
and lists detected assets (glTF/GLB with texture sets, texture-only folders, `.hdr` files)
with what was found: primitive/material counts, alphaMode/doubleSided flags, texture
resolutions, and license/author pulled from `source.txt` **or the glTF `asset.extras`**
(Sketchfab embeds author/license/source there — the coconut palm proves it; auto-harvest).
Options stay minimal: target name, max texture size, fast/high BC quality, ".hdr → skybox"
toggle, a **unit/scale normalizer** (show the asset's baked world-space bounding size and offer
"normalize longest axis to N meters" → bake a uniform scale, or write a default `scale` into
spawned entries) — glTF has no reliable 1-unit=1-meter guarantee and Sketchfab assets vary wildly
(measured in `import_staging/`: coconut_palm ~6 m upright but rock_boulder ~115 m, campfire ~120 m,
all at scale 1.0 — cm-authored with no meter conversion), and **"import as: single asset / split
by top-level nodes"** — split registers each
top-level node as its own spawnable registry entry (`rocks.glb#node:Rock_1`, `...Rock_2`) for
prop packs, while a palm stays one asset. (Fallback for packs authored as ONE primitive:
a "separate loose parts" connected-components split — optional, H4-tier; Blender is the
one-off workaround.) Import = copy gltf+bin into `models/<name>/`, write DDS siblings (H1),
append a CREDITS.md entry, refresh AssetRegistry, completion toast. The imported asset is immediately
spawnable via B4. Verify end-to-end with the coconut palm: Import → spawn → mipped BC textures
confirmed (no shimmer at distance), CREDITS.md entry correct.

**H4 (optional polish). — DONE (Codex, commit 658f546; reviewed + amended by Fable 2026-07-16).**
Shipped far beyond the spec: manifest-backed freshness (`.assetimport.json` per import with
per-source size/writeTime/hash + outputs inventory), per-RESOURCE status
(Untracked/Staged/UpToDate/SourceNewer/Incomplete) with dependency mapping (set maps, flipbook
frames, same-stem loose), badges + one-click Import/Reimport in both the import panel and the
content browser (list column, tile badge, context menu), partial per-resource reimport, deleted
source ⇒ removes only its mapped output. Fable's post-review amendments: (a) dialog subset imports
are MERGE imports — converting a subset never deletes previously-imported sibling outputs; only a
full import syncs/owns the destination folder; (b) Refresh built a manifest-parse + source-walk per
RECORD — now cached per import folder (`ImportFolderContext`), killing the O(records × files) cost;
(c) folder-wrapped `.hdr` skyboxes (Poly Haven unzips into a folder) are now detected by the import
panel and by the registry's reverse skybox mapping. *(original spec)* Reimport: detect a source
newer than its DDS siblings, offer one-click reimport; per-asset import status badges in the
content browser.

**H5 — GPU texture encoder (do before Part I). — DONE (Fable 5, 2026-07-16), uncommitted.**
Implemented via DirectXTex's own DirectCompute path instead of touching the engine device: vendored
`BCDirectCompute.cpp` + `DirectXTexCompressGPU.cpp` (same pin 99a6a50) + `Shaders/{BC6HEncode,
BC7Encode}.hlsl`, precompiled the 14 `.inc` blobs with fxc (cs_5_0 + cs_4_0, committed —
no fxc step in the build; per-file `AdditionalIncludeDirectories=Shaders\Compiled` on
BCDirectCompute.cpp). `RunImport` creates a private D3D11 FL11.0 hardware device (offline file
conversion — zero interop with the engine's D3D12 device; WARP intentionally rejected, it loses to
the OpenMP CPU path), released via RAII on every exit. Single chokepoint `CompressBlocks()` routes
BC6H/BC7 to `Compress(ID3D11Device*,...)` and everything else (BC5) to CPU; GPU failure mid-run
degrades to CPU per-texture instead of failing the import. CPU knobs (PARALLEL/BC7_QUICK) are
stripped for the GPU call — the GPU encoder always runs its full mode search, so `--high` is now
free. Fallbacks: `--cpu` CLI flag, "GPU texture encode" checkbox in the import panel (default on),
auto CPU when device creation fails (headless/CI). Measured (32c CPU vs RTX GPU): campfire 3×2K BC7
fast 7.25s→2.16s (~3.4×), `--high` ~60s→1.91s (~30×), palm 15 tex 6s→1.75s; fmt/mips outputs
byte-format-identical (fmt=98/99, mips=12, DDS roundtrip-verified). Device-less CLI regression ok
(`gpu encode : unavailable → CPU path` line in asset_import.log is the tell). *(original spec
below)* The CPU DirectXTex path is
the bottleneck: even with OpenMP block-parallelism a 2K BC7 costs seconds and `--high` is ~20s, so
importing a palm/rock pack stalls for many seconds (H3 now shows a progress bar, but the wall-clock
is the real problem). Move BC6H/BC7 compression onto the GPU so a 2K encode is sub-second.
- **Approach**: wire a compute-shader block encoder (candidates: `DirectXTexCompressGPU` /
  BCDirectCompute reusing the engine's existing D3D12 device + a transient upload/readback path, or
  a vendored HLSL BC7/BC6H encoder à la GPURealTimeBCn). BC7 for LDR albedo/MR/normal, BC6H_UF16
  for the skybox cube. Keep the CPU path as a fallback (headless CLI with no device, or GPU encode
  failure) behind a flag so `--import` still works device-less.
- **Progress**: the atomic `progressDone`/`progressTotal` channel added in H3 stays the API — the
  GPU path just advances it per-texture as dispatches retire.
- **Contract**: byte-format identical to the CPU output (BC7_UNORM/_SRGB, BC5, BC6H_UF16 + DX10
  header, full mip chain) so H2's DDS-sibling resolution and the loaders need no change.
- **Verify**: re-import the palm + a Poly Haven 2K rock set, confirm identical in-engine appearance
  vs the CPU output and a large wall-clock win (target: 2K FAST well under 1s, `--high` a few
  seconds); confirm the device-less CLI fallback still produces valid DDS.

**H6 — normalize glTF MR to engine layout on import.
— DONE (Fable 5, 2026-07-16), uncommitted.** Importer: packed-MR stems (`metallicroughness`,
`metalrough`, `_mr`, `_orm`, `_arm` — all share G=rough/B=metal) are repacked via TransformImage
swizzle (R=metal←B, G=rough←G, AO parked in B←R, A=1) before BC compress; log tag
`[mr gltf->engine]`. Loader: `MaterialDataManager` sets `mrLayoutGltf` from the convention —
false when the MR path is already `.dds` or a sibling `.dds` exists (mirrors H2's
`Texture2D::CreateFromFile` resolution), true only for raw-png staging previews.
**AMENDED TWICE.** (1) Real regression: imported palm rendered metallic/black — `MR_LAYOUT_GLTF`
gated TWO things, the fetch swizzle AND the glTF factor-MULTIPLY semantics (A3); turning it off for
imported DDS switched materials to engine `lerp(params, tex)` semantics, so `metallicFactor=0`
stopped neutralizing the palm's metal=255 filler. Multiply-for-everyone is a NO-GO
(`MaterialParams.metalRough` defaults {0.0, 0.35} + demo levels set params on textured meshes).
(2) User verdict on the first fix (a second GLTF_FACTORS define): NO runtime defines, NO glTF-ish
textures in the project — **the import step must produce FINAL engine data**. Final architecture:
the importer BAKES the glTF material factors into the textures at conversion —
`HarvestGltfFactors` (cgltf parse of every staged glTF) maps each referenced texture file to its
material's factors; packed MR becomes `R = metal*metallicFactor, G = rough*roughnessFactor, B = 0`
(log tag `[mr baked mF=… rF=…]`, palm shows mF=0.00 — filler neutralized IN the DDS, thumbnails
green like set-synth MRs); albedo gets baseColorFactor.rgb baked sRGB-correctly (decode → linear
multiply → re-encode; alpha factor NOT baked — it stays a runtime alpha-test parameter, applied
identically for raw and imported). GLTF_FACTORS define REVERTED — shaders are back to the single
pre-H6 `MR_LAYOUT_GLTF` branch, which now exists ONLY for raw-staging glTF previews; imported
assets take the plain engine path (texture replaces params, factors already inside; absent
textures still fall back to factor params — engine lerp gives exactly glTF semantics there).
Texture shared by materials with different factors → WARN in log, last wins. Verified: palm
converts with mF=0.00 tags, models/coconut_palm DDS refreshed with baked versions, scene-stress
boot CLEAN. Re-import is required for anything imported before this landed (nothing else was). *(original spec below)* Today the importer is convert-only
for glTF packed metallicRoughness: the DDS keeps glTF's ORM channel order (R=AO, G=rough, B=metal)
and the runtime reads it via the per-material `mrLayoutGltf` flag → `MR_LAYOUT_GLTF` shader define →
a whole extra **PSO permutation** just to swizzle `.bg`. Meanwhile texture-**sets** already synth an
engine-layout MR (R=metal, G=rough), so imported glTF assets are second-class vs sets/presets. Fix
= tidy it at import so imported assets are first-class engine-native:
- **Importer**: for MR-role textures (`metallicRoughness` / `_mr`), repack channels before BC
  compress — `outR = inB` (metal), `outG = inG` (rough); AO (inR) is unused by the engine MR (`.rg`)
  so drop it (or park it in B). Output DDS is engine-layout MR. (Texture-set path already does this;
  this makes the glTF path match.)
- **Convention**: *a `.dds` MR is always engine-layout; a raw `.png` MR is glTF-layout.* Clean and
  documentable because our importer is the only producer of `.dds`.
- **Loader**: `MaterialDataManager` (the glTF material path, currently hardcodes
  `md->mrLayoutGltf = true`) instead sets it from the convention — `false` when the MR resolves to a
  `.dds` sibling (path is available as `d.mrPath`; cheap `fs::exists` on the `.dds`). Raw-staging
  glTF preview stays correct (still glTF-layout); imported assets stop emitting `MR_LAYOUT_GLTF`.
- **Migration**: already-imported glTF assets must be **re-imported** — their existing MR DDS are
  glTF-layout and would be misread as engine-layout after the convention flips (metal/rough swap).
  Cheap for the atoll (a few palms/rocks).
- **Verify**: re-import the palm, confirm its glTF material no longer emits the `MR_LAYOUT_GLTF`
  define (uses the default gbuffer variant), metal/rough read correctly, and the scene looks
  identical to before. Bonus: one fewer shader permutation for every imported asset.

## Part I — material editor (v2, rewritten 2026-07-16 after H4–H6 landed)

Parameter-level editing of materials — still deliberately small: texture pickers + sliders +
checkboxes. NOT a node graph, NOT shader authoring, no material-instance hierarchy. **Rewritten**
because the original spec predated the importer pipeline and the content-browser file management,
and audit against the code found three stale assumptions: (a) `MaterialPreset` is only 3 texture
paths + `normalIsRG`/`useTBN` — most fields the editor must expose (tint, metal/rough scalars,
emissive, alphaCutoff/twoSided, tiling) do NOT exist in the preset schema, they are per-OBJECT
`MaterialParams`; (b) the monolithic `data/materials.json` now fights the content browser — file
delete/move/status-badges are per-FILE and had to be disabled for presets; (c) infrastructure the
spec assumed missing now exists (offscreen mesh thumbnails in `AssetThumbnailCache`, the
`ConsumeMaterialHotReloadFlag` → `RefreshMaterialHandles` → `OnMaterialHotReload` vehicle, CB
drag-drop payloads + `AssignMaterial` action, GPU object-ID picking). `mrLayout` is DEAD as an
editable field (H6: a `.dds` MR is always engine-layout). **Ordering**: B4/C/D/H all done — Part I
is unblocked end to end; needed during G2–G3 for tuning sand/rock/fronds/embers in place.

**I0 — materials become per-file assets with a full schema (the foundation). — DONE (Fable 5,
2026-07-16), uncommitted.** Implemented as specced, notes: schema v2 lives in `MaterialPreset`
(+`emissivePath` parsed but RESERVED — no consumer, the gbuffer SRV table is 3); param defaults
ride NEW `MaterialData::hasPresetParams/presetParams`, seeded in `GBufferRenderable::Init` only
when the slot's params memcmp-equal the factory defaults (explicit level-JSON overrides win);
alphaTest/alphaCutoff/twoSided map onto the existing glTF MaterialData fields so the per-slot PSO
plumbing (ALPHA_TEST define, cull) is reused unchanged. Migrated 5 presets to
`data/materials/<name>.json` (texture keys + flags ONLY — no params → hasParams=false → demo
byte-identical), `git rm data/materials.json`; App + EditorPreviewRenderer load legacy-then-dir
(per-file wins on clashes); AssetThumbnailCache material-dependency hash prefers the per-file
path (legacy fallback); AssetRegistry gained a `data/materials` DirRoot (id.key = filename STEM =
material name, record.path = file) replacing the monolith parse block, texture-based import-status
carry ported per-file; importer `WritePresets` writes/merges per-file, `RepointPresetPaths`
sweeps `data/materials/*.json`; CB Delete enabled for materials (request carries `assetPath`
since a material's id.key is its NAME), move stays refused (name-referenced, flat folder).
Verified: build clean, scene-stress boot CLEAN (demo materials resolve from files), CLI set
import writes `data/materials/coast_sand_01.json`.
**AMENDED (user request): schema v2 also carries an optional `"shader"` key** — a material can
override the gbuffer shader for feature materials (vegetation sway etc.). Wiring: applied to the
slot's graphics PSO in `ConfigureGraphicsPipeline` (slot 0 / single-slot, so the base material and
— via the `_csm`-suffix convention in `BuildShadowDesc` — the SHADOW PSO follow automatically) and
in `BuildSlotMaterials` for slots 1+ (each slot resets shaderFile to its own override or the
object default; deliberately NOT in `ApplySlotPipelineOverrides`, which the instanced descs reuse
with their own shader). Contract for custom shaders: keep the standard PerObject b0 layout (the
CB-size guard falls a mismatching slot back to slot 0) and ship the `<name>_csm.hlsl` counterpart.
Auto-instancing is DISABLED for objects whose materials override the shader (no instanced
counterpart — a batch would silently draw them without the feature); a future sway shader that
needs instancing must add its own instcb variant. GpuInstancedModels ignores overrides (its own
shader family). *(original spec below)* One material = one file:
`data/materials/<name>.json`, where the filename stem IS the material name (levels and slots keep
referencing materials by name — identity unchanged). Schema v2 per file: textures
(`albedo`/`mr`/`normal`/`emissive`) + parameter DEFAULTS (`tint` rgba, `metalRough` xy,
`emissiveColor`+`emissiveStrength`, `alphaTest`+`alphaCutoff`+`twoSided`, `texOffsScale`,
`normalStrength`, `normalIsRG`, `useTBN`). Semantics: material params seed the per-object
`MaterialParams` at Init; per-object overrides in level JSON keep working and WIN (demo levels
must not change appearance — verify by diffing a demo screenshot before/after). Work: (1)
`MaterialPreset`/`MaterialDataManager` extended to schema v2 + a folder loader
(`LoadPresetsFromDirectory("data/materials")`); (2) migrate the 5 existing presets to files and
DELETE the monolith (keep `LoadPresetsFromJsonFile` code one release for safety, nothing ships
using it); (3) the importer's texture-set preset registration writes a per-file material instead
of merging into the monolith (`WritePresets` → per-file, `RepointPresetPaths` follows); (4)
AssetRegistry: MaterialPreset records keep `id.key` = material NAME (all existing consumers —
slot combos, SetMaterialCommand — keep working) but `record.path` = the real file → H4 badges and
the content-browser Delete/Move/context-menu work for materials automatically (remove the
"presets live in materials.json" guards added with the file-management pass).

## Part J — mesh asset format (added 2026-07-16; executes NOW — after I0, before I1)

Level objects currently repeat renderer plumbing per instance — `inputLayout`, `shader`,
`material`, often `texOffsScale`/`renderLayer` — for every mesh placed (see any staticMesh entry
in a level file). That is boilerplate that belongs to the ASSET, not the placement. Same
philosophy as I0: one mesh = one file, engine-native, first-class in the content browser.

**J1 — mesh assets (`models/<name>.mesh.json`). — DONE (Opus 4.8, 2026-07-16), uncommitted.**
As landed: `SceneObjectFactory::ResolveMeshAsset` expands a `"mesh"` reference by folding EVERY
asset key the object doesn't already carry (general fold, not a fixed list — `geometry`→`model`,
`spawnScale` excluded as a spawn-time hint; object keys always win), called at the top of both
`CreateStaticMeshFromJson` and `CreateTransparentMeshFromJson` so every load/spawn/respawn path
(SceneObjectRegistry, SceneStress, SpawnMeshCommand, SetMaterial/SetMaterialSlot respawn) resolves
uniformly. The editor spawn factory (`StaticMeshObjectFactory::BuildDefaultJson`) detects a
`.mesh.json` record (`IsMeshAsset`) and emits the compact `{"mesh": path, position, scale}` form
(scale from the asset's own `spawnScale`); raw `.obj/.gltf` records keep the legacy inline form.
AssetRegistry indexes `.mesh.json` under the models root (Mesh type, id.key = path). The importer
writes `models/<name>/<name>.mesh.json` on a whole-asset mesh import (`geometry` = the copied
glTF, `spawnScale` from the size-normalizer). Back-compat is intact — legacy `model`+inline
objects load byte-identically (ResolveMeshAsset returns them untouched) and round-trip verbatim.
Verified: a compact `{"mesh":"models/atoll_island.mesh.json"}` level booted headless → 1 caster,
RT + cull-validation PASS (material/renderLayer/texOffsScale all pulled from the asset);
scene-stress on the legacy demo levels CLEAN. *(original spec below)*
One file describes how to RENDER a piece of geometry:
```json
{
  "geometry": "models/atoll_island.obj",      // or "models/rocks/scene.gltf#node:Rock_1"
  "inputLayout": "PosNormTanUV",              // optional, default PosNormTanUV
  "shader": "shaders/gbuffer.hlsl",           // optional, default gbuffer (materials can still override per I0)
  "material": "sandstone_cracks",             // optional; slot 0 (default: "auto" for glTF, first preset otherwise)
  "materials": ["bark", "auto"],              // optional per-slot list (B2)
  "renderLayer": "Terrain",                   // optional default
  "spawnScale": 0.015                         // optional; wins over .assetimport.json spawnScale
}
```
- **Level objects shrink** to placement + reference: `{"type":"staticMesh", "mesh":
  "models/atoll_island.mesh.json", position/rotation/scale, name, id}`. Per-object overrides
  (material/materials/shader/texOffsScale/renderLayer/params) REMAIN LEGAL and WIN over the mesh
  asset's defaults — same layering rule as I0 material params.
- **Back-compat is a hard rule**: existing objects using `model` + inline plumbing keep loading
  byte-identically, and the editor's round-trip serialization must NOT rewrite them into the new
  form (verbatim `properties` preservation already guarantees this). Only NEW spawns emit the
  compact form.
- **Resolution point**: `SceneObjectFactory::CreateStaticMeshFromJson` (+ StaticMesh) — when the
  object has `"mesh"`, read the asset file, then apply the object's own overrides on top. Engine
  code, no editor dependency (ships in non-editor builds).
- **Importer (H3)**: a mesh import also writes `models/<name>/<name>.mesh.json` (geometry=the
  glTF, everything else defaulted — glTF assets are nearly empty files; spawnScale moves in from
  the manifest verdict). Split-by-nodes can later emit one tiny mesh asset per node instead of
  virtual registry records (optional harmonization, not required for J1).
- **Registry/browser**: `.mesh.json` under models/ = the spawnable Mesh record (id.key = path,
  like other file assets); the spawn factory writes `"mesh"` for these records and keeps the
  legacy `"model"` form for raw .obj/.gltf records. Existing drag-drop/spawn/delete/move work via
  the file-management pass. (Optionally hide raw geometry files that are owned by a mesh asset —
  polish, not J1.)
- **Verify**: demo levels load unchanged (round-trip diff clean); spawn a mesh asset from the
  browser → compact level entry; palm .mesh.json with `"materials"` slots renders identically to
  the same palm spawned the legacy way.

**J2 (future, not scheduled)**: mesh-asset-level flags as they become real — castShadow,
LOD config, collision ref; per-node mesh assets for split glTF packs.

## Part I (continued) — material editor steps I1–I4

**I1 — material file operations in the browser.** *(exec: GPT 5.6 terra — small once I0 lands)*
Content-browser context menu on a material: Create New (from a default template), Duplicate,
Delete (the I0 file records make Delete/Move just work — this step only adds Create/Duplicate).
Rename = file rename, BUT levels/slots reference materials by name — on rename scan
`data/levels/*.json` for references and either warn ("N references in: …") or offer auto-update;
never silently break a level.

**I2 — material editor window (the core).** *(exec: Opus 4.8; escalate to Fable ONLY for the
in-place texture reload if the GPU sync fights back)* Opened by double-clicking a material in the
content browser or an "edit" button next to the slot's preset picker (B4 inspector). Fields =
schema v2: texture pickers for albedo/mr/normal/emissive (thumbnails via AssetThumbnailCache, DDS
preferred per H2), tint, metal/rough scalars, normalStrength, texOffsScale tiling,
alphaTest/alphaCutoff/twoSided, emissiveColor/emissiveStrength, normalIsRG/useTBN. NO mrLayout
(dead per H6). **Live-apply, two tiers** (this replaces the old spec's "respawn everything"):
- *Scalar params + texture swaps*: mutate the CACHED `MaterialData` in place — WaitForPreviousFrame,
  reload changed textures into the same MaterialData via UploadBatch, update params, then raise the
  existing material-hot-reload flag → `RefreshMaterialHandles` → `OnMaterialHotReload` rebuilds
  descriptor handles engine-wide. CAVEAT verified in code: `OnMaterialHotReload` ONLY rebuilds
  handles — the in-place texture reload is the delicate half (frames in flight; this is the Fable
  escalation point). Param edits must also re-seed `MaterialParams` of scene objects referencing
  the material WITHOUT a level override (per-slot: `slotPresets_` already records which slot uses
  which preset).
- *PSO-affecting toggles* (alphaTest on/off, twoSided, normalIsRG): these change shader
  defines/cull mode → fall back to the proven respawn pattern (SetMaterialCommand-style: idle →
  remove → recreate from JSON → re-add) for every referencing object.
Undo/redo via `SetMaterialPropertyCommand` on the editor command stack; the file is saved on an
explicit Save button (dirty marker in the title), not on every slider tick.

**I3 — "Save as material" on a slot.** *(exec: Opus 4.8)* glTF import (A3) produces runtime-only
auto-materials; one click promotes a slot's effective material (auto-material or
preset+overrides) into a named `data/materials/<name>.json` and rebinds the slot to it (existing
SetMaterialSlotCommand). Post-H6 this is trivial data-wise — imported textures are already final
engine-layout DDS, so the new file just references them. Verify with the palm fronds: tweak
`alphaCutoff`, save as `palm_fronds`, reload level, slot still references it.

**I4 — material thumbnails + viewport drag-drop.** *(exec: Opus 4.8 — extends the existing
offscreen thumbnail path, not a new renderer)* (1) Sphere-preview thumbnails: add a Material kind
to `AssetThumbnailCache` reusing the existing offscreen mesh-thumbnail pipeline (lit sphere +
the material's textures/params). (2) Drag a material from the browser onto a mesh in the
viewport: the asset drag payload + `AssignMaterial` action already exist; add the viewport drop
target and resolve the hovered object (+ slot, via submesh id where applicable) through the GPU
object-ID picking readback from level-editor Step 20. Both sub-steps are independent — land in
either order.

## Risks / gotchas summary

- glTF handedness/winding vs engine — verify first thing in A2 with an asymmetric test asset.
- glTF MR channel layout (G=rough, B=metal) vs engine `mr` (R=metal, G=rough) — shader-side
  `mrLayout` flag, not pixel re-encode.
- WIC-loaded PNG has **no mips** → distant shimmer; final textures should be BC7/BC5 DDS
  (texconv), or do A5.
- Submesh refactor (Part B) is the widest-touch step: LODs must re-simplify per submesh range,
  instancing/sort keys change (regression-gate on demo-level visuals AND batch counts), shadow/
  VSM/RT paths start iterating submeshes. Land B1 as a behavioral no-op commit before anything
  else; don't tangle it with the `instancedModels` GI→VSM refactor.
- Alpha-test + DLSS jitter shimmer; masked geometry opaque in RT reflections (accept/exclude).
- Particle GPU buffers: frame-in-flight safety on destroy (deferred release), UAV↔SRV hazard
  between sim and last frame's draw (double-buffer or prove ordering), scene-stress the level
  switches.
- GPU sim debuggability: no CPU-side particle state — rely on `vfx::g_freeze`, alive-count
  readback, RenderDoc. Budget extra time for the first "why is nothing spawning" hour.
- Skybox DDS must have a DX10 header (TextureCube rejects legacy-header HDR formats); cube
  face order/orientation is the classic conversion bug — smoke-test on the demo level first.
- Emissive without bloom is subtle — the flickering point light sells the fire, not the glow.

---

# Гайд по ассетам (для человека)

Всё складывай в `import_staging/<имя-ассета>/` как скачалось (glTF/GLB + текстуры, PNG/JPG/HDR
как есть). Конвертацию (DDS с мипами и BC-сжатием, кубмапа из .hdr) делает **импортер прямо в
редакторе** — кнопка Import в content browser (Part H); пока он не готов, исполнитель гоняет
тот же бэкенд через CLI. Тебе руками ничего конвертировать не нужно. MR-каналы вообще не
перепаковываются (шейдерный флаг). Рядом кидай текстовый файл `source.txt` со ссылкой и
лицензией — импортер сам дополнит CREDITS.md (у Sketchfab-ассетов автор и лицензия ещё и зашиты
внутри glTF, они подхватятся автоматически).

Качать всё заранее не обязательно: если на каком-то шаге исполнителю не хватит ассета, он
остановит шаг и попросит недостающее, повторив инструкцию и дав прямую ссылку (это прописано в
конвенциях выше, «Asset gate»). Так что можно начинать с пустым `import_staging/` — движковые
части A–F от ассетов не зависят (кокосовая пальма для тестов A–C уже лежит в staging).

## Что качать

**1. Skybox (небо):** [polyhaven.com/hdris](https://polyhaven.com/hdris) → категория
Skies/Outdoor, поиск `beach`, `tropical`, `sunny`. Нужен солнечный день с кучевыми облаками,
солнце не в зените. Качай **4K .hdr** (не .exr, не tonemapped). Лицензия CC0. Одного файла
достаточно. Конвертировать ничего не нужно: .hdr → DDS-кубмапа (BC6H) делается офлайн-скриптом
исполнителя, движок это уже читает (проверено — TextureCube принимает DX10-форматы как есть).

**2. Текстуры песка и камня:** [ambientcg.com](https://ambientcg.com) (CC0) и
[polyhaven.com/textures](https://polyhaven.com/textures) (CC0):
- Песок пляжный с рябью: на ambientCG ищи `Sand` (например «Sand с ripples»), на Poly Haven —
  `beach`, `coast sand`.
- Скала/камень для пещеры: `Rock`, `Cliff` (серо-коричневый, не лава).
- Качай **2K, формат PNG**, набор каналов: Color/Albedo, Normal (GL или DX — исполнитель
  разберётся, но отметь в source.txt какой), Roughness. Metallic для песка/камня не нужен.

**3. Пальмы (самое важное):** [sketchfab.com](https://sketchfab.com) → поиск `coconut palm` /
`palm tree`, слева фильтры **Downloadable** и License = **CC0** или **CC Attribution**. Качай в
**glTF** формате. Критерии выбора:
- реалистичная (не мультяшная), листья текстурой с прозрачностью (это норма), не «на миллион
  полигонов» — до ~100k треугольников;
- желательно 2–3 разных пальмы (высокая, наклонённая, молодая) для разнообразия.
Также проверь [polyhaven.com/models](https://polyhaven.com/models) — у них появляются
фотосканы деревьев/камней (CC0, идеальное качество). И если ты успел забрать бесплатную
библиотеку **Quixel Megascans на Fab** (акция была до начала 2025) — это лучший источник камней
и растительности; на [fab.com](https://fab.com) есть и текущий free-раздел (лицензия Fab
Standard позволяет использовать в своём движке).

**4. Камни для пещеры:** Poly Haven models → rocks/boulders (фотосканы, CC0) — 4–6 разных
валунов; либо Sketchfab `rock scan`, `boulder` (CC0/CC-BY, glTF). Пещеру соберём из валунов
прямо в редакторе. Если найдёшь готовый цельный `cave` меш с интерьером — тоже неси, но валуны
надёжнее.

**5. Костёр:** Sketchfab `campfire` (CC0/CC-BY, glTF) — брёвна + круг камней. Отдельно хорошо
бы текстуру углей/жара для emissive (подойдёт albedo углей из самого ассета или `Lava`/`Coals`
с ambientCG).

**6. Спрайты огня и дыма (для частиц):**
- [opengameart.org](https://opengameart.org) — поиск `fire flipbook`, `fire sprite sheet`,
  `smoke sheet` (фильтр по лицензии CC0). Нужен атлас кадров огня (сетка типа 4×4/8×8) и
  мягкие клубы дыма.
- [kenney.nl/assets](https://kenney.nl/assets) → Particle Pack (CC0) — хорошие мягкие дымы и
  вспышки, пригодятся как минимум для дыма/искр.
- Запасной вариант: исполнитель сгенерит флипбук сам (Blender Mantaflow) — но готовый атлас
  быстрее.

**7. Остров:** качать не нужно — меш острова исполнитель сгенерит процедурно
(`tools/gen_island.py`), песок натянем тайлингом из п.2.

## Лицензии
CC0 — бери не думая. CC-BY — можно, но нужно указать автора: просто сохрани ссылку в
`source.txt`, исполнитель заполнит CREDITS.md. Ничего с пометкой Editorial/NoAI/
NonCommercial-без-нужды лучше не брать, чтобы не разбираться.

## Порядок работ (актуализировано 2026-07-16)
СДЕЛАНО: A (glTF), B (сабмеши), C (листва), D (эмиссив), E1–E3 (частицы: сим, биллборды,
softparticles, сортировка, пресеты+редактор), H целиком (H1–H6: бэкенд, DDS-резолв, UI
импортёра, манифесты/бейджи/реимпорт, GPU-энкодер, запекание MR/факторов), G0 (остров),
G1 почти (скайбокс импортирован), I0 (пер-файловые материалы + shader override).

ОСТАЛОСЬ, в порядке исполнения:
1. **J1 — mesh-ассеты** (`models/<name>.mesh.json`) — следующий шаг; убирает бойлерплейт
   inputLayout/shader/material из объектов уровня перед массовой расстановкой.
2. **I1** (файл-операции материалов в браузере) и **I2** (окно редактора материалов) — к
   тюнингу песка/камня/листвы/углей в G2–G3. I3/I4 по ходу.
3. **E4** (тюнинг пресетов fire/smoke/sparks на реальных флипбуках) + **F** (фликер поинт-лайта)
   — к моменту костра (G3).
4. **G2 → G3 → G4** — расстановка острова, костёр, полировка + `--scene-stress=30` гейт.
