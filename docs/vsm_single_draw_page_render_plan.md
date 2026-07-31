# VSM single-draw page render plan (GPU-driven page iteration)

**Goal.** Replace the 1024-iteration CPU loop in `VirtualShadowMap::RecordPageRender`
(`sources/rendering/shadows/VirtualShadowMap.cpp:1063-1112` — per page: `RSSetViewports` +
`RSSetScissorRects` + `SetGraphicsRootConstantBufferView` + `ExecuteIndirect(maxCount=groups)`)
with **one** `ExecuteIndirect` over all `(page, group)` argument records. The per-page viewport
moves into the vertex shader as a clip-space scale/bias, and the page's own borders become four
`SV_ClipDistance0` planes, so the hardware clips exactly what the scissor used to.

Executor conventions (same as the other VSM plans): one step per commit; build BOTH configs via
PowerShell MSBuild (`test_cube.sln`, Debug+Release, expect 0 warnings / 0 errors); verify with
`--scene-stress-gbv` (known-noise ids {939,940,1006,1358}); shaders compile at RUNTIME, so every
verify must actually run in **VSM mode** (Ctrl+V); line endings `.cpp/.h/.hlsl` = CRLF, `.md` = LF;
do NOT commit (the user commits per step). The visual gate is a flag A/B — the user signs off.

---

## Honest expected win (read this before starting)

**This is not a GPU optimization.** Two measurements already on record:

- `docs/rt_shadows_integration_plan.md:687-693` — `g_residentIterOnly` (which skips the ~45 % free
  pages in the same loop) moves GPU time by **~2 % (noise)** and CPU submission 0.27 → 0.13 ms.
  Empty, zero-instance `ExecuteIndirect` records are effectively free on the GPU. The doc's verdict
  — *"Do not build the resident-list compaction; it was measured to do nothing here"* — is about
  GPU time and still stands.
- `docs/vsm_per_page_instance_cull_plan.md:105-109` lists **"GPU-driven page iteration (one indirect
  dispatch/draw over the resident set instead of a 1024-iteration CPU loop)"** as the intended
  follow-up rung. The 2.31 ms quoted there is **Debug**.

So the value delivered is, in order:

1. **Removes an artifact class — but check the cheap fix first.** The `residentReadback_` ring
   (`VirtualShadowMap.h:333-340`) is a `kFrameCount`-old snapshot: a newly resident page is skipped
   by the CPU loop for ~3 frames and shows unshadowed (edge pop-in while the camera moves). With one
   draw there is nothing to skip — the setup CS writes `InstanceCount = 0` for free pages in the SAME
   frame — so the readback, `g_residentIterOnly`, and the flicker all disappear by construction.
   **However:** `vsm::g_residentIterOnly` is currently `true` (`VirtualShadowMap.h:129`) even though
   its own comment and the one at `VirtualShadowMap.cpp:958` both claim "DEFAULT OFF". That is why
   the artifact is live today — and flipping that one line to `false` removes it just as completely,
   at the cost of ~0.13 ms of CPU submission that `Pass_VsmPageRender` spends on a render-graph
   worker (`SceneRenderer.cpp:579-583`), i.e. off the critical path. So the honest statement of this
   plan's value is *"the artifact goes away without paying that CPU"*, not *"the artifact goes away"*.
   Other VSM flicker classes (allocation churn, LRU eviction, cache warmup) are untouched.
2. **CPU submission**: ~0.13 ms → ~0.01 ms (Release); ~2.31 → ~0.05 ms (Debug). This is *aggregate*
   CPU, not frame time: `Pass_VsmPageRender` records on a render-graph worker, and the sibling plan
   already measured exactly this shape — `docs/vsm_per_page_instance_cull_plan.md:98-99`, "aren't the
   critical path there — the saving is real aggregate CPU work, just off the hot path". Do not
   promise FPS from this line.
3. **Lifts the CPU ceiling off the pool size.** `kPoolPageCount` currently cannot grow without
   paying linearly in the loop.
4. **Unblocks compacted args** (Step 4): once the page id travels per-instance, argument records no
   longer have to be laid out `[page][group]`, so the setup can append only non-empty records and
   drive the draw with a count buffer.

