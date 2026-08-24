# Removing the VSM 64-mesh-group cap — execution plan

**Status: PLAN COMPLETE 2026-08-24 (uncommitted). S1-S6 ALL DONE — the VSM mesh-group cap is GONE (only a PSO-failure fallback and the GI-fold policy still reference it), proven at 66 groups on wind_test and 70 with local lights on demo; plus two follow-up levers (`Pass_VsmPageRender` -12% on wind_test) and S5's 7.8x on the local-light per-page cull.** Facts re-verified 2026-08-21 20:30 against HEAD `0dd0aba`. Written earlier
the same day, right after terrain chunking took wind_test from 21 to 56 groups (see
`docs/terrain_shadow_chunking_plan.md`).

**The chunked-terrain LOD feature was REWRITTEN under this plan by `f0a01c4 "vsm and lod tuning"`**
— the additive-bias-with-a-radius it was written against is gone, replaced by a per-frame ABSOLUTE
per-group override equal to each chunk's own CAMERA tier (`RenderableObject::chunkLods_` →
`ShadowGpuData::RefreshChunkGroupLods` → `groupLodOverride_` → the VSM setup CB). The facts below
are restated against that design. **What did NOT change: the cap, the mechanism that breaks at it,
and the headroom** — wind_test re-measured today still reads
`2708 casters, 56 mesh-groups (1 chunked of 9 meshes, cap 64)`, `cull validation PASS`.

The rewrite also means chunking is **no longer shadow-only**: the gbuffer now draws a chunked mesh
per submesh at per-chunk LOD (`RenderableObject.cpp:173-178`). That does not touch the group cap
(camera draws are not grouped) but it does mean a chunked mesh now costs one draw per chunk on the
camera path too — relevant when sizing any future grid, not to this plan.

**The verdict up front:** the cap is not a property of the per-page-culling approach — Unreal's
non-Nanite VSM path does the same job with no compile-time group limit at all. Our 64 exists only
because the per-group prefix-sum state lives in two fixed local arrays inside the per-page setup
thread. Everything else in the pipeline is already sized by the real group count, including the CPU
tables — only the *transport* truncates.

---

## Facts — verified, do NOT re-derive

- **F1. Everything sized by the cap** (re-enumerated 2026-08-21 20:30 — the list GREW by one since
  the first draft, and the new entry is on the CPU):

  | where | what | note |
  |---|---|---|
  | `vsm_page_setup_cs.hlsl:186` | `uint perGroupCount[VSM_MAX_SETUP_GROUPS]` | per-THREAD local — the UB (F2) |
  | `vsm_page_setup_cs.hlsl:244` | `uint perGroupBase[VSM_MAX_SETUP_GROUPS]` | per-THREAD local — the UB (F2) |
  | same, CB | `uint4 gGroupLodMega[64 * KMAX_SHADOW_LODS]` | guarded by `g2 < VSM_MAX_SETUP_GROUPS` |
  | same, CB | `int4 gGroupLodOverride[KGROUP_BIAS_VEC4]` | **was `gGroupLodBias`**; same guard |
  | `ShadowGpuData.h:413` | `std::array<std::int8_t, vsm::kMaxMeshGroups> groupLodOverride_` | **NEW — a fixed CPU array, refreshed EVERY FRAME** |
  | `ShadowGpuData.cpp:650` | `kMaxGroups` — GI fold policy | soft (F7) |
  | `ShadowGpuData.cpp:933` | `kMaxMegaGroups` — mega gate | soft-ish (F2 step 1) |

  Every loop over the two local arrays runs to `gNumGroups`, which the CPU fills **unclamped** from
  `shadowGpu->MeshGroupCount()` (`VirtualShadowMap.cpp:1144`).
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
- **F4. Half the CPU side is already full-size; the other half no longer is.**
  `groupLodMega_.assign(numMeshGroups_ * kLods * 4)` (ShadowGpuData.cpp:853) carries every group and
  only the CB fill clamps (`gm = min(groups, kMaxMegaGroups)`, `VirtualShadowMap.cpp:1171`) — for it,
  removing the cap really is just a transport change (CB → SRV).
  **`groupLodOverride_` is different: it is a `std::array` hard-sized at the cap**, and
  `RefreshChunkGroupLods` stops filling at it (`if (g >= groupLodOverride_.size()) break;`,
  ShadowGpuData.cpp:396). S3 must therefore GROW that container as well as move it, not merely
  re-point it — and it is refreshed every frame, so its upload wants a per-frame ring region rather
  than the static region `groupLodMega_` can use.
