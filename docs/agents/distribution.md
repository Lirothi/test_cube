# Distribution -- VFS, Packages, Shipping

Moved verbatim from `AGENTS.md` on 2026-11-24, when that file had grown to 513 KB; `AGENTS.md` now holds the rules in
short form and an index of these notes. This note keeps the full text: design, measurements, history and the reasons
behind each rule. Section titles are the old ones, so an old reference «AGENTS.md "<title>"» lands here.

## Distribution -- VFS, Packages, Shipping

Plan and every measurement: `docs/distribution_build_plan.md` (§13, D1-D9; owner, 2026-11-13: "a build system for
distribution: packages, our file system over packages and direct paths, encryption, whatever else it needs").

- **Every runtime read goes through `core/vfs/Vfs.h`** (`vfs::ReadAll / ReadText / InStream / Open / GetStat / Exists / List`),
  never `std::ifstream`, `_wfopen`, `CreateFile`, WIC from a file name, `std::filesystem::exists` on content.
  `python tools/check_file_io.py` (exit = findings) gates it; a line that must touch the disk (the user's settings, a cache, a
  tool rewriting an asset) says why with `// vfs-ok: <reason>`. Paths are virtual: forward slashes from the content root
  (`vfs::ContentRoot()`, the cwd in dev); an absolute path goes straight to the OS. `GetStat().version` is a loose file's
  write time and a packaged file's CONTENT HASH -- a key built from it (the water mask bake, a physics shape) must not
  assume FILETIME. A derived file is written beside its source only when `vfs::IsLooseFile` says a loose file serves it.
- **Mounts**: `--vfs=loose|packages|both`, `--pak-dir=<folder>` (every `*.tcpak`, by priority then name), `--pak-key=<key.json>`
  (or `TC_PAK_KEY`), `--vfs-record=<file>` (each content file found, once). `.tcpak` (`core/vfs/Pak.h`): 64 KB blocks, zstd
  or stored, CRC32C each, AES-256-GCM through CNG when encrypted (HKDF sub-keys, synthetic nonce, AAD per block), TOC at the
  end, deterministic (same files -> same bytes), "deleted" records for patches. Tools (no window): `--pak-build=<out>
  --pak-files=<list>`, `--pak-list=`, `--pak-verify=`, `--pak-keygen=<key.json>` (never over a file), `--pak-key-header=
  <key.json>,<out.h>`, `--vfs-selftest` (134 checks with their controls). A package that opens is checked whole: the
  block layout Read relies on, the header's TOC size against its zstd frame, a disk read error on the mapped view read as
  damage (`EXCEPTION_IN_PAGE_ERROR`), not a crash; no entry is larger than all the package's blocks (2026-11-18: a size
  within 64 KB of 2^64 wrapped the block count's sum, the entry passed with no blocks and Read indexed the block table
  out of range -- the self-test's control arm fails without the check).
  **A build that carries a key mounts only packages encrypted with a key it has** (2026-11-18, the code review;
  `vfs::Config::requireEncrypted`, unset = on exactly when a key is embedded -- an encrypted distribution -- and off in
  every other build, dev and `--no-encrypt` ones included): a package is mounted by its presence in `content\` and sorted
  by the priority its OWN header claims, so an unencrypted file dropped beside an encrypted game replaced any file of it
  (a level, `config/boot.json`, a cooked shader). Such a package is left out with an Error (`vfs: <file> is not
  encrypted ... left out`), the rest mount; none left = the mount fails and says so. Checked by the self-test with the
  rule set by hand (both ways); NOT yet run on a real keyed Shipping exe -- do that at the next encrypted `build_dist`.
- **Shaders**: `--shader-model=6.6` caps the target (a mounted `shaders.tcpak` sets its own: `shaders/dxil/cooked.txt`);
  `--shader-record=<file>` lists every compile request; cooked DXIL is looked up first (`shaders/dxil/<id>.dxil`, the id
  without absolute paths, Release flags), a miss logs `shader permutation not cooked` once and compiles from the packaged
  HLSL. `tools/dist/record_shaders.py` re-records `tools/dist/shader_list.txt` (264) and `vfs_record.txt`: run it (or
  `build_dist.py --mode=record`) after adding a shader permutation a shipped level can reach, and review the diff. The list
  names the levels its runs went over (`# levels:`); `build_dist.py` warns and the Build Distribution window says which of a
  build's levels are not among them (their permutations may compile at the player's).
