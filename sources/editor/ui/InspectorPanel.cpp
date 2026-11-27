#include "editor/ui/InspectorPanel.h"
#include "app/scene/GtaoSettingsJson.h"
#include "core/JsonField.h" // R9.6: jsonfield::Value -- value()'s conversions, no throw outside the loader
#if WITH_EDITOR

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/EditorExtensionRegistry.h"
#include "editor/scene/EditorZone.h"
#include "editor/scene/EditorRaceTracks.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/commands/CompositeCommand.h"
#include "editor/commands/EditEnvironmentCommand.h"
#include "editor/commands/EditObjectPropertiesCommand.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/RenameObjectCommand.h"
#include "editor/commands/SetEnabledCommand.h"
#include "editor/commands/SetMeshAssetCommand.h"
#include "editor/commands/SetMaterialCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "editor/ui/EditorDragDrop.h"
#include "editor/ui/EnvironmentInspector.h" // R10.3
#include "rendering/view/Camera.h"
#include "rendering/RenderLayers.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/lighting/PointLight.h"
#include "rendering/lighting/SpotLight.h"
#include "rendering/renderables/GBufferRenderable.h"
#include "rendering/renderables/RenderableObjectBase.h"
#include "rendering/renderables/TransparentStaticMesh.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "materials/MaterialDataManager.h"

namespace
{
    const EditorObject* FindEnvironmentObject(
        const EditorSceneDocument& document,
        EditorObjectId id)
    {
        for (const EditorObject& environment : document.Environment())
        {
            if (environment.id.value == id.value)
            {
                return &environment;
            }
        }
        return nullptr;
    }

    bool TryGetEnabled(const EditorSceneDocument& document,
        EditorObjectId id,
        bool& outEnabled)
    {
        if (const EditorObject* object = document.Find(id))
        {
            outEnabled = object->enabled;
            return true;
        }

        const EditorObject* environment = FindEnvironmentObject(document, id);
        if (!environment || (environment->type != "pointLight" &&
            environment->type != "spotLight" &&
            environment->type != "ocean"))
        {
            return false;
        }
        outEnabled = jsonfield::Value(environment->properties, "enabled", true);
        return true;
    }

    std::unique_ptr<EditorCommand> BuildEnabledCommand(
        const EditorSceneDocument& document,
        EditorObjectId id,
        bool enabled)
    {
        if (document.Find(id))
        {
            return std::make_unique<SetEnabledCommand>(id, enabled);
        }

        const EditorObject* environment = FindEnvironmentObject(document, id);
        if (!environment)
        {
            return nullptr;
        }
        nlohmann::json after = environment->properties;
        after["enabled"] = enabled;
        return std::make_unique<EditEnvironmentCommand>(
            id,
            environment->properties,
            std::move(after),
            enabled ? "Enable Environment" : "Disable Environment");
    }

    void DrawSharedEnabled(EditorContext& ctx, EditorCommandStack& commandStack)
    {
        bool enabled = true;
        bool first = true;
        bool mixed = false;
        for (const EditorObjectId id : ctx.selection.Ordered())
        {
            bool objectEnabled = true;
            if (!TryGetEnabled(ctx.document, id, objectEnabled))
            {
                ImGui::TextDisabled("Enabled is not shared by this selection.");
                return;
            }
            if (first)
            {
                enabled = objectEnabled;
                first = false;
            }
            else
            {
                mixed |= enabled != objectEnabled;
            }
        }

        if (mixed)
        {
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        }
        const bool changed = ImGui::Checkbox("Enabled", &enabled);
        if (mixed)
        {
            ImGui::PopItemFlag();
        }
        if (!changed)
        {
            return;
        }

        auto composite = std::make_unique<CompositeCommand>(
            std::string(enabled ? "Enable " : "Disable ") +
            std::to_string(ctx.selection.Size()) + " Objects");
        for (const EditorObjectId id : ctx.selection.Ordered())
        {
            bool currentEnabled = true;
            if (TryGetEnabled(ctx.document, id, currentEnabled) && currentEnabled == enabled)
            {
                continue; // A no-op environment command must not abort the composite.
            }
            std::unique_ptr<EditorCommand> command =
                BuildEnabledCommand(ctx.document, id, enabled);
            if (command)
            {
                composite->Add(std::move(command));
            }
        }
        commandStack.Execute(ctx, std::move(composite));
    }

