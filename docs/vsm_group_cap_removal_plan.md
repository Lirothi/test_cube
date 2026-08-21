# Removing the VSM 64-mesh-group cap — execution plan

**Status: NOT STARTED.** Written 2026-08-21, immediately after terrain chunking took wind_test from
21 to 56 groups (see `docs/terrain_shadow_chunking_plan.md`) and left 8 of headroom. Every claim
below was re-verified against the code and against the UE drop on the day of writing; file:line
references are to the tree as of that session (uncommitted chunking work included).

**The verdict up front:** the cap is not a property of the per-page-culling approach — Unreal's
non-Nanite VSM path does the same job with no compile-time group limit at all. Our 64 exists only
because the per-group prefix-sum state lives in two fixed local arrays inside the per-page setup
thread. Everything else in the pipeline is already sized by the real group count, including the CPU
tables — only the *transport* truncates.

---

## Facts — verified, do NOT re-derive

- **F1. The only fixed-size group state in the whole engine** is in `vsm_page_setup_cs.hlsl`:

  ```
  uint perGroupCount[VSM_MAX_SETUP_GROUPS];   // :183
  uint perGroupBase [VSM_MAX_SETUP_GROUPS];   // :246
  ```

  plus the CB tables `gGroupLodMega[64*4]` and `gGroupLodBias[16]` (int4-packed 64). Every loop over
  those arrays runs to `gNumGroups`, which the CPU fills **unclamped** from
  `shadowGpu->MeshGroupCount()` (`VirtualShadowMap.cpp:1136`).
- **F2. What actually breaks at 65 groups — and it is NOT soft.** The chain:
  1. `ShadowGpuData::Rebuild` refuses to build the mega buffer (`numMeshGroups_ > kMaxMegaGroups` →
     warning, `MegaReady()==false`, ShadowGpuData.cpp ~911).
  2. `useMega=false` kills `singleDraw` and `compactArgs` with it (`VirtualShadowMap.cpp:461-466`) →
     the draw side falls back to the per-page loop binding each mesh's own IB at the flat LOD
     (`VirtualShadowMap.cpp:1374-1421`). That fallback is *correct by itself*.
  3. **But the setup CS still runs with `gNumGroups > 64`** and indexes both local arrays out of
     bounds — the scattered branch's read loop (`perGroupCount[gs0]` for `gs0 >= 64` is an OOB
     write), the brute-force count (`perGroupCount[g] += 1` guarded only by `g < gNumGroups`,
     :211), the prefix sum, and the pass-2 scatter cursor. Local-array OOB is genuine shader UB —
     indexable temps live in the thread's scratch, so the write can corrupt `planes[]`, the page
     matrix, anything. Garbage `InstanceCount` / visible-list indices follow.

  So today the 65th group does not degrade the frame — it hands the whole VSM path undefined
  behavior. The "mega path disabled" warning reads like a soft fallback; it is not one.
- **F3. Everything else already scales.** Verified consumers with no fixed arrays and real-sized
  buffers: `shadow_cull_cs` / `shadow_cull_clear_cs` (stride `v * gNumGroups + g` into UAVs),
  `vsm_page_scatter_cs` (appends via `InterlockedAdd` into `PageGroupCount[p * gNumGroups + g]` and
  `PageVisibleList` at `PerGroup[g].x` — no local group state at all),
  `RecordIndirectShadowDraws` (Legacy CSM + Rung-0: loops `numMeshGroups_`, binds per-mesh IBs),
  and the buffers `pageDrawArgs_` (pool × groups × 5, grows with groups), `pageGroupCount_`,
  `perViewGroup_`, `casterGroup_`, `indirectArgs_`. `pageVisibleList_` scales with *casters*, not
  groups. **Legacy CSM at >64 groups is genuinely fine.**
- **F4. The CPU tables are ALREADY full-size.** `groupLodMega_.assign(numMeshGroups_ * kLods * 4)`
  (ShadowGpuData.cpp:831) and `groupLodBias_` (:779) carry every group; only the CB fill clamps at
  `gm = min(groups, kMaxMegaGroups)` (`VirtualShadowMap.cpp:1158`). Removing the cap is a transport
  change (CB → SRV), not a data-structure change.
