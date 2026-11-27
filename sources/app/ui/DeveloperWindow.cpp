// A shipped build compiles none of this (DeveloperWindow.h: the class is a stub there).
#if !TC_SHIPPING
#include "app/game/PlayerStart.h"
#include "app/game/PlayWorld.h" // R6.2a
#include "app/ui/DeveloperWindow.h"
#include "app/ui/DeveloperTabs.h" // R10.4: graphicsControl, GRAPHICS_CONTROL, the Frame
#include "core/vfs/Vfs.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <string>
#include <string_view>
#include <system_error>

#include "app/Systems.h"
#include "app/GraphicsSettings.h"
#include "app/levels/JsonLevel.h"
#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "rendering/scene/SkyOcclusionMap.h" // the sky-from-above map's ranges and coverage
#include "app/settings/Scalability.h"  // the Scalability tab
#include "app/settings/UserSettings.h" // the look's sensitivity and inverted vertical (Frame)
#include "core/logging/Log.h"
#include "core/profiling/ProfilerScopes.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "input/InputManager.h"
#include "rendering/core/Renderer.h"
#include "rendering/streaming/StreamingSettings.h" // Streaming tab (A3)
#include "rendering/streaming/TextureStreaming.h"
#include "rendering/core/RenderStats.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/core/VisibilityStats.h" // S0 occlusion plan: per-view visibility table
#include "rendering/visibility/OcclusionHistory.h" // S3a: vis::g_occlusion_game
#include "rendering/meshes/LodSelect.h"
#include "rendering/debug/LodDebugView.h"
#include "rendering/renderables/InstanceTypes.h"
#include "rendering/shadows/VirtualShadowMap.h"
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/shadows/ShadowGpuData.h" // Tessy N3: the cluster counter
#include "rendering/shadows/TessySettings.h"
#include "rendering/core/RasterSettings.h" // A6: the bindless checkbox
#include "core/threading/RenderThread.h"  // game thread plan G8: the Frame tab's switch
#include "ocean/OceanSimulation.h"
#include "text/TextManager.h"
#include "app/settings/SettingContext.h"     // R2.n: the Settings tab
#include "core/settings/SettingsRegistry.h"  // ...through the registry
#include "ui/ImGuiRenderer.h" // R4.3: renderer.ImGui()

namespace
{
    ID3D12Resource* GetOceanShoreDepthResource()
    {
        OceanSimulation* oceanSimulation = Systems::GetOceanSimulation();
        return oceanSimulation ? oceanSimulation->GetShoreDepthResource() : nullptr;
    }

}

// Hover text for a developer-window control. Kept deliberately blunt about GPU cost: several of
// these knobs are linear in frame time and nothing about the label says so.
void DeveloperWindow::DevHelp(const char* text)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        return;
    }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

bool DeveloperWindow::IsGraphicsToggle(GraphicsControl control)
{
    switch (control)
    {
    case GraphicsControl::AsyncCompute:
    case GraphicsControl::VSync:
    case GraphicsControl::CloudShadowBilinearWeather:
    case GraphicsControl::CloudShadowDetailMean:
    case GraphicsControl::VisibilityChunkMask:
    case GraphicsControl::OcclusionIndirectQueries:
    case GraphicsControl::DlssEnabled:
    case GraphicsControl::Fxaa:
    case GraphicsControl::UeSsrGlossyRays:
    case GraphicsControl::UeSsrUseSurfaceRoughness:
    case GraphicsControl::ReflectionTemporal:
    case GraphicsControl::WaterReflectionTemporal:
    case GraphicsControl::WaterReflectionSurfaceNormal:
    case GraphicsControl::RtWindBlas:
    case GraphicsControl::LodEnabled:
    // ShadowMode is NOT here any more: S15 made it a three-way combo, and this list decides
    // where the reset icon is placed. A toggle's frame is one FrameHeight wide, so the icon
    // landed a checkbox-width in -- i.e. on top of the combo's own text ("SDS(o)(GPU-fit)").
    // Anything whose frame is CalcItemWidth() wide belongs in the default branch.
    case GraphicsControl::CsmAutoSplits:
    case GraphicsControl::CsmScissorOptim:
    case GraphicsControl::CsmAccurateCasterCull:
    case GraphicsControl::CsmHzbCull:
    case GraphicsControl::CsmPancake:
    case GraphicsControl::CsmOverBlur:
    case GraphicsControl::ContactEnabled:
    case GraphicsControl::ContactTemporal:
    case GraphicsControl::ContactLengthWorldSpace:
    case GraphicsControl::GiIndirectShadows:
    case GraphicsControl::IndirectGBuffer:
    case GraphicsControl::GbufferHzb:
    case GraphicsControl::ShadowLodBiasNearTier:
    case GraphicsControl::VsmSmrtEnabled:
    case GraphicsControl::VsmSmrtTemporal:
    case GraphicsControl::VsmResidentOnly:
    case GraphicsControl::VsmSingleDraw:
    case GraphicsControl::VsmHzbCull:
    case GraphicsControl::VsmPageCaching:
    case GraphicsControl::TessyEnabled:
    case GraphicsControl::TessyVsm:
    case GraphicsControl::TessyRaster:
    case GraphicsControl::TessyAtlas:
    case GraphicsControl::TessyGBuffer:
        return true;
    default:
        return false;
    }
}