- **F4b. Rebuild bakes nothing camera-dependent any more.** `perViewGroup_` carries the PLAIN view
  LOD for every group including chunk groups (the `biasedLod` lambda is now the identity —
  ShadowGpuData.cpp:806); the per-chunk LOD arrives only through the per-frame override in the VSM
  CB. So the Legacy/Rung-0 indirect path draws chunk groups at the view LOD by design (documented
  divergence, ShadowGpuData.cpp:800-805), and the cap work cannot regress it because it never had
  the override to lose.
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
- **F7b. NEW since the rewrite: >64 also breaks chunked terrain's QUALITY, not just its safety.**
  A chunk whose group lands past the cap gets no camera-tier override — `RefreshChunkGroupLods`
  `break`s, and the shader's over-cap branch reads `Rung0Args` (view LOD). The caster then stops
  matching the geometry the gbuffer drew for that chunk, which is precisely the identity the whole
  chunk-LOD rewrite exists to hold ("retired the whole terrain self-shadow-mismatch family —
  banding, low-sun stairs, phantom blobs"). So the failure at the cap is now **UB on the local
  arrays AND silent banding on the tail chunks** — and the configuration that reaches the cap is a
  finer terrain grid, i.e. exactly the chunks that would lose it. This raises the stakes on S1's
  clamp: it removes the UB but NOT this, which only S3+S4 fix.
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

### S1 + S2 RESULT — DONE 2026-08-24 (as ONE edit: they touch the same lines)

`shaders/vsm_page_setup_cs.hlsl` only — no C++, no CB, no root signature, no resources.

* `numLocalGroups = min(gNumGroups, VSM_MAX_SETUP_GROUPS)` bounds every touch of the two per-thread
  tables: the zero-fill, the brute-force count guard, the prefix sum, and pass 2's scatter cursor.
  Past the cap a group is never counted, so it draws nothing — a deterministic loss.
* **The scattered (clipmap) path no longer touches either array.** It reads
  `PageGroupCount[p*gNumGroups+g2]` and `PerGroup[g2].x` directly in the args loop, so directional
  pages are already cap-free.
* The args loop still runs to `gNumGroups` — it must fully rewrite the real-sized args buffer, or a
  skipped tail leaves last frame's records for the loop-path ExecuteIndirect. Over-cap groups on the
  brute-force path emit zero instances.
* The count/base selection is **real control flow, not a ternary**: `if (scattered) / else if (g2 <
  VSM_MAX_SETUP_GROUPS) / else`. A select would still evaluate the array read on the untaken side —
  which is the exact out-of-bounds access being removed.

Verification, at 56 groups. Read the readiness caveat: this is "unchanged where we can see" plus
code reading, **not** a >64 proof — that is still S4.5 and still needs the user.

| gate | result |
|---|---|
| `tools/check_shaders.py vsm_page_setup` | 1/1 compiled |
| Release boot + **Debug** boot | both render; `cull validation PASS: 44 views match CPU (2708 casters, 56 groups)` |
| image, HEAD shader vs this one | terrain bands at the noise floor (0.004–0.012 against a floor of 0.003–0.010). The one band above it is the canopy + the HUD text — sway phase and changing FPS digits |
| `Pass_VsmPageRender` (interleaved ×2) | before 0.788 / 0.802, after 0.801 / 0.804 — inside the before-side spread |
| `VsmPageRender.Setup` | 0.016 both |
| `VsmPageRender.Scatter` | 0.107 both |
| `Pass_Compose` / `Pass_Tonemap` controls | 0.030 / 0.385 throughout |

S2 was predicted "neutral-to-better" and measured neutral: `Setup` is 0.016 ms in total, so deleting
a 56-iteration copy per page had nothing to give back. Its value is the removed cap, not speed.

**A/B method worth reusing for any runtime-compiled shader.** The "before" was
`git show HEAD:shaders/vsm_page_setup_cs.hlsl` — legitimate because `git diff --stat HEAD --` proved
the file was clean before the edit. It was swapped onto the real path inside a PowerShell
`try/finally` that restores the edited version, with a `Get-FileHash` equality check printed after.
Same binary, same level, minutes apart, and git state never touched.


### S2 — Scattered path: delete the local arrays (directional cap gone)

**Touch:** `shaders/vsm_page_setup_cs.hlsl`.

Per F5 this is a pure de-caching: in the `scattered` branch drop both array fills and read
`PageGroupCount[p * gNumGroups + g2]` / `PerGroup[g2].x` directly in the args loop. Declare the
arrays only in the brute-force scope so the scattered path carries zero scratch. Expected perf:
neutral-to-better (`VsmPageRender.Setup` is 0.010–0.023 ms today; each element was read exactly
once anyway).

**Done when:** clipmap pages have no group-count limit; image + perf at 56 groups unchanged. **DONE — see the S1 + S2 RESULT block above; both steps landed as one edit.**

### S3 — CB tables → SRVs (the transport fix that unblocks everything)

**Touch:** `vsm_page_setup_cs.hlsl` (RS + cbuffer), `VirtualShadowMap.cpp` (SetupCB + dispatch),
`ShadowGpuData.{h,cpp}` (expose the vectors as GPU buffers).

1. Two new structured SRVs, and note they have DIFFERENT lifetimes (F4):
   - `StructuredBuffer<uint4> GroupLodMega : register(t9)` — numGroups × kLods entries, written once
     per Rebuild from the already-full-size `groupLodMega_`. Static region, same as `perGroup_`.
   - `StructuredBuffer<int> GroupLodOverride : register(t10)` — numGroups entries, rewritten EVERY
     FRAME by `RefreshChunkGroupLods`. Needs a **per-frame ring region** (like `instances_` /
     `bounds_`), not a static one, and `groupLodOverride_` has to become a `std::vector` sized to
     `numMeshGroups_` with the `break` guard dropped.
2. RS: `SRV(t0, numDescriptors=9)` → `11`; extend the descriptor list in the
   `RecordComputeDispatch` call. Drop `gGroupLodMega`, `gGroupLodOverride`, `KGROUP_BIAS_VEC4` from
   the CB and its CPU mirror **in the same edit** (the CB is a mirror; repack both sides together).
   `gViewLod` stays in the CB — views are fixed at 44, groups are not. `_pad5` is free again and
   stays padding.
3. The `g2 < VSM_MAX_SETUP_GROUPS ? CB : Rung0Args` fallback branch dies; every group reads the SRV.
   That is also what retires F7b: past the cap, chunk groups get their real camera tier instead of
   silently reverting to the view LOD.
4. Verification: **full gate set** — new SRVs + RS change (both configs, Release `--scene-stress`,
   Debug `--scene-stress-gbv`, comparator), plus the A/B image + trace protocol. This is exactly
   the change class `tools/check_shaders.py` cannot vouch for alone: dxc accepting the shader says
   nothing about the PSO or the descriptor table — run it.

**Done when:** no group table lives in a CB; behavior at 56 groups byte-stable. **DONE — see the S3 + S4 RESULT block below.**

### S4 — Lift the mega/single-draw gate

**Touch:** `ShadowGpuData.cpp` (the two `<= kMaxMegaGroups` conditions + warning text),
`VirtualShadowMap.h` (`kMaxMeshGroups` comment: now the LOCAL cull cap, not the global budget).

With S3 in place nothing indexes a fixed table, and the mega build itself never depended on 64
(F6). Lifting the gate keeps `MegaReady()` — and therefore `singleDraw` — alive at >64, so the
over-cap configuration is not only correct but stays on the fast path. GI folding keeps its policy
gate (F7). Update the hand-synced pairing note (F8) on both sides.

**Done when:** a >64 level renders directional VSM correctly on the single-draw path, with locals
deterministically capped and logged.

### S3 + S4 RESULT — DONE 2026-08-24

**S3, transport.** `gGroupLodMega` and `gGroupLodOverride` left the constant buffer for two SRVs
sized by the real group count:

* `StructuredBuffer<uint4> GroupLodMega : register(t9)` — `groupLodMegaBuf_`, a STATIC ring region 0
  written at the end of `Rebuild`, deliberately after the mega block patches `groupLodMega_` in
  place (absolute starts + base vertex).
* `StructuredBuffer<int> GroupLodOverride : register(t10)` — `groupLodOverrideBuf_`, a PER-FRAME ring
  region, rewritten by `RefreshChunkGroupLods` (which now takes the renderer: Scene calls it after
  `UpdateForFrame`, so there is no later per-frame hook to defer the upload to). Its frame index is
  the same `renderer->GetCurrentFrameIndex()` the instance/bounds rings use.
* `groupLodOverride_` became a `std::vector<std::int32_t>` sized `numMeshGroups_`. As a
  `std::array<int8_t, 64>` it silently dropped every chunk past the cap — F7b.
* Root signature `SRV(t0, numDescriptors=9)` → `11`, and the two handles appended to the dispatch's
  descriptor list **in register order** — that list IS t0..t10 positionally.
* The `g2 < VSM_MAX_SETUP_GROUPS ? CB : Rung0Args` branch in the args loop is gone; every group
  reads the SRVs. That is what retires F7b.

**S4, the mega gate.** `Rebuild`'s `numMeshGroups_ <= kMaxMegaGroups` condition is deleted along
with its warning: the mega layout never had a 64 dependency (it iterates `megaCopy_` per unique
MESH), and the gate existed only because a CB array could not address more groups. Keeping it would
have cost the >64 case its single-draw fast path for nothing.

Verification:

| gate | result |
|---|---|
| both configs | build clean |
| `tools/check_shaders.py vsm_page_setup` | 1/1 compiled |
| Release boot | PSO builds with the new RS; render clean, palm shadows sharp |
| `cull validation` | `PASS: 44 views match CPU (2708 casters, 56 groups)` |
| **image vs the S1+S2 state** | terrain bands **0.0003 / 0.0001 / 0.0001** — pixel-identical, an order below the same-binary noise floor (0.0096 / 0.0080 / 0.0026). Only the canopy+HUD band differs (0.044 vs a 0.061 floor) |
| Debug `--scene-stress=24 --scene-stress-gbv` | `verdict: CLEAN after 24 iterations` (barriers 8820 enhanced / 0 legacy) |
| Release `--scene-stress=24` | `verdict: CLEAN after 24 iterations` (8214 / 0) |

Both stress runs exercise level switches with the chunked mesh present; `cull validation PASS`
recurs across them at 37/38 groups as levels change.

The terrain coming out bit-identical through an entirely different data path is the strongest signal
available here: the values reaching the shader are the same, only their transport changed.

**Two leftovers, deliberate.** `Rung0Args` (t1) is now unread by this shader and `gArgBaseElems` is
now unused in its CB — both are LEFT IN PLACE. Removing t1 would shift t2..t8 and a descriptor table
is positional; removing the CB field would repack the mirror. Neither buys anything.

**What is NOT proven.** Still 56 groups. S4.5 (a >64 level) remains the only thing that can exercise
the paths this work exists for, and it still needs the user's asset writes.


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

### S4.5 RESULT — THE >64 PROOF, and it holds (2026-08-24)

The user re-imported the island at `chunkGrid 7`. 7x7 with 3 empty corner cells = **46 chunk
submeshes**, so wind_test came out at **66 mesh-groups** — two past the old cap, and the first time
this engine has ever run there.

| gate | result at 66 groups |
|---|---|
| boot | renders clean; palm shadows sharp, no missing terrain shadow, no banding |
| `cull validation` | `PASS: 44 views match CPU (2718 casters, 66 groups)` |
| **fast path alive** | `Pass_VsmPageRender` **0.808 / 0.803** vs **0.801 / 0.804** at 56 groups — unchanged |
| `VsmPageRender.Scatter` | 0.107, unchanged |
| `Pass_Compose` / `Pass_Tonemap` | 0.029-0.030 / 0.385 |
| Debug `--scene-stress=24 --scene-stress-gbv` | `verdict: CLEAN after 24 iterations` |

**The perf number IS the proof that S4 worked.** Had the mega gate still fired, the draw would have
fallen back to the per-page per-group binding loop — 1024 pages x 66 groups of
IASetVertexBuffers/IASetIndexBuffer/ExecuteIndirect. That is milliseconds, not 0.003 ms of noise.
Before this work the same configuration was undefined behaviour on two per-thread arrays.

**One real cost, and I did not measure it when I should have.** `VsmPageRender.Setup` went
**0.016 -> 0.051 ms**. Two things changed between those samples (S3's CB->SRV transport, and 56->66
groups), because S3 was verified by image and stress but never traced — that was a gap in the S3
gate. The confound is bounded by argument, though: the group count only scales the args loop, so
56->66 can account for at most +18% (0.016 -> ~0.019). **At least 0.032 of the 0.035 ms is the SRV
transport** — a constant-buffer load per group became a structured-buffer load, 66 of them per page
across 1024 page threads.

In context that is +0.035 ms on a ~2.1 ms GPU frame (~1.7%), paid to remove the cap, and
`Pass_VsmPageRender` itself did not move. Whether to claw it back is a separate question; note that
the obvious idea — folding the override into `PerGroup`'s unused `.w` — does NOT work, because
`PerGroup` is a static region-0 buffer while the override is per-frame. A cheap partial win would be
a uniform "no chunked mesh in this level" flag that skips the override load entirely, which helps
every level that has no chunked terrain and does nothing for this one.

Also fixed here: the rebuild log printed `cap %u`, which read like a hard limit the moment a level
went past it. It now prints how many groups still cast from LOCAL lights:
`66 mesh-groups (1 chunked of 9 meshes; 64 cast from local lights)`.

### Follow-up found while answering "what do groups cost as they grow" (2026-08-24)

Group cost is **O(pages x groups)**: the setup CS runs one thread per page and that thread loops
every group, and until now an EMPTY group still cost a loop body (two SRV loads + a 20-byte store).
At 66 groups that is 67.6k arg records per frame. Three levers, in order of readiness:

1. **Compacted draw args — measured, and it is now a win.** `g_pageDrawCompact` was recorded a small
   LOSS at 6 groups in 2026-08-01 and its comment asked for a re-measure in a group-heavy scene.
   Done at 66: `Pass_VsmPageRender` 0.794/0.809 -> 0.757/0.756, `VsmPageRender.Setup` 0.050 -> 0.022.
   It skips the loop BODY for empty groups, so it also recovers most of the CB->SRV transport cost
   S3 introduced. Left OFF by default — the right default is scene-dependent and that is a decision.
2. **The dispatch throws away 7 of every 8 threads.** `vsm_page_setup_cs` is `numthreads(8,8,1)` and
   opens with `if (dtid.y != 0u) { return; }` — the y lane exists only because RecordComputeDispatch
   has a fixed shape. Since S2, the SCATTERED path's args loop has no per-thread state (both
   groupCount and groupBase are direct buffer reads), so it could be split across `dtid.y` — ~8x on
   exactly the loop that scales with group count, for no new resources. The per-page prologue
   (planes, projection, dirty decision) stays on one lane. Not valid for the brute-force local path,
   whose prefix sum is inherently serial.
