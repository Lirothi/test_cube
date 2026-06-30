# AI Step Plan: Level Editor

## How To Use This Document

Give an AI executor one numbered step at a time. Example:

```text
Now implement Step 1 from docs/level_editor_implementation_plan.md.
Only do that step. Keep the project buildable and stop after the acceptance
checks for Step 1 pass.
```

Each step is intentionally separable. A later step may depend on files or APIs
created by earlier steps, but the executor should not jump ahead. If a step
cannot be completed cleanly, the executor should stop, explain the blocker, and
leave the repository buildable.

**Before your first step, read [`docs/level_editor_HANDOFF.md`](level_editor_HANDOFF.md).**
It carries the cross-cutting build/gating discipline, engine conventions (the ID
model, no `dynamic_cast`, safe scene mutation), and the per-step landmines that this
plan assumes but does not repeat. Steps 1–13 are already done; you start at Step 14.

## Global Rules For Every Step

- Preserve existing behavior when the editor is closed.
- Gate all editor code behind the `WITH_EDITOR` macro — every new editor file and
  every edit to an existing engine file — so a build without `WITH_EDITOR`
  compiles and behaves exactly as the original engine. See "Editor Compilation
  Gating (WITH_EDITOR)".
- Keep the project buildable at the end of every step.
- Do not replace `Scene`, `LevelManager`, `DemoLevel`, or the render graph.
- Do not introduce a full ECS.
- Do not generate thumbnails in the first editor pass.
- Do not load every mesh just to populate the content browser.
- Do not serialize runtime pointers, GPU handles, loaded material pointers, or
  transient render state.
- Do not mutate `Scene::objects_` while render pass recording may be reading it.
- Mutate `Scene` only inside the editor draw/Tick window, never during render
  pass recording, and gate every live scene mutation with
  `Renderer::WaitForPreviousFrame()`.
- Reload always re-parses saved JSON through the existing level-load path. The
  shared object factory is only for live spawn of new plain `staticMesh` and
  `transparentMesh` objects.
- Round-trip unmodeled data verbatim: any field or object type the editor does
  not understand must survive load to save unchanged through
  `EditorObject::properties`.
- Add any new `.h` or `.cpp` files to both `test_cube.vcxproj` and
  `test_cube.vcxproj.filters`.
- Keep C++ and Visual Studio project files CRLF. Existing Markdown docs use LF;
  preserve the touched file's existing style.

## Non-Goals (First Pass)

These bounded the **first pass (Steps 1–13)**. Several were **superseded on
2026-06-30** — see "Data-Driven, Fully Editable Editor (Scope Change 2026-06-30)"
below. The first pass deliberately excluded:

- ~~No editing of lights, camera, skybox, or ocean as selectable objects.~~ **Now in
  scope** (Steps 22–24). Until implemented they remain preserved verbatim on save.
- No copy/paste, duplicate, or multi-select. (Still out of scope.)
- ~~No GPU ID-buffer picking.~~ **Now the chosen picking path** (Step 20); CPU
  ray-vs-bounds picking (Step 11) was only the first-pass placeholder.
- No thumbnails (also a global rule). (Still out of scope.)
- No prefab or nested-object hierarchy. (Still out of scope.)

Also newly in scope: a fully data-driven loader that replaces the hardcoded
`DemoLevel` (Steps 16–17), load/save of any level file through a File menu
(Steps 18–19), and a selection outline (Step 21).

## Editor Compilation Gating (WITH_EDITOR)

All editor code is compiled only when the `WITH_EDITOR` macro is defined. A build
without it must be identical to the original engine.

- `WITH_EDITOR=1` is defined in the `Debug|x64` configuration's
  `PreprocessorDefinitions` in `test_cube.vcxproj`. It is NOT defined for
  `Release|x64`. Use `#if WITH_EDITOR` (an undefined macro evaluates to off).
- New editor source files wrap their entire body in the macro. Headers put
  `#if WITH_EDITOR` immediately after `#pragma once` (so the includes and the
  types are both gated) with `#endif` at end of file. `.cpp` files keep their own
  `#include "...self.h"` first, then `#if WITH_EDITOR` ... `#endif` around
  everything else, so they become empty translation units when the editor is off
  (they remain listed in the project unconditionally).
- Edits to existing engine files gate ONLY the editor-specific additions:
  includes, members, methods, call sites, and any new function parameters. A
  conditionally-present parameter uses `#if WITH_EDITOR` inside the parameter
  list, and every call site must wrap the matching argument the same way.
- Data files cannot be `#if`-gated. Keep shared engine data (e.g.
  `input/bindings.json`) unchanged and drive editor-only input from gated code.
  In this project `F2` toggles the whole editor interface (all windows) via
`ImGui::IsKeyPressed` (gated), and
  the GPU-instancing hotkey is relocated to `F12` only in editor builds; a
  non-editor build keeps the original `F2 = instancing` binding.
- Both configurations must build. Verify `WITH_EDITOR` on (the normal Debug
  build) and off (see "Verify the non-editor build").

## Editor Windowing

Editor panels are independent ImGui windows, not sections of one window:

- A small **Level Editor** window holds the status line, Undo/Redo, and
  checkboxes that show/hide the other windows. Closing it (its `X`) closes the
  whole editor.
- **Content Browser**, **Scene Outliner**, and **Inspector** are each their own
  `ImGui::Begin`/`End` window backed by a visibility bool owned by
  `EditorController`.
- `EditorController` owns the overall open state and the per-window visibility
  bools, builds the `EditorContext` once per frame, and draws each visible
  window. A panel draws its own window and returns an action; the controller
  turns actions into commands.
- The whole interface is shown or hidden together by the editor open state,
  toggled by `F2` (and the developer-window checkbox). When the editor is closed,
  no editor window draws and engine input is unaffected.
- Adding a future panel is additive: a new window, a visibility bool, and a
  toggle checkbox — existing panels do not change.

## Required Local Context

Before Step 1, the executor should read:

- `sources/app/App.cpp`
- `sources/app/AppController.h`
- `sources/app/AppController.cpp`
- `sources/app/DeveloperWindow.h`
- `sources/app/DeveloperWindow.cpp`
- `sources/app/scene/Scene.h`
- `sources/app/scene/Scene.cpp`
- `sources/app/scene/SceneResourceBootstrapper.h`
- `sources/app/scene/SceneResourceBootstrapper.cpp`
- `sources/app/levels/DemoLevel.cpp`
- `sources/rendering/meshes/StaticMesh.h`
- `sources/rendering/meshes/StaticMesh.cpp`
- `sources/rendering/renderables/RenderableObject.h`
- `sources/rendering/renderables/GBufferRenderable.h`
- `sources/rendering/renderables/TransparentStaticMesh.h`
- `sources/materials/MaterialDataManager.h`
- `data/levels/demo.json`
- `data/materials.json`

Background references already used for this plan:

- Unreal content browser:
  `https://dev.epicgames.com/documentation/en-us/unreal-engine/content-browser-in-unreal-engine`
- Unreal asset management:
  `https://dev.epicgames.com/documentation/en-us/unreal-engine/asset-management-in-unreal-engine`
- Unity AssetDatabase:
  `https://docs.unity3d.com/6000.0/Documentation/ScriptReference/AssetDatabase.html`
- Unity Undo:
  `https://docs.unity3d.com/6000.0/Documentation/ScriptReference/Undo.html`
