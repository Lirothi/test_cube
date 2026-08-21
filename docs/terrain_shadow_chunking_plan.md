# Terrain shadow chunking — execution plan

**Status: S0-S4 DONE (2026-08-21). The island ships chunked; `Pass_VsmPageRender` 0.92 -> 0.73 ms
(-21 %) at the shipped default, 0.67 ms (-27 %) from chunking alone. The ONE thing still open is the
banding verdict, which needs a repro camera from the user — see the end of S4.**

Written 2026-08-20 after the investigation in `docs/bug_shadow_lod_bias_perf.md` (read it
first — the measurement methodology and the harness keys used below were built there).

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
- Never `git checkout/reset` anything; additive edits only; do not commit — the user commits.
  (The tree WAS clean at the start of the 2026-08-21 session; the GTAO work this line warned about
  is committed as of `b8abdce`.)
- `models/**` is GITIGNORED except `*.mesh.json`. A re-baked `.mesh.bin` cannot be recovered with
  git — take a copy first (see S3's rollback note).
- **Never write into `models/`, `import_staging/`, or `data/` on your own initiative** — every
  asset write (including "just a test bake") is a USER GATE: present the exact command, let the
  user run or approve it.
- Standard capture camera (wind_test): `--cam-pos=-59.50,4.25,52.73
  --cam-rot=-0.0392,0.9604,-0.1793,-0.2098`. The banding-repro camera must come from the user (S0).

---

## S0 — Baselines — DONE 2026-08-21 (except the banding camera: still OWED BY THE USER)

1. **Banding repro camera: STILL MISSING.** S4's banding verdict cannot be pronounced without it;
   everything else proceeded. Ask again before S4's verification.
2. **Baseline traces (medians, first quarter dropped). `Pass_VsmPageRender` = 0.92 ms**, five
   samples on the S1+S2+S4-dormant binary:

   | GPU scope | r1 | r2 | r3 | r4 | r5 |
   |---|---|---|---|---|---|
   | `Pass_VsmPageRender` | 0.921 | 0.916 | 0.924 | 0.919 | 0.929 |
   | `VsmPageRender.Scatter` | 0.098 | 0.099 | 0.097 | 0.097 | 0.097 |
   | `VsmPageRender.Setup` | 0.010 | 0.011 | 0.011 | 0.011 | 0.010 |
   | `Pass_Compose` (control) | 0.030 | 0.029 | 0.030 | 0.031 | 0.030 |
   | `Pass_Tonemap` (= DLSS) | 0.383 | 0.383 | 0.385 | 0.382 | 0.384 |

   Run-to-run spread ±0.7 %, so the plan's "expected ≈ 0.92 ms" is confirmed and a real change will
   be obvious against it.

   **One trap, recorded because it cost half an hour and would cost it again.** An intermediate pair
   of runs read **1.144 / 1.167** on the same source — a 25 % outlier that briefly looked like the
   `b8abdce "vsm fixes"` commit having made VsmPageRender more expensive. It did not: five later
   samples (above) and the two earliest samples of the session all sit at 0.92, and `Pass_Compose` /
   `Pass_Tonemap` were IDENTICAL in the outlier pair, so it was not a GPU clock state either. Cause
   unknown, environment-side. The rule that follows: **two samples is not a measurement of this
   scope. Take five, and check that the control scopes did not move.**
3. **A separate, real cross-binary difference — check the exe's date before trusting a baseline.**
   The session's first runs used `x64/Release/test_cube.exe` as it sat on disk: built 00:13, i.e.
   from the PARENT of HEAD (`b8abdce` was committed 00:29). Its screenshot differs from a
   post-rebuild one by 1.8 % mean luminance over 99.9 % of pixels — which looks exactly like "my
   dormant change is not dormant" until `git log -1 --format=%cd` is compared with the exe's mtime.
   That commit's `vsm_sample.hlsli` receiver-plane bias changes the image; it does not change this
   pass's cost.
4. **F2 re-confirmed** on the current build: `resident=360`, `clip=[111 103 85 49 11 1 0 0]`
   (plan said 112/104/85/50/11/1 — unchanged within sampling jitter). F3's ~8.3 M tris/frame model
   stands.
5. **F4 re-confirmed and now READABLE HEADLESS.** `ShadowGpuData::Rebuild`'s summary and the cull
   validation verdict were `OutputDebugStringA`-only, i.e. invisible to exactly the headless runs
   that gate this work; both now also append to `logs/shadow_casters.log`. Pre-chunking wind_test:

   ```
   [ShadowGpuData] rebuilt: 2673 casters (2673 static + 0 GI in 0 objs), 21 mesh-groups
                            (0 chunked of 9 meshes, cap 64); ...
   [ShadowGpuData] cull validation PASS: 44 views match CPU (2673 casters, 21 groups).
   ```

   S3 must turn this into **2708 casters / 56 groups / 1 chunked of 9 meshes**.

### Capture protocol for every image comparison from here on

The standard `--shot` recipe is **not** usable as an A/B reference: DLSS + auto-exposure put its
run-to-run noise floor at |mean channel delta| ≈ **0.56** over 63 % of pixels, which is the same
order as a real shadow change. Adding `--dlss=off --set=exposure.autoExposure:0` drops the floor to
**0.024** over 1.5 % of pixels — 20x quieter, and stable across rebuilds:

```bash
x64/Release/test_cube.exe --level=data/levels/wind_test.json --wind-freeze --cam-pos=-59.50,4.25,52.73 --cam-rot=-0.0392,0.9604,-0.1793,-0.2098 --vsm-lodbias=1 --dlss=off --set=exposure.autoExposure:0 --shot=<dir>/ref.png --shot-delay=12
```

(Exposure drift was ruled out as a cause separately: `--shot-delay=30` reproduces `--shot-delay=12`
to 0.008 % of mean luminance, so 12 s is fully converged.) Perf traces stay on the DEFAULT path
(DLSS on) — that is the configuration being optimized.

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

### S1 RESULT — DONE 2026-08-21

Code: `MeshLoadOptions::chunkGrid`; `ChunkifyLod0` + the chunk-aware flag block in `BuildLodsCpu`
(`MeshManager.cpp`); `HashOptions`; `--reimport-chunk=N` (`main.cpp`). Both configs build.

**Editor UI (added 2026-08-21 on the user's request).** A *Shadow chunking* section in the mesh
import dialog: on/off + tiles-per-axis (2..8), a live readout of tile size in metres and of how many
of the level's shadow caster groups the grid would spend, and a greyed-out state with a stated reason
for the two configurations the bake rejects (multi-submesh source, split-by-node import). Plumbing:
`ImportPanel::RecreateMeshAssets`/`WriteImportedMeshAsset` take an explicit grid, `-1` meaning "no
choice was made — keep what mesh.json has" (the same convention `activeMeshSpawnScale_` already used
for bulk/per-resource re-imports), and `0` meaning an explicit "off" that ERASES the key.

Three things that had to be true for the control not to lie:
* **One value drives both the bake and mesh.json.** They are resolved together in
  `WriteImportedMeshAsset` — a .bin baked chunked while mesh.json says nothing is the exact failure
  this feature cannot survive (the runtime would treat 36 tiles as one caster with the object's box).
* **Off must erase.** Clearing the checkbox passes 0, not -1.
* `vsm::kMaxMeshGroups` was hoisted into `VirtualShadowMap.h` so the number the dialog quotes is the
  engine's, not a fourth copy of the literal 64 (`ShadowGpuData.cpp` had two, `VirtualShadowMap.cpp`
  one; `VSM_MAX_SETUP_GROUPS` in the shader remains the copy that must be kept in step by hand).

Also corrected while in there: the *Recreate mesh JSON only* tooltip claimed no binary is rewritten.
It re-bakes the `.mesh.bin` — which is precisely why it is the right button for a chunk-grid change.

**Control first (the thing that makes the rest of the numbers mean anything):** the island re-baked
UNCHUNKED with the edited code is **byte-identical from offset 32** to the committed
`models/atoll_island/atoll_island.mesh.bin` — verts, all 4 LOD index buffers, all submesh tables.
(Only `optionsHash` in the header differs, and it always did: the committed file was baked through
the editor's option set. `LoadBinaryDirect` does not check it — by design.) So the unchunked path is
provably untouched, and a chunked bake can be trusted to show only what chunking did.

**Scratch bake, `--reimport-chunk=6` (asserts from the step above, all PASS):**

| | LOD0 | LOD1 | LOD2 | LOD3 |
|---|---|---|---|---|
| unchunked tris | 82,944 | 41,471 | 20,736 | 9,952 |
| chunked tris | 82,944 | 41,450 | 20,734 | **10,150** |
| submeshes | 36 | 36 | 36 | 36 |

* LOD0 is a **pure partition**: the multiset of triangles equals the original's, winding and
  per-triangle vertex order preserved, submesh ranges contiguous and summing to the total. So the
  camera image cannot change — the gbuffer draws one whole-buffer draw either way (the island's
  `matDatas_.size()==1`, so `MultiSlotDraw()` stays false).
* All 36 cells are non-empty; submesh count and cell order identical at every LOD.
* Per-chunk ratios land on the targets (0.500 / 0.250 / 0.120) for every chunk above ~500 tris.
  Only the four AABB-corner slivers (3, 4, 32, 85 tris) stall, which is why LOD3 totals **+2.0 %**
  instead of matching. **LockBorder did NOT stall the chain** — the S6 skirt contingency is not
  needed. The chain still produces 4 LODs.
* Chunk sizes are very uneven (min 3, max 7,433 tris) because the island is a disc inside a square
  AABB. Costs 4 groups on near-empty corners; harmless at 56/64, worth revisiting only if a finer
  grid is ever wanted.

**Border integrity (the crack check), scripted rather than eyeballed:** for each LOD, take every
chunk's set of open (used-once) edges as quantised position pairs.

| LOD | seam edges shared by 2 chunks | one-sided edges | **cracks** | seam drift vs LOD0 |
|---|---|---|---|---|
| 0 | 2515 | 576 | **0** | — |
| 1 | 2515 | 576 | **0** | lost 0, added 0 |
| 2 | 2515 | 576 | **0** | lost 0, added 0 |
| 3 | 2515 | 576 | **0** | lost 0, added 0 |

The 576 one-sided edges are *exactly* the island's own outer silhouette (LockBorder pins that too),
so a "crack" is defined as a one-sided edge that is NOT on the rim — and there are none, at any LOD.
The seam is bit-for-bit the LOD0 seam at every level.

**One judgement call worth knowing about:** `meshopt_SimplifySparse` re-bases the relative error onto
each SUBSET's extent (~60 m per chunk instead of the island's 388 m), which would have silently
tightened the error budget ~6x. The code therefore also passes `meshopt_SimplifyErrorAbsolute` and
multiplies `errors[]` by `meshopt_simplifyScale` over the WHOLE mesh, so the budget keeps exactly
the meaning it has for every other asset: a fraction of the whole mesh's largest extent.

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

### S2 RESULT — DONE 2026-08-21

Code: `Mesh::MarkChunkedSubmeshes` / `IsChunkedSubmeshes` / `GetSubmeshBounds` (`Mesh.{h,cpp}`);
`LoadBinaryDirect` takes the options and marks the mesh (`MeshManager.cpp`), `MeshCacheKey` now
separates grids; `StaticMesh::SetChunkGrid` + the mesh.json `chunkGrid` read in
`SceneObjectFactory`; `FillChunkBounds` + the per-slot bounds, the `casterMeta` slot-count and the
mover path in `ShadowGpuData.cpp`.

Two decisions worth restating because they are where this could go quietly wrong:

* **`casterMeta` slot count is 1 for EVERY slot of a chunked mesh**, not "N on the first slot". The
  VSM cull tests bounds once per *object* and hands the verdict to that object's whole slot run —
  which is precisely the behaviour chunking exists to defeat. Each chunk must lead itself.
* **Per-slot bounds go through `AABB::Transform`**, the same conservative 8-corner transform the
  object bounds already use, so chunk boxes and object boxes come out of one piece of math. A slot
  whose local box is missing/degenerate falls back to the object's box: conservative, and it degrades
  to today's cost rather than to a wrong image.

Gates: both configs build; Debug `--scene-stress=24` → `verdict: CLEAN after 24 iterations`
(barriers 8818 enhanced / 0 legacy); wind_test boot → `cull validation PASS: 44 views match CPU
(2673 casters, 21 groups)`, `0 chunked of 9 meshes`, caster/group counts unchanged from S0. Nothing
in the level is chunked yet, so every new branch is provably unreached.

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

### S3 — READY TO FIRE, WAITING ON THE USER (2026-08-21)

Everything else is in place; this is the only step that writes an asset. Two routes, same result.

**Route A — the mesh import dialog (preferred).** Debug build → content browser → *Import Assets* →
`atoll_island` → **Shadow chunking** → tick *Split into shadow chunks*, *Tiles per axis* = 6 →
**Recreate mesh JSON only**. That button re-bakes `models/atoll_island/atoll_island.mesh.bin` AND
writes `"chunkGrid": 6` into `models/atoll_island.mesh.json` from the same value, which is the point:
the .bin carries no chunk flag of its own, so mesh.json is the only thing telling the runtime those
submeshes are spatial tiles, and the two must never be set separately. It also leaves the asset's
`material` / `texOffsScale` / `renderLayer` alone (the writer starts from the existing file) and does
not re-convert textures or touch the staging folder.

The dialog greys chunking out and says why for the two cases the bake would reject: a multi-submesh
source, or *Split by top-level nodes* being on. Clearing the checkbox ERASES `chunkGrid` from
mesh.json — the control can be turned off, not just on.

**Route B — the headless CLI.** Two steps, in order; the mesh.json edit is NOT optional (without it
the runtime sees 36 submeshes of one caster, i.e. today's cost plus 36 draw ranges).

1. Add one line to `models/atoll_island.mesh.json` (the only tracked file this touches — a plain
   `git checkout` reverts it):

```json
  "chunkGrid": 6,
```

2. Re-bake. The mesh.json carries no `recomputeNormalSlots`, no `windFoliage`, no `bakeScale` and no
   LOD overrides, so the bake needs no other flags:

```bash
x64/Release/test_cube.exe --reimport-src=import_staging/atoll_island/atoll_island.obj --reimport-out=models/atoll_island/atoll_island.mesh.bin --reimport-chunk=6
```

**Rollback.** `models/**` is gitignored, so the .mesh.bin is NOT under version control and `git
checkout` will not bring it back. Two ways back, either is exact:

* re-run the same command WITHOUT `--reimport-chunk=6` — verified byte-reproducible this session
  (an unchunked re-bake matched the committed file exactly from offset 32; only the header's
  `optionsHash`, which nothing reads on this path, differs);
* or restore the byte-identical copy taken before any of this:
  `<scratchpad>/BACKUP_atoll_island.mesh.bin`.

### S3 RESULT — FIRED BY THE USER, VERIFIED 2026-08-21

The user re-imported through the dialog. Everything predicted came out exactly:

* `models/atoll_island.mesh.json` gained **only** `"chunkGrid": 6` — `material`, `texOffsScale`,
  `renderLayer` and `source` all preserved.
* The shipped `.mesh.bin` is **byte-identical from offset 32 to the scratch bake validated in S1**
  (identical `optionsHash` too), so S1's proofs — LOD0 a pure partition, zero cracks at every LOD —
  apply verbatim to the shipped asset. 36 submeshes; 82,944 / 41,450 / 20,734 / 10,150 tris.
* `logs/shadow_casters.log`, exactly the predicted numbers:

  ```
  [ShadowGpuData] rebuilt: 2708 casters (2708 static + 0 GI in 0 objs), 56 mesh-groups
                           (1 chunked of 9 meshes, cap 64); ...
  [ShadowGpuData] cull validation PASS: 44 views match CPU (2708 casters, 56 groups).
  ```

  2673 → 2708 casters (+35), 21 → 56 groups (+35), 8 groups of headroom under the cap.
* Resident pages unchanged (`resident=360`, `clip=[111 103 85 49 11 1]`) — as expected, caster bounds
  never reach the request/alloc path.
* Camera image vs the pre-chunk reference: |mean| **0.039** against a 0.024–0.036 noise floor, i.e.
  indistinguishable. (It has to be: LOD0 is a pure partition, and the island draws as ONE
  whole-buffer draw in the gbuffer regardless — `MultiSlotDraw()` is false at one material slot.)

**What to check after it runs** (recipes above; the first two are hard numbers, not opinions):

* `logs/shadow_casters.log` must read **2708 casters, 56 mesh-groups, 1 chunked of 9 meshes**
  (from 2673 / 21 / 0) and still `cull validation PASS`. If groups exceed 64 the mega path degrades
  — stop and drop the grid.
* `Pass_VsmPageRender` should fall from **0.92 ms** even before the S4 bias is exercised, because
  the island stops being drawn into every page it covers.
* Camera image at the standard camera: indistinguishable (LOD0 is a pure partition — proven in S1).
* Then S4's A/B: `--set=vsm.chunkLodBias:0` vs `-1`, difference-imaged, plus the banding camera.

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

### S4 STATUS — code DONE and DORMANT-VERIFIED 2026-08-21; the banding + perf verdicts wait on S3

Built ahead of S3 on purpose: none of it depends on the asset, and all of it is provably inert
while no chunked mesh is loaded (every bias entry is 0, so the biased LOD *is* the view LOD).

Code: `render::g_chunkShadowLodBias` (`LodSelect.h`, default −1); the per-group bias table +
`biasedLod()` helper folded into the `perViewGroup_` bake and exposed as `GroupLodBias()`
(`ShadowGpuData.{h,cpp}`); `SetupCB::groupLodBias` + its fill (`VirtualShadowMap.cpp`);
`int4 gGroupLodBias[KGROUP_BIAS_VEC4]` + the per-group LOD in the group loop
(`vsm_page_setup_cs.hlsl`); `--set/--sweep=vsm.chunkLodBias` (`App.cpp`);
`Scene::ReconcileShadowLodBias` now polls both biases.

* **The CB array is APPENDED AFTER `gGroupLodMega`**, so every existing array keeps its offset.
  The CPU struct and the HLSL cbuffer were edited as one change.
* **The biased LOD comes out of ONE helper** (`biasedLod`) shared by the `perViewGroup_` bake and
  the CB the setup CS reads. Two sites computing "the same" LOD is how a page ends up drawing one
  LOD's triangle count from another LOD's index range.
* The over-cap group path (`g2 >= VSM_MAX_SETUP_GROUPS`) does NOT index the bias array — it reads
  `Rung0Args`, which the CPU already baked the same bias into.

Dormant verification (all against the deterministic capture protocol above, noise floor 0.024):

| check | result |
|---|---|
| bias −1 (default) vs the pre-S4 reference | \|mean\| 0.024 — at the floor |
| bias −1 vs `--set=vsm.chunkLodBias:0` | \|mean\| 0.032 — at the floor (no-op, as required) |
| `Pass_VsmPageRender` | 0.921 / 0.916 / 0.924 / 0.919 / 0.929 (unchanged) |
| shaders | `python tools/check_shaders.py` 45/45, and the app RUNS (a dxc pass is not a PSO) |
| Debug `--scene-stress=24 --scene-stress-gbv` | `verdict: CLEAN after 24 iterations` |
| cull validation | `PASS: 44 views match CPU (2673 casters, 21 groups)` |
| `--shadow-mode=legacy` | boots, renders, shadows present (differs from VSM by 2.17 mean, expected) |

Tooling added on the way: **the 12 GPU-driven shadow compute shaders are now in
`tools/check_shaders.py`** (the whole set was missing while this work was editing the setup CS's
constant buffer — precisely the change class that tool exists to catch).

### S4 MEASURED ON THE CHUNKED ISLAND — 2026-08-21

Interleaved ×2, medians, first quarter dropped. Pre-chunk baseline for the same scope: **0.92 ms**
(five samples, ±0.7 %).

| GPU scope | pre-chunk | chunked, bias 0 | chunked, bias −1 (shipped) | chunked, bias +3 |
|---|---|---|---|---|
| `Pass_VsmPageRender` | 0.921 / 0.916 / 0.924 / 0.919 / 0.929 | **0.663 / 0.682** | **0.736 / 0.725** | 0.636 / 0.636 |
| `VsmPageRender.Scatter` | 0.097 | 0.106 | 0.107 / 0.106 | 0.107 / 0.106 |
| `VsmPageRender.Setup` | 0.010 | 0.022 / 0.017 | 0.023 / 0.018 | 0.018 / 0.021 |
| `Pass_Compose` (control) | 0.030 | 0.030 | 0.031 | 0.030 |
| `Pass_Tonemap` (control) | 0.384 | 0.389 / 0.384 | 0.389 / 0.386 | 0.389 / 0.387 |

* **Chunking alone (bias 0, i.e. the same LOD curve the baseline ran): 0.92 → 0.67 ms, −27 %.**
* **Shipped default (bias −1, terrain casting one LOD finer near the camera): 0.92 → 0.73 ms, −21 %**
  — the terrain gets FINER shadow geometry and the pass is still a fifth cheaper than before.
* Sub-scopes moved the way more casters must move them and stay negligible: `Scatter` +0.009 ms
  (2708 boxes instead of 2673, 36 of them independent), `Setup` +0.009 ms (56 groups instead of 21).
* Whole GPU frame drifts the same way (2.39/2.43 → 2.14/2.14 at bias 0) but its run-to-run spread is
  the size of the effect, so quote the pass, not the frame.

**F3's model over-attributed.** It put ~8.3 M tris/frame on the island and implied the pass was
mostly that. Driving the terrain to its COARSEST LOD (bias +3, 10 k tris) only reaches 0.636 ms, so
terrain geometry is worth **at most ~0.29 ms** of the pass and the remaining ~0.64 ms is palms plus
fixed per-page cost. Chunking captured most of the terrain share; it was never going to be 12x.

**The bias reaches the GPU — proven by cost, not by looking.** bias −1 → 0 → +3 gives
0.73 → 0.67 → 0.64 ms, monotone, reproduced twice, with both control scopes flat. Finer costs more,
coarser costs less; nothing else produces that ordering.

**It changes NOTHING in the image at the standard camera, and that is correct.** Even the extreme
bias +3 moves 0.1 % of pixels at |mean| 0.011 — below the noise floor. The reason is visible the
moment you look at the shot instead of the metric: the standard camera frames a FLAT stretch of
beach. Every shadow in it is cast by palms, which are not chunked; the terrain casts no self-shadow
there at all, so its caster LOD cannot matter. **This is exactly the trap of judging a shadow change
from a camera that contains none of that shadow.**

**Still owed: the banding repro camera (S0.1).** The banding is terrain-shadowing-terrain, so it
needs dunes at a low sun — the one thing the standard camera does not contain. Until it arrives, the
banding claim is UNVERIFIED; the perf result above stands on its own.

### S5 RESULT — closing gates, all green 2026-08-21

* **The bug this whole line of work started from is still fixed.** HUD round trip in ONE process,
  `--sweep=vsm.shadowLodBias:1,0,1,1`: **2.66 / 2.77 / 2.66 / 2.63 ms** — the excursion to bias 0
  costs and gives it all back, no hysteresis. (Pre-chunk the same round trip read 2.79 → 2.78, so the
  whole frame is ~0.13 ms cheaper now too.)
* **Legacy CSM** (`--shadow-mode=legacy`) renders correctly on the chunked island — no cracks, no
  holes, shadows intact. It now issues 36 ranged draws per cascade instead of one
  (`RenderableObject::RenderShadow` loops submeshes when there is more than one); that is CPU cost in
  the non-default path, and it is the documented divergence, not a defect.
* **Debug `--scene-stress=24`: `verdict: CLEAN after 24 iterations`** (barriers 8818 enhanced / 0
  legacy). The log shows the chunked island surviving level switches — `1 chunked of 2 meshes` on the
  small level, back to `1 chunked of 9` on wind_test.
* **RT** picks the chunked mesh up for free: the BLAS builds one geometry desc per submesh and the
  bindless table one record per submesh, both pre-existing paths (`AccelerationStructure.cpp:84`,
  `BindlessTable.cpp:126`). 36 descs instead of 1, negligible.

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
