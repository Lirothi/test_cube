// R10.4: Developer Controls > Visibility (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"
#include "app/ui/DeveloperTabs.h" // GRAPHICS_CONTROL

#include <algorithm>
#include <string>

#include "app/GraphicsSettings.h"
#include "app/scene/Scene.h"
#include "imgui.h"
#include "rendering/core/Renderer.h"
#include "rendering/streaming/StreamingSettings.h" // Streaming tab (A3)
#include "rendering/streaming/TextureStreaming.h"
#include "rendering/core/VisibilityStats.h" // S0 occlusion plan: per-view visibility table
#include "rendering/visibility/OcclusionHistory.h" // S3a: vis::g_occlusion_game
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/shadows/ShadowGpuData.h" // Tessy N3: the cluster counter
#include "rendering/core/RasterSettings.h" // A6: the bindless checkbox

void DeveloperWindow::DrawVisibilityTab(Renderer& renderer, Scene& scene, SceneRenderSettings& settings,
    GraphicsSettingsManager& graphicsSettings)
{
    // S0 (docs/occlusion_culling_plan.md): per-view visibility. Same numbers a headless
    // run gets from --vis-readout, so a HUD reading and a log reading never disagree.
    if (ImGui::BeginTable("VisibilityStats", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("view");
        ImGui::TableSetupColumn("objects in");
        ImGui::TableSetupColumn("frustum");
        ImGui::TableSetupColumn("occluded");
        ImGui::TableSetupColumn("chunks in");
        ImGui::TableSetupColumn("chunks drawn");
        ImGui::TableSetupColumn("instances");
        ImGui::TableSetupColumn("tris (M)");
        ImGui::TableHeadersRow();
        static const char* kViewNames[render::kVisibilityViews] = { "camera", "c0", "c1", "c2", "c3" };
        for (unsigned v = 0; v < render::kVisibilityViews; ++v)
        {
            const render::VisibilityViewCounters& c = render::g_visibilityStats.last[v];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%s", kViewNames[v]);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%u", c.objectsIn);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%u", c.objectsFrustum);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%u", c.objectsOccluded);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%u", c.chunksIn);
            ImGui::TableSetColumnIndex(5); ImGui::Text("%u", c.chunksDrawn);
            ImGui::TableSetColumnIndex(6); ImGui::Text("%u", c.instancesDrawn);
            ImGui::TableSetColumnIndex(7); ImGui::Text("%.3f", static_cast<double>(c.trianglesSubmitted) / 1.0e6);
        }
        ImGui::EndTable();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Per view, last completed frame. objects in = what the view's source offered;\n"
                          "frustum = kept by the frustum test (per object, before instancing);\n"
                          "occluded = cut by an occlusion test (0 until the occlusion plan's S3/S5);\n"
                          "chunks = terrain chunks of surviving chunked meshes, in vs kept by the\n"
                          "view's frustum (S1 mask); instances = GI instances the camera draws;\n"
                          "tris = CPU-path estimate at the selected LOD.\n"
                          "Headless: --vis-readout -> logs/visibility_readout.log.");
    // S1 rollback, live: with it off every chunk/instance of a passed object draws again.
    GRAPHICS_CONTROL(VisibilityChunkMask, "visibilityChunkMask",
        ImGui::Checkbox("Chunk / instance frustum mask (S1)", &render::g_visChunkMask_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Frustum test below the object level: terrain chunks (camera + Legacy CSM\n"
                          "CPU loop) and GPU-instanced cloud instances (camera). Off = pre-S1: an\n"
                          "object that passes draws all of its chunks. --set=vis.chunkMask:0");
    // S3a: the occlusion method, live, and what last frame asked and learned.
    {
        static const char* kMethods[] = { "off", "hardware queries", "hzb tester" };
        int method = std::clamp(vis::g_occlusion_game.method, 0, 2);
        if (GRAPHICS_CONTROL_WIDTH(OcclusionMethod, "occlusionMethod", 126.0f,
            ImGui::Combo("Occlusion", &method, kMethods, 3)))
        {
            vis::g_occlusion_game.method = method;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Camera-only occlusion culling against the G-buffer depth, with a history and a\n"
                              "one-frame-or-more latency. Hardware queries (S3a) = UE's default desktop path,\n"
                              "one query per box after the base pass. HZB tester (S3b) = UE's r.HZBOcclusion=1,\n"
                              "one compute dispatch of every box against the depth pyramid, binary verdicts.\n"
                              "Shadows never consult either. --set=vis.method:0|1|2, vis.queryLatency:1..3");
        ImGui::SameLine();
        GRAPHICS_CONTROL_WIDTH(OcclusionQueryLatency, "occlusionQueryLatency", 56.0f,
            ImGui::SliderInt("latency", &vis::g_occlusion_game.queryLatency, 1,
                             static_cast<int>(vis::kOcclusionBufferedFrames)));
        // S6: ownership -- objects the GPU-driven G-buffer draws are occluded by its own
        // two-pass (S5) and leave this history; the checkbox restores the pre-S6 overlap.
        ImGui::SameLine();
        GRAPHICS_CONTROL(OcclusionIndirectQueries, "occlusionIndirectQueries",
            ImGui::Checkbox("also query GPU-driven objects", &vis::g_occlusion_game.indirectQueries));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("OFF (S6 rule): an object the GPU-driven G-buffer draws is occluded by the camera's\n"
                              "two-pass HZB (zero latency) and is not queried here at all -- the CPU path and the\n"
                              "lights keep their queries. ON = both systems test it (pre-S6). --set=vis.indirectQueries:0|1");
        const render::OcclusionFrameStats& os = render::g_visibilityStats.occlusionLast;
        ImGui::Text("queries/tests: %u individual, %u grouped (x16), %u dropped; %u results read, latency %u, %u entries, %u lights occluded%s",
                    os.queriesIndividual, os.queriesGrouped, os.queriesDropped, os.queriesTested,
                    os.latencyFrames, os.historyEntries, os.lightsOccluded,
                    os.ignoredResults ? " [results ignored: cut]" : "");
    }

    ImGui::SeparatorText("GPU-driven G-buffer");
    // Occlusion plan S4: the camera's G-buffer through the same registry.
    GRAPHICS_CONTROL(IndirectGBuffer, "indirectGBuffer",
        ImGui::Checkbox("GPU-driven G-buffer (ExecuteIndirect per group)",
                        &render::g_indirectGBufferEnabled_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The opaque G-buffer drawn from the shadow registry's camera cull: one\n"
                          "ExecuteIndirect per (mesh submesh, LOD), the group's material bound by the CPU.\n"
                          "Objects the registry did not take (non-casters, GI clouds, shader overrides,\n"
                          "per-object texture overrides on a shared mesh) keep the CPU path. Pixel parity\n"
                          "with the CPU path is the contract (measured 2026-09-04). --set=gbuffer.indirect:0|1");
    // Texture streaming A6: material textures by index in one shader-visible heap.
    GRAPHICS_CONTROL(RasterBindless, "rasterBindless",
        ImGui::Checkbox("Bindless material textures (SM6.6 ResourceDescriptorHeap)",
                        &render::g_rasterBindless_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("G-buffer and masked-shadow PSOs read albedo/MR/normal by heap index instead of a\n"
                          "per-draw descriptor table; the GPU-driven G-buffer then draws every group of one PSO\n"
                          "with a single ExecuteIndirect. Toggling rebuilds those PSOs in place (a hitch).\n"
                          "Same pixels either way. --set=raster.bindless:0|1");
    {
        const ShadowGpuData& sg = scene.ShadowGpu();
        ImGui::Text("indirect gbuffer: %s, %u eligible caster slots",
                    sg.GBufferIndirectThisFrame() ? "ON this frame" : "off",
                    sg.GBufferIndirectEligibleCasters());
    }
    // Occlusion plan S5: the camera's two-pass HZB occlusion inside that G-buffer.
    GRAPHICS_CONTROL(GbufferHzb, "gbufferHzb",
        ImGui::Checkbox("HZB occlusion in the GPU-driven G-buffer (two-pass)",
                        &render::g_gbufferHzbCullEnabled_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Nanite's main/post split on the indirect G-buffer: candidates last frame's depth\n"
                          "pyramid hid are deferred, pass A draws the rest, the pyramid of pass A's depth\n"
                          "retests the deferred with this frame's camera, pass B draws the survivors.\n"
                          "Zero latency, no holes by construction. --set=gbuffer.hzb:0|1");
    {
        const ShadowGpuData& sg = scene.ShadowGpu();
        ImGui::Text("camera hzb cull: %s; deferred %u, drawn in pass B %u (fading casters twice, frame N-3)",
                    sg.CamHzbThisFrame() ? "ON this frame" : "off", sg.CamHzbDeferred(), sg.CamHzbDrawnB());
    }

    ImGui::Separator();
    if (ImGui::Button("Reset visibility to defaults"))
    {
        graphicsSettings.ResetVisibility(renderer, scene, settings);
    }
}

#endif // !TC_SHIPPING
