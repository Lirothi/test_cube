# Level Editor UI Improvement Plan

Executor-facing plan for improving the current level editor UI, with emphasis on
the Content Browser and Scene Outliner.

This plan is intentionally split into separable steps. Each step should leave the
repo buildable and usable on its own. Do not combine steps unless the user asks
for a larger batch.

## Current Baseline (updated 2026-07-13)

Steps 1 through 15 — including 5A, 11A, and 12A through 12E — are implemented
and committed. Steps 16 through 20 are also implemented. The editor currently provides:

- A UE-style Content Browser: navigation bar with back/forward and breadcrumbs,
  a resizable Sources panel (folder tree, Favorites, Collections), additive type
  filters, search, list/tile views, empty-folder create/delete, drag-and-drop to
  the viewport and Inspector, delayed hover hints, and a real thumbnail pipeline
  (`AssetThumbnailCache` + `EditorPreviewRenderer`) covering textures, meshes,
  and material presets. Cube-texture previews and an on-disk thumbnail cache
  remain open (Step 12F).
- A Scene Outliner with search, type filters, sorting, collapsible groups, row
  context actions (delete, duplicate, rename, frame, enable), and environment
  entities (lights, camera, skybox, ocean) with enable toggles.
- An Inspector with command-backed name/enabled/transform/material editing,
  registry-driven per-type property drawers, environment inspectors, and asset
  drop targets.
- Viewport interaction: GPU object-id picking, stencil selection outline,
  ImGuizmo translate/rotate/scale (one drag = one undo entry), environment light
  gizmos, icon billboards, and selected-light wireframes.
- A full command stack with undo/redo, a Command History window (Step 5A), and
  hotkeys (Q/W/E/R/Space, Delete, Ctrl+D, Ctrl+Z/Y, Ctrl+Shift+Z, Ctrl+S, F,
  Esc).
- Object and environment creation through a Create menu (camera, mesh, lights,
  ocean), all command-backed (Step 13).
- Persistent UI state in `editor_state.json` (panel visibility, outliner state,
  Content Browser folder/view/filters, Favorites/Collections, per-level camera,
  and per-level camera bookmarks).

Known limitations that the next wave of steps (12F and 21 through 23) addresses:

- The asset registry refreshes only on demand.
- Generator entities (`metalRoughGrid`, `instancedModels`) appear in the
  outliner but are effectively read-only.
- Disabled objects can still appear in RT reflections, which gather scene
  objects without the `IsVisible` check.

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

## Step 5A: Command History Window (Done)

Goal: expose the undo timeline and allow restoring any reachable command state.

Primary files:

- `sources/editor/commands/EditorCommand.*`
- `sources/editor/commands/EditorCommandStack.*`
- `sources/editor/ui/CommandHistoryPanel.*`
- `sources/editor/EditorController.*`

Implemented behavior:

- Store commands in one linear timeline with an applied-command cursor.
- Show Initial State plus one row for the state after each command.
- Selecting a row moves backward with Undo or forward with Redo until that state
  is restored.
- Keep forward entries visible after rewinding; executing a new command discards
  that forward branch.
- Show concise labels supplied by each command type.
- Register Command History as a normal editor window.

Acceptance criteria:

- Undo and redo buttons preserve their existing behavior.
- Selecting Initial State undoes every applied command.
- Selecting a later row reapplies commands up to that state.
- A new edit after rewind removes states that are no longer reachable.
- Loading or creating a level clears the history.

Validation:

- Build editor and no-editor x64 configurations.
- Exercise rename, transform, duplicate, delete, rewind, forward restore, and a
  new branch after rewind.

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

## Step 11A: Viewport Material Drop To Selected Object

Goal: make material drag/drop work the way the user expects: dropping a material
asset anywhere in the viewport assigns it to the currently selected object, if
that object supports material assignment.

Primary files:

- `sources/editor/ui/ViewportGizmo.*`
- `sources/editor/ui/EditorDragDrop.h`
- `sources/editor/commands/SetMaterialCommand.*`
- `sources/editor/EditorController.cpp`

Implementation notes:

- Reuse the existing `EditorDragDrop::kAssetPayloadType` and
  `EditorDragDrop::DecodeAssetPayload` path from Step 11.
- Extend the viewport drop target so `EditorAssetType::MaterialPreset` payloads
  are accepted, not rejected.
- Apply material drops to `ctx.selectedObject`; do not require a mesh under the
  cursor for this step.
- Only execute when the selected document object exists and has
  `type == "staticMesh"`.