- **The cook** (`Cook|x64`, `x64/Cook/test_cube.exe --cook=tools/dist/<profile>.json --cook-out=<dir> [--cook-check]
  [--levels= --boot-level= --version= --no-encrypt --pak-key=]`, `sources/app/dist/Cook.cpp`): walks the profile's levels
  (strings with a slash and a content extension, bare names as `data/materials/<name>.json`, derived files), the
  `include`, the record list and string literals of `sources/` outside the editor; re-keys water mask bakes for the
  package (a bake made for another write of the geometry is REBUILT from its stored draft and glass slots; one keyed with
  another draft than the asset's mesh.json vessel says now is REBUILT with that draft -- the glass slots are the materials'
  and are not judged by the cook; one that cannot serve -- an old bake version, a hull no longer masked -- is LEFT OUT with a
  Warning: the game builds it at load); errors
  on a level in a folder no package carries (`logs/`, `dist/`, ...: it would ship as nothing), on a missing file and -- with `strictLicences` -- an asset without a licence in CREDITS.md (a
  `## <asset>` section with `license type:` or `License:`). It writes `cook_report.json` (every file, who named it, sizes,
  per level, licences) and `base.tcpak` + `shaders.tcpak`. While it compiles shaders, a Debug exe uses Release flags too.
- **Shipping|x64** (`TC_PACKAGED=1; TC_SHIPPING=1`, no rendering change): content from `<exe>\content\*.tcpak` only; the user's folder
  (`%LOCALAPPDATA%\test_cube` or `--user-dir`) is created and becomes the CURRENT DIRECTORY before the log opens, so
  everything written relative (logs/, `settings/imgui.ini`, artifacts, `crashes/`, `--shot`) lands there;
  `WritablePath` maps Settings / Cache / Saved (`data/bot_memory/` and `data/ocean/` -- Development's F7 presets, Save
  goes through `WritablePath(Saved)` -- read back through the user overlays, `vfs::Config::overlays`). Switches: only
  `--window --level --log-level --user-dir --shot --shot-delay --cam-pos --cam-rot --wind-freeze --time --play
  --crash-selftest` (the rest is logged as ignored; `--name value` is read as `--name=value`). No developer keys or
  windows -- the Developer Controls' code is not even compiled in (DeveloperWindow.h is a no-op stub under TC_SHIPPING, the
  windows it owns left to the linker: the exe 9.67 -> 9.13 MB); Esc is the game's menu (Continue, the levels the packages hold -- `data/levels/*.json` --, Quit;
  `AppController::DrawGameMenu`). Starts by `config/boot.json` (the profile's `bootLevel` / `startMode`); with `startMode`
  "play" every level goes by its own rule (owner, 2026-11-13; `g_playByLevelRule`): a Player Start -> first person from it,
  none -> the free camera at its `freeCameraStart`, none -> the free camera at the origin, level (F8 still walks from there).
  An absolute path is read only inside the game's folder and the user's (`Vfs.cpp AbsoluteAllowed`: `--level=C:/x.json`
  is refused, logged once) -- the owner's "no loose files" (§11), enforced. Graphics settings: defaults from `config/graphics_defaults.json`
  (`tools/dist/graphics_defaults.json`), the player's file holds only the differences (user_settings.json's graphics
  section since 2026-11-16, "Graphics Scalability" below); every build saves it atomically.
  Before the window: `D3D12\D3D12Core.dll` present (the Agility SDK 1.619.6 runtime it exports `D3D12SDKVersion = 619` for:
  Windows 10 has no SM 6.6 in-box, and without the file no device is made even on Windows 11 26100), the GPU check (SM 6.6,
  binding tier 3, DXR 1.1 -- `gpu::CheckRequirements`, `--gpu-check` in dev), NVIDIA's signature on the delay-loaded
  `sl.interposer.dll`. Crash reports (`core/diagnostics/CrashHandler.h`): a minidump in `crashes\`, a Fatal line, a box (the engine's own long-lived threads -- the task workers, render, RHI, the physics step and Jolt's jobs, the texture streaming IO -- call `InstallThisThread`: std::terminate's handler is per thread in the MSVC CRT, and each gets the 64 KB stack guarantee);
  `--crash-selftest=av|stack|abort|terminate|throw|purecall|invalidparam|fastfail|overflow` (fastfail: no report --
  nothing in the process runs; overflow: a `tc::inl_vector` outgrown, below). `TEST_CUBE_MESSAGE_TIMEOUT_MS` closes any
  of the game's message boxes (the gates' way past them).
  **A dev build reports its crashes too** (2026-11-18, the code review; main.cpp, `#if !TC_PACKAGED`): Debug, Release and
  Release_Editor install the same handler -- no message box, minidumps in `logs/crashes` (the newest 10 kept,
  `crash::Options::keepDumps`), the session log's `crash: <what> -- minidump <path>` Fatal line, the exit code the
  exception's (0xC0000005 ...) or 3 for abort / terminate. Every harness and tool runs under it (the log's `crash
  reports: on` line). Not under a debugger (it takes the fault, and abort() must break into it); a CRT assert keeps its
  own road (the report hook ends the process first); scene-stress still installs its own filter over it. Until then a
  dev run that died read as "the log just stops" and was dug out of WER's dumps.
  **`tc::inl_vector` ends the process on an overflow, in every build** (same day): `emplace_back` / `push_back` /
  `resize` past the declared capacity write a Fatal line with both numbers (`inl_vector: N element(s) asked of a vector
  declared for M`, `tc::detail::InlVectorOverflow` in Log.cpp) and abort -- an assert said so in Debug, Release wrote
  past the storage in silence (the task system's dependents, the render graph's lists). A caller that can meet more
  than its capacity checks `size() < capacity()` first and says what it dropped: the glass draws of one object
  (`GBufferRenderable::RenderTranslucent`, 32 now, were 8 unchecked) do.
