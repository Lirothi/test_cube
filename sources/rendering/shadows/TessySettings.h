#pragma once

#include <string>

// Tessy (docs/tessy_plan.md) -- our cluster geometry, UE Nanite's analog. The live knobs, one per consumer so an A/B
// splits (plan 0.2). Each is read ONCE a frame into its consumer's frame decisions (the pass builder), never in a
// record body, so a frame's cull and draw cannot disagree about it (plan 4.7). Switching allocates nothing: the
// cluster tables and buffers exist whatever the knobs say.
namespace tessy
{
// The MASTER switch: off = no Tessy path runs anywhere -- the VSM pages and the lamps' atlases draw whole meshes, the
// cluster view is off -- whatever the per-consumer knobs below say (they keep their values for when it comes back on).
// The cluster tables stay resident, so it switches instantly both ways. `--set tessy.enabled:0|1`, MCP,
// Developer Controls > Tessy, graphics_settings.json vsm.tessyEnabled.
inline bool g_enabled = true;
// The cluster view (a debug view): every caster the camera draws, over the lit frame in Main_DebugDraw, one colour per
// CLUSTER (1) or per MESHLET (2), or by LEVEL (3, N7e: a DAG cluster by its DAG level, green = the leaves to magenta =
// the coarsest; a discrete LOD's by its tier, light to dark blue); 0 = off. Its records come from the camera's own cull
// row, so it shows exactly the geometry and the LODs the G-buffer drew -- and while it is on the camera's two-pass HZB
// cull stays off (pass B's casters would be missing from that row). Needs the indirect G-buffer and a mesh shader tier.
// Not persisted. `--set tessy.debug:0|1|2|3`, MCP, Developer Controls > Tessy.
inline unsigned g_debugView = 0u;
// N3: the VSM page render draws every caster bucket whose mesh has clusters cluster by cluster
// (tessy_vsm_cull_cs.hlsl + shadow_indirect_csm.hlsl VSM_TESSY) instead of the whole mesh into each page.
// `--set tessy.vsm:0|1`, Developer Controls > Tessy, graphics_settings.json vsm.tessy.
inline bool g_vsm = true;
// The sway pad of a cluster's box in that cull (tessy_vsm_cull_cs.hlsl WindPadBox): 1 = directional (the bend is a
// rotation toward the wind), 2 = isotropic (2 x the shear + the flutter, the first version: the A/B), 0 = none -- only
// to SEE what the pad prevents (a swaying crown's shadow loses the triangles that left their cluster's box).
// `--set tessy.windPad:0|1|2`, transient.
inline unsigned g_vsmWindPad = 1u;
// The pad's validator (tessy_vsm_cull_cs.hlsl gValidate): every tested cluster's vertices swayed exactly as the shadow
// VS sways them and checked against the padded box; get_readouts tessy.padMisses / padMissMaxUm. A debug mode -- a
// frame's worth of vertex work per page. `--set tessy.validate:0|1`, transient.
inline bool g_vsmValidate = false;
// N6: how the surviving clusters are rasterized -- true = the mesh shader (shadow_indirect_csm.hlsl MSMain: each meshlet
// vertex once, triangles outside the page culled before setup), false = the VS path (DrawInstanced(384, N), every corner
// of every triangle). Without a mesh shader tier (or its PSO) true draws through the VS, logged as the verdict.
// `--set tessy.raster:0|1`, MCP, Developer Controls > Tessy, graphics_settings.json vsm.tessyRaster.
inline bool g_vsmMeshShader = true;
// The mesh shaders' own triangle culls (tessy_common.hlsli kTessyMsCull*): bit 0 = a triangle whose screen box holds no
// pixel centre (it would raster nothing -- exact) -- the VSM pages', the camera G-buffer's and, since 2026-10-30, the
// lamps' atlases' and the SDSM partitions' (TESSY_ATLAS_MS). 0 = only the page-border cull. `--set tessy.msCull:0|1`,
// transient.
inline unsigned g_vsmMsCull = 1u;
// N4: the LOCAL lights' atlas views (point faces, spots) draw the casters whose meshes have clusters cluster by cluster
// (tessy_atlas_cull_cs.hlsl + shadow_indirect_csm.hlsl TESSY_ATLAS_MS). Atlas modes (Legacy / SDSM) only; needs a mesh
// shader tier. `--set tessy.atlas:0|1`, MCP, Developer Controls > Tessy, graphics_settings.json vsm.tessyAtlas.
inline bool g_atlas = true;
// The SDSM sun partitions draw the casters whose meshes have clusters cluster by cluster: Main_SdsmCull's rows through
// tessy_atlas_cull_cs.hlsl (row set 3, the analyze's 16 planes a partition) and the atlas mesh shader into each tile.
// Pass B of the light-space HZB cull stays classic. SDSM only; needs a mesh shader tier. `--set tessy.sdsm:0|1`, MCP,
// Developer Controls > Tessy, graphics_settings.json vsm.tessySdsm. On by default (owner, 2026-10-30).
inline bool g_sdsm = true;
// N5: the CAMERA's G-buffer draws the casters whose meshes have clusters cluster by cluster -- the camera row through
// tessy_atlas_cull_cs.hlsl (the clusters culled by the camera frustum, sorted by the merged G-buffer's PSO runs) and
// each run's mesh shader twin (gbuffer_indirect.hlsl TESSY_GBUFFER_MS). Groups of a table PSO, pass B (the casters
// last frame's pyramid hid) and the validation frame stay classic. Needs the indirect G-buffer and a mesh shader tier.
// `--set tessy.gbuffer:0|1`, MCP, Developer Controls > Tessy, graphics_settings.json vsm.tessyGBuffer. On by default
// since 2026-10-30 (owner: "switch Tessy on by default in the G-buffer"; it measured +0.01-0.02 ms of frame on wind_test).
inline bool g_gbuffer = true;
// N5's heavy variant: the camera's clusters are also tested against LAST frame's camera pyramid (UE Nanite's two-pass
// occlusion) -- the hidden ones deferred to Main_CamCullPost, retested against this frame's pyramid
// (tessy_camera_post_cs.hlsl) and the visible ones drawn by Main_GBufferB. Off by default: on wind_test's low-poly
// assets the instance HZB already culls what it could, and the clusters it defers (1.5-5 %) save less than the test and
// pass B cost (2026-10-30: grove +0.04 ms, tunnel +0.03). Needs the camera's two-pass HZB on that frame.
// `--set tessy.gbufferHzb:0|1`, MCP, Developer Controls > Tessy; not persisted.
inline bool g_gbufferHzb = false;
// The camera's cluster records laid out in the classic merged draw's order (tessy_atlas_cull_cs.hlsl Sweep) -- 0 = where
// the atomics land, the A/B: two coplanar surfaces of two meshes (the_bathroom_free's ceiling) then swap every frame.
// `--set tessy.ordered:0|1`, transient.
inline bool g_ordered = true;
// The camera's G-buffer runs, each culled by this many groups (contiguous slices of its commands, drawn in order): with
// one group a run only as many SMs worked as there were PSO runs (six), and the DAG's 30 k tests on the owner's aerial
// view took 0.26 ms. 2026-10-31, DLSS Balanced, the camera cull at 1 / 4 / 8 / 16 / 32: DAG 0.258 / 0.172 / 0.113 / 0.057 /
// 0.042 ms, discrete LODs 0.082 / 0.066 / 0.060 / 0.058 / 0.056; the G-buffer +0.005 at 16 (a DispatchMesh a slice). 16:
// with the cull and the G-buffer added, 16 and 32 measure the same. `--set tessy.camSplit:<1..32>`, transient.
inline unsigned g_camSplit = 16u;
// N7: `--tessy-lod-build=all|<part>,<part>` builds the cluster DAG at load for every clustered asset, or for those whose
// .mesh.bin path holds one of the parts -- what mesh.json "tessyLod": true does per asset, without touching manifests.
inline std::string g_lodBuild;
// N7e: the DAG's leaf-card submeshes simplify with meshopt_SimplifyPrune (whole cards leave once their size fits the
// error) -- `--tessy-lod-prune=0` builds them with edge collapses alone, the A/B (read at load, like the build itself).
inline bool g_lodPrune = true;
// A submesh of double-sided duplicates (every triangle with a back twin) is simplified one side up and emits the twins
// (TessyDag FoldTwins) -- `--tessy-lod-fold=0` keeps both sides, the A/B (read at load / bake, like the build itself).
inline bool g_lodFold = true;
// N7: the cluster DAG's cut where a consumer can take it (N7c: the VSM clipmap pages) for the assets that have a DAG --
// else their discrete LOD's clusters, as before. A switch flushes the VSM page cache (a cached page holds the other
// geometry). `--set tessy.lod:0|1`, MCP, Developer Controls > Tessy, graphics_settings.json vsm.tessyLod (persisted since
// 2026-10-31, owner: "turning the DAG on is not saved" -- every .mesh.bin v5 carries its baked DAG).
inline bool g_lod = false;
// N7e: the camera's DAG cut tests, per caster, only the band of levels its distance can cut (tessy_atlas_cull_cs DagBand)
// -- 0 tests every level of every caster (N7d's flat test), the A/B: the cut is the same, only the tests' count differs.
// `--set tessy.lodBand:0|1`, transient.
inline bool g_lodBand = true;
// Under tessy.lod the SDSM sun partitions cut the DAG with the CAMERA's eye and threshold (kFlagCameraCut), so a caster
// draws the clusters its receiver drew -- 0 = their discrete tier's clusters, the A/B. `--set tessy.sunCut:0|1`, transient.
inline bool g_sunCameraCut = true;
// Every cluster cull drops a cluster whose padded box holds no sample of the view it is drawn into -- its projected
// rectangle falls between two pixel / texel centres (tessy_cull_common TessyBoxHoldsSample): the camera's runs, the
// lamps' atlases, the SDSM partitions, the VSM pages. Exact: such a cluster rasterizes nothing (the mesh shaders' per-
// triangle test, tessy.msCull, finds the same one triangle at a time, after its vertices). `--set tessy.clusterCull:0|1`,
// transient; get_readouts tessy.rowRectCulled / sdsmRectCulled / rectCulled count the drops. OFF by default: on wind_test
// (2026-10-30, DLSS Balanced) it drops 0-19 clusters of 5-35 k records a view (the lamps' grove atlases 0, from 140 m up
// 103 of 57 k) -- LOD selection at one pixel of error and the DAG keep a far object's cluster several pixels wide -- and
// costs each cull 0.005-0.009 ms. For assets whose far clusters do fall between pixels.
inline bool g_clusterCull = false;
// The DAG cut's threshold: shadow texels a cluster's error may span -- UE's MaxPixelsPerEdge (1) x r.Shadow.NaniteLODBias
// (shadows one level coarser: x2). `--set tessy.lodShadowTexels:<x>`, transient.
inline float g_lodShadowTexels = 2.0f;
// The camera's DAG cut threshold: pixels of THIS screen a cluster's error may span -- the DAG's own `lod.pixelError`
// (which the discrete LODs keep): larger = the DAG's clusters coarsen sooner, smaller = later; 1 = the discrete LODs'
// rule. The SDSM partitions' camera cut (tessy.sunCut) follows it; the lamps' atlases and the VSM pages keep
// tessy.lodShadowTexels. `--set tessy.lodPixelError:<px>`, MCP, Developer Controls > Tessy "DAG pixel error",
// graphics_settings.json vsm.tessyLodPixelError.
inline float g_lodPixelError = 1.0f;
// Does --tessy-lod-build name this asset?
inline bool DagBuildRequested(const std::string& binPath)
{
    if (g_lodBuild.empty()) { return false; }
    if (g_lodBuild == "all") { return true; }
    size_t start = 0;
    while (start <= g_lodBuild.size())
    {
        const size_t comma = g_lodBuild.find(',', start);
        const std::string part = g_lodBuild.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!part.empty() && binPath.find(part) != std::string::npos) { return true; }
        if (comma == std::string::npos) { break; }
        start = comma + 1;
    }
    return false;
}
} // namespace tessy
