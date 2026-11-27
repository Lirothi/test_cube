// R10.4: Developer Controls > Bindings (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"

#include <string>

#include "imgui.h"
#include "input/InputManager.h"

void DeveloperWindow::DrawBindingsTab(const InputManager& input)
{
    const auto& descs = input.GetBindingDescs();
    if (descs.empty())
    {
        ImGui::TextDisabled("No bindings loaded.");
    }
    else
    {
        const ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY;
        const float tableHeight = ImGui::GetContentRegionAvail().y;
        if (ImGui::BeginTable("BindingsTable", 2, tableFlags, ImVec2(0.0f, tableHeight)))
        {
            ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthFixed, 180.0f);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const auto& desc : descs)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(desc.keys.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(desc.action.c_str());
            }

            ImGui::EndTable();
        }
    }

}

#endif // !TC_SHIPPING