bool DeveloperWindow::ResetIconButton(ImTextureID texture)
{
    const float size = ImGui::GetFrameHeight();
    if (texture == ImTextureID_Invalid)
    {
        return ImGui::Button("##reset", ImVec2(size, size));
    }

    constexpr float kAtlasWidth = 384.0f;
    constexpr float kCellSize = 64.0f;
    constexpr float kCell = 5.0f;
    const ImVec2 uv0((kCell * kCellSize + 0.5f) / kAtlasWidth, 0.5f / kCellSize);
    const ImVec2 uv1(((kCell + 1.0f) * kCellSize - 0.5f) / kAtlasWidth,
                     (kCellSize - 0.5f) / kCellSize);
    const float padding = std::max(1.0f, size * 0.18f);
    const float imageSize = std::max(1.0f, size - padding * 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding, padding));
    const bool pressed = ImGui::ImageButton(
        "##reset",
        texture,
        ImVec2(imageSize, imageSize),
        uv0,
        uv1,
        ImVec4(0.0f, 0.0f, 0.0f, 0.0f),
        ImGui::GetStyleColorVec4(ImGuiCol_Text));
    ImGui::PopStyleVar();
    return pressed;
}


const DeveloperWindow::TabEntry DeveloperWindow::kTabs[19] = {
    { Tab::Scalability, "Scalability" }, { Tab::Frame, "Frame" }, { Tab::AntiAliasing, "AA / Scale" },
    { Tab::Visibility, "Visibility" }, { Tab::Reflections, "Reflections" }, { Tab::Gi, "GI" },
    { Tab::Fog, "Fog" }, { Tab::Sky, "Sky" }, { Tab::Debug, "Debug" },
    { Tab::Lod, "LOD" }, { Tab::Csm, "CSM" }, { Tab::Contact, "Contact" },
    { Tab::Vsm, "VSM" }, { Tab::Tessy, "Tessy" }, { Tab::Physics, "Physics" }, { Tab::Streaming, "Streaming" }, { Tab::Memory, "Memory" },
    { Tab::Bindings, "Bindings" }, { Tab::Settings, "Settings" }
};

bool DeveloperWindow::OpenTab(const std::string& label)
{
    const auto same = [&label](std::string_view name) {
        return name.size() == label.size() &&
            std::equal(name.begin(), name.end(), label.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            });
    };
    for (const TabEntry& entry : kTabs)
    {
        const std::string_view name(entry.label);
        // --dev-tab reads its value up to a space (BootParse.cpp DevTab), so "AA / Scale" never arrived whole and the tab
        // could not be opened: a label is also its first word ("--dev-tab=aa").
        if (same(name) || same(name.substr(0, name.find(' '))))
        {
            open_ = true;
            activeTab_ = entry.tab;
            return true;
        }
    }
    return false;
}