- Use `SetMaterialCommand` so undo/redo restores the previous material.
- Keep existing mesh-asset viewport drops working exactly as they do now.
- Keep unsupported drops non-mutating with clear tooltip feedback:
  - no selected object;
  - selected object is not a static mesh;
  - dragged asset is not a material;
  - dragged asset no longer exists in the registry.
- Do not add drag-to-hovered-object behavior in this step. That is a separate
  feature because it needs reliable object hit-testing during drag preview.

Acceptance criteria:

- Select a static mesh, drag a material asset from the Content Browser, and drop
  it anywhere over the viewport: the selected mesh changes material.
- Undo/redo restores and reapplies the material assignment.
- Dropping a material over the viewport with no selected object does nothing and
  explains that a static mesh must be selected.
- Dropping a material while a non-static-mesh object or environment entity is
  selected does nothing and explains the unsupported target.
- Mesh asset drops into the viewport still spawn objects.
- Folder drops and other asset-type drops still do not mutate state.

Validation:

- Build `Debug|x64`.
- In `data/levels/demo.json`, select a static mesh and drag at least two
  material presets into the viewport.
- Verify undo/redo after each material drop.
- Try material drops with no selection and with a non-static-mesh selection.
- Drag a mesh asset into the viewport and confirm spawn still works.

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

## Step 12A: Resizable Sources And Asset View Split

Goal: remove the fixed, undersized Sources column and let the user choose how
much horizontal space the folder tree receives while the Asset View resizes to
fill the remainder.

Primary files:

- `sources/editor/ui/ContentBrowserPanel.*`

Implementation notes:

- Remove the bottom Details/Preview child introduced by Step 12. The Sources and
  Asset View children should use all remaining height below the navigation bar.
- Replace the fixed `220.0f` Sources width with panel-owned splitter state.
- Draw a narrow vertical splitter between Sources and Asset View. While dragging:
  - show the horizontal-resize cursor;
  - update the Sources width continuously;
  - resize the Asset View in the same frame;
  - clamp Sources to a practical minimum, initially `160.0f`;
  - preserve a practical Asset View minimum, initially `320.0f`;
  - recompute the upper clamp from current Content Browser width so resizing the
    outer window cannot push either child off-screen.
- Use stable child sizes and a stable splitter hit target so dragging does not
  shift when scrollbars appear.
- Keep Favorites, Collections, and Folders in the Sources child. Its vertical
  scrollbar must cover the full-height Sources area.
- Remove footer-only selection state that no longer has a consumer. Keep asset
  selection used by drag/drop, context actions, and visual highlighting.
- Do not persist the width in this step. Step 14 owns editor UI persistence.

Acceptance criteria:

- Dragging the splitter left and right visibly resizes Sources and Asset View.
- Sources cannot become too narrow to use and Asset View cannot collapse.
- Resizing the Content Browser clamps the saved in-memory split without overlap.
- Removing the bottom panel gives the Asset View the reclaimed vertical space.
- List and tile views, folder navigation, drag/drop, Favorites, and Collections
  continue to work.

Validation:

- Build `Debug|x64`.
- Drag the splitter at default, narrow, and wide Content Browser sizes.
- Exercise Sources and Asset View scrollbars at both splitter extremes.
- Switch between list and tile view after resizing.

## Step 12B: Resource Details In Hover Hints

Goal: preserve useful inspection data without reserving permanent layout space
for a Details/Preview panel.

Primary files:

- `sources/editor/ui/ContentBrowserPanel.*`
- `sources/editor/assets/AssetRegistry.*` only if additional cheap metadata is
  required

Implementation notes:

- Add one reusable hover-hint renderer for assets and one for folders.
- Invoke the same hint renderer from:
  - Asset View list rows;
  - Asset View tiles;
  - folder tree rows;
  - Favorite and Collection references.
- Use delayed hover behavior so hints do not flash while the user moves across
  a dense tile grid. Suppress hints during drag/drop.
- Asset hints should show the real thumbnail when that asset type supports one,
  otherwise the baked fallback icon, plus display name, virtual path, and
  concise type-specific metadata:
  - Texture: dimensions, format, kind, mip count, and invalid/unreadable state.
  - Material: preset name and definition file.
  - Mesh, Level, and Shader: source path and extension.
- Folder hints should show virtual path, child folder count, direct asset count,
  and recursive asset count.
- Keep hints read-only. Move `Add/Remove Favorite` and `Collections` controls
  from the removed footer into the existing resource-specific context menus.
  These two organization entries are valid for both asset and folder resources;
  do not reintroduce unrelated entries.
- A missing Favorite or Collection reference should show an unavailable hint
  and remain non-mutating until explicitly removed.

Acceptance criteria:

- Hovering any visible asset or folder shows its details after a short delay.
- Texture metadata failures remain obvious in the hint.
- Hints do not obscure drag/drop feedback or appear while dragging.
- Favorite and Collection membership can still be changed after the footer is
  removed.
- Context menus remain scoped to the selected resource type.

Validation:

- Build `Debug|x64`.
- Hover every asset type in list and tile view, plus folders in both Sources and
  Asset View.
- Verify hints for valid and invalid texture metadata.
- Add/remove Favorite and Collection references from asset and folder menus.
- Start a drag and confirm hover hints remain closed.

## Step 12C: Folder And Fallback Content Browser Icons

Goal: replace text-only folder and non-previewable-resource badges with
recognizable baked icons without using generic icons where a real asset preview
is possible.

Primary files:

- `art/editor/content_browser_icons/*.svg` (new editable vector sources)
- `textures/editor/content_browser_icons.png` (new baked runtime atlas)
- `sources/editor/ui/ContentBrowserPanel.*`
- `sources/editor/EditorController.*` if renderer/resource ownership belongs
  outside the panel
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

Implementation notes:

- Author simple SVG source icons for Folder, Level, Shader, Unknown, and Preview
  Failed. The Folder source must be a real folder silhouette, not text in a box.
- Do not author or display normal-state generic icons for Mesh, Material, or
  Texture assets. Step 12D supplies real thumbnails for those types.
- Convert the SVGs to transparent PNG cells at a fixed resolution, initially
  `64x64`, and pack them into one committed PNG atlas with a documented,
  deterministic cell order.
- Keep SVG sources and the baked PNG atlas in the repository. Runtime code must
  load the PNG atlas only; it must not parse or rasterize SVG.
- Use a deterministic conversion command or small tooling script and document
  it next to the SVG sources so another executor can reproduce the atlas.
- Load the atlas once using the existing `Texture2D`, `UploadBatch`, and ImGui
  texture-ID path. Give resource lifetime to an editor-owned object and release
  it through the normal renderer-safe shutdown path.
- Tile view:
  - reserve a stable icon region at the top of every tile;
  - draw the icon without stretching its aspect ratio;
  - place the asset/folder name below it with clipping or wrapping that cannot
    resize the tile.
- List view:
  - draw a smaller icon before the resource name;
  - keep row height stable across resource types.
- Use the same atlas icons in Step 12B hover hints.
- Use the Preview Failed icon only after real-thumbnail generation reports a
  failure. Use a neutral loading placeholder while work is queued.
- Keep a text badge fallback when atlas loading fails.

Acceptance criteria:

- Every folder in Sources and Asset View uses the baked folder icon.
- Level and Shader entries use distinct baked icons.
- Mesh, Material, and Texture entries do not show generic type icons in their
  normal ready state.
- Folder/fallback icons render in list view, tile view, Favorites, Collections,
  and hover hints.
- Tile dimensions remain stable and labels do not overlap icons.
- Missing atlas data falls back to readable text without breaking the browser.

Validation:

- Rebuild the PNG atlas from the committed SVG sources and confirm the output is
  deterministic.
- Build `Debug|x64`.
- Inspect list and tile views at narrow/default/wide Content Browser sizes.
- Confirm the icon atlas loads once and does not leak ImGui descriptors across
  frames.
- Temporarily make the atlas unavailable and verify the text fallback.

## Step 12D: Real Asset Thumbnail Pipeline

Goal: show the actual resource contents in Asset View for resources that can be
meaningfully previewed, instead of representing them with generic type icons.

Status (2026-07-08): the "safe core" of this step is implemented and builds clean
(editor and no-editor). What landed:

- `AssetThumbnailCache` (`sources/editor/assets/AssetThumbnailCache.*`): the full
  cache framework with `Missing/Queued/Generating/Ready/Failed` states, keying by
  asset id plus source write time, bounded per-frame generation (one fenced upload
  batch), LRU eviction, and renderer-safe GPU/descriptor lifetime.
- Real **texture** thumbnails wired into tile view, list rows, and hover hints
  (aspect-preserved, checkerboard behind alpha, loading and failed placeholders).
- Engine support (all `WITH_EDITOR`-gated): a per-resource ImGui preview-descriptor
  release, and the ImGui preview SRV heap raised from 64 to 512 for editor builds
  because each preview costs `kFrameCount` descriptors and a texture folder would
  otherwise exhaust the heap (which throws).

Deferred to Step 12E (they need an offscreen 3D render pass and/or an on-disk
cache, which are higher risk): mesh thumbnails, material thumbnails, cubemap face
previews, and the on-disk thumbnail cache for cross-restart reuse. The mesh and
material acceptance/validation items below are tracked under Step 12E. The rest of
this section is retained as the design record for the previewable-resource work.