- **F5. The scattered path needs the local arrays for NOTHING.** In the `scattered` branch (clipmap
  views — all directional pages, i.e. exactly where terrain chunk grids spend groups) each array
  element is written once from a buffer and read once in the args loop:
  `perGroupCount[g2]` ← `PageGroupCount[p*gNumGroups+g2]`, `perGroupBase[g2]` ← `PerGroup[g2].x`,
  and pass 2 is skipped (`if (scattered) return`). Direct buffer reads in the args loop are
  drop-in. Only the **brute-force local-light path** structurally needs per-group state across its
  two passes (count → prefix → scatter cursor).
- **F6. The mega build itself has no 64 dependency.** It iterates `megaCopy_` per unique *mesh*;
  `baseVertex_` / `startIndex_` are sized `numMeshGroups_`. The `<= kMaxMegaGroups` gate exists
  solely because the CB table could not address more groups — it falls with F4's fix.
- **F7. GI folding** stops folding GPU-instanced casters when `numGroups + 1 > 64`
  (ShadowGpuData.cpp ~566): they keep drawing through the CPU `RenderShadow` tail. Designed-soft,
  stays as policy either way.
- **F8. One hand-synced shader constant.** `VSM_MAX_SETUP_GROUPS` (vsm_page_setup_cs.hlsl:21) must
  equal `vsm::kMaxMeshGroups` (VirtualShadowMap.h:117) — the shader cannot include the header. The
  plan shrinks what that number means (see S4) but does not eliminate the pairing.

## What Unreal does (the reference; read before redesigning anything)

Source read 2026-08-21 in `D:/Programming/ue_strip`. The non-Nanite VSM path —
`Shaders/Private/VirtualShadowMaps/VirtualShadowMapBuildPerPageDrawCommands.usf` + its policy side
`Source/Runtime/Renderer/Private/VirtualShadowMaps/VirtualShadowMapArray.cpp` (~3015-3160) — is our
exact problem solved with a different data-flow shape:

1. **`CullPerPageDrawCommandsCs`** — load-balanced over (instance runs × views), walks mip levels,
   and for each visible (instance, page footprint) **appends** an `FVSMVisibleInstanceCmd` to one
   flat list via a wave atomic, while `InterlockedAdd`-ing the command's `InstanceCount` in the
   indirect args. The capacity guard makes an overflowing append return false — nothing corrupts.
2. **`AllocateCommandInstanceOutputSpaceCs`** — one thread per *draw command* (`NumIndirectArgs` is
   a plain uniform), allocates each command's output range with a global atomic.
3. **`OutputCommandInstanceListsCs`** — one thread per visible instance, scatters ids into those
   ranges through a cursor buffer.

**No compile-time group count exists anywhere in that chain.** Our per-thread prefix sum is their
dispatch 2. Their only cap is on visible (instance, page) pairs —
`MaxNumInstancesPerPass = Clamp(scaled, 1, CVarNonNaniteMaxCulledInstanceAllocationSize)` — and its
overflow contract is exemplary: the shader raises `VSM_STAT_OVERFLOW_FLAG_VISIBLE_INSTANCES`
(surfaced on screen), drops the write, and the CPU `ensureMsgf` names the two cvars to adjust.
Bounded, checked, reported, tunable.

**Nanite is not the answer to this question**: `RenderVirtualShadowMapsNanite`
(VirtualShadowMapArray.cpp:3267) rasterizes clusters in compute (`EOutputBufferMode::DepthOnly`) —
no draw commands, so no groups, so no cap *for Nanite content*. The path above exists because UE
still needs draw commands for everything else. The cap-free design is in the non-Nanite half.

## Locked design decisions

- **The cap is removed for the directional (clipmap) path and the single-draw path** — that is
  where terrain chunking and any future submesh growth spend groups. The local-light brute-force
  path keeps a *deterministic, clamped* 64 until S5, which is optional and separately priced.
- **No "just raise it to 128".** Any fixed N re-creates the cliff one asset later, and doubling the
  per-thread scratch (2 × N uints per page thread; 32 KB per 8×8 group at N=64 already) in the
  hottest VSM loop is paying occupancy/scratch traffic for a number that will be wrong again.