If the goal is "the frame is too slow today", this is the wrong lever — use `render::g_shadowLodBias`
and `vsm::g_clipmapBaseExtent` (`docs/rt_shadows_integration_plan.md:694-708`). If the goal is
"remove the stale-snapshot artifact and the structural CPU ceiling", this is the plan.

## Why it is feasible (what is already in place)

- **The per-page projection is already page-local NDC.** `vsm_page_setup_cs.hlsl:112-121` builds
  `pm = gViewProj[view] * S`, where `S` stretches the page's virtual sub-rect to fill `[-1,1]`.
  Placing that into a pool cell is therefore a linear scale/bias on clip space, and the page's
  borders are exactly the four side planes of the same clip volume.
- **The pattern already works in this engine.** `vsm_page_clear.hlsl:25-37` emits per-page cell
  quads under a full-pool viewport.
- **The argument buffer is already contiguous** as `[page][group]`
  (`VirtualShadowMap.cpp:560`, `vsm_page_setup_cs.hlsl:251-253`), so one call with `argOffset = 0`
  and `maxCount = kPoolPageCount * groups` reads it unchanged — **no arg-layout change**.
- **Free and cached pages are already zeroed**: `vsm_page_setup_cs.hlsl:92-101` (free page → all
  groups `InstanceCount = 0`) and `:198-206` (clean cached page → same). The CPU skip was never a
  correctness requirement.
- **`pageProj_` is always written for resident pages** before the dirty early-out
  (`vsm_page_setup_cs.hlsl:122-130`), including the wind tail at byte 192/208.

## The blocker and the fix

A viewport cannot live in indirect arguments, and the per-page projection arrives as a **root CBV**
(`b1`, set per page at `VirtualShadowMap.cpp:1076`), which likewise cannot vary inside one
`ExecuteIndirect` — the command signature is a plain `DRAW_INDEXED` (`Renderer.cpp:969`). That is
the whole reason the loop exists. Both go away with two changes:

- **Viewport → VS.** The VS emits `H = pageLocalClip` scaled/biased into the page's pool cell, plus
  `SV_ClipDistance0 = (w+x, w-x, w+y, w-y)` of the *page-local* clip position, which is precisely
  the page rect. One full-pool viewport for the entire draw.
- **Root CBV → SRV + packed page id.** The per-instance stream is the page's own slice of
  `pageVisibleList_`, so the physical page index can be packed into the same `uint` as the caster id
  (1024 pages = 10 bits; caster slots need 22 bits = 4.19 M, current scenes are ~800). The VS then
  reads that page's matrix + wind from a `StructuredBuffer<float4>` view of `pageProj_`.

Rejected alternative: a command signature carrying `D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW`
or `_CONSTANT`. It needs a root-signature-bound command signature (one per shader permutation), a
5 → 6/7 uint arg stride (touching every `*20u` offset in the setup shader), and 64-bit GPU-VA
arithmetic in HLSL for the per-page CBV address. Strictly more churn for the same result.

## Register layout (constrained by `RenderContext::kMaxBindings == 4`)

`Material::Bind` keys descriptor tables by their **base register** and silently skips any root
parameter whose base is `>= kMaxBindings` (`sources/materials/Material.cpp:470-472`,
`sources/rendering/core/RenderContext.h:15`). So every table base must stay in `t0..t3`. For the
`VSM_PAGE` permutations:

| variant | table | contents |
|---|---|---|
| opaque | `DescriptorTable(SRV(t0, numDescriptors=2))` | `t0` Instances, `t1` PageProj |
| masked | `DescriptorTable(SRV(t0, numDescriptors=20))` | `t0` Instances, `t1` CasterGroup, `t2` GroupMask, `t3` PageProj, `t4..t19` `gMaskAlbedo[16]` |

The masked variant folds today's two tables (`t0..t2` + `t3..t18`) into one and moves the albedo
array to `t4` — **only inside `#if VSM_PAGE`**; the legacy permutations keep their current layout.
Putting PageProj *before* the albedos matters: the staged range is then
`{instances, casterGroup, groupMask, pageProj} + MaskedAlbedoCount()` descriptors, so unused albedo
slots need no dummy descriptors (exactly today's situation, where only `count` are staged into a
16-wide range and the shader never indexes past it).

