// R10.4: Developer Controls > Streaming (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"
#include "app/ui/DeveloperTabs.h" // GRAPHICS_CONTROL
#include "rendering/core/RasterSettings.h"

#include <cstdint>
#include <vector>
#include <string>

#include "app/GraphicsSettings.h"
#include "app/scene/Scene.h"
#include "imgui.h"
#include "rendering/core/Renderer.h"
#include "rendering/streaming/StreamingSettings.h" // Streaming tab (A3)
#include "rendering/streaming/TextureStreaming.h"
#include "rendering/shadows/ShadowSettings.h"

void DeveloperWindow::DrawStreamingTab(Renderer& renderer, Scene& scene, SceneRenderSettings& settings,
    GraphicsSettingsManager& graphicsSettings)
{
    ImGui::TextWrapped("Texture streaming (UE r.Streaming.*): level textures load their 7 smallest mips and the "
        "manager streams the rest by distance, inside the pool budget. --set=streaming.<knob>:<v>");
    GRAPHICS_CONTROL(StreamingEnabled, "streamingEnabled", ImGui::Checkbox("Enabled", &streaming::g_enabled_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: no new requests; textures keep the mips they have. Textures loaded while off load whole.");
    GRAPHICS_CONTROL(StreamingPoolSizeMB, "streamingPoolSizeMB", ImGui::InputInt("Pool size MB", &streaming::g_poolSizeMB_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("-1 = 70%% of dedicated VRAM (UE), 0 = unlimited. Over budget the manager drops the least-kept mips first.");
    GRAPHICS_CONTROL(StreamingMipBias, "streamingMipBias", ImGui::SliderInt("Global mip bias", &streaming::g_mipBias_game, 0, 4));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE r.Streaming.MipBias. Per-texture bias ON: how many top mips the budget may take from one texture "
            "when the pool is over budget; while the pool has room nothing changes. OFF: every texture loses this many top mips now.");
    GRAPHICS_CONTROL(StreamingPerTextureBias, "streamingPerTextureBias", ImGui::Checkbox("Per-texture bias", &streaming::g_perTextureBias_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE r.Streaming.UsePerTextureBias (default on): over budget, textures lose their top mips one at a time up to "
            "Global mip bias and get them back when the pool has room. Off: Global mip bias caps every texture directly.");
    {
        // The material samplers' anisotropy (UE r.MaxAnisotropy), the next frame's: 1 = trilinear only.
        static const char* const kAniso[] = { "1x", "2x", "4x", "8x", "16x" };
        int choice = 0;
        for (int i = 0; i < 5; ++i) { if ((1u << i) == render::g_textureAnisotropy_game) { choice = i; } }
        GRAPHICS_CONTROL(StreamingAnisotropy, "textureAnisotropy",
            ImGui::Combo("Anisotropy", &choice, kAniso, 5) && ((render::g_textureAnisotropy_game = 1u << choice), true));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("UE r.MaxAnisotropy: how sharp a texture stays seen at a slant (the ground ahead). Every "
                "material sampler, live. --set=textures.anisotropy:<1..16>");
    }
    GRAPHICS_CONTROL(StreamingBoost, "streamingBoost", ImGui::SliderFloat("Boost", &streaming::g_boost_game, 0.25f, 4.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scales the screen size every texture is judged against: 2 = one mip sharper everywhere.");
    GRAPHICS_CONTROL(StreamingHiddenScale, "streamingHiddenScale", ImGui::SliderFloat("Hidden primitive scale", &streaming::g_hiddenScale_game, 0.0f, 1.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Objects outside the frustum ask for this fraction of their size (UE 0.5): what stays warm for a camera turn.");
    GRAPHICS_CONTROL(StreamingMipFade, "streamingMipFade", ImGui::Checkbox("Mip fade", &streaming::g_mipFade_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Newly arrived mips reveal over 0.3 s per mip (UE FMipBiasFade) instead of popping. --set=streaming.mipFade / mipFadeIn / mipFadeOut");
    ImGui::Separator();
    if (streaming::TextureStreaming* ts = renderer.GetTextureStreaming())
    {
        const streaming::TextureStreamingManager::Stats& st = ts->Manager().GetStats();
        const auto mb = [](std::uint64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); };
        ImGui::Text("Textures %u (with bounds %u, left behind %u: %.1f MB), bounds %u, cycles %u, calc %.3f ms, screen size %.0f px",
                    st.textures, st.withBounds, st.leftBehind, mb(st.leftBehindBytes), st.bounds, st.cycles, st.calcMs, st.screenSize);
        ImGui::Text("Pool %.1f MB | budget %.1f | used %.1f | budgeted %.1f | wanted %.1f | retired %.1f",
                    mb(st.poolBytes), mb(st.budgetBytes), mb(st.usedBytes), mb(st.budgetedBytes), mb(st.wantedBytes), mb(ts->RetiredBytes()));
        ImGui::Text("Last cycle: in %u (%.2f MB), out %u (%.2f MB), cancels %u, refused %u, in flight %u",
                    st.requestsIn, mb(st.bytesInCycle), st.requestsOut, mb(st.bytesOutCycle), st.cancels, st.refused, st.inFlight);
        ImGui::Text("wanted - resident:");
        for (int i = 0; i < 9; ++i)
        {
            ImGui::SameLine();
            ImGui::Text("[%+d] %u", i - 4, st.deltaHistogram[static_cast<std::size_t>(i)]);
        }
        const std::vector<streaming::TextureStreamingManager::Row>& rows = ts->Manager().Rows();
        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        ImGui::TextDisabled("needed vis/hid = mips the distance asks for right now (on screen / off screen x0.5); wanted = after the budget, and with room to spare the budget KEEPS resident mips (UE TryKeepMips)");
        if (ImGui::BeginTable("streamingTextures", 10, flags, ImVec2(0.0f, ImGui::GetFontSize() * 18.0f)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Texture", ImGuiTableColumnFlags_WidthStretch, 4.0f);
            ImGui::TableSetupColumn("mips");
            ImGui::TableSetupColumn("resident");
            ImGui::TableSetupColumn("needed vis/hid");
            ImGui::TableSetupColumn("wanted");
            ImGui::TableSetupColumn("budgeted");
            ImGui::TableSetupColumn("requested");
            ImGui::TableSetupColumn("texel m");
            ImGui::TableSetupColumn("seen s");
            ImGui::TableSetupColumn("retention");
            ImGui::TableHeadersRow();
            for (const streaming::TextureStreamingManager::Row& r : rows)
            {
                std::string path;
                path.reserve(r.path.size());
                for (wchar_t c : r.path) { path.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?'); }
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(path.c_str());
                if (r.unknownRef && ImGui::IsItemHovered()) { ImGui::SetTooltip("no placed object references it: kept fully loaded (UE unknown ref)"); }
                if (r.leftBehind && ImGui::IsItemHovered()) { ImGui::SetTooltip("its last placed object is gone (the material cache keeps it for the next visit): wants its tail, the budget keeps what it has"); }
                ImGui::TableNextColumn(); ImGui::Text("%u", r.mipCount);
                ImGui::TableNextColumn(); ImGui::Text("%u", r.resident);
                ImGui::TableNextColumn(); ImGui::Text("%u / %u", r.visibleWanted, r.hiddenWanted);
                ImGui::TableNextColumn(); ImGui::Text("%u", r.wanted);
                ImGui::TableNextColumn(); ImGui::Text("%u%s", r.budgeted, r.bias ? "*" : "");
                ImGui::TableNextColumn(); ImGui::Text("%u", r.requested);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", r.texelFactor);
                ImGui::TableNextColumn(); if (r.lastSeen < 1.0e5f) { ImGui::Text("%.1f", r.lastSeen); } else { ImGui::TextUnformatted("-"); }
                ImGui::TableNextColumn(); ImGui::Text("%d", r.retention);
            }
            ImGui::EndTable();
        }
    }
    ImGui::Separator();
    if (ImGui::Button("Reset streaming to defaults"))
    {
        graphicsSettings.ResetStreaming(renderer, scene, settings);
    }
}

#endif // !TC_SHIPPING