Primary files:

- `sources/editor/assets/AssetThumbnailCache.*` (new)
- `sources/editor/assets/AssetRegistry.*`
- `sources/editor/ui/ContentBrowserPanel.*`
- `sources/editor/EditorController.*`
- renderer/material/mesh helpers only where required for isolated thumbnail
  rendering
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

Implementation notes:

- Add an editor-owned thumbnail cache with explicit `Missing`, `Queued`,
  `Generating`, `Ready`, and `Failed` states.
- Key each thumbnail by asset ID, source write time, thumbnail schema version,
  and any directly tracked dependencies needed for correctness.
- Generate or refresh thumbnails lazily for assets visible in Asset View. Bound
  work per frame so opening a large folder does not stall the editor.
- Cache generated thumbnail PNGs under an editor cache directory that is not
  source-controlled. Loading the same unchanged folder on a later run should
  reuse those files.
- Texture thumbnails must display the real texture contents:
  - preserve aspect ratio;
  - use a checkerboard behind alpha;
  - apply the correct linear/sRGB presentation path;
  - use a representative face or documented projection for cubemaps;
  - report unreadable/unsupported textures as `Failed`.
- Mesh thumbnails must render the real mesh:
  - use an isolated offscreen preview scene or render pass;
  - frame the mesh from its bounds with a deterministic three-quarter camera;
  - use neutral lighting and a neutral background;
  - use the mesh's available/default material when safe, otherwise a neutral
    preview material;
  - never add the preview mesh to the edited level or mutate selection.
- Material thumbnails must render the real preset on a standard preview object,
  preferably a sphere plus a small planar section for surface readability.
  Track `data/materials.json` and referenced texture write times for invalidation.
- Level and Shader assets remain on their baked Step 12C icons until dedicated
  preview renderers are intentionally designed.
- Tile view should use the largest available real thumbnail inside its stable
  preview region. List view may use a smaller version of the same thumbnail.
- Step 12B hover hints should show a larger version of the same real thumbnail;
  do not create a second preview path.
- Create ImGui texture IDs once per cached GPU thumbnail and release them through
  the renderer-safe editor shutdown/eviction path. Do not allocate descriptors
  every frame.
- On `Queued` or `Generating`, draw a quiet loading placeholder. On `Failed`,
  draw the Step 12C Preview Failed icon and include the failure reason in the
  hover hint.

Acceptance criteria:

- Texture tiles show the actual texture, not a `[TEX]` badge or texture icon.
- Mesh tiles show a framed render of the actual geometry, not a `[MESH]` badge
  or mesh icon.
- Material tiles show the actual preset on the standard preview object when the
  material pipeline supports it.
- Scrolling through a large folder remains responsive while thumbnails fill in.
- Revisiting unchanged assets reuses cached thumbnails.
- Modifying an asset invalidates and regenerates only affected thumbnails.
- Thumbnail generation never changes the current level, selection, command
  history, or dirty state.

Validation:

- Build `Debug|x64`.
- Inspect several textures with different aspect ratios, alpha, formats, and a
  cubemap.
- Inspect small, large, and unusually oriented meshes and confirm deterministic
  framing.
- Inspect multiple material presets and verify visible differences.
- Restart the editor and confirm unchanged thumbnails come from cache.
- Modify one source asset, refresh, and confirm only its thumbnail regenerates.
- Navigate a folder with many previewable assets while watching frame time and
  ImGui descriptor usage.

## Step 12E: Mesh And Material Thumbnail Previews

Goal: complete the previewable-resource work started in Step 12D by rendering real
mesh and material thumbnails (and a representative cubemap face), and persist all
thumbnails to an on-disk cache so unchanged assets survive an editor restart.

Status (2026-07-08): mesh and material previews are implemented and build clean
(editor and no-editor). What landed:

- `EditorPreviewRenderer` (`sources/editor/assets/EditorPreviewRenderer.*`,
  `shaders/editor_preview.hlsl`): a self-contained offscreen forward pass (own
  PSO/root signature/depth target/constant buffer, private MeshManager and
  MaterialDataManager) that renders a mesh from a deterministic three-quarter
  camera with neutral lighting. Meshes use a neutral material; materials render
  their preset albedo on a shared unit sphere.
- `AssetThumbnailCache` extended to generate mesh and material thumbnails through
  that renderer (bounded per frame, fenced, one draw per command list), reusing
  the same states/keying/eviction/GPU-lifetime paths as texture thumbnails. The
  browser's existing `Ready` image path renders them in tiles, list, and hover.

