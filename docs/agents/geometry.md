# Geometry -- Import, LOD, Tessy

Moved verbatim from `AGENTS.md` on 2026-11-24, when that file had grown to 513 KB; `AGENTS.md` now holds the rules in
short form and an index of these notes. This note keeps the full text: design, measurements, history and the reasons
behind each rule. Section titles are the old ones, so an old reference «AGENTS.md "<title>"» lands here.

## Rest Pose — How a New Copy Lies

mesh.json `"restRotationDeg": [pitch, yaw, roll]` (the level's rotationDeg order) is how a NEW copy of
the asset lies; absent = as authored, upright. Every creation path applies it through one call,
`restpose::ApplyToNewObject` (`editor/assets/MeshRestPose.*`): the object factory (Content Browser,
viewport drop and its drag ghost), `spawn` (the pose turned by its random yaw) and `place` without a
`rotationDeg` (turned by `yawDeg`). The pivot is raised or lowered so the posed geometry's lowest point
sits at the ground height -- the lift is measured from the .mesh.bin at spawn, never stored, so a rebake
cannot leave it stale. Placed copies keep their own rotation. Mesh Editor > Rest pose > "Lay flat
(auto)" writes the automatic pose (thinnest surface axis up, a cupped shape rim-down);
`intent_regression --rest-pose <mesh.json>...` prints the same numbers headless. The seashells carry
one: imported, they stood on their hinges 37-56 degrees off flat.

mesh.json `"pivot": "base"` moves the model origin to the mesh's footing AT BAKE TIME (lowest point,
centred on the bottom 5 % of its height; `MeshLoadOptions::pivotToBase`, logged as `[meshbake] pivot to
base: origin moved by (...)`). A node split out of a multi-object glTF otherwise keeps that file's
layout offset -- fern_02 baked with the pivot 1.0-1.4 m beside the plant, so wind bent it about a
point beside it. New split imports get it by default; an existing asset keeps what its manifest says,
because switching it on moves the geometry under every placed copy: re-place them by
`T' = T + R (s * offset)` with the logged offset. Every node-split asset carries it since 2026-09-24
(fern_02, lowpoly_sticks, rocks_node, seashells, beach_grass), and wind_test's copies were re-placed
that way; for a rock or a stick the footing is its contact patch, up to 0.3 m off its box centre.

## Import — Where a Mesh Asset Lives, and Leaf Card or Surface

A mesh asset is `models/<name>/<name>.mesh.json`, beside its `.mesh.bin` and textures; a split import
writes `models/<name>/<name>_node_<part>.mesh.json`. Until 2026-10-23 they lay flat in `models/` (a
hundred-node pack spilled a hundred files between every other asset); all 148 were moved and every
level re-pointed then, and nothing resolves the old flat path -- a stale one reports `mesh asset not
found`. A mesh.json inside an import folder stands for the WHOLE import (registry status, Content
Browser Reimport), like a material preset: it is not a copied output, and the per-resource reimport
would have read it as an orphan and deleted it.

Each glTF material is read as a LEAF CARD (twoSidedFoliage shading, its slot marked wind foliage, an
auto windStrength) or a SURFACE (defaultLit, rigid). Leaf = a two-sided alpha cutout whose cutout
removes at least 20 % of its surface (`materialgen::ClassifyLeafCards`: the albedo alpha sampled at
area-weighted points over the material's triangles). The flags alone said leaf for anything Sketchfab
exported -- it marks every material doubleSided and anything with a hint of opacity BLEND -- and
the_bathroom_free's curtains, doors, towels and carpets swayed in the wind indoors. Measured: palm
fronds, fern_02, ground/young palm 35-68 %; that bathroom's Door_and_windows 2.2 %, Fabric 0.9 %. A
single-sided cutout stays a surface (curly_palm's frond boot is bark), which misses date_palm's
single-sided fronds -- that is what the choice is for. The Import window's Materials list shows each
material's verdict and measurement and takes a per-material choice (Auto / Leaf card / Surface); MCP
`import_asset {surfaces: {"<material>": "leaf"|"surface"}}` is the same choice and its answer lists
every verdict. Auto only decides NEW assets and NEW material files; a choice also switches an existing
asset's wind slot and its material's four shading keys (`materialgen::ApplyLeafShading`), every other
edit kept. `--seam-ratio=<gltf>` prints the verdict per slot headless; `intent_regression`'s
`TestLeafCards` holds the rule on the staged sources.

**Handedness.** glTF is right-handed (front +Z, up +Y, the asset's right -X), the engine left-handed
(`LookAtLH`): `MeshManager::ParseGltfFile` negates X after each node's world transform (UnityGLTF's
convention) and the det -1 turns the winding through the negative-scale rule. Until 2026-10-24 it read the
coordinates as they were and every glTF asset rendered as its own mirror image -- the_bathroom_free back to
front against its Sketchfab render; the CCW winding only looked right because the mirror flipped it too.
`intent_regression`'s `TestGltfHandedness` holds it (a vertex on glTF -X lands on +X; the front stays
clockwise). `kMeshBinVersion` 3 marks converted bakes; all 148 .mesh.bin were re-baked from their manifests
(`--reimport --reimport-src="<src>" --reimport-out=<bin> --reimport-manifest=<mesh.json>`; a quoted value may
hold spaces -- node names do). The levels were migrated so every copy stays its own mirror where it stood: the
`the_bathroom` group and its candles mirrored about the room's centre plane, 459 tilted copies (pitch/roll)
about the vertical plane nearest their local X (keeping their ground contact; keeping the rotation would
have mirrored them about a tilted plane, into the sand), the fishing boat about its hull centre (2.3 m off its
origin); seashells' `restRotationDeg` -> (p, -y, -r). OBJ is still read as it was (teapot, corgi).

## Mesh LOD -- the Error Is Measured on the Screen It Is Seen On

Selection is UE's (`rendering/meshes/LodSelect.h`: screen size against a per-mesh estimate from the baked deviation,
temporal dither), and so is the estimate's form -- `ViewDistance = deviation * pixelsPerUnit / PixelError + R`
(`StaticMesh.cpp:3064-3134`). UE fixes `pixelsPerUnit` at 960 (1920 wide, 90 degrees) and PixelError at 8: eight
1080p pixels, a visible pop, tuned to UE's own deviation metric (`sqrt(quadric)/8`, which reads small). With
meshopt's distance at 8, the_bathroom_free's props switched in arm's reach (LOD1 from a median 1.6 m, 36 of 100
inside 1 m, the candles at 0.37 m where their 0.9 mm was 3 pixels of a 1440p screen). Since 2026-10-24
`pixelsPerUnit` is THIS frame's -- 0.5 x output width x P[0][0] (1280 at 2560 wide, 90 degrees), the screen size
taken with this frame's multiple too -- and `lod.pixelError` (Developer Controls > LOD "Pixel error", graphics
settings `lod.screenPixelError`; the old key `pixelError` is ignored on purpose) is in those pixels, default 1:
a LOD comes in where its worst deviation spans one pixel of the output. The bathroom: LOD1 from a median 8.6 m,
none inside 1 m (the candles at ~2 m); every model: median 1.2 -> 6.4 m. Cost on wind_test (level start, the
grove, the bathroom; old = `lod.pixelError:6`, interleaved): GPU frame +0.009 / -0.007 / +0.015 ms, noise. A
mesh's numbers: scratchpad-style `lod_survey.py` (the .mesh.bin LOD table: triangles, deviation, the distance
each LOD comes in; `LOD_K` = pixels per unit / PixelError). **Superseded on 2026-11-11 by the paragraph below.**

