#pragma once

// Tessy (docs/tessy_plan.md) -- our cluster geometry, UE Nanite's analog. The live knobs, one per consumer so an A/B
// splits (plan 0.2). Each is read ONCE a frame into its consumer's frame decisions (the pass builder), never in a
// record body, so a frame's cull and draw cannot disagree about it (plan 4.7). Switching allocates nothing: the
// cluster tables and buffers exist whatever the knobs say.
namespace tessy
{
// N3: the VSM page render draws every caster bucket whose mesh has clusters cluster by cluster
// (tessy_vsm_cull_cs.hlsl + shadow_indirect_csm.hlsl VSM_TESSY) instead of the whole mesh into each page.
// `--set tessy.vsm:0|1`, Developer Controls > VSM, graphics_settings.json vsm.tessy.
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
// `--set tessy.raster:0|1`, MCP, Developer Controls > VSM, graphics_settings.json vsm.tessyRaster.
inline bool g_vsmMeshShader = true;
// The mesh shader's own triangle culls (tessy_common.hlsli kTessyMsCull*): bit 0 = a triangle whose screen box holds no
// pixel centre (it would raster nothing -- exact). 0 = only the page-border cull. `--set tessy.msCull:0|1`, transient.
inline unsigned g_vsmMsCull = 1u;
// N4: the LOCAL lights' atlas views (point faces, spots) draw the casters whose meshes have clusters cluster by cluster
// (tessy_atlas_cull_cs.hlsl + shadow_indirect_csm.hlsl TESSY_ATLAS_MS). Atlas modes (Legacy / SDSM) only; needs a mesh
// shader tier. `--set tessy.atlas:0|1`, MCP, Developer Controls > VSM, graphics_settings.json vsm.tessyAtlas.
inline bool g_atlas = true;
} // namespace tessy
