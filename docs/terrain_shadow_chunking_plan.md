# Terrain shadow chunking — execution plan

**Status: NOT STARTED.** Written 2026-08-20 after the investigation in
`docs/bug_shadow_lod_bias_perf.md` (read it first — the measurement methodology and the harness
keys used below were built there).

## The two problems this solves

1. **Shadow banding on the terrain ("разводы")** — user-reported: the island's shadow-caster LOD
   (LOD1+ via the per-view curve + `g_shadowLodBias = 1`) deviates vertically from the LOD0 the
   camera rasterizes, so the simplified caster surface shadows the detailed receiver in patches.
   Forcing shadow LOD0 today is unaffordable because the island is ONE caster (see fact F3).
2. **The island renders into every VSM page it covers** — its AABB spans every clipmap level's
   whole page grid, so its full LOD index range is drawn once per resident page:
   **~8.3 M triangles/frame** (F3), likely the bulk of `Pass_VsmPageRender`'s ~0.8 ms draw.

Chunking fixes both at once: a page only draws the chunks that overlap it, which makes "terrain at
LOD0 near the camera" cheap — the finest clipmap ring (12 m) sits inside 1–4 chunks.

## Facts already established — do NOT re-derive

- **F1. The island asset**: `models/atoll_island.mesh.json` → `models/atoll_island/atoll_island.mesh.bin`.
  41,760 verts, 1 submesh, 4 LODs: 82,944 / 41,471 / 20,736 / 9,952 tris. World AABB
  361 × 15.8 × 388 m. Source: `import_staging/atoll_island/atoll_island.obj`. Companion
  `atoll_lagoon_floor` is 256 tris — ignore it, it costs nothing.
- **F2. Resident VSM pages** (wind_test, doc-standard camera, from `logs/vsm_pages.log` via
  `--set=vsm.logPageStats:1`): clipmap levels L0..L5 = 112 / 104 / 85 / 50 / 11 / 1, locals 0.
- **F3. Terrain shadow cost today**: per-view LOD = clipmap level + bias(1); the island lands in
  every resident page ⇒ 112×41.5k + 104×20.7k + 85×9.9k + 62×9.9k ≈ **8.3 M tris/frame**.
  Post-chunking estimate (6×6 grid, terrain bias −1): **~0.7 M tris/frame** with LOD0 in the L0 ring.
- **F4. Groups**: caster mesh-groups are per (mesh, submesh) (`ShadowGpuData::Rebuild`,
  `meshToGroup` / `nextGroup += slots`). Currently **21 groups**; caps are
  `VSM_MAX_SETUP_GROUPS = 64` (vsm_page_setup_cs.hlsl) == `kMaxMegaGroups` (ShadowGpuData.cpp).
  6×6 = 36 chunks → 56 groups: **fits**. Finer grids need a cap bump.
- **F5. Per-slot plumbing already exists**: every caster SLOT has its own entries in
  `cpuBounds_` / `casterMesh` / `casterSub` (ShadowGpuData.cpp ~487–557) — today the object's one
  AABB is merely COPIED into each slot ("per-submesh bounds are a future refinement" comment), and
  `CasterMeta` makes the first slot lead its siblings (B3). Giving chunk slots **slot-count 1 and
  their own AABBs requires ZERO shader changes** — the setup CS and the scatter CS already handle
  independent casters.
- **F6. Bake plumbing**: `MeshLoadOptions::lodSimplifyOptions` already passes raw
  `meshopt_Simplify*` flags from mesh.json to `meshopt_simplify` (MeshManager.cpp:82) and is
  hashed into optionsHash. Our vendored meshoptimizer has `meshopt_SimplifyLockBorder`,
  `meshopt_SimplifySparse`, `meshopt_SimplifyErrorAbsolute`; the README explicitly recommends
  LockBorder (+Sparse+ErrorAbsolute) for "simplifying mesh subsets independently ... without
  introducing cracks" — our exact case.
- **F7. The camera NEVER renders the island above LOD0** (`SelectLodTier`: ratio = dist/radius,
  island radius ≈ 265 m ⇒ LOD1 at ~4 km). Island LODs 1–3 exist ONLY for shadow pages. Corollary:
  chunk-border cracks and/or skirts at LOD ≥ 1 can never appear in the gbuffer.