- **Development|x64 = the packaged game WITH the developer features** (`TC_PACKAGED=1` without `TC_SHIPPING`; owner,
  2026-11-13: "two kinds of build, dev and shipping"; docs/scalability_plan.md §7). Picked at BUILD time, not by hand:
  the Build Distribution window's "Build type" Shipping / Development, `build_dist.py --flavour=development`, MCP
  `build_dist {flavour}`, a profile's `"flavour"`. Everything packaged is Shipping's (content from the exe's packages, the
  Agility SDK, the GPU / Streamline checks, crash reports, boot.json and the level rule, settings as differences, the Esc
  menu); everything stripped is kept: F1 Developer Controls and the F keys, the whole command line (`--set`, `--sweep`,
  harnesses, `--log-level=debug`), the FXC fallback, shader hot reload off (the packages do not change), absolute paths
  allowed, `--pak-key` / `TC_PAK_KEY` for an encrypted package. The tools that need the source tree (`--import`,
  `--reimport`, `--cook`, `--pak-build`, `--tessy-upgrade`, `--lod-report`, `--water-mask-lid`, `--uv-density`,
  `--shader-cook/-record`, `--vfs-selftest`) refuse with an Error, exit 2. It says so: "DEVELOPMENT BUILD <version>" on the
  HUD's first line, " [Development]" in the title, `development` in the log's name; its user folder is
  `%LOCALAPPDATA%\test_cube_dev` (never Shipping's). `--scene-stress` there churns the packages' first three levels when
  the tree's demo levels are not in them. The distribution: x64/Development's exe AND its PDB in the folder
  `<product>-<version>-dev`, never encrypted, the profile's `devInclude` cooked too (default `data/ocean/*.json`, the
  ocean window's presets; not a literal path in `Cook.cpp`: the cook's literal scan would ship it in both), `flavour` in
  the history, the manifest and
  the cook report; `versionMustGrow` counts each flavour apart. Reset (every button) in a packaged build goes back to the
  package's `config/graphics_defaults.json`, not the compiled defaults. `build::Packaged()` / `Development()` /
  `Shipping()` (BuildVersion.h) for code that must tell them apart.
- **Generated headers ALWAYS exist** (`sources/app/generated/BuildVersion.g.h`, `sources/core/vfs/generated/PakKey.g.h`,
  not in git): the project writes dev / no-key ones when missing. Not `__has_include`: MSBuild does not see a header that
  was absent at the last compile, and the first encrypted build's exe shipped without its key. The tree's are ALWAYS the
  dev ones: a distribution's stamped version and embedded key are written under its staging folder and reach only
  `BuildVersion.cpp`, `PakCrypto.cpp` and `test_cube.rc` through `/p:TcGeneratedRoot=` (test_cube.vcxproj); stamped in the
  tree, a killed or cancelled build left the release KEY in every dev build after it. `build_dist.py` repairs a stale
  stamp it finds.
