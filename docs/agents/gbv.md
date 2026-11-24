# GPU-Based Validation -- How to Run It, Incidents, Tools

Moved verbatim from `AGENTS.md` on 2026-11-24, when that file had grown to 513 KB; `AGENTS.md` now holds the rules in
short form and an index of these notes. This note keeps the full text: design, measurements, history and the reasons
behind each rule. Section titles are the old ones, so an old reference «AGENTS.md "<title>"» lands here.

## GPU-Based Validation (GBV) — How to Run It

Debug only (the debug layer exists only there). GBV rewrites each shader on the CPU the first time
its pipeline is bound, so the cost is minutes, not milliseconds: a full wind_test frame waits
**155-161 s** for its first frame, Lighting alone ~150 s of it. Pick the run by the question:

- **"Is the pass I changed clean?"** -- narrow it:

  ```bash
  test_cube.exe --level=data/levels/wind_test.json --gbv --gbv-passes=CloudTrace --no-streamline --shot=out.png --shot-delay=3
  ```

  Guarded validation on the listed passes' command lists, STATE_TRACKING_ONLY on the rest: **16 s**
  to the first frame. Names are `RenderPassToWString`'s (CloudTrace, Compose, Lighting, RTTrace, ...),
  case-insensitive; `GBuffer` also covers `GBuffer.*`; a CL group's members are patched together
  (logged). It says NOTHING about the unlisted passes -- their shaders are not validated.
- **"Is the frame clean?"** -- the gate is `--scene-stress-gbv` (guarded, every pass, level churn;
  it ignores `--gbv-passes`; a Debug build boots the editor's last opened level first, `--stress-level=<level.json>` names
  it and adds it to the churn), or a plain `--gbv` without `--gbv-passes` for one level with your
  `--set`s. Minutes; run it before trusting a pass/barrier/resource change.