- Godot editor plugins:
  `https://docs.godotengine.org/en/stable/tutorials/plugins/editor/making_plugins.html`

Practical takeaways:

- browse assets through cheap metadata first
- load or upload assets only when spawned, previewed, or otherwise needed
- route scene edits through commands
- use stable IDs for editor objects
- keep editor surfaces registry-friendly so later object types and panels do not
  require rewriting the first pass
- objects can be initialized at any time through an open `UploadBatch`
  (`Renderer::WaitForPreviousFrame` then `UploadBatch::Begin` then `obj->Init`
  then `UploadBatch::SubmitAndWait`); `Init` is not restricted to
  `FinalizeLevelLoad`
- the editor object ID and the runtime `SceneObjectId` share one value space (see
  Step 5), so selection, inspector, delete, and reload all key off a single
  `uint64`

## Build And Validation Commands

Use this Debug build command after each step that changes C++ or project files:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe' test_cube.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
```

Verify this MSBuild path exists before relying on it. If Visual Studio is
installed at a different edition or version, adjust the path; a wrong path makes
every step's build check fail in a way unrelated to the editor work.

Line-ending check for touched C++ and project files:

```powershell
$files = @(
  'test_cube.vcxproj',
  'test_cube.vcxproj.filters'
)
foreach ($f in $files) {
  [byte[]]$b = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $f).Path)
  $loneLf = 0
  $loneCr = 0
  for ($i = 0; $i -lt $b.Length; $i++) {
    if ($b[$i] -eq 13) {
      if (($i + 1) -lt $b.Length -and $b[$i + 1] -eq 10) { $i++ } else { $loneCr++ }
    } elseif ($b[$i] -eq 10) {
      $loneLf++
    }
  }
  Write-Output "$f loneLF=$loneLf loneCR=$loneCr"
}
```

Add touched `.h` and `.cpp` files to `$files`. Expected for C++ and project
files is `loneLF=0 loneCR=0`.

### Verify the non-editor build

Editor changes must not break a build without `WITH_EDITOR`. After steps that add
or gate editor code, temporarily remove `WITH_EDITOR=1;` from the `Debug|x64`
`PreprocessorDefinitions`, run a Rebuild, confirm success, then restore the define
and Rebuild again:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe' test_cube.vcxproj /t:Rebuild /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
```

## Step 0: Orientation Only

Use this when starting a fresh AI executor.

### Prompt

```text
Do Step 0 from docs/level_editor_implementation_plan.md. Read the required
context, summarize the current architecture, and do not edit files.
```

### Scope

No file edits. The executor should understand where editor code will integrate.

### Tasks

- Read the required local context files listed above.
- Confirm how ImGui is currently drawn and how input capture works.
- Confirm how `DemoLevel` parses JSON into runtime objects.
- Confirm that `Scene::AddObject` does not initialize objects.
- Confirm where C++ files are included in `test_cube.vcxproj` and filters.

### Acceptance

- Executor reports the current integration points.
- No files changed.

## Step 1: Editor Shell

### Prompt

```text
Now implement Step 1 from docs/level_editor_implementation_plan.md.
Only create the editor shell and wire it into the app. Do not implement asset
scanning yet.
```

### Goal

Create a minimal `Level Editor` ImGui window controlled by `AppController`.

### Add Files

- `sources/editor/EditorController.h`
- `sources/editor/EditorController.cpp`
- `sources/editor/EditorContext.h`

### Modify Files

- `sources/app/AppController.h`
- `sources/app/AppController.cpp`
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

### Required Shape

`EditorController` should own open/closed state and draw a minimal window:

```cpp
class EditorController
{
public:
    bool IsOpen() const;
    void SetOpen(bool open);
    void ToggleOpen();
    void Draw(EditorContext& ctx);

private:
    bool open_ = false;
};
```

`EditorContext` should initially hold references needed by editor UI:

```cpp
struct EditorContext
{
    Renderer& renderer;
    Scene& scene;
    LevelManager& levelManager;
};
```

If include dependencies make this exact shape awkward, forward declare and keep
the context simple.

### Tasks

- Add `EditorController` as a member of `AppController`.
- Call `editorController_.Draw(...)` from `AppController::Tick`.
- Add a visible toggle in `DeveloperWindow` or use an existing/debug key path.
  Prefer a simple checkbox/button labeled `Level Editor` in the developer
  window.
- The new editor window should show placeholder text and no asset list yet.

### Acceptance

- Debug build succeeds.
- Existing F1 developer window still opens.
- The `Level Editor` window can be opened and closed.
- Existing camera input capture behavior is unchanged when the editor is closed.
- No asset scanning code exists yet.

## Step 2: Asset Registry

### Prompt

```text
Now implement Step 2 from docs/level_editor_implementation_plan.md.
Only add the metadata-only AssetRegistry and show counts in the Level Editor.
Do not add spawn actions yet.
```

### Goal

Add metadata-only asset discovery. The content browser UI can wait until Step 3.

### Add Files

- `sources/editor/assets/AssetRegistry.h`
- `sources/editor/assets/AssetRegistry.cpp`

### Modify Files

- `sources/editor/EditorController.h`
- `sources/editor/EditorController.cpp`
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

### Required Types

```cpp
enum class EditorAssetType
{
    Mesh,
    MaterialPreset,
    Texture,
    Level,
    Shader,
    Unknown
};

struct EditorAssetId
{
    EditorAssetType type = EditorAssetType::Unknown;
    std::string key;
};

struct EditorAssetRecord
{
    EditorAssetId id;
    std::string path;
    std::string displayName;
    std::string extension;
    uint64_t fileWriteTime = 0;
};
```

`AssetRegistry` must expose at least:

```cpp
void Refresh();
const std::vector<EditorAssetRecord>& Assets() const;
std::vector<const EditorAssetRecord*> Search(std::string_view text,
    EditorAssetType typeFilter) const;
const EditorAssetRecord* FindByPath(std::string_view path) const;
```

### Scan Roots

- `models/`: `.obj`, `.txt`, `.mesh.txt`
- `textures/`: `.dds`, `.png`
- `data/levels/`: `.json`
- `data/materials.json`: preset names under `presets`
- `shaders/`: `.hlsl`

### Tasks

- Add `AssetRegistry` as a member of `EditorController`.
- Call `Refresh()` once lazily when the editor first opens, plus from a temporary
  refresh button.
- Display asset counts by type in the editor window.
- Do not call `MeshManager::Load`, texture loading, or GPU upload code from the
  registry.

### Acceptance

- Debug build succeeds.
- Opening the editor shows counts for meshes, materials, textures, levels, and
  shaders.
- `models/box.obj`, `models/sphere.obj`, and material presets from
  `data/materials.json` are included in the registry.
- Refresh does not visibly stall on this repository's assets.

## Step 3: Content Browser Panel

### Prompt

```text
Now implement Step 3 from docs/level_editor_implementation_plan.md.
Only add the content browser UI over AssetRegistry. Do not add scene spawning.
```

### Goal

Display searchable/filterable asset records in a real panel.

### Add Files

- `sources/editor/ui/ContentBrowserPanel.h`
- `sources/editor/ui/ContentBrowserPanel.cpp`

### Modify Files

- `sources/editor/EditorController.h`
- `sources/editor/EditorController.cpp`
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

### UI Requirements

Draw the content browser as its own ImGui window (`Content Browser`, see
"Editor Windowing") containing:

