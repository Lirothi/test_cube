#include "editor/ui/InspectorPanel.h"
#include "app/scene/GtaoSettingsJson.h"
#if WITH_EDITOR

#include <algorithm>
#include <cctype>
#include <string_view>
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
#include "editor/ui/EditorLightDirection.h"
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
    // Case-insensitive suffix test, for hiding the IBL sibling cubes from the sky picker.
    bool EndsWithSuffix(std::string_view value, std::string_view suffix)
    {
        if (value.size() < suffix.size()) { return false; }
        const size_t off = value.size() - suffix.size();
        for (size_t i = 0; i < suffix.size(); ++i)
        {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(value[off + i])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
            if (a != b) { return false; }
        }
        return true;
    }

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

        const std::string current = object.properties.value("mesh", std::string());
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

    void DrawMeshEditorButton(EditorContext& ctx,
        const AssetRegistry& registry,
        const EditorObject& object)
    {
        if (object.type != "staticMesh")
        {
            return;
        }

        const std::string meshKey = object.properties.value("mesh", std::string());
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

    // Inspector for the top-level environment sections (Step 24). Edits write the
    // entity's `properties` (round-tripped on save via the entity-driven
    // serializer) and patch the live runtime; lights/camera update instantly,
    // skybox texture edits rebuild the live skybox, and wind edits update the shared WindState.
    // Mirrors the developer window's marker so the two surfaces explain a control identically --
    // the inspector is where a look is made permanent, so it is the surface that most needs the
    // explanation, not the least.
    void InspectorHelp(const char* text)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

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

        // Cap the width of every control in this inspector. ImGui defaults to roughly 65% of the
        // panel, which on a wide docked Inspector drags each slider halfway across the screen and
        // pushes its label out of eyeshot. A fixed cap keeps label and value together; the panel can
        // still be narrower, hence the min().
        const float controlWidth = std::min(220.0f, std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.5f));
        ImGui::PushItemWidth(controlWidth);
        struct ItemWidthScope
        {
            ~ItemWidthScope() { ImGui::PopItemWidth(); }
        } itemWidthScope;

        const std::string historyLabel =
            env.type == "pointLight" ? "Edit Point Light" :
            env.type == "spotLight" ? "Edit Spot Light" :
            env.type == "directionalLight" ? "Edit Directional Light" :
            env.type == "camera" ? "Edit Camera" :
            env.type == "skybox" ? "Edit Skybox" :
            env.type == "ocean" ? "Edit Ocean" :
            env.type == "wind" ? "Edit Wind" :
            env.type == "cameraExposure" ? "Edit Camera Exposure" :
            env.type == "colorPipeline" ? "Edit Color Pipeline" :
            env.type == "gtao" ? "Edit Ambient Occlusion" :
            env.type == "atmosphere" ? "Edit Aerial Perspective" :
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
            // P4. The default handed to the drag is the level's LEGACY `exposure`, which is exactly
            // the value the migration folded in -- so the row opens showing what is on screen, and
            // the first drag writes `sunIntensity` without the image jumping. Once that key exists
            // the legacy field is ignored (JsonLevel and EnvironmentRuntime both branch on it).
            checkB("Use Colour Temperature", "useSunTemperature", false);
            if (p.value("useSunTemperature", false))
            {
                dragF("Temperature (K)", "sunTemperatureK", 6500.0f, 25.0f, 1000.0f, 15000.0f, "%.0f");
            }
            InspectorHelp(
                "Tints the sun by a black-body temperature, multiplying the colour above. OFF by "
                "default, so the authored colour is used as-is.\n\n"
                "1700 candle, 2700 tungsten, 4000 cool white, 5500 midday sun, 6500 D65 (neutral "
                "here), 7500+ overcast/shade blue.\n\n"
                "The locus is Unreal's, minus its Stefan-Boltzmann brightness term, and the result "
                "is renormalised to unit luminance -- so this changes HUE ONLY. Without that, 3000K "
                "would arrive about a stop darker than 6500K and you would have to chase every "
                "temperature change with an intensity change.\n\n"
                "Valid 1000-15000K; outside that the fit bends back on itself, so it is clamped.");

            const float legacyExposure = JsonFloat(p, "exposure", 1.0f);
            dragF("Sun Intensity", "sunIntensity", legacyExposure, 0.05f, 0.0f, 100.0f);
            InspectorHelp(
                "How bright the sun is. This is NOT a camera control -- the camera lives on the "
                "Camera Exposure object and meters the frame by itself.\n\n"
                "It replaces the old `exposure` field, which multiplied the sun AND the ambient "
                "while leaving the sky background, spot/point lights and emissive alone. With auto "
                "exposure on, that field stopped changing brightness at all (the metering cancels "
                "it) and only changed the RATIO between lit geometry and sky -- a confusing thing "
                "for a control with that name.");
            dragF("Ambient", "ambient", 0.05f, 0.005f, 0.0f, 10.0f);
            InspectorHelp("Sky fill intensity -- the light everything gets from the sky rather than "
                          "from the sun. Independent of Sun Intensity.");

            dragF("Sky Fill Intensity", "skyFillIntensity", 1.0f, 0.01f, 0.0f, 4.0f, "%.3f");
            InspectorHelp(
                "How much of the SKY'S OWN measured irradiance reaches diffuse surfaces. Only "
                "active when this level's sky was imported with its IBL derivatives -- check "
                "logs/ibl.log if you are not sure which path a level took.\n\n"
                "1 = the irradiance cube at face value, which is the physical answer. It is a "
                "separate control from Ambient above on purpose: Ambient means 'this fraction of "
                "the SUN colour bounces around', a number authored against a different equation "
                "entirely, and reusing it here would bury the fill about twenty times too deep.\n\n"
                "Ambient still drives the flat fallback fill on levels whose sky has no "
                "derivatives.");

            if (p.contains("sunIntensity"))
            {
                ImGui::TextDisabled("legacy 'exposure' present but ignored");
                InspectorHelp("This object carries the new Sun Intensity, so the old whole-scene "
                              "`exposure` field no longer does anything. Delete it from the level "
                              "JSON whenever you next hand-edit the file.");
            }

            Math::float3 rayDirection = EditorLightDirection::NormalizedRay(
                JsonFloat3(p, "direction", Math::float3(-1.0f, -1.0f, -1.0f)));
            float sourceAzimuth = 0.0f;
            float sourceElevation = 0.0f;
            EditorLightDirection::SourceAngles(
                rayDirection, sourceAzimuth, sourceElevation);

            {
                const nlohmann::json beforeItem = p;
                const bool changed = ImGui::DragFloat("Source azimuth (Y)",
                    &sourceAzimuth, 0.5f, -180.0f, 180.0f, "%.1f deg",
                    ImGuiSliderFlags_AlwaysClamp);
                if (changed)
                {
                    rayDirection = EditorLightDirection::RayFromSourceAngles(
                        sourceAzimuth, sourceElevation);
                    p["direction"] = {
                        rayDirection.x, rayDirection.y, rayDirection.z };
                }
                trackContinuousEdit(beforeItem, changed);
            }
            {
                const nlohmann::json beforeItem = p;
                const bool changed = ImGui::DragFloat("Source elevation",
                    &sourceElevation, 0.5f, -89.0f, 89.0f, "%.1f deg",
                    ImGuiSliderFlags_AlwaysClamp);
                if (changed)
                {
                    rayDirection = EditorLightDirection::RayFromSourceAngles(
                        sourceAzimuth, sourceElevation);
                    p["direction"] = {
                        rayDirection.x, rayDirection.y, rayDirection.z };
                }
                trackContinuousEdit(beforeItem, changed);
            }

            rayDirection = EditorLightDirection::NormalizedRay(rayDirection);
            ImGui::PushStyleColor(
                ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::InputFloat3("Normalized ray", &rayDirection.x, "%.3f",
                ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("World-space direction in which the light rays travel.");
            }
        }
        else if (env.type == "camera")
        {
            dragF("H FOV (deg)", "hfovDeg", 90.0f, 0.5f, 1.0f, 179.0f);
            dragF("Z Near", "zNear", 0.01f, 0.001f, 0.0001f, 100.0f);
            dragF("Z Far", "zFar", 10000.0f, 1.0f, 0.1f, 1000000.0f);
        }
        else if (env.type == "cameraExposure")
        {
            // P1: dormant. Everything here round-trips through the level and reaches
            // Scene::SetCameraExposure, but nothing reads it yet -- the metering passes are P2.
            checkB("Enabled", "enabled", false);
            if (!p.value("enabled", false))
            {
                ImGui::TextDisabled("Dormant: exposure multiplier is exactly 1.0.");
            }
            checkB("Auto Exposure", "autoExposure", true);

            const bool automatic = p.value("autoExposure", true);
            if (automatic)
            {
                ImGui::SeparatorText("Target");
                dragF("Compensation (EV)", "compensationEv", 0.0f, 0.05f, -8.0f, 8.0f, "%.2f");
                InspectorHelp("Offsets whatever the meter decides. This is the artistic knob - "
                              "negative darkens the whole image, positive lifts it.");
                ImGui::SeparatorText("Range (clamps on the metered result)");
                dragF("Min EV100", "minEv100", -6.0f, 0.1f, -16.0f, 20.0f, "%.2f");
                dragF("Max EV100", "maxEv100", 16.0f, 0.1f, -16.0f, 20.0f, "%.2f");
                ImGui::SeparatorText("Metering (which pixels set the exposure)");
                dragF("Low Percentile", "lowPercentile", 0.15f, 0.005f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("The histogram window the meter averages between. Widening it makes "
                              "exposure react to more of the frame; narrowing it makes the meter "
                              "pickier and steadier.");
                dragF("High Percentile", "highPercentile", 0.80f, 0.005f, 0.0f, 1.0f, "%.3f");
                ImGui::SeparatorText("Weight mask (centre-weighted metering)");
                dragF("Meter Mask Strength", "meterMaskStrength", 0.7f, 0.01f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("How strongly the frame edges are discounted. 0 meters the whole "
                              "frame evenly, which lets a bright sky at the top drag the whole "
                              "image dark.");
                dragF("Meter Mask Inner", "meterMaskInnerRadius", 0.35f, 0.01f, 0.0f, 2.0f, "%.3f");
                dragF("Meter Mask Outer", "meterMaskOuterRadius", 1.0f, 0.01f, 0.0f, 2.0f, "%.3f");
                dragF("Meter Mask Sky Bias", "meterMaskSkyBias", 0.6f, 0.01f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("Extra discount applied to the upper part of the frame, on top of the "
                              "radial mask. This is the sky-specific version of the same problem.");
                ImGui::SeparatorText("Adaptation (how fast it moves)");
                dragF("Speed Up (stops/s)", "speedUp", 3.0f, 0.05f, 0.0f, 20.0f, "%.2f");
                InspectorHelp("Rate towards a BRIGHTER target (stepping into sun). Faster than "
                              "the down rate on purpose - that asymmetry is how a real eye behaves.");
                dragF("Speed Down (stops/s)", "speedDown", 1.0f, 0.05f, 0.0f, 20.0f, "%.2f");
                InspectorHelp("Rate towards a DARKER target (stepping into shade).");
                dragF("Ease-in Distance", "adaptationStartDistance", 1.5f, 0.05f, 0.05f, 20.0f, "%.2f");
                dragF("Black Bucket Influence", "blackBucketInfluence", 1.0f, 0.01f, 0.0f, 1.0f, "%.3f");
            }
            else
            {
                dragF("Manual EV100", "manualEv100", 0.0f, 0.05f, -16.0f, 20.0f, "%.2f");
            }

            // Plan section 6.2 wants both representations visible, because EV is the authored
            // quantity but the linear multiplier is what a shader bug would show up in.
            ImGui::Separator();
            const float shownEv = automatic
                ? JsonFloat(p, "compensationEv", 0.0f)
                : JsonFloat(p, "manualEv100", 0.0f);
            const float multiplier = p.value("enabled", false)
                ? render::ExposureMultiplierFromEv100(shownEv)
                : render::kIdentityExposureMultiplier;
            ImGui::TextDisabled(automatic
                ? "Compensation %.2f EV -> x%.5f"
                : "Manual %.2f EV100 -> x%.5f",
                shownEv, multiplier);

            ImGui::SeparatorText("Local exposure");
            dragF("Local Highlights", "localHighlightContrast", 1.0f, 0.01f, 0.1f, 2.0f, "%.3f");
            InspectorHelp("Contrast scale for everything brighter than middle grey, judged by the "
                          "BLURRED neighbourhood rather than the pixel. 1 = off.\n\n"
                          "Below 1 COMPRESSES -- bright regions come down while their detail stays "
                          "(measured on sun_glint: 0.55 cut clipping 65x for a 2% median move). "
                          "Above 1 EXPANDS, which on wind_test is the more useful direction: this "
                          "scene's problem is too little range, not too much. Either way it is the "
                          "one thing a global exposure cannot do.");
            dragF("Local Shadows", "localShadowContrast", 1.0f, 0.01f, 0.1f, 2.0f, "%.3f");
            InspectorHelp("Same for everything darker than middle grey. Below 1 lifts shaded "
                          "regions without touching the lit ones; above 1 deepens them, and being "
                          "per-neighbourhood it deepens WITHOUT crushing the lit side.");
            dragF("Local Detail", "localDetailStrength", 1.0f, 0.01f, 0.0f, 3.0f, "%.3f");
            InspectorHelp("How much per-pixel detail survives the scaling. 1 = all of it, which is "
                          "the point: scaling the blurred base while passing detail through "
                          "untouched is what keeps micro-contrast.");
            dragF("Local HL Threshold", "localHighlightThreshold", 0.0f, 0.05f, 0.0f, 8.0f, "%.2f");
            dragF("Local SH Threshold", "localShadowThreshold", 0.0f, 0.05f, 0.0f, 8.0f, "%.2f");
            InspectorHelp("Stops away from middle grey before the effect starts, so mid-tones -- "
                          "usually the subject -- are left alone.");
            {
                // Mirrors the dev window's local-exposure presets exactly, including "Expand",
                // which is the one measured against docs/ref/ref_wind_test.png.
                struct LocalPreset { const char* name; float hl, sh, detail, hlT, shT; const char* tip; };
                static const LocalPreset kLocal[] = {
                    { "Off", 1.00f, 1.00f, 1.00f, 0.00f, 0.00f,
                      "Neutral -- a true no-op, the shader skips the block entirely." },
                    { "Gentle", 0.85f, 0.90f, 1.00f, 0.50f, 0.50f,
                      "Mild COMPRESSION. Reach for it when a frame clips, e.g. into the sun over water." },
                    { "Strong", 0.65f, 0.75f, 1.10f, 0.25f, 0.25f,
                      "Heavy compression. Check the horizon for halos." },
                    { "Expand", 1.35f, 1.35f, 1.00f, 0.00f, 0.00f,
                      "The measured match to the reference: p99/p02 spread 23.6x -> 41.4x on the "
                      "overview view with clipping still at 0.000%. 1.5 on both reaches 52.9x "
                      "against the reference's 53.6x. Median drops a little -- about +0.1 EV back." },
                };
                ImGui::PushID("localPresets");
                for (int i = 0; i < static_cast<int>(std::size(kLocal)); ++i)
                {
                    const LocalPreset& preset = kLocal[i];
                    if (i != 0) { ImGui::SameLine(); }
                    if (ImGui::SmallButton(preset.name))
                    {
                        nlohmann::json after = p;
                        after["localHighlightContrast"] = preset.hl;
                        after["localShadowContrast"] = preset.sh;
                        after["localDetailStrength"] = preset.detail;
                        after["localHighlightThreshold"] = preset.hlT;
                        after["localShadowThreshold"] = preset.shT;
                        executeChange(std::move(after), historyLabel);
                    }
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", preset.tip); }
                }
                ImGui::PopID();
            }


            // Live metering state and the histogram, the same readouts the developer window shows.
            // They belong here too: the percentile and mask sliders above are unreadable without
            // seeing the distribution they are clipping.
            const ExposureMetering& metering = ctx.renderer.Exposure();
            const ExposureMetering::Readback readback = metering.LatestReadback();
            ImGui::SeparatorText("Live");
            if (readback.valid)
            {
                ImGui::Text("Adapted %+.3f EV100 (x%.5f)", readback.adaptedEv100,
                    render::ExposureMultiplierFromEv100(readback.adaptedEv100));
                ImGui::Text("Target  %+.3f EV100", readback.targetEv100);
                ImGui::TextDisabled("Metered low %.5f  high %.5f (scene linear)",
                    readback.lowLuminance, readback.highLuminance);
                const float gap = readback.targetEv100 - readback.adaptedEv100;
                ImGui::TextDisabled(fabsf(gap) < 0.01f ? "settled" : "adapting (%+.2f EV to go)", gap);
            }
            else
            {
                ImGui::TextDisabled("waiting for the first readback (enable the camera above)");
            }
            if (ImGui::SmallButton("Reset adaptation"))
            {
                ctx.renderer.Exposure().RequestReset();
            }
            InspectorHelp("Snaps the adapted value to the target instantly -- the same thing that "
                          "happens on level load, resize and camera cuts.");

            {
                static float bins[ExposureMetering::kHistogramBins] = {};
                UINT total = 0;
                if (metering.LatestHistogram(bins, ExposureMetering::kHistogramBins, &total) && total > 0)
                {
                    constexpr int kBinCount = static_cast<int>(ExposureMetering::kHistogramBins);
                    const ImVec2 size(ImGui::GetContentRegionAvail().x, 70.0f);
                    const ImVec2 origin = ImGui::GetCursorScreenPos();
                    ImGui::PlotHistogram("##inspectorHistogram", bins, kBinCount, 0, nullptr,
                        0.0f, 1.0f, size);

                    // Markers follow the CUMULATIVE distribution: placing them at
                    // lowPercentile * binCount would simply misreport where the window sits.
                    float sum = 0.0f;
                    for (int i = 0; i < kBinCount; ++i) { sum += bins[i]; }
                    const float invSum = sum > 0.0f ? 1.0f / sum : 0.0f;
                    const float lowPct = JsonFloat(p, "lowPercentile", 0.15f);
                    const float highPct = JsonFloat(p, "highPercentile", 0.80f);
                    int loBin = 0;
                    int hiBin = kBinCount - 1;
                    bool haveLo = false;
                    float cumulative = 0.0f;
                    for (int i = 0; i < kBinCount; ++i)
                    {
                        cumulative += bins[i] * invSum;
                        if (!haveLo && cumulative >= lowPct) { loBin = i; haveLo = true; }
                        if (cumulative >= highPct) { hiBin = i; break; }
                    }
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const float binW = size.x / static_cast<float>(kBinCount);
                    const float x0 = origin.x + binW * static_cast<float>(loBin);
                    const float x1 = origin.x + binW * static_cast<float>(hiBin + 1);
                    dl->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1, origin.y + size.y),
                        IM_COL32(90, 170, 255, 40));
                    ImGui::TextDisabled("blue = the window the percentiles actually meter");
                }
                else
                {
                    ImGui::TextDisabled("histogram appears once metering has run");
                }
            }
        }
        else if (env.type == "gtao")
        {
            // P6B. Every tooltip states the PERFORMANCE consequence as well as the look one, because
            // three of these knobs are linear in GPU cost and that is not guessable from the name.
            {
                const nlohmann::json beforeItem = p;
                bool gtaoEnabled = p.value("enabled", false);
                const bool changed = ImGui::Checkbox("Enabled", &gtaoEnabled);
                if (changed) { p["enabled"] = gtaoEnabled; }
                trackContinuousEdit(beforeItem, changed);
            }
            InspectorHelp("Ground-truth screen-space ambient occlusion. The whole chain costs about "
                          "0.12 ms at the defaults (2 directions x 6 steps, half render resolution). "
                          "Off schedules no pass at all, rather than a pass that writes 1.");

            ImGui::SeparatorText("Look");
            dragF("Strength", "strength", 1.0f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("How much of the DYNAMIC occlusion reaches the image. 0 is an exact no-op: "
                          "material AO from the G-buffer still applies, so this cannot switch off "
                          "what F9 already did. FREE - the pass runs either way.");
            dragF("World Radius", "worldRadius", 0.75f, 0.01f, 0.05f, 8.0f, "%.2f m");
            InspectorHelp("Occlusion reach in METRES, so contacts do not grow and shrink as the "
                          "camera moves. FREE in GPU cost. Note it loses authority below about 1 m: "
                          "the search radius is floored at Steps pixels, so past a modest distance it "
                          "stops being a world radius at all.");
            dragF("Intensity", "intensity", 1.0f, 0.01f, 0.1f, 4.0f, "%.2f");
            InspectorHelp("Exponent on the occlusion term - above 1 deepens, below 1 lifts. FREE.");
            dragF("Thickness", "thickness", 0.6f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("How solid an occluder is assumed to be behind its silhouette. 1 = fully "
                          "solid, so foliage casts a slab behind every leaf; low values keep leaves "
                          "thin. UE equivalent works out to 0.75. FREE.");

            ImGui::SeparatorText("Quality (these cost GPU time)");
            {
                const nlohmann::json beforeItem = p;
                int angles = static_cast<int>(p.value("numAngles", 2u));
                const bool changed = ImGui::SliderInt("Directions", &angles, 1, 8);
                if (changed) { p["numAngles"] = static_cast<std::uint32_t>(angles < 1 ? 1 : angles); }
                trackContinuousEdit(beforeItem, changed);
            }
            InspectorHelp("Screen directions searched per pixel. Cost is LINEAR in this - doubling it "
                          "roughly doubles the raw pass. 2 is the UE default (r.GTAO.NumAngles) and "
                          "leans on the temporal stage to average the rest over time.");
            {
                const nlohmann::json beforeItem = p;
                int steps = static_cast<int>(p.value("numSteps", 6u));
                const bool changed = ImGui::SliderInt("Steps", &steps, 2, 16);
                if (changed) { p["numSteps"] = static_cast<std::uint32_t>(steps < 1 ? 1 : steps); }
                trackContinuousEdit(beforeItem, changed);
            }
            InspectorHelp("Taps along each direction. Also LINEAR in cost, and it doubles as the floor "
                          "on the pixel radius - so raising it widens the reach at distance as a side "
                          "effect.");
            dragF("Fade Start", "fadeStart", 60.0f, 0.5f, 0.0f, 500.0f, "%.0f m");
            InspectorHelp("Distance where AO begins fading out; beyond it the estimate is mostly noise "
                          "anyway. Pulling both fades IN is the cheapest quality trade here - faded "
                          "pixels still run the pass, but stop contributing artefacts.");
            dragF("Fade End", "fadeEnd", 120.0f, 0.5f, 0.0f, 1000.0f, "%.0f m");
            InspectorHelp("Distance where AO is gone entirely.");

            ImGui::SeparatorText("Filtering");
            {
                const nlohmann::json beforeItem = p;
                bool denoise = p.value("denoise", true);
                const bool changed = ImGui::Checkbox("Denoise", &denoise);
                if (changed) { p["denoise"] = denoise; }
                trackContinuousEdit(beforeItem, changed);
            }
            InspectorHelp("Bilateral 5x5 across depth and normal, about 0.017 ms. Removes roughly 40% "
                          "of the raw noise; off is immediately visible as per-pixel grain.");
            {
                const nlohmann::json beforeItem = p;
                bool temporal = p.value("temporal", true);
                const bool changed = ImGui::Checkbox("Temporal", &temporal);
                if (changed) { p["temporal"] = temporal; }
                trackContinuousEdit(beforeItem, changed);
            }
            InspectorHelp("Accumulates over frames, which is what makes a 2x6-tap estimate usable at "
                          "all. About 0.012 ms plus one extra half-res target. Off is noisier than any "
                          "setting above can compensate for.");
            dragF("Temporal Blend", "temporalBlendWeight", 0.1f, 0.005f, 0.02f, 1.0f, "%.3f");
            InspectorHelp("Weight of the CURRENT frame. 0.1 (the UE default) is roughly a 10-frame "
                          "history. Higher is more responsive and noisier. FREE.");
            dragF("Temporal Clamp", "temporalClampRange", 0.35f, 0.01f, 0.05f, 1.0f, "%.2f");
            InspectorHelp("How far history may deviate before it is clamped back, with a still camera. "
                          "Below about 0.2 the history stops accumulating on this engine (DLSS jitter "
                          "widens the per-frame spread past the window); above about 0.5 the clamp "
                          "stops protecting against ghosting. FREE.");
            dragF("Filter Plane Tolerance", "filterPlaneTolerance", 0.05f, 0.005f, 0.005f, 1.0f, "%.3f m");
            InspectorHelp("Metres a neighbour may sit off the fitted plane before the denoise drops it. "
                          "Too small leaves grazing floors noisy, too large blurs away the contacts. "
                          "FREE.");
            dragF("Upsample Tolerance", "upsampleTolerance", 0.02f, 0.002f, 0.002f, 1.0f, "%.3f");
            InspectorHelp("Depth tolerance of the edge-aware upsample, as a FRACTION of the pixel "
                          "depth. Large values degrade it to plain bilinear, which spreads occlusion "
                          "past silhouettes. FREE.");

            ImGui::SeparatorText("Advanced");
            {
                const nlohmann::json beforeItem = p;
                bool gbufferNormal = p.value("useGBufferNormal", false);
                const bool changed = ImGui::Checkbox("Normal from G-buffer", &gbufferNormal);
                if (changed) { p["useGBufferNormal"] = gbufferNormal; }
                trackContinuousEdit(beforeItem, changed);
            }
            {
                const nlohmann::json beforeItem = p;
                bool useHzb = p.value("useHzb", true);
                const bool changed = ImGui::Checkbox("Use depth pyramid (HZB)", &useHzb);
                if (changed) { p["useHzb"] = useHzb; }
                trackContinuousEdit(beforeItem, changed);
            }
            InspectorHelp("Walks the horizon search over the hierarchical depth buffer, reading a "
                          "coarser mip the further a step reaches. Measured 22% faster on the AO pass "
                          "AND slightly less noisy. It under-estimates occlusion a little by design: "
                          "the pyramid keeps the FURTHEST depth in each tile, so a coarse level "
                          "cannot invent contacts.");
            InspectorHelp("OFF matches UE (r.GTAO.UseNormals = 0) and is almost certainly what you "
                          "want: the horizon search walks the DEPTH buffer, so the integral has to be "
                          "given the geometric normal. ON feeds it the normal-mapped one instead, "
                          "which reads as occlusion wherever the two disagree - on detail-mapped sand "
                          "that measured AO 0.35 on a fully open dune. Kept only for comparison.");
        }
        else if (env.type == "atmosphere")
        {
            // P7. The model is transcribed from UE's HeightFogCommon.ush, so each tooltip names the
            // UE parameter it corresponds to -- and, where a UE default does NOT carry over, says
            // why. That distinction is the whole reason these numbers are not simply theirs.
            {
                const nlohmann::json beforeItem = p;
                bool fogEnabled = p.value("enabled", false);
                const bool changed = ImGui::Checkbox("Enabled", &fogEnabled);
                if (changed) { p["enabled"] = fogEnabled; }
                trackContinuousEdit(beforeItem, changed);
            }
            InspectorHelp("Global analytic height fog, applied to opaque geometry in compose AND to "
                          "the ocean surface, which share one packed parameter set so the water and "
                          "the land cannot end up in different weather. Off by default: it is a real "
                          "image change and earns its default with an explicit A/B. Off costs "
                          "nothing - the shader skips the whole block on a zero density.");

            ImGui::SeparatorText("Medium");
            dragF("Density", "density", 0.004f, 0.0002f, 0.0f, 0.05f, "%.4f");
            InspectorHelp("UE's FogDensity: extinction per world unit at the reference height. This "
                          "is the main dial. THEIR DEFAULT (0.02) DOES NOT TRANSFER - UE author "
                          "against a centimetre world and this engine is in metres, so the shape of "
                          "the curve is theirs but the magnitude is ours to tune.");
            dragF("Height Falloff", "heightFalloff", 0.02f, 0.001f, 0.0f, 0.2f, "%.3f");
            InspectorHelp("UE's FogHeightFalloff, base 2. How fast density thins with altitude; 0 "
                          "makes it a uniform distance fog. This term is what keeps a high camera "
                          "from getting a flat screen-space wash - looking down from altitude, most "
                          "of the ray is already in thin air. Same unit caveat as Density.\n\n"
                          "IT ALSO SETS HOW THIN THE LAYER IS, and a thin layer seen from above "
                          "collapses into a BRIGHT LINE ON THE HORIZON. Density halves every "
                          "1/falloff metres, so 0.058 is a 17 m layer: from 80 m up the whole sea "
                          "sits at transmittance ~0.9 and the only ray that accumulates any depth "
                          "is the one aimed exactly at the horizon. Measured at 82 m, the luminance "
                          "step across the horizon row was 25.7/255 at 0.058 and 1.6 at 0.030. If "
                          "the camera flies, keep this near 0.02-0.03; a thin ground layer is a "
                          "look for a camera that stays inside it.");
            dragF("Reference Height", "referenceHeight", 0.0f, 0.5f, -100.0f, 500.0f, "%.1f m");
            InspectorHelp("World Y at which Density is exactly the authored value. Sea level here. "
                          "UE call this the fog actor's own height.");
            dragF("Start Distance", "startDistance", 25.0f, 0.5f, 0.0f, 500.0f, "%.0f m");
            InspectorHelp("UE's StartDistance: fog-free air in front of the camera, so near contrast "
                          "survives. Their default is 0; this project starts at 25 m because the "
                          "canonical views put the beach within a few tens of metres.");
            dragF("Max Opacity", "maxOpacity", 0.9f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("UE's FogMaxOpacity, and note what it actually does: it is a FLOOR on "
                          "transmittance, clipping the far end of the curve, not a scale on "
                          "coverage that would bend the whole curve. 0 makes the fog a no-op while "
                          "still enabled, which is a free A/B lever.\n\n"
                          "THE CEILING IS RELEASED AGAIN DEEP INTO THE FOG, and without that the "
                          "whole range below 1 was unusable: the sky is never fogged - it IS fog of "
                          "infinite depth - so it ignores the ceiling, while the water at the "
                          "horizon kept (1 - maxOpacity) of its own colour and the two could not "
                          "meet. Measured at 25 m, the luminance step across the horizon row at "
                          "0.70 was 9.9/255 and is now 2.65, which is what 1.00 gives. The ceiling "
                          "still does its job where it means something - at 25 m, 0.70 against 1.00 "
                          "moves 21% of the frame, and that difference sits in the FAR and middle "
                          "thirds, not on the horizon line.");

            ImGui::SeparatorText("Sun in-scattering");
            dragF("Sun Scatter", "sunScatterStrength", 0.35f, 0.01f, 0.0f, 3.0f, "%.2f");
            InspectorHelp("Weight of the forward-scattered sun added on top of the sky colour, so "
                          "looking into the sun warms the haze and looking away from it does not. "
                          "Judge it on wind_test's sun_glint view - no other canonical view has a "
                          "sun field to read it against.");
            dragF("Sun Scatter Tightness", "sunScatterExponent", 4.0f, 0.25f, 1.0f, 64.0f, "%.0f");
            InspectorHelp("UE's DirectionalInscatteringExponent, and their default of 4 IS used "
                          "here: it is dimensionless, so unlike density it transfers unchanged. "
                          "Higher = a tighter glow around the sun.");
            dragF("Sun Scatter Start", "sunScatterStartDistance", 100.0f, 1.0f, 0.0f, 1000.0f, "%.0f m");
            InspectorHelp("UE's DirectionalInscatteringStartDistance: the sun lobe builds up on its "
                          "OWN line integral past this distance, so it does not tint the near "
                          "field. Theirs is 10000 in centimetres. The lobe then FADES BACK OUT as "
                          "the fog saturates, which UE's does not: their base colour is an authored "
                          "constant with no sun in it, ours is the sky, which already has the sun's "
                          "glow. A lobe surviving to the horizon would add it twice - and only to "
                          "geometry, never to the sky pixel beside it, which reads as a hard warm "
                          "band stopping dead at the horizon line.");

            ImGui::SeparatorText("Sky sampling");
            dragF("Back Scatter", "skyBackScatter", 1.0f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("The phase function, as how bright the haze is with the sun BEHIND you "
                          "relative to looking into it. 1 = flat: identical haze whichever way you "
                          "face, which is what this shipped with. Real haze is forward-peaked, so "
                          "below 1 a backlit shore stays thick while flying away from the island "
                          "with the sun behind you no longer sinks it into blue milk - at the SAME "
                          "density. It is deliberately NOT a second density: density is extinction, "
                          "and a directional one would change how much of the far island survives "
                          "rather than only its colour, so shapes would fade in and out as you pan. "
                          "0.4-0.6 is a normal haze; fades out with distance, where the sky sample "
                          "already carries its own anisotropy.");
            dragF("Sky Blur", "skyBlur", 0.5f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("How blurred the sky is where it is read as the fog's COLOUR, expressed as "
                          "a roughness. 0 samples it sharp, and then the fog is literally a picture "
                          "of what stands behind the surface - cloud edges and the sunset band print "
                          "themselves onto the palms in front of them. Only the lightly-fogged end "
                          "is blurred: a fully fogged pixel always converges on the sharp sky, "
                          "because the sky IS fog of infinite depth and anything else seams at the "
                          "horizon. FREE - it is a mip choice, not an extra sample.");

            ImGui::TextDisabled("Fog colour comes from the SKY along the view ray, not an authored");
            ImGui::TextDisabled("colour as in UE - that is what removes the horizon seam by");
            ImGui::TextDisabled("construction instead of by tuning.");
            // The debug views deliberately live only in the dev window: they are a viewing mode,
            // not level data, and putting them on the object would invite them into the save.
            ImGui::TextDisabled("Debug views (transmittance / in-scattering) are in the F1 window,");
            ImGui::TextDisabled("Render tab - they are a viewing mode, not level data.");
            ImGui::TextDisabled("Fix exposure when comparing: auto-exposure reacts to fog and");
            ImGui::TextDisabled("shifts the WHOLE frame, sky included.");
        }
        else if (env.type == "colorPipeline")
        {
            // P3C. Same set the dev window exposes, so a look tuned live can be written into the
            // level here rather than only through the clipboard button.
            const std::string curve = p.value("toneCurve", std::string("legacy"));
            int curveIndex = (curve == "agx") ? 1 : ((curve == "filmic" || curve == "film") ? 2 : 0);
            {
                const nlohmann::json beforeItem = p;
                const char* kCurveNames[] = { "Legacy (ACES fit)", "AgX", "Filmic (Unreal)" };
                const bool changed = ImGui::Combo("Tone Curve", &curveIndex, kCurveNames, 3);
                if (changed)
                {
                    p["toneCurve"] = (curveIndex == 1) ? "agx" : (curveIndex == 2 ? "filmic" : "legacy");
                }
                trackContinuousEdit(beforeItem, changed);
            }

            ImGui::SeparatorText("Colour grade (applies to every curve)");
            dragF("Grade Saturation", "gradeSaturation", 1.30f, 0.01f, 0.0f, 4.0f, "%.3f");
            InspectorHelp("Chroma around luma, applied in linear BEFORE the curve -- the same place "
                          "Unreal bakes it into its LUT. 1 = unchanged, 0 = greyscale.");
            dragF("Grade Contrast", "gradeContrast", 1.15f, 0.01f, 0.1f, 4.0f, "%.3f");
            InspectorHelp("Pivoted on middle grey (0.18), so raising it deepens shadows and lifts "
                          "highlights while midtones stay put. This is the control that STRETCHES "
                          "the histogram -- exposure can only slide it.");
            dragF("Grade Gamma", "gradeGamma", 1.10f, 0.01f, 0.1f, 4.0f, "%.3f");
            InspectorHelp("Midtone weighting. Above 1 opens midtones without moving black or white "
                          "as much as gain would.");
            dragF("Grade Gain", "gradeGain", 1.0f, 0.01f, 0.0f, 4.0f, "%.3f");
            InspectorHelp("Plain multiplier. Overlaps with exposure compensation -- prefer the "
                          "camera's compensation for overall brightness or the two will fight.");
            dragF("Grade Offset", "gradeOffset", 0.0f, 0.001f, -1.0f, 1.0f, "%.4f");
            InspectorHelp("Plain lift. Small positive values give the faded, milky-black film look; "
                          "negative crushes the black point.");

            // Same preset set as the developer window, written through the command stack so a
            // preset is one undo step rather than five stray field edits.
            {
                struct GradePreset { const char* name; float sat, con, gam, gain, off; const char* tip; };
                static const GradePreset kPresets[] = {
                    { "Neutral", 1.00f, 1.00f, 1.00f, 1.00f, 0.000f,
                      "No grading. Bit-identical to the ungraded image." },
                    { "Vivid", 1.40f, 1.25f, 1.00f, 1.00f, 0.000f,
                      "The measured match to the reference photograph. Set compensation to suit the "
                      "CURVE: about -0.4 EV on Legacy, 0.0 on Filmic." },
                    { "Punchy", 1.20f, 1.45f, 1.00f, 1.00f, 0.000f,
                      "Contrast-forward rather than colour-forward." },
                    { "Filmic", 1.10f, 1.05f, 1.00f, 1.00f, 0.015f,
                      "The faded look -- the offset lifts the black point off zero." },
                    { "Warm sand", 1.30f, 1.15f, 1.10f, 1.00f, 0.000f,
                      "The default. Vivid with the midtones opened up so lit sand and foliage keep "
                      "detail. Pairs with -0.15 EV on the Filmic curve." },
                    { "Flat", 0.85f, 0.90f, 1.00f, 1.00f, 0.000f,
                      "Deliberately washed out, for judging LIGHTING rather than look -- shading "
                      "errors stop hiding behind the grade." },
                };
                ImGui::PushID("gradePresets");
                for (int i = 0; i < static_cast<int>(std::size(kPresets)); ++i)
                {
                    const GradePreset& preset = kPresets[i];
                    if (i != 0) { ImGui::SameLine(); }
                    if (ImGui::SmallButton(preset.name))
                    {
                        nlohmann::json after = p;
                        after["gradeSaturation"] = preset.sat;
                        after["gradeContrast"] = preset.con;
                        after["gradeGamma"] = preset.gam;
                        after["gradeGain"] = preset.gain;
                        after["gradeOffset"] = preset.off;
                        executeChange(std::move(after), historyLabel);
                    }
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", preset.tip); }
                }
                ImGui::PopID();
            }

            if (curveIndex == 2)
            {
                ImGui::SeparatorText("Film curve (Unreal's controls)");
                dragF("Film Slope", "filmSlope", 0.88f, 0.005f, 0.1f, 2.0f, "%.3f");
                InspectorHelp("Steepness of the straight middle section, i.e. the curve's overall "
                              "contrast. Unreal's default is 0.88.");
                dragF("Film Toe", "filmToe", 0.55f, 0.005f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("How much the shadows roll off. Higher keeps shadow detail and lifts "
                              "the black end; lower crushes toward black sooner. Default 0.55.");
                dragF("Film Shoulder", "filmShoulder", 0.26f, 0.005f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("How much the highlights roll off. LOWER reaches white sooner, which "
                              "is the knob to use when the image reads short of the reference at "
                              "the top end. Default 0.26.");
                dragF("Film Black Clip", "filmBlackClip", 0.0f, 0.005f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("How far below zero the toe may reach before clipping. Default 0.");
                dragF("Film White Clip", "filmWhiteClip", 0.04f, 0.005f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("How far above one the shoulder may reach. Default 0.04.");
                if (ImGui::SmallButton("Unreal defaults"))
                {
                    nlohmann::json after = p;
                    after["filmSlope"] = 0.88f;
                    after["filmToe"] = 0.55f;
                    after["filmShoulder"] = 0.26f;
                    after["filmBlackClip"] = 0.0f;
                    after["filmWhiteClip"] = 0.04f;
                    executeChange(std::move(after), historyLabel);
                }
            }
            else if (curveIndex == 1)
            {
                ImGui::SeparatorText("AgX look");
                dragF("AgX Slope", "agxSlope", 1.0f, 0.01f, 0.0f, 4.0f, "%.3f");
                dragF("AgX Power", "agxPower", 1.0f, 0.01f, 0.1f, 4.0f, "%.3f");
                InspectorHelp("Contrast. NOTE it acts on the [0,1] log-encoded value, so raising it "
                              "DARKENS -- unlike the grade contrast above, which pivots on middle "
                              "grey. That is why the AgX 'punchy' look crushes the image.");
                dragF("AgX Saturation", "agxSaturation", 1.0f, 0.01f, 0.0f, 4.0f, "%.3f");
                ImGui::PushID("agxLook");
                if (ImGui::SmallButton("Neutral"))
                {
                    nlohmann::json after = p;
                    after["agxSlope"] = 1.0f; after["agxPower"] = 1.0f; after["agxSaturation"] = 1.0f;
                    executeChange(std::move(after), historyLabel);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Punchy"))
                {
                    nlohmann::json after = p;
                    after["agxSlope"] = 1.0f; after["agxPower"] = 1.35f; after["agxSaturation"] = 1.4f;
                    executeChange(std::move(after), historyLabel);
                }
                ImGui::PopID();
            }
        }
        else if (env.type == "skybox")
        {
            dragF("Intensity", "intensity", 1.0f, 0.01f, 0.0f, 8.0f, "%.3f");
            InspectorHelp(
                "How bright this level's sky is, on the engine's linear scale. 1 = the cubemap's "
                "own radiance, untouched.\n\n"
                "It exists because HDRI libraries are not calibrated to our scale. Measured on "
                "citrus_orchard_puresky_4k: the sky's median luminance is 0.657, and the default "
                "manual exposure multiplier is 1.44, which puts 48.6% of the sky above 1.0 BEFORE "
                "the tone curve runs. No curve can undo that -- it needs less light, not a "
                "different shoulder. Around 0.45 brings that sky back under the knee.\n\n"
                "Auto exposure used to hide this by metering it away. That is exactly why it must "
                "not be the only thing holding the image together: turn adaptation off and the "
                "scene should still be photographable.");
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
                    // F7 ships SERVICE cubes beside every sky -- the GGX-prefiltered `_spec` and
                    // the cosine-convolved `_diffuse`. They are cubemaps, so they pass the filter
                    // above, but choosing one as the level's sky is never what anyone wants: the
                    // first is a blurred stack indexed by roughness, the second is a 32^2 blob.
                    // Hide them; the runtime finds them from the display cube's name by itself.
                    if (EndsWithSuffix(rec.id.key, "_spec.dds") ||
                        EndsWithSuffix(rec.id.key, "_diffuse.dds"))
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
                    // surf sim injection: mode-independent (docs/ocean_surf_sim_plan.md).
                    {
                        bool surfSimEnabled = render.surfSimEnabled;
                        if (ImGui::Checkbox("Surf Sim Enabled", &surfSimEnabled))
                        {
                            OceanRenderConfig afterRender = render;
                            afterRender.surfSimEnabled = surfSimEnabled;
                            nlohmann::json after = p;
                            after["render"] = OceanRenderConfigJson::ToJson(afterRender);
                            executeChange(std::move(after), "Set Ocean Surf Sim Enabled");
                        }
                        // Tooltip helper: annotates the LAST widget (the drag above it).
                        auto tip = [](const char* text)
                        {
                            if (ImGui::IsItemHovered())
                            {
                                ImGui::SetTooltip("%s", text);
                            }
                        };
                        // --- Spawner: where and what is born ---
                        renderDrag("Surf Spawn Distance", render.surfSimSpawnDistance, 0.25f, 5.0f, 200.0f);
                        tip("Metres seaward of the waterline where a wave segment is born.\n"
                            "Farther = longer travel, more refraction, more decay on the way in.");
                        renderDrag("Surf Segment Length", render.surfSimSegmentLength, 0.25f, 4.0f, 120.0f);
                        tip("Along-shore length of each spawned wavefront (m).\n"
                            "30 = local roller, 100+ = a long wall across the beach.");
                        renderDrag("Surf Spawn Interval", render.surfSimSpawnInterval, 0.02f, 0.25f, 30.0f);
                        tip("Seconds between spawns at FULL wind. The wave frequency lever.");
                        renderDrag("Surf Wind Coupling", render.surfSimWindCoupling, 0.005f, 0.0f, 1.0f);
                        tip("How much wind drives the spawner. 1 = calm is silent, amplitude and\n"
                            "cadence scale with wind; 0 = waves always come as authored.");
                        renderDrag("Surf Min Spawn Depth (m)", render.surfSimMinSpawnDepth, 0.02f, 0.0f, 10.0f);
                        tip("Bottom depth required under the WHOLE segment to be born.\n"
                            "Kills births in lagoons, on shoal banks and in channels.");
                        // --- Wave shape: peak width and height IN THE SIM (the cap follows) ---
                        renderDrag("Surf Wave Amplitude", render.surfSimWaveAmplitude, 0.005f, 0.0f, 2.0f);
                        tip("Injected height budget (m). It is the time-INTEGRAL of the forcing,\n"
                            "not the visible peak - the actual crest is lower and depends on\n"
                            "Wave Sigma / Spawn Duration.");
                        renderDrag("Surf Wave Sigma (m)", render.surfSimWaveSigma, 0.02f, 1.5f, 12.0f);
                        tip("Across-shore half-width of the injected hump - THE wavelength lever.\n"
                            "Smaller = narrower, sharper, higher wave (same amplitude budget in\n"
                            "less width) and a narrower foam cap. Below ~1.5 m the 1 m sim grid\n"
                            "aliases.");
                        renderDrag("Surf Spawn Duration (s)", render.surfSimSpawnDuration, 0.01f, 0.3f, 3.0f);
                        tip("Seconds the hump inflates. Shorter = more compact, TALLER packet\n"
                            "(less smearing while it departs); the amplitude integral is kept.");
                        renderDrag("Surf Bore Floor (m)", render.surfSimCelerityFloor, 0.005f, 0.1f, 1.0f);
                        tip("Minimum depth used for wave speed (c = sqrt(g*depth)).\n"
                            "High = the front marches reliably to the waterline.\n"
                            "Low = waves slow and COMPRESS on the shallows (shorter and steeper\n"
                            "near shore) but spend longer dying in the strip.");
                        renderDrag("Surf Wave Damping (1/s)", render.surfSimWaveDamping, 0.001f, 0.0f, 0.4f);
                        tip("Open-water settle rate. Lower = waves survive the trip fuller;\n"
                            "higher = calmer field between waves.");
                        // --- Breaking and foam ---
                        renderDrag("Surf Breaker Gamma", render.surfSimBreakerGamma, 0.005f, 0.4f, 1.2f);
                        tip("Surf breaker index H/d: the wave foams where its height exceeds\n"
                            "gamma * depth (McCowan ~0.78). Lower = breaks earlier and farther\n"
                            "out = wider foam band.");
                        renderDrag("Surf Break Onset", render.surfSimBreakOnset, 0.005f, 0.1f, 0.9f);
                        tip("Fraction of the breaker criterion where foam STARTS ramping in.\n"
                            "Lower = foam appears on the approach = wider cap;\n"
                            "higher = only right at the break = narrow cap.");
                        renderDrag("Surf Deposit Strength", render.surfSimDepositStrength, 0.01f, 0.0f, 5.0f);
                        tip("Peak foam a breaking crest stamps into the field\n"
                            "(max() semantics, like the FFT whitecaps).");
                        renderDrag("Surf Foam Fade Time (s)", render.surfSimFoamFadeTime, 0.01f, 0.2f, 30.0f);
                        tip("Seconds a full-strength foam stamp takes to dissolve\n"
                            "(linear decay behind the crest). The tail length lever.");
                        // --- Foam look ---
                        renderDrag("Surf Front Breakup", render.surfSimFrontBreakup, 0.005f, 0.0f, 1.0f);
                        tip("Tear of the FRESH foam (values near the stamp peak). 0 = solid cap.");
                        renderDrag("Surf Tail Breakup", render.surfSimTailBreakup, 0.005f, 0.0f, 2.0f);
                        tip("Tear of the decayed tail. Above 1 the threshold outgrows the\n"
                            "pattern and even mid-fresh foam shreds.");
                        renderDrag("Surf Cap Width", render.surfSimCapWidth, 0.005f, 0.25f, 4.0f);
                        tip("Bends the value-age curve of the tear: > 1 keeps foam on the FRONT\n"
                            "amount longer = wider dense zone; < 1 hands it to the tail sooner\n"
                            "= narrower.");
                        renderDrag("Surf Tear Scale (m)", render.surfSimTearScale, 0.05f, 1.0f, 50.0f);
                        tip("Patch size of the drifting tear pattern (metres).");
                        // --- Surface coupling ---
                        renderDrag("Surf Wave Displacement", render.surfSimDisplacement, 0.005f, 0.0f, 2.0f);
                        tip("How much of the sim wave height rides the water surface as vertex\n"
                            "displacement. 0 = foam only, the surface stays flat.");
                        renderDrag("Surf Run Inland (m)", render.surfSimRunInland, 0.05f, 0.0f, 20.0f);
                        tip("Metres past the SDF waterline the wave may live - the VISIBLE water\n"
                            "edge sits inland of the shore map's zero.");
                    }
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
                        ImGui::SeparatorText("Contact Foam Tail");
                        renderDrag("Tail Texture Scale", render.shoreLegacyTailTextureScale, 0.005f, 0.001f, 10.0f);
                        renderDrag("Tail Depth", render.shoreLegacyTailDepth, 0.002f, 0.0f, 5.0f);
                        renderDrag("Tail Scroll Speed", render.shoreLegacyTailScrollSpeed, 0.005f, 0.0f, 10.0f);
                        renderDrag("Tail De-Tile", render.shoreLegacyTailDetile, 0.005f, 0.0f, 1.0f);
                        renderDrag("Tail Contrast", render.shoreLegacyTailContrast, 0.005f, 0.0f, 4.0f);
                        renderDrag("Tail Brightness Bias", render.shoreLegacyTailBias, 0.002f, -1.0f, 1.0f);
                        renderDrag("Tail Edge Fade", render.shoreLegacyTailEdgeFade, 0.002f, 0.001f, 2.0f);
                        ImGui::SeparatorText("Foam Dissipation");
                        renderDrag("Dissipation Scale", render.shoreLegacyDissipationScale, 0.25f, 1.0f, 200.0f);
                        renderDrag("Dissipation Speed", render.shoreLegacyDissipationSpeed, 0.005f, 0.0f, 5.0f);
                        renderDrag("Dissipation Amount", render.shoreLegacyDissipationAmount, 0.002f, 0.0f, 1.0f);
                        renderDrag("Dissipation Contrast", render.shoreLegacyDissipationContrast, 0.01f, 0.1f, 8.0f);
                        renderDrag("Wind Thinning", render.shoreLegacyWindThinning, 0.002f, 0.0f, 1.0f);
                        // Shared fields: the same soft-edge and shore-albedo knobs the modern surface uses.
                        renderDrag("Refraction Soft Edge Distance", render.shoreEdgeSoftDepth, 0.001f, 0.0f, 0.25f);
                        renderDrag("Albedo Scale", render.shoreContactFoamAlbedoScale, 0.01f, 0.001f, 10.0f);
                        renderDrag("Albedo Scroll Speed", render.shoreContactFoamAlbedoScrollSpeed, 0.01f, 0.0f, 10.0f);

                        if (ImGui::TreeNodeEx(
                            "Wet Sand",
                            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
                        {
                            renderDrag(
                                "Deposit Depth",
                                render.shoreWetnessDepositDepth,
                                0.005f,
                                0.001f,
                                2.0f);
                            renderDrag(
                                "Wet Time (s)",
                                render.shoreWetnessWetTime,
                                0.02f,
                                0.05f,
                                30.0f);
                            renderDrag(
                                "Dry Time (s)",
                                render.shoreWetnessDryTime,
                                0.1f,
                                0.05f,
                                300.0f);
                            renderDrag(
                                "Darkening",
                                render.shoreWetnessDarkening,
                                0.005f,
                                0.0f,
                                1.0f);
                            renderDrag(
                                "Film Reflection",
                                render.shoreWetnessReflectionStrength,
                                0.005f,
                                0.0f,
                                2.0f);
                            renderDrag(
                                "Water Edge Offset (m)",
                                render.shoreWetnessEdgeOffset,
                                0.01f,
                                0.0f,
                                20.0f);
                            renderDrag(
                                "Max Slope (deg)",
                                render.shoreWetnessMaxSlopeDegrees,
                                0.25f,
                                0.0f,
                                89.0f);
                            ImGui::SeparatorText("Distant Height Fallback");
                            renderDrag(
                                "Above Water (m)",
                                render.shoreWetnessFallbackAboveWater,
                                0.01f,
                                0.0f,
                                20.0f);
                            renderDrag(
                                "Below Water (m)",
                                render.shoreWetnessFallbackBelowWater,
                                0.02f,
                                0.0f,
                                50.0f);
                            renderDrag(
                                "Fade Start",
                                render.shoreWetnessFallbackFadeStartPercent,
                                0.25f,
                                0.0f,
                                100.0f,
                                "%.0f %%");
                            renderDrag(
                                "Breakup Strength",
                                render.shoreWetnessFallbackBreakupStrength,
                                0.005f,
                                0.0f,
                                1.0f);
                            renderDrag(
                                "Breakup Scale (m)",
                                render.shoreWetnessFallbackBreakupScale,
                                0.1f,
                                0.1f,
                                500.0f,
                                "%.1f");
                            ImGui::TreePop();
                        }
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
                        "Wet Sand",
                        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
                    {
                        renderDrag(
                            "Deposit Depth",
                            render.shoreWetnessDepositDepth,
                            0.005f,
                            0.001f,
                            2.0f);
                        renderDrag(
                            "Wet Time (s)",
                            render.shoreWetnessWetTime,
                            0.02f,
                            0.05f,
                            30.0f);
                        renderDrag(
                            "Dry Time (s)",
                            render.shoreWetnessDryTime,
                            0.1f,
                            0.05f,
                            300.0f);
                        renderDrag(
                            "Darkening",
                            render.shoreWetnessDarkening,
                            0.005f,
                            0.0f,
                            1.0f);
                        renderDrag(
                            "Film Reflection",
                            render.shoreWetnessReflectionStrength,
                            0.005f,
                            0.0f,
                            2.0f);
                        renderDrag(
                            "Max Slope (deg)",
                            render.shoreWetnessMaxSlopeDegrees,
                            0.25f,
                            0.0f,
                            89.0f);
                        ImGui::SeparatorText("Distant Height Fallback");
                        renderDrag(
                            "Above Water (m)",
                            render.shoreWetnessFallbackAboveWater,
                            0.01f,
                            0.0f,
                            20.0f);
                        renderDrag(
                            "Below Water (m)",
                            render.shoreWetnessFallbackBelowWater,
                            0.02f,
                            0.0f,
                            50.0f);
                        renderDrag(
                            "Fade Start",
                            render.shoreWetnessFallbackFadeStartPercent,
                            0.25f,
                            0.0f,
                            100.0f,
                            "%.0f %%");
                        renderDrag(
                            "Breakup Strength",
                            render.shoreWetnessFallbackBreakupStrength,
                            0.005f,
                            0.0f,
                            1.0f);
                        renderDrag(
                            "Breakup Scale (m)",
                            render.shoreWetnessFallbackBreakupScale,
                            0.1f,
                            0.1f,
                            500.0f,
                            "%.1f");
                        ImGui::TreePop();
                    }

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
    DrawMeshEditorButton(ctx, registry, *obj);

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