- **`python tools/build_dist.py --profile=<test|release|path> --mode=check|record|content|full`** (`--levels --boot-level
  --version --no-encrypt --flavour=shipping|development --skip-build --out --key --progress=json --keep-temp`): `check`
  and `content` bring x64/Cook up to date first (an incremental MSBuild with the tree's dev headers: 0.5 s when nothing
  changed, ~20 s the first time after a full build; owner, 2026-11-17 -- they took it as it lay, and a stale one ran the
  old cook: a new switch unknown), the game's exe stays as built; `--skip-build` = as they are, and the window's automatic
  size check on opening passes it (no compile for opening a window); `record` builds Release; `full` =
  MSBuild Cook + Shipping (or Development) with the
  version (`0.1.0+<build> (<git>[-dirty])`, the build number from `<out>/build_history.json`) and the embedded key, the
  cook, the folder (exe, the DLLs it LOADS -- sampled from a run, NGX's over-the-air copies mapped back to the local
  `sl.*.dll` they stand in for --, `D3D12\`, the VC++ runtime app-local), a check run of the staged game (exit 0, no Error,
  nothing compiled, Streamline up), CREDITS / THIRD_PARTY_NOTICES / README / manifest.json (SHA-256 per file), zip,
  `dist_symbols/<version>/` (exe + PDB + report). Staged in `.dist_tmp` beside the output -- the zip and the symbols made
  there too -- and renamed whole: a cancelled or failed build leaves nothing in `dist/`. Its MSBuild runs `/nodeReuse:false`
  with a PDB server of its own (`_MSPDBSRV_ENDPOINT_`): the job object that ends a cancelled build ends nothing another
  build shares. The version is `a.b.c[-suffix]` (checked in the script, the window and MCP: it is a folder name, a C string
  and a command-line value). The key: `%USERPROFILE%\.test_cube\pak_key.json`, made on the first encrypted build --
  KEEP A COPY; a lost key leaves its packages unreadable. `release` needs `versionMustGrow` and every licence known (until
  the owner settles `rocks` and three textures in `import_staging/`, it builds only with the licences overridden).
  **`versionMustGrow` counts only PUBLISHED builds** (2026-11-17, owner: "if I don't bump the version I cannot build"; until
  then any build used its version up): a build is published when it is handed to players -- the window's History
  "Published" tick, `build_dist.py --mark-published=<build>[,...]` / `--unmark-published=` (the history's `published` and
  `publishedDate`; nothing built) -- and a content / full build then needs a version above every published build of the
  profile and flavour (`version_key`: a.b.c, a release after its pre-releases). A version only built builds again: its
  folder and zip are replaced, its symbols kept apart by the full string (`dist_symbols/0.1.1+3`). The window says it
  under the Version row before a run (red: published, raise it; grey: built before, builds again). **History "Delete"**
  (owner, same day; `build_dist.py --delete-build=<build>[,...]`, a job of the window's: mode `delete`, never MCP's): the
  build out of the history and its files off the disk after a confirmation that lists them -- its folder, zip and kept cook
  report while they are still its own (a later build of the same version replaced them: kept, and the folder's
  manifest.json is read first), its symbols (`dist_symbols/<a.b.c+build>`); only under the output folder and
  dist_symbols/; a published build is refused. `--build-dist=<profile>,delete=<build>` opens the confirmation for a
  `--shot`.
  **One run never touches another's files** (2026-11-24, the code review, docs/architecture_review_plan.md 10.14 / 10.18).
  Each run stages in a folder of its own under `.dist_tmp` and holds `owner.lock` open in it; a run clears only the
  folders nobody holds (a killed run's). Until then EVERY run began by deleting `.dist_tmp` whole -- the check the
  window starts when it opens included, under a build from the console. A content / full build, `--delete-build` and
  `--mark-published` hold `.dist_tmp/writer.lock`: a second writer of the same output folder is refused ("another build
  is writing ..."). An open file is the claim (Windows will not delete it while its process lives, and frees it when the
  process is killed): no pid to be reused. **`build_history.json` that is there and is not a list of builds is DAMAGED,
  not empty**: content / full / delete / mark refuse and change nothing (read as empty, the next build was numbered 1,
  laid its symbols over the real first build's and replaced the file, the published marks with it); a check warns; the
  window says so in red over the History. **Symbols live BESIDE the output folder** (`symbols_root`:
  `<out>/../dist_symbols` -- the repository's own for `dist`): they were the repository's whatever `--out` said, and a
  probe of `--delete-build` with `--out` in a temp folder deleted the real `dist_symbols/0.1.0+1`. So: a tool that deletes
  is run on a copy, and first read for the paths it takes from somewhere else than its `--out`. Delete never takes a root
  itself (an entry whose folder IS the output folder). Probe: scratchpad-style `dist_probe.py` (the history in five
  states, a live / dead / lockless staging folder, two writers, a killed writer, the script refusing over a damaged
  history, a delete beside a scratch output with the repository's `dist_symbols` listed before and after).
  **`--override-licences`** (2026-11-16, owner: "a checkbox to override the licences when building the release config";
  the window's "Override the licences for this build" under "Every licence known", MCP `build_dist {overrideLicences:
  true}`): ONE build ships what a profile's `strictLicences` refuses -- the cook makes each such asset a warning
  ("... -- shipped anyway: the licence check is overridden for this build") and lists them in the report's
  `licencesOverridden`, build_dist.py prints them and writes them into the history entry (the window's History marks that
  build's profile "*" with the list as its tooltip); the profile keeps its rule, the window never saves the tick and a
  profile loaded clears it, ticking it drops the last check's result (Check again). release's check: 4 errors -> 0, the
  4 assets listed; on a profile without `strictLicences` it changes nothing and says so.
- **The window and MCP**: Level Editor > File > Build Distribution... (`--build-dist` opens it;
  `--build-dist=<profile>[,override-licences]` on that profile, the override ticked -- for a `--shot`), MCP `build_dist {profile,
  mode, levels, bootLevel, version, encrypt, overrideLicences, flavour}` -> `{id}`, `build_dist_status {id}`, `build_dist_cancel` -- one job
  (`dist::TheBuildJob`, a job object: Cancel and the editor closing end the whole tree). The window and the console give
  the same bytes (checked from Release_Editor and Debug). content / full write `dist/`, record rewrites `tools/dist/*.txt`:
  only when the person asked. A check message that names a level's object -- the cook says `(named by <level>, object
  '<name>')` for a path an object of a level's objects / pointLights / spotLights names -- is a button: a click selects it
  in the open level (`BuildDistributionPanel::ObjectOfMessage`, the hook `selectObject`; another level is named, not
  opened). `BuildJob::Status(maxEvents)` copies only the newest events asked for (the window 200, MCP 12). The window
  reads its profile and the history field by field through `jsonfield::Get` (below, "A Field of Another Type"), never
  `value()`: the script writes `"zip": null` for a build without an archive (the test profile), History > Delete walks
  the LATER builds for who replaced a folder or a zip, and `value("zip", std::string())` on that null threw out of the
  frame -- a release build, then a test build, then Delete of the first took the editor down
  (`BuildDistributionPanel::LaterOwnerOf`, gate `TestJsonFieldsAreReadWithoutThrowing`).
- **The check mode's gate**: `python tools/dist/check_gate.py` (its first arm brings x64/Cook up to date): the test
  profile's walk passes; three
  control arms FAIL and must say why -- a missing include (`missing file ... (named by profile include)`), a level whose
  object names a missing mesh (the error names the level and the object), a level under `logs/` (`lies in a folder no
  package carries` -- such a level shipped as nothing, silently, until 2026-11-16). Its files go to `out/check_gate/`
  (gitignored) and are removed.
- **Measuring a distribution**: parity harness `parity_shots.py` with `PARITY_EXTRA="--vfs=packages --pak-dir=<dir>"`
  (scratchpad-style), Release on packages over the record arms (0 `not cooked` is the gate). Compare Shipping with dev
  Release only at the SAME settings: dev reads the developer's own graphics section of `user_settings.json`, Shipping the
  committed defaults (vsync alone moved 20 % of the pixels through DLSS's jitter phase).