3. **UE's shape** — no page thread iterates draw commands at all; see the UE section above. The
   structural fix, and the one that also retires S5.

### Levers 1 + 2 — DONE and measured 2026-08-24

Both landed after the cap work, on wind_test at 66 groups (island chunked 7x7). Interleaved x2,
`Pass_Compose` flat at 0.029 throughout:

| config | `Pass_VsmPageRender` | `VsmPageRender.Setup` |
|---|---|---|
| 1 lane, compact OFF (the state S4.5 was proven in) | 0.794 / 0.809 | 0.050 |
| 1 lane, compact ON | 0.757 / 0.756 | 0.022 |
| 8 lanes, compact OFF | 0.757 / 0.766 | 0.014 |
| **8 lanes, compact ON (shipped)** | **0.704 / 0.706** | **0.008** |

**-0.097 ms on the pass (-12%), and Setup 6.3x.** Setup at 0.008 is now BELOW its pre-S3 value
(0.016), so the CB->SRV transport cost the cap removal introduced is not merely paid off — the pass
is cheaper than before any of this work.

**Lever 2 (`vsm_page_setup_cs`): the dispatch was throwing away 7 of every 8 threads.** It opened
`if (dtid.y != 0u) return;` because RecordComputeDispatch has a fixed 8x8 group shape. Since S2 the
SCATTERED path's per-group loop has no per-thread state, so it is now strided across the lanes
(`gStart = lane`, `gStep = VSM_SETUP_LANES`). Lane 0 alone keeps: the page projection + PageProj
stores, the frustum planes, the `PerPageDirty` write, and the whole brute-force local-light path
(its prefix sum is inherently serial). Extra lanes retire immediately on a non-scattered page.

