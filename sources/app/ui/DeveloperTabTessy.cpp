// R10.4: Developer Controls > Tessy (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"
#include "app/ui/DeveloperTabs.h" // GRAPHICS_CONTROL

#include <string>

#include "app/GraphicsSettings.h"
#include "app/scene/Scene.h"
#include "imgui.h"
#include "rendering/meshes/LodSelect.h"
#include "rendering/renderables/InstanceTypes.h"
#include "rendering/shadows/VirtualShadowMap.h"
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/shadows/ShadowGpuData.h" // Tessy N3: the cluster counter
#include "rendering/shadows/TessySettings.h"
#include "rendering/core/RasterSettings.h" // A6: the bindless checkbox

void DeveloperWindow::DrawTessyTab(Scene& scene)
{
    // Tessy (docs/tessy_plan.md): the master switch, the paths, the cluster view, the counters.
    ImGui::TextWrapped("Tessy -- our cluster geometry (UE Nanite's analog): meshes cut into clusters of <= 128 "
        "triangles, culled and drawn cluster by cluster: the VSM pages, the lamps' atlas views and, with "
        "\"Camera G-buffer\", the camera itself (plan step N5).");
    GRAPHICS_CONTROL(TessyEnabled, "tessyEnabled",
        ImGui::Checkbox("Tessy (master switch)", &tessy::g_enabled_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("OFF: no Tessy path runs anywhere -- the VSM pages and the lamps' atlases draw\n"
                          "whole meshes, the cluster view is off -- whatever the switches below say (they\n"
                          "keep their values). The cluster tables stay loaded, so it switches instantly\n"
                          "both ways. --set=tessy.enabled:0|1");
    ImGui::BeginDisabled(!tessy::g_enabled_game);
    GRAPHICS_CONTROL(TessyVsm, "tessyVsm",
        ImGui::Checkbox("Tessy: draw casters cluster by cluster", &tessy::g_vsm_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ON: a (page, caster) pair whose mesh has clusters (a .mesh.bin v4) is culled\n"
                          "cluster by cluster against the page (each cluster's box padded by how far this\n"
                          "frame's wind can move it) and only the clusters that touch the page are drawn;\n"
                          "a mesh without clusters stays on the whole-mesh draw in the same frame.\n"
                          "OFF: every caster drawn whole into every page its box touches.\n"
                          "The depth is the same either way. --set=tessy.vsm:0|1");
    GRAPHICS_CONTROL(TessyRaster, "tessyRaster",
        ImGui::Checkbox("Tessy: mesh shader raster", &tessy::g_vsmMeshShader_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ON: the surviving clusters go through a mesh shader -- each meshlet vertex\n"
                          "transformed once, a triangle wholly outside its page culled before setup.\n"
                          "OFF: the VS path, DrawInstanced(384, clusters) -- every corner of every\n"
                          "triangle. Without a mesh shader tier ON draws through the VS.\n"
                          "The depth is the same either way. --set=tessy.raster:0|1");
    GRAPHICS_CONTROL(TessyAtlas, "tessyAtlas",
        ImGui::Checkbox("Tessy: lamp atlases cluster by cluster", &tessy::g_atlas_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ON (Legacy / SDSM): a point light's cube face or a spot draws the casters whose\n"
                          "meshes have clusters cluster by cluster -- each cluster's box (padded for the\n"
                          "wind) against the view's frustum, the survivors through a mesh shader.\n"
                          "OFF: every such caster drawn whole into every view it touches.\n"
                          "Needs a mesh shader tier. The depth is the same either way. --set=tessy.atlas:0|1");
    GRAPHICS_CONTROL(TessySdsm, "tessySdsm",
        ImGui::Checkbox("Tessy: SDSM sun partitions cluster by cluster", &tessy::g_sdsm_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ON (SDSM): each sun partition draws the casters whose meshes have clusters cluster\n"
                          "by cluster -- each cluster's box (padded for the wind) against the partition's cull\n"
                          "volume and box (the analyze's 16 planes), the survivors through the lamp atlases'\n"
                          "mesh shader. Pass B of the light-space HZB cull stays classic.\n"
                          "OFF: every such caster drawn whole into every partition it touches.\n"
                          "Needs a mesh shader tier. The depth is the same either way. --set=tessy.sdsm:0|1");
    ImGui::Text("SDSM partitions: %s", scene.ShadowGpu().TessySunVerdict());
    GRAPHICS_CONTROL(TessyGBuffer, "tessyGBuffer",
        ImGui::Checkbox("Tessy: camera G-buffer cluster by cluster", &tessy::g_gbuffer_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ON: the camera's indirect G-buffer draws every caster whose mesh has clusters cluster\n"
                          "by cluster -- each cluster's box (padded for the wind) against the camera frustum,\n"
                          "the survivors through a mesh shader twin of their material's PSO (a triangle that\n"
                          "holds no pixel centre culled before setup). Pass B of the HZB cull, the table PSOs\n"
                          "and the validation frame stay classic. Needs a mesh shader tier.\n"
                          "The picture is the same either way. --set=tessy.gbuffer:0|1");
    ImGui::Text("camera G-buffer: %s (%u of %u PSO runs have a mesh shader twin)", scene.ShadowGpu().TessyGBufferVerdict(),
                scene.ShadowGpu().TessyGBufferRuns(), scene.ShadowGpu().TessyGBufferRunsTotal());
    ImGui::Indent();
    ImGui::BeginDisabled(!tessy::g_gbuffer_game);
    ImGui::Checkbox("Clusters against the previous HZB (two-pass)", &tessy::g_gbufferHzb_game);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE Nanite's two-pass occlusion per CLUSTER: a cluster last frame's pyramid hid waits\n"
                          "for pass B, is retested against this frame's pyramid and drawn there if visible. Off\n"
                          "by default: on wind_test the instance HZB already culls what it could, and the 1.5-5 %%\n"
                          "of clusters it defers save less than the test and pass B cost. For dense assets.\n"
                          "Needs the camera's two-pass HZB. Not saved. --set=tessy.gbufferHzb:0|1");
    ImGui::EndDisabled();
    ImGui::Unindent();
    GRAPHICS_CONTROL(TessyLod, "tessyLod",
        ImGui::Checkbox("Tessy: cluster DAG LOD (camera, lamp atlases, VSM pages)", &tessy::g_lod_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("N7: the assets with a cluster DAG (baked into every .mesh.bin v5; a v4 file builds one\n"
                          "at load for mesh.json \"tessyLod\" or --tessy-lod-build) draw the DAG's cut --\n"
                          "continuous LOD instead of the discrete tiers, each caster its own.\n"
                          "Camera: a cluster whose error spans at most \"DAG pixel error\" pixels at the nearest\n"
                          "point of its sphere while its parent's spans more (at 1, where a discrete LOD of that\n"
                          "deviation switches); it turns the camera G-buffer's clusters on for itself.\n"
                          "Shadows (the lamps' atlases, every VSM page): the same against the view's texels x\n"
                          "tessy.lodShadowTexels (a switch flushes the page cache and the lamps' static cache).\n"
                          "--set=tessy.lod:0|1, tessy.lodShadowTexels:<x>, tessy.lodBand:0|1 (the camera's level band)");
    ImGui::SameLine();
    ImGui::TextDisabled("(%u groups, %u clusters)", scene.ShadowGpu().TessyDagGroups(), scene.ShadowGpu().TessyDagClusters());
    ImGui::Indent();
    ImGui::SetNextItemWidth(220.0f);
    GRAPHICS_CONTROL(TessyLodPixelError, "tessyLodPixelError",
        ImGui::SliderFloat("DAG pixel error", &tessy::g_lodPixelError_game, 0.25f, 16.0f, "%.2f px",
                           ImGuiSliderFlags_Logarithmic));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The camera's DAG cut: how many pixels of this screen a cluster's simplification error may\n"
                          "span before its parent takes over. Larger = the DAG's clusters coarsen sooner (fewer\n"
                          "triangles), smaller = later. 1 = the discrete LODs' rule (LOD tab \"Pixel error\", which\n"
                          "this does not touch). The SDSM partitions' camera cut follows it; the lamps' atlases and\n"
                          "the VSM pages keep tessy.lodShadowTexels. --set=tessy.lodPixelError:<px>");
    ImGui::Unindent();
    {
        int view = static_cast<int>(tessy::g_debugView_game);
        const char* const views[] = { "Off", "Clusters", "Meshlets", "Levels" };
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Cluster view", &view, views, 4)) { tessy::g_debugView_game = static_cast<unsigned>(view); }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Every caster the camera draws, over the lit frame, one colour per cluster (or per\n"
                              "meshlet: a cluster cut into runs of <= 64 vertices for the mesh shader). From the\n"
                              "camera's own cull row: the geometry and the LODs the G-buffer drew. While it is\n"
                              "on the camera's two-pass HZB cull stays off. Needs the indirect G-buffer and a\n"
                              "mesh shader tier. Not saved. --set=tessy.debug:0|1|2|3\n"
                              "Levels: a cluster of the DAG's cut by its DAG level -- green = the leaves (LOD0's\n"
                              "detail), yellow, red, magenta = the coarsest -- and a discrete LOD's cluster by its\n"
                              "tier, light (LOD0) to dark blue (LOD3). With tessy.lod on, the cut moves as you fly.");
        ImGui::Text("cluster view: %s", scene.ShadowGpu().TessyViewVerdict());
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    {
        const ShadowGpuData& sg = scene.ShadowGpu();
        const auto& ts = scene.Vsm().GetTessyStats();
        ImGui::Text("tessy: clusters for %u of %u caster groups (%u clusters, %llu KB)",
                    sg.TessyGroupsWithClusters(), sg.TessyGroupsTotal(), sg.TessyClusterCount(),
                    static_cast<unsigned long long>((sg.TessyBytes() + 1023u) / 1024u));
        if (tessy::g_vsm_game && scene.Vsm().TessyOffReason()[0] != '\0')
        {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "tessy: classic path -- %s", scene.Vsm().TessyOffReason());
        }
        const auto& as = sg.TessyAtlasStats();
        ImGui::Text("meshlet records: atlases %u + static %u, camera %u (cap %u a row set)",
                    as[0], as[1], as[2], ShadowGpuData::TessyAtlasRowSetCap());
        ImGui::Text("%u views did not fit; %u clusters tested; %u camera clusters deferred to pass B (HZB) "
                    "(frame N-3, the last frame a cluster row ran)", as[3], as[4], as[5]);
        ImGui::Text("camera pass A: %.3f M triangles, %u DAG clusters (the DAG's cut %s)", as[6] * 1e-6, as[7],
                    sg.TessyCameraDagThisFrame() ? "on" : "off");
        ImGui::Text("SDSM partitions: %u meshlet records, %.3f M triangles, %u clusters tested, %u runs did not fit",
                    as[8], as[11] * 1e-6, as[10], as[9]);
        ImGui::TextDisabled("VSM pages (when the VSM is the shadow mode):");
        if (ts.valid)
        {
            ImGui::Text("pairs drawn whole %u (%.2f M tris); to clusters %u (%.2f M tris whole)",
                        ts.wholePairs, ts.wholeTris * 1e-6, ts.tessyPairs, ts.tessyWholeTris * 1e-6);
            ImGui::Text("clusters: %u tested, %u drawn (%.2f M tris) as %u %s; runs %u (frame N-3)",
                        ts.clustersTested, ts.clusters, ts.clusterTris * 1e-6, ts.records,
                        ts.tessyMs ? "meshlets (mesh shader)" : "instances (VS)", ts.runs);
            if (ts.clustersLost > 0u || ts.runsLost > 0u)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "lost: %u clusters (shadow holes), %u runs drawn whole",
                                   ts.clustersLost, ts.runsLost);
            }
        }
    }
}

#endif // !TC_SHIPPING
