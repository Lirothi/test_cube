# Level Editor — Executor Handoff

Read this once before your first step, then follow `docs/level_editor_implementation_plan.md`
one numbered step at a time. This file holds the cross-cutting rules and the
non-obvious gotchas the plan assumes but does not repeat. **Steps 1–13 are already
implemented; you start at Step 14.**

## 0. Repo state & commits

- Steps 1–13 are **complete but uncommitted** on `master`. **Do not commit, push,
  amend, or switch branches** — the owner does their own commits. (Worth asking them
  to commit the working v1 before you build Steps 14+ on top, so any regression is
  bisectable.)

## 1. Build & verify — every step that touches C++ or project files

- Use the Debug|x64 build command in the plan's "Build And Validation Commands", and
  the "Verify the non-editor build" recipe. Run **both**:
  1. Build with `WITH_EDITOR` on (normal Debug).
  2. Strip `WITH_EDITOR=1;` from the Debug|x64 `PreprocessorDefinitions`, **Rebuild**,
     confirm success, restore the define, **Rebuild** again.
- Debug|x64 is `/WX` (warnings = errors), `/W3`, C++20 (`stdcpp20`), RTTI on. One
  warning fails the build.
- **A clean build is necessary but NOT sufficient** for the renderer steps (20, 21,
  23). Their failure modes are silent (wrong CBV offset, mis-scaled pick, broken
  descriptor heap). Ask the owner to runtime/PIX-verify those — a build cannot.

## 2. WITH_EDITOR gating

- All editor code compiles only under `WITH_EDITOR` (defined `=1` in Debug|x64 only).
  A build without it must be byte-for-byte the original engine in behavior.
- New editor headers: `#if WITH_EDITOR` immediately after `#pragma once`, `#endif` at
  EOF. New editor `.cpp`: own `#include "self.h"` first, then `#if WITH_EDITOR …
  #endif` around the rest (empty TU when off). Files stay listed in the vcxproj
  unconditionally.
- Edits to engine files gate ONLY the editor additions (includes, members, call sites,
  params). A conditionally-present function parameter uses `#if WITH_EDITOR` inside the
  parameter list, and every call site wraps the matching argument the same way.
- Data files can't be `#if`-gated. `input/bindings.json` stays original; editor input
  is driven from gated code (`F2` toggles the editor; instancing relocates to `F12` in
  editor builds).

## 3. Project files & line endings

- No auto-globbing: every new `.h`/`.cpp` must be added to BOTH `test_cube.vcxproj`
  (ClCompile/ClInclude) and `test_cube.vcxproj.filters` (a filter group).
- Vendored/third-party `.cpp` gets per-file
  `<WarningLevel>TurnOffAllWarnings</WarningLevel><TreatWarningAsError>false</TreatWarningAsError>`
  — copy the existing meshoptimizer / ImGuizmo entries.
- **Line endings: CRLF** for C++ and `.vcxproj`/`.filters`; **LF** for markdown. Verify
  with the script in the plan's Build section (`loneLF=0 loneCR=0` for C++/project
  files).

## 4. Engine conventions

- **No C++ `dynamic_cast`.** Use internal-RTTI virtual accessors:
  `RenderableObjectBase::AsRenderableObject()/AsGBufferRenderable()/AsTransparentStaticMesh()`
  (each returns `this` in the matching subclass, `nullptr` in the base). Add a new
  `AsX()` if you need a new downcast.
- **Mutate `Scene` only inside the editor draw/tick window, after
  `Renderer::WaitForPreviousFrame()`** — never during render-pass recording. Live
  object init uses an open `UploadBatch`: `Begin(&r)` → `obj->Init(&r,
  uploads.CommandList(), uploads.KeepAlive())` → `SubmitAndWait(&r)`.
- Runtime working directory is the **repo root**; asset paths are relative (`data/…`,
  `models/…`, `input/…`).
- Route every scene edit through an `EditorCommand` (undo/redo).

## 5. The ID model (central to Steps 14–17)