// Every --set / MCP set_settings name the settings registry holds (architecture plan R2.n: "Developer Controls, an
// ordinary widget by type"): a checkbox for a Bool, an integer for an Int, a drag for a Float, a button for an action --
// read and written through the row, as --set does; a family's names (race.*, sg.<group>, the ocean's colours...) by its
// keys. The tags: P saved with the graphics, L the level's (the level file sets it; Ctrl+S saves the document, not
// this), S the graphics levels manage it, A an action. Only the visible rows are read (a clipper over ~1000 names).
bool DeveloperWindow::DrawSettingsTab(Scene& scene, SceneRenderSettings& settings)
{
    struct Entry
    {
        std::string name;
        settings::SettingType type;
        std::uint32_t flags;
    };
    static std::vector<Entry> entries;
    static std::vector<int> shown;
    static std::string shownFor = "\x01"; // the filter `shown` was made for; never a real one at first
    // The list is made each time the tab opens: a family's keys change (sg.reload reads another scalability.json).
    static int drawnFrame = -2;
    static std::string held; // the row whose edit is in progress, and its value (see the Int / Float widgets)
    static float heldValue = 0.0f;
    // The rows' values, read every 30 frames (a value changed elsewhere shows within half a second): a row's reader
    // reaches whatever its name does, so reading waits for the render half's frame -- on those frames only, and before
    // an edit is applied; the other frames draw from here (AGENTS.md: a panel waits only where it reaches them).
    static std::vector<float> cachedValue;
    static std::vector<std::uint8_t> cachedReadable;
    static std::vector<int> cachedGeneration;
    static int generation = 0;
    static int generationFrame = 0;
    const bool opened = ImGui::GetFrameCount() != drawnFrame + 1;
    drawnFrame = ImGui::GetFrameCount();
    const settings::SettingsRegistry& registry = settings::Registry();
    if (opened)
    {
        shownFor = "\x01";
        held.clear();
        entries.clear();
        for (const settings::SettingDesc& d : registry.All()) { entries.push_back({ d.name, d.type, d.flags }); }
        for (const settings::SettingFamily& f : registry.Families())
        {
            std::vector<std::string> keys;
            f.keys(keys);
            for (const std::string& k : keys) { entries.push_back({ std::string(f.prefix) + k, f.type, f.flags }); }
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
        cachedValue.assign(entries.size(), 0.0f);
        cachedReadable.assign(entries.size(), 0u);
        cachedGeneration.assign(entries.size(), -1);
    }
    if (opened || ImGui::GetFrameCount() - generationFrame >= 30)
    {
        ++generation;
        generationFrame = ImGui::GetFrameCount();
    }
    bool waited = false;
    const auto reachRender = [&waited] {
        if (!waited)
        {
            threading::WaitRenderForGame("DeveloperWindow::DrawSettingsTab (rows)");
            waited = true;
        }
    };
    ImGui::TextWrapped("Every --set name (%zu), through the settings registry (app/settings/*SettingRows.cpp): what --set, "
                       "--sweep and MCP set_settings write. Transient unless tagged P (saved with the graphics, as an edit "
                       "in the other tabs).", entries.size());
    ImGui::InputTextWithHint("##settingsFilter", "filter: a part of the name (vsm., bloom.int)", settingsFilter_,
                             sizeof(settingsFilter_));
    if (shownFor != settingsFilter_)
    {
        shown.clear();
        for (int i = 0; i < static_cast<int>(entries.size()); ++i)
        {
            if (entries[static_cast<std::size_t>(i)].name.find(settingsFilter_) != std::string::npos) { shown.push_back(i); }
        }
        shownFor = settingsFilter_;
    }
    settings::SettingContext ctx{ scene, settings };
    bool persistedChanged = false;
    if (ImGui::BeginTable("SettingsRegistry", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("Tags", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()));
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const std::size_t index = static_cast<std::size_t>(shown[static_cast<std::size_t>(row)]);
                const Entry& e = entries[index];
                if (cachedGeneration[index] != generation)
                {
                    reachRender();
                    float read = 0.0f;
                    cachedReadable[index] = registry.Read(ctx, e.name, read) ? 1u : 0u;
                    cachedValue[index] = read;
                    cachedGeneration[index] = generation;
                }
                const auto apply = [&](float v) {
                    reachRender();
                    cachedGeneration[index] = -1; // read back next frame: the row may clamp it
                    return registry.Apply(ctx, e.name, v);
                };
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.name.c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(e.name.c_str());
                ImGui::SetNextItemWidth(-FLT_MIN);
                float value = cachedValue[index];
                const bool readable = cachedReadable[index] != 0u;
                bool changed = false;
                if (e.flags & settings::SettingAction)
                {
                    if (ImGui::SmallButton("Run")) { changed = apply(1.0f); }
                }
                else if (!readable)
                {
                    // No reader (each boat's own vessel.*, what a script injects): only a value to send.
                    if (ImGui::InputFloat("##send", &value, 0.0f, 0.0f, "%g", ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        changed = apply(value);
                    }
                }
                else if (e.type == settings::SettingType::Bool)
                {
                    bool on = value != 0.0f;
                    if (ImGui::Checkbox("##v", &on)) { changed = apply(on ? 1.0f : 0.0f); }
                }
                else
                {
                    // The edit in progress is held here, not re-read from the row: a row that clamps (sg.all 5 -> 3,
                    // gi.rays 3 -> 16) would differ from the field's text every frame and be applied again on each
                    // one. An Int, and a Float whose change rebuilds or idles something (targets, the GPU, a cache,
                    // pipelines), is applied once, when the field lets go after an edit (Enter, a click elsewhere);
                    // a cheap Float follows the drag.
                    if (held == e.name) { value = heldValue; }
                    bool edited = false;
                    if (e.type == settings::SettingType::Int)
                    {
                        int i = static_cast<int>(value);
                        edited = ImGui::InputInt("##v", &i, 0, 0); // no step buttons: the field is the one item
                        value = static_cast<float>(i);
                    }
                    else
                    {
                        const float speed = std::max(std::abs(value) * 0.005f, 0.001f);
                        edited = ImGui::DragFloat("##v", &value, speed, 0.0f, 0.0f, "%g");
                    }
                    constexpr std::uint32_t kCostly = settings::SettingCostTargets | settings::SettingCostIdleGpu |
                                                      settings::SettingCostCache | settings::SettingCostPipelines |
                                                      settings::SettingCostBoot;
                    const bool live = e.type == settings::SettingType::Float && (e.flags & kCostly) == 0u;
                    if (edited)
                    {
                        held = e.name;
                        heldValue = value;
                        if (live) { changed = apply(value); }
                    }
                    if (held == e.name && ImGui::IsItemDeactivated())
                    {
                        if (!live && ImGui::IsItemDeactivatedAfterEdit())
                        {
                            changed = apply(heldValue);
                        }
                        held.clear();
                    }
                }
                persistedChanged |= changed && (e.flags & settings::SettingPersistent) != 0u;
                ImGui::PopID();
                ImGui::TableNextColumn();
                char tags[8] = {};
                int n = 0;
                if (e.flags & settings::SettingPersistent) { tags[n++] = 'P'; }
                if (e.flags & settings::SettingLevelOwned) { tags[n++] = 'L'; }
                if (e.flags & settings::SettingScalable) { tags[n++] = 'S'; }
                if (e.flags & settings::SettingAction) { tags[n++] = 'A'; }
                ImGui::TextDisabled("%s", tags);
            }
        }
        ImGui::EndTable();
    }
    return persistedChanged;
}