- The CB group tables move to **structured SRVs** (the CPU vectors are already full-size — F4).
- Over-cap behavior during the interim (after S1, before S5): over-cap groups **cast no shadow from
  local lights** and the rebuild log says so. Deterministic loss beats UB; directional is exact.
- `vsm::kMaxMeshGroups` is retained but its meaning narrows to "local brute-force cull cap + GI
  fold policy". The import dialog's budget text must be rewritten when that happens
  (per the controls-must-not-lie rule) — S6.

## Execution steps

### S1 — Hardening: make >64 deterministic instead of UB (small, do first)

**Touch:** `shaders/vsm_page_setup_cs.hlsl` only.

1. Clamp every local-array access to the compile-time size; keep loops over `gNumGroups` where they
   write real-sized *buffers*:
   - scattered read loop: skip `gs0 >= VSM_MAX_SETUP_GROUPS` (args loop reads the buffer for the
     rest — or simply fold into 2 below);
   - brute-force count: `if (g < VSM_MAX_SETUP_GROUPS) perGroupCount[g] += 1u;`
   - prefix sum + pass-2 cursor: iterate `min(gNumGroups, VSM_MAX_SETUP_GROUPS)`.
2. The args loop stays at `g2 < gNumGroups` (the args buffer is real-sized and MUST be fully
   written each frame — a skipped tail leaves stale garbage for the loop-path ExecuteIndirect), but
   over-cap groups emit **zero-instance args**:
   `count = (g2 < VSM_MAX_SETUP_GROUPS) ? perGroupCount[g2] : 0u;` etc.
3. Verification: dxc (`tools/check_shaders.py`), boot, same-binary A/B at 56 groups against the
   deterministic capture protocol (must be at the noise floor), one Debug GBV run. No stress needed
   beyond that — indexing inside existing resources (gate-discipline rule).

**Done when:** ≤64 behavior provably unchanged; the 65th group can no longer corrupt a thread.

### S2 — Scattered path: delete the local arrays (directional cap gone)

**Touch:** `shaders/vsm_page_setup_cs.hlsl`.

Per F5 this is a pure de-caching: in the `scattered` branch drop both array fills and read
`PageGroupCount[p * gNumGroups + g2]` / `PerGroup[g2].x` directly in the args loop. Declare the
arrays only in the brute-force scope so the scattered path carries zero scratch. Expected perf:
neutral-to-better (`VsmPageRender.Setup` is 0.010–0.023 ms today; each element was read exactly
once anyway).

**Done when:** clipmap pages have no group-count limit; image + perf at 56 groups unchanged
(interleaved ×2, five samples for any scope you quote — see the S0 lesson in the chunking doc).

### S3 — CB tables → SRVs (the transport fix that unblocks everything)

**Touch:** `vsm_page_setup_cs.hlsl` (RS + cbuffer), `VirtualShadowMap.cpp` (SetupCB + dispatch),
`ShadowGpuData.{h,cpp}` (expose the vectors as GPU buffers).

1. New structured SRVs, uploaded from the already-full-size CPU vectors (F4):
   `StructuredBuffer<uint4> GroupLodMega : register(t9)` (numGroups × kLods entries) and
   `StructuredBuffer<int> GroupLodBias : register(t10)` (numGroups). Ring/static buffer rebuilt on
   Rebuild, same as `perGroup_`.
2. RS: `SRV(t0, numDescriptors=9)` → `11`; extend the descriptor list in the
   `RecordComputeDispatch` call. Drop `gGroupLodMega`, `gGroupLodBias`, `KGROUP_BIAS_VEC4` from the
   CB and its CPU mirror **in the same edit** (the CB is a mirror; repack both sides together).
   `gViewLod` stays in the CB — views are fixed at 44, groups are not.
3. The `g2 < VSM_MAX_SETUP_GROUPS ? CB : Rung0Args` fallback branch dies; every group reads the SRV.
4. Verification: **full gate set** — new SRVs + RS change (both configs, Release `--scene-stress`,
   Debug `--scene-stress-gbv`, comparator), plus the A/B image + trace protocol. This is exactly
   the change class `tools/check_shaders.py` cannot vouch for alone: dxc accepting the shader says
   nothing about the PSO or the descriptor table — run it.

