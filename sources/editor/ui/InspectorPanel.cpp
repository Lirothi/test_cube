#include "editor/ui/InspectorPanel.h"
#if WITH_EDITOR

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app/scene/Scene.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/EditorExtensionRegistry.h"
#include "editor/scene/EnvironmentRuntime.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/commands/CompositeCommand.h"
#include "editor/commands/EditEnvironmentCommand.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/RenameObjectCommand.h"
#include "editor/commands/SetEnabledCommand.h"
#include "editor/commands/SetMaterialCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "editor/ui/EditorDragDrop.h"
#include "app/Systems.h"
#include "app/camera/Camera.h"
#include "ocean/OceanSimulation.h"
#include "rendering/RenderLayers.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/lighting/PointLight.h"
#include "rendering/lighting/SpotLight.h"
#include "rendering/renderables/GBufferRenderable.h"
#include "rendering/renderables/RenderableObjectBase.h"
#include "rendering/renderables/TransparentStaticMesh.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace
{
    float JsonFloat(const nlohmann::json& p, const char* key, float def)
    {
        const auto it = p.find(key);
        return (it != p.end() && it->is_number()) ? it->get<float>() : def;
    }

    Math::float3 JsonFloat3(const nlohmann::json& p, const char* key, const Math::float3& def)
    {
        const auto it = p.find(key);
        if (it != p.end() && it->is_array() && it->size() >= 3)
        {
            return Math::float3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
        }
        return def;
    }

    std::string NormalizePath(std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        return path;
    }

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
            environment->type != "directionalLight" &&
            environment->type != "ocean"))
        {
            return false;
        }
        outEnabled = environment->properties.value("enabled", true);
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
            std::unique_ptr<EditorCommand> command =
                BuildEnabledCommand(ctx.document, id, enabled);
            if (command)
            {
                composite->Add(std::move(command));
            }
        }
        commandStack.Execute(ctx, std::move(composite));
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

    // Inspector for the top-level environment sections (Step 24). Edits write the
    // entity's `properties` (round-tripped on save via the entity-driven
    // serializer) and patch the live runtime; lights/camera update instantly,
    // skybox texture edits rebuild the live skybox.
    void DrawEnvironmentInspector(
        EditorContext& ctx,
        EditorCommandStack& commandStack,
        const AssetRegistry& registry,
        EditorObject& env,
        bool showEnabled,
        EditorObjectId& activeEditObject,
        nlohmann::json& propertiesBeforeEdit)
    {
        nlohmann::json& p = env.properties;
        const std::string historyLabel =
            env.type == "pointLight" ? "Edit Point Light" :
            env.type == "spotLight" ? "Edit Spot Light" :
            env.type == "directionalLight" ? "Edit Directional Light" :
            env.type == "camera" ? "Edit Camera" :
            env.type == "skybox" ? "Edit Skybox" :
            env.type == "ocean" ? "Edit Ocean" :
            "Edit Environment";

        const auto executeChange = [&](nlohmann::json after, const std::string& label)
        {
            commandStack.Execute(ctx, std::make_unique<EditEnvironmentCommand>(
                env.id,
                p,
                std::move(after),
                label));
        };

        const auto trackContinuousEdit = [&](const nlohmann::json& beforeItem, bool changed)
        {
            if (ImGui::IsItemActivated())
            {
                activeEditObject = env.id;
                propertiesBeforeEdit = beforeItem;
            }
            if (changed)
            {
                EnvironmentRuntime::Apply(ctx, env);
                ctx.document.SetDirty(true);
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                const nlohmann::json before =
                    activeEditObject.value == env.id.value ?
                        propertiesBeforeEdit :
                        beforeItem;
                commandStack.Execute(ctx, std::make_unique<EditEnvironmentCommand>(
                    env.id,
                    before,
                    p,
                    historyLabel));
                activeEditObject = EditorObjectId{};
            }
        };

        const bool supportsEnable =
            env.type == "pointLight" || env.type == "spotLight" ||
            env.type == "directionalLight" || env.type == "ocean";
        if (showEnabled && supportsEnable)
        {
            bool enabled = p.value("enabled", true);
            if (ImGui::Checkbox("Enabled", &enabled))
            {
                nlohmann::json after = p;
                after["enabled"] = enabled;
                executeChange(
                    std::move(after),
                    enabled ? "Enable Environment" : "Disable Environment");
            }
            ImGui::Separator();
        }

        auto colorEdit = [&]()
        {
            const nlohmann::json beforeItem = p;
            const Math::float3 c = JsonFloat3(p, "color", Math::float3(1.0f, 1.0f, 1.0f));
            float cv[3] = { c.x, c.y, c.z };
            const bool changed = ImGui::ColorEdit3("Color", cv);
            if (changed) { p["color"] = { cv[0], cv[1], cv[2] }; }
            trackContinuousEdit(beforeItem, changed);
        };
        auto dragF = [&](const char* label, const char* key, float def, float speed, float lo, float hi)
        {
            const nlohmann::json beforeItem = p;
            float v = JsonFloat(p, key, def);
            const bool changed = ImGui::DragFloat(label, &v, speed, lo, hi);
            if (changed) { p[key] = v; }
            trackContinuousEdit(beforeItem, changed);
        };
        auto dragF3 = [&](const char* label, const char* key, const Math::float3& def, float speed)
        {
            const nlohmann::json beforeItem = p;
            const Math::float3 d3 = JsonFloat3(p, key, def);
            float v[3] = { d3.x, d3.y, d3.z };
            const bool changed = ImGui::DragFloat3(label, v, speed);
            if (changed) { p[key] = { v[0], v[1], v[2] }; }
            trackContinuousEdit(beforeItem, changed);
        };
        auto checkB = [&](const char* label, const char* key, bool def)
        {
            bool v = p.value(key, def);
            if (ImGui::Checkbox(label, &v))
            {
                nlohmann::json after = p;
                after[key] = v;
                executeChange(std::move(after), historyLabel);
            }
        };

        if (env.type == "pointLight")
        {
            colorEdit();
            dragF("Intensity", "intensity", 1.0f, 0.1f, 0.0f, 1000.0f);
            dragF("Radius", "radius", 1.0f, 0.05f, 0.0f, 1000.0f);
            dragF3("Position", "position", Math::float3(0.0f, 0.0f, 0.0f), 0.05f);
            checkB("Cast Shadows", "shadowsEnabled", false);

            bool flickerEnabled = p.contains("flicker") && p["flicker"].is_object();
            if (ImGui::Checkbox("Flicker", &flickerEnabled))
            {
                nlohmann::json after = p;
                if (flickerEnabled)
                {
                    after["flicker"] = {
                        { "amplitude", 0.35f },
                        { "frequencyHz", 7.0f },
                        { "seed", 3 }
                    };
                }
                else
                {
                    after.erase("flicker");
                }
                executeChange(std::move(after), "Toggle Point Light Flicker");
            }

            if (flickerEnabled)
            {
                ImGui::SeparatorText("Flicker");
                auto dragFlicker = [&](const char* label, const char* key,
                    float def, float speed, float lo, float hi)
                {
                    const nlohmann::json beforeItem = p;
                    float value = JsonFloat(p["flicker"], key, def);
                    const bool changed = ImGui::DragFloat(label, &value, speed, lo, hi);
                    if (changed) { p["flicker"][key] = value; }
                    trackContinuousEdit(beforeItem, changed);
                };
                dragFlicker("Amplitude", "amplitude", 0.35f, 0.01f, 0.0f, 1.0f);
                dragFlicker("Frequency (Hz)", "frequencyHz", 7.0f, 0.1f, 0.0f, 60.0f);

                const nlohmann::json beforeItem = p;
                int seed = p["flicker"].value("seed", 3);
                const bool seedChanged = ImGui::DragInt("Seed", &seed, 1.0f, 0, 1000000);
                if (seedChanged) { p["flicker"]["seed"] = std::max(seed, 0); }
                trackContinuousEdit(beforeItem, seedChanged);
            }
        }
        else if (env.type == "spotLight")
        {
            colorEdit();
            dragF("Intensity", "intensity", 5.0f, 0.1f, 0.0f, 1000.0f);
            dragF("Range", "range", 10.0f, 0.1f, 0.0f, 10000.0f);
            dragF("Inner Angle (deg)", "innerAngleDeg", 15.0f, 0.2f, 0.0f, 89.0f);
            dragF("Outer Angle (deg)", "outerAngleDeg", 25.0f, 0.2f, 0.0f, 89.0f);
            dragF3("Position", "position", Math::float3(0.0f, 0.0f, 0.0f), 0.05f);
            dragF3("Direction", "direction", Math::float3(0.0f, -1.0f, 0.0f), 0.01f);
            checkB("Cast Shadows", "shadowsEnabled", false);
            dragF("Shadow Normal Bias", "shadowNormalBias", 0.05f, 0.001f, 0.0f, 10.0f);
            dragF("Shadow Depth Bias", "shadowDepthBias", 0.0001f, 0.00005f, 0.0f, 1.0f);
        }
        else if (env.type == "directionalLight")
        {
            colorEdit();
            dragF("Exposure", "exposure", 1.0f, 0.05f, 0.0f, 100.0f);
            dragF("Ambient", "ambient", 0.05f, 0.005f, 0.0f, 10.0f);
            dragF3("Direction", "direction", Math::float3(-1.0f, -1.0f, -1.0f), 0.01f);
        }
        else if (env.type == "camera")
        {
            dragF("H FOV (deg)", "hfovDeg", 90.0f, 0.5f, 1.0f, 179.0f);
            dragF("Z Near", "zNear", 0.01f, 0.001f, 0.0001f, 100.0f);
            dragF("Z Far", "zFar", 10000.0f, 1.0f, 0.1f, 1000000.0f);
        }
        else if (env.type == "skybox")
        {
            const std::string current = p.value("texture", std::string());
            const std::string currentLabel = current.empty()
                ? std::string("(none)")
                : std::filesystem::path(current).filename().string();

            if (ImGui::BeginCombo("Texture Asset", currentLabel.c_str()))
            {
                int visibleTextures = 0;
                for (const EditorAssetRecord& rec : registry.Assets())
                {
                    if (rec.id.type != EditorAssetType::Texture ||
                        !rec.texture.valid ||
                        rec.texture.kind != EditorTextureKind::TextureCube)
                    {
                        continue;
                    }

                    const bool selected = rec.id.key == current;
                    std::string label = rec.id.key;
                    if (!rec.texture.format.empty())
                    {
                        label += "  [";
                        label += rec.texture.format;
                        label += "]";
                    }
                    ++visibleTextures;
                    if (ImGui::Selectable(label.c_str(), selected) && !selected)
                    {
                        nlohmann::json after = p;
                        after["texture"] = NormalizePath(rec.id.key);
                        executeChange(std::move(after), "Set Skybox Texture");
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                if (visibleTextures == 0)
                {
                    ImGui::TextDisabled("No cubemap textures found.");
                }
                ImGui::EndCombo();
            }

            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s", p.value("texture", std::string()).c_str());
            const float applyButtonWidth = ImGui::CalcTextSize("Apply Texture").x +
                ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetNextItemWidth(std::max(120.0f,
                ImGui::GetContentRegionAvail().x - applyButtonWidth - ImGui::GetStyle().ItemSpacing.x));
            const nlohmann::json beforeItem = p;
            const bool textureChanged = ImGui::InputText("Texture", buf, sizeof(buf));
            if (ImGui::IsItemActivated())
            {
                activeEditObject = env.id;
                propertiesBeforeEdit = beforeItem;
            }
            if (textureChanged)
            {
                p["texture"] = NormalizePath(buf);
                ctx.document.SetDirty(true);
            }

            const bool textureCommitted = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine();
            const bool applyTexture = ImGui::Button("Apply Texture");
            if (textureCommitted)
            {
                const nlohmann::json before =
                    activeEditObject.value == env.id.value ?
                        propertiesBeforeEdit :
                        beforeItem;
                commandStack.Execute(ctx, std::make_unique<EditEnvironmentCommand>(
                    env.id,
                    before,
                    p,
                    "Set Skybox Texture"));
                activeEditObject = EditorObjectId{};
            }
            else if (applyTexture)
            {
                EnvironmentRuntime::Apply(ctx, env);
            }
        }
        else if (env.type == "ocean")
        {
            OceanSimulation* ocean = Systems::GetOceanSimulation();
            if (!ocean)
            {
                ImGui::TextDisabled("No ocean in this level.");
                ImGui::TextDisabled("Use the Ocean menu to create one.");
            }
            else
            {
                // Preset (reference): changing it reloads the sim live.
                const std::string curPreset = p.value("preset", std::string());
                const std::string curLabel = std::filesystem::path(curPreset).filename().string();
                if (ImGui::BeginCombo("Preset", curLabel.empty() ? "(none)" : curLabel.c_str()))
                {
                    for (const std::string& pr : EnvironmentRuntime::OceanPresets())
                    {
                        const bool sel = (pr == curPreset);
                        const std::string label = std::filesystem::path(pr).filename().string();
                        if (ImGui::Selectable(label.c_str(), sel) && !sel)
                        {
                            nlohmann::json after = p;
                            after["preset"] = pr;
                            executeChange(std::move(after), "Set Ocean Preset");
                        }
                    }
                    ImGui::EndCombo();
                }

                const nlohmann::json windForceBefore = p;
                float windForce = JsonFloat(p, "windForce", ocean->GetWindForce01());
                const bool windForceChanged =
                    ImGui::SliderFloat("Wind Force", &windForce, 0.0f, 1.0f);
                if (windForceChanged) { p["windForce"] = windForce; }
                trackContinuousEdit(windForceBefore, windForceChanged);

                const nlohmann::json windDirectionBefore = p;
                float windDirection = JsonFloat(
                    p,
                    "windDirectionDeg",
                    ocean->GetLocalWindDirectionDegrees());
                const bool windDirectionChanged = ImGui::DragFloat(
                    "Wind Direction",
                    &windDirection,
                    0.5f,
                    -360.0f,
                    360.0f,
                    "%.1f deg");
                if (windDirectionChanged) { p["windDirectionDeg"] = windDirection; }
                trackContinuousEdit(windDirectionBefore, windDirectionChanged);

                const nlohmann::json swellDirectionBefore = p;
                float swellDirection = JsonFloat(
                    p,
                    "swellDirectionDeg",
                    ocean->GetSwellDirectionDegrees());
                const bool swellDirectionChanged = ImGui::DragFloat(
                    "Swell Direction",
                    &swellDirection,
                    0.5f,
                    -360.0f,
                    360.0f,
                    "%.1f deg");
                if (swellDirectionChanged) { p["swellDirectionDeg"] = swellDirection; }
                trackContinuousEdit(swellDirectionBefore, swellDirectionChanged);
            }
        }
        else
        {
            ImGui::TextDisabled("No editable properties.");
        }
    }
}

void InspectorPanel::Draw(EditorContext& ctx,
    EditorCommandStack& commandStack,
    const AssetRegistry& registry,
    const EditorExtensionRegistry& extensions,
    bool* open)
{
    CPU_SCOPE(ProfilerScopes::kInspectorDraw);
    ImGui::SetNextWindowSize(ImVec2(340.0f, 460.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector", open))
    {
        ImGui::End();
        return;
    }

    const EditorObjectId primary = ctx.selection.Primary();
    const bool multiSelection = ctx.selection.Size() > 1;
    if (multiSelection)
    {
        ImGui::Text("%zu objects selected", ctx.selection.Size());
        DrawSharedEnabled(ctx, commandStack);
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
            DrawEnvironmentInspector(
                ctx,
                commandStack,
                registry,
                env,
                !multiSelection,
                environmentEditObject_,
                environmentPropertiesBeforeEdit_);
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

    if (!multiSelection)
    {
        bool enabled = obj->enabled;
        if (ImGui::Checkbox("Enabled", &enabled))
        {
            commandStack.Execute(ctx, std::make_unique<SetEnabledCommand>(obj->id, enabled));
        }
    }

    if (obj->properties.contains("model") && obj->properties["model"].is_string())
    {
        ImGui::Text("Model: %s", obj->properties["model"].get<std::string>().c_str());
    }

    ImGui::Separator();
    DrawTransformEditor(ctx, commandStack, *obj);

    if (const IEditorPropertyDrawer* drawer = extensions.FindPropertyDrawer(obj->type))
    {
        ImGui::Separator();
        drawer->Draw(ctx, commandStack, registry, *obj);
    }

    DrawInspectorDropTarget(ctx, commandStack, registry, obj);
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
