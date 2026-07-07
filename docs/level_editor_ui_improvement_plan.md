# Level Editor UI Improvement Plan

Executor-facing plan for improving the current level editor UI, with emphasis on
the Content Browser and Scene Outliner.

This plan is intentionally split into separable steps. Each step should leave the
repo buildable and usable on its own. Do not combine steps unless the user asks
for a larger batch.

## Current Baseline

- `ContentBrowserPanel` is a searchable/filterable asset table over
  `AssetRegistry`. It supports refresh, asset selection, details, and mesh spawn
  actions through `IEditorObjectFactory`.
- `SceneOutlinerPanel` is a flat table over `EditorSceneDocument::Objects()` plus
  a labeled environment section. It supports row selection, object delete, and
  enable toggles.
- Selection is shared across outliner, inspector, viewport picking, object-id
  readback, icon billboards, and selection outline.
- Regular object edits mostly use `EditorCommand`. Environment edits are mostly
  direct live patches and are not undoable.

Important source files:

- `sources/editor/ui/ContentBrowserPanel.h`
- `sources/editor/ui/ContentBrowserPanel.cpp`
- `sources/editor/ui/SceneOutlinerPanel.h`
- `sources/editor/ui/SceneOutlinerPanel.cpp`
- `sources/editor/ui/InspectorPanel.cpp`
- `sources/editor/ui/ViewportGizmo.cpp`
- `sources/editor/EditorController.cpp`
- `sources/editor/EditorExtensionRegistry.*`
- `sources/editor/assets/AssetRegistry.*`
- `sources/editor/scene/EditorSceneDocument.*`
- `sources/editor/commands/*`

## General Executor Rules

- Keep all editor-only C++ under `WITH_EDITOR`.
- Route scene/document mutations through `EditorCommand` whenever practical.
  If a step intentionally leaves an edit non-undoable, make that explicit in the
  UI or comments.
- Preserve current repository line endings. C++ and Visual Studio project files
  use CRLF. Markdown files use LF.
- Avoid broad refactors. Keep each step focused on the panel, command, or model
  surface named by the step.
- After C++ or project-file edits, build `Debug|x64`. For editor/engine boundary
  edits, also verify a no-editor build using the recipe in
  `docs/level_editor_HANDOFF.md`.
- Do not commit, push, switch branches, or rewrite unrelated user changes.

## Step 1: Outliner Search And Type Filtering

Goal: make the current flat outliner usable on medium-size levels without
changing document semantics.

Primary files:

- `sources/editor/ui/SceneOutlinerPanel.h`
- `sources/editor/ui/SceneOutlinerPanel.cpp`

Implementation notes:

- Add persistent panel state: search buffer, object/environment visibility
  toggles, and a type filter.
- Filter by name, type, id text, and common asset fields such as `model` when
  present.
- Show visible row count versus total row count.
- Keep regular objects and environment entities in their existing order.
- Do not change selection semantics. If the selected item is hidden by the
  filter, keep the selection and make that state visible in the panel footer.

Acceptance criteria:

- Typing a search term filters object and environment rows.
- Filtering never changes document order or selected object id.
- Enable checkboxes and context menus still work on filtered rows.
- Empty result states are clear and do not leave a blank table.

Validation:

- Build `Debug|x64`.
- Smoke test with `data/levels/demo.json`, which currently has many object rows.

## Step 2: Outliner Sorting And Stable Row Identity

Goal: allow quick scanning by name/type/id without breaking command targets.

Primary files:

- `sources/editor/ui/SceneOutlinerPanel.*`

Implementation notes:

- Add ImGui table sorting for Name, Type, ID, and On.
- Sort only the displayed row references, not `EditorSceneDocument` storage.
- Keep row IDs based on `EditorObjectId`, not filtered/sorted row index.
- Preserve the existing environment label or replace it with a sortable row model
  that still distinguishes environment entities from regular objects.

Acceptance criteria:

- Clicking table headers changes display order only.
- Selecting, deleting, and toggling rows still target the intended object after
  sorting.
- Saving the level after sorting does not reorder JSON objects.

Validation:

- Build `Debug|x64`.
- Manually sort by each column and delete/toggle a row.

## Step 3: Preserve Object Order Across Delete Undo

Goal: fix a workflow bug where undoing a delete re-adds the restored object at
the end of the document/outliner.

Primary files:

- `sources/editor/scene/EditorSceneDocument.h`
- `sources/editor/scene/EditorSceneDocument.cpp`
- `sources/editor/commands/DeleteObjectCommand.*`

Implementation notes:

- Add a document API that can insert an `EditorObject` at a stable index while
  keeping `nextId_` correct.
- Have `DeleteObjectCommand` capture the removed object's original index.
- Undo should restore the object at its original index when possible.
- Keep behavior sane if intervening edits changed the document length: clamp the
  restore index to the valid range.

Acceptance criteria:

- Delete then undo restores the object to its previous outliner position.
- Saved object order is stable across delete/undo.
- Existing delete redo behavior remains correct.

Validation:

- Build `Debug|x64`.
- Run a manual delete/undo/save smoke test.

## Step 4: Outliner Groups And Context Actions

Goal: make scene contents navigable by purpose, not just raw load order.

Primary files:

- `sources/editor/ui/SceneOutlinerPanel.*`
- `sources/editor/EditorController.cpp`

Implementation notes:

- Add collapsible groups such as Environment, Meshes, Lights, Cameras, and Other.
- Keep group expansion state inside `SceneOutlinerPanel`.
- Add row context actions where supported: Delete, Duplicate, Frame Selection,
  Rename, Enable/Disable.
- Return explicit `OutlinerAction` values for new actions and execute commands in
  `EditorController`, matching the current delete/enable pattern.
- For unsupported environment actions, either hide them or disable them with a
  clear label.

Acceptance criteria:

- Groups can be collapsed without losing selection.
- Context actions do not appear enabled when they cannot work.
- Duplicate/delete/frame use existing command/hotkey behavior.

Validation:

- Build `Debug|x64`.
- Smoke test group collapse, context menu actions, and selection outline.

## Step 5: Rename Command

Goal: make object renaming undoable and consistent between inspector and
outliner.

Primary files:

- `sources/editor/commands/`
- `sources/editor/ui/InspectorPanel.cpp`
- `sources/editor/ui/SceneOutlinerPanel.*`
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

Implementation notes:

- Add `RenameObjectCommand` for regular document objects.
- Consider whether environment entity rename should be supported. If supported,
  add an environment-aware command or a generic command that can find either list.
- Replace direct inspector name edits with command-backed commit-on-deactivate
  behavior.
- If outliner inline rename is added, use the same command.

Acceptance criteria:

- Renaming through inspector is undoable.
- Outliner immediately reflects renamed objects.
- Empty names are rejected or normalized to a sensible fallback.

Validation:

- Build `Debug|x64`.
- Verify undo/redo for rename.
- Verify no-editor build if new command files touch project configuration.

## Step 6: Content Browser Material Assignment

Goal: turn material assets from passive records into a useful editing action.

Primary files:

- `sources/editor/ui/ContentBrowserPanel.*`
- `sources/editor/EditorController.cpp`
- `sources/editor/commands/SetMaterialCommand.*`

Implementation notes:

- Enable "Assign Material to Selected" only when:
  - the selected asset is `EditorAssetType::MaterialPreset`;
  - the selected object exists;
  - the selected object type supports material assignment.
- Return a new `ContentBrowserAction` for material assignment.
- Execute `SetMaterialCommand` from `EditorController`.
- Keep unsupported object types disabled, not silently ignored.

Acceptance criteria:

- Right-clicking a material asset can assign it to a selected static mesh.
- Undo/redo restores the previous material.
- The action is disabled for textures, shaders, levels, and unsupported selected
  objects.

Validation:

- Build `Debug|x64`.
- Assign each material preset to a selected mesh and undo/redo.

## Step 7: Content Browser Level Actions

Goal: make level assets openable from the Content Browser.

Primary files:

- `sources/editor/ui/ContentBrowserPanel.*`
- `sources/editor/EditorController.cpp`

Implementation notes:

- Add double-click and context-menu actions for level assets:
  - Open Level
  - Open Level Preserving Camera
- Reuse `EditorController::RequestOpenLevelPath` or existing level-open helpers.
- Do not auto-save dirty documents without explicit user action.
- If the current document is dirty, show a confirmation path before loading or
  defer this step until a dirty-document prompt exists.

Acceptance criteria:

- Double-clicking a level asset starts a level load.
- Recent levels and level status update consistently with File > Open.
- Dirty-document behavior is explicit and cannot silently discard work.

Validation:

- Build `Debug|x64`.
- Open `data/levels/demo.json` and another level from the Content Browser.

## Step 8: Content Browser Asset Details And Preview Polish

Goal: improve inspection value before adding heavier asset-management features.

Primary files:

- `sources/editor/ui/ContentBrowserPanel.*`
- `sources/editor/assets/AssetRegistry.*`

Implementation notes:

- Replace the raw details footer with a clear details panel.
- Add compact type icons or labels for Mesh, Material, Texture, Level, and Shader.
- For textures, show dimensions, format, kind, mip count, and invalid metadata
  state.
- For material presets, show the source file and preset name. Optional: parse
  common texture slots from `data/materials.json`.
- Avoid GPU thumbnail loading in this step unless the executor can keep it small
  and isolated.

Acceptance criteria:

- Selecting any asset type shows useful type-specific details.
- Invalid/unreadable texture metadata is obvious.
- Details remain visible when the selected asset is hidden by the active filter.

Validation:

- Build `Debug|x64`.
- Select each asset type in the Content Browser.

## Step 9: Drag And Drop From Content Browser

Goal: support faster creation and assignment without replacing existing context
menus.

Primary files:

- `sources/editor/ui/ContentBrowserPanel.*`
- `sources/editor/ui/InspectorPanel.cpp`
- `sources/editor/ui/ViewportGizmo.cpp`
- `sources/editor/EditorController.cpp`

