#include "editor/EditorController.h"

#include "editor/EditorContext.h"
#include "imgui.h"

void EditorController::Draw(EditorContext&)
{
    if (!open_)
    {
        return;
    }

    // Lazy first scan: discover assets only once the editor is actually opened.
    if (!assetsScanned_)
    {
        assetRegistry_.Refresh();
        assetsScanned_ = true;
    }

    ImGui::SetNextWindowSize(ImVec2(420.0f, 300.0f), ImGuiCond_FirstUseEver);

    bool open = open_;
    if (ImGui::Begin("Level Editor###LevelEditor", &open))
    {
        if (ImGui::Button("Refresh Assets"))
        {
            assetRegistry_.Refresh();
        }

        ImGui::Separator();
        ImGui::Text("Assets discovered: %zu", assetRegistry_.Assets().size());
        ImGui::Text("Meshes:    %zu", assetRegistry_.CountByType(EditorAssetType::Mesh));
        ImGui::Text("Materials: %zu", assetRegistry_.CountByType(EditorAssetType::MaterialPreset));
        ImGui::Text("Textures:  %zu", assetRegistry_.CountByType(EditorAssetType::Texture));
        ImGui::Text("Levels:    %zu", assetRegistry_.CountByType(EditorAssetType::Level));
        ImGui::Text("Shaders:   %zu", assetRegistry_.CountByType(EditorAssetType::Shader));

        ImGui::Separator();
        ImGui::TextWrapped(
            "Content browser, outliner, and inspector arrive in later steps.");
    }
    ImGui::End();

    open_ = open;
}
