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

    ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_FirstUseEver);

    bool open = open_;
    if (ImGui::Begin("Level Editor###LevelEditor", &open))
    {
        contentBrowser_.Draw(assetRegistry_, selectedAsset_);
    }
    ImGui::End();

    open_ = open;
}
