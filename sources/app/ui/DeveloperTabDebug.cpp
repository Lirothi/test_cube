// R10.4: Developer Controls > Debug (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "core/vfs/Vfs.h"
#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "imgui.h"
#include "rendering/core/Renderer.h"
#include "rendering/shadows/ShadowSettings.h"
#include "ocean/OceanSimulation.h"
#include "text/TextManager.h"
#if WITH_EDITOR
#include "editor/EditorController.h"
#endif

namespace
{
    std::string NormalizeLevelPath(std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        return path;
    }

    std::string LevelPathString(const std::filesystem::path& path)
    {
        return NormalizeLevelPath(path.string());
    }

    template <size_t N>
    void SetTextBuffer(char (&buffer)[N], const std::string& text)
    {
        const std::string normalized = NormalizeLevelPath(text);
        std::snprintf(buffer, N, "%s", normalized.c_str());
    }

    bool LevelFileExists(const std::string& path)
    {
        const std::string normalizedPath = NormalizeLevelPath(path);
        if (normalizedPath.empty())
        {
            return false;
        }

        return vfs::IsFile(normalizedPath);
    }
}

void DeveloperWindow::RefreshLevelList()
{
    availableLevelPaths_.clear();
    levelChangeStatus_.clear();
    levelListScanned_ = true;

    if (!vfs::IsDirectory("data/levels"))
    {
        levelChangeStatus_ = "Cannot scan data/levels";
        return;
    }
    vfs::List("data/levels", "*.json", [&](const vfs::ListEntry& entry)
    {
        if (!entry.directory)
        {
            availableLevelPaths_.push_back(LevelPathString(std::filesystem::path(vfs::Widen(entry.path))));
        }
    });

    std::sort(availableLevelPaths_.begin(), availableLevelPaths_.end());
    if (availableLevelPaths_.empty())
    {
        levelChangeStatus_ = "No JSON levels found in data/levels";
    }
}