**Done when:** no group table lives in a CB; behavior at 56 groups byte-stable.

### S4 — Lift the mega/single-draw gate

**Touch:** `ShadowGpuData.cpp` (the two `<= kMaxMegaGroups` conditions + warning text),
`VirtualShadowMap.h` (`kMaxMeshGroups` comment: now the LOCAL cull cap, not the global budget).

With S3 in place nothing indexes a fixed table, and the mega build itself never depended on 64
(F6). Lifting the gate keeps `MegaReady()` — and therefore `singleDraw` — alive at >64, so the
over-cap configuration is not only correct but stays on the fast path. GI folding keeps its policy
gate (F7). Update the hand-synced pairing note (F8) on both sides.

**Done when:** a >64 level renders directional VSM correctly on the single-draw path, with locals
deterministically capped and logged.

### S4.5 — The >64 proof (USER GATE: needs scratch assets)

There is no committed level with >64 groups. The honest test is the island at `chunkGrid 7`
(49 + 20 = 69 groups) in a scratch copy of wind_test — both writes land under `models/` / `data/`,
so this is a user-run step, same contract as chunking's S3:

```bash
x64/Release/test_cube.exe --reimport-src=import_staging/atoll_island/atoll_island.obj --reimport-out=models/atoll_island/atoll_island.mesh.bin --reimport-chunk=7
```

plus `"chunkGrid": 7` in the mesh.json (or the import dialog at 7 tiles per axis — it will warn at
69/64; after S4 that warning text is stale, which is S6's job). Verify: `logs/shadow_casters.log`
shows 69 groups and **no** "mega path disabled" line; image clean; then restore grid 6 the same
way. Before S1..S4 land, do NOT run a >64 level expecting anything — F2 says what happens.

### S5 — OPTIONAL: local lights join the scatter (full cap removal)

Locals brute-force because a perspective view has no trivial AABB→page-rect mapping
(vsm_page_setup_cs.hlsl:172 comment). UE crosses that bridge in `VirtualShadowMapPageOverlap.ush`
(clip-space AABB footprint per mip). The equivalent here: extend `vsm_page_scatter_cs` (or a
sibling) to walk local views' mip pyramids and append (caster, page) pairs the same way clipmap
levels do — after which every page takes the `scattered` branch and the local arrays are deleted
outright, per the delete-don't-disable rule (the brute-force path is currently also the fallback
when the scatter PSO fails to build; decide its fate then, not now). Price: a real shader-design
step, ~a day plus verification. **Not needed until a level combines local lights with >64 groups**
— today wind_test has zero locals.

### S6 — Truth pass on docs + UI

- Import dialog (`ImportPanel.cpp`, Shadow chunking section): the budget readout currently implies
  a hard level-wide 64. Post-S4 the honest text is "N tiles = N caster groups; directional
  unlimited, local-light shadows cap at 64 groups; more groups = more arg records" — and the
  warn/red thresholds move to whatever S4 leaves meaningful.
- `docs/terrain_shadow_chunking_plan.md` F4 and the group-budget notes; the `kMaxMeshGroups`
  comment block; the memory files.

## Executor environment notes

Everything in `docs/terrain_shadow_chunking_plan.md`'s notes block applies verbatim (build both
configs; MSBuild does not compile shaders — run the app; `Start-Process -Wait`; interleaved traces
with `Pass_Compose` control and FIVE samples per quoted scope; shader edits via targeted string
replacement only; never write assets without the user; `models/**` is gitignored except
`*.mesh.json`). Additional, specific to this plan:

- `logs/shadow_casters.log` (truncate-per-process) is the ground truth for group counts and the
  cull-validation verdict — read it, don't infer.
- The cull validator (`PollValidation`) checks *view-level* counts, not per-page lists — it will
  NOT catch a page-level regression from S1/S2. The page-level check is the image A/B under the
  deterministic capture protocol plus `logs/vsm_pages.log` residency.
- `RecordComputeDispatch` divides both dispatch dims by 8 — the setup CS is `numthreads(8,8,1)`
  over `(kPoolPageCount, 1)`; don't "fix" that while editing.
