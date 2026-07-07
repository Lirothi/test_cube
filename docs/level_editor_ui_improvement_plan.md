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

## UE-Like Content Browser Direction

Reference shape:

- Epic Content Browser overview:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/content-browser-in-unreal-engine
- Epic Content Browser interface:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/content-browser-interface-in-unreal-engine
- Epic filters and collections:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/filters-and-collections-in-unreal-engine
- Epic working with assets:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/working-with-assets-in-unreal-engine

Target behavior for this repository:

- Treat the Content Browser as a folder-first editor surface, not just an asset
  table. The main areas should become Navigation Bar, Sources Panel, optional
  Collections/Favorites, Filters/Search Bar, Asset View, Details/Preview, and
  Settings.
- Add an expandable folder tree in the Sources Panel. It should behave like a
  regular file explorer: select a folder, expand/collapse children, search
  folders, and show folder rows in the Asset View.
- Keep a UE-style breadcrumb and back/forward path history above the Asset View.
- Support view modes in the Settings menu. Aim for Tiles, List, and Columns,
  but ship them in separable increments.
- Search and filters should apply to the selected folder or collection. Multiple
  type filters are additive.
- Collections/Favorites should be references to assets, not physical folders.
- Assets in this repository are raw files and JSON records, not UE packages.
  Do not implement destructive move/delete/rename behavior for non-empty asset
  folders until references can be updated or repaired safely.

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

## Step 6: Content Browser Asset Model And Folder Tree

Goal: give the Content Browser a folder-aware data model and Sources Panel before
adding more asset actions.

Primary files:

- `sources/editor/assets/AssetRegistry.*`
- `sources/editor/ui/ContentBrowserPanel.*`

Implementation notes:

- Add virtual content paths to assets, derived from existing physical paths.
  Suggested roots:
  - `/Game/Models` for mesh/model assets;
  - `/Game/Textures` for texture assets;
  - `/Game/Materials` for material presets;
  - `/Game/Levels` for level JSON files;
  - `/Game/Shaders` for shader assets.
- Expose a folder tree from `AssetRegistry` with child folders, direct asset
  ids, and recursive asset counts.
- Preserve existing asset ids and action behavior. This step is read-only from
  the filesystem perspective.
- Keep sort deterministic: folders first A-Z, then assets A-Z.
- Track selected source folder in `ContentBrowserPanel` state.
- Add a recursive display flag, but default to direct contents of the selected
  folder if that is practical with the current registry.

Acceptance criteria:

- The Sources Panel shows a project root with expandable virtual folders.
- Selecting a folder changes the Asset View contents.
- Refresh rebuilds the tree and keeps the selected folder if it still exists.
- Folder filtering/search does not mutate the registry or physical files.
- Existing mesh spawn behavior still works.

Validation:

- Build `Debug|x64`.
- Refresh the Content Browser and navigate every root folder.
- Confirm asset counts match visible records for at least Models, Textures, and
  Levels.

## Step 7: UE-Style Content Browser Layout Shell

Goal: restructure the panel into the recognizable UE Content Browser areas.

Primary files:

- `sources/editor/ui/ContentBrowserPanel.*`

Implementation notes:

- Add a top Navigation Bar with:
  - Add, Import, and Save All controls as disabled or placeholder actions until
    real implementations exist;
  - Back and Forward folder history;
  - a clickable breadcrumb for the selected virtual folder;
  - Refresh;
  - Settings.
- Add a left Sources Panel containing the folder tree from Step 6.
- Keep the right side as Asset View and reserve space for a bottom/right
  Details/Preview area.
- Add a compact status line with displayed asset count and selected folder path.
- Use stable split sizes so the panel remains usable at small window sizes.
- Do not add collections in this step unless they are read-only placeholders.

Acceptance criteria:

- The browser visibly has Navigation Bar, Sources Panel, Asset View, and
  Details/Preview regions.
- Back, Forward, and breadcrumbs navigate folders without changing asset
  selection incorrectly.
- Disabled placeholder controls explain why they are unavailable.
- No text or controls overlap at the default editor window size.

Validation:

- Build `Debug|x64`.
- Navigate through nested folders with the Sources tree, breadcrumbs, and
  Back/Forward controls.
- Resize the editor window and confirm the layout remains readable.

## Step 8: Asset View Modes, Search, And Filters

Goal: make folder browsing feel close to UE by combining folder rows, asset rows,
view modes, search, and additive type filters.

Primary files:

- `sources/editor/ui/ContentBrowserPanel.*`
- `sources/editor/assets/AssetRegistry.*`

Implementation notes:

- Represent Asset View entries as either folder entries or asset entries.
- Double-click folder entries to enter them. Double-click assets should keep
  existing asset-specific behavior where present.