---

## Steps

### Step 0 — flag + dev-window toggle (dormant) — **DONE (uncommitted)**
`sources/rendering/shadows/VirtualShadowMap.h`, next to `g_residentIterOnly`:

```cpp
// Single-draw page render: ONE ExecuteIndirect over all (page, group) args instead of the
// 1024-iteration CPU loop (per-page viewport -> VS clip-space remap + SV_ClipDistance page
// borders). Requires the mega buffer (geometry bound once). Default ON; OFF restores the
// per-page loop for A/B and for per-page inspection in PIX.
inline bool g_pageDrawSingle = true;
```

Checkbox in the dev window's VSM section (`sources/app/ui/DeveloperWindow.cpp`), placed directly
under the "Resident-only render" checkbox at `:703` — the two flags are alternatives for the same
artifact, so they belong side by side.

**Verify:** both configs 0/0; nothing else changes.

**Result:** Debug + Release both `0 Warning(s) 0 Error(s)`. `g_pageDrawSingle` has exactly two
references (its definition and the checkbox), so the toggle is provably inert — no runtime check
needed at this step. Also corrected two stale comments that claimed `g_residentIterOnly` defaults
OFF (`VirtualShadowMap.h:125-129`, `VirtualShadowMap.cpp:958`); it is `true` deliberately — the user
wants the CPU saving — which is exactly why the blink is visible today.

### Step 1 — page-id packing plumbed into both list writers (dormant, shift = 0) — **DONE (uncommitted)**
Both writers of `PageVisibleList` OR in the physical page index:

- `shaders/vsm_page_setup_cs.hlsl:272` → `PageVisibleList[pageBase + perGroupBase[g]] = (c2 + s2) | (p << gPageIdShift);`
- `shaders/vsm_page_scatter_cs.hlsl:118` → `PageVisibleList[p * gNumCasters + PerGroup[g].x + rank] = (c + s) | (p << gPageIdShift);`

`gPageIdShift` is a new `uint` in `SetupCB` and `ScatterCB` (filled at
`VirtualShadowMap.cpp:824-900`): `22u` when the single-draw path is active, `0u` otherwise. Treat
`0u` as "do not pack" with an explicit `if (gPageIdShift != 0u)` branch — do not shift by zero.

Add `static_assert(vsm::kPoolPageCount <= (1u << 10), "page id must fit in 10 bits");`

**Verify:** both 0/0. This step is pure plumbing: `gPageIdShift` is hard-wired to `0u` at both call
sites (the `singleDraw` predicate that will drive it lands in Step 2, once the SRV and the PSOs it
tests actually exist). Nothing is packed yet, so shadows are byte-identical in both flag states.

**Result:** Debug + Release `0 Warning(s) 0 Error(s)`; `--scene-stress=6` (Release, VSM default)
`verdict: CLEAN`. `vsm::kPageIdShift = 22u` + the static_assert live in `VirtualShadowMap.h` next to
`kPoolPageCount` so the CPU side has one source of truth (Steps 2-3 would otherwise repeat the
literal three times); the HLSL `kPageIdShift` in Step 2 must be kept equal to it by hand.

Two notes for whoever repeats this:
- **MSBuild proves nothing about the shaders** — they compile at runtime, and a broken CS here fails
  SOFT (`pageSetupMat_` null → `RecordPageRender` returns; scatter PSO null → `scatterActive` false →
  silent fallback to brute force). Compile them the way the engine does before trusting a green
  build: `dxc -E CSMain -T cs_6_6 -Zpr -HV 2021 -O3 -I shaders shaders/<file>.hlsl`
  (`C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe`; the flags mirror
  `Material.cpp:139-154`). This also validates the embedded `[RootSignature]`.
- **Both CB fields took an existing pad slot** — `_pad3` in `SetupCB` (offset 36, `gWind0` stays at
  48) and `_pad0` in `ScatterCB` (offset 12, `gClipViewProj` stays at 16). Zero layout drift is what
  makes "byte-identical" provable rather than hopeful; keep it that way if the field ever moves.