void DeveloperWindow::ToggleTextureInspector()
{
    textureDebugViewer_.SetOpen(!textureDebugViewer_.IsOpen());
}

void DeveloperWindow::DrawScalabilityTab()
{
    settings::Scalability& s = settings::TheScalability();
    ImGui::TextWrapped("Graphics levels (docs/scalability_plan.md): each group's knobs at Low / Medium / High / Ultra, from %s. "
                       "Ultra is today's picture; a knob the level sets is never raised above the level's own value. A choice "
                       "here is applied at once and saved in %s; 'Not managed' leaves the knobs as the graphics settings have "
                       "them. A knob changed in another tab while its group is managed is kept over the level (set by hand).",
                       settings::Scalability::kTablePath, settings::Scalability::kUserFile);
    if (ImGui::SmallButton("Reload table"))
    {
        s.LoadTable();
        s.LoadUser();
        s.Apply("Developer Controls: table reloaded");
    }
    if (!s.Loaded())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "The table did not load:");
        for (const std::string& e : s.TableErrors()) { ImGui::BulletText("%s", e.c_str()); }
        return;
    }
    const settings::ScalabilityTable& table = s.Table();
    bool changed = false;
    const auto levelCombo = [&](const char* label, int current, const std::array<std::string, settings::kScalabilityLevels>& names,
                                const char* customName) -> int
    {
        const std::string preview = current >= 0 ? names[static_cast<std::size_t>(current)] : current == -1 && customName ? customName : "Not managed";
        int chosen = current;
        if (ImGui::BeginCombo(label, preview.c_str()))
        {
            if (ImGui::Selectable("Not managed", current < -1 || (current == -1 && !customName))) { chosen = customName ? -2 : -1; }
            for (int i = 0; i < settings::kScalabilityLevels; ++i)
            {
                if (ImGui::Selectable(names[static_cast<std::size_t>(i)].c_str(), current == i)) { chosen = i; }
            }
            ImGui::EndCombo();
        }
        return chosen;
    };
    const int preset = s.Preset();
    const int newPreset = levelCombo("Preset", preset, table.levelNames, "Custom");
    if (newPreset != preset)
    {
        s.SetPreset(newPreset >= 0 ? newPreset : -1);
        changed = true;
    }
    std::array<std::string, settings::kScalabilityLevels> resolutionNames = table.levelNames;
    for (int i = 0; i < settings::kScalabilityLevels; ++i)
    {
        if (!table.resolutionLevelLabels[static_cast<std::size_t>(i)].empty())
        {
            resolutionNames[static_cast<std::size_t>(i)] = table.levelNames[static_cast<std::size_t>(i)] + " (" +
                                                           table.resolutionLevelLabels[static_cast<std::size_t>(i)] + ")";
        }
    }
    const int resolution = s.ResolutionLevel();
    const int newResolution = levelCombo(table.resolutionLabel.c_str(), resolution, resolutionNames, nullptr);
    if (newResolution != resolution)
    {
        s.SetResolutionLevel(newResolution);
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", s.DlssAvailable() ? "DLSS" : "no DLSS: render scale + FXAA");
    ImGui::Separator();
    for (std::size_t g = 0; g < table.groups.size(); ++g)
    {
        const settings::ScalabilityGroup& group = table.groups[g];
        std::array<std::string, settings::kScalabilityLevels> names = table.levelNames;
        for (int i = 0; i < settings::kScalabilityLevels; ++i)
        {
            if (!group.levelLabels[static_cast<std::size_t>(i)].empty()) { names[static_cast<std::size_t>(i)] = group.levelLabels[static_cast<std::size_t>(i)]; }
        }
        const int level = s.GroupLevel(static_cast<int>(g));
        ImGui::PushID(static_cast<int>(g));
        const int newLevel = levelCombo(("sg." + group.key).c_str(), level, names, nullptr);
        if (newLevel != level)
        {
            s.SetGroupLevel(static_cast<int>(g), newLevel);
            changed = true;
        }
        if (ImGui::IsItemHovered() && !group.hint.empty()) { ImGui::SetTooltip("%s: %s", group.label.c_str(), group.hint.c_str()); }
        ImGui::SameLine();
        if (ImGui::TreeNode("knobs", "%s", group.label.c_str()))
        {
            // The knobs of the group at its level (Ultra when not managed): what the table asks, what the knob holds.
            const int shown = level >= 0 ? level : settings::kScalabilityLevels - 1;
            for (const auto& [name, op] : group.levels[static_cast<std::size_t>(shown)])
            {
                float current = 0.0f, target = 0.0f;
                const bool read = s.Read(name, current);
                const bool hasTarget = s.Target(name, op, target);
                const bool pinned = s.Pins().count(name) != 0;
                const auto manual = s.Overrides().find(name);
                if (manual != s.Overrides().end() && level >= 0) { target = manual->second; }
                const bool differs = read && hasTarget && std::abs(current - target) > 1e-4f * std::max(1.0f, std::abs(target));
                const char* opText = op.kind == settings::ScalabilityOp::Kind::Max ? "max " : op.kind == settings::ScalabilityOp::Kind::Min ? "min " :
                                     op.kind == settings::ScalabilityOp::Kind::Level ? "level" : "";
                char line[256];
                if (op.kind == settings::ScalabilityOp::Kind::Level)
                {
                    std::snprintf(line, sizeof(line), "%-34s table: level        now %g%s", name.c_str(), static_cast<double>(current), pinned ? "  (pinned by --set/MCP)" : "");
                }
                else
                {
                    std::snprintf(line, sizeof(line), "%-34s table: %s%-8g  now %g%s", name.c_str(), opText, static_cast<double>(op.value),
                                  static_cast<double>(current), pinned ? "  (pinned by --set/MCP)"
                                  : manual != s.Overrides().end() ? "  (set by hand)" : "");
                }
                if (differs && level >= 0) { ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", line); }
                else { ImGui::TextUnformatted(line); }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    if (!s.Pins().empty())
    {
        std::string pins;
        for (const std::string& p : s.Pins()) { pins += (pins.empty() ? "" : ", ") + p; }
        ImGui::TextWrapped("Pinned by --set / MCP (left alone): %s", pins.c_str());
        if (ImGui::SmallButton("Unpin")) { s.ClearPins(); changed = true; }
    }
    // Values changed by hand over a managed group's level (an edit in another tab while a level is chosen): they stay over
    // any preset until taken back here, by the Reset of their tab, or by the menu's "Back to the levels".
    if (!s.Overrides().empty())
    {
        const std::map<std::string, float> inUse = s.ActiveOverrides();
        ImGui::SeparatorText("Set by hand over the levels");
        std::string clear;
        for (const auto& [name, value] : s.Overrides())
        {
            ImGui::PushID(name.c_str());
            if (ImGui::SmallButton("Back to the level")) { clear = name; }
            ImGui::SameLine();
            if (inUse.count(name)) { ImGui::Text("%-34s %g", name.c_str(), static_cast<double>(value)); }
            else { ImGui::TextDisabled("%-34s %g  (its group is not managed: unused)", name.c_str(), static_cast<double>(value)); }
            ImGui::PopID();
        }
        if (ImGui::SmallButton("All back to the levels")) { clear = "*"; }
        if (!clear.empty())
        {
            s.ClearOverride(clear == "*" ? std::string() : clear);
            changed = true;
        }
    }
    const settings::ScalabilityReport& r = s.LastReport();
    ImGui::TextWrapped("Last apply (%s): %d knob(s) changed, %d pinned, %d set by hand, %d failed; render targets %d, idle "
                       "reconciles %d; %.2f ms", r.reason.c_str(), r.keysWritten, r.keysPinned, r.keysOverridden, r.keysFailed,
                       r.targetChanges, r.idleChanges, r.ms);
    if (changed)
    {
        s.Apply("Developer Controls");
        std::string why;
        if (!s.SaveUser(why)) { LOG_WARNING(logging::LogCategory::App, "scalability: not saved: {}", why); }
    }
}

void DeveloperWindow::DrawTraceControls()
{
    if (!traceWindowOpen_)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(420.0f, 190.0f), ImGuiCond_FirstUseEver);
    bool open = traceWindowOpen_;
    if (ImGui::Begin("Trace capture###TraceCapture", &open))
    {
        const Profiler::TraceCaptureStatus status = Profiler::Get().GetTraceCaptureStatus();

        if (status.active)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "RECORDING");
            ImGui::SameLine();
            if (status.openEnded)
            {
                ImGui::Text("%u frames, %zu events", status.framesRecorded, status.events);
            }
            else
            {
                ImGui::Text("%u frames left", status.framesRemaining);
            }
        }
        else if (status.pending)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "starting next frame...");
        }
        else
        {
            ImGui::TextDisabled("idle");
        }

        ImGui::Separator();

        // Start/Stop, not a frame count: the things worth capturing (a drag, a hitch while walking)
        // have no length you can know in advance.
        ImGui::BeginDisabled(status.active || status.pending);
        if (ImGui::Button("Start", ImVec2(120.0f, 0.0f)))
        {
            Profiler::Get().BeginTraceCapture();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!status.active && !status.pending);
        if (ImGui::Button("Stop", ImVec2(120.0f, 0.0f)))
        {
            Profiler::Get().StopTraceCapture();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(status.active || status.pending);
        if (ImGui::Button("Quick 120", ImVec2(120.0f, 0.0f)))
        {
            Profiler::Get().RequestTraceCapture(120);
        }
        ImGui::EndDisabled();

        ImGui::TextDisabled("F11 does the same as Quick 120 (unless ImGui has the keyboard).");

        ImGui::Separator();
        if (status.lastPath.empty())
        {
            ImGui::TextDisabled("no capture written yet");
        }
        else
        {
            ImGui::TextWrapped("last: %s", status.lastPath.c_str());
            if (ImGui::Button("Copy path"))
            {
                ImGui::SetClipboardText(status.lastPath.c_str());
            }
        }
    }
    ImGui::End();
    traceWindowOpen_ = open;
}

