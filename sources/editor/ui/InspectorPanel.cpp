#include "editor/ui/InspectorPanel.h"
#if WITH_EDITOR

#include <cstdio>
#include <memory>
#include <string>

#include "editor/EditorContext.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/SetEnabledCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "imgui.h"

void InspectorPanel::Draw(EditorContext& ctx, EditorCommandStack& commandStack, bool* open)
{
    ImGui::SetNextWindowSize(ImVec2(320.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector", open))
    {
        ImGui::End();
        return;
    }

    EditorObject* obj = ctx.document.Find(ctx.selectedObject);
    if (!obj)
    {
        ImGui::TextDisabled("No object selected.");
        ImGui::End();
        return;
    }

    ImGui::Text("ID: %llu", static_cast<unsigned long long>(obj->id.value));
    ImGui::Text("Type: %s", obj->type.c_str());

    char nameBuf[256];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", obj->name.c_str());
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
    {
        obj->name = nameBuf;
        ctx.document.SetDirty(true);
    }

    bool enabled = obj->enabled;
    if (ImGui::Checkbox("Enabled", &enabled))
    {
        commandStack.Execute(ctx, std::make_unique<SetEnabledCommand>(obj->id, enabled));
    }

    if (obj->properties.contains("model") && obj->properties["model"].is_string())
    {
        ImGui::Text("Model: %s", obj->properties["model"].get<std::string>().c_str());
    }

    ImGui::Separator();
    DrawTransformEditor(ctx, commandStack, *obj);

    ImGui::End();
}

void InspectorPanel::DrawTransformEditor(EditorContext& ctx, EditorCommandStack& commandStack, EditorObject& object)
{
    EditorTransform t = object.transform;
    bool changed = false;
    bool committed = false;

    float position[3] = { t.position.x, t.position.y, t.position.z };
    changed |= ImGui::DragFloat3("Position", position, 0.05f);
    if (ImGui::IsItemActivated()) { transformBeforeEdit_ = object.transform; }
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    t.position = Math::float3(position[0], position[1], position[2]);

    float rotation[3] = { t.rotationDeg.x, t.rotationDeg.y, t.rotationDeg.z };
    changed |= ImGui::DragFloat3("Rotation (deg)", rotation, 0.5f);
    if (ImGui::IsItemActivated()) { transformBeforeEdit_ = object.transform; }
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    t.rotationDeg = Math::float3(rotation[0], rotation[1], rotation[2]);

    float scale[3] = { t.scale.x, t.scale.y, t.scale.z };
    changed |= ImGui::DragFloat3("Scale", scale, 0.05f);
    if (ImGui::IsItemActivated()) { transformBeforeEdit_ = object.transform; }
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    t.scale = Math::float3(scale[0], scale[1], scale[2]);

    // Live feedback while dragging (not yet an undo entry).
    if (changed)
    {
        TransformObjectCommand::ApplyTransform(ctx, object.id, t);
    }

    // Commit the whole gesture as one undo action. IsItemDeactivatedAfterEdit only
    // fires when the value actually changed, so this never records a no-op.
    if (committed)
    {
        commandStack.Execute(ctx, std::make_unique<TransformObjectCommand>(
            object.id, transformBeforeEdit_, object.transform));
    }
}

#endif // WITH_EDITOR
