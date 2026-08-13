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
#include "editor/commands/SetMeshAssetCommand.h"
#include "editor/commands/SetMaterialCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "editor/ui/EditorDragDrop.h"
#include "app/Systems.h"
#include "app/camera/Camera.h"
#include "ocean/OceanRenderConfigJson.h"
#include "ocean/OceanRenderable.h" // ocean::g_shoreRunup gates the Shore and surf section
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

    bool IsMeshAsset(const EditorAssetRecord& record)
    {
        return record.id.type == EditorAssetType::Mesh && record.extension == ".mesh.json";
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

        const std::string current = object.properties.value("mesh", std::string());
        const EditorAssetRecord* currentRecord = nullptr;
        for (const EditorAssetRecord& record : registry.Assets())
        {
            if (IsMeshAsset(record) && record.id.key == current)
            {
                currentRecord = &record;
                break;
            }
        }

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
            const std::string legacyModel = object.properties.value("model", std::string());
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
    // skybox texture edits rebuild the live skybox, and wind edits update the shared WindState.
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
            env.type == "wind" ? "Edit Wind" :
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
        auto dragF = [&](const char* label, const char* key, float def, float speed, float lo, float hi,
                         const char* fmt = "%.3f")
        {
            const nlohmann::json beforeItem = p;
            float v = JsonFloat(p, key, def);
            const bool changed = ImGui::DragFloat(label, &v, speed, lo, hi, fmt);
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
            dragF("Shadow Normal Bias", "shadowNormalBias", 0.05f, 0.001f, 0.0f, 10.0f, "%.5f");
            dragF("Shadow Depth Bias", "shadowDepthBias", 0.0001f, 0.00005f, 0.0f, 1.0f, "%.6f");
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

                OceanRenderConfig render = ocean->GetRenderConfig();
                const auto renderIt = p.find("render");
                if (renderIt != p.end() && renderIt->is_object())
                {
                    OceanRenderConfigJson::ApplyOverrides(*renderIt, render);
                }

                const auto storeRender = [&]()
                {
                    p["render"] = OceanRenderConfigJson::ToJson(render);
                };
                const auto beginRenderContinuousEdit = [&]()
                {
                    if (ImGui::IsItemActivated())
                    {
                        activeEditObject = env.id;
                        propertiesBeforeEdit = p;
                    }
                };
                const auto finishRenderContinuousEdit = [&](bool changed)
                {
                    if (changed)
                    {
                        EnvironmentRuntime::Apply(ctx, env);
                        ctx.document.SetDirty(true);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        nlohmann::json before =
                            activeEditObject.value == env.id.value ?
                                std::move(propertiesBeforeEdit) :
                                p;
                        commandStack.Execute(ctx, std::make_unique<EditEnvironmentCommand>(
                            env.id,
                            std::move(before),
                            p,
                            historyLabel));
                        activeEditObject = EditorObjectId{};
                    }
                };
                const auto renderDrag = [&](const char* label,
                    float& value,
                    float speed,
                    float minimum,
                    float maximum,
                    const char* format = "%.3f")
                {
                    const bool changed = ImGui::DragFloat(
                        label, &value, speed, minimum, maximum, format);
                    beginRenderContinuousEdit();
                    if (changed)
                    {
                        value = std::clamp(value, minimum, maximum);
                        storeRender();
                    }
                    finishRenderContinuousEdit(changed);
                };
                const auto renderColor = [&](const char* label, Math::float4& color)
                {
                    float values[3] = { color.x, color.y, color.z };
                    const bool changed = ImGui::ColorEdit3(
                        label,
                        values,
                        ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                    beginRenderContinuousEdit();
                    if (changed)
                    {
                        color = Math::float4(values[0], values[1], values[2], color.w);
                        storeRender();
                    }
                    finishRenderContinuousEdit(changed);
                };
                const auto renderVector4 = [&](const char* label,
                    Math::float4& value,
                    float speed,
                    float minimum,
                    float maximum)
                {
                    float values[4] = { value.x, value.y, value.z, value.w };
                    const bool changed = ImGui::DragFloat4(
                        label, values, speed, minimum, maximum, "%.3f");
                    beginRenderContinuousEdit();
                    if (changed)
                    {
                        value = Math::float4(
                            std::clamp(values[0], minimum, maximum),
                            std::clamp(values[1], minimum, maximum),
                            std::clamp(values[2], minimum, maximum),
                            std::clamp(values[3], minimum, maximum));
                        storeRender();
                    }
                    finishRenderContinuousEdit(changed);
                };

                ImGui::TextDisabled("Render settings are stored in this level and override the preset.");

                if (ImGui::CollapsingHeader("Surface color", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    renderColor("Deep Scatter Tint", render.deepScatterColor);
                    renderColor("Subsurface Tint", render.sssColor);
                    renderColor("Diffuse Tint", render.diffuseColor);
                }

                if (ImGui::CollapsingHeader("Specular and reflection", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    renderDrag("Specular Strength", render.specularStrength, 0.01f, 0.0f, 10.0f);
                    renderDrag("Roughness Scale", render.roughnessScale, 0.005f, 0.0f, 1.0f);
                    renderDrag("Roughness Distance", render.roughnessDistance, 1.0f, 1.0f, 5000.0f, "%.1f");
                    renderDrag("Reflection Normal Flattening", render.reflectionNormalStrength, 0.005f, 0.0f, 1.0f);
                    renderDrag("Horizon Fog Strength", render.horizonFogStrength, 0.005f, 0.01f, 5.0f);
                    renderDrag("Horizon Fog Distance Scale", render.horizonFogDistanceScale, 0.01f, 0.0f, 20.0f);
                    renderDrag("Cascade Fade Scale", render.cascadeFadeScale, 0.1f, 0.0f, 1000.0f);
                    renderDrag("Minimum Mesh Scale", render.minMeshScale, 0.1f, 0.001f, 1000.0f);
                    renderDrag("Detail Normal Mip Bias", render.detailNormalMipBias, 0.05f, -4.0f, 4.0f);
                    renderDrag("Macro Normal Mip Bias (DLSS)", render.macroNormalMipBiasDlss, 0.05f, -4.0f, 4.0f);
                    renderDrag("Macro Normal Mip Bias (Native)", render.macroNormalMipBiasNative, 0.05f, -4.0f, 4.0f);
                }

                if (ImGui::CollapsingHeader("Refraction and volume"))
                {
                    renderDrag("Surface Refraction", render.surfaceRefractionStrength, 0.005f, 0.0f, 5.0f);
                    renderDrag("Underwater Refraction", render.underwaterRefractionStrength, 0.005f, 0.0f, 5.0f);
                    renderDrag("Absorption Depth", render.absorptionDepthScale, 0.1f, 1.0f, 1000.0f);
                    renderDrag("Fog Density", render.fogDensity, 0.005f, 0.0f, 10.0f);
                }

                if (ImGui::CollapsingHeader("Subsurface scattering"))
                {
                    renderDrag("Sun Scatter", render.sunScatterStrength, 0.01f, 0.0f, 10.0f);
                    renderDrag("Sky Scatter", render.skyScatterStrength, 0.01f, 0.0f, 10.0f);
                    renderDrag("Scatter Spread", render.scatterSpread, 0.005f, 0.001f, 2.0f);
                    renderDrag("View Alignment", render.viewAlignmentStrength, 0.005f, 0.0f, 1.0f);
                    renderDrag("Height Bias", render.sssHeightBias, 0.01f, -10.0f, 10.0f);
                    renderDrag("Distance Fade", render.sssFadeDistance, 0.1f, 0.0f, 1000.0f);
                }

                if (ImGui::CollapsingHeader("Wind shading"))
                {
                    renderDrag("Wind Speed", render.windSpeed, 0.1f, 0.0f, 100.0f);
                    renderDrag("Waves Scale", render.wavesScale, 0.01f, 0.0f, 10.0f);
                    renderDrag("Wind Alignment", render.windAlignment, 0.005f, 0.0f, 1.0f);
                    renderDrag("UV Warp Strength", render.windUvWarpStrength, 0.005f, 0.0f, 5.0f);
                }

                if (ImGui::CollapsingHeader("Shore and surf", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    // Mirrors OceanControlsWindow: the two surface variants read different
                    // settings, so only the live ones are shown per mode.
                    if (!ocean::g_shoreRunup)
                    {
                        ImGui::TextDisabled(
                            "Legacy ocean surface (relaunch with --ocean-runup-shore for the modern stack).");
                        renderDrag("Vertical Damp Strength", render.shoreLegacyVerticalDampStrength, 0.005f, 0.0f, 1.0f);
                        renderDrag("XZ Damp Strength", render.shoreLegacyXzDampStrength, 0.005f, 0.0f, 1.0f);
                        renderDrag("Damp Fade Depth", render.shoreLegacyDampFadeDepth, 0.05f, 0.01f, 50.0f);
                        // Shared with the modern surface: the same fields drive its normal fade.
                        renderDrag("Normal Fade Depth", render.shoreNormalFadeDepth, 0.01f, 0.01f, 10.0f);
                        renderVector4("Normal Cascade Minimums", render.shoreNormalMinWeights, 0.005f, 0.0f, 1.0f);
                        renderDrag(
                            "Contact Foam Strength",
                            render.shoreLegacyContactFoamStrength,
                            0.002f,
                            0.0f,
                            1.0f);
                    }
                    else
                    {
                    renderDrag("Vertical Fade Depth", render.shoreVerticalFadeDepth, 0.01f, 0.01f, 10.0f);
                    renderDrag("Shallow XZ Strength", render.shoreHorizontalMin, 0.005f, 0.0f, 1.0f);
                    renderDrag("XZ Restore Depth", render.shoreHorizontalFadeDepth, 0.01f, 0.01f, 10.0f);
                    renderDrag("Normal Fade Depth", render.shoreNormalFadeDepth, 0.01f, 0.01f, 10.0f);
                    renderVector4("Normal Cascade Minimums", render.shoreNormalMinWeights, 0.005f, 0.0f, 1.0f);
                    renderDrag("Run-up Depth", render.shoreRunupDepth, 0.01f, 0.01f, 10.0f);
                    renderDrag("Run-up Strength", render.shoreRunupStrength, 0.01f, 0.0f, 10.0f);
                    renderDrag("Run-up Max Wave", render.shoreRunupMaxWave, 0.01f, 0.0f, 10.0f);
                    renderDrag("Run-up Slope Fade Start", render.shoreRunupSlopeStartDegrees, 0.25f, 0.0f, 89.0f);
                    renderDrag("Run-up Slope Cutoff", render.shoreRunupSlopeEndDegrees, 0.25f, 0.0f, 89.0f);
                    renderDrag("Swash Amplitude", render.shoreSwashAmplitude, 0.005f, 0.0f, 1.0f);
                    renderDrag("Run-up Slope Smoothing", render.shoreRunupSlopeSmoothing, 0.05f, 0.5f, 8.0f);
                    renderDrag("Bottom Clearance", render.shoreBottomClearance, 0.005f, 0.0f, 1.0f);
                    renderDrag("Refraction Soft Edge Distance", render.shoreEdgeSoftDepth, 0.001f, 0.0f, 0.25f);
                    renderDrag(
                        "Geometry Edge Refraction Fade",
                        render.shoreGeometryEdgeRefractionFadeDepth,
                        0.005f,
                        0.0f,
                        2.0f);
                    renderDrag(
                        "Geometry Wave Fade Distance",
                        render.shoreGeometryFadeDistance,
                        1.0f,
                        1.0f,
                        2000.0f,
                        "%.1f");

                    if (ImGui::TreeNodeEx(
                        "Contact Foam",
                        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
                    {
                        ImGui::SeparatorText("Coverage");
                        renderDrag("Main Width", render.shoreContactFoamMainWidth, 0.002f, 0.0f, 1.0f);
                        renderDrag(
                            "Main Breakup Length",
                            render.shoreContactFoamBreakupLength,
                            0.001f,
                            0.0f,
                            1.0f);
                        renderDrag(
                            "Breakup Length Variation",
                            render.shoreContactFoamBreakupLengthVariation,
                            0.002f,
                            0.0f,
                            1.0f);
                        renderDrag(
                            "Breakup Variation Scale",
                            render.shoreContactFoamBreakupVariationScale,
                            0.002f,
                            0.001f,
                            1.0f);
                        renderDrag("Opacity", render.shoreContactFoamOpacity, 0.005f, 0.0f, 1.0f);

                        ImGui::SeparatorText("Wind Response");
                        renderDrag(
                            "Calm Amount",
                            render.shoreContactFoamCalmAmount,
                            0.005f,
                            0.0f,
                            1.0f);
                        renderDrag(
                            "Full At Wind Force",
                            render.shoreContactFoamFullWindForce,
                            0.005f,
                            0.01f,
                            1.0f);

                        ImGui::SeparatorText("Breakup Pattern");
                        renderDrag("Pattern Scale", render.shoreContactFoamPatternScale, 0.005f, 0.001f, 2.0f);
                        renderDrag("Pattern Density", render.shoreContactFoamPatternDensity, 0.005f, 0.0f, 1.0f);
                        renderDrag("Pattern Scroll Speed", render.shoreContactFoamPatternScrollSpeed, 0.01f, 0.0f, 10.0f);

                        ImGui::SeparatorText("Signed Depth Warp");
                        renderDrag(
                            "Depth Warp Scale",
                            render.shoreContactFoamDepthWarpScale,
                            0.002f,
                            0.001f,
                            2.0f);
                        renderDrag(
                            "Depth Warp Strength",
                            render.shoreContactFoamDepthWarpStrength,
                            0.002f,
                            0.0f,
                            0.5f);
                        renderDrag(
                            "Depth Warp Range",
                            render.shoreContactFoamDepthWarpRange,
                            0.005f,
                            0.0f,
                            2.0f);

                        ImGui::SeparatorText("Appearance");
                        renderDrag("Albedo Scale", render.shoreContactFoamAlbedoScale, 0.01f, 0.001f, 10.0f);
                        renderDrag("Albedo Scroll Speed", render.shoreContactFoamAlbedoScrollSpeed, 0.01f, 0.0f, 10.0f);
                        renderDrag(
                            "Normal Strength",
                            render.shoreContactFoamNormalStrength,
                            0.005f,
                            0.0f,
                            1.0f);
                        ImGui::TreePop();
                    }
                    } // modern shore stack (ocean::g_shoreRunup)
                }

                if (ImGui::CollapsingHeader("Foam"))
                {
                    renderColor("Foam Tint", render.foamTint);
                    renderDrag("Foam Normal Strength", render.foamNormalStrength, 0.005f, 0.0f, 1.0f);
                    renderDrag("Underwater Foam Parallax", render.underwaterFoamParallax, 0.01f, 0.0f, 10.0f);
                }

                if (ImGui::CollapsingHeader("Caustics"))
                {
                    bool causticsEnabled = render.causticsEnabled;
                    if (ImGui::Checkbox("Caustics Enabled", &causticsEnabled))
                    {
                        OceanRenderConfig afterRender = render;
                        afterRender.causticsEnabled = causticsEnabled;
                        nlohmann::json after = p;
                        after["render"] = OceanRenderConfigJson::ToJson(afterRender);
                        executeChange(std::move(after), "Set Ocean Caustics Enabled");
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip(
                            "Sun caustics on everything below the water line. Applied in the\n"
                            "deferred lighting pass, so they follow the sun shadow.");
                    }
                    renderDrag("Intensity", render.causticsIntensity, 0.01f, 0.0f, 6.0f);
                    renderDrag("Tile Size (m)", render.causticsScale, 0.05f, 0.25f, 60.0f);
                    renderDrag("Speed (frames/s)", render.causticsSpeed, 0.1f, 0.0f, 60.0f);
                    renderDrag("Depth Fade (m)", render.causticsDepthFade, 0.1f, 0.1f, 120.0f);
                    renderDrag("Surface Fade (m)", render.causticsSurfaceFade, 0.01f, 0.0f, 5.0f);
                    renderDrag("Up-Facing Gate", render.causticsUpFacing, 0.005f, 0.0f, 1.0f);
                    renderDrag("Dark Bias", render.causticsBias, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip(
                            "Pattern value that means \"no gain\". The generator prints the value\n"
                            "unfocused sunlight encodes to; at that setting the dark cells stay\n"
                            "neutral and only the cords brighten.");
                    }
                    renderDrag("Dispersion (texels)", render.causticsDispersion, 0.01f, 0.0f, 8.0f);
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("Chromatic split of the cords. 0 is monochrome and 3x cheaper.");
                    }
                    renderDrag("De-Tile Layer", render.causticsLayerBlend, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip(
                            "Second layer at another scale, min-combined, to hide the tiling\n"
                            "period. 0 halves the texture taps.");
                    }
                    renderColor("Caustics Tint", render.causticsTint);
                }

                if (ImGui::CollapsingHeader("Absorption gradient"))
                {
                    bool curvedGradient = render.absorptionGradientType >= 0.5f;
                    if (ImGui::Checkbox("Curved Interpolation", &curvedGradient))
                    {
                        OceanRenderConfig afterRender = render;
                        afterRender.absorptionGradientType = curvedGradient ? 1.0f : 0.0f;
                        nlohmann::json after = p;
                        after["render"] = OceanRenderConfigJson::ToJson(afterRender);
                        executeChange(std::move(after), "Set Ocean Absorption Gradient");
                    }

                    for (size_t index = 0; index < render.absorptionColors.size(); ++index)
                    {
                        ImGui::PushID(static_cast<int>(index));
                        Math::float4& key = render.absorptionColors[index];
                        renderColor("Color", key);
                        renderDrag("Position", key.w, 0.005f, 0.0f, 1.0f);
                        ImGui::Separator();
                        ImGui::PopID();
                    }

                    if (render.absorptionColors.size() < 8u && ImGui::Button("Add Key"))
                    {
                        OceanRenderConfig afterRender = render;
                        const Math::float4 last = afterRender.absorptionColors.empty()
                            ? Math::float4(1.0f, 1.0f, 1.0f, 1.0f)
                            : afterRender.absorptionColors.back();
                        afterRender.absorptionColors.push_back(last);
                        nlohmann::json after = p;
                        after["render"] = OceanRenderConfigJson::ToJson(afterRender);
                        executeChange(std::move(after), "Add Ocean Absorption Key");
                    }
                    if (render.absorptionColors.size() > 1u)
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("Remove Key"))
                        {
                            OceanRenderConfig afterRender = render;
                            afterRender.absorptionColors.pop_back();
                            nlohmann::json after = p;
                            after["render"] = OceanRenderConfigJson::ToJson(afterRender);
                            executeChange(std::move(after), "Remove Ocean Absorption Key");
                        }
                    }
                }

                ImGui::SeparatorText("Simulation overrides");
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
        else if (env.type == "wind")
        {
            const vfx::WindState& wind = ctx.scene.GetWindState();

            ImGui::SeparatorText("Flow");
            dragF("Direction", "directionDeg", wind.directionDeg,
                0.5f, -360.0f, 360.0f, "%.1f deg");

            const nlohmann::json strengthBefore = p;
            float strength = JsonFloat(p, "strength", wind.strength);
            const bool strengthChanged = ImGui::SliderFloat("Strength", &strength, 0.0f, 1.0f);
            if (strengthChanged) { p["strength"] = strength; }
            trackContinuousEdit(strengthBefore, strengthChanged);

            dragF("Sway Frequency", "swayFrequency", wind.swayFrequency,
                0.01f, 0.0f, 10.0f, "%.2f");
            dragF("Foliage Sway", "foliageSwayMeters", wind.foliageSwayMeters,
                0.01f, 0.0f, 5.0f, "%.2f m");

            ImGui::SeparatorText("Gusts");
            const auto dragGust = [&](const char* label, const char* key, float def,
                float speed, float lo, float hi, const char* fmt)
            {
                const nlohmann::json beforeItem = p;
                float value = def;
                const auto gustIt = p.find("gust");
                if (gustIt != p.end() && gustIt->is_object())
                {
                    value = JsonFloat(*gustIt, key, def);
                }
                const bool changed = ImGui::DragFloat(label, &value, speed, lo, hi, fmt);
                if (changed)
                {
                    if (!p.contains("gust") || !p["gust"].is_object())
                    {
                        p["gust"] = nlohmann::json::object();
                    }
                    p["gust"][key] = value;
                }
                trackContinuousEdit(beforeItem, changed);
            };
            dragGust("Amplitude", "amplitude", wind.gustAmplitude,
                0.01f, 0.0f, 2.0f, "%.2f");
            dragGust("Frequency", "frequencyHz", wind.gustFrequencyHz,
                0.005f, 0.0f, 2.0f, "%.3f Hz");
            ImGui::TextDisabled("Current envelope: %.2fx", wind.gustMul);

            // W8 distance fade. Also global rather than level data: the right distance depends on
            // the shot, and a value baked into a level would freeze foliage someone else framed
            // deliberately. Off while End <= Start.
            ImGui::SeparatorText("Distance Fade");
            ImGui::DragFloat("Fade Start", &vfx::g_windFadeStart, 1.0f, 0.0f, 5000.0f, "%.0f m");
            ImGui::DragFloat("Fade End", &vfx::g_windFadeEnd, 1.0f, 0.0f, 5000.0f, "%.0f m");
            if (!(vfx::g_windFadeEnd > vfx::g_windFadeStart))
            {
                ImGui::TextDisabled("Disabled (End must exceed Start)");
            }

            // W8 debug freeze. Deliberately NOT written into `p`: this is a viewing aid, not level
            // data, so it must never end up saved in the level or land in the undo stack.
            ImGui::SeparatorText("Debug");
            ImGui::Checkbox("Freeze time", &vfx::g_windFreeze);
            ImGui::BeginDisabled(!vfx::g_windFreeze);
            ImGui::SameLine();
            static float s_windStepSeconds = 1.0f; // UI-only; the freeze itself is a global
            if (ImGui::Button("Step")) { vfx::g_windStep = s_windStepSeconds; }
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Step size", &s_windStepSeconds, 0.01f, 0.001f, 10.0f, "%.3f s");
            ImGui::EndDisabled();
            ImGui::TextDisabled("Wind clock: %.3f s%s", wind.time,
                vfx::g_windFreeze ? " (frozen)" : "");
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

    DrawMeshAssetSelector(ctx, commandStack, registry, *obj);

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