**Lever 1 (`g_pageDrawCompact`): default flipped to ON.** Its comment demanded a group-heavy
re-measure before enabling; done, and the 2026-08-01 verdict inverted. The light-scene check the
decision actually needed — demo.json, 6 groups, 9 spots + 8 points — came out **0.367/0.360 OFF vs
0.360/0.358 ON**, i.e. the old loss is gone, because lever 2 means compaction's atomic no longer
lands in a serial loop. Neutral on a light scene, -0.057 ms on a group-heavy one. `--vsm-page-compact`
would have become a no-op flag, so `--vsm-page-nocompact` was added as the A/B control.

Verification: both configs build; dxc 1/1; wind_test image vs the 1-lane build |mean| **0.0264**
against a same-binary noise floor of **0.0321** (i.e. below the floor); demo.json renders local-light
shadows correctly inside the spot cones — the path lever 2 deliberately leaves on lane 0;
`cull validation PASS` on both levels; Debug `--scene-stress=24 --scene-stress-gbv` and Release
`--scene-stress=24` both `verdict: CLEAN after 24 iterations`.

### S5 — OPTIONAL: local lights join the scatter (full cap removal)

### S5 RESULT — DONE 2026-08-24 (the user built the level it needed)

The blocker was "no level combines local lights with >64 groups". The user added atoll meshes to
demo.json, which now reads **70 mesh-groups + 9 spots + 8 points** — so the step became justified and
verifiable in the same move.