Still deferred (documented below): cubemap face previews and the on-disk
thumbnail cache for cross-restart reuse. This is a renderer path, so a clean
build is necessary but not sufficient; it needs a GUI check (correct framing,
non-empty thumbnails, no descriptor/barrier issues).

Primary files:

- `sources/editor/assets/AssetThumbnailCache.*` (extend the existing cache; do not
  fork it)
- `sources/editor/ui/ContentBrowserPanel.*` (no new preview path; the same
  `ResolveAssetThumbnail`/`BrowserThumbnail` sites already handle any Ready image)
- `sources/editor/EditorController.*`
- renderer/material/mesh helpers only where required for an isolated preview render
- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

Implementation notes:

- Build on the Step 12D `AssetThumbnailCache`. It already owns the state machine,
  write-time keying, bounded per-frame budget, LRU eviction, and GPU/descriptor
  lifetime. This step adds new generators that produce a GPU thumbnail for mesh,
  material, and cubemap records; the browser needs no new draw code because it
  already renders any `Ready` thumbnail image.
- All generation must run inside the editor draw/tick window, fenced with
  `Renderer::WaitForPreviousFrame` like the current texture path, and must never
  change the edited level, selection, command history, or dirty state. Respect the
  Stop Condition on GPU/resource lifetime outside the editor draw/tick window: the
  offscreen preview scene, targets, and pipeline are private to the cache.
- Bound offscreen renders per frame more tightly than file loads; they are heavier.
  A large folder should fill in over several frames without a visible stall.
- Mesh thumbnails must render the real mesh:
  - use an isolated offscreen preview scene or render pass with its own color +
    depth targets;
  - frame the mesh from its bounds with a deterministic three-quarter camera;
  - use neutral lighting and a neutral background;
  - use the mesh's available/default material when safe, otherwise a neutral
    preview material;
  - never add the preview mesh to the edited level or mutate selection.
- Material thumbnails must render the real preset on a standard preview object,
  preferably a sphere plus a small planar section for surface readability. Key the
  thumbnail on `data/materials.json` and referenced texture write times so editing
  a material invalidates only its own thumbnail.
- Cubemap textures (skipped in Step 12D and left on their badge) get a
  representative face via a documented sampling pass into a 2D target.
- Add an on-disk thumbnail cache under an editor cache directory that is not
  source-controlled. Key each cached PNG by asset id, source write time, the
  thumbnail schema version (already reserved as `kThumbnailSchemaVersion`), and any
  tracked dependencies. Loading the same unchanged folder on a later run must reuse
  those files instead of re-rendering.
- Tile view uses the largest available real thumbnail in its stable preview region;
  list view uses a smaller version; Step 12B hover hints use a larger version of the
  same thumbnail. Do not create a second preview path.
- Create ImGui texture ids once per cached GPU thumbnail and release them through
  the renderer-safe eviction/shutdown path already in the cache. On `Queued`/
  `Generating`, draw the quiet loading placeholder; on `Failed`, draw the Step 12C
  Preview Failed icon and include the failure reason in the hover hint.

Acceptance criteria:

- Mesh tiles show a framed render of the actual geometry, not a `[MESH]` badge.
- Material tiles show the actual preset on the standard preview object when the
  material pipeline supports it.
- Cubemap tiles show a representative face instead of the texture badge.
- Scrolling through a large folder remains responsive while thumbnails fill in.
- Restarting the editor reuses unchanged thumbnails from the on-disk cache.
- Modifying an asset invalidates and regenerates only affected thumbnails.
- Thumbnail generation never changes the current level, selection, command
  history, or dirty state.

Validation:

- Build `Debug|x64` and verify the no-editor build.
- Inspect small, large, and unusually oriented meshes and confirm deterministic
  framing.
- Inspect multiple material presets and verify visible differences.
- Restart the editor and confirm unchanged thumbnails come from the disk cache.
- Modify one source asset, refresh, and confirm only its thumbnail regenerates.
- Navigate a folder with many previewable assets while watching frame time and
  ImGui descriptor usage.

## Step 12F: Thumbnail Disk Cache And Cubemap Previews

Goal: finish the two items Step 12E deferred — reuse thumbnails across editor
restarts through an on-disk cache, and preview cube textures with a
representative face instead of a badge.

Primary files:

- `sources/editor/assets/AssetThumbnailCache.*`
- `sources/editor/assets/EditorPreviewRenderer.*`
- `shaders/editor_preview.hlsl` or a small companion shader for cube sampling
- `.gitignore`

Implementation notes:

- Disk cache: after a mesh/material/cubemap thumbnail is rendered (and optionally
  for decoded textures), read the 256x256 color target back to the CPU and encode
  it as PNG. Use WIC for encoding — `Texture2D` already decodes through WIC, so
  no new dependency. Follow the fenced readback pattern; never stall per frame
  (readback inside the existing bounded `ProcessPending` batch is fine).
- Cache files live under an editor cache directory (for example
  `editor_cache/thumbnails/`), added to `.gitignore`. Key each file by a hash of
  asset id, source write time, `kThumbnailSchemaVersion` (already reserved in
  `AssetThumbnailCache.cpp`), and tracked dependencies (materials also key on
  `data/materials.json` and referenced texture write times).
- On a cache-entry miss in `Request`, probe the disk cache before queueing GPU
  generation; a disk hit uploads the PNG like any texture thumbnail.
- Cubemap previews: sample one documented face (+X is fine) of the cube texture
  into the standard preview color target with a small pixel shader pass in
  `EditorPreviewRenderer`. Unreadable cubemaps report `Failed`. Remove the
  cube-texture exclusion in `ContentBrowserPanel`'s `ResolveAssetThumbnail` once
  this works.
- Stale cache files (key mismatch) are deleted lazily when encountered.

Acceptance criteria:

- Restarting the editor shows mesh/material thumbnails without re-rendering them
  (disk hits).
- Modifying a source asset regenerates only that thumbnail and replaces its
  cache file.
- Cube textures show a real face in tiles, list, and hover hints.
- The cache directory is never committed and can be deleted safely at any time.

Validation:

- Build `Debug|x64` and verify the no-editor build.
- Browse Models/Materials, restart, and confirm thumbnails appear without the
  generation delay.
- Delete the cache directory while the editor is closed and confirm clean
  regeneration.
- Touch one texture file and confirm only its thumbnail regenerates.

## Step 13: Command-Backed Environment Edits (Done)

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
  filter, selected source folder, recursive mode, view mode, source/asset split
  width, and selection outline radius.
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

## Step 16: Gizmo Snapping And Transform Spaces

Goal: give the transform gizmo the two features every editor user reaches for
first — increment snapping and a local/world space toggle.

Primary files:

- `sources/editor/ui/ViewportGizmo.*`
- `sources/editor/EditorHotkeys.*`
- `sources/editor/EditorController.cpp` (persistence)

Implementation notes:

- `ImGuizmo::Manipulate` already accepts a snap vector; pass one per operation
  when snapping is active: translation grid (default 0.5), rotation angle
  (default 15 degrees), scale increment (default 0.1).
- Add toolbar controls next to the existing mode buttons: a snap on/off toggle
  and compact per-operation increment fields.
- Holding Ctrl during a drag temporarily inverts the snap state (UE behavior).
  Route the modifier through `ViewportGizmo` so it cannot fight the
  Ctrl-based hotkeys; snapping only matters while a drag is active.
- Add a LOCAL/WORLD toggle mapped to ImGuizmo's mode parameter. Environment
  light gizmos build synthetic matrices and stay WORLD regardless of the
  toggle.
- Persist snap enabled, the three increments, and the space toggle in
  `editor_state.json` under `levelEditor`, reusing the Step 14 helpers.

Acceptance criteria:

- Dragging with snap on moves the object in exact increments; rotation snaps to
  the configured angle.
- Ctrl inverts snapping only for the duration of the hold.
- Local mode orients the gizmo with the object's rotation; world mode matches
  today's behavior.
- Settings survive an editor restart.

Validation:

- Build `Debug|x64`.
- Manually verify snap values in the Inspector transform fields after snapped
  drags, in both spaces, on a mesh and on a spot light.

## Step 17: Cursor-Aware Placement And Drop To Ground

Goal: make object placement land where the user points instead of always five
units in front of the camera.

Primary files:

- `sources/editor/ui/ViewportGizmo.cpp` (viewport drop target)
- `sources/editor/EditorExtensionRegistry.*` (factory position hint)
- `sources/editor/EditorHotkeys.*`
- `sources/editor/EditorController.cpp`

Implementation notes:

- On a viewport mesh drop, cast the cursor ray with the existing
  `Scene::RaycastEditorObject` path and spawn at the hit point; keep the current
  camera-forward-times-five position as the no-hit fallback and for the Create
  menu.
- Extend `IEditorObjectFactory::BuildDefaultJson` (or add an overload) with an
  optional world-position hint so the drop site controls placement without
  duplicating factory logic.
- Add an End-key "drop to ground" action: ray straight down from the selected
  object's bounds bottom; on a hit, move the object with a
  `TransformObjectCommand` so it is undoable; on no hit, do nothing and say so
  in the status line.