- Add a Settings view-mode enum for Tiles, List, and Columns. Implement at least
  Tiles and List in this step; leave Columns disabled with a reason if it cannot
  be implemented cleanly.
- Keep the current search box but scope results to the selected folder or
  recursive view.
- Add a Filters button/menu with additive type filters for Mesh, Material,
  Texture, Level, and Shader.
- Show active filters as compact chips/buttons that can be toggled or removed.
- Support a folder search field in the Sources Panel. Prefix `-text` should hide
  folders containing that text, matching UE's exclusion behavior.

Acceptance criteria:

- Asset View can show folder entries and assets together.
- Search, type filters, selected folder, and recursive display combine
  predictably.
- The displayed count reflects the final filtered Asset View.
- View mode changes do not lose selected folder or selected asset.
- Clearing search restores the same folder contents.

Validation:

- Build `Debug|x64`.
- Navigate to each virtual root and test search, type filters, recursive mode,
  and view-mode switching.
- Verify that an active filter cannot make the panel look broken; empty states
  must say what filters are active.

## Step 9: Content Browser Folder Operations And Context Menus

Goal: add UE-like folder and asset context menus while staying safe with raw
repository files.

Primary files:

- `sources/editor/ui/ContentBrowserPanel.*`
- `sources/editor/assets/AssetRegistry.*`
- `sources/editor/EditorController.cpp`

Implementation notes:

- Add empty-space, folder-row, source-tree, and asset-row context menus.
- Implement `New Folder` only for virtual paths that map to approved physical
  content roots. Validate names and reject paths containing separators or
  traversal.
- Implement `Delete Empty Folder` with confirmation only when the mapped
  directory is empty.
- Do not surface future folder actions such as rename or move/copy in normal
  context menus until asset reference repair exists.
- Keep context menus scoped to the clicked resource type. Avoid generic "all
  commands" menus; show disabled items only when the action is relevant to that
  resource but unavailable because of current selection or safety state.
- Add asset actions that do not mutate files:
  - Copy Virtual Path;
  - Copy Physical Path;
  - Show In Sources;
  - Refresh.
- Keep filesystem writes outside editor roots impossible by construction.

Acceptance criteria:

- Right-clicking empty space can create a new folder under the selected source
  when the selected source maps to a writable editor content root.
- Empty folder delete requires confirmation and refuses non-empty folders.
- Unavailable relevant actions explain their current blocker without cluttering
  unrelated resource menus.
- Asset context menus expose copy/show actions without breaking existing spawn
  actions.

Validation:

- Build `Debug|x64`.
- Create and delete an empty folder under an approved content root.
- Attempt invalid names and confirm they are rejected.
- Confirm non-empty folder delete and folder move/copy are unavailable.

## Step 10: Content Browser Asset Actions

Goal: make assets useful from the UE-like browser after the folder model exists.

Primary files:

- `sources/editor/ui/ContentBrowserPanel.*`
- `sources/editor/EditorController.cpp`
- `sources/editor/commands/SetMaterialCommand.*`

Implementation notes:

- Preserve existing mesh spawn actions and surface them from asset context menus.
- Enable "Assign Material to Selected" only when:
  - the selected asset is `EditorAssetType::MaterialPreset`;
  - the selected object exists;
  - the selected object type supports material assignment.
- Execute material assignment through `SetMaterialCommand`.
- Add double-click and context-menu actions for level assets:
  - Open Level;
  - Open Level Preserving Camera, if supported by existing controller helpers.
- Reuse `EditorController::RequestOpenLevelPath` or existing level-open helpers.
- Do not auto-save dirty documents. If current dirty-document handling cannot
  prompt before loading, defer level-open implementation and keep the action
  disabled with a reason.

Acceptance criteria:

- Right-clicking a material asset can assign it to a selected static mesh.
- Undo/redo restores the previous material.
- Double-clicking a level asset starts a level load only through an explicit
  dirty-document-safe path.
- Recent levels and level status update consistently with File > Open.
- Unsupported actions are disabled, not silently ignored.

Validation:

- Build `Debug|x64`.
- Assign each material preset to a selected mesh and undo/redo.
- Open `data/levels/demo.json` and another level from the Content Browser.

## Step 11: Content Browser Drag And Drop

Goal: support fast UE-like workflows without adding unsafe file moves.

Primary files:

- `sources/editor/ui/ContentBrowserPanel.*`
- `sources/editor/ui/InspectorPanel.cpp`
- `sources/editor/ui/ViewportGizmo.cpp`
- `sources/editor/EditorController.cpp`

Implementation notes:

- Define ImGui drag payloads for `EditorAssetId` and virtual folder paths.
- Drag mesh assets into the viewport to spawn using existing object factories.
- Drag material assets onto the inspector or selected object target to execute
  `SetMaterialCommand`.