    bool IsMeshAsset(const EditorAssetRecord& record)
    {
        return record.id.type == EditorAssetType::Mesh && record.extension == ".mesh.json";
    }

    const EditorAssetRecord* FindMeshAsset(const AssetRegistry& registry, const std::string& key)
    {
        for (const EditorAssetRecord& record : registry.Assets())
        {
            if (IsMeshAsset(record) && record.id.key == key)
            {
                return &record;
            }
        }
        return nullptr;
    }

    void DrawMeshAssetSelector(EditorContext& ctx,
        EditorCommandStack& commandStack,
        const AssetRegistry& registry,
        const EditorObject& object)
    {
        if (object.type != "staticMesh")
        {
            return;
        }

        const std::string current = jsonfield::Value(object.properties, "mesh", std::string());
        const EditorAssetRecord* currentRecord = FindMeshAsset(registry, current);

        std::string currentLabel;
        if (currentRecord)
        {
            currentLabel = currentRecord->displayName;
        }
        else if (!current.empty())
        {
            currentLabel = std::filesystem::path(current).filename().string() + " (missing)";
        }
        else
        {
            const std::string legacyModel = jsonfield::Value(object.properties, "model", std::string());
            currentLabel = legacyModel.empty()
                ? std::string("(none)")
                : std::filesystem::path(legacyModel).filename().string() + " (legacy)";
        }

        if (!ImGui::BeginCombo("Mesh Asset", currentLabel.c_str()))
        {
            return;
        }

        int assetCount = 0;
        for (const EditorAssetRecord& record : registry.Assets())
        {
            if (!IsMeshAsset(record))
            {
                continue;
            }

            ++assetCount;
            const bool selected = record.id.key == current;
            ImGui::PushID(record.id.key.c_str());
            if (ImGui::Selectable(record.displayName.c_str(), selected) && !selected)
            {
                commandStack.Execute(ctx,
                    std::make_unique<SetMeshAssetCommand>(object.id, record.id.key));
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", record.id.key.c_str());
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        if (assetCount == 0)
        {
            ImGui::TextDisabled("No .mesh.json assets found.");
        }
        ImGui::EndCombo();
    }

    void DrawMeshEditorButton(EditorContext& ctx,
        const AssetRegistry& registry,
        const EditorObject& object)
    {
        if (object.type != "staticMesh")
        {
            return;
        }

        const std::string meshKey = jsonfield::Value(object.properties, "mesh", std::string());
        const EditorAssetRecord* meshAsset = FindMeshAsset(registry, meshKey);
        const bool canOpenMesh = meshAsset && static_cast<bool>(ctx.openMeshEditor);
        ImGui::BeginDisabled(!canOpenMesh);
        if (ImGui::Button("Open Mesh Editor"))
        {
            ctx.openMeshEditor(meshAsset->path);
        }
        ImGui::EndDisabled();
        if (!canOpenMesh && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("The selected object has no editable .mesh.json asset.");
        }
    }

    void DrawInspectorDropTarget(EditorContext& ctx,
        EditorCommandStack& commandStack,
        const AssetRegistry& registry,
        EditorObject* object)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (!window || window->SkipItems)
        {
            return;
        }

        const ImRect targetRect = window->WorkRect;
        if (!ImGui::BeginDragDropTargetCustom(targetRect, ImGui::GetID("##inspectorDropTarget")))
        {
            return;
        }

        constexpr ImGuiDragDropFlags flags =
            ImGuiDragDropFlags_AcceptBeforeDelivery |
            ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(EditorDragDrop::kAssetPayloadType, flags))
        {
            EditorAssetId assetId;
            const EditorAssetRecord* record = nullptr;
            if (EditorDragDrop::DecodeAssetPayload(payload, assetId))
            {
                record = registry.FindById(assetId);
            }

            const char* reason = nullptr;
            if (!record)
            {
                reason = "Dragged asset is no longer in the registry.";
            }
            else if (record->id.type != EditorAssetType::MaterialPreset)
            {
                reason = "Only material assets can be dropped on the Inspector.";
            }
            else if (!object)
            {
                reason = "Select a static mesh object first.";
            }
            else if (object->type != "staticMesh")
            {
                reason = "Selected object does not support material assignment.";
            }

            if (reason)
            {
                ImGui::SetTooltip("%s", reason);
            }
            else
            {
                window->DrawList->AddRect(targetRect.Min,
                    targetRect.Max,
                    ImGui::GetColorU32(ImGuiCol_DragDropTarget),
                    0.0f,
                    0,
                    2.0f);
                ImGui::SetTooltip("Assign material: %s", record->displayName.c_str());
                if (payload->IsDelivery())
                {
                    commandStack.Execute(ctx,
                        std::make_unique<SetMaterialCommand>(object->id, record->id.key));
                }
            }
        }

        if (ImGui::AcceptDragDropPayload(EditorDragDrop::kFolderPayloadType, flags))
        {
            ImGui::SetTooltip("Folder drops do not change Inspector properties.");
        }

        ImGui::EndDragDropTarget();
    }

}

