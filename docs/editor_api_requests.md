# Editor API requests

Things the Command Bar was asked for that the editor cannot do. Each entry is
written when the local model answers `needs_api`: it records the phrase, the
actions the model weighed and rejected, and what it thought implementing this
would take.

**These are suggestions, not specifications.** A model wrote them from one
designer's sentence; it has no view of the codebase beyond the action registry it
was handed. Read an entry as evidence that somebody wanted something -- the
valuable part -- and judge the proposed design yourself. Repeats matter more than
any single entry: the same request three times is a feature, once is a mood.

Appended by `sources/editor/intent/IntentNotes.cpp`. Safe to prune.

---

## respond_to_chat

- **asked**: как дела твои
- **read as**: chat about how things are going
- **rejected existing because**: all available actions are for editing the level (spawn, delete, move, etc.) and none support conversational responses
- **level**: data/levels/wind_test.json
- **when**: 2026-09-15 04:39

The designer asked "how are you," seeking a conversational response rather than level editing. No existing action supports chat; all are strictly for manipulating objects or environment settings. The smallest viable addition would be a `respond_to_chat` action, requiring the editor to interface with an LLM or pre-defined response system, which current actions lack. This is harder than it looks because it introduces non-deterministic state changes and requires integrating external AI services or maintaining a large database of context-aware replies, complicating the editor’s deterministic nature. Alternatively, if this is a joke, maybe they just want to know if the level loads? But assuming literal intent, we need a new input handler for natural language queries that don't map to spatial transformations.

---

## respond_to_chat(message)

- **asked**: как дела твои
- **read as**: chat about how things are going
- **rejected existing because**: all available actions modify level objects or environment settings; none handle conversational responses
- **level**: data/levels/wind_test.json
- **when**: 2026-09-15 04:39

The designer asked "how are you," a social greeting, not a level editing command. No existing action handles conversational chat; all current tools modify objects, environment settings, or camera views. The smallest covering action would be a new `respond_to_chat(message)` function. This requires the editor to parse natural language intent distinct from spatial manipulation and route non-command inputs to a dialogue handler rather than a geometry engine. This is harder than it looks because the editor currently lacks a semantic layer to distinguish between a user asking a question about the scene (which might use `count` or `frame`) and a user engaging in idle chatter. Adding chat support requires defining boundaries for when the editor should respond socially versus when it should interpret the input as a potential, albeit malformed, command.

---

## none

- **asked**: че почем?
- **read as**: translate 'че почем?' into an editor command
- **rejected existing because**: 'че почем?' is a colloquial Russian phrase meaning 'what's the price?' or 'what's going on?'. It is not a level design instruction and does not map to any of the available editor actions (spawn, delete, move, etc.).
- **level**: data/levels/wind_test.json
- **when**: 2026-09-15 04:58

The designer likely intended to query the cost or properties of an asset, perhaps checking prices or stats. No existing action fits this. The smallest covering action would be a new `query` command, accepting an asset name and returning specific metadata like price or stats. This requires the editor to expose asset database fields to the command interface, which current actions like `count` or `frame` do not support. It is harder than it looks because the editor currently treats commands as spatial manipulations, not data lookups. Integrating a read-only data retrieval mechanism requires exposing internal asset structures without creating security holes or performance bottlenecks, as these queries might need to fetch heavy metadata.

---

## renameSelection('spheres')

- **asked**: переименуй шарики в spheres
- **read as**: rename objects to 'spheres'
- **rejected existing because**: The editor has no rename or setMetadata action; only structural or visual transforms like move, scale, or delete are available.
- **level**: data/levels/wind_test.json
- **when**: 2026-09-15 04:59

The designer wants to rename selected objects to "spheres." No existing action supports metadata editing; only structural or visual transforms (move, scale, delete, etc.) are available. The smallest covering action would be `renameSelection('spheres')`, requiring a new editor capability to update object names without altering position, rotation, or scale. This is harder than it looks because the editor currently lacks a generic metadata update mechanism. While `replace` swaps meshes, it doesn’t change names. `select` or `count` don’t modify anything. Implementing `renameSelection` would require adding a new action type to the editor’s API, distinct from the current structural/visual-only actions. Without this, the request cannot be fulfilled.

---

## rename(objects, 'spheres')

- **asked**: переименуй все шарики в spheres
- **read as**: rename objects to 'spheres'
- **rejected existing because**: The editor has no rename or setLabel action; only select, delete, move, rotate, scale, etc., which do not change object names.
- **level**: data/levels/wind_test.json
- **when**: 2026-09-15 05:03