- **Under GBV a resource's creation and a submission exclude each other** (2026-11-06; `gpu::SubmissionScope` round every RHI op, `CreationScope` in the `gpu::Create*` wrappers; `--set=frame.gbvCreationExcludesSubmission:0` = the A/B, which loses the device). GBV's layer runs an ExecuteCommandLists of its own from inside `CreateCommittedResource`, on the creating thread (`gpu::CreateCommittedTexture` -> D3D12SDKLayers -> D3D12Core's execute -> nvwgf2umx); with ours in the same driver function on the RHI thread at that moment, NVIDIA's driver (32.0.16.2101) reports an error and the runtime calls `D3DKMTMarkDeviceAsError` (0x80000006, the driver's). The reason reads `0x887A0005`, no breadcrumbs, no TDR in the system log: the GPU never saw anything wrong. Without the RHI thread the two never met. It takes both: a long Execute (a patched pipeline's first frames: any level with a spot in its first frames under `--gbv-passes=SpotLights,Skybox`) and a creation that makes the layer submit (the texture streaming's first new textures, frame 7, 3 ms into it; `streaming.enabled:0`, a 1 MB ring that only lets small ones through, or a spot added 30 s in, when the streaming has finished, all lived and each read as a cause for a while, as the rays did). local_shadow_test: without the lock 11 of 12 runs lost the device at frame 7, with it 0 of 3, id=935 caught, nothing alive at exit. wind_test with two spots and two points, and with the group "Lamps" (`VsmPageRequest,VsmPageRender,PointLights,SpotLights,Skybox`): alive and clean; `--scene-stress-gbv=20 --stress-level=data/levels/local_shadow_test.json --barrier-cmp --canonical-check`: CLEAN. The same frames still go to the queues in the same order, and the recording still overlaps them: what GBV sees of the RHI thread's lag stays.
- **`id=939` / `id=940` in the all-list gate with the RHI thread: twice on 11-06, never since, and the constants checked intact.** On the night of 2026-11-06 (843e4d23's code) `--scene-stress-gbv` on local_shadow_test reported them in 2 of 2 runs, once, in the first frame's messages: `rt_resolve_cs.hlsl:25` (list Compose, heap index 117, a UAV or a BUFFER SRV where a Texture2D is read) and `rt_reflect_common.hlsli:430` (GlassReflections, index 1, a BUFFER) -- both inside slot 0's frame descriptor ring (0..4095), where the constants held 12302 and 12311; the verdict said CLEAN. On 2026-11-07, the same code: 0 of 6 full gates (`=2` five times -- bare, with the ring check alone, with the GPU copies alone, twice with both --, `=20 --barrier-cmp --canonical-check` once with both). What differed from that night was not found: the DXIL and PSO caches were warm then too (the second run wrote no PSO), UE had closed at 22:17, the settings files had not moved. `--upload-ring-check` (next item) in five of those gates, 1 000-4 000 checks a plain run: no byte of any frame's upload ring moved between the end of its recording and the GPU finishing the slot, nothing was allocated or staged into a sealed slot, and the three RT reflection dispatches' constants as the GPU itself copied them right after each dispatch equal what the CPU wrote (288 of 288 in the gates). If it comes back: run that gate with `--upload-ring-check=gpu` (the copies alone, the lightest on the timing). A `rt constants: ... the GPU read other bytes` line beside it = the constants moved; none = the layer's report is off (its messages carry garbage in `Asm Operand Index`, the self-test's id=935 included). A run takes 11-12 minutes, 6.5 of them to the first frame. 2026-11-09, the same kind: the FIRST frame of a narrowed run (`Transparent,Transparent.Water,Skybox`, the editor, the yacht's spray emitters made in that frame's Sync) once reported id=1387 -- a particle draw (`Translucent.Simple`) whose t0, the emitter's own CPU heap, held `SkyAtmosphere.Transmittance`, the sky LUT table's first entry, rebuilt in that frame -- and id=1358 (`Ocean.ShoreDepth` in COPY_SOURCE in the ocean's VS); 1 of 3 runs on that build, nothing in the other two (one of them `ObjectCompute,Transparent,Transparent.Water,OceanWake,Skybox`). If it comes back, `--upload-ring-check` names a descriptor table staged after its slot was sealed. 2026-11-16, the same first frame: `Tonemap,CloudTrace,Skybox` + a `--sweep` of bloom.method / cloud.enabled reported id=1358 on `BloomKernelStarAnamorphic.dds` (Tonemap) and `VolumetricCloud.BlueNoise` (CloudTrace) in COPY_DEST -- both uploaded in that frame, before any value of the sweep moved anything; the same run without the sweep: only id=935.
- **`--upload-ring-check[=gpu|selftest]`** (2026-11-07; `FrameResource::SealUpload` / `VerifyUpload` / `NoteFilled`, `DescriptorAllocator::SealForCheck`, `SceneRenderer_Reflections.cpp ProbeRtConstantsOnGpu`): a frame's upload ring (`AllocDynamic`: every per-frame constant buffer) is sealed when its recording ends and compared before the queue takes it, after ExecuteCommandLists and once the GPU has finished the slot; a block filled through `UploadFrameCB` / `RecordComputeDispatch` / the RT reflections is compared from its fill on; an allocation or a descriptor table taken from a sealed slot is reported with its call stack (`tools/symbolize.py`); the RT reflection dispatches' constants are copied by the GPU right after each dispatch and compared when the slot comes round. Each report names the allocation the bytes lie in and the code that made it (and the slot's allocation there a frame before), with the old and new words. At exit: `upload ring check: N checks, M bytes moved, K allocations and T descriptor tables after a seal` and `rt constants as the GPU read them: N matched, M differed` -- Error when anything moved. `=gpu` = the copies alone (the ring check takes a call stack per allocation, Debug milliseconds of recording a frame, which moves a race's timing); `=selftest` breaks four things on frame 3 -- a filled block, a sealed canary, a descriptor taken after the seal, the resolve's unread pad behind its GPU copy -- and each must be reported (proven). Clean: local_shadow_test and wind_test in Debug, wind_test in Release (4 863 copies), the gates above.
- **A device removal says when and who** (`rendering/core/DeviceRemovalWatch`): a fence reaches UINT64_MAX at the removal, a thread that sleeps on it logs `device removal, the moment: reason ...` and every thread's stack by module, `python tools/symbolize.py x64/Debug/test_cube.exe <log or "module+0xoffset ...">` reads them (the engine by its PDB, a system DLL by its exports, no symbol server). Under GBV and `--dr-check` the stacks are taken within microseconds and `D3DKMTMarkDeviceAsError` is hooked in win32u: `device marked as error, reason 0x..., by: <stack>`, the thread and the call that ended the device; other runs get the moment and stacks tens of milliseconds late. Every other report is the first call that happened to notice (`[texcreate] ... FAILED`, `RT off: ... Map failed`), and what follows it (`id=907`: Present failed, so the back buffer did not advance; `std::terminate`) is the wreck, not the cause. DRED is always on in Debug, so `--dred` changes nothing there. Reason `0x887A0005` with no TDR event = look for that line first.
- **Never gate on `--gbv-mode=state`**: it is falsely clean on uninitialized root arguments (the bug
  class that stalled the queue). `--gbv-rest=none` is the fastest narrowed smoke; the runtime then
  calls its state tracking "possibly imprecise" (id=1044) from the first unpatched list on.
- **Prove a mode catches before trusting its silence**: `--gbv-selftest=1@<Pass>` injects an
  uninitialized-root-argument draw into that pass. Listed -> `ERROR id=935`. The line
  `gbv self-test: injected ... into command list '<Pass>'` must be in the log; without it nothing was
  injected and the silence proves nothing. Aim it at a list whose shader READS the tables it unbinds:
  `Skybox` is proven (id=935); `TerrainDepth` (gbuffer_csm) takes the injection and stays SILENT (2026-10-24),
  and a compute pass (GiUpdate, PointLights, ...) cannot take one at all -- list Skybox beside it as the run's
  catcher.

Reading it: errors land in the session log as `[ERROR][render.validation] ERROR id=...` (and in
`logs/gbv.log`); `[render.rhi] [gbv] requested patch mode = ... device default = ...` and the
`--gbv-passes:` line say what was actually patched. **A clean run has NO `render.validation` warnings**
(2026-10-20, owner: "I need a clean GBV"): under GBV the G-buffer draws its groups one ExecuteIndirect each
(`GraphicsDevice::GbvEnabled`, ShadowGpuData -- the merged bindless commands change VB/IB/root bindings per draw,
which GBV cannot track: that was the old `id=1044` on `GBuffer.Indirect`), and the exit report collapses to one Info
line `live objects at exit: none` when only the device, held by the report's own interfaces, is alive. So any
`id=1044` or `Live ...` WARNING is now a finding. A Debug run whose log just STOPS with an odd exit code is the debug layer's
break-on-error with no debugger: rerun the same line with `--gbv --gbv-mode=none` and the message
lands in the log.

Timing GBV: consecutive runs warm a patched-shader cache, so alternate A and B and compare second
runs. Look at the `--shot` before trusting a time -- a frame missing a mesh measures a different
frame (a worktree without `import_staging/sandy_gravel_1k/textures/*.dds` skips the island's draw).
Run it in a normal visible window: a minimized one has a degenerate backbuffer, so neither its time
nor its shot is the frame you think. Numbers and the trade-offs: `docs/gbv_startup_profile.md`.

**The PSO cache is one folder per device flavour and driver**: `shader_cache/pso` (Release, Release_Editor),
`pso_debug` (Debug: the debug layer), `pso_gbv` (Debug + GBV), each holding `<vendor>-<device>-<driver version>/`
(`Material.cpp PipelineCacheDirectory`, logged once as `PSO cache: <folder> (...)`). A driver refuses a blob stored
behind another layer or by another driver (`0x887E0002`, the debug layer's `id=886` "a cached PSO intended for a
different driver version"), so one shared folder ping-ponged: a Debug wind_test run after Release_Editor refused 69
of 158 blobs and overwrote them for the next Release run to refuse (2026-10-27). A driver update now starts an empty
folder and deletes the others; a refused blob is still compiled again and replaced (`PSO cache: the driver refused a
stored blob`, once a process). Measured after the split: Release / Release_Editor (Streamline on or off) / Debug /
Debug + GBV in any order -- all hits, 0 rejects. Neither `--dred` nor the GBV device's patch mode changes a blob.