- search text input
- asset type filter combo
- refresh button
- table with type, display name, and path
- selected asset details

Use a plain table/list. Do not implement thumbnails.

### Tasks

- Move asset-list UI out of `EditorController` into `ContentBrowserPanel`.
- Store selected asset ID/path in editor state.
- Add disabled or placeholder context menu entries for:
  - `Spawn Static Mesh`
  - `Spawn Transparent Mesh`
  - `Assign Material to Selected`
- Keep these actions non-functional until later steps.

### Acceptance

- Debug build succeeds.
- Search narrows visible assets.
- Type filter works.
- Refresh updates the list.
- Selecting an asset shows its metadata.
- No scene objects are created in this step.

## Step 4: Editor Scene Document

### Prompt

```text
Now implement Step 4 from docs/level_editor_implementation_plan.md.
Only add EditorSceneDocument and load object metadata from data/levels/demo.json.
Do not mutate the runtime scene yet.
```

### Goal

Add editor-side serializable scene state with stable IDs.

### Add Files

- `sources/editor/scene/EditorSceneDocument.h`
- `sources/editor/scene/EditorSceneDocument.cpp`

### Modify Files

- `sources/editor/EditorController.h`
- `sources/editor/EditorController.cpp`
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

### Required Types

```cpp
struct EditorObjectId
{
    uint64_t value = 0;
};

struct EditorTransform
{
    Math::float3 position{ 0.0f, 0.0f, 0.0f };
    Math::float3 rotationDeg{ 0.0f, 0.0f, 0.0f };
    Math::float3 scale{ 1.0f, 1.0f, 1.0f };
};

struct EditorObject
{
    EditorObjectId id;
    std::string name;
    std::string type;
    bool enabled = true;
    EditorTransform transform;
    nlohmann::json properties;
};
```

`EditorSceneDocument` must support:

```cpp
EditorObjectId AllocateId();
EditorObject* Find(EditorObjectId id);
const EditorObject* Find(EditorObjectId id) const;
void Add(EditorObject object);
bool Remove(EditorObjectId id);
std::vector<EditorObject>& Objects();
const std::vector<EditorObject>& Objects() const;
bool IsDirty() const;
void SetDirty(bool dirty);
```

### JSON Loading Rules

- Load objects from `data/levels/demo.json`.
- Existing objects without `id` must load successfully. `demo.json` objects have
  no `id` today, so every object gets a generated ID on first open.
- Assign generated IDs for missing IDs.
- Do not save or rewrite the level file in this step.
- Common fields go into `EditorObject`; object-specific fields stay in
  `properties`.
- `properties` must preserve every field the editor does not model verbatim (for
  example `rotateSpeedDeg`, which the runtime loader turns into a
  `RotatingObject`). These survive load to save unchanged.
- Load all object entries, including types the editor cannot edit yet (for
  example `metalRoughGrid`, `instancedModels`). Mark non-editable types read-only
  rather than dropping them.

### Tasks

- Add `EditorSceneDocument` to `EditorController`.
- Load `data/levels/demo.json` into the document when the editor opens.
- Display object count and selected active level path in the editor window.
- Do not touch the runtime scene.

### Acceptance

- Debug build succeeds.
- Editor reports the same number of document objects as `objects` entries in
  `data/levels/demo.json`.
- Existing demo level rendering is unchanged.

## Step 5: Scene Object IDs And Safe Mutation

### Prompt

```text
Now implement Step 5 from docs/level_editor_implementation_plan.md.
Only add editor object IDs and safe initialized object add/remove APIs to Scene.
Do not add UI spawn actions yet.
```

### Goal

Prepare `Scene` for live editor-spawned objects.

### Modify Files

- `sources/app/scene/Scene.h`
- `sources/app/scene/Scene.cpp`

### Required API

```cpp
using SceneObjectId = uint64_t;

SceneObjectId AddEditorObject(std::unique_ptr<RenderableObjectBase> obj);
bool AddInitializedEditorObject(Renderer& renderer,
    UploadBatch& uploads,
    SceneObjectId id,
    std::unique_ptr<RenderableObjectBase> obj);
bool RemoveEditorObject(SceneObjectId id);
RenderableObjectBase* FindEditorObject(SceneObjectId id);
const RenderableObjectBase* FindEditorObject(SceneObjectId id) const;
```

If exact names conflict with existing style, preserve the intent and document
the chosen names in the final response.

### Implementation Guidance

- ID model: the value in `EditorObject::id` (Step 4) and the `SceneObjectId` for
  that object's live runtime presence are the same `uint64`. Allocate one ID per
  object and use it for both the document entry and the scene entry. This is what
  lets `SpawnMeshCommand`, `DeleteObjectCommand`, the outliner, the inspector, and
  the gizmo all key off a single ID.
- Add `std::vector<SceneObjectId> objectIds_` kept in lockstep with `objects_`.
- ID `0` means runtime object with no editor identity.
- Existing `AddObject` should append ID `0`.
- `Clear` must clear both vectors.
- `RemoveEditorObject` should remove both object and ID at the same index.
- `AddInitializedEditorObject` must call `obj->Init(&renderer,
  uploads.CommandList(), uploads.KeepAlive())` before appending.
- Return false if the upload batch is not open or object is null.
- Audit every site that mutates `objects_` before relying on lockstep. Today only
  `AddObject` and `Clear` touch it; any other erase or reorder must update
  `objectIds_` identically or the two vectors desync.
- `FindEditorObject` and `RemoveEditorObject` may scan linearly (O(n)); that is
  fine at editor object counts. Do not add a map unless profiling demands it.

### Acceptance

- Debug build succeeds.
- Existing level load still works.
- Existing `Scene::AddObject` callers do not need to pass IDs.
- No editor UI uses the new APIs yet.

## Step 6: Shared Mesh Object Factory

### Prompt

```text
Now implement Step 6 from docs/level_editor_implementation_plan.md.
Move reusable staticMesh and transparentMesh creation out of DemoLevel.cpp into
a shared factory. Keep demo level behavior unchanged.
```

### Goal

Avoid duplicate mesh creation logic between level loading and editor spawning.

### Add Files

Choose one location:

- `sources/app/scene/SceneObjectFactory.h`
- `sources/app/scene/SceneObjectFactory.cpp`

or:

- `sources/app/levels/LevelObjectFactory.h`
- `sources/app/levels/LevelObjectFactory.cpp`

Prefer `sources/app/scene/SceneObjectFactory.*` if include dependencies are
clean.

### Modify Files

- `sources/app/levels/DemoLevel.cpp`
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

### Move Into Shared Code

From `DemoLevel.cpp`, move or reimplement:

- `ToFloat3`
- `ToFloat4`
- `RenderLayerFromString`
- common static mesh material parameter application
- static mesh creation
- transparent mesh creation

Keep these demo-specific pieces in `DemoLevel.cpp`:

- `RotatingObject`
- `metalRoughGrid`
- `instancedModels`
- ocean loading
- debug grid creation

### Required Factory Functions

JSON-based functions are preferred to avoid app code depending on editor types:

```cpp
std::unique_ptr<RenderableObjectBase> CreateStaticMeshFromJson(
    const nlohmann::json& objectJson);

std::unique_ptr<RenderableObjectBase> CreateTransparentMeshFromJson(
    Scene& scene,
    const nlohmann::json& objectJson);
```

Add helpers for building default JSON later if useful, but do not overbuild this
step.