**UE's heuristic again, on a MEASURED deviation** (2026-11-11, owner, on boats that never reached LOD3: "do it as UE: the
measured deviation and a normal LOD3", then "fully UE, and push the small meshes' threshold by hand so the bathroom stays
right", "the trees keep their aggressive LOD"). Four parts:
- **The deviation is measured** (`MeshManager.cpp MeasureLodDeviation`, every bake and the runtime parse; a chunked mesh keeps
  meshopt's): the two-sided distance between the level's surface and LOD0's -- every vertex plus points ~1/512 of the mesh's
  size apart, each against the other side's triangles exactly (an AABB tree). meshopt's `result_error` is its quadric's (and
  the attribute terms under `lodNormalWeight`) and read 2-3x the real distance at LOD3: police_boat 2.29 m against 1.10. The
  bake logs both: `[meshbake] '<bin>': LOD3 normal, deviation from LOD0 in m, measured (meshopt's): ...`.
- **UE's chain** for every asset with no foliage slot and no chunks (`BuildLodsCpu ueChain`): each level half the
  triangles (50 / 25 / 12.5 % of LOD0, UE's LOD groups' LODPercentTriangles=50) and no error budget (UE's MaxDeviation 0):
  the budgets stopped meshopt short -- steps of 85-99 %, LOD3 as heavy as LOD2. `lodErrorScale` caps the budget below 1 (the
  mesh's size); a cap of 0.1 is the cure for one collapse that ruins a level (police_boat's LOD3 1.10 -> 0.48 m,
  Sink_002's 0.44 -> 0.056). Foliage keeps its tuned chain (UE's foliage makes no LODs) and the terrain its budgets.
  `lod3Aggressive` absent = harsh only for an asset with a foliage slot (`Lod3AggressiveOf`); the import window says Auto /
  Aggressive / Normal.
- **UE's threshold**: `AutoLodScreenSizes` on UE's reference view (1920 x 1080, 90 degrees: 960 px per unit, multiple 0.889),
  PixelError `lod.pixelError` 8 (graphics settings `lod.uePixelError`; the old `screenPixelError`, 1 px of this screen, is
  ignored), or the asset's UE LOD group, mesh.json `"lodGroup"` (`render::kUeLodGroups`: SmallProp / LargeProp / Deco 10,
  LevelArchitecture 12, HighDetail 6; Mesh Editor > LOD "LOD group (UE)"), or its own `"lodPixelError"`. Assigned: the boats
  and the tent LargeProp, the beach props and the buoys SmallProp, the lagoon floor LevelArchitecture, and every
  the_bathroom_free node `"lodPixelError": 1` (seen at arm's length): its LOD1 median 8.6 -> 9.4 m, none nearer than 2 m
  (6 before), LOD3 77 -> 43 m. The boats' LOD3: police_boat 2.94 km -> 52 m, small_yacht 2.46 km -> 26 m, fishing_boat
  1.22 km -> 39 m; old_boat takes `lodPermissive` (its LOD2 and LOD3 were stuck at 3530 triangles on its seams: 4179 /
  2089 / 1043 now). The bins before: `D:\Programming\test_cube_backups\mesh_bin_v5_before_measured_lod_2026-11-11`, the
  manifests `...\mesh_json_before_ue_lod_2026-11-11`.
- **What is out of range, and what to do** (`rendering/meshes/LodSwitchReport.h`, one report): Mesh Editor > "LOD switching"
  (`--edit-mesh=<mesh.json>#lod`; the header counts the warnings) -- per LOD triangles, measured deviation, UE's screen size,
  where it comes in, the mesh's and the deviation's 1080p pixels there, "skipped" for a level the next one shadows --; the
  importer logs every warning with its fix (`import <asset>: LOD: ... Fix: ...`), shows them under the Import window's
  status and in MCP `import_status` (`lodWarnings`); `test_cube.exe --lod-report[=<path part>]` prints all of it for every
  asset (exit code = assets with warnings). Warnings: a level with as many triangles or a weak step (over 1000 triangles);
  a level that comes in under 4 px; over 2 triangles a pixel before the next level (over 1000); an authored screen size that
  pops; a gap of x25; no measured deviation. Each names the knob (`lodErrorScale`, `lodPermissive`, `lodDropSmallParts`,
  `lodRatioScale`, `lodGroup` / `lodPixelError`, the foliage prune).
- **The discrete chain folds double-sided duplicates as the DAG does** (same day, owner: "let date_palm take the DAG's
  method"; `tessy::FoldTwins`, public in TessyDag.h now): a range whose every triangle has a back twin simplifies its front
  triangles and emits each kept front's back. It needs `meshopt_SimplifySparse` (and the budget in absolute units): meshopt
  classifies a vertex by every vertex at its position in the whole buffer, so each front vertex, its twin unreferenced beside
  it, read as complex -- without Sparse the folded fronds came back whole, 8400 indices in, 8400 out. date_palm LOD1 / LOD2
  4438 / 4148 -> 1658 / 766 triangles at a measured 0.37 / 0.43 -> 0.085 / 0.27 m; curly_palm 2191 / 1221 -> 1181 / 275 at
  0.115 / 0.118 -> 0.062 / 0.105 m; their LOD3 (the prune) as it was. `--tessy-lod-fold=0` turns it off with the DAG's.
- **A skipped level is replaced and the chain filled** (same day, owner: "if LODs are skipped, as on some yachts, generate the
  missing levels: L3 becomes L1 and L2, L3 are made anew"). On UE's chain the levels are made one at a time, each half the
  last, and measured: one that deviates no more than the level before it (within 0.1 %, or 1e-4 of the mesh's size: a flat
  tile measures float noise) takes that level's place -- it would come in at the same distance and the finer one would never
  be drawn -- and the halving goes on until three levels differ, the simplifier stalls (under 5 % fewer triangles) or ten
  halvings are spent, so a mesh may keep fewer than three. The bake logs `chain UE, N skipped level(s) replaced by coarser
  ones`. fast_boat_13 (LOD1-3 all 0.0213 m before; with `lodPermissive` now): 25 / 12.5 / 6.3 % at 0.018 / 0.024 / 0.031 m;
  Floor_tile_001 (`lodErrorScale` 0.1): one level of 18 triangles, exact. After all of it `--lod-report`: 155 assets, no
  warning, no skipped level; every bin's LOD0 and vertices byte for byte as before.
- **A range keeps half its area** (same day, owner: "on small_yacht's last LOD the glass disappears"). With no budget a thin
  plate is the simplifier's cheapest prey -- its faces meet at the cost of its thickness: the windshield went 2.9 -> 0.027 m2
  at LOD3, and the measured deviation could not tell (the cabin lies behind the pane; LOD3's 0.215 m was mostly it). A range
  that keeps under half its area takes twice the triangles until it keeps it (`RangeArea`; not under SimplifyPrune, where a
  part's vanishing is the point); the bake logs `N range simplification(s) redone to keep half the area`. Hit: small_yacht
  (LOD3 0.215 -> 0.042 m, the glass whole), the tent, the bathroom's three curtains and two folded towels.
**A mesh takes a share of the pixel error**: mesh.json `"lodPixelErrorScale"` (Mesh Editor > LOD "Pixel error scale";
absent = 1; runtime, no rebake) multiplies the frame's pixel error for this asset -- the discrete LODs' (Mesh::
GetLodScreenSizes) and the cluster DAG's (ShadowGpuData divides the uploaded cluster and level errors by it, so the
camera's cut, the VSM pages' and the atlases' agree; a root's FLT_MAX stays). Set on the shared Mesh by every copy
(RenderableObject::SetLodPixelErrorScale). Under 1 an asset keeps its detail nearer the eye. small_yacht at 2 px (the
owner's tessy.lodPixelError) from 3.5 m: 0.25 took the camera's triangles 13.7 -> 23.4 k and the hatch's facets away.
But first look at how the asset was BAKED (below, the DAG's normal weight): the facets were its simplification's.

## Tessy -- Cluster Geometry (the VSM Pages, the Lamps' Atlases, the Camera; Plan N0-N7)

Tessy is our UE Nanite analog (`docs/tessy_plan.md`, journal in its section 9). A CLUSTER is a run of <= 128 triangles
/ 256 vertices of one LOD's index buffer inside one submesh (`Mesh::Cluster`); the classic paths draw the same index
buffer and never notice. `.mesh.bin` v4 carries them after each LOD; v3 still loads, without clusters, and such a
mesh stays on the classic path in the same frame (the log line `tessy: clusters for N of M caster groups (...);
classic path: <assets>` says who is where). `--tessy-upgrade=<a.mesh.bin>,...` rewrites a v3 file as v4 in place --
the same vertices byte for byte, the same triangles per (LOD x submesh), hashes kept; it writes into `models/`, so it
is the owner's call (plan Q4) -- back the files up first. Done for all 148 on 2026-10-30 (owner): the shared `models/`
is v4 (log `classic path: none`), the v3 originals are in `D:\Programming\test_cube_backups\mesh_bin_v3_2026-10-30`. A
checkout older than the Tessy merge (0bd5c72f) reads v3 ONLY and refuses every one of them.

**`.mesh.bin` v5 carries the cluster DAG** (N7, below) after the LOD blocks: a bake (`--bake`, `--reimport`, the import)
builds it with the asset's own simplification weights (`MeshManager.cpp BuildDagForMesh`, the same function a load uses
for a v4 file), `--tessy-upgrade=<bin>,...|all` adds it to a v3/v4 file in place (clusters kept, `all` = every
`models/**/*.mesh.bin`; a v5 file is left alone), and a load takes the stored DAG after `ValidateDag` -- no build at boot.
Done for all 148 on 2026-10-30 (owner: "bake the DAG at import and bake every mesh"): 4624 DAG clusters, 511 k triangles
over all levels; the v4 files are in `D:\Programming\test_cube_backups\mesh_bin_v4_2026-10-30`. A checkout before this
reads v4 at most and refuses them. `--tessy-lod-build` still rebuilds at load (the A/B of a builder change).

**What a bake stored is readable** (2026-11-11, owner: "is Tessy always baked into the mesh, with no settings of its own? at
least write its stats"). There is no per-asset Tessy switch: the clusters (<= 128 triangles / 256 vertices, every LOD) and
the DAG are in every v5 file; the DAG takes the LOD section's `lodNormalWeight` and `foliageUvWeight` (foliage slots) and the
process's `--tessy-lod-prune` / `--tessy-lod-fold`; a load cuts the clusters into meshlets (<= 64 vertices). A bake now logs
`[meshbake] '<bin>': Tessy -- DAG weights ...; LOD clusters (meshlets) ...; DAG ... by level (clusters, tris, max error m)`
beside its DAG summary, and Mesh Editor > "Tessy (clusters)" (`--edit-mesh=<mesh.json>#tessy` opens and scrolls to it)
shows the same from the bin (`MeshManager::DescribeMeshBinary` / `DescribeDag`): per LOD triangles, clusters, meshlets and
deviation; the DAG's levels with their clusters, triangles, share of LOD0 and error range; the roots (the fewest triangles a
far copy draws under `tessy.lod`); and the knobs in force. The Mesh Editor read the bin only for a chunked mesh until then,
so "Baked deviation from LOD0" never showed on any other. police_boat: 1750 / 904 / 474 / 205 clusters, the DAG 8 levels,
x2.00 of LOD0 over all of them, roots 8412 triangles (4 %) at 3.04 m.

- **Developer Controls > Tessy** (2026-10-30, owner: "its own tab, a master switch to turn it all off, and a cluster
  debug view"): the master switch `tessy.enabled` (default 1; `graphics_settings.json` vsm.tessyEnabled, `--set`, MCP)
  turns EVERY Tessy path off -- the VSM pages and the lamps' atlases draw whole meshes, the cluster view is off -- while
  `tessy.vsm` / `tessy.raster` / `tessy.atlas` keep their values (greyed out); the cluster tables stay loaded, so it
  switches both ways at once. The verdicts say `off (tessy.enabled 0)` (`tessy.vsm: classic path -- ...`).
- **The cluster view** `tessy.debug:1|2|3` (the tab's "Cluster view" Clusters / Meshlets / Levels; not persisted) draws
  every caster the camera drew this frame over the lit scene colour in `Main_DebugDraw`, one hashed colour per cluster or
  meshlet -- or, Levels (N7e), a DAG cluster by its DAG level (green = the leaves, yellow, red, magenta = the coarsest; the
  kernel's `kFlagLevelId` puts the level in the record's .w high half) and a discrete LOD's cluster by its tier (light to
  dark blue), so `tessy.lod` is SEEN cutting the DAG as the camera moves,
  cutouts clipped: the atlas kernel runs a third row set (2, the camera's row: `camArgs_` / `camVisibleList_`) with keep
  rows / no cull / cluster id in the record's `.w`, and `TESSY_ATLAS_MS` + `TESSY_VIEW` draws it with the G-buffer's
  jittered view (`BuildGBufferViewCB`), depth `GREATER_EQUAL` + a slope bias against the frame's own depth. So it shows
  the camera's geometry and LODs, not the shadows' -- the shadow paths cull their own. While it is on, the camera's
  two-pass HZB cull is off (pass B's rows would be missing from the row it reads); it needs the indirect G-buffer and a
  mesh shader tier, and says why not (`tessy.view: off -- ...`, the tab's `cluster view:` line, `get_readouts`
  tessy.view). The auto exposure meters the colours too: the sky darkens behind them. Cost: `Pass_DebugDraw`
  0.03-0.04 ms on the grove (9.2 k meshlets).
- **N5, the camera's G-buffer by clusters** (`tessy.gbuffer`, default 1 since 2026-10-30 -- owner: "switch Tessy on by
  default in the G-buffer" -- at the cost measured below; Developer Controls > Tessy "camera G-buffer",
  `graphics_settings.json` vsm.tessyGBuffer, `--set`, MCP; verdict `tessy.gbuffer: on` / `classic -- <why>`): the camera
  row goes through `tessy_atlas_cull_cs.hlsl` too, BY PSO RUN (`kFlagByRun`: one 1024-thread group per run of the merged
  G-buffer commands, `VgRun` maps a virtual group to its run) -- every (caster, cluster) box, wind padded, against the
  camera frustum, the survivors' meshlets to the run's row, the rows zeroed -- and each run's MESH SHADER TWIN draws
  them in pass A (`gbuffer_indirect.hlsl TESSY_GBUFFER_MS`: the run's own desc and defines; one DispatchMesh spans the
  run's groups, so the record's `.w` carries the virtual group -> the LOD side of the crossfade and the group's
  SurfaceParams from a per-frame buffer (`MaterialData::FillGBufferSurfaceParams`), the textures by
  `NonUniformResourceIndex`). A run the kernel took whole writes 0 into its classic merged commands' COUNT buffer
  (`kTessyCamClassicCountOffset` in the args region): 824 empty commands cost the grove 0.045 ms. Table PSOs, pass B,
  GBV's per-group path keep their classic draws; never on the validation frame (the readback sums the camera row) nor
  in wireframe. Measured 2026-10-30 (DLSS Balanced, panels closed, alternated x2): the G-buffer 0.155 vs 0.171-0.174 ms
  on the grove, 0.169 vs 0.187-0.189 in the tunnel, 0.181 vs 0.173-0.174 in the bathroom, but the kernel adds
  0.013-0.035 -- the frame +0.01-0.02 ms. Picture: on/off at the noise floor (grove 0.23 vs 0.21-0.22 %, the cabinet
  0.03-0.16 vs 0.44-0.64 %): on wind_test's low-poly assets clusters do not pay in the main pass (off by default until
  the owner switched it on).
  The mesh shader's 25 output values a vertex are not its limit (a 19-value version measured the same).
- **`tessy.gbufferHzb`** (default 0, not persisted): N5's two-pass HZB per CLUSTER (UE's main/post): a cluster last
  frame's pyramid hid goes to `TessyCamDeferred` (claimed with the run's records -- both fit or the run stays classic;
  unwritten slots dead), `tessy_camera_post_cs.hlsl` in Main_CamCullPost retests it against this frame's pyramid into the
  run's pass-B row (after pass A's), Main_GBufferB draws it with the twins. wind_test defers 1.5-5 % of the clusters
  (grove 66-76 of 4325, tunnel 330-338), and the test + pass B cost more than they cull (+0.03-0.04 ms) -- for dense
  assets. Needs the camera's two-pass HZB that frame (the cluster view switches it off).
- **N7, the cluster DAG (continuous LOD)** (plan section N7 "Этапы"): `rendering/meshes/TessyDag.*` builds
  UE's DAG over LOD0's clusters per submesh (baked into v5 since N7e; at load for a v4 file) -- groups of ~16 (`meshopt_partitionClusters`), each simplified to
  half (the discrete LODs' normal / foliage-UV weights, `ErrorAbsolute | Sparse`) with the vertices it shares with other
  groups or other submeshes locked BY POSITION, recut into clusters, to the root; every cluster carries the error and
  sphere of the simplification that made it (lod*) and of the one that consumes it (parent*). Every v5 asset has one; a
  v4 file builds one at load only when its mesh.json says `"tessyLod": true` or `--tessy-lod-build=all|<path part>,...`
  names it. `Mesh::Dag` + its own IB, appended to the mega IB after the mesh's LODs; the DAG clusters join
  the cluster table with a parallel `ClusterLod` table, per mesh group a DAG range and per level its error interval
  (`TessyClusterLodSrv` / `TessyDagGroupSrv` / `TessyDagLevelSrv`). `tessy.lod` (default 0, Tessy tab) lets the VSM
  CLIPMAP pages draw the DAG's cut (`tessy_vsm_cull_cs.hlsl`: an orthographic page has a distance-free threshold,
  `tessy.lodShadowTexels` (2) x its texel size; the run's levels a contiguous band by its casters' scale spread); a
  switch flushes the page cache. Gate: `intent_regression TestTessyDag` (invariants, the cut's triangles falling, the
  fine cuts' area within 20 % of LOD0's). Measured on the grove (2026-10-30): the palms' DAG at 2 / 4 / 8 texels draws
  3.19 / 3.00 / 2.84 M triangles into the pages against the shadow tier's 2.77 M, `VsmPageRender` the same within
  noise -- the date and curly palms' fronds stop halving at ~45 % where the discrete LOD3 drops whole leaves -- and the
  palms' self-shadowing changes (6-8 % of pixels): the camera still drew discrete LODs, and a shadow finer than its
  receiver breaks the caster/receiver LOD contract.
- **N7d: the camera draws the DAG's cut too** (`tessy.lod` turns the cluster G-buffer on for itself; the gbuffer verdict
  then says `on (tessy.lod: ...)`, the event `tessy.lod: the camera draws the cluster DAG's cut`). On the camera's runs
  `tessy_atlas_cull_cs.hlsl` takes a mesh group with a DAG from `DagGroups` instead of its tier's clusters and tests
  every (caster, DAG cluster): drawn when its own error x the caster's scale x (`g_lodPixelsPerUnit` / lod.pixelError /
  lod.distanceScale) is at most the distance to the NEAREST point of its lod sphere and its parent's is more -- where a
  discrete LOD of that deviation switches (ViewDistance + R), so DAG and discrete meshes share one rule. ONE function
  (`DagTooCoarse`, `precise`) answers both tests: a parent's must answer exactly as its children's own, or the cut
  tears. A crossfading caster sits in two tiers' entries and is drawn once, from its own (`CasterLods`); the record's
  `kTessyCamDagBit` (.w bit 15, `kTessyCamVgMask` is 0x7FFF now) gives the mesh shader no crossfade. Pass B (the casters
  the instance HZB hid) stays classic, on the discrete LOD, for the frame it draws them. Readouts: `tessy.camDag`,
  `camTris` / `camDagClusters` (pass A, counters [24] / [28]). Measured on wind_test (2026-10-30, the palms' DAG, panels
  closed, VSM, alternated twice): the grove draws 259 k camera triangles either way, cull 0.029 -> 0.041 ms (the flat
  test doubles: 4.3 k -> 9.0 k), G-buffer 0.163 / 0.162, VSM 0.42 -> 0.44 (its 2-texel cut is finer than the shadow
  tier); the far view [-20,30,20] 428 k -> 657 k triangles, G-buffer 0.165 -> 0.18-0.19, cull 0.04 -> 0.07-0.09 -- the
  palms' foliage LODs are not error-driven (they drop whole leaves by distance) and the DAG's roots stop at ~45 % of
  LOD0. No crack looking up into the crowns (DLSS off) nor in the cluster view; the picture moves where the shadows do
  (8.5 % of pixels, floor 2.2 %). GBV (`ShadowCull,GBuffer,CamCullPost,GBufferB,VsmPageRender,Skybox` + the Skybox
  self-test): clean.
- **N7e: the DAG's cut in every shadow and a level band for the camera** (2026-10-30). `tessy.lod` now also cuts the
  DAG in the lamps' ATLASES (SDSM / Legacy; the atlas kernel's row sets 0 and 1 take each view's light and its texels per
  unit at distance 1 / `tessy.lodShadowTexels` from `ShadowGpuData::viewDag_`, filled from `LocalViewCull::dag`; a switch
  bumps the lamps' static cache version) and in EVERY VSM page (`tessy_vsm_cull_cs.hlsl PageDagTooCoarse`: an error spans
  e s K / w texels, K = |column x| x 64 / texels, w = the clip w of the sphere's nearest point -- 1 on a clipmap page, the
  light's depth on a local one; the run's levels a band from its casters' threshold spread over `DagSpheres`; a change of
  the cut or its texels flushes the page cache). The crossfade dedupe is the camera row's only (a shadow row holds a
  caster once). The camera's DAG groups test each caster's BAND of levels (`DagBand`: the levels whose error interval can
  meet its threshold anywhere on the DAG's sphere -- `Tessy.DagSpheres`, around every lod and parent sphere): tests on the
  grove 8.3 k -> 4.5 k, the far view 21 k -> 6.0 k, the same clusters; `tessy.lodBand:0` = the flat test (the A/B). Leaf
  cards simplify with `meshopt_SimplifyPrune` (whole cards leave once their radius fits the error, which the error then
  carries; `--tessy-lod-prune=0` = edge collapses alone): date_palm's roots 4275 -> 829 triangles at 0.37 -> 1.17 m, the
  camera's distances unchanged (a leaf is a pixel only at hundreds of metres -- the honest metric does not drop visible
  leaves; the palms' discrete foliage LODs do, by hand, `lodDropSlots` / `foliagePruneKeep`). Measured, wind_test with
  "Lamps" at 23:00 on the grove, alternated twice: SDSM `Pass_PointShadows` 0.280-0.282 -> 0.260-0.261 ms (atlas meshlet
  records 34.3 k -> 27.5 k), VSM `Pass_VsmPageRender` 1.082-1.089 -> 1.050-1.060 (9.5 -> 8.9 M triangles); the picture moves
  where the lamps' shadows fall on far palms (SDSM 1.7 %, VSM 2.3 % of pixels over 8 levels, floor 0.2-0.3 %), "0" repeats
  "0" and "1" repeats "1" at the floor. A flight burst (60 fps, DLSS off, the temporal second difference per frame) tells
  the arms apart nowhere -- the motion swamps the metric, so it shows no pop stands out, not that none exists. GBV (SDSM:
  `ShadowCull,PointShadows,SpotShadows,LocalShadowCache,GBuffer,Skybox`; VSM: `VsmPageRender,ShadowCull,GBuffer,Skybox`; the
  bathroom at 23:00 with the Skybox self-test): clean. The DAG is baked into .mesh.bin v5 since (above). The SDSM / CSM
  sun partitions draw discrete LODs, by clusters under `tessy.sdsm` (below) -- an orthographic DAG cut there is not done.
  With every asset's DAG (2026-10-30, SDSM, panels closed, alternated twice) `tessy.lod` 1 in the G-buffer: grove
  0.157 vs 0.160 ms, cull +0.009-0.012 (262 k vs 263 k triangles); bathroom 0.167-0.168 vs 0.170, cull +0.001-0.004;
  tunnel 0.174 vs 0.170, cull +0.014 (482 k vs 419 k triangles); the far view [-20,30,20] 550 k vs 382 k -- the palms'
  foliage LODs drop whole leaves by hand, the DAG's honest metric does not. The picture moves where LODs differ (grove
  2.6 %, tunnel 1.8-2.0 %, floor 0.8-1.7 %). Hence `tessy.lod` stays 0 by default. A FACETED DAG ON A GLOSSY HULL IS ITS BAKE (2026-11-09, owner at small_yacht's bow hatch:
"tessy switches clusters very early, the DAG mangles the UVs"): the DAG takes the asset's `lodNormalWeight` (and leaf
UV weight), and small_yacht's is 0 -- a position-only simplification on ONE shared vertex buffer: a collapse a tenth of
a millimetre off the surface leaves LOD0's normals interpolated across a triangle twice the size, and at 2 px a
smooth white hull showed it as wedges of light 3 m from the eye. Rebaked into the scratchpad (`--reimport
--reimport-out=<scratch> --reimport-manifest=... --reimport-normal-weight=<w>`, models/ untouched) and viewed through
a scratch level copy: w 0.5 and 1 both smooth where w 0 was faceted, the camera's triangles 13.7 -> 16.3 / 18.9 k on
that view (the normal error joins the cluster's error: the roots 1.5 m -> 10.6 / 40 m). No UV weight on a surface slot
exists yet (`foliageUvWeight` is the leaves'); the yacht's artefact was the normals'. small_yacht carries `lodNormalWeight` 0.5
since (rebaked in place, the bytes the scratch bake was judged on; the old file in
`D:\Programming\test_cube_backups\mesh_bin_v5_small_yacht_before_normal_weight_2026-11-09`): both bow views smooth, 18.3 /
16.3 k triangles. Its LOD0, collision and water mask hull are LOD0's and did not move. The Import window's LOD "Normal
weight" starts at 0.5 since (owner: "make it 0.5 by default"; MCP `import_asset` takes the window's value too): a NEW
import writes `"lodNormalWeight": 0.5`; an absent key still means 0, the bake the manifest describes, and a bulk
re-import keeps each asset's own (Mesh Editor > LOD shows the same key).
- **The camera's cluster records are laid out in the classic draw's order** (`tessy_atlas_cull_cs.hlsl` `kFlagOrdered`,
  2026-10-30, owner at cam -62.12 46.92 136.30: "with the DAG the meshes flicker, the bathroom roof most"). The mesh
  shader draws a run in RECORD order, and the records went where the atomics landed -- new every frame. Two coplanar
  surfaces of two meshes (the_bathroom_free's ceiling: `Selling_top` and `Selling_top_001`, one LOD each, the same
  plane) then swapped the depth-test winner every frame: the roof flickered between the perforated sheet and the rusty
  one, 19.5 % of the roof's pixels over 8 levels across 48 still frames with DLSS off (10.7 % with DLSS). The classic
  draw had kept them in its command order, and before the DAG both drew classic (no clusters at their far tiers). Now
  the camera rows (the G-buffer's runs, the cluster view) gather a run's virtual groups in the merged draw's command
  order (`RunCmds`, t13: per run {first, count}, then `cmdOrder_`) and write the records in test order (entry, caster,
  cluster) by a group prefix sum (`OrderedPrefix`); the shadow rows write depth alone and keep the atomics. After: the
  roof 0.06 % (no DAG 0.06 %), and the same layer wins as classic (3 % of the roof differs, the DAG's geometry). Cost:
  `ShadowCull.TessyCamera` +0.004-0.006 ms (grove, tunnel), +0.012-0.018 on that aerial view, +0.02-0.04 with the DAG
  there; the G-buffer reads ordered records a little faster (bathroom 0.179 -> 0.172). `tessy.ordered:0` = the atomics,
  the A/B (it brings the flicker back). Which of a run's entries overflow `kMaxEntries` (1024; thousands of palms from
  the air) and stay classic is now the command order's too. GBV (`ShadowCull,GBuffer,SdsmCull,SdsmShadow,Skybox` + the
  Skybox self-test, that view with `tessy.lod:1`, 833 frames): clean.
- **A camera run is culled by 16 groups** (`tessy.camSplit`, transient, 1-32; 2026-10-31, owner on the aerial view
  cam -70.98 34.67 144.04: "why are the discrete LODs faster even at 2 px?"). One 1024-thread group a PSO run left as
  many SMs working as there were runs (six) -- ~8-10 ns a (caster, cluster) test, the rate the SDSM partitions had
  before their split -- and the DAG's ~30 k tests cost 0.26 ms of `ShadowCull.TessyCamera` against the discrete LODs'
  8 k at 0.08, more than the DAG saved the G-buffer. Now group g takes run g / split and the (g % split)-th CONTIGUOUS
  part of its commands (the classic draw's order), writes its own row, and `RecordTessyGBufferDraws_` issues a run's
  rows in slice order, so the records keep the order the roof flicker fix needs (the bathroom roof: 0.05 % at 1, 8 and
  16 slices). The run's classic count is written by its LAST slice to finish: each adds 1 (+ 1 << 16 when it left records
  classic) to the run's counter at `kTessyCamRunDoneOffset` + 4 r (the counter region is 336 bytes; at most 64 runs).
  Pass B's rows start after pass A's slices. Measured (DLSS Balanced, panels closed, alternated twice), the camera cull
  at 1 / 4 / 8 / 16 / 32: DAG 0.258 / 0.172 / 0.113 / 0.057 / 0.042 ms, discrete 0.082 / 0.066 / 0.060 / 0.058 / 0.056;
  the G-buffer +0.005 at 16 (a DispatchMesh a slice). On that view the GPU frame with the DAG at 1 px is now the
  discrete LODs' (3.34 vs 3.32 ms), at 2 px below it (3.29).
- **A count buffer never holds 0xFFFFFFFF** (2026-10-30, owner at cam -44.92 11.56 97.73, 16:00: "with the DAG this
  small rock dithers"). A camera run the kernel did not take whole wrote ~0 into its classic count, meaning "all the
  run's commands" -- ExecuteIndirect is specified to take min(count, MaxCommandCount), but this driver (NVIDIA
  32.0.16.2101) then drew NOTHING: every group a run left classic vanished. Proven by forcing such a run classic whole:
  the island, the bathroom's walls and the trunks went with it; with the exact count back. The rock's group fell out of
  the kernel's entries in some frames (DAG band blocks handed out first come, first served: 156 blocks left 18 DAG
  groups no room on the grove's run) and then out of the frame. Now the count is the run's exact command count
  (`RunCmds[2r + 1]`), and a band block is handed out only if every DAG group after it keeps room for a plain entry (no
  DAG group falls to classic for want of entries; the band then still tests 25-40 k clusters on the rock and aerial
  views against the flat test's 62-82 k: `ShadowCull.TessyCamera` 0.20 vs 0.24 ms, 0.24 vs 0.33). The rock's box over
  48 still frames: 0.00 % of pixels move over 8 levels (DLSS off), DAG and discrete alike. Any count buffer: write the
  number, never a sentinel.
- **Double-sided duplicates are folded in the DAG** (2026-10-31, owner: "so the current LODs are better for the palms? is
  it the foliage, do the trunks simplify fine?"). The trunks did (one cluster, 70-120 triangles). date_palm's fronds,
  curly_palm's fronds and curly_palm's frond boots are double-sided sheets built of DUPLICATES -- every triangle has a
  back twin at the same positions with the opposite winding (`twin_scan`: exactly those three submeshes over all 148
  assets, 100 % paired) -- so every edge is shared by four triangles, meshoptimizer calls the vertices complex and never
  collapses them, and only `meshopt_SimplifyPrune` acted: date_palm's 35 fronds (~1.1 m each) went 5600 -> 560 at once at
  1.05 m of error, i.e. the camera drew all 5600 out to ~1.3 km. `TessyDag.cpp FoldTwins`: a submesh whose every
  triangle is paired grows its DAG over one side (half-size clusters) and every emitted cluster carries its triangles'
  twins (each front vertex's back vertex, the winding reversed). date fronds now 5600 -> 2796 -> 1398 -> 698 -> 136 -> 20
  (1.9 / 6.9 / 26 / 113 cm), curly fronds 1568 -> ... -> 72, boots 2400 -> ... -> 74; roots date 829 -> 283, curly 1515
  -> 307. Live, `tessy.lod:1` with the two palms rebuilt at load (`--tessy-lod-build=date_palm,curly_palm`), DLSS
  Balanced: grove 243 k triangles (discrete 263 k, old DAG 262 k), far view 320 k (378 k, 544 k), aerial 916 k (972 k,
  2.21 M); G-buffer 0.156 / 0.153 / 0.245 ms against the discrete 0.166 / 0.178 / 0.26 -- the DAG now beats the
  discrete LODs on all three, but its camera cull costs the aerial view 0.25 ms against ~0.1. date_palm and curly_palm
  were rebaked with it the same day (owner: "rebake"; `--reimport` from their manifests -- every byte before the DAG
  section identical to the old file, the loaded numbers equal to the at-load build); the unfolded files are in
  `D:\Programming\test_cube_backups\mesh_bin_v5_unfolded_dag_2026-10-31`. `--tessy-lod-fold=0` = no folding (the A/B); `intent_regression TestTessyDag` holds it on date_palm (folded, the 10 cm cut under half,
  every folded cluster's second half its first half's twins).
- **`tessy.lod` is persisted** (`graphics_settings.json` shadows.vsm.tessyLod, since 2026-10-31 -- owner: "turning the
  DAG on is not saved"): every v5 asset carries its baked DAG, so nothing is built at load any more.
- **`tessy.lodPixelError`** (default 1 px; Developer Controls > Tessy "DAG pixel error", `graphics_settings.json`
  vsm.tessyLodPixelError, `--set`, MCP): the camera's DAG cut threshold on its own -- the pixels a cluster's error may
  span before its parent takes over (the discrete LODs keep `lod.pixelError`; the SDSM sun's camera cut follows it).
  Measured with the folded palms: 0.5 / 1 / 2 / 4 / 8 px = grove 298 / 244 / 159 / 108 / 78 k triangles, far 481 / 320 /
  205 / 176 / 126 k, aerial 1.42 M / 916 / 749 / 564 / 505 k (G-buffer 0.295 -> 0.175 ms); the discrete LODs unmoved.
- **`tessy.sunCut`** (default 1, transient): under `tessy.lod` the SDSM partitions cut the DAG with the camera's eye and
  threshold (`kFlagCameraCut`) -- a far palm's shadow then has the fronds the camera draws, not LOD3's thinned ones (the
  aerial view: 2.4 % of the frame over 8 levels against a 0-0.7 % floor, the sand under the far crowns); +0.015-0.06 ms
  of SDSM cull + draw. 0 = their discrete tier.
- **`tessy.vsmCameraCut`** (default 1, transient; 2026-10-31, owner at cam 45.20 4.51 42.03, 17:00, VSM: "shadow acne"):
  under `tessy.lod` the VSM CLIPMAP pages cut the DAG with the camera's eye and threshold too (`tessy_vsm_cull_cs.hlsl
  CamDagTooCoarse`, the camera row's `DagTooCoarse` term for term; `ShadowGpuData::TessyDagCamThisFrame`, kept every frame).
  Cut by their own texels, a coarser level drew a coarser surface than the receiver's -- up to `tessy.lodShadowTexels` of
  ITS texels above it -- and SMRT's rays, which walk out of the receiver's level into the coarser ones, met it: the sand
  dark and hatched in a patch edged where the level changes. Neither half alone: `tessy.lod:0` or `vsm.smrtRayCount:0`
  was clean, and caching off was not (it is geometry, not stale pages). Measured at that view: the patch's mean 149.5 ->
  158.0 of 255, 9.0 % of the frame over 8 levels against a 0.4-0.6 % floor; `Pass_VsmPageRender` with every page redrawn
  0.32 -> 0.345 ms there, 0.64 -> 0.70 on the grove (page triangles 1.20 -> 1.41 M, 6.37 -> 7.06 M: a coarse level draws
  the receiver's clusters); cached the same. A local light's (perspective) pages keep their texels' cut. A cached page
  holds the cut of the eye it was drawn from until its level re-renders (a level snaps every texel it moves). 0 = the
  page's own cut, the A/B; a switch flushes the page cache. GBV (`VsmPageRender,Skybox` + the Skybox self-test, that
  view): clean.
- **A cluster or a triangle that holds no sample is not drawn** (2026-10-30, owner: "do we drop a cluster that is tiny on
  the screen? the shadow passes need it too"). Per TRIANGLE: every mesh shader -- the camera's twins, the VSM pages and,
  since then, `TESSY_ATLAS_MS` (the lamps' atlases, the SDSM partitions: root constants {row, flags, viewport} -- 4 now)
  -- culls a triangle whose box holds no pixel / texel centre of its viewport (`tessy.msCull`, exact, default 1; the
  atlases' cost and picture unchanged within noise). Per CLUSTER: `tessy.clusterCull` (default 0, transient; Tessy
  setting `g_clusterCull`) has every cull drop a cluster whose padded box projects between two sample centres
  (`tessy_cull_common.hlsli TessyBoxHoldsSample`: its 8 corners through the view's matrix; a corner at w <= 0 keeps it)
  -- the camera's runs by `CameraHzbCB`'s jittered matrix (SceneRenderer fills it every frame now, HZB or not), the lamps'
  views by `ViewRects` (t15, per view slot: the atlas matrix and the face's edge, from `LocalViewCull`), the SDSM rows by
  their partition's `lightViewProj` (t14) on `Renderer::ShadowTileContentTexels`, the VSM pages by `PageProj` on 128.
  Exact as well, but on wind_test (DLSS Balanced, SDSM cache off) it drops 0-19 clusters of 5-35 k records a view, VSM
  0-17, the lamps' grove atlases 0, 103 of 57 k from 140 m up -- LOD selection at one pixel and the DAG keep a far
  object's clusters pixels wide -- and costs each cull 0.005-0.009 ms; hence off. Counted: `get_readouts` tessy
  `rowRectCulled` (camera + lamps), `sdsmRectCulled`, `rectCulled` (VSM).
- **A RAW view starts on 16 bytes**: a frame's DispatchMesh args are `kTessyAtlasRows` x 12 bytes, and with the third
  row set that stopped being a multiple of 16 -- the second frame's RAW UAV removed the device at its creation (debug
  layer `id=345`, Release: `0x887A0005` at the next create, then abort). The region is `kTessyAtlasArgsRegionBytes`,
  rounded up; anything that sizes a per-frame RAW region by a row count rounds it the same way.

- The VSM page setup hands each (page x caster bucket) of a group with clusters to `tessy_vsm_cull_cs.hlsl`, which
  tests every (caster, cluster) box against the page and appends survivors; `VSM_TESSY` of `shadow_indirect_csm.hlsl`
  draws them as ONE `DrawInstanced(384, N)` (UE's VS raster path, vertices fetched by hand from the mega VB/IB).
- N6, the default: `tessy.raster` 1 draws them through a MESH SHADER instead (`VSM_TESSY_MS` `MSMain`, one
  `DispatchMesh` whose args `tessy_ms_args_cs.hlsl` writes): one group per MESHLET -- a cluster cut at load into runs of
  <= 64 unique vertices (`Mesh::BuildClusterLocal`; a leaf card's cluster has ~2 vertices a triangle) -- each vertex
  transformed once, triangles outside the page or covering no pixel centre culled (`tessy.msCull`, exact). 64 because
  the DECLARED output sets how many groups an SM holds: a 256-vertex group was slower than the VS path. Without a mesh
  shader tier (log `mesh shader tier: N`) or its PSO it falls back to the VS path, logged `tessy.raster: vertex shader
  -- <why>`. Near `SetMeshOutputCounts` no branch and no early return: DXC's `ms_6_6` tail-duplicates the call and the
  validator refuses the shader.
- `tessy.vsm` (default 1; Developer Controls > Tessy, `graphics_settings.json` vsm.tessy, `--set`, MCP) switches live;
  the event `tessy.vsm: on` / `classic path -- <why>` is logged on every change of the verdict. VSM only: the level
  default may be SDSM -- pass `--shadow-mode=vsm` (or `shadow.mode:1`) or the cluster path never runs.
- The wind pad: a cluster's box grows by an exact-per-frame, per-axis interval bound of `WindOffset`'s bend (a
  rotation about the pivot) -- `tessy.windPad` 1; 2 = the isotropic 2D + F A/B; 0 = none (only to see the holes).
  `tessy.validate:1` sways every vertex of every tested cluster with the shader's own function and counts those
  outside their padded box -- `get_readouts` tessy.padMisses must stay 0 (proven to catch: 98 % with no pad).
- N4, the lamps' ATLASES (SDSM / Legacy): `tessy_atlas_cull_cs.hlsl` runs in `Pass_ShadowCull` between the cull and
  the S6a compaction -- one 256-thread group per local view (a point face, a spot) tests every (caster, cluster) of the
  view's records against the view's six planes (the wind pad as above), claims the view's run of meshlets with one
  atomic, writes it and zeroes the taken records' `InstanceCount` (the compaction skips them); `TESSY_ATLAS_MS` draws
  the run with one `DispatchMesh` per view right after the view's classic ExecuteIndirect. S7's static rows go through it
  only for the views that draw them this frame (`SetLocalStaticDrawn`: a cache miss or an uncached view; a hit copies
  them). `tessy.atlas` (default 1; Developer Controls > Tessy, `graphics_settings.json` vsm.tessyAtlas, `--set`, MCP);
  event `tessy.atlas: on` / `classic -- <why>`; every 5 s `tessy.atlas: <main> + <static> meshlet records (cap ...), <n>
  views did not fit, <m> clusters tested` (counters of frame N - 3). A view that does not fit stays classic. Measured
  (SDSM 23:00, "Lamps" on): the grove's point atlases 0.415 -> 0.287 ms for +0.034 of cull, frame -0.10 ms; the bathroom
  (five candles) +0.025 -- nothing there to save.
- **The SDSM sun partitions by clusters** (`tessy.sdsm`, default 1 -- owner, 2026-10-30: "switch Tessy on by default in
  SDSM"; Developer Controls > Tessy, `graphics_settings.json` vsm.tessySdsm, `--set`, MCP; verdict `tessy.sdsm: on` /
  `classic -- <why>`, `get_readouts` tessy.sdsm*): Main_SdsmCull, after its re-cull of rows 0..3, runs the atlas kernel's
  ROW SET 3 against the analyze's 16 planes a partition (`gPlanes`: its cull volume and its box; a local view tests 6),
  `kTessySunSplit` (16) 256-thread groups a partition, each a slice of the virtual groups (vg % 16) and its own run (one
  1024-thread group a partition cost 0.05 ms of latency for 6 k tests); Main_SdsmShadow draws each partition's 16 runs
  after its classic draw with the lamps' atlas mesh shader (`RecordTessySunDraw`: the cascade atlas has the atlas PSO
  state, the partition's GPU-written view CB). Pass B of the light-space HZB stays classic; the cut is the discrete
  tiers'. The SDSM counters are [32..48) of the counter region (`gCounterAt`), cleared by Main_SdsmCull only when the
  main cull cleared none; a re-cull that did not run writes every run empty (no stale args). Measured (2026-10-30, the
  live editor, panels closed, 15:00, wind held, alternated twice): `Pass_SdsmShadow` grove 0.203-0.211 -> 0.170-0.173 ms,
  bathroom 0.106-0.112 -> 0.088-0.092, tunnel 0.121-0.131 -> 0.097-0.098, but the cull `SdsmCull.TessySun` costs
  0.034-0.041 / 0.020-0.023 / 0.032-0.040 and the `Pass_ShadowCull` row (the main and the SDSM cull share it) grows by
  0.02-0.05 -- the frame +0.01-0.02 ms by the passes, within the frame total's +-0.1 noise. SDSM does not follow its
  triangle count (N4): 0.18-0.56 M triangles into the partitions. Picture at the floor (grove 1.3-1.6 vs 1.4-1.6 %,
  bathroom 0.19-0.27 vs 0.18-0.26, tunnel 0.75-0.83 vs 0.75-0.83). GBV (`ShadowCull,SdsmCull,SdsmShadow,Skybox` + the
  Skybox self-test, wind_test SDSM): clean.
- **A wave's prefix sum runs in LANE order, not in `SV_GroupIndex` order** (2026-10-29): in an 8x8 group NVIDIA does not
  lay the threads into lanes by `SV_GroupIndex`, and `gsPrefix[SV_GroupIndex] = WavePrefixSum(x)` handed the prefixes to
  other threads' entries -- the atlas kernel tested the wrong (caster, cluster), different lamps lost their shadows every
  frame (the entries' order is the atomics'). Give each thread its place in wave order (the wave's index from an LDS
  atomic by its first lane, plus `WaveGetLaneIndex()`) and distribute by that; `WaveActiveSum` alone is safe.
- A classic shadow range must come from the EXPLICIT LOD's tables: `groupLodMega_` read `SubmeshesForLod` (the runtime
  `lod.forced` / `lod.enabled` override) until 2026-10-29, and a Rebuild under the knob drew another part of every mesh
  in the local views until the next Rebuild. A/B a shadow path with those knobs at their defaults and relaunch after.
- Mesh-shader PSOs stay out of the disk PSO cache (`kMeshPsoDiskCache`, Material.cpp): the runtime takes no cached
  MESH pipeline back. All six stored blobs were refused on every launch (debug layer `id=887`), and a probe in one
  process (2026-10-30, system D3D12 runtime, NVIDIA 32.0.16.2101) had a mesh pipeline refuse its OWN fresh blob
  whatever the stream held (zeroed padding, no root signature / blend / rasterizer / sample mask / pixel shader,
  DEPTH_STENCIL1) and `ID3D12PipelineLibrary::LoadPipeline` refuse it too (`id=972`), while a vertex-shader pipeline
  through the same stream took its blob back. So they compile once a boot (~18 ms of Release); probe again with
  another driver or an Agility SDK runtime before turning the flag on.
- Counters: `get_readouts` block `tessy` (pairs drawn whole / handed to clusters, clusters tested / drawn and their
  triangles, `records` = the draw's instances or mesh shader groups, lost runs / clusters, `meshShader`; `enabled`,
  `debugView`, `view` = the cluster view's verdict, `gbuffer` = N5's, `gbufferTwins`, and the cluster rows' meshlet
  records `atlasRecords` / `staticRecords` / `viewRecords` (the camera's pass A), `viewsNotFit`, `rowClustersTested`,
  `camDeferred` -- frame N - 3 of the last frame a row ran); profiler rows `GBuffer_Indirect.Tessy`,
  `ShadowCull.TessyCamera`, `CamCullPost.TessyCamera`;
  `tools/tessy_baseline.py --port <p>` measures the plan's three views.
- Measured 2026-10-29 (all 148 assets v4 in a copy): the grove's `Pass_VsmPageRender` 1.34-1.39 -> 0.66-0.69 ms on
  the VS path (20.7 M triangles into the pages -> ~3 M in 25-29 k clusters) -> 0.51 with the mesh shader (its draw
  0.41 -> 0.23) -> 0.40 with the scatter's pages shared by the group (below); the bathroom 0.91 -> 0.59 -> 0.48 ->
  0.46, the tunnel view 0.67 -> 0.33 with the `tunnel` group on (N3), then 0.27 -> 0.30 at the same camera with the
  group off as the level saves it -- the boxes are not in those two. Time an A/B of the raster with the
  wind FROZEN (`wind.freeze:1`: the pages still redraw, the cluster counts stay put) -- with the wind running the two
  arms draw different counts and the EMA mixes them (a first reading put the 256-vertex mesh shader ahead; frozen, it
  was behind).
- The page scatter (`vsm_page_scatter_cs.hlsl`, every VSM page render pays it): a thread finds its (object, target)'s
  page rects and counts them, the GROUP queues those pages (a prefix sum in groupshared) and all 64 threads take them in
  turn. Walked by its own thread, each pair waited on its atomic's return before its write and a straddling local caster
  on 341 page-table reads: grove 0.21 -> 0.08 ms, bathroom 0.22 -> 0.14, the same pairs count for count. Comparing two
  runs of the scatter needs `wind.freezeAt:<s>` (and `tod.animate:0`): `wind.freeze:1` holds whatever moment it is, and
  the pages the receivers request move with the pose.
  A picture A/B of it needs the bathroom candles' flicker off and the clouds off (`cloud.enabled:0`): both move with
  time and read as a difference between two shots of the same arm.