- Note the ray only hits editor-linked, visible objects; document that
  generators and hidden objects are not landing surfaces.

Acceptance criteria:

- Dropping a mesh onto existing geometry spawns it at the cursor hit point.
- Dropping onto empty sky uses the old fallback position.
- End rests the selected object on the surface below it, undoably.

Validation:

- Build `Debug|x64`.
- Drop meshes onto the demo floor and onto the sky; verify both paths.
- Drop-to-ground an object raised above the floor, then undo.

## Step 18: Object Copy And Paste

Goal: complete the edit loop with clipboard operations that work across levels.

Primary files:

- `sources/editor/EditorHotkeys.*`
- `sources/editor/EditorController.*`
- `sources/editor/commands/` (paste command, mirroring `DuplicateObjectCommand`)

Implementation notes:

- Ctrl+C serializes the selected document object with the existing
  `EditorSceneDocument::ObjectToJson` into an editor-owned clipboard string;
  also mirror it to the OS clipboard via `ImGui::SetClipboardText` so a paste
  can cross editor instances.
- Ctrl+V parses the clipboard JSON, validates it (must be an object with a
  known `type`), allocates a fresh id, offsets the position slightly, and
  executes an undoable command that follows the `DuplicateObjectCommand`
  document+runtime spawn path.
- Environment entities follow the duplicate rules: point and spot lights are
  copyable; singletons (camera, skybox, directional light, ocean) refuse with a
  status message.
- Suppress both hotkeys while ImGui wants text input, matching the existing
  hotkey gating.
- Paste into a different level works because object `properties` are
  self-contained JSON.

Acceptance criteria:

- Ctrl+C then Ctrl+V produces a working copy, selected, undoable as one entry.
- Copy in one level, open another, paste there: the object appears and saves
  correctly.
- Malformed clipboard content is rejected without side effects.

Validation:

- Build `Debug|x64`.
- Copy/paste a mesh and a point light; undo each paste.
- Cross-level paste via File > Open between two levels.

## Step 19: Multi-Selection Foundation

Status (2026-07-13): implemented. The ordered selection, composite command
history entries, batch clipboard format, multi-object gizmo drag, combined F
frame, and 64-object editor outline set are in place. `Debug|x64` and the
no-editor `Release|x64` build both pass.

Goal: turn the single-object selection model into an ordered multi-selection so
bulk workflows (move, delete, duplicate, enable) stop being one-at-a-time.

Primary files:

- `sources/editor/EditorController.*` and `sources/editor/EditorContext.h`
- `sources/editor/ui/SceneOutlinerPanel.*`
- `sources/editor/ui/ViewportGizmo.*`
- `sources/editor/ui/InspectorPanel.*`
- `sources/editor/commands/` (composite command)
- `sources/app/scene/Scene.*` (selection outline id set, editor-gated)

Implementation notes:

- Replace the single selected id with an ordered selection (vector of
  `EditorObjectId` plus a primary). Keep a primary-selection accessor with the
  old semantics so panels can migrate incrementally.
- Outliner: plain click replaces the selection, Ctrl+click toggles membership,
  Shift+click range-selects over the currently displayed row order.
- Viewport: Ctrl+click adds/removes the picked object; plain click replaces.
- Add a `CompositeCommand` that executes child commands in order and undoes in
  reverse; Delete, Duplicate, Enable, and Copy/Paste over a selection become one
  composite entry in the history.
- Gizmo: manipulate the primary object and apply the same delta to the rest;
  one composite transform command per drag.
- Inspector: with multiple objects, show a "N objects selected" header, the
  shared Enabled checkbox, and the primary object's details; do not attempt
  full mixed-value editing in this step.
- Selection outline: `Scene::SetSelectedEditorObjectId` is singular. Extend the
  editor-gated API to accept a small bounded set (for example up to 64 ids) and
  compare in the outline pass. This is the step's only renderer-adjacent edit;
  keep it behind `WITH_EDITOR` and verify the no-editor build.
- F frames the combined bounds of the whole selection.

Acceptance criteria:

- Ctrl/Shift selection works in the outliner; Ctrl+click works in the viewport.
- Deleting five objects is a single undo entry that restores all five.
- A gizmo drag moves every selected object rigidly and undoes as one entry.
- All selected objects show the outline; single-selection behavior is unchanged.

Validation:

- Build `Debug|x64` and verify the no-editor build.
- Multi-select mixes of meshes and lights; exercise delete, duplicate, enable,
  drag, and undo after each.

## Step 20: Camera Bookmarks And Scene Framing

Status (2026-07-13): implemented. Ctrl+1 through Ctrl+9 store per-level camera
bookmarks, 1 through 9 recall them, and Home frames visible editor objects.
`Debug|x64` builds and launches successfully.