### Step 2 — `pageProj_` SRV + the `VSM_PAGE` shader permutations (dormant)
`VirtualShadowMap::EnsureRenderResources`: add a `pageProjSrv_` descriptor to `renderHeap_` —
`StructuredBuffer<float4>` over the existing buffer: `NumElements = kPoolPageCount * 16`,
`StructureByteStride = 16`, `Format = DXGI_FORMAT_UNKNOWN`. (256-byte page stride ÷ 16 = 16
elements per page; row `r` of the matrix is element `page*16 + r`, the wind tail is elements
`page*16 + 12` and `+ 13`.)

**`renderHeap_` is exactly full — grow it.** `NumDescriptors = 12` at `VirtualShadowMap.cpp:645` and
slots 0..11 are all assigned at `:656-667`. This step MUST bump it to `13`, take slot 12 for
`pageProjSrv_`, and add `pageProjSrv_.ptr == 0` to the `needHeap` predicate at `:634-639` — otherwise
the new descriptor is written one slot past the heap.

`shaders/shadow_indirect_csm.hlsl`, new `VSM_PAGE` permutation. **Its root signature drops
`CBV(b1)`** — the per-page projection now arrives as an SRV, and leaving the entry in would make
`Material::Bind` push `ctx.cbv[1]` (still set to `pageProj_`'s VA at `VirtualShadowMap.cpp:1004`) as
a root CBV over a resource this path holds in `NON_PIXEL_SHADER_RESOURCE`. Both new signatures in
full:

```hlsl
#if VSM_PAGE
  #if SHADOW_MASKED
    #define SHADOW_INDIRECT_CSM_RS \
        "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), " \
        "DescriptorTable(SRV(t0, numDescriptors=20, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
        "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_WRAP)"
  #else
    #define SHADOW_INDIRECT_CSM_RS \
        "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), " \
        "DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"
  #endif
#endif
```

The rest of the permutation:

```hlsl
#if VSM_PAGE
#include "vsm_addressing.hlsli" // VSM_POOL_PAGES_AXIS (do not re-hardcode 32)
StructuredBuffer<float4> PageProjRows : register(t1); // t3 in the masked variant

static const float kPoolAxis    = (float)VSM_POOL_PAGES_AXIS;
static const uint  kPageIdShift = 22u;
static const uint  kCasterMask  = (1u << kPageIdShift) - 1u;

float4x4 LoadPageVP(uint page, out float4 w0, out float4 w1)
{
    const uint b = page * 16u;          // 256 B slot / 16 B per float4
    w0 = PageProjRows[b + 12u];         // bytes 192: windTime, prevTime, dirX, dirZ
    w1 = PageProjRows[b + 13u];         // bytes 208: swayAmp, swayFreq, gustMul, prevGustMul
    return float4x4(PageProjRows[b + 0u], PageProjRows[b + 1u],
                    PageProjRows[b + 2u], PageProjRows[b + 3u]);
}

// page-local clip -> the page's pool cell, + the 4 page-border clip planes.
void PagePlace(uint page, float4 hLocal, out float4 H, out float4 CD)
{
    const float s  = 1.0f / kPoolAxis;  // half a cell in NDC (2*128/4096/2)
    const float gx = (float)(page % VSM_POOL_PAGES_AXIS); // same split as the CPU loop at :1066-1067
    const float gy = (float)(page / VSM_POOL_PAGES_AXIS);
    H.x = hLocal.x * s + hLocal.w * (-1.0f + (2.0f * gx + 1.0f) * s);
    H.y = hLocal.y * s + hLocal.w * ( 1.0f - (2.0f * gy + 1.0f) * s); // NDC +y up, pool row 0 top
    H.z = hLocal.z;
    H.w = hLocal.w;
    CD  = float4(hLocal.w + hLocal.x, hLocal.w - hLocal.x,
                 hLocal.w + hLocal.y, hLocal.w - hLocal.y);
}
#endif
```

In the VS body: `const uint page = i.casterId >> kPageIdShift;` and
`const uint casterId = i.casterId & kCasterMask;`; take `viewProj` + the wind values from
`LoadPageVP(page, w0, w1)` instead of the `b1` `PerView` CB (the wind maths must stay
byte-for-byte the behaviour of `WindTransformH` — factor it so both permutations share one
function taking `viewProj`/wind as parameters, do **not** duplicate `WindOffset` logic); add
`float4 CD : SV_ClipDistance0` to the VS output struct and fill it via `PagePlace`. Under
`#if VSM_PAGE`, give the PS its **own** input struct without `CD` (both `PSMain`s today take the VS
output struct directly — `VSOutD` / `VSOutMasked` at `shadow_indirect_csm.hlsl:142` and `:104`); the
masked PS keeps `texSlot`/`cutoff`.

Under `#if VSM_PAGE`, the masked variant additionally declares `Texture2D gMaskAlbedo[16] : register(t4);`
and uses the single 20-descriptor table from the layout table above.

PSOs: in `ShadowGpuData::EnsureShaderResources` (`ShadowGpuData.cpp:1080-1110`) create two more
materials with `gd.defines.emplace_back("VSM_PAGE", "1")` — opaque (`PosOnly_InstCasterId`,
`CULL_BACK`) and masked (`PosUV_InstCasterId`, `CULL_NONE`) — and add
`Material* IndirectShadowPageMaterial() const` mirroring `:331` (masked when
`MaskedShadowsActive()`, else opaque, null when the PSO failed). Log + reset on failure like the
masked PSO does at `:1105`. Input layouts are unchanged: the same `uint` arrives in `CASTERID`,
only its interpretation differs.

Everything the predicate tests now exists, so add it here (still not acted on):

```cpp
Material* pageMat = shadowGpu->IndirectShadowPageMaterial();
const bool singleDraw = vsm::g_pageDrawSingle && useMega &&
                        activeCasters < (1u << 22) && pageProjSrv_.ptr != 0 &&
                        pageMat && pageMat->GetPipelineState();
```

It must be computed **above `VirtualShadowMap.cpp:809`** (`useMega` is ready at `:791`), because
Step 3 feeds `gPageIdShift` from it into *both* the scatter CB (`:830`) and the setup CB (`:865`).
Log once (DBWIN) when `g_pageDrawSingle` is on but `singleDraw` came out false, naming the reason.

**Verify:** both 0/0; DBWIN shows no PSO failure; `gPageIdShift` is still passed as `0u` and the
draw path still binds `IndirectShadowMaterial()`, so shadows are byte-identical in both flag states.

### Step 3 — the flip
In `RecordPageRender`, when `singleDraw`:

- bind `shadowGpu->IndirectShadowPageMaterial()` instead of `IndirectShadowMaterial()`;
- transition `pageProj_` to `D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE` (replacing
  `VERTEX_AND_CONSTANT_BUFFER` at `:972`) — keep the old state on the loop path;
- leave `ctx.cbv[1]` **unset** (`:1004` becomes loop-path-only). The VSM_PAGE root signature has no
  `CBV(b1)`, so a stale value would be ignored — but the pairing "no root CBV entry, no `ctx.cbv[1]`"
  is what keeps the SRV state transition above unambiguous;
- stage the table into `ctx.srvTable[0]`: opaque `{ InstanceReadSrv(f), pageProjSrv_ }`; masked
  `{ InstanceReadSrv(f), CasterGroupSrv(), GroupMaskSrv(), pageProjSrv_ }` followed by
  `MaskedAlbedoCount()` albedo handles, via
  `StageSrvUavTable(std::array<D3D12_CPU_DESCRIPTOR_HANDLE,20>&, 4 + count)`. `ctx.srvTable[3]` is
  no longer set on this path;
- pass `gPageIdShift = 22u` to both the setup and the scatter CB;
- replace the loop with:

```cpp
const float poolTexels = static_cast<float>(vsm::kPoolTexels);
D3D12_VIEWPORT vp{ 0.0f, 0.0f, poolTexels, poolTexels, 0.0f, 1.0f };
D3D12_RECT     sc{ 0, 0, static_cast<LONG>(vsm::kPoolTexels), static_cast<LONG>(vsm::kPoolTexels) };
cl->RSSetViewports(1, &vp);
cl->RSSetScissorRects(1, &sc);
renderer->ExecuteIndirect(cl, sig, vsm::kPoolPageCount * groups, pageDrawArgs_.Get(), 0, nullptr, 0);
```

- gate the `residentReadback_` copy + the `residentSet` skip (`:955-968`, `:1065`) behind
  `!singleDraw`. Do not delete them — they serve the loop path. `g_residentIterOnly` keeps its
  meaning there.

**Verify:** both 0/0; VSM-mode `--scene-stress-gbv=120` CLEAN except the known-noise ids. Visual
A/B on the flag: shadows identical, and specifically **no seams or bleed at page borders** — check
the dev-window pool grid and the clipmap level boundaries, where a clip-plane/scissor mismatch
would show as a one-texel strip. Also confirm the artifact this buys: sweep the camera fast and
verify newly resident pages no longer flash unshadowed for a few frames with the flag ON (with it
OFF and `g_residentIterOnly` ON, they still do). Capture `Pass_VsmPageRender` **CPU and GPU**,
avg + max, in both flag states, on healthy clocks only (>2500 frames — see the throttling caveat
in `docs/rt_shadows_integration_plan.md:715`).

### Step 4 — compacted args + count buffer (optional, only if a measurement asks for it)
With the page id in the instance stream, record order is free. The setup can `InterlockedAdd` a
counter and append only non-empty `(page, group)` records; the draw then uses that counter as
`ExecuteIndirect`'s count buffer (already supported by the wrapper, `Renderer.cpp:983`; the count
buffer must be transitioned to `D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT`). This cuts ~65 536
records to a few hundred. Treat it as a separate commit with its own before/after numbers, not as
part of the flip.