- `EditorObjectId.value` **is** the `SceneObjectId` — one `uint64` space. `id 0` =
  runtime object with no editor identity.
- `Scene::objectIds_` is lockstep with `objects_`. Today only `AddObject` (pushes `0`)
  and `Clear` resize `objects_` — re-audit if you add another mutation site.
  `nextEditorId_` starts at 1.
- `AddInitializedEditorObject` inits immediately (editor spawn). Step 14's new
  `AddObjectWithEditorId` must **defer** init to `FinalizeLevelLoad`, like `AddObject`.
- `SceneRenderQueue::Bucketize` skips `!obj->IsVisible()` (the enabled toggle). RT
  reflections gather `objects_` directly, so a hidden object can still appear in RT
  reflections — known minor gap.

## 6. Per-step landmines

- **14–15 (keystone):** The editor document currently loads `demo.json` *independently*
  of the runtime — it must instead be driven from the same objects the loader produced,
  so ids match 1:1. `demo.json` objects have no `id`; generate stable ones in load
  order. The generators (`metalRoughGrid`, `instancedModels`) are ONE document entity
  each, not N.
- **16–17:** Generators expand to many runtime objects sharing one editor id →
  `FindEditorObject` returns the first; a transform gizmo on a generator is out of
  first-pass scope. `ocean` lives in the `Systems` singleton (not `Scene`) and is torn
  down on `Unload`. `DebugGrid` is runtime-only and must **never** be serialized.
  Camera/lights/skybox are parsed inline in `DemoLevel` today — move that into
  `JsonLevel`. `LevelManager` loads by registered name; add `LoadLevelFromPath`.
  `App.cpp` (~line 306) registers `DemoLevel` — switch it to `JsonLevel`.
- **18:** `LevelDocumentSerializer` already exists; `EditorSceneDocument::RootJson()`
  preserves the header sections. Keep object order and ids stable across saves.
- **19:** No built-in ImGui file dialog. Use Win32 `GetOpenFileName`/`GetSaveFileName`
  or a tiny in-engine browser — do not add a new third-party dependency.
- **20 (highest risk):** The PerObject CBV layout must match the HLSL `PerObject` struct
  (`shaders/gbuffer_common.hlsl`, ~lines 24–34) **byte-for-byte** (HLSL 16-byte
  packing). The `objectID` target must be at **render** resolution; clear to a
  sentinel. Scale the pick cursor by `renderW/displayW`; jitter is irrelevant for
  discrete ids but use the same matrices the G-buffer used. The instanced path
  (`shaders/gbuffer_inst.hlsl` + `GpuInstancedModels`) needs its own id write from the
  per-instance struct. Reuse the readback pattern in `OceanSimulation.cpp` (~line 1029),
  fenced — never stall per-frame. Keep the CPU ray-vs-bounds picker as a fallback.
  Engine is SM6.6 `ResourceDescriptorHeap`/bindless and reverse-Z — read the existing
  G-buffer pass before editing it.
- **21:** Depth is `D32_FLOAT_S8X24_UINT`; stencil is already used by point-light
  volumes (`PointLight::MakeZFail_DS`) but reset per light, so coexist carefully. Insert
  `Pass_SelectionOutline` between DebugDraw and Tonemap.
- **23:** Lights/camera are NOT `RenderableObjectBase` (they live in
  `LightManager`/`Scene`). Picking them needs editor icon billboards written into the
  Step-20 id buffer — so 23 depends on 20.
- **ImGuizmo (Step 11, already done):** `third_party/ImGuizmo/ImGuizmo.cpp` has a LOCAL
  PATCH gated by `#define TEST_CUBE_IMGUIZMO_CENTER_FLIP` (default 1). If ImGuizmo is
  ever re-downloaded, **re-apply it**. Reverse-Z previously broke ImGuizmo's
  behind-camera cull and its axis flip.

## 7. When stuck

Stop, leave the repo buildable, and report — don't push through a broken intermediate
state or invent engine APIs. The plan's "Stop Conditions" section lists the hard stops.