void DeveloperWindow::ApplySunElevation(Scene& scene) const
{
    constexpr double radians = 3.14159265358979323846 / 180.0;
    const double elevation = (sunElevationMin_ +
        (sunElevationMax_ - sunElevationMin_) * sunElevationProgress_) * radians;
    // Double precision keeps cos(+/-90 degrees) nonnegative and preserves azimuth at the poles.
    // T3: through the scene, so a level with a Time of Day gets the time that puts the sun there.
    scene.SetSunToward(Math::float3(
        static_cast<float>(std::cos(elevation) * std::sin(sunElevationAzimuth_)),
        static_cast<float>(std::sin(elevation)),
        static_cast<float>(std::cos(elevation) * std::cos(sunElevationAzimuth_))));
}

// S15 -- THE shadow-mode control, in one place and drawn from both tabs (CSM and VSM). It was
// two checkboxes, one per tab, each phrased as "my algorithm is the active one"; with a third
// mode that phrasing cannot be honest in either tab. The order matches the Ctrl+V cycle.
void DeveloperWindow::DrawShadowModeCombo(const char* id)
{
    static const char* const kModeNames[] = { "Legacy CSM", "VSM (clipmap)", "SDSM (GPU-fit)" };
    int modeIdx = static_cast<int>(render::g_shadowMode_game);
    if (GRAPHICS_CONTROL(ShadowMode, id,
        ImGui::Combo("Shadow mode [Ctrl+V]", &modeIdx, kModeNames, 3)))
    {
        render::g_shadowMode_game = static_cast<render::ShadowMode>(std::clamp(modeIdx, 0, 2));
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Which machinery produces the SUN's shadow.\n"
                          "Legacy CSM: four cascades fitted on the CPU to frustum-slice spheres.\n"
                          "VSM: the virtual clipmap (directional and locals).\n"
                          "SDSM: four partitions whose depth range AND light-space box are reduced\n"
                          "from THIS frame's depth buffer on the GPU. Same atlas, same filter --\n"
                          "the cascades simply stop covering empty air.");
}