**What shipped.** `vsm_page_scatter_cs` now scatters LOCAL views as well as clipmap levels: the
dispatch's y axis indexes a scatter TARGET (`[0,gNumLevels)` = clipmap level,
`[gNumLevels, +gNumLocalViews)` = local view), the CB carries every view's matrix instead of only the
8 clipmap ones, and a local view walks its whole mip pyramid (a receiver pixel marks a page at its
own level, so every level can be resident). The setup CS's `scattered` now covers local pages too;
its brute-force block survives ONLY as the `gScatterActive == 0` fallback (a scatter-PSO failure must
degrade to slow, not to no shadows) — which is also the one place the group cap still exists.

**The perspective rect, and the trap in copying UE literally.** UE's
`BoxCullFrustumPerspective` answers the straddling-w=0 case by taking the FULL [-1,1] rect. Ported
verbatim that is a **+0.47 ms regression**: `Pass_VsmPageRender` 0.308 -> 0.776, because a straddling
caster then appends to every resident page of the view. A one-line diagnostic (straddle -> reject,
deliberately wrong) put the pass back at 0.325 and named the cause outright. The fix is to treat the
full rect as a page CANDIDATE SET and test each candidate against that page's exact frustum planes
(`BoxInPagePlanes`, the same positive-vertex rule the brute-force path used), after the residency
check so only real pages pay. Applying that to straddlers alone still left +0.09; applying it to ALL
local pages — the NDC rect is loose under perspective generally, not just at the near plane — landed
it. Progression, all on demo.json:

| variant | `Pass_VsmPageRender` | `Setup` |
|---|---|---|
| brute-force (before S5) | 0.308 / 0.310 | 0.072 |
| S5, UE's full-rect bail | 0.776 / 0.765 | 0.009 |
| S5, exact test for straddlers only | 0.393 / 0.403 | 0.009 |
| **S5, exact test for all local pages** | **0.309 / 0.306** | **0.009** |

**Final verdict, same-binary A/B** (`--vsm-no-local-scatter` was added so both paths live in one
binary — the toggle is `vsm::g_scatterLocalViews`):

| | `Pass_VsmPageRender` | `Setup` | `Scatter` | GPU frame |
|---|---|---|---|---|
| brute-force locals | 0.308 / 0.306 | 0.070 / 0.071 | 0.040 | 2.144 / 2.222 |
| **scattered locals (default)** | 0.306 / 0.308 | **0.009 / 0.009** | 0.050 | 2.172 / 2.223 |

**Perf-neutral today, structurally better tomorrow, and the cap is gone.** `Setup` is 7.8x cheaper
because the O(pages x casters) term for local lights disappeared — that term grew with the scene,
the scatter's does not. The +0.010 on `Scatter` pays for it. wind_test (66 groups, no local lights)
is untouched: 0.700/0.715 vs 0.704/0.706, `Setup` 0.007.