- **F8. The .bin format already stores a submesh table PER LOD** (`WriteMeshBinary::writeLod`)
  and `Mesh::Submesh` = {indexOffset, indexCount, materialSlot}. Chunks-as-submeshes need **no
  format change and no `kMeshBinVersion` bump** (a bump would strand every committed .bin — see
  the NOTE at MeshManager.cpp:121).
- **F9. Seam taxonomy**: (a) same-level neighbor chunks — all chunks in one clipmap view draw at
  the SAME LOD (LOD is per-view), so the only crack source is independent simplification of
  shared borders ⇒ fixed by LockBorder (or skirts). (b) cross-level seams (receiver pixels switch
  clipmap level at the 12/24/48 m rings, sampling depth rendered at different LODs) — **exist
  today already**, chunking neither adds nor removes them; the terrain bias of S4 shifts the first
  one from LOD1|LOD2 to LOD0|LOD1 and removes the mismatch inside the L0 ring entirely.

## Locked design decisions

- Chunk grid **6×6 over the island's XZ AABB** (≈ 60×65 m cells); empty cells emit no submesh.
- Chunks are **submeshes of the one island mesh** (one object, one material, one level entry).
- Runtime treats a mesh's submeshes as independent casters only when mesh.json says so
  (`"chunkGrid": N` load option → a bool on Mesh) — existing multi-submesh meshes (palms) keep
  B3 shared-bounds behavior unchanged.
- Simplify per chunk with `meshopt_SimplifyLockBorder | meshopt_SimplifySparse |
  meshopt_SimplifyErrorAbsolute` first; **skirts are the fallback**, not the default (S6).
- Terrain chunks get a **per-group shadow-LOD bias of −1** (cancels the global default `+1` ⇒
  curve = clipmap level exactly ⇒ L0 ring draws LOD0). VSM path only; Legacy CSM
  (`--shadow-mode=legacy`) keeps its uniform per-view LOD — documented divergence, and LockBorder
  means Legacy is merely LOD-mismatched there, not cracked.
- v1 restriction: chunking supports single-submesh input meshes only (assert + skip otherwise).

## Executor environment notes (hard-won; violate at your peril)

- Windows. Build **both configs** with MSBuild (locate via
  `vswhere -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`);
  Debug carries WITH_EDITOR. **MSBuild does NOT compile shaders** — shaders compile at runtime
  PSO creation, so "dxc accepts it" and even "it builds" prove nothing: RUN the app and look.
- The exe is GUI-subsystem: from PowerShell it DETACHES. Always
  `Start-Process -FilePath x64\Release\test_cube.exe -ArgumentList "..." -Wait`.
- A headless run only exits because of `--shot=...`; `--trace=N` runs also self-exit but must NOT
  be combined with `--shot` (the shot exits before the trace is written).
- Perf methodology: interleave A/B runs ×2 with `Start-Sleep 15` between launches (GPU downclocks
  on back-to-back starts), medians with the first quarter dropped, and ALWAYS report
  `Pass_Compose` as the unmoving control. Trace parser: `traceEvents` with `tid == 0` are GPU
  scopes; a ready-made parser pattern is in `docs/bug_shadow_lod_bias_perf.md`'s session.
- Edit shaders with targeted string replacement only — PowerShell `Set-Content` mangles em-dashes.
- The working tree carries UNRELATED uncommitted features (GTAO etc.). Never `git checkout/reset`
  anything; additive edits only; do not commit — the user commits.
- **Never write into `models/`, `import_staging/`, or `data/` on your own initiative** — every
  asset write (including "just a test bake") is a USER GATE: present the exact command, let the
  user run or approve it.
- Standard capture camera (wind_test): `--cam-pos=-59.50,4.25,52.73
  --cam-rot=-0.0392,0.9604,-0.1793,-0.2098`. The banding-repro camera must come from the user (S0).

---

## S0 — Baselines (no code changes)

1. Ask the user for a camera position where the shadow banding is visible; record it HERE in this
   doc. Take a baseline shot there (`--shot=... --shot-delay=12 --wind-freeze`).
2. Interleaved baseline traces ×2 at the standard camera (recipe above). Expected ballpark:
   `Pass_VsmPageRender` ≈ 0.92 ms.
3. Confirm F2 still holds (`--set=vsm.logPageStats:1`, read `logs/vsm_pages.log`) — if the level
   was re-tuned, re-derive the resident counts and update F2/F3.

**Done when:** banding repro shot + baseline trace numbers are recorded in this section.

## S1 — Bake-side chunking (dormant: no asset changes yet)