### Acceptance

- Debug build succeeds.
- Demo level looks and behaves the same.
- `DemoLevel.cpp` uses the shared factory for `staticMesh` and
  `transparentMesh`.
- No editor spawning is added yet.

## Step 7: Command Stack

### Prompt

```text
Now implement Step 7 from docs/level_editor_implementation_plan.md.
Only add the editor command stack and wire Undo/Redo buttons to empty state.
Do not implement spawn commands yet.
```

### Goal

Add command infrastructure before scene editing.

### Add Files

- `sources/editor/commands/EditorCommand.h`
- `sources/editor/commands/EditorCommandStack.h`
- `sources/editor/commands/EditorCommandStack.cpp`

### Modify Files

- `sources/editor/EditorController.h`
- `sources/editor/EditorController.cpp`
- `sources/editor/EditorContext.h`
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

### Required Interfaces

```cpp
class EditorCommand
{
public:
    virtual ~EditorCommand() = default;
    virtual bool Execute(EditorContext& ctx) = 0;
    virtual void Undo(EditorContext& ctx) = 0;
};

class EditorCommandStack
{
public:
    bool Execute(EditorContext& ctx, std::unique_ptr<EditorCommand> command);
    void Undo(EditorContext& ctx);
    void Redo(EditorContext& ctx);
    bool CanUndo() const;
    bool CanRedo() const;
};
```

`EditorContext` should expose at least:

- `Renderer& renderer`
- `Scene& scene`
- `LevelManager& levelManager`
- `EditorSceneDocument& document`
- selected editor object ID

### UI Requirements

- Add disabled/enabled Undo and Redo buttons to the editor window.
- Buttons may do nothing when stacks are empty.

### Acceptance

- Debug build succeeds.
- Empty Undo and Redo buttons are visible and disabled when no command exists.
- No scene mutation commands exist yet.

## Step 8: Spawn Mesh Command

### Prompt

```text
Now implement Step 8 from docs/level_editor_implementation_plan.md.
Add SpawnMeshCommand and connect content browser mesh actions to live scene
spawning. Keep property editing out of scope.
```

### Goal

Spawn selected mesh assets into the live scene as initialized runtime objects.

### Add Files

- `sources/editor/commands/SpawnMeshCommand.h`
- `sources/editor/commands/SpawnMeshCommand.cpp`

### Modify Files

- `sources/editor/ui/ContentBrowserPanel.cpp`
- `sources/editor/EditorController.h`
- `sources/editor/EditorController.cpp`
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

### Spawn Behavior

`SpawnMeshCommand::Execute` must:

1. Create or reuse an `EditorObject` with stable ID.
2. Insert it into `EditorSceneDocument`.
3. Convert it to object JSON.
4. Create the runtime renderable through the shared object factory.
5. Call `renderer.WaitForPreviousFrame()` before mutating `Scene`.
6. Open an `UploadBatch` (`UploadBatch::Begin`).
7. Call `Scene::AddInitializedEditorObject`, passing the `EditorObject`'s ID as
   the `SceneObjectId` (Step 5 ID model).
8. Close the batch with `UploadBatch::SubmitAndWait`.
9. Select the new object.
10. Mark the document dirty.

Notes:

- The command runs inside the editor draw/Tick window. ImGui draw data is
  CPU-side and scene render recording happens later in the frame, so mutating
  `Scene` here is safe once `WaitForPreviousFrame` has idled the prior frame.
- This performs two full GPU stalls per spawn (`WaitForPreviousFrame` plus
  `SubmitAndWait`, which idles again). That is fine for an infrequent editor
  action; do not copy this pattern into any per-frame path.

`Undo` must:

1. Call `renderer.WaitForPreviousFrame()`.
2. Remove runtime object by ID.
3. Remove document object by ID.
4. Restore previous selection if possible.
5. Mark the document dirty.

### Defaults

- spawn position: `camera position + camera forward * 5.0f`
- rotation: `[0, 0, 0]`
- scale: `[1, 1, 1]`
- static mesh material: `damaged_plaster` if present, otherwise first material
  preset, otherwise empty string
- static mesh shader: `shaders/gbuffer.hlsl`
- input layout: `PosNormTanUV`
- transparent mesh defaults copied from an existing transparent object in
  `data/levels/demo.json`

### UI Requirements

Enable content browser context menu actions:

- `Spawn Static Mesh`
- `Spawn Transparent Mesh`

Only show/enable those actions for `EditorAssetType::Mesh`.

### Acceptance

- Debug build succeeds.
- Static mesh spawn from `models/box.obj` renders.
- Transparent mesh spawn from `models/sphere.obj` renders.
- Undo removes spawned object from document and scene.
- Redo restores spawned object with the same ID.
- Existing demo objects still render.

## Step 9: Delete Command And Outliner

### Prompt

```text
Now implement Step 9 from docs/level_editor_implementation_plan.md.
Add an outliner and undoable delete for editor objects. Do not add inspector
property editing yet.
```

### Goal

Select and delete editor-owned objects by ID.

### Add Files

- `sources/editor/commands/DeleteObjectCommand.h`
- `sources/editor/commands/DeleteObjectCommand.cpp`
- `sources/editor/ui/SceneOutlinerPanel.h`
- `sources/editor/ui/SceneOutlinerPanel.cpp`

### Modify Files

- `sources/editor/EditorController.h`
- `sources/editor/EditorController.cpp`
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

### Outliner Requirements