**Do not assume this one is free to skip.** The "empty records are effectively free" evidence is
`g_residentIterOnly` skipping ~45 % of the pages for a ~2 % GPU change — but 2 % of a ~1.5 ms pass is
~30 µs for 45 % of the records, which puts the whole set at ~60-70 µs of command-processor time
rather than at zero. And because that flag is ON today (see the note in "Honest expected win"), the
current baseline only walks ~55 % of the records while the single-draw path walks 100 % of them. So
the Step 3 A/B can show a GPU regression that Step 4 is the fix for. Measure `Pass_VsmPageRender` GPU
with `g_residentIterOnly` in BOTH positions on the flag-OFF baseline before judging the flip.

---

## Risks / notes

- **`SV_ClipDistance` vs the scissor.** Clip planes cut geometry at the exact NDC cell edge; the
  scissor cut pixels on the same cell boundary. Expected pixel-identical — a mismatch shows as a
  strip at page seams. This is the primary visual check of Step 3.
- **Depth bias is unaffected — say so before someone "fixes" it.** The viewport grows 32x while the
  clip position shrinks 32x, so a page's content still lands on the same 128² pixels and `dz/dpixel`
  is unchanged. The PSO's `DepthBias`/`SlopeScaledDepthBias` (`ConfigureShadowPipeline`) and
  `vsm::g_clipmapDepthBias` therefore keep their current meaning; do not re-tune them for this flip.