bool DeveloperWindow::Draw(Renderer& renderer, Scene& scene, const InputManager& input,
    LevelManager& levelManager, SceneRenderSettings& settings,
    GraphicsSettingsManager& graphicsSettings
#if WITH_EDITOR
    , EditorController& editorController
#endif
)
{
    CPU_SCOPE(ProfilerScopes::kBuildDeveloperWindow);
    // Game thread plan G8b: the tabs that read or set the render half's own objects wait for the render thread's frame
    // in flight -- just before the tab's content is drawn, below. The others work on the game half's copies (the
    // settings, the knobs' _game twins, the game-owned cascade config) and the renderer's setters, which wait on their
    // own; they keep the overlap. Waiting for every tab took it away whenever the window was open: on wind_test in Debug
    // the game half stood 6.6 ms a frame in that wait (2026-11-03). The texture inspector and the ocean controls wait
    // on their own when they are open.
    // Run independently of the selected tab and the window's visibility.
    if (sunElevationPlaying_)
    {
        sunElevationProgress_ = std::min(1.0, sunElevationProgress_ +
            static_cast<double>(std::max(0.0f, ImGui::GetIO().DeltaTime)) / sunElevationSeconds_);
        ApplySunElevation(scene);
        if (sunElevationProgress_ >= 1.0) sunElevationPlaying_ = false;
    }
    bool graphicsSettingsDirty = false;
    ImTextureID resetIconTexture = ImTextureID_Invalid;
    // R10.4: what graphicsControl (DeveloperTabs.h) reaches of this frame, from the tabs' own files.
    Frame frame{ renderer, scene, settings, graphicsSettings, resetIconTexture, graphicsSettingsDirty };
    frame_ = &frame;
    struct FrameScope
    {
        DeveloperWindow* window;
        ~FrameScope() { window->frame_ = nullptr; }
    } frameScope{ this };

    if (!open_)
    {
        textureDebugViewer_.Draw(renderer, GetOceanShoreDepthResource());
    // The inspector cannot brighten its own preview (ImGui clamps the image tint), so it can hand
    // the request over to the fullscreen view, which draws through a shader we control.
    if (const int requested = textureDebugViewer_.TakeFullscreenRequest(); requested >= 0)
    {
        settings.debugTexTarget = requested;
        settings.debugTexMip = textureDebugViewer_.RequestedMip();
        settings.debugTexMode = true;
    }
        oceanControlsWindow_.Draw(renderer, Systems::GetOceanSimulation());
        logWindow_.Draw(); // returns immediately unless open (logging plan L8 cost rule)
        DrawTraceControls();
        return false;
    }

    ImGui::SetNextWindowSize(ImVec2(700.0f, 800.0f), ImGuiCond_FirstUseEver);

    bool open = open_;
    const ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        (windowMaximize_.maximized ? ImGuiWindowFlags_NoMove : 0);
    if (ImGui::Begin("Developer Controls [F1]###DeveloperControls", &open, windowFlags))
    {
        ui::HandleWindowTitleDoubleClickMaximize(windowMaximize_);

        if (!resetIconAtlasTried_)
        {
            resetIconAtlasTried_ = true;
            renderer.WaitForPreviousFrame();
            UploadBatch uploads;
            if (uploads.Begin(&renderer))
            {
                Texture2D::CreateDesc desc;
                desc.path = L"textures/editor/content_browser_icons.png";
                desc.usage = Texture2D::Usage::AlbedoSRGB;
                resetIconAtlas_.emplace();
                resetIconAtlas_->SetMemTag(MemTag::Ui);
                resetIconAtlasReady_ = resetIconAtlas_->CreateFromFile(
                    &renderer, uploads.CommandList(), desc, uploads.KeepAlive());
                uploads.SubmitAndWait(&renderer);
            }
        }
        if (resetIconAtlasReady_ && resetIconAtlas_)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = resetIconAtlas_->GetSrvFormat();
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            renderer.ImGui().MarkTextureShaderReadable(resetIconAtlas_->GetResource());
            resetIconTexture = renderer.ImGui().CreateTextureId(resetIconAtlas_->GetResource(), srvDesc);
        }

        ImGui::SeparatorText("Project graphics settings");
        ImGui::TextUnformatted("user_settings.json: graphics");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("What differs from the build's defaults (tools/dist/graphics_defaults.json in the tree,\n"
                              "config/graphics_defaults.json in the packages). graphics_settings.json is read once to move\n"
                              "it here and never written. A Reset goes back to the graphics levels and the defaults.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Save now"))
        {
            graphicsSettings.SaveCurrent(renderer, scene, settings);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload file"))
        {
            graphicsSettings.Reload(renderer, scene, settings);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset all graphics"))
        {
            graphicsSettings.ResetAll(renderer, scene, settings);
        }
        if (!graphicsSettings.Status().empty())
        {
            ImGui::TextDisabled("%s", graphicsSettings.Status().c_str());
        }
        ImGui::TextDisabled("Global quality autosaves after an edit; level look stays in the level file.");

        const float navigationWidth = ImGui::GetFontSize() * 8.0f;
        if (ImGui::BeginChild("DeveloperControlsNavigation", ImVec2(navigationWidth, 0), true))
        {
            for (const auto& entry : kTabs)
            {
                if (ImGui::Selectable(entry.label, activeTab_ == entry.tab))
                    activeTab_ = entry.tab;
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        // The tabs that reach the render half's objects directly (the caster table, the VSM, the cascades and SDSM,
        // the sky's and the clouds' LUT objects, the debug targets, the streaming registry, the memory providers).
        switch (activeTab_)
        {
        case Tab::Visibility: case Tab::Sky: case Tab::Debug: case Tab::Csm: case Tab::Vsm: case Tab::Tessy:
        case Tab::Streaming: case Tab::Memory:
            threading::WaitRenderForGame("DeveloperWindow::Draw (render-half tab)");
            break;
        default:
            break;
        }
        // A separate child per tab preserves its scroll position; navigation stays visible.
        ImGui::PushID(static_cast<int>(activeTab_));
        if (ImGui::BeginChild("DeveloperControlsContent", ImVec2(0, 0), false,
            ImGuiWindowFlags_HorizontalScrollbar))
        {
            ImGui::PushItemWidth(ImGui::CalcItemWidth() * 0.70f);
            if (activeTab_ == Tab::Scalability)
            {
                DrawScalabilityTab();
            }
            if (activeTab_ == Tab::Frame)
            {
                DrawFrameTab(renderer, scene, input, settings);
            }

            if (activeTab_ == Tab::AntiAliasing)
            {
                DrawAntiAliasingTab(renderer, scene, settings, graphicsSettings);
            }

            if (activeTab_ == Tab::Visibility)
            {
                DrawVisibilityTab(renderer, scene, settings, graphicsSettings);
            }

            if (activeTab_ == Tab::Reflections)
            {
                DrawReflectionsTab(renderer, scene, settings, graphicsSettings);
            }

            if (activeTab_ == Tab::Fog)
            {
                DrawFogTab(renderer, scene, settings, graphicsSettings);
            }

            if (activeTab_ == Tab::Sky)
            {
                DrawSkyTab(scene, settings);
            }

            if (activeTab_ == Tab::Debug)
            {
                DrawDebugTab(renderer, scene, levelManager, settings
#if WITH_EDITOR
                    , editorController
#endif
                );
            }

            if (activeTab_ == Tab::Gi)
            {
                DrawGiTab(renderer, scene, settings, graphicsSettings);
            }

            if (activeTab_ == Tab::Lod)
            {
                DrawLodTab(renderer, scene, settings, graphicsSettings);
            }

            if (activeTab_ == Tab::Csm)
            {
                DrawCsmTab(renderer, scene, settings, graphicsSettings);
            }

            if (activeTab_ == Tab::Contact)
            {
                DrawContactTab(renderer, scene, settings, graphicsSettings);
            }

            if (activeTab_ == Tab::Vsm)
            {
                DrawVsmTab(renderer, scene, settings, graphicsSettings);
            }

            if (activeTab_ == Tab::Physics)
            {
                DrawPhysicsTab(scene);
            }

            if (activeTab_ == Tab::Tessy)
            {
                DrawTessyTab(scene);
            }

            if (activeTab_ == Tab::Streaming)
            {
                DrawStreamingTab(renderer, scene, settings, graphicsSettings);
            }

            if (activeTab_ == Tab::Memory)
            {
                memoryTab_.Draw(renderer.GetDevice());
            }

            if (activeTab_ == Tab::Settings)
            {
                graphicsSettingsDirty |= DrawSettingsTab(scene, settings);
            }

            if (activeTab_ == Tab::Bindings)
            {
                DrawBindingsTab(input);
            }

            ImGui::PopItemWidth();
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
    ImGui::End();
    open_ = open;

    textureDebugViewer_.Draw(renderer, GetOceanShoreDepthResource());
    oceanControlsWindow_.Draw(renderer, Systems::GetOceanSimulation());
    logWindow_.Draw();
    DrawTraceControls();
    return graphicsSettingsDirty;
}
#undef GRAPHICS_CONTROL
#undef GRAPHICS_CONTROL_WIDTH
#endif // !TC_SHIPPING
