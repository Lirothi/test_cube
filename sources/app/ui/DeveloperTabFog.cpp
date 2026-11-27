// R10.4: Developer Controls > Fog (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"
#include "app/ui/DeveloperTabs.h" // GRAPHICS_CONTROL

#include <string>

#include "app/GraphicsSettings.h"
#include "app/scene/Scene.h"
#include "imgui.h"
#include "rendering/core/Renderer.h"
#include "rendering/shadows/ShadowSettings.h"

void DeveloperWindow::DrawFogTab(Renderer& renderer, Scene& scene, SceneRenderSettings& settings,
    GraphicsSettingsManager& graphicsSettings)
{
    ImGui::TextWrapped("Volumetric fog GRID: project-wide quality, like UE's r.VolumetricFog.* cvars. The fog's "
        "look (density, scattering, history, local lights) is level data: Inspector > Environment > Volumetric fog.");
    {
        const UINT px = renderer.GetFogGridPixels();
        const UINT gw = (renderer.GetRenderWidth() + px - 1u) / px;
        const UINT gh = (renderer.GetRenderHeight() + px - 1u) / px;
        const UINT gz = renderer.GetFogGridZ();
        ImGui::Text("Froxel grid: %ux%ux%u (%.2f M cells)", gw, gh, gz,
                    static_cast<double>(gw) * gh * gz / 1.0e6);
    }
    // Volumetric fog grid (UE r.VolumetricFog.GridPixelSize 16 / GridSizeZ 64, global cvars). The
    // renderer recreates the ring at the next frame boundary when this changes.
    GRAPHICS_CONTROL(FogGridPixels, "fogGridPixels",
        ([&]() {
            static const char* kFogCells[] = { "4 px", "8 px", "16 px (UE default)", "32 px", "64 px" };
            static const unsigned kFogCellPx[] = { 4u, 8u, 16u, 32u, 64u };
            int idx = 2;
            for (int i = 0; i < 5; ++i) { if (render::g_fogGridPixels_game == kFogCellPx[i]) { idx = i; } }
            const bool changed = ImGui::Combo("Volumetric fog cell", &idx, kFogCells, 5);
            if (changed) { render::g_fogGridPixels_game = kFogCellPx[idx]; }
            return changed;
        })());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Froxel size in RENDER pixels (UE r.VolumetricFog.GridPixelSize, their 16): under DLSS\n"
                          "the cells are correspondingly larger on screen. Cost scales with the cell count.\n"
                          "--set=fog.gridPixels:4|8|16|32|64");
    GRAPHICS_CONTROL(FogGridZ, "fogGridZ",
        ([&]() {
            static const char* kFogSlices[] = { "16", "32", "48", "64 (UE default)", "96", "128" };
            static const unsigned kFogSliceN[] = { 16u, 32u, 48u, 64u, 96u, 128u };
            int idx = 3;
            for (int i = 0; i < 6; ++i) { if (render::g_fogGridZ_game == kFogSliceN[i]) { idx = i; } }
            const bool changed = ImGui::Combo("Volumetric fog slices", &idx, kFogSlices, 6);
            if (changed) { render::g_fogGridZ_game = kFogSliceN[idx]; }
            return changed;
        })());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Depth slices of the froxel volume (UE r.VolumetricFog.GridSizeZ, their 64), spread by the\n"
                          "log distribution from the volume's near to its far plane. --set=fog.gridZ:16..128");

    ImGui::Separator();
    if (ImGui::Button("Reset fog grid to defaults"))
    {
        graphicsSettings.ResetFog(renderer, scene, settings);
    }
}

#endif // !TC_SHIPPING