void InspectorPanel::Draw(EditorContext& ctx,
    EditorCommandStack& history,
    const AssetRegistry& registry,
    const EditorExtensionRegistry& extensions,
    bool* open)
{
    CPU_SCOPE(ProfilerScopes::kInspectorDraw);
    ImGui::SetNextWindowSize(ImVec2(340.0f, 460.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector", open))
    {
        multiEdit_.Finish(ctx, history);
        ImGui::End();
        return;
    }

    const EditorObjectId primary = ctx.selection.Primary();
    const bool multiSelection = ctx.selection.Size() > 1;
    if (multiSelection)
    {
        ImGui::Text("%zu objects selected", ctx.selection.Size());
        DrawSharedEnabled(ctx, history);
        ImGui::Separator();
    }
    // A race gate or course selected (its marks): the gate's or the course's own section first (EditorRaceTracks.h);
    // the marks' shared properties follow as for any selection.
    editorrace::DrawInspector(ctx, history);

    const bool sharedProperties = multiEdit_.Begin(ctx, history);
    if (multiSelection && !sharedProperties)
    {
        ImGui::TextWrapped("Select objects of the same type to edit shared properties.");
        ImGui::End();
        return;
    }
    EditorCommandStack& commandStack = sharedProperties ? multiEdit_.DrawCommands() : history;
    struct EndSharedEdit
    {
        InspectorMultiEdit& edit;
        EditorContext& ctx;
        EditorCommandStack& history;
        bool enabled;
        ~EndSharedEdit() { if (enabled) { edit.End(ctx, history, ImGui::IsAnyItemActive()); } }
    } endSharedEdit{ multiEdit_, ctx, history, sharedProperties };
    if (sharedProperties)
    {
        nameEditActive_ = false;
        nameEditObject_ = EditorObjectId{};
        ImGui::TextWrapped("Edits apply to all selected objects; untouched fields stay individual.");
        if (multiEdit_.HasMixedValues())
        {
            ImGui::TextDisabled("Mixed values: showing the active object's values.");
        }
        ImGui::Separator();
    }

    EditorObject* obj = ctx.document.Find(primary);
    if (!obj)
    {
        nameEditActive_ = false;
        nameEditObject_ = EditorObjectId{};

        // Environment entities (camera/lights/skybox/ocean) live in a separate list.
        for (EditorObject& env : ctx.document.Environment())
        {
            if (env.id.value != primary.value) { continue; }
            ImGui::Text("Selection: %s", env.name.c_str());
            ImGui::TextDisabled("Type: %s | ID: %llu",
                env.type.c_str(),
                static_cast<unsigned long long>(env.id.value));
            ImGui::Separator();
            EnvironmentInspector(ctx, commandStack, registry, env, !multiSelection, environmentEditObject_,
                environmentPropertiesBeforeEdit_, scrollToGroup_).Draw();
            DrawInspectorDropTarget(ctx, commandStack, registry, &env);
            ImGui::End();
            return;
        }

        ImGui::TextDisabled("No Selection");
        DrawInspectorDropTarget(ctx, commandStack, registry, nullptr);
        ImGui::End();
        return;
    }

    ImGui::Text("Selection: %s", obj->name.c_str());
    ImGui::TextDisabled("Type: %s | ID: %llu",
        obj->type.c_str(),
        static_cast<unsigned long long>(obj->id.value));

    if (nameEditObject_.value != obj->id.value || !nameEditActive_)
    {
        nameEditObject_ = obj->id;
        std::snprintf(nameEditBuffer_, sizeof(nameEditBuffer_), "%s", obj->name.c_str());
    }

    ImGui::BeginDisabled(multiSelection);
    const bool submitted = ImGui::InputText(
        "Name",
        nameEditBuffer_,
        sizeof(nameEditBuffer_),
        ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemActivated())
    {
        nameEditActive_ = true;
        nameBeforeEdit_ = obj->name;
    }

    const bool cancelNameEdit =
        nameEditActive_ && ImGui::IsKeyPressed(ImGuiKey_Escape);
    const bool finishNameEdit =
        submitted || (nameEditActive_ && ImGui::IsItemDeactivated());
    if (cancelNameEdit)
    {
        nameEditActive_ = false;
        std::snprintf(nameEditBuffer_, sizeof(nameEditBuffer_), "%s", obj->name.c_str());
    }
    else if (finishNameEdit)
    {
        nameEditActive_ = false;
        commandStack.Execute(ctx, std::make_unique<RenameObjectCommand>(
            obj->id,
            nameBeforeEdit_,
            std::string(nameEditBuffer_)));
        std::snprintf(nameEditBuffer_, sizeof(nameEditBuffer_), "%s", obj->name.c_str());
    }
    ImGui::EndDisabled();

    if (!multiSelection)
    {
        bool enabled = obj->enabled;
        if (ImGui::Checkbox("Enabled", &enabled))
        {
            commandStack.Execute(ctx, std::make_unique<SetEnabledCommand>(obj->id, enabled));
        }
    }

    DrawMeshAssetSelector(ctx, commandStack, registry, *obj);
    DrawMeshEditorButton(ctx, registry, *obj);

    if (obj->properties.contains("model") && obj->properties["model"].is_string())
    {
        ImGui::Text("Model: %s", obj->properties["model"].get<std::string>().c_str());
    }

    ImGui::Separator();
    DrawTransformEditor(ctx, commandStack, *obj);
    DrawZoneEditor(ctx, commandStack, *obj);

    if (const IEditorPropertyDrawer* drawer = extensions.FindPropertyDrawer(obj->type))
    {
        ImGui::Separator();
        drawer->Draw(ctx, commandStack, registry, *obj);
    }
    // ScrollToGroup on an object (--inspect="<name>#end"): its rows have no group anchors, so any group scrolls to the end.
    if (!scrollToGroup_.empty())
    {
        ImGui::SetScrollHereY(1.0f);
        scrollToGroup_.clear();
    }

    DrawInspectorDropTarget(ctx, commandStack, registry, obj);
    ImGui::End();
}

void InspectorPanel::DrawZoneEditor(EditorContext& ctx, EditorCommandStack& commandStack,
    EditorObject& object)
{
    if (object.type != editorzone::kTypeName)
    {
        return;
    }
    editorzone::Zone zone;
    if (!editorzone::FromObject(object, zone))
    {
        return;
    }

    ImGui::Separator();
    if (!ImGui::CollapsingHeader("Zone", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    // Every edit here goes through one snapshot-and-command path. A zone's whole definition
    // lives in `properties`, so there is nothing to be gained from a command per field, and
    // one entry per gesture is what undo should feel like.
    const nlohmann::json before = object.properties;
    bool changed = false;

    const char* shapes[] = { "circle", "rect", "spline" };
    const std::string current = jsonfield::Value(object.properties, "shape", "circle");
    int shapeIndex = current == "rect" ? 1 : current == "spline" ? 2 : 0;
    if (ImGui::Combo("Shape", &shapeIndex, shapes, IM_ARRAYSIZE(shapes)))
    {
        object.properties["shape"] = shapes[shapeIndex];
        // Switching INTO a spline with no points would give a zone that silently falls back
        // to a circle. Seed it from the size it already had, so the change is visible.
        if (shapeIndex == 2 && editorzone::LocalPoints(object).size() < 2)
        {
            const float size = std::max(jsonfield::Value(object.properties, "radius", 25.0f),
                jsonfield::Value(object.properties, "halfX", 25.0f));
            editorzone::StoreLocalPoints(object, {
                Math::float3(-size, 0.0f, -size * 0.4f),
                Math::float3(-size * 0.35f, 0.0f, size * 0.45f),
                Math::float3(size * 0.35f, 0.0f, size * 0.45f),
                Math::float3(size, 0.0f, -size * 0.4f),
            });
            object.properties["halfWidth"] = size * 0.25f;
        }
        changed = true;
    }

    if (shapeIndex == 2)
    {
        bool closed = jsonfield::Value(object.properties, "closed", false);
        if (ImGui::Checkbox("Closed loop", &closed))
        {
            object.properties["closed"] = closed;
            changed = true;
        }

        const char* fills[] = { "along the line", "inside the outline" };
        int fillIndex = jsonfield::Value(object.properties, "fill", std::string("along")) == "inside" ? 1 : 0;
        if (ImGui::Combo("Plant", &fillIndex, fills, IM_ARRAYSIZE(fills)))
        {
            object.properties["fill"] = fillIndex == 1 ? "inside" : "along";
            changed = true;
        }
        if (fillIndex == 1 && !closed)
        {
            ImGui::TextDisabled("(open spline; it is closed automatically to fill it)");
        }
        if (fillIndex == 0 && closed)
        {
            // Saying it, because the two readings of "in this zone" are genuinely different
            // and only one of them is on screen.
            ImGui::TextDisabled("(finding objects \"in this zone\" uses the whole enclosed\n"
                "area; this setting only decides where NEW ones go)");
        }

        if (fillIndex == 0)
        {
            float halfWidth = jsonfield::Value(object.properties, "halfWidth", 8.0f);
            if (ImGui::DragFloat("Half width", &halfWidth, 0.1f, 0.25f, 500.0f, "%.1f m"))
            {
                object.properties["halfWidth"] = std::max(0.25f, halfWidth);
                changed = true;
            }
        }

        const char* planes[] = { "XZ (ground plan)", "XY (front)", "ZY (side)" };
        const std::string planeNow = jsonfield::Value(object.properties, "plane", std::string("xz"));
        int planeIndex = planeNow == "xy" ? 1 : planeNow == "zy" ? 2 : 0;
        if (ImGui::Combo("Measured in", &planeIndex, planes, IM_ARRAYSIZE(planes)))
        {
            object.properties["plane"] = planeIndex == 1 ? "xy" : planeIndex == 2 ? "zy" : "xz";
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Points are full 3D. This picks the two axes that \"inside\" "
                "and \"within the band\" are measured in -- a trail over ground is XZ, a "
                "line up a cliff face is XY.");
        }

        float pointRadius = jsonfield::Value(object.properties, "pointRadius", 1.5f);
        if (ImGui::DragFloat("Point size", &pointRadius, 0.05f, 0.05f, 50.0f, "%.2f m"))
        {
            object.properties["pointRadius"] = std::max(0.05f, pointRadius);
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Radius of the draggable spheres, and of the click test that "
                "picks them -- what you see is what you hit.");
        }

        std::vector<Math::float3> points = editorzone::LocalPoints(object);
        ImGui::TextDisabled("%zu points -- click one to work on it", points.size());
        // Its own scrolling region, ten rows deep. A coastline is thirty points and the
        // list was pushing everything else off the panel; and without a row to click,
        // nothing told you WHICH of them is the one out in the water.
        const float rowHeight = ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginChild("##zonePoints", ImVec2(0.0f, rowHeight * 10.0f), true))
        {
            // ONE HEIGHT FOR THE WHOLE ROW, and it has to be said explicitly because the
            // three widgets disagree by default: Selectable sizes itself to its text,
            // DragFloat3 to the frame, and SmallButton to something smaller than either.
            // Left alone they came out as three different heights side by side, none of
            // them lined up with the others.
            const ImGuiStyle& style = ImGui::GetStyle();
            const float rowH = ImGui::GetFrameHeight();
            const float indexW = 30.0f;
            // Square, so the glyph sits in the middle instead of in a wide slab.
            const float buttonW = rowH;
            // The numbers take whatever is left, so the list grows with the panel instead
            // of keeping a width that was right on one person's screen.
            const float dragW = (std::max)(120.0f,
                ImGui::GetContentRegionAvail().x - indexW - buttonW * 2.0f
                    - style.ItemSpacing.x * 3.0f - style.ScrollbarSize);

            for (std::size_t index = 0; index < points.size(); ++index)
            {
                ImGui::PushID(static_cast<int>(index));
                const bool active = ctx.selection.ActivePoint() == static_cast<int>(index);
                // The index is the label, so a row can be matched to the sphere that grew
                // when it was picked in the viewport, and the other way round.
                char label[32];
                std::snprintf(label, sizeof(label), "%zu", index);
                ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
                if (ImGui::Selectable(label, active, 0, ImVec2(indexW, rowH)))
                {
                    ctx.selection.SetActivePoint(active ? -1 : static_cast<int>(index));
                }
                ImGui::PopStyleVar();
                ImGui::SameLine();
                float xyz[3] = { points[index].x, points[index].y, points[index].z };
                ImGui::SetNextItemWidth(dragW);
                if (ImGui::DragFloat3("##xyz", xyz, 0.1f, 0.0f, 0.0f, "%.1f"))
                {
                    points[index] = Math::float3(xyz[0], xyz[1], xyz[2]);
                    editorzone::StoreLocalPoints(object, points);
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("+", ImVec2(buttonW, rowH)))
                {
                    const int added = editorzone::InsertPointAfter(object,
                        static_cast<int>(index));
                    if (added >= 0)
                    {
                        ctx.selection.SetActivePoint(added);
                    }
                    changed = true;
                }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Insert a point after this one"); }
                ImGui::SameLine();
                // Disabled rather than hidden at the floor, so the reason is visible instead
                // of the button vanishing and leaving someone hunting for it.
                ImGui::BeginDisabled(points.size() <= 2);
                if (ImGui::Button("-", ImVec2(buttonW, rowH)))
                {
                    if (editorzone::RemovePoint(object, static_cast<int>(index)))
                    {
                        // The index would now point at a different point, or at none.
                        ctx.selection.SetActivePoint(-1);
                    }
                    changed = true;
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(points.size() <= 2 ? "A spline needs at least two points"
                                                         : "Remove this point");
                }
                ImGui::PopID(); 
                {
                    break;   // the list just changed shape; redraw it next frame
                }
            }
        }
        ImGui::EndChild();
    }
    else
    {
        if (shapeIndex == 0)
        {
            float radius = jsonfield::Value(object.properties, "radius", 25.0f);
            if (ImGui::DragFloat("Radius", &radius, 0.1f, 0.1f, 2000.0f, "%.1f m"))
            {
                object.properties["radius"] = std::max(0.1f, radius);
                changed = true;
            }
        }
        else
        {
            float half[2] = { jsonfield::Value(object.properties, "halfX", 25.0f),
                jsonfield::Value(object.properties, "halfZ", 25.0f) };
            if (ImGui::DragFloat2("Half extents", half, 0.1f, 0.1f, 2000.0f, "%.1f m"))
            {
                object.properties["halfX"] = std::max(0.1f, half[0]);
                object.properties["halfZ"] = std::max(0.1f, half[1]);
                changed = true;
            }
        }
        ImGui::TextDisabled("Scale in the transform above resizes this too.");
    }

    if (changed)
    {
        nlohmann::json after = object.properties;
        object.properties = before;
        commandStack.Execute(ctx, std::make_unique<EditObjectPropertiesCommand>(
            object.id, before, std::move(after), "Edit Zone"));
    }
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

    if (changed)
    {
        TransformObjectCommand::ApplyTransform(ctx, object.id, t);
    }
    if (committed)
    {
        commandStack.Execute(ctx, std::make_unique<TransformObjectCommand>(
            object.id, transformBeforeEdit_, object.transform));
    }
}

#endif // WITH_EDITOR
