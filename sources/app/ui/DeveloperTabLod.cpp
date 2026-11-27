// R10.4: Developer Controls > LOD (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"
#include "app/ui/DeveloperTabs.h" // GRAPHICS_CONTROL

#include <string>

#include "app/GraphicsSettings.h"
#include "app/scene/Scene.h"
#include "imgui.h"
#include "rendering/core/Renderer.h"
#include "rendering/meshes/LodSelect.h"
#include "rendering/debug/LodDebugView.h"
#include "rendering/shadows/ShadowSettings.h"

void DeveloperWindow::DrawLodTab(Renderer& renderer, Scene& scene, SceneRenderSettings& settings,
    GraphicsSettingsManager& graphicsSettings)
{
    // Every mesh-LOD SELECTION control in one place (shadow-specific LOD lives with the
    // shadows: the per-view curve bias stays on the VSM tab). All of these apply live.
    ImGui::TextWrapped("Mesh LOD selection. Regular meshes pick a tier from "
        "distance / instance radius (object-size-relative: a palm switches later than a "
        "pebble); chunked terrain picks a tier PER CHUNK in metres, and its shadow "
        "casters always match the drawn LOD by construction.");
    ImGui::Separator();

    if (ImGui::Button("Reset LOD quality to defaults"))
    {
        graphicsSettings.ResetLod(renderer, scene, settings);
    }

#if WITH_EDITOR
    GRAPHICS_CONTROL(LodEnabled, "lodEnabled",
        ImGui::Checkbox("Mesh LOD [F10]", &render::g_lodEnabled_game));
#else
    GRAPHICS_CONTROL(LodEnabled, "lodEnabled",
        ImGui::Checkbox("Mesh LOD", &render::g_lodEnabled_game));
#endif
    ImGui::BeginDisabled(!render::g_lodEnabled_game);
    // -1 = automatic (screen-size / cascade); 0..3 force that level on every mesh.
    static const char* kForcedLodLabels[] = { "Auto", "0 (full)", "1", "2", "3" };
    int forcedLodCombo = render::g_forcedLod_game + 1; // map -1..3 -> 0..4
    if (ImGui::Combo("Force LOD level", &forcedLodCombo, kForcedLodLabels, IM_ARRAYSIZE(kForcedLodLabels)))
    {
        render::g_forcedLod_game = forcedLodCombo - 1;
    }

    ImGui::SeparatorText("Regular meshes (UE screen size)");
    GRAPHICS_CONTROL(LodUeScreenSize, "lodUeScreenSize",
        ImGui::Checkbox("UE screen size, per mesh", &render::g_lodUeScreenSize_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE's static-mesh LOD: a LOD is drawn while the object's screen size\n"
                          "(bounding-sphere diameter / screen height) is at most that LOD's screen\n"
                          "size, estimated per mesh from its measured deviation (UE's automatic screen\n"
                          "size: the deviation spans 'Pixel error' pixels of a 1920x1080, 90-degree\n"
                          "screen) or authored in mesh.json \"lodScreenSize\". Off = the global\n"
                          "distance/radius ratios below.");
    ImGui::BeginDisabled(!render::g_lodUeScreenSize_game);
    GRAPHICS_CONTROL(LodPixelError, "lodPixelError",
        ImGui::SliderFloat("Pixel error", &render::g_lodPixelError_game, 0.25f, 32.0f, "%.2f px"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE's PixelError (default 8): how many pixels of a 1920x1080, 90-degree screen\n"
                          "a LOD's measured deviation may span where it switches in -- a visible pop by\n"
                          "design, as in UE. Higher = coarser sooner. Per asset: mesh.json \"lodGroup\"\n"
                          "(UE's groups: SmallProp / LargeProp / Deco 10, LevelArchitecture 12,\n"
                          "HighDetail 6) or \"lodPixelError\" (the bathroom's 1: seen at arm's length).");
    GRAPHICS_CONTROL(LodDistanceScale, "lodDistanceScale",
        ImGui::SliderFloat("LOD distance scale", &render::g_lodDistanceScale_game, 0.1f, 8.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE r.StaticMeshLODDistanceScale (default 1): multiplies every LOD's screen\n"
                          "size, so ABOVE 1 SWITCHES EARLIER (2 = half the distance) -- the opposite\n"
                          "direction to the per-asset mesh.json \"lodDistanceScale\", which multiplies\n"
                          "the object's radius (UE's per-component factor) and keeps detail longer.");
    ImGui::EndDisabled();

    ImGui::SeparatorText("Regular meshes (distance / radius)");
    ImGui::BeginDisabled(render::g_lodUeScreenSize_game);
    GRAPHICS_CONTROL(LodBound0, "lodBound0",
        ImGui::SliderFloat("LOD1 at ratio", &render::g_lodBound0_game,
                           2.0f, 60.0f, "%.0f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("distance / instance radius where LOD0 steps to LOD1. A 2 m-radius palm\n"
                          "at ratio 8 switches at 16 m. UE screen size = 1.778 / ratio at 90 deg hfov\n"
                          "16:9 (8 = 0.22). Each boundary is forced at least 5%% past the previous one;\n"
                          "with the LOD dither time > 0 a switch is a temporal dissolve, else +/-15%%\n"
                          "hysteresis. Foliage takes these in UE mode too (no automatic estimate).");
    GRAPHICS_CONTROL(LodBound1, "lodBound1",
        ImGui::SliderFloat("LOD2 at ratio", &render::g_lodBound1_game,
                           4.0f, 120.0f, "%.0f"));
    GRAPHICS_CONTROL(LodBound2, "lodBound2",
        ImGui::SliderFloat("LOD3 at ratio", &render::g_lodBound2_game,
                           8.0f, 240.0f, "%.0f"));
    ImGui::EndDisabled();
    GRAPHICS_CONTROL(LodTemporalLag, "lodTemporalLag",
        ImGui::SliderFloat("LOD dither time", &render::g_lodTemporalLag_game,
                           0.0f, 2.0f, "%.2f s"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE's temporal LOD (lod.TemporalLag, default 0.5 s): the LOD is selected from\n"
                          "two camera positions this far apart in time, and when they disagree BOTH\n"
                          "levels draw with complementary screen-door masks while the handover runs\n"
                          "0 -> 1 over this time - a dissolve instead of a pop, resolved by DLSS/TAA.\n"
                          "A standing camera dithers nothing. 0 = hard switches with +/-15%% hysteresis.");

    ImGui::SeparatorText("Chunked terrain (metres, per chunk)");
    GRAPHICS_CONTROL(ChunkLodDistance, "chunkLodDistance",
        ImGui::SliderFloat("Chunk LOD distance (m)",
                           &render::g_chunkLodDist0_game,
                           24.0f, 400.0f, "%.0f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A terrain chunk closer than this (closest point of its box) draws AND\n"
                          "casts LOD0; each 'factor' further steps one LOD coarser. The caster always\n"
                          "matches the drawn LOD per chunk, so no setting here can cause the terrain\n"
                          "self-shadow banding/phantom family -- this knob trades triangles for pop-in\n"
                          "distance only.");
    GRAPHICS_CONTROL(ChunkLodFactor, "chunkLodFactor",
        ImGui::SliderFloat("Chunk LOD factor", &render::g_chunkLodDistFactor_game,
                           1.2f, 4.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Distance multiplier between LOD steps: LOD2 starts at distance*factor,\n"
                          "LOD3 at distance*factor^2. +/-15%% hysteresis keeps a boundary chunk from\n"
                          "flipping while the camera breathes.");
    ImGui::EndDisabled();

    ImGui::SeparatorText("Debug view");
    ImGui::TextWrapped("Draws the LOD DECISION next to the INPUTS it was made from, so "
        "a wrong-looking LOD can be pinned on the curve or on the geometry without a "
        "rebuild. Every number shown is read back from what selection already stored "
        "this frame, never re-derived.");
    static const char* kLodDebugLabels[] = {
        "Off", "Tier (selected LOD)", "Density (apparent triangle size)" };
    int lodDebugCombo = static_cast<int>(render::g_lodDebugMode_game);
    if (ImGui::Combo("LOD debug view", &lodDebugCombo, kLodDebugLabels, IM_ARRAYSIZE(kLodDebugLabels)))
    {
        render::g_lodDebugMode_game = static_cast<render::LodDebugMode>(lodDebugCombo);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Tier: colour = the LOD each chunk selected (green 0 -> red 3).\n"
                          "Density: colour = the triangle size that LOD actually DELIVERS on\n"
                          "screen. The two disagree whenever chunks differ in source density --\n"
                          "simplification is a fixed ratio of each chunk's own LOD0 triangle\n"
                          "count, so an equal tier is not equal detail. That gap is exactly why\n"
                          "a nearer chunk can look coarser than a farther one.");

    ImGui::BeginDisabled(render::g_lodDebugMode_game == render::LodDebugMode::Off);
#if WITH_EDITOR
    // Only offered where a selection can exist; in a non-editor build there is nothing
    // to select, so the control is absent rather than present and inert.
    static const char* kLodDebugFilterLabels[] = { "Whole level", "Selection only" };
    int lodDebugFilterCombo = static_cast<int>(render::g_lodDebugFilter_game);
    if (ImGui::Combo("Report on", &lodDebugFilterCombo, kLodDebugFilterLabels, IM_ARRAYSIZE(kLodDebugFilterLabels)))
    {
        render::g_lodDebugFilter_game = static_cast<render::LodDebugFilter>(lodDebugFilterCombo);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Selection only narrows the view to the objects picked in the viewport\n"
                          "(all chunks of a selected chunked mesh included, and never range-culled).\n"
                          "It does not quietly widen back to the whole level when nothing is\n"
                          "selected -- the readout says the selection is empty instead.");
#endif
    ImGui::Checkbox("Boxes", &render::g_lodDebugBoxes_game);
    ImGui::SameLine();
    ImGui::Checkbox("Labels", &render::g_lodDebugLabels_game);
    ImGui::SameLine();
    ImGui::Checkbox("Criteria", &render::g_lodDebugCriteria_game);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Draws the boundaries as what they geometrically ARE: spheres around\n"
                          "the camera, sliced at sea level. It is why height alone can put every\n"
                          "chunk past a boundary. Also probes the chunk under the crosshair --\n"
                          "white box, plus the exact closest-point segment its distance was\n"
                          "measured along.");
    ImGui::Checkbox("Include regular meshes", &render::g_lodDebugRegularMeshes_game);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Palms and props select on distance/RADIUS (the ratio sliders above),\n"
                          "not on metres. Off by default so the two curves stay separable.");
    ImGui::SliderFloat("Debug range (m)", &render::g_lodDebugRange_game, 40.0f, 800.0f, "%.0f");
    ImGui::SliderInt("Max boxes", &render::g_lodDebugMaxBoxes_game, 0, 800);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A populated level has hundreds of meshes and every box is 12 lines,\n"
                          "which buries the terrain the view is usually there to judge. Chunks\n"
                          "are drawn first, then the nearest regular meshes fill what is left.\n"
                          "The readout always counts everything in range, so this hides\n"
                          "geometry, never numbers. 0 = no limit.");
    ImGui::EndDisabled();

}

#endif // !TC_SHIPPING