The designer wanted to rename all 'ball' objects to 'spheres' for clarity. No existing action supports renaming. The minimal solution is a new `rename` action taking a target selector and a new name string. This requires the editor to modify object metadata (names/labels) rather than just transform or delete them. This is harder than it looks because object names are often unique identifiers or linked to scripts; blindly renaming could break references. Additionally, the editor must handle naming collisions if multiple objects share the target name. Without this feature, designers must manually edit JSON or use workarounds, which is inefficient and error-prone. Please consider adding a safe rename capability that validates against existing names and script dependencies before committing changes.

---

## rename(selection, 'spheres')

- **asked**: переименуй шарики в spheres
- **read as**: rename objects to 'spheres'
- **rejected existing because**: The editor has no rename or metadata-editing action; it can only transform geometry, visibility, or environment settings.
- **level**: data/levels/wind_test.json
- **when**: 2026-09-15 05:11

The designer wants to rename selected objects to 'spheres'. No existing action supports metadata editing or renaming. The minimal feature is a `rename` action accepting a string parameter, which would update object labels without altering geometry or transforms. This requires the editor to expose and persist name metadata, distinct from asset references. The difficulty lies in distinguishing between an object’s internal ID, its display name, and its asset type. If names are purely cosmetic, this is trivial. If they affect logic or asset loading, it becomes complex. Currently, the editor only handles spatial and visual properties. Adding a simple rename capability would bridge this gap, allowing designers to organize levels more intuitively without needing external tools or manual JSON edits.

---

## spawn mixed-type palms with even distribution

- **asked**: посади 20 новых пальм разного типа равномерно по острову
- **read as**: plant 20 new palms of mixed types evenly across the island
- **rejected existing because**: The spawn action scatters objects randomly around a point and does not support specifying multiple asset types in one call, nor does it guarantee even spatial distribution across a large area like an island. There is no single command to spawn multiple different assets evenly distributed over a specific terrain area.
- **level**: data/levels/wind_test.json
- **when**: 2026-09-15 05:14

The designer wants to populate an island with twenty distinct palm models, evenly spaced. No existing action achieves this. A new `spawnEven` action is needed, accepting a list of asset IDs and a target area, then calculating a grid or Poisson-disc distribution to place instances. This requires spatial math absent from the current `spawn` command, which only scatters randomly around a single point. The complexity lies in handling mixed asset types while maintaining uniform density across irregular terrain. The editor would need to detect valid ground points for each type and ensure no overlap, rather than simple random placement. This is a significant feature gap for level setup efficiency.

---

## setMaterial with a red material or setBaseColor

- **asked**: покрась пальмы в ярко-красный цвет
- **read as**: paint palms bright red
- **rejected existing because**: setMaterial requires an existing material asset name, and none of the available materials are described as 'bright red'; replace swaps the entire mesh which is not what was asked; no action exists to change only the color of an existing material.
- **level**: C:/Users/darkc/AppData/Local/Temp/claude/D--Programming-test-cube/7e0da404-de59-4512-aa43-87c611d9772a/scratchpad/atoll_ring.json
- **when**: 2026-09-16 13:50

The designer wants to change the visual color of palm trees to bright red. The closest existing action is `setMaterial`, but it requires a pre-existing material asset name. Since no 'bright red' material exists in the asset library, this action fails. To support this request, we need a new action, perhaps `setColor`, that accepts a hex code or RGB value to dynamically tint the base color of the selected objects' materials without requiring a new asset file. This is harder than it looks because it requires runtime material modification logic, which may not be supported by the current rendering pipeline or material system architecture. Implementing this would allow designers to quickly iterate on color themes without needing an artist to create new assets for every shade.

---

## setMaterial with a red material or setBaseColor

- **asked**: покрась пальмы в ярко-красный цвет
- **read as**: paint palms bright red
- **rejected existing because**: setMaterial requires an existing material asset name, and none of the available materials are described as 'bright red'; replace swaps the entire mesh which is not what was asked; no action exists to change only the color of an existing material.
- **level**: C:/Users/darkc/AppData/Local/Temp/claude/D--Programming-test-cube/7e0da404-de59-4512-aa43-87c611d9772a/scratchpad/atoll_ring.json
- **when**: 2026-09-16 13:53

The designer wants to change the color of existing palm meshes to bright red without replacing the geometry. The closest existing action, `setMaterial`, requires a pre-existing material asset name. Since no "bright red" material exists in the current asset library, this action fails. A new action, `setMaterialColor`, would be needed, accepting a material reference and a hex color code to override the base color dynamically. This is harder than it looks because it requires the editor to support runtime material instance creation or modification, rather than just swapping static asset references. Without this capability, the only workaround is manually creating a red material asset first, which breaks the workflow.