Implementation notes:

- Define ImGui drag payloads for `EditorAssetId`.
- Drag mesh assets into the viewport to spawn using existing object factories.
- Drag material assets onto the inspector or selected object target to execute
  `SetMaterialCommand`.
- Optional: drag cube textures onto a selected skybox environment entity.
- Keep all mutations command-backed where commands exist.

Acceptance criteria:

- Dragging a mesh asset into the viewport spawns a new object.
- Dragging a material asset onto a supported selected mesh assigns it.
- Unsupported drops are ignored visibly or have disabled drop affordance.

Validation:

- Build `Debug|x64`.
- Smoke test drag/drop with a mesh and material.

## Step 10: Command-Backed Environment Edits

Goal: remove the largest undo/redo inconsistency in the editor.

Primary files:

- `sources/editor/commands/`
- `sources/editor/scene/EnvironmentRuntime.*`
- `sources/editor/ui/InspectorPanel.cpp`
- `sources/editor/ui/ViewportGizmo.cpp`
- `sources/editor/EditorController.cpp`

Implementation notes:

- Add command support for environment entities:
  - enable/disable light or ocean;
  - light position/direction/color/intensity edits;
  - skybox texture changes;
  - environment gizmo transform gestures.
- Commands should store before/after JSON properties and call
  `EnvironmentRuntime::Apply`, `SetEnabled`, `RebuildLights`, or `Remove` as
  appropriate.
- Keep direct live updates during drag, but push one command when the gesture
  commits, mirroring regular transform gizmo behavior.

Acceptance criteria:

- Environment enable toggles from outliner and inspector are undoable.
- Light gizmo movement is undoable as one command per gesture.
- Skybox texture edits are undoable.
- Existing live preview behavior remains.

Validation:

- Build `Debug|x64`.
- Smoke test undo/redo for point light, spot light, directional light, skybox,
  and ocean enabled state where present.

## Step 11: Persist Editor Panel UI State

Goal: make the editor reopen in the user's preferred layout/filter state.

Primary files:

- `sources/editor/EditorController.cpp`
- `sources/editor/ui/SceneOutlinerPanel.*`
- `sources/editor/ui/ContentBrowserPanel.*`

Implementation notes:

- Extend `editor_state.json` under the existing `levelEditor` object.
- Persist panel visibility, outliner group expansion, content browser type
  filter, and selection outline radius.
- Avoid persisting search strings unless the user explicitly asks; stale filters
  can make panels appear empty.

Acceptance criteria:

- Closing/reopening the app restores panel visibility and group expansion.
- Corrupt or missing state falls back to defaults without breaking startup.

Validation:

- Build `Debug|x64`.
- Run once, change panel state, close, relaunch, and confirm state restoration.

## Step 12: UX Cleanup Pass

Goal: remove small sources of confusion after functional work lands.

Primary files:

- `sources/editor/ui/ContentBrowserPanel.*`
- `sources/editor/ui/SceneOutlinerPanel.*`
- `sources/editor/ui/InspectorPanel.cpp`
- `sources/editor/EditorController.cpp`

Implementation notes:

- Standardize labels and action names across menu bar, content browser, outliner,
  and inspector.
- Show dirty state in the Level Editor window.
- Make disabled actions explain why they are disabled where ImGui makes this
  practical.
- Keep UI dense and tool-like. Do not add marketing-style panels or decorative
  layout.

Acceptance criteria:

- A user can tell what is selected, what can be edited, and whether the level has
  unsaved changes.
- Disabled actions no longer look like broken commands.
- No overlapping text or table footer clipping at common window sizes.

Validation:

- Build `Debug|x64`.
- Manually inspect Content Browser, Scene Outliner, Inspector, and Level Editor
  at small and default window sizes.

## Suggested Execution Order

Recommended order for highest value with lowest risk:

1. Step 1: Outliner Search And Type Filtering
2. Step 3: Preserve Object Order Across Delete Undo
3. Step 6: Content Browser Material Assignment
4. Step 2: Outliner Sorting And Stable Row Identity
5. Step 4: Outliner Groups And Context Actions
6. Step 7: Content Browser Level Actions
7. Step 8: Content Browser Asset Details And Preview Polish
8. Step 10: Command-Backed Environment Edits
9. Step 9: Drag And Drop From Content Browser
10. Step 11: Persist Editor Panel UI State
11. Step 12: UX Cleanup Pass

Step 10 is intentionally later because it has broader command/runtime impact.
Step 9 is later because drag/drop touches several UI surfaces and is easier once
the underlying actions already exist.

## Stop Conditions

Stop and report instead of pushing through if:

- a step requires changing runtime behavior in no-editor builds;
- a command cannot reliably restore both document state and live runtime state;
- level loading can discard dirty work without an explicit user path;
- a UI action targets environment and regular object rows differently but appears
  identical to the user;
- GPU/resource lifetime changes are needed outside the editor draw/tick mutation
  window.
