#include "editor/EditorController.h"

#include "editor/EditorContext.h"
#include "imgui.h"

void EditorController::Draw(EditorContext&)
{
    if (!open_)
    {
        return;
    }

    // Lazy first-open init: scan assets and load the active level's object
    // metadata once. Neither touches the runtime scene.
    if (!firstOpenInitialized_)
    {
        assetRegistry_.Refresh();
        document_.LoadFromLevelFile("data/levels/demo.json");
        firstOpenInitialized_ = true;
    }

    ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_FirstUseEver);

    bool open = open_;
    if (ImGui::Begin("Level Editor###LevelEditor", &open))
    {
        const std::string& levelPath = document_.LevelPath();
        ImGui::Text("Level: %s | Document objects: %d",
            levelPath.empty() ? "(none)" : levelPath.c_str(),
            static_cast<int>(document_.Objects().size()));
        ImGui::Separator();

        contentBrowser_.Draw(assetRegistry_, selectedAsset_);
    }
    ImGui::End();

    open_ = open;
}