- Optional: drag cube textures onto a selected skybox environment entity.
- Do not implement drag-to-move files between folders until reference repair and
  undo behavior are designed.
- Make unsupported drops visibly unavailable.

Acceptance criteria:

- Dragging a mesh asset into the viewport spawns a new object.
- Dragging a material asset onto a supported selected mesh assigns it.
- Dragging folders or assets onto folder targets does not move files yet and
  communicates that this is intentionally disabled.
- Unsupported drops do not mutate state.

Validation:

- Build `Debug|x64`.
- Smoke test drag/drop with a mesh and material.
- Try an unsupported folder move and confirm no files move.

## Step 12: Content Browser Details, Preview, Collections, And Favorites

Goal: add inspection and organization features that make the browser feel closer
to UE without requiring full package management.

Primary files:

- `sources/editor/ui/ContentBrowserPanel.*`
- `sources/editor/assets/AssetRegistry.*`
- `sources/editor/EditorController.cpp`

Implementation notes:

- Replace the raw details footer with a clear Details/Preview panel.
- Add compact type icons or labels for Mesh, Material, Texture, Level, Shader,
  and Folder.
- For textures, show dimensions, format, kind, mip count, and invalid metadata
  state.
- For material presets, show the source file and preset name. Optional: parse
  common texture slots from `data/materials.json`.
- Add Favorites as local references to assets or folders.
- Add Collections only as reference lists, not physical folders. Collection
  delete must not delete assets.
- Optional: add Recently Opened/Recently Used as a generated collection.
- Avoid GPU thumbnail loading unless it remains small and isolated.

Acceptance criteria:

- Selecting any asset type shows useful type-specific details.
- Selecting a folder shows child folder and asset counts.
- Invalid/unreadable texture metadata is obvious.
- Favorites or Collections can show referenced assets without moving files.
- Details remain visible when the selected asset is hidden by the active filter.

Validation:

- Build `Debug|x64`.
- Select each asset type and at least one folder in the Content Browser.
- Add and remove a favorite or collection reference without moving/deleting the
  underlying asset.

## Step 13: Command-Backed Environment Edits

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

## Step 14: Persist Editor Panel UI State

Goal: make the editor reopen in the user's preferred layout/filter state.

Primary files:

- `sources/editor/EditorController.cpp`
- `sources/editor/ui/SceneOutlinerPanel.*`
- `sources/editor/ui/ContentBrowserPanel.*`

Implementation notes:

- Extend `editor_state.json` under the existing `levelEditor` object.
- Persist panel visibility, outliner group expansion, content browser type
  filter, selected source folder, recursive mode, view mode, source/details
  split sizes, and selection outline radius.
- Avoid persisting search strings unless the user explicitly asks; stale filters
  can make panels appear empty.
- Persist Favorites/Collections only if Step 12 implemented them.

Acceptance criteria:

- Closing/reopening the app restores panel visibility and group expansion.
- Content Browser reopens on the same source folder and view mode.
- Corrupt or missing state falls back to defaults without breaking startup.

Validation:

- Build `Debug|x64`.
- Run once, change panel state, close, relaunch, and confirm state restoration.

## Step 15: UX Cleanup Pass

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

Recommended order from this point, prioritizing a UE-like Content Browser:

1. Step 6: Content Browser Asset Model And Folder Tree
2. Step 7: UE-Style Content Browser Layout Shell
3. Step 8: Asset View Modes, Search, And Filters
4. Step 9: Content Browser Folder Operations And Context Menus
5. Step 10: Content Browser Asset Actions
6. Step 11: Content Browser Drag And Drop
7. Step 12: Content Browser Details, Preview, Collections, And Favorites
8. Step 4: Outliner Groups And Context Actions
9. Step 5: Rename Command
10. Step 13: Command-Backed Environment Edits
11. Step 14: Persist Editor Panel UI State
12. Step 15: UX Cleanup Pass

Steps 4 and 5 remain valuable, but they are intentionally moved behind the
Content Browser foundation because the current product direction is a UE-like
folder browser. Step 13 is intentionally later because it has broader
command/runtime impact. Step 11 is later because drag/drop touches several UI
surfaces and is easier once the underlying browser actions already exist.

## Stop Conditions

Stop and report instead of pushing through if:

- a step requires changing runtime behavior in no-editor builds;
- a command cannot reliably restore both document state and live runtime state;
- level loading can discard dirty work without an explicit user path;
- a folder operation would move/delete assets without a reference repair plan;
- a filesystem operation can escape approved content roots;
- a UI action targets environment and regular object rows differently but appears
  identical to the user;
- GPU/resource lifetime changes are needed outside the editor draw/tick mutation
  window.