- **Clipping can disable rasterizer fast paths**, which is the most likely source of a small GPU
  regression. That is what the flag is for; measure both states. The term to watch is casters that
  STRADDLE a page border: each one is now geometrically clipped against up to 4 planes *in every page
  it lands in*, where the scissor used to reject it for free — and on the coarse clipmap levels a
  single palm covers many pages. Measure on the 610-palm grove, the same scene the per-page cull and
  the LOD-bias work were measured on.
- **Bit budget.** 22 bits of caster slot vs ~800 today is a wide margin, but `--scene-stress`
  variants inflate the caster set — the runtime guard in Step 1 must stay.
- **Non-mega path** (`useMega == false`, heterogeneous meshes) cannot collapse: it binds VB/IB per
  mesh group. It keeps the loop, unchanged.
- **Profiling granularity.** One draw is harder to inspect per page in PIX; flag OFF restores the
  per-page view.
- **Wind divergence.** The `VSM_PAGE` VS must read the wind from the `pageProj_` slot
  (`vsm_page_setup_cs.hlsl:129-130` writes bytes 192/208), not from `b1`. Any drift from
  `WindTransformH` detaches shadows from the gbuffer sway — share one function, do not fork it.
- **Rollback.** Steps 0-2 are dormant (flag, extra CB field passed as 0, unused SRV + PSOs);
  reverting Step 3 alone restores the per-page loop with everything else in place.