**Correctness, and the measurement that nearly went wrong.** The first comparison read |mean| 2.06
over 71 % of pixels, which looks alarming — until demo.json's own noise floor turned out to be
**1.07** (animated scene, nothing frozen). With `--wind-freeze` and the same-binary toggle the floor
drops to **0.135 / 0.098** and the actual brute-force-vs-scattered difference is **0.164** — barely
above it, with mean luminance 87.939 -> 87.850, i.e. a hair MORE shadow, which is exactly what
un-capping the tail groups should do. The amplified diff is confined to specular highlights on the
sphere grids and the tent; no structured loss anywhere. A second self-check agrees: rect-only vs
exact-test differ by 0.144, below the floor, so the exact test prunes nothing that was being drawn.

Gates: both configs build; `check_shaders.py` 45/45; `cull validation PASS` on both levels; Debug
`--scene-stress=24 --scene-stress-gbv` and Release `--scene-stress=24` both `CLEAN after 24
iterations`.

**What is left capped:** only the `gScatterActive == 0` fallback (scatter PSO failure) and the
GI-fold policy. `vsm::kMaxMeshGroups`'s doc block and the import dialog text should be narrowed once
more to say that.

#### Original step text

Locals brute-force because a perspective view has no trivial AABB→page-rect mapping
(vsm_page_setup_cs.hlsl:172 comment). UE crosses that bridge in `VirtualShadowMapPageOverlap.ush`
(clip-space AABB footprint per mip). The equivalent here: extend `vsm_page_scatter_cs` (or a
sibling) to walk local views' mip pyramids and append (caster, page) pairs the same way clipmap
levels do — after which every page takes the `scattered` branch and the local arrays are deleted
outright, per the delete-don't-disable rule (the brute-force path is currently also the fallback
when the scatter PSO fails to build; decide its fate then, not now). Price: a real shader-design
step, ~a day plus verification. **Not needed until a level combines local lights with >64 groups**
— today wind_test has zero locals.

### Readiness — what a start would cost, and what it does NOT depend on

- **S1 is unblocked and cheap** (shader-only, ~an hour with its gate) and buys the biggest single
  thing: the 65th group stops being undefined. It does not need S4.5's assets.
- **S2 is unblocked** and is where the directional cap actually falls.
- **S3 is the real work** (new SRVs + RS + a container change + a per-frame ring) and the only step
  needing the full stress/GBV set.
- **S4.5 needs the user** (asset writes) and is the only step that can prove >64 end to end. Until
  it runs, S1..S4 are verified only at 56 groups — i.e. "unchanged where we can see" plus code
  reading. State that limit rather than implying a >64 proof.
- Nothing here depends on the banding camera still owed on the chunking plan, and nothing here
  blocks that work.

### S6 RESULT — DONE 2026-08-24 (the parts that do not wait on S5)

* **Import dialog** (`ImportPanel.cpp`, Shadow chunking): the readout said "N of the level's 64",
  which now over-warns — directional shadows have no group limit any more. It states the real
  consequence instead: past the budget the extra tiles **stop casting from spot/point lights, sun and
  sky are unaffected**. The red "over cap" colour is gone; only the amber caution remains, because
  there is no longer a hard failure to signal.
* **`vsm::kMaxMeshGroups`** doc block narrowed: it is no longer "the shadow path's cap" but the
  local-light brute-force cull's per-thread table size plus the GI-fold policy cap, with a pointer to
  S5 for removing even that. The stale "over it, the mega-buffer path degrades" clause is deleted —
  S4 removed that gate.
* Both configs rebuilt after the edit.

Left for when S5 lands (or is declined): nothing else — the chunking plan's F4 and the memory files
were updated in the same pass.

### S6 — Truth pass on docs + UI (original step text)

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