**Touch:** `MeshManager.{h,cpp}` (BakeToBinary / BuildLodsCpu path), mesh.json option parsing,
the `--reimport-*` CLI (main.cpp/App.cpp) — plus `test_cube.vcxproj` AND `.filters` if any new
file is added (C++ paths use forward slashes; filters use backslashes).

1. `MeshLoadOptions` gains `uint32_t chunkGrid = 0` (0 = off). Parse from mesh.json; hash into
   `HashOptions` **only when non-default** (mirrors `lodRatioScale`). Add a `--reimport-chunk=N`
   CLI flag; the bake options MUST match what mesh.json will carry at runtime (same rule as
   `recomputeNormalSlots` / the wantCW lesson).
2. In the bake, after LOD0 is assembled and BEFORE simplification: reject meshes with >1 submesh
   (log + bake unchunked). Assign LOD0 triangles to grid cells over the XZ AABB by centroid;
   reorder indices so each non-empty cell is one contiguous submesh (materialSlot preserved from
   the original single submesh).
3. Per LOD 1..3: simplify EACH chunk independently over its index subset with
   `opt.lodSimplifyOptions | LockBorder | Sparse | ErrorAbsolute` (per-chunk target = chunk share
   of the LOD's global target; keep the existing error ladder). Concatenate chunk results into the
   LOD's index buffer + submesh table. All LODs must emit the SAME submesh count in the same cell
   order (a chunk that simplifies to zero still emits a zero-count submesh so slot ordinals stay
   aligned across LODs — slot ↔ chunk identity is positional).
4. Offline verification WITHOUT touching models/: bake the island to the session scratchpad
   (`--reimport-src=import_staging/atoll_island/atoll_island.obj --reimport-out=<scratchpad>/...
   --reimport-chunk=6` + whatever recompute flags `models/atoll_island.mesh.json` implies — check
   it), then python-parse the .bin (header 32 B; verts 52 B each; per LOD: u32 ic, u32 sc,
   indices, submeshes 12 B) and assert:
   - LOD0 total tris == 82,944 and equals the sum over submeshes;
   - submesh count == non-empty cells (expect ~30–36), identical across LODs;
   - per-chunk LOD1..3 ratios: record them. **If LOD2/LOD3 stall near the locked-border floor
     (chunk barely shrinks), note it for the S6 decision — do not block.**
   - border integrity: for each pair of adjacent chunks, the set of border vertex POSITIONS used
     at each LOD is identical on both sides (script it; this is the crack check, cheaper and
     stricter than eyeballing).

**Done when:** scratch bake passes all asserts; runtime behavior byte-identical (option is off
everywhere; both configs build).

## S2 — Per-submesh caster bounds + independent slots (engine, dormant)

**Touch:** `Mesh.{h,cpp}`, `MeshManager.cpp` (load path), `ShadowGpuData.cpp` (Rebuild + the
mover path near line 1060), `ShadowGpuData.h`.

1. At mesh load (both `.bin` and parsed paths), when the mesh is flagged chunked (`chunkGrid > 0`
   from mesh.json): compute per-submesh LOD0 AABBs (one pass over indices/verts, mesh-local).
   Store on Mesh with an accessor + a `IsChunkedSubmeshes()` bool.
2. `ShadowGpuData::Rebuild`: for a caster whose mesh is chunked — per slot, build the world AABB
   from the mesh-local submesh AABB × model matrix (conservative transform: center via full
   matrix, extents via abs of the 3×3 — same math as `shadow_gi_scatter_cs.hlsl`), instead of
   copying the object AABB. Emit `CasterMeta` slot-count **1** for each such slot (each chunk
   leads itself). Everything else (groups, mega, masks, windFoliage patching) is untouched.
   Non-chunked meshes keep B3 exactly as-is.
3. Same per-slot bounds in the `UpdateForFrame` mover path (the island is static, but do not
   leave a path that would silently regress if a chunked mesh ever moves).
4. Verification (still no asset changes — dormant): both configs build; wind_test renders
   byte-identical (`--shot` vs S0 baseline); one Debug `--scene-stress=24` run (caster-set code
   touched → cheap insurance; the cull validation readback must stay PASS).

**Done when:** dormant-clean: no chunked mesh exists, zero behavior change, gates green.

## S3 — USER GATE: re-bake the island chunked

**This step writes into `models/` — the user runs or explicitly approves each command.**

1. Proposed edit to `models/atoll_island.mesh.json`: add `"chunkGrid": 6` and (if S1 chose to
   route flags via mesh.json) the simplify-flag value. Show the diff to the user first.
2. Proposed bake command (adjust flags to match the mesh.json exactly):

```bash
x64/Release/test_cube.exe --reimport-src=import_staging/atoll_island/atoll_island.obj --reimport-out=models/atoll_island/atoll_island.mesh.bin --reimport-chunk=6
```

3. Verify after the user runs it: boot wind_test —
   - `[ShadowGpuData] rebuilt:` DBWIN line (or the log) shows ~+35 casters and ~56 mesh-groups
     (F4); if groups exceed 64 the mega path degrades — STOP and reconsider the grid;
   - camera image at the standard camera is byte-identical or visually indistinguishable from S0
     (LOD0 is a pure partition of the original triangles);
   - `Pass_VsmPageRender` trace: should ALREADY drop (island no longer drawn into every page even
     at the old curve) — record the number;
   - low-sun shadow pan across dunes: no light-leak hairlines along chunk borders (LockBorder
     doing its job at LOD1+ pages).

**Done when:** numbers + shots recorded here; user is happy with the visual.

## S4 — Per-group shadow LOD bias (terrain −1 near the camera)

**Touch:** `ShadowGpuData.{h,cpp}` (Rebuild: per-group bias table + `perViewGroup_` bake),
`VirtualShadowMap.cpp` (SetupCB fill), `shaders/vsm_page_setup_cs.hlsl` (CB mirror + group loop),
`App.cpp` (one sweep key).

1. Rebuild computes a per-group `lodBias` (int, default 0): **−1 for groups of chunked terrain
   casters** (identify via the mesh's chunked flag; the object's `renderLayer == "Terrain"` is
   the cross-check). Apply it when baking `perViewGroup_` (the Rung-0/Legacy seed):
   `lod = clamp(viewLod_[v] + groupBias[g], 0, cap)` — and store the table for the CB.
2. `vsm_page_setup_cs.hlsl`: add the packed per-group bias to SetupCB (64 groups; pack 4-per-uint
   like gViewLod) **keeping the CPU struct and HLSL cbuffer in lockstep** (the CB is a mirror —
   repack explicitly, verify offsets of the arrays that follow). In the group loop, the LOD used
   for `gGroupLodMega[g2 * gNumLods + lod]` becomes the biased, clamped one.
   Both the loop path and compact path go through the same code — one change site.
3. Add sweep key `vsm.chunkLodBias` (default −1, 0 = off) so the A/B lives inside ONE binary.
4. Verification:
   - **Invariant with a known answer** (do not trust "looks right"): with bias 0 vs −1, the L0
     ring's terrain shadow texel content must change (difference-image two shots at the standard
     camera); with the island un-chunked this key must be a no-op.
   - Banding: the S0 repro camera, bias −1 — the patches inside the ~12 m ring must be GONE
     (caster == receiver geometry there). Difference-image against S0.
   - Perf: interleaved traces ×2, `vsm.chunkLodBias:0` vs `-1` — expect the −1 side slightly
     more expensive than 0 but far below the S0 baseline (F3 model: ~0.4 M extra tris in L0).
   - One Debug `--scene-stress=24` + GBV run (CB layout changed).
   - Legacy sanity: `--shadow-mode=legacy` boots and shadows render (uniform LOD there, expected).

**Done when:** banding gone at the repro camera, perf table recorded here, gates green.

## S5 — Closing verification + docs

1. Full before/after table in this doc: S0 baseline vs final (`Pass_VsmPageRender`,
   `VsmPageRender.Scatter`, `Pass_Compose` control, HUD frame time), banding shots before/after.
2. The shadow-LOD-bias slider round trip (`--sweep=vsm.shadowLodBias:1,0,1,1`) must still be
   hysteresis-free (the bug this all started from).
3. Update `docs/bug_shadow_lod_bias_perf.md`'s follow-up section (chunking supersedes the "island
   × pages" note) and the memory files (`shadow-lod-bias`, new `terrain-chunking` entry).

## S6 — CONTINGENCY: skirts instead of border locks

Only if S1's ratio report or S3's visuals show LockBorder failing (coarse LODs stuck at the
border floor, or leaks anyway): per chunk, after free simplification (no LockBorder), emit a
vertical skirt strip along the chunk's border edges (extrude down by the LOD's max simplification
error). Skirts are shadow-only in practice (F7: the camera never sees island LOD ≥ 1). Costs a
day; changes only the bake step (S1.3) — the engine side is oblivious.
