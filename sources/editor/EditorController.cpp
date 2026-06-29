#include "editor/EditorController.h"

#include "editor/EditorContext.h"
#include "imgui.h"

void EditorController::Draw(EditorContext&)
{
    if (!open_)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(420.0f, 300.0f), ImGuiCond_FirstUseEver);

    bool open = open_;
    if (ImGui::Begin("Level Editor###LevelEditor", &open))
    {
        ImGui::TextUnformatted("Level Editor");
        ImGui::Separator();
        ImGui::TextWrapped(
            "Placeholder shell. The content browser, outliner, and inspector "
            "arrive in later steps.");
    }
    ImGui::End();

    open_ = open;
}