Draw the outliner as its own ImGui window (`Scene Outliner`, see "Editor
Windowing"), containing a table from `EditorSceneDocument::Objects()`:

- selected state
- enabled checkbox
- name
- type
- ID

Actions:

- select object
- delete an object via a right-click context menu entry, through
  `DeleteObjectCommand`

### Delete Command

Delete command must store enough serialized object data to restore on undo.

Execute:

- wait for previous frame
- remove runtime object by ID when present
- remove document object by ID
- clear or restore selection
- mark dirty

Undo:

- reinsert document object with same ID
- recreate runtime object through shared factory
- initialize through upload batch
- restore selection
- mark dirty

### Acceptance

- Debug build succeeds.
- Spawned objects appear in the outliner.
- Selecting an object in the outliner updates editor selection.
- Delete removes selected spawned object.
- Undo restores deleted object.

## Step 10: Inspector Transform Editing

### Prompt

```text
Now implement Step 10 from docs/level_editor_implementation_plan.md.
Add inspector UI for selected objects and implement live transform editing only.
Do not add material editing yet.
```

### Goal

Edit selected object's name, enabled state, and transform.

### Add Files

- `sources/editor/ui/InspectorPanel.h`
- `sources/editor/ui/InspectorPanel.cpp`

### Modify Files

- `sources/editor/EditorController.h`
- `sources/editor/EditorController.cpp`
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

### UI Fields

Draw the inspector as its own ImGui window (`Inspector`, see "Editor Windowing")
with:

- ID read-only
- name
- type read-only
- enabled
- position
- rotation degrees
- scale
- model path read-only

### Runtime Patch Rules

- For runtime objects derived from `RenderableObject`, call:
  - `SetPosition`
  - `SetRotationEulerDeg`
  - `SetScale`
- Update the corresponding `EditorObject` fields.
- Mark document dirty when values change.
- Transform edits must not corrupt the undo stack. Pick one and state which in the
  final response:
  - (a) Wrap each edit gesture as a `TransformObjectCommand` (one committed
    numeric edit = one undo action). Preferred, and required before Step 11 so
    gizmo drags reuse the same command.
  - (b) If left non-undoable for now, every direct edit must clear the redo stack
    so a later Undo cannot silently discard the edit by undoing an earlier spawn.
    Do not leave a half-wired stack where Undo after an edit pops an unrelated
    command.

### Acceptance

- Debug build succeeds.
- Selecting a spawned object shows inspector fields.
- Editing transform moves the rendered object.
- Name edits update the outliner.
- Existing camera/input behavior remains usable.

## Step 11: Viewport Selection And Transform Gizmo

### Prompt

```text
Now implement Step 11 from docs/level_editor_implementation_plan.md.
Add viewport click-selection and a transform gizmo for the selected editor
object. Reuse the command stack so one gizmo drag is one undo action.
```

### Goal

Let the user click objects in the 3D viewport to select them and manipulate the
selected object's transform with an on-screen translate/rotate/scale gizmo.

### Add Files

- `sources/editor/ui/ViewportGizmo.h`
- `sources/editor/ui/ViewportGizmo.cpp`
- `sources/editor/commands/TransformObjectCommand.h`
- `sources/editor/commands/TransformObjectCommand.cpp`

If `TransformObjectCommand` already exists from Step 10 option (a), extend it here
instead of recreating it.

### Modify Files

- `sources/editor/EditorController.h`
- `sources/editor/EditorController.cpp`
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`
- third-party integration files if ImGuizmo is vendored (see below)

### Gizmo Library

Prefer vendoring **ImGuizmo** (single `.h`/`.cpp`) under `third_party/ImGuizmo/`,
added to both project files. It integrates directly with the existing ImGui
context and needs the camera view matrix, projection matrix, and the object's
world matrix. If adding a third-party dependency is undesirable, a hand-rolled
translate-only gizmo is an acceptable first-pass fallback; state which path was
taken in the final response.

### Selection Picking

- On a viewport click that is not over an ImGui window and not capturing camera
  input, build a world-space ray from the camera through the cursor.
- Intersect the ray against editor-owned objects only (those with a non-zero
  `SceneObjectId`), using each object's world-space bounds. Pick the nearest hit.
- Set the editor selection to the picked object's shared ID; the outliner and
  inspector already key off this ID (Step 5 ID model).
- CPU ray-vs-bounds picking is sufficient for the first pass; GPU ID-buffer
  picking is a later upgrade (see Non-Goals).

### Gizmo Behavior

- Draw the gizmo only when an editor object is selected and the editor is open.
- Provide translate / rotate / scale modes (e.g. W/E/R or toolbar buttons) and a
  world/local space toggle.
- While dragging, patch the live runtime object every frame via `SetPosition`,
  `SetRotationEulerDeg`, `SetScale` so motion is visible immediately. If using
  ImGuizmo, decompose its returned world matrix into position/euler/scale before
  calling the setters.
- On drag begin, capture the object's transform. On drag end, push a single
  `TransformObjectCommand` carrying before/after transforms so the whole drag is
  one undo action. Do not push per-frame commands.
- Keep the inspector transform fields (Step 10) and the gizmo in sync: editing one
  updates the other.

### Input Rules

- The gizmo must not fight camera input. When the gizmo is hovered or active,
  suppress camera look/move, consistent with how `uiCapturingInput` already gates
  camera input in `AppController`.
- When the editor is closed, no picking, gizmo, or selection input runs.

### Acceptance

- Debug build succeeds.
- Clicking a spawned object in the viewport selects it (outliner highlight
  updates).
- The gizmo moves, rotates, and scales the selected object live.
- One gizmo drag produces exactly one undo entry; Undo restores the pre-drag
  transform.
- Inspector transform fields and the gizmo stay in sync.
- Camera input still works when nothing is selected and when the editor is closed.

## Step 12: Inspector Mesh Setup Editing

### Prompt

```text
Now implement Step 12 from docs/level_editor_implementation_plan.md.
Extend the inspector to edit mesh setup fields: material preset, render layer,
material params, and transparent mesh params.
```

### Goal

Edit setup fields for `staticMesh` and `transparentMesh`.

### Modify Files

- `sources/editor/ui/InspectorPanel.h`
- `sources/editor/ui/InspectorPanel.cpp`
- optional command files if undoable property edits are implemented

### Static Mesh Fields

- material preset combo
- render layer combo
- `texOffsScale`
- `normalStrength`
- `useMR`
- `metalRough`

### Transparent Mesh Fields

- `tint`
- `absorption`
- `thickness`
- `reflectionStrength`
- `refractionDistortion`
- `roughness`
- `ior`
- `normalMap`

### Runtime Patch Rules

- `GBufferRenderable::MaterialParamsRef()` can be patched for material
  parameter fields.
- `MaterialParams` fields are indirect: `normalStrength` is `texFlags.w` (use
  `SetNormalStrength`), `useMR` is `texFlags.y` (use `SetUseMR`), `metalRough` is
  a `float2` (x = metallic, y = roughness), and `texOffsScale` is a `float4`.
  Patch through the setters; do not invent field names.
- Only expose and write JSON keys the runtime loader actually consumes (the keys
  `ApplyCommonMeshProperties` reads in the demo loader, e.g. `material`,
  `normalStrength`). Writing keys the loader ignores means edits silently vanish
  on reload.
- Material preset changes may remove and respawn the object if no safe setter
  exists.
- Transparent-specific setters should use `TransparentStaticMesh` public API
  where available.
- Update `EditorObject::properties` for every edit.
- Mark the document dirty.

### Acceptance

- Debug build succeeds.
- Static mesh material parameter edits visibly affect supported objects.
- Material preset assignment works or respawns safely.
- Transparent mesh fields update the runtime object where supported.
- Edits survive within the current editor document.

## Step 13: Save And Reload

### Prompt

```text
Now implement Step 13 from docs/level_editor_implementation_plan.md.
Add level document save/reload for editor objects while preserving demo level
compatibility.
```

### Goal

Persist editor-created and edited objects to JSON.

### Prerequisite

Verify the runtime loader can open an arbitrary JSON path. `LevelManager` today
loads levels by registered name (`LoadLevel(name, ...)`,
`RequestLevelChange(levelName, ...)`) and `DemoLevel` is bound to
`data/levels/demo.json`. If there is no path-parameterized load, add one (a
generic `JsonFileLevel` or `LevelManager::LoadLevelFromPath`) before relying on
save-as-copy plus reload. If that turns out to be a large change, stop and report
per Stop Conditions.

### Add Files

- `sources/editor/serialization/LevelDocumentSerializer.h`
- `sources/editor/serialization/LevelDocumentSerializer.cpp`

### Modify Files

- `sources/editor/EditorController.h`
- `sources/editor/EditorController.cpp`
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

### JSON Shape

Preserve top-level sections:

- `camera`
- `skybox`
- `ocean`
- `directionalLight`
- `spotLights`
- `pointLights`
- `objects`

Object additions:

- `id`
- `name`
- optional `editor`

Do not save generated runtime-only objects such as `DebugGrid`.

### UI Actions

- Save current level.
- Save as copy, even if first version uses a simple path text box.
- Reload active level or saved copy.

### Save Rules

- Stable object order.
- Stable IDs.
- Consistent indentation.
- No random ID churn across saves.
- Unmodeled object fields and object types are written back from
  `EditorObject::properties` verbatim.
- Existing `data/levels/demo.json` should remain readable whether or not it has
  editor IDs.

### Reload Rules

1. Wait for previous frame.
2. Rebuild the runtime scene through the existing level-load path by re-parsing
   the saved JSON. Do not rebuild through the editor factory: only the loader
   recreates special types correctly (`RotatingObject` from `rotateSpeedDeg`,
   `metalRoughGrid`, `instancedModels`, ocean).
3. Rebuild editor document.
4. Preserve selection only if selected ID still exists.

### Acceptance

- Debug build succeeds.
- Spawn a mesh, edit transform, save to a copy, reload, and see the same object.
- The saved copy can be loaded by the existing runtime loader.
- Load `demo.json`, save an unedited copy, reload it, and confirm rotating boxes
  still rotate and the metal-rough grid and instanced models remain.
- Original `data/levels/demo.json` still loads.

## Data-Driven, Fully Editable Editor (Scope Change 2026-06-30)

Steps 1–13 produced a working editor **for objects spawned in-editor**. Three of the
first-pass Non-Goals are exactly what make it feel unfinished: loaded objects can't
be edited, there's no load/save of arbitrary levels (the hardcoded `DemoLevel` is the
only loader), and selection is CPU bounding-box picking with no outline. Steps 14–24
remove those limits, one separable step at a time; Steps 25–26 (Hotkeys, Extensibility
Cleanup) follow last.

### Locked Decisions

1. **Loader rewrite — full data-driven.** All object creation moves into a type
   registry; a generic `JsonLevel` loads/saves any file; `metalRoughGrid` and
   `instancedModels` become registered generator types; `DemoLevel` is **deleted**.
2. **Picking — GPU object-ID buffer.** Pixel-accurate selection via an `R32_UINT`
   G-buffer target plus a one-pixel readback on click.
3. **Environment editable.** Lights, camera, skybox, and ocean become
   selectable/editable entities (Steps 22–24).

### Root-Cause Diagnosis (carry into every step below)

The editor today has **two parallel worlds that only connect for spawned objects**:

- The runtime scene is built by `DemoLevel::Load()`, which calls `Scene::AddObject`.
  That path hard-codes the object's editor id to **0** (`Scene.cpp:262`,
  `objectIds_.push_back(0)`). Id `0` means "no editor identity"; picking, find,
  inspector, and delete all skip id `0` (`Scene.cpp:358`).
- The `EditorSceneDocument` is a *separate* mirror parsed from the same JSON with its
  own ids, never linked back to the id-0 runtime objects.
- Only `SpawnMeshCommand` allocates one id and gives the **same** id to both the
  document and `Scene::AddInitializedEditorObject` — which is why only spawned objects
  are editable. Reload routes back through `DemoLevel`, so even a saved object returns
  with id `0` and goes un-editable.

Most of `demo.json` is **already** data-driven (camera, lights, skybox, ocean,
static/transparent meshes). The genuinely hardcoded pieces are the `type` dispatch
in `DemoLevel`, `RotatingObject`, the `metalRoughGrid`/`instancedModels` generators,
and the always-on `DebugGrid`.

The fix is one object lifecycle: **every object in a loaded level carries a live
editor id, and the document and runtime stay 1:1 by that id.**

### Additional Global Rules

- Every object in a loaded level gets a live editor id; the document mirrors the
  runtime 1:1; ids are stable across save/load.
- Everything else from "Global Rules For Every Step" still holds — `WITH_EDITOR`
  gating, build on/off, CRLF for C++/project files, command-pattern + undo, mutate
  `Scene` only in the editor window after `WaitForPreviousFrame()`, no
  `dynamic_cast` (internal-RTTI `AsX()` accessors), round-trip unmodeled data.

## Step 14: Scene Add Object With Editor Id

### Prompt

```text
Now implement Step 14 from docs/level_editor_implementation_plan.md.
Only add Scene::AddObjectWithEditorId (explicit id, deferred init). Do not change
level loading yet.
```

### Goal

Let level loading give a runtime object a real editor id without initializing it
immediately, so loaded objects can become editor-linked in Step 15. No renderer
changes; this is the keystone — it is what finally makes loaded objects editable.

### Modify Files

- `sources/app/scene/Scene.h`
- `sources/app/scene/Scene.cpp`

### Required API

```cpp
void AddObjectWithEditorId(std::unique_ptr<RenderableObjectBase> obj, SceneObjectId id);
```

### Tasks

- Append to `objects_`, push `id` (not `0`) to `objectIds_`, and bump `nextEditorId_`
  past `id`. Defer `Init` to `FinalizeLevelLoad`, exactly like `AddObject`.
- Keep `AddObject` (id `0`) for truly anonymous runtime-only objects (`DebugGrid`), or
  give them an id flagged non-serialized.
- Re-audit every `objects_` mutation site for `objectIds_` lockstep.

### Acceptance

- Debug build succeeds (editor on and off).
- Demo level loads and renders unchanged.
- No editor UI uses the new API yet.

## Step 15: Load-Time ID Assignment And Document Mirror

### Prompt

```text
Now implement Step 15 from docs/level_editor_implementation_plan.md.
Make every loaded level object editor-linked and mirrored into the document, so
loaded objects are editable like spawned ones and survive reload.
```

### Goal

Every object in a loaded level gets a live editor id and a matching `EditorObject`.

### Modify Files

- `sources/app/levels/DemoLevel.cpp` (still the loader at this step)
- `sources/editor/EditorController.*` / `sources/editor/scene/EditorSceneDocument.*`

### Tasks

- During load, allocate one id per object in stable order and add via
  `AddObjectWithEditorId`.
- Under `WITH_EDITOR`, add a matching `EditorObject` to the document (lift common
  fields; rest into `properties`) so the document is a true mirror of the runtime,
  loaded objects included.
- Drive the editor document from the same JSON the runtime used; reload reuses this
  path so editability survives a reload.

### Acceptance

- Debug build succeeds (editor on and off).
- Every demo object appears in the outliner with a non-zero id.
- Clicking and editing a **loaded** box works live (transform / material).
- Reload keeps loaded objects editable. *(Resolves the primary complaint.)*

## Step 16: Scene Object Type Registry

### Prompt

```text
Now implement Step 16 from docs/level_editor_implementation_plan.md.
Replace DemoLevel's hardcoded type dispatch with a registry of type -> creator. Keep
demo behavior identical.
```

### Goal

A data-driven object-type registry so new types are additive, not code edits.

### Add Files

- `sources/app/scene/SceneObjectRegistry.h`
- `sources/app/scene/SceneObjectRegistry.cpp` (ungated engine code)

### Modify Files

- `sources/app/levels/DemoLevel.cpp`
- `test_cube.vcxproj` / `test_cube.vcxproj.filters`

### Tasks

- The registry maps a `type` string to a creator that returns the runtime object(s).
  Register `staticMesh`, `transparentMesh`, rotating mesh (`staticMesh` +
  `rotateSpeedDeg`), `metalRoughGrid`, `instancedModels`, `ocean`, `debugGrid`.
- Move the per-type creation bodies out of `DemoLevel` into creators; reuse
  `SceneObjectFactory`.
- Generators emit their runtime objects all tagged with the generator's single editor
  id, so picking any child selects the one generator entity.

### Acceptance

- Debug build succeeds (editor on and off).
- A loader driven by the registry reproduces the demo scene identically.

## Step 17: Generic JsonLevel And Delete DemoLevel

### Prompt

```text
Now implement Step 17 from docs/level_editor_implementation_plan.md.
Add a generic path-parameterized JsonLevel that loads any level file through the
registry, make it the default, and delete DemoLevel.
```

### Goal

Load any level file by path; remove the hardcoded loader.

### Add Files

- `sources/app/levels/JsonLevel.h`
- `sources/app/levels/JsonLevel.cpp`

### Modify Files

- `sources/app/levels/LevelManager.h` / `LevelManager.cpp`
- `sources/app/App.cpp`
- delete `sources/app/levels/DemoLevel.h` / `DemoLevel.cpp`
- `test_cube.vcxproj` / `test_cube.vcxproj.filters`

### Tasks

- `JsonLevel : Level` holds a file path; `Load` parses camera/lights/skybox/ocean and
  `objects[]` via the registry (Step 16), using the id assignment from Steps 14–15.
- Add `LevelManager::LoadLevelFromPath(path)`.
- `App` registers `JsonLevel("data/levels/demo.json")` as the default level; remove
  `DemoLevel` and its registration.

### Acceptance

- Debug build succeeds (editor on and off).
- App starts on `demo.json` via `JsonLevel`; demo is visually identical.
- `DemoLevel` is gone from the project.

## Step 18: Save Document To Any Path

### Prompt

```text
Now implement Step 18 from docs/level_editor_implementation_plan.md.
Save the full editor document (loaded + edited objects) to an arbitrary path and
reload it through JsonLevel.
```

### Goal

Round-trip any level to and from disk through the editor.

### Modify Files

- `sources/editor/serialization/LevelDocumentSerializer.*`
- `sources/editor/EditorController.*`

### Tasks

- The serializer writes the full document (loaded objects with ids included) plus the
  preserved top-level sections, to an arbitrary path; reload via `JsonLevel`.
- Stable object order, stable ids, consistent indentation; unmodeled fields and types
  written back from `properties` verbatim.

### Acceptance

- Debug build succeeds (editor on and off).
- Load demo, save a copy, and the diff is stable (ids added, order preserved,
  generators and unmodeled fields verbatim).
- Reload the copy and all objects remain fully editable.

## Step 19: Level File Menu

### Prompt

```text
Now implement Step 19 from docs/level_editor_implementation_plan.md.
Add a File menu (New / Open / Save / Save As / Recent) with a file picker over the
JsonLevel load/save path.
```

### Goal

A real load/save UI for any level file, replacing the temporary path text box.

### Modify Files

- `sources/editor/EditorController.*`
- small UI/serialization helpers as needed; `test_cube.vcxproj` / `.filters`

### Tasks

- Menu bar: **New / Open / Save / Save As / Recent** with a file picker over
  `data/levels` (and arbitrary paths). New = empty document with default
  camera/lights. Open/Reload route through `JsonLevel` so everything stays editable.
- Remove the Step 13 path text box.

### Acceptance

- Debug build succeeds (editor on and off).
- Open a second level file, edit it, Save As, and reopen it.
- New gives an empty editable scene.

## Step 20: GPU Object-ID Picking

### Prompt

```text
Now implement Step 20 from docs/level_editor_implementation_plan.md.
Add an R32_UINT object-id render target and a one-pixel readback so viewport
selection is pixel-accurate. Replace the CPU ray-vs-bounds picking.
```

### Goal

Pixel-accurate selection of any object under the cursor.

### Modify Files

- `sources/rendering/core/RenderTargetManager.*` (objectID target)
- `shaders/gbuffer_common.hlsl`, `shaders/gbuffer*.hlsl` (write the id)
- `sources/rendering/renderables/RenderableObject.cpp` (set the id per draw)
- the scene renderer (bind the target, issue the readback)
- `sources/rendering/core/Renderer.*` (readback buffer)
- `sources/editor/ui/ViewportGizmo.*` (use the readback id)

### Tasks

- Add an `R32_UINT` `objectID` target to `DeferredTargets` at **render** resolution;
  clear to a sentinel = "none".
- Add `uint objectId` to the PerObject CBV; set it per draw in
  `RenderableObject::Render` from `objectIds_`; the GBuffer pixel shader (and the
  instanced path) writes it.
- On click, copy the 1×1 region at the cursor (scaled to render res:
  `cursorX * renderW/displayW`) into a readback buffer (reuse the `OceanSimulation`
  readback pattern), fence, map, then set selection from the id. Jitter is irrelevant
  for discrete ids.

### Acceptance

- Debug build succeeds (editor on and off).
- Clicking any pixel of any object (loaded or spawned, overlapping, concave) selects
  exactly that object.

## Step 21: Selection Outline

### Prompt

```text
Now implement Step 21 from docs/level_editor_implementation_plan.md.
Add a stencil-based outline pass that highlights the selected object.
```

### Goal

A visible outline around the selected object.

### Modify Files

- the scene renderer (new `Pass_SelectionOutline`)
- selected-object stencil marking in the GBuffer draw path
- a fullscreen outline shader under `shaders/`

### Tasks

- Mark the selected object's draws with a stencil bit during GBuffer (depth is
  `D32_FLOAT_S8X24_UINT`, so stencil is available).
- After Compose / before Tonemap, a fullscreen pass writes an outline where a selected
  texel borders a non-selected one. Insert `Pass_SelectionOutline` between DebugDraw
  and Tonemap; gate `WITH_EDITOR`.

### Acceptance

- Debug build succeeds (editor on and off).
- The selected object shows a crisp outline; deselecting removes it; works with DLSS
  on.

## Step 22: Environment Entities And Serialization

### Prompt

```text
Now implement Step 22 from docs/level_editor_implementation_plan.md.
Represent lights, camera, skybox, and ocean as document entities with ids and
round-trip them on save. Do not add editing UI yet.
```

### Goal

Make the top-level JSON sections first-class document entities.

### Modify Files

- `sources/editor/scene/EditorSceneDocument.*`
- `sources/editor/serialization/LevelDocumentSerializer.*`
- `sources/editor/ui/SceneOutlinerPanel.*`

### Tasks

- Represent each light, the camera, the skybox, and the ocean as document entities
  with ids; serialize them back to their JSON sections on save.
- Show them in the outliner alongside mesh objects.

### Acceptance

- Debug build succeeds (editor on and off).
- The environment sections appear as entities in the outliner.
- Save and reload round-trip them unchanged.

## Step 23: Editor Icon Billboards And Picking

### Prompt

```text
Now implement Step 23 from docs/level_editor_implementation_plan.md.
Draw editor icon billboards for non-mesh entities (lights, camera) and make them
pickable via the object-id buffer.
```

### Goal

Select lights and the camera by clicking their viewport icons.

### Modify Files

- the editor overlay draw path; an icon shader under `shaders/`
- the scene renderer
- `sources/editor/ui/ViewportGizmo.*`

### Tasks

- Draw editor icon billboards for non-mesh entities in an editor overlay, writing their
  id into the object-ID buffer (Step 20) so they pick.
- All gated `WITH_EDITOR`.

### Acceptance

- Debug build succeeds (editor on and off).
- Clicking a light's icon selects that light.

## Step 24: Environment Gizmos And Inspectors

### Prompt

```text
Now implement Step 24 from docs/level_editor_implementation_plan.md.
Add gizmos and inspector fields for lights, camera, skybox, and ocean, with undoable
live edits.
```

### Goal

Edit environment entities live.

### Modify Files

- `sources/editor/ui/ViewportGizmo.*`
- `sources/editor/ui/InspectorPanel.*`
- new commands as needed

### Tasks

- Gizmos: translate for point lights; translate+rotate for spot/directional; camera
  transform.
- Inspectors: light color / intensity / range / angles / shadow bias; camera fov and
  clip; skybox texture; ocean preset/params. Live patch into `LightManager`/`Scene`;
  undoable.

### Acceptance

- Debug build succeeds (editor on and off).
- Select a light by its icon, move and recolor it live, save, reload — it persists.
- Same round-trip for the camera, skybox, and ocean.

## Step 25: Editor Hotkeys

### Prompt

```text
Now implement Step 25 from docs/level_editor_implementation_plan.md.
Add an editor hotkey layer using the common Unreal Editor shortcuts, wired to the
editor actions built in earlier steps. Do not add new actions beyond those.
```

### Goal

Drive the existing editor actions (gizmo mode, selection, delete, undo/redo, save,
focus) from keyboard shortcuts matching Unreal Engine conventions.

### Add Files

- `sources/editor/EditorHotkeys.h`
- `sources/editor/EditorHotkeys.cpp`

May instead live inside `EditorController` if include dependencies are trivial,
but a small separate translation unit keeps the shortcut table easy to extend.

### Modify Files

- `sources/editor/EditorController.h`
- `sources/editor/EditorController.cpp`
- `sources/editor/ui/ViewportGizmo.*` (set the gizmo mode from hotkeys)
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

### Hotkey Map (Unreal conventions)

Transform-mode keys (apply only when the camera is not being flown — see Input
Rules):

- `Q` — Select mode (gizmo hidden / select only)
- `W` — Translate (move) gizmo
- `E` — Rotate gizmo
- `R` — Scale gizmo
- `Spacebar` — cycle Translate → Rotate → Scale

Action keys:

- `Delete` — delete the selected object (`DeleteObjectCommand`, Step 9)
- `Ctrl+Z` — Undo
- `Ctrl+Y` and `Ctrl+Shift+Z` — Redo
- `Ctrl+S` — Save the current level (Step 13)
- `F` — focus / frame the camera on the selected object
- `Esc` — clear the selection

### Input Rules

- Hotkeys run only while the editor window is open; closing it restores original
  input behavior.
- Suppress all hotkeys when ImGui is capturing text input
  (`ImGui::GetIO().WantTextInput`) so typing in the search box or inspector
  fields never triggers a shortcut.
- `Q/W/E/R/Spacebar` collide with the engine's WASD/QE fly-camera movement.
  Mirror Unreal: apply them only when the camera is NOT in fly mode (the
  look-toggle / right mouse button is not held). While flying, those keys move
  the camera as before.
- Use `ImGui::GetIO().KeyCtrl` / `KeyShift` plus `ImGui::IsKeyPressed` for the
  chorded shortcuts.
- All of this is `WITH_EDITOR`-gated.

### Tasks

- Route `Q/W/E/R/Spacebar` to the viewport gizmo mode (Step 11).
- Route `Delete` through the command stack as a `DeleteObjectCommand` (Step 9).
- Route `Ctrl+Z` / `Ctrl+Y` / `Ctrl+Shift+Z` to `EditorCommandStack::Undo`/`Redo`.
- Route `Ctrl+S` to the level save path (Step 13).
- `F` moves the camera to frame the selected object's transform; `Esc` clears the
  selection.
- Show the active transform mode and the key hints in the editor UI.

### Acceptance

- Debug build succeeds (editor on and off).
- With the editor open and no text field focused: `W/E/R/Q` change the transform
  mode, `Spacebar` cycles it, `Delete` removes the selected object, `Ctrl+Z` /
  `Ctrl+Y` undo/redo, `Ctrl+S` saves, `F` frames the selection, `Esc` deselects.
- Typing in the content browser search box does not trigger shortcuts.
- Holding the fly-camera button still moves with WASD/QE; releasing it restores
  the transform-mode keys.
- Closing the editor restores original input behavior.

## Step 26: Extensibility Cleanup

### Prompt

```text
Now implement Step 26 from docs/level_editor_implementation_plan.md.
Refactor editor panels and object/property behavior into small registries.
Do not change user-visible behavior.
```

This step is largely subsumed by the Step 16 type registry; the remaining work is the
editor panel and property-drawer registries below.

### Goal

Make future object types and panels additive.

### Add Or Refactor Toward

```cpp
class IEditorPanel
{
public:
    virtual ~IEditorPanel() = default;
    virtual void Draw(EditorContext& ctx) = 0;
};

class IEditorObjectFactory
{
public:
    virtual ~IEditorObjectFactory() = default;
    virtual std::string_view Type() const = 0;
    virtual nlohmann::json BuildDefaultJson(const EditorAssetRecord* sourceAsset,
        const EditorContext& ctx) const = 0;
};

class IEditorPropertyDrawer
{
public:
    virtual ~IEditorPropertyDrawer() = default;
    virtual std::string_view Type() const = 0;
    virtual void Draw(EditorContext& ctx, EditorObject& object) = 0;
};
```

Register built-ins:

- content browser panel
- outliner panel
- inspector panel
- viewport/gizmo panel
- `staticMesh` factory
- `transparentMesh` factory
- `staticMesh` property drawer
- `transparentMesh` property drawer

### Acceptance

- Debug build succeeds.
- User-visible editor behavior is unchanged.
- Adding a future `pointLight` object type would require registering a factory
  and property drawer rather than editing the content browser core.

## Final Manual QA

Run this after the last implemented step in a session:

- app starts and shows the existing demo level
- F1 developer window still works
- level editor opens and closes
- content browser lists meshes/materials/textures/levels/shaders
- search and filter work
- refresh works
- static mesh spawn from `models/box.obj` renders
- transparent mesh spawn from `models/sphere.obj` renders
- spawned object can be selected in outliner
- transform edits move the rendered object
- click an object in the viewport to select it
- move, rotate, and scale with the gizmo; one gizmo drag is one undo
- undo removes spawned object
- redo restores spawned object
- delete selected object is undoable
- save to a copy of `data/levels/demo.json`
- reload saved copy and verify spawned/edited objects remain
- load demo.json, save an unedited copy, reload, and confirm rotating boxes still
  rotate and the metal-rough grid and instanced models remain
- reload original demo level through existing UI
- toggle wireframe, reflections, instancing, and LOD after spawning
- with the editor open and no text field focused: W/E/R/Q switch transform mode,
  Spacebar cycles it, Delete removes the selection, Ctrl+Z/Ctrl+Y undo/redo,
  Ctrl+S saves, F frames the selection, Esc deselects
- typing in the content browser search box does not trigger shortcuts
- holding the fly-camera button still moves with WASD/QE
- close editor and verify camera input still works

## Stop Conditions

Stop and report clearly if:

- live object spawning cannot be made safe without a larger scene lifetime
  change
- Visual Studio project updates become ambiguous
- object `Init` cannot be driven from an editor-opened `UploadBatch` (verified
  workable in this codebase: `Init` only needs an open upload batch, but stop if a
  hidden dependency on `FinalizeLevelLoad` surfaces)
- build breaks in existing renderer code unrelated to editor changes

When stopped, leave the repository buildable unless explicitly instructed to
continue through a broken intermediate state.

