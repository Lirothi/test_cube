# Graphics Scalability -- Groups, Levels, the Player's Settings

Moved verbatim from `AGENTS.md` on 2026-11-24, when that file had grown to 513 KB; `AGENTS.md` now holds the rules in
short form and an index of these notes. This note keeps the full text: design, measurements, history and the reasons
behind each rule. Section titles are the old ones, so an old reference «AGENTS.md "<title>"» lands here.

## Graphics Scalability -- Groups, Levels, the Player's Settings

2026-11-13 (owner: "scalability as UE has it -- categories with their levels, and above them low / medium / high / ultra;
a graphics menu for the release"; then "do the plan"; docs/scalability_plan.md). `sources/app/settings/Scalability.*`, the
table `data/config/scalability.json`, the menu `sources/app/ui/GameSettingsMenu.*`.

- **The table is data**: 9 groups (viewDistance, shadows, gi, reflections, water, volumetrics, postProcess, textures,
  effects) x Low / Medium / High / Ultra; a cell maps `--set` names (ApplySweepValue -- one name table) to a number, to
  "level" (the level's own value) or to {"max": n} / {"min": n} (a ceiling / floor over it -- only on a knob the LEVEL sets).
  `Scalability.cpp kKnobs` lists the knobs a table may name, whether the level sets each, and its cost class (L next frame,
  R render targets, I an idle-GPU reconcile). The render resolution (`sg.resolution`: the DLSS mode Performance / Balanced /
  Quality / DLAA, or without DLSS `aa.renderScale` 0.5 / 0.67 / 0.77 / 1 + `aa.fxaa`) is a choice of its own: a preset does
  not move it. The validator (at load and in the gate; a field of the wrong type is an error, not a crash): a known knob,
  the SAME keys at every level of a group (no sticky key), a key in one group, a ceiling only on a level's knob. A knob
  that would be the same at every level stays out of the table (it would only override the settings file). A new knob: its
  `--set` name, its reader in App.cpp `ReadSweepValue`, its row in kKnobs.
- **NEVER WRITE A CAP INTO A STRUCT THE EDITOR SAVES** (review, 2026-11-14): the boats' spray was capped through the ocean's
  `OceanRenderConfig`, which `LevelDocumentSerializer` writes into the level -- a Ctrl+S at Low would have saved wind_test's
  spray as 0.35 / 0. It is a runtime ceiling now (`spray.rateMax` / `spray.mistMax`, `vfx::g_sprayRateMax`, Scene::Sync
  takes the smaller). The clouds, fog, GTAO, bloom and the sun are saved from the document, not from the scene: their
  ceilings are safe. And a ceiling on a count must cap its partner too: the cloud march takes at least
  `viewSampleCountMin` (wind_test 64) whatever the max says, so Low caps the min as well.
- **Ultra = today** (`--scalability-check`: 0 knobs differ from the boot's values). Nothing saved and nothing asked = not
  managed: no knob is touched (a dev build's picture stays the developer's own graphics section). A group or the
  resolution set back to not managed (`-1`, the tab's Not managed, the menu's Cancel) puts its knobs back: a project knob to
  its value before scalability first wrote it, a level's knob to the level's value -- the gate checks every knob against the
  start. A Developer Controls autosave while groups are managed stores those knobs' pre-scalability values
  (GraphicsSettings.cpp CaptureScalabilityOriginals): a preset never becomes the developer's own file. (A `--set` / MCP
  value still does on the next UI edit, as before.)
- **Layers** (§3.3, all of them since 2026-11-16): compiled < the build's graphics DEFAULTS (`config/graphics_defaults.json`
  in the packages -- what a dev build reads too with them mounted --, else a dev build's `tools/dist/graphics_defaults.json`,
  its source) with the person's own graphics section over them < the level < scalability < MANUAL OVERRIDES < `--set` /
  MCP. An override (Scalability.h `NoteManualValues`, user_settings.json "overrides", by `--set` name) is a project knob a
  managed group sets that holds something else than the last apply left in it -- a Developer Controls edit, the menu's
  Advanced: noted at the next save or apply, kept over any preset, unused while its group is not managed, listed in
  Developer Controls > Scalability ("Set by hand over the levels", a button each) and the menu ("Changed by hand: N, Back to
  the levels"); beside the control itself its reset icon wears a dot -- blue while a graphics level sets it, orange once it
  was set by hand over the level -- and its tooltip names the group and level (`ScalabilityKnobOf(GraphicsControl)` ->
  `Scalability::ManagerLabel`; the reset then goes back to the level's value). Never a pinned knob, and never after a level load (`OnContentReloaded` applies with `noteManual` false: the
  load puts the project CSM back itself -- the first cut read that as csm.filterMode "set by hand", the gate caught it). A
  RESET (every Developer Controls Reset) goes back to the levels: a managed knob the reset moved loses its override and takes
  the level's value (`GraphicsSettingsManager::FinishReset`); the CSM tab's reset takes the build's defaults now, not the
  compiled ones. A knob `--set`, MCP set_settings, `--sweep`, `--shadow-mode` or `--dlss` set is PINNED for the process
  (UE's SetByScalability is the weakest priority); `sg.unpin:1` forgets the pins. After a level load the ceilings are applied
  again (OnContentReloaded); after an editor environment edit the edited type's knobs are re-based on the document's values
  (an Inspector that unticks GTAO at Low writes the 0 the ceiling wrote: without that, Medium turned it back on) and the
  ceilings re-applied quietly (EnvironmentRuntime::Apply).
- **One target recreation per apply** (`Renderer::BeginTargetBatch` / `EndTargetBatch`: the reflection scales, the DLSS
  mode and the render scale in one batch; the fog grid's two knobs in one at BeginFrame): Ultra -> Low stalled 162 ms with
  every target knob recreating on its own, now 94 ms with 1 recreation; any other apply ~75 ms.
- **Switches**: `--scalability=low|medium|high|ultra|auto|off` (Shipping keeps it), `--set sg.<group>:0..3` / `sg.all` /
  `sg.resolution` (-1 = not managed; TRANSIENT, never saved), `sg.reload:1` (the table again), `scalability.autodetect:1`,
  `scalability.gpuTimeScale:<x>`; MCP get_readouts block `scalability`; Developer Controls > Scalability (the levels, each
  knob's table value against its value now, the pins, the last apply; a choice there IS saved).
- **The person's file** `user_settings.json` (`settings/UserSettings.h`; vfs::WritablePath(Settings): the repo root in
  dev, gitignored; `<user>\settings\` packaged; version 2), sections `graphics` (what differs from the defaults: every
  knob Developer Controls persists, frame pacing, mouse smoothing -- WAS graphics_settings.json), `scalability`, `overrides`,
  `autodetect`, `display` (the window), `image`, `controls`. Each writer replaces its own sections and keeps the rest (read,
  changed, temp + rename). **graphics_settings.json is read ONCE** -- when there is no graphics section yet, its values less
  the defaults are moved in (the round trip checked before it is written; a dev build's full snapshot and a packaged one's
  differences alike; the owner's 2026-11-16 file: `{"frame": {"vsync": true}}`, the same frame within the processes' own
  spread) -- and never written again; a git pull or an edit of it changes nothing after that. Not in git since 2026-11-17
  (.gitignore; owner: it showed as a change in every status): each tree keeps its own, a fresh clone has none and starts
  from tools/dist/graphics_defaults.json. EVERY `graphics_settings.json
  <key>` IN THIS FILE NOW MEANS THAT KEY IN user_settings.json's graphics SECTION. Written by the settings menu's Apply /
  Back, Developer Controls (its autosave, the Scalability tab) and a packaged build's first-start auto-detect -- never by
  `--set`, MCP or `--scalability=auto` in dev. **A dev test that writes it must BACK IT UP AND PUT IT BACK, never delete
  it**: it holds the developer's own graphics now (deleted, the next boot moves the stale graphics_settings.json in again).
  `record_shaders.py`'s plain arms pass `--scalability=off` against levels left behind.
- **The window** (`settings/DisplayMode.*`, S6): windowed or borderless over the whole monitor (no exclusive fullscreen,
  §10 item 10), which monitor (`EnumDisplayMonitors`, by device name; the primary when it is gone), the windowed client size
  (standard sizes that fit the work area, 16:10 and wider, and the current), the frame-rate limit while another window has
  the focus (a packaged build 30 by default, a dev build none: a script's editor in the background keeps its pace; not while
  auto-detect measures -- an idle GPU clocks down). Read BEFORE the window is made (App::InitWindow; nothing saved = the
  window as it always started, `--window=` stronger and never saved); changed live by the style and SetWindowPos at the
  frame's safe point (afterFrame), whose WM_SIZE takes the old road -- the pending size is one atomic word now (the game
  half pumps WM_SIZE, the render thread's BeginFrame takes it). WM_DPICHANGED: the UI rescales by the new DPI over the boot
  monitor's (the panels keep their 1.5 there), a dragged window takes Windows' suggested rect. `--set display.mode:0|1`,
  `display.width` / `display.height` (two in one frame build on each other), `display.monitor:<n>` (1 = the first),
  `display.backgroundFps` -- transient. Checked on a desktop of its own (a borderless window must not cover the owner's
  screen): borderless 3840x2160, windowed 1600x900, a saved borderless and a saved 1920x1080 at boot; the background limit
  started unfocused (`run_noactivate.py`-style, SW_SHOWNOACTIVATE): 92 frames in 6.3 s at a limit of 15 (`frame pacing: in
  the background` in the log; the HUD's FPS shows the frame's work, not the pace). THIS MACHINE HAS TWO MONITORS (the
  owner, 2026-11-16 -- the first check took one for granted): \\.\DISPLAY1 "LG ULTRAGEAR+" primary at (0, 0) and
  \\.\DISPLAY2 "LS27D70xE" to its left at (-3840, 0), both 3840x2160 at 144 dpi. Checked on a desktop of its own
  (scratchpad-style `monitor_tests.py`: the window's rect sampled by EnumDesktopWindows): borderless moved live to the
  second and back (`--sweep=display.monitor:2,1;display.mode:1,1`), windowed 1920x1080 centred on the second
  (-2891, 512), a saved `\\.\DISPLAY2` makes the window there, a saved monitor that is gone gives the primary. A monitor
  is named by its EDID (QueryDisplayConfig, `DisplayMode.cpp FriendlyNames`; the driver's string was "Generic PnP Monitor"
  for both) and where it stands from the primary ("2: LS27D70xE, 3840x2160, left"). Another DPI is not checked: both are
  150 %, and changing one is the owner's (a Windows setting).
- **Image and controls** (S7): brightness `render::g_playerExposureEv` (a KNOB, -2..+2 EV, added to the level's metered and
  manual compensation in SceneRenderer_Post -- never into the level's CameraExposureSettings, which the editor saves; +1 EV:
  the frame's mean 112 -> 152), the field of view `g_cameraPlayerFovDeg` (horizontal at 16:9, Hor+: the vertical kept at
  any aspect; 0 = the level's, the projection bit for bit as before; the camera's `GetHFov()` is what the projection took),
  mouse sensitivity and inverted look (`Camera::AddLookInput`, the one road). `--set image.brightnessEv / image.fovDeg /
  controls.sensitivity / controls.invertY`. **The inverted look is the person's setting, never a binding** (2026-11-17,
  owner: "my Y invert flies into the build"): `input/bindings.json` carried LookY `"invert": true` since 2025-10 -- with
  DirectX's pitch (positive = DOWN) and raw mouse y (positive = toward the person) that is the flight-style look, so every
  distribution shipped the developer's taste and the menu's "Invert vertical look" turned it NORMAL. LookY is plain now;
  invertY true = the mouse pushed away looks down. A build with the editor has no game menu: Developer Controls > Frame
  "Invert vertical look" / "Mouse sensitivity" (user_settings.json controls, saved at once / when the slider is let go;
  `settings::SaveControlsSettings`). The owner's own invert lives in the owner's user_settings.json (gitignored), not in the tree.
- **Auto-detect** (§5): the real frame -- High at Balanced, 60 frames and 5 s to settle (1 s for the next levels), the
  median of the GPU frame's OWN times over 3 s (the profiler's `OverlaySample::lastMs`; its `avgMs` is an EMA of 0.997 a
  frame, ~333 frames to settle, and measured a level as a blend with the one before -- Medium 3.55 instead of 3.2 ms, the
  reviews of 2026-11-14); within 0.6 of the budget (60 fps less 30 %, 11.7 ms) Ultra is tried, over it Medium, Low, then the
  resolution down; under 6 / 4 GB of VRAM the textures stay at Medium / Low; one frame over 1 s stops it at Medium. A
  packaged build runs it at its first start on a SYSTEM -- no measurement saved, another GPU (its name -- a driver update
  keeps the player's levels), or another computer (the record's `system`: the CPU's brand string and the installed memory,
  App.cpp `SystemIdentity`, since 2026-11-17; a record without it is judged by its GPU alone) -- and saves it.
  **That run is FORCED** (owner, 2026-11-17; `Scalability::AutodetectForced`, any run that saves -- `--scalability=auto`
  in a packaged build too): a card over the frame says what happens and which level is measured (AppController
  `DrawAutodetectScreen`, the HUD line "SETTING UP THE GRAPHICS"), the player, the free camera, F8 and the Esc menu wait
  (the boot's own Play still starts: its view is the one measured), nothing cancels it (`AbortAutodetect` ignores it), and a
  quit before the end saves nothing, so the next start measures again. Checked on the release distribution with the
  Development exe and an empty `--user-dir`: the card at 7 s; quit there -> no record, the next start measures; a full run
  -> the record with `system`; the next start does not measure; the record's system edited -> "another computer", it
  measures; `system` taken out -> it does not; Esc posted at 4 s opens nothing during it, the same Esc after it opens the
  menu. `--scalability=auto` in a dev build, MCP `scalability.autodetect:1` and the menu's button (kept by Done, dropped by
  Cancel, which also stops a measurement) do not save and are not forced. This machine (RTX 4090): Ultra (High 4.25,
  Ultra 4.56 ms); `gpuTimeScale:4`: Low (High 16.9, Medium 12.9, Low 9.9).
- **The menu**: Esc in EVERY build without the editor (Release too since 2026-11-16; F1 stays Developer Controls; the
  mouse is the menu's while it is up): Continue, Settings, Developer (not in Shipping: Play from the Player Start or the
  camera / Stop playing, the editor's Play and Stop without the editor), the level as ONE drop-down with Load / Restart
  (owner: "a drop-down, not a million buttons"; a dev build lists the tree's data/levels, a packaged one its packages'),
  Quit.
  `--settings-menu[=display|graphics|image|controls|menu]` opens a tab (or the Esc menu) at boot for a `--shot`. Settings:
  Display (mode, monitor, size, VSync, the limits; a change of the window asks "Keep these display settings?" and goes back
  after 15 s), Graphics (auto-detect, Quality -- Custom while the groups differ --, render resolution, the groups with "*"
  where a change pauses, what was changed by hand, Advanced: nine safe knobs set as overrides), Image, Controls (the game's
  keys to read). Applied at once behind it; Apply and Cancel show while something differs from what was there, Back keeps
  and closes, Esc puts everything back and closes; Apply waits while auto-detect measures. Its own look (`GameMenuStyle`):
  a font of 20 px a 1080 line (27 at 1440, 40 at 2160) whatever the panels' 1.5, a dark backdrop, colours given as sRGB and
  linearised (the UI draws into an sRGB target: taken as linear they came out washed grey-blue). Every word in
  `ui/GameMenuStrings.h`.
- **Cost** (wind_test's start view, 18:00, 1440p DLSS Balanced, the GPU frame, two runs each, re-measured 2026-11-16 after
  the review fixes through MCP, scratchpad-style `preset_cost.py`): today 4.02-4.09, Ultra 4.03-4.08, High 3.89-3.91, Medium
  3.25, Low 2.44-2.45 ms -- High 96 %, Medium 80 %, Low 60 % of Ultra. A preset takes 4-40 % off; the render resolution, the
  player's own choice, is the larger lever. High's first cut (UE High's VSM bias 0, solid foliage in RT) cost MORE than Ultra here:
  measure a cell before believing it.
- **Gates**: `--scalability-selftest` (no window: the table parses and validates; the control arms -- a misspelt knob, a
  sticky key, a knob in two groups, a ceiling on a project knob, a level missing -- each refused; the ceilings through
  Apply: Low, Ultra gives the level's value back, a level edited to the ceiling's value keeps it, not managed puts a project
  knob back; the overrides: one noted, kept over another preset, taken away by a reset, unused while not managed, never a
  pin); `--scalability-check` (the running game: every level of every group applied and read back, the level reloaded
  at Low and the ceilings holding, then not managed again and every knob against the start -- 320 reads on wind_test; a
  reload that never finishes FAILs after 3000 frames; exit = the failures).
  `tools/dist/record_shaders.py` records per preset (Low / Medium add SDSM's and the sky occlusion map's permutations).
- **S3** (2026-11-16): `textures.anisotropy` (UE r.MaxAnisotropy; render::g_textureAnisotropy, every material sampler's
  AnisoWrap, live -- the samplers are staged a frame by description; Developer Controls > Streaming, graphics
  `streaming.anisotropy`; the Textures group 4 / 8 / 16 / 16; 1 against 16 moves 13.9 % of the start view's pixels, the
  same arm twice 0.24 %); the GI probe volumes FREED when a knob turns them off (`SceneRenderer::ReleaseUnwantedGi` at
  Scene::Render's reconcile, the GPU drained; made again when wanted): gpu local 2111 MB with GI, 2038 without (-73), 2068
  without the far cascade (-43) -- the scalability gate frees and remakes them twice, narrowed GBV
  (`GiTrace,GiUpdate,Lighting,Skybox`) clean; `shadow.localMaxRes` and `vsm.resolutionLodBiasLocal` persisted (graphics
  `shadows.localLightMaxResolution`, `vsm.resolutionLodBiasLocal`). The same evening, the bloom's FFT grids (3 x RGBA32F at
  the padded grid, 96 MB at 1440p, 192 at 4K) shrink to a 16x16 placeholder while the convolution method does not run
  (`Renderer::SetBloomFftResidency` at Scene::Render's reconcile: the GPU drained, the views rebuilt, the targets'
  generation bumped so the kernel spectrum is rebuilt; `DeferredTargets::bloomFftResident` gates the convolution; kept
  across a target recreation), and the clouds' fixed resources go with `cloud.enabled` 0 (`SceneRenderer::
  ReleaseUnwantedClouds`, VolumetricCloud::Reset; Prepare makes them again). Measured in the live editor (`get_memory`
  tags): Low bloom 184 -> 88.5 MB and GI 73 -> 0, back at Ultra 184 / 73, gpu local 2135 -> 1599 -> 2135 MB in both
  rounds; clouds off 51.4 -> 12.4 MB (the per-frame trace targets stay), on again 51.4. The FFT bloom after its grids were
  made again against the same arm twice at a pinned exposure: 2.5 / 2.9 % of pixels over 8 levels against 2.3 / 3.0 %; no
  bloom at all (the control) 7.6 %.
- Not done (plan §9): the foliage density (plan §4.4) -- wind_test's plants are 1650 staticMesh copies, no instancedModels,
  so thinning them means hiding copies of chosen assets (a mesh.json key) with their shadows and collisions: the owner's
  call which assets.