Goal: fast navigation around a level during editing.

Primary files:

- `sources/editor/EditorHotkeys.*`
- `sources/editor/EditorController.*` (per-level persistence already exists)

Implementation notes:

- Ctrl+1..9 stores the current camera pose in bookmark slots; 1..9 recalls.
  Suppress while ImGui wants text input and while the fly-camera RMB is held,
  matching existing hotkey gating.
- Persist bookmarks inside the existing per-level camera record in
  `editor_state.json` so each level keeps its own set.
- Add "Frame Scene" (Home): frame the combined bounds of all visible editor
  objects, reusing the F-focus math.
- List the new keys in the hotkey hint text.

Acceptance criteria:

- Bookmarks recall exact camera poses and survive restarts, per level.
- Home frames the whole scene.
- Number keys never fire while typing in a text field.

Validation:

- Build `Debug|x64`.
- Store/recall bookmarks in two different levels and restart between them.

## Step 21: Asset Registry Auto-Refresh

Goal: keep the Content Browser in sync with the filesystem without manual
Refresh clicks.

Primary files:

- `sources/editor/assets/AssetRegistry.*`
- `sources/editor/EditorController.cpp`
- `sources/editor/ui/ContentBrowserPanel.cpp` (status text only)

Implementation notes:

- Start with throttled polling: at most every ~2 seconds of editor-open time,
  compare a cheap change signature of the asset roots (directory and file write
  times from the existing scan machinery) and run `Refresh` when it differs.
  The current registry scan is metadata-only over a small tree; measure its
  cost once before considering a `ReadDirectoryChangesW` watcher, and prefer
  the poll if it stays trivially cheap.
- Never rescan mid-frame more than once, and never from a worker thread — the
  registry has no locking.
- Selected folder, history, and selection survive via the existing
  `EnsureSelectedFolder` handling; thumbnails invalidate for free through the
  `fileWriteTime` keying.
- Show a subtle "auto-refreshed" note in the browser status line; keep the
  manual Refresh button.

Acceptance criteria:

- Adding, deleting, or overwriting an asset file on disk shows up in the
  browser within a few seconds, without pressing Refresh.
- The selected folder and view state are preserved across auto-refreshes.
- Frame time shows no visible periodic hitch from the poll.

Validation:

- Build `Debug|x64`.
- Copy a texture into `textures/`, watch it appear; delete it, watch it leave.
- Watch the profiler HUD while the poll runs.


## Step 22: Visibility Consistency Hardening

Goal: make the Enabled toggle authoritative in every render path, not just the
bucketized ones.

Primary files:

- The RT reflections instance gather (TLAS build site in the renderer)
- Any other pass that iterates scene objects without `SceneRenderQueue`

Implementation notes:

- `SceneRenderQueue::Bucketize` already skips `!IsVisible()` objects, so main,
  shadow, and glass passes honor the toggle. RT reflections gather `objects_`
  directly and still include hidden objects; add the `IsVisible` check to the
  RT instance gather.
- Audit the remaining direct consumers (VSM caster gather, instanced-model
  paths) and align any that bypass the check.
- This is ungated engine code, but behavior-invariant outside the editor:
  `IsVisible` defaults to true and only editor code calls `SetVisible`. State
  that invariant in a comment at the check site.
- Confirm the TLAS rebuild path handles instance-count changes between frames
  (it rebuilds per frame; verify rather than assume).

Acceptance criteria:

- Disabling an object removes it from RT reflections (F5 RT mode) the same
  frame it disappears from the main view.
- Non-editor Release rendering is unchanged.
- No new GBV or debug-layer messages.

Validation:

- Build `Debug|x64` and the no-editor Release build.
- Toggle Enabled on a mirror-visible object with RT reflections active.
- Run `--scene-stress` once as a regression gate.

## Suggested Execution Order

Steps 1 through 20 (including 5A, 11A, 12A-12E) are complete. Recommended order
for the next wave:

1. Step 12F: Thumbnail Disk Cache And Cubemap Previews — closes out the
   Content Browser thumbnail family.
2. Step 21: Asset Registry Auto-Refresh.
3. Step 22: Generator Entity Editing — needs a new gated Scene helper and
   respawn churn care; keep it late and stress-test it.
4. Step 23: Visibility Consistency Hardening — small but ungated engine edit;
   wants a runtime/GBV pass, so schedule it when one is planned anyway.

Ordering rationale: finish the thumbnail work first, then add camera workflow
and asset-refresh quality-of-life. The generator and ungated renderer work remain
last because they need broader regression coverage.

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