void DeveloperWindow::DrawDebugTab(Renderer& renderer, Scene& scene, LevelManager& levelManager, SceneRenderSettings& settings
#if WITH_EDITOR
    , EditorController& editorController
#endif
)
{
    const std::string_view activeLevelName = levelManager.GetActiveLevelName();
    ImGui::Text("Active level: %.*s", static_cast<int>(activeLevelName.size()), activeLevelName.data());
    const std::string_view activeLevelPath = levelManager.GetActiveLevelSourcePath();
    if (!activeLevelPath.empty())
    {
        ImGui::Text("Active path: %.*s", static_cast<int>(activeLevelPath.size()), activeLevelPath.data());
    }

    if (!levelListScanned_)
    {
        RefreshLevelList();
    }

    const auto requestLevelPathChange = [&](const std::string& path)
    {
        const std::string normalizedPath = NormalizeLevelPath(path);
        if (normalizedPath.empty())
        {
            levelChangeStatus_ = "Level path is empty";
            return;
        }
        if (!LevelFileExists(normalizedPath))
        {
            levelChangeStatus_ = "Level file not found: " + normalizedPath;
            return;
        }

        SetTextBuffer(levelPathBuffer_, normalizedPath);
#if WITH_EDITOR
        if (editorController.RequestOpenLevelPath(levelManager, normalizedPath, preserveCameraOnLevelChange_))
        {
            levelChangeStatus_ = "Queued " + normalizedPath;
        }
        else
        {
            levelChangeStatus_ = "Could not queue " + normalizedPath;
        }
#else
        LevelLoadOptions loadOptions;
        loadOptions.preserveCameraTransform = preserveCameraOnLevelChange_;
        levelManager.RequestLevelPathChange(normalizedPath, loadOptions);
        levelChangeStatus_ = "Queued " + normalizedPath;
#endif
    };

    const char* preview = levelPathBuffer_[0] ? levelPathBuffer_ : "Select level";
    if (ImGui::BeginCombo("JSON level", preview))
    {
        for (const std::string& levelPath : availableLevelPaths_)
        {
            const bool selected = NormalizeLevelPath(levelPathBuffer_) == levelPath;
            if (ImGui::Selectable(levelPath.c_str(), selected))
            {
                SetTextBuffer(levelPathBuffer_, levelPath);
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh levels"))
    {
        RefreshLevelList();
    }
    if (ImGui::Button("Load JSON Level"))
    {
        requestLevelPathChange(levelPathBuffer_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload JSON Level"))
    {
        requestLevelPathChange(levelPathBuffer_);
    }
    ImGui::Checkbox("Preserve camera transform", &preserveCameraOnLevelChange_);
    if (!levelChangeStatus_.empty())
    {
        ImGui::TextDisabled("%s", levelChangeStatus_.c_str());
    }

    ImGui::Separator();

    bool wireframe = renderer.GetWireframeMode();
    if (ImGui::Checkbox("Wireframe", &wireframe))
    {
        renderer.SetWireframeMode(wireframe);
    }
    bool textureViewerOpen = textureDebugViewer_.IsOpen();
    // Render-resolution targets are re-sampled at a different sub-pixel offset every
    // frame, so a half-res intermediate (GTAO, SSR) shimmers while you stare at it.
    // This holds the grid still. DLSS needs that jitter, so its own output loses its
    // anti-aliasing while this is on — hence the warning rather than a silent toggle.
    {
        bool jitterPaused = renderer.IsJitterPaused();
        ImGui::BeginDisabled(!renderer.IsDlssActive());
        if (ImGui::Checkbox("Pause DLSS jitter (steady preview)", &jitterPaused))
        {
            renderer.SetJitterPaused(jitterPaused);
        }
        ImGui::EndDisabled();
        if (jitterPaused && renderer.IsDlssActive())
        {
            ImGui::TextDisabled("  DLSS is reconstructing without jitter: expect aliasing.");
        }
    }
    if (ImGui::Checkbox("Texture inspector [F4]", &textureViewerOpen))
    {
        textureDebugViewer_.SetOpen(textureViewerOpen);
    }
    bool oceanControlsOpen = oceanControlsWindow_.IsOpen();
    if (ImGui::Checkbox("Ocean controls [F7]", &oceanControlsOpen))
    {
        oceanControlsWindow_.SetOpen(oceanControlsOpen);
    }
    bool logWindowOpen = logWindow_.IsOpen();
    if (ImGui::Checkbox("Session log", &logWindowOpen))
    {
        logWindow_.SetOpen(logWindowOpen);
    }
#if WITH_EDITOR
    bool levelEditorOpen = editorController.IsOpen();
    if (ImGui::Checkbox("Level Editor [F2]", &levelEditorOpen))
    {
        editorController.SetOpen(levelEditorOpen);
    }
#endif
    // Mirrors SceneRenderer::PickDebugTexTarget — keep the order identical.
    static const char* kDebugTexLabels[] = {
        "Cascade shadow atlas", "GTAO raw", "GTAO denoised", "GTAO temporal",
        "GTAO upsampled", "HZB furthest (AO + UE SSR)", "Scene depth", "HZB closest (debug/P9)",
        "SSR hit mask (alpha)" };

    ImGui::SeparatorText("Fullscreen debug view");
    // Picking a target TURNS THE VIEW ON. Two separate controls where one is useless
    // without the other is not two controls, it is one control and a trap: the combo
    // silently did nothing until the checkbox above it was found.
    if (ImGui::Combo("Show", &settings.debugTexTarget, kDebugTexLabels,
                     IM_ARRAYSIZE(kDebugTexLabels)))
    {
        settings.debugTexMode = true;
    }
    DevHelp("Replaces the whole frame with the chosen render target. Picking one here "
            "switches the view on; use Off below to get the scene back.");

    ImGui::BeginDisabled(settings.debugTexTarget != 5 && settings.debugTexTarget != 7);
    ImGui::SliderInt("HZB mip", &settings.debugTexMip, 0, 12);
    ImGui::EndDisabled();
    DevHelp("Which level of the depth pyramid to show. Mip 0 is half the render "
            "resolution, and each level halves again. Depth-like targets go through a "
            "monotone pow() stretch, or reversed-Z would blit as a black rectangle. "
            "Furthest is what the AO horizon search reads; Closest is what the HiZ "
            "reflection march reads, and it is only built while that technique is the "
            "active reflection source.");

    if (settings.debugTexMode)
    {
        if (ImGui::Button("Off (show the scene)"))
        {
            settings.debugTexMode = false;
        }
        ImGui::SameLine();
        // Say what is actually on screen, including the grid size -- "GTAO raw" alone
        // does not tell you that you are looking at a half-resolution target.
        const unsigned halfW = (renderer.GetRenderWidth() + 1u) / 2u;
        const unsigned halfH = (renderer.GetRenderHeight() + 1u) / 2u;
        switch (settings.debugTexTarget)
        {
        case 1: case 2: case 3:
            ImGui::Text("showing %s  (%ux%u, half res)",
                        kDebugTexLabels[settings.debugTexTarget], halfW, halfH);
            break;
        case 4: case 6:
            ImGui::Text("showing %s  (%ux%u, render res)",
                        kDebugTexLabels[settings.debugTexTarget],
                        renderer.GetRenderWidth(), renderer.GetRenderHeight());
            break;
        case 8:
            ImGui::Text("showing %s  (%ux%u)  white = the ray hit something",
                        kDebugTexLabels[settings.debugTexTarget],
                        renderer.GetReflectionTextureWidth(),
                        renderer.GetReflectionTextureHeight());
            break;
        case 5: case 7:
            ImGui::Text("showing %s mip %d  (%ux%u)",
                        settings.debugTexTarget == 5 ? "furthest" : "closest",
                        settings.debugTexMip,
                        std::max(1u, halfW >> settings.debugTexMip),
                        std::max(1u, halfH >> settings.debugTexMip));
            break;
        default:
            ImGui::Text("showing %s", kDebugTexLabels[settings.debugTexTarget]);
            break;
        }

        // The AO targets are only written while the pass runs. Without this the view is
        // whatever was last left in the texture, which reads as "the feature is broken".
        if (settings.debugTexTarget >= 1 && settings.debugTexTarget <= 4 &&
            !scene.GetGtao().enabled)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                "GTAO is OFF in the level postProcess settings - this target is stale, not empty.");
        }
    }
    ImGui::Checkbox("Profiler overlay", &settings.showProfiler);
#if WITH_EDITOR
    ImGui::Checkbox("GPU instancing [F12]", &render::g_instancingEnabled_game);
#else
    ImGui::Checkbox("GPU instancing", &render::g_instancingEnabled_game);
#endif

    // (Mesh LOD enable/force + the selection boundaries moved to the "LOD" tab.)
    const Camera& camera = scene.CameraRef();
    const auto& camPos = camera.GetPosition();
    const auto& camDir = camera.GetDirection();
    ImGui::Text("Camera position: %.2f, %.2f, %.2f", camPos.x, camPos.y, camPos.z);
    ImGui::Text("Camera direction: %.2f, %.2f, %.2f", camDir.x, camDir.y, camDir.z);
    ImGui::Text("Move speed multiplier: %.2f", scene.CameraControllerRef().GetMoveSpeedMult());
    ImGui::Text("Frame: %llu", static_cast<unsigned long long>(renderer.GetTotalFrameNumber()));

    ImGui::Separator();
    if (ImGui::TreeNodeEx("TextManager stats"))
    {
        TextManager* textManager = renderer.GetTextManager();
        bool textStatsEnabled = textManager->GetPerfStatsEnabled();
        if (ImGui::Checkbox("Enable TextManager instrumentation", &textStatsEnabled))
        {
            textManager->SetPerfStatsEnabled(textStatsEnabled);
        }
        const TextManager::PerfStats& textStats = renderer.GetTextManager()->GetPerfStats();
        ImGui::Text("Instrumentation: %s", textStatsEnabled ? "enabled" : "disabled");
        ImGui::Text("Regions:%u backgrounds:%u AddText:%u AddTextf:%u cached:%u positional:%u",
            textStats.regions, textStats.backgrounds, textStats.addTextCalls,
            textStats.addTextfCalls, textStats.addCachedTextCalls, textStats.positionalTextCalls);
        ImGui::Text("Lines direct:%u deferred:%u chars:%u direct glyphs:%u run glyphs:%u retarget verts:%u",
            textStats.directLines, textStats.deferredLines, textStats.inputChars,
            textStats.directGlyphs, textStats.runGlyphs, textStats.retargetedVertices);
        ImGui::Text("AddText %.2fus  cached %.2fus  format %.2fus",
            textStats.addTextUs, textStats.addCachedTextUs, textStats.formatUs);
        ImGui::Text("GlyphRun %.2fus (%u)  direct emit %.2fus (%u)  run emit %.2fus (%u)",
            textStats.buildGlyphRunUs, textStats.glyphRunBuilds,
            textStats.directEmitUs, textStats.directEmitCalls,
            textStats.runEmitUs, textStats.runEmitCalls);
        ImGui::Text("Direct split reserve %.2fus  setup %.2fus  loop %.2fus",
            textStats.directEmitReserveUs, textStats.directEmitSetupUs, textStats.directEmitLoopUs);
        ImGui::Text("Retarget %.2fus  Build %.2fus  reserve %.2fus  regions %.2fus",
            textStats.lineRetargetUs, textStats.buildUs,
            textStats.buildReserveUs, textStats.buildRegionsUs);
        ImGui::Text("Upload rect %.2fus  upload text %.2fus  Draw %.2fus  Begin %.2fus",
            textStats.uploadRectUs, textStats.uploadTextUs, textStats.drawUs, textStats.beginUs);
        ImGui::TreePop();
    }

}

#endif // !TC_SHIPPING
