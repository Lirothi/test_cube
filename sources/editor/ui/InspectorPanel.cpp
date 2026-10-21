#include "editor/ui/InspectorPanel.h"
#include "app/scene/GtaoSettingsJson.h"
#include "rendering/core/PhotographicSettingsJson.h" // E1: the curve plot parses as the runtime does
#include "rendering/lighting/TimeOfDaySettings.h"    // the moon's light colour default
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
#include "editor/scene/EditorZone.h"
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
#include "rendering/shadows/ShadowSettings.h" // local-light plan S8: which bias the shadow fields drive
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
        nlohmann::json& props = env.properties;
        // P8B: WHICH sub-object the widgets below write to. Empty = the object's own properties,
        // which is every environment type that owns its settings directly. The folded "postProcess"
        // object points this at each group in turn, so the five settings blocks are reused as they
        // are instead of being rewritten against a nested path.
        //
        // Undo snapshots and the command payload stay on `props` REGARDLESS: EditEnvironmentCommand
        // replaces an object's whole property set, so handing it a group would delete its siblings.
        std::string groupKey;
        nlohmann::json missingGroup = nlohmann::json::object();
        const auto tgt = [&]() -> nlohmann::json& {
            if (groupKey.empty()) { return props; }
            const auto it = props.find(groupKey);
            return it != props.end() && it->is_object() ? *it : missingGroup;
        };
        // `after` for the commands that build one by copy-then-mutate.
        const auto withField = [&](const char* key, nlohmann::json value) {
            nlohmann::json after = props;
            if (groupKey.empty()) { after[key] = std::move(value); }
            else { after[groupKey][key] = std::move(value); }
            return after;
        };

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
            env.type == "timeOfDay" ? "Edit Time of Day" :
            env.type == "camera" ? "Edit Camera" :
            env.type == "skyAtmosphere" ? "Edit Sky Atmosphere" :
            env.type == "volumetricCloud" ? "Edit Volumetric Cloud" :
            env.type == "skybox" ? "Edit Skybox" :
            env.type == "ocean" ? "Edit Ocean" :
            env.type == "wind" ? "Edit Wind" :
            env.type == "cameraExposure" ? "Edit Camera Exposure" :
            env.type == "colorPipeline" ? "Edit Color Pipeline" :
            env.type == "gtao" ? "Edit Ambient Occlusion" :
            env.type == "heightFog" ? "Edit Height Fog" :
            env.type == "bloom" ? "Edit Bloom" :
            env.type == "postProcess" ? "Edit Post Process" :
            "Edit Environment";

        const auto executeChange = [&](nlohmann::json after, const std::string& label)
        {
            commandStack.Execute(ctx, std::make_unique<EditEnvironmentCommand>(
                env.id,
                props,
                std::move(after),
                label));
        };

        // Capture once on activation, BEFORE writing the widget's new value.
        // Copying the entire environment JSON per visible control made expanded
        // Post Process particularly expensive in unoptimized Debug builds.
        const auto beginContinuousEdit = [&](bool changed)
        {
            if (ImGui::IsItemActivated() ||
                (changed && activeEditObject.value != env.id.value))
            {
                activeEditObject = env.id;
                propertiesBeforeEdit = props;
            }
        };
        const auto trackContinuousEdit = [&](bool changed)
        {
            if (changed)
            {
                if (!groupKey.empty() &&
                    (!props.contains(groupKey) || !props[groupKey].is_object()))
                {
                    props[groupKey] = std::move(missingGroup);
                    missingGroup = nlohmann::json::object();
                }
                EnvironmentRuntime::Apply(ctx, env);
                ctx.document.SetDirty(true);
            }
            // Checkboxes/combos can finish in the same frame as activation.
            if (activeEditObject.value == env.id.value &&
                (ImGui::IsItemDeactivatedAfterEdit() ||
                    (changed && !ImGui::IsItemActive())))
            {
                commandStack.Execute(ctx, std::make_unique<EditEnvironmentCommand>(
                    env.id,
                    propertiesBeforeEdit,
                    props,
                    historyLabel));
                activeEditObject = EditorObjectId{};
            }
        };

        const bool supportsEnable =
            env.type == "pointLight" || env.type == "spotLight" || env.type == "ocean";
        if (showEnabled && supportsEnable)
        {
            bool enabled = tgt().value("enabled", true);
            if (ImGui::Checkbox("Enabled", &enabled))
            {
                nlohmann::json after = withField("enabled", enabled);
                executeChange(
                    std::move(after),
                    enabled ? "Enable Environment" : "Disable Environment");
            }
            ImGui::Separator();
        }

        auto colorEdit = [&]()
        {
            const Math::float3 c = JsonFloat3(tgt(), "color", Math::float3(1.0f, 1.0f, 1.0f));
            float cv[3] = { c.x, c.y, c.z };
            const bool changed = ImGui::ColorEdit3("Color", cv);
            beginContinuousEdit(changed);
            if (changed) { tgt()["color"] = { cv[0], cv[1], cv[2] }; }
            trackContinuousEdit(changed);
        };
        // `flags` is here for P16.2's lux row: a quantity whose useful range spans five decades
        // cannot be dragged linearly, and a logarithmic drag is a property of THAT row, not a reason
        // for a second helper.
        auto dragF = [&](const char* label, const char* key, float def, float speed, float lo, float hi,
                         const char* fmt = "%.3f", ImGuiSliderFlags flags = 0)
        {
            float v = JsonFloat(tgt(), key, def);
            const bool changed = ImGui::DragFloat(label, &v, speed, lo, hi, fmt, flags);
            beginContinuousEdit(changed);
            if (changed) { tgt()[key] = v; }
            trackContinuousEdit(changed);
        };
        auto dragF3 = [&](const char* label, const char* key, const Math::float3& def, float speed)
        {
            const Math::float3 d3 = JsonFloat3(tgt(), key, def);
            float v[3] = { d3.x, d3.y, d3.z };
            const bool changed = ImGui::DragFloat3(label, v, speed);
            beginContinuousEdit(changed);
            if (changed) { tgt()[key] = { v[0], v[1], v[2] }; }
            trackContinuousEdit(changed);
        };
        auto checkB = [&](const char* label, const char* key, bool def)
        {
            bool v = tgt().value(key, def);
            if (ImGui::Checkbox(label, &v))
            {
                nlohmann::json after = withField(key, v);
                executeChange(std::move(after), historyLabel);
            }
        };
        // Plan L: when a light comes on (rendering/lighting/NightLamps.h); the key is absent while it is "always".
        auto onAtCombo = [&]()
        {
            const LampSwitch current = LampSwitchFromName(tgt().value("onAt", std::string()));
            static const char* const kLabels[] = { "Always", "At night", "By day" };
            int index = static_cast<int>(current);
            if (ImGui::Combo("On At", &index, kLabels, 3) && index != static_cast<int>(current))
            {
                nlohmann::json after = props;
                if (index == 0) { after.erase("onAt"); }
                else { after["onAt"] = LampSwitchName(static_cast<LampSwitch>(index)); }
                executeChange(std::move(after), "Set Light On At");
            }
            InspectorHelp("A lamp that comes on at night: it switches when the sun goes down past Time of Day > "
                          "Lamps > Switch At (0 = sunset, as Ultra Dynamic Sky's Sunset event), after a delay of "
                          "up to Random Delay, warming up over Turn On with a flicker; it goes out at once at "
                          "sunrise. By day = the opposite. Setting the clock by hand switches it at once. Off, "
                          "it costs no shading and takes no shadow slot. A night lamp is night light: Time of Day > "
                          "Night Brightness multiplies it as it does the moon, so the lumens stay a bulb's.");
            if (current != LampSwitch::Always)
            {
                const NightLamps& lamps = ctx.scene.GetNightLamps();
                const float level = lamps.Level(current, JsonFloat3(tgt(), "position", Math::float3(0.0f, 0.0f, 0.0f)));
                ImGui::TextDisabled("Now: %s, at %.0f %%", lamps.Night() ? "night" : "day", level * 100.0f);
            }
        };

        // P8B: the five level-wide look groups, lifted out of the type chain so they can be
        // drawn EITHER as their own legacy object or as a section of the folded "postProcess"
        // one. `tgt()` is what makes that work: the same widget code writes to the object's own
        // properties or to a named sub-object, decided by `groupKey` at the call site.
        const auto drawCameraExposure = [&]()
        {
            // P1: dormant. Everything here round-trips through the level and reaches
            // Scene::SetCameraExposure, but nothing reads it yet -- the metering passes are P2.
            checkB("Enabled", "enabled", false);
            InspectorHelp("Master switch for the whole photographic camera. While OFF the linear "
                          "exposure multiplier is exactly 1.0 AND no metering work is scheduled, so "
                          "the frame is bit-for-bit what it was before any of this existed - off is "
                          "a true no-op, not a neutral value being applied.");
            if (!tgt().value("enabled", false))
            {
                ImGui::TextDisabled("Dormant: exposure multiplier is exactly 1.0.");
            }
            checkB("Auto Exposure", "autoExposure", true);
            InspectorHelp("ON meters the scene each frame and adapts towards what it finds. OFF "
                          "holds Manual EV100 exactly, which is what every A/B in this project uses "
                          "- a feature that changes average luminance moves the meter, and then "
                          "every pixel in the frame shifts including ones the feature never "
                          "touched.");

            const bool automatic = tgt().value("autoExposure", true);
            if (automatic)
            {
                ImGui::SeparatorText("Target");
                dragF("Compensation (EV)", "compensationEv", 0.0f, 0.05f, -8.0f, 8.0f, "%.2f");
                InspectorHelp("Offsets whatever the meter decides. This is the artistic knob - "
                              "negative darkens the whole image, positive lifts it.\n\n"
                              "P16.13: AUTO MODE ONLY. Manual mode has its own field, so a trim you "
                              "dial here cannot follow you into a fixed-exposure shot and back.");
                ImGui::SeparatorText("Range (clamps on the metered result)");
                dragF("Min EV100", "minEv100", -6.0f, 0.1f, -16.0f, 20.0f, "%.2f");
                dragF("Max EV100", "maxEv100", 16.0f, 0.1f, -16.0f, 20.0f, "%.2f");
                InspectorHelp("A safety net on the ADAPTED value, not a look control. The default "
                              "span is 22 stops, which clamps essentially nothing - it is \"off\", "
                              "not \"tuned\". Narrowing it is how a scene stops the camera opening "
                              "all the way up in a dark frame.\n\n"
                              "It is deliberately NOT the way to keep different lighting conditions "
                              "apart: percentile metering is scale-invariant, so a correctly "
                              "metered night and a correctly metered noon land on the same target, "
                              "and clamping the result flattens rather than fixes that. The "
                              "compensation CURVE below is the real answer.");

                // Time-of-day plan E1.
                ImGui::SeparatorText("Compensation curve (by the light)");
                {
                    // The rows are the document's own pairs, in its order; the runtime sorts them by EV
                    // (PhotographicSettingsJson::ApplyOverrides), so a row dragged past its neighbour
                    // still evaluates in order.
                    // A COPY, read once: releasing a row's drag commits an EditEnvironmentCommand that
                    // replaces the object's properties under this loop, and an iterator into them dangled --
                    // the next row dereferenced it (nlohmann invalid_iterator, owner 2026-10-13). Writes go
                    // through a fresh lookup each time, checked.
                    const auto curveIt = tgt().find("exposureCurve");
                    const bool hasCurve = curveIt != tgt().end() && curveIt->is_array();
                    const nlohmann::json curveNow = hasCurve ? *curveIt : nlohmann::json::array();
                    const std::size_t count = curveNow.size();
                    const auto pointOf = [](const nlohmann::json& p, float& ev, float& comp)
                    {
                        const bool ok = p.is_array() && p.size() >= 2 && p[0].is_number() && p[1].is_number();
                        ev = ok ? p[0].get<float>() : 0.0f;
                        comp = ok ? p[1].get<float>() : 0.0f;
                    };
                    std::size_t removeAt = count;
                    ImGui::PushID("exposureCurve");
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        float v[2] = { 0.0f, 0.0f };
                        pointOf(curveNow[i], v[0], v[1]);
                        ImGui::PushID(static_cast<int>(i));
                        const bool changed = ImGui::DragFloat2("EV100 -> EV", v, 0.05f, -16.0f, 24.0f, "%.2f");
                        beginContinuousEdit(changed);
                        if (changed)
                        {
                            const auto live = tgt().find("exposureCurve");
                            if (live != tgt().end() && live->is_array() && i < live->size())
                            {
                                (*live)[i] = nlohmann::json::array({ v[0], std::clamp(v[1], -10.0f, 10.0f) });
                            }
                        }
                        trackContinuousEdit(changed);
                        ImGui::SameLine();
                        if (ImGui::SmallButton("x")) { removeAt = i; }
                        ImGui::PopID();
                    }
                    if (count == 0u)
                    {
                        ImGui::TextDisabled("None: the meter's middle grey at any light.");
                    }
                    else
                    {
                        // The curve as the solve will run it -- parsed (sorted, clamped) by the same
                        // ApplyOverrides and evaluated by the CPU twin of the shader's ExposureCurve.
                        render::CameraExposureSettings parsed;
                        render::PhotographicSettingsJson::ApplyOverrides(tgt(), parsed);
                        constexpr int kSamples = 49;
                        constexpr float kLoEv = -8.0f, kHiEv = 16.0f;
                        float samples[kSamples];
                        float lo = -1.0f, hi = 0.0f; // always show 0 and a stop below it
                        for (int s = 0; s < kSamples; ++s)
                        {
                            const float ev = kLoEv + (kHiEv - kLoEv) * static_cast<float>(s) / (kSamples - 1);
                            samples[s] = render::EvaluateExposureCurve(parsed, ev);
                            lo = std::min(lo, samples[s]);
                            hi = std::max(hi, samples[s]);
                        }
                        ImGui::PlotLines("##curvePlot", samples, kSamples, 0, "EV100 -8 .. 16", lo - 0.5f, hi + 0.5f,
                                         ImVec2(ImGui::GetContentRegionAvail().x, 50.0f));
                    }
                    if (removeAt < count)
                    {
                        nlohmann::json curve = curveNow;
                        curve.erase(removeAt);
                        executeChange(withField("exposureCurve", std::move(curve)), historyLabel);
                    }
                    else if (count < render::CameraExposureSettings::kExposureCurveMaxPoints &&
                             ImGui::SmallButton("Add point"))
                    {
                        // Two stops of light past the last point at its compensation: a flat start.
                        nlohmann::json curve = curveNow;
                        float ev = 0.0f, comp = 0.0f;
                        if (!curve.empty()) { pointOf(curve.back(), ev, comp); ev += 2.0f; }
                        curve.push_back(nlohmann::json::array({ ev, comp }));
                        executeChange(withField("exposureCurve", std::move(curve)), historyLabel);
                    }
                    ImGui::PopID();

                    ImGui::PushID("curvePresets");
                    if (ImGui::SmallButton("None"))
                    {
                        executeChange(withField("exposureCurve", nlohmann::json::array()), historyLabel);
                    }
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("No curve: every level renders as it did before E1."); }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Eye"))
                    {
                        render::CameraExposureSettings eye;
                        render::SetEyeExposureCurve(eye);
                        nlohmann::json curve = nlohmann::json::array();
                        for (std::uint32_t i = 0; i < eye.exposureCurvePoints; ++i)
                        {
                            curve.push_back(nlohmann::json::array({ eye.exposureCurveEv[i], eye.exposureCurveComp[i] }));
                        }
                        executeChange(withField("exposureCurve", std::move(curve)), historyLabel);
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("What a level with a time of day wants: a day untouched, the end of "
                                          "twilight 1.3 stops down, a full-moon night about 3.6, a moonless one 5 "
                                          "(with Min EV100 low enough for the eye to reach it, about -11).");
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("UDS"))
                    {
                        render::CameraExposureSettings uds;
                        render::SetUdsExposureCurve(uds);
                        nlohmann::json curve = nlohmann::json::array();
                        for (std::uint32_t i = 0; i < uds.exposureCurvePoints; ++i)
                        {
                            curve.push_back(nlohmann::json::array({ uds.exposureCurveEv[i], uds.exposureCurveComp[i] }));
                        }
                        executeChange(withField("exposureCurve", std::move(curve)), historyLabel);
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("Ultra Dynamic Sky's curve, for UDS's moon (Time of Day > Night Brightness "
                                          "~11400: the moon 1/33 of the sun): a night ~1.5 stops under the day, the "
                                          "dawn a moonlit blue until the sun is nearly up. With the physical moon "
                                          "(Night Brightness 1) it leaves the night far too dark -- that one wants Eye.");
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("UDS Steady Night"))
                    {
                        render::CameraExposureSettings steady;
                        render::SetUdsSteadyNightExposureCurve(steady);
                        nlohmann::json curve = nlohmann::json::array();
                        for (std::uint32_t i = 0; i < steady.exposureCurvePoints; ++i)
                        {
                            curve.push_back(nlohmann::json::array({ steady.exposureCurveEv[i], steady.exposureCurveComp[i] }));
                        }
                        executeChange(withField("exposureCurve", std::move(curve)), historyLabel);
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("UDS's curve with a night that holds: the same from EV 10.55 up (the day, the "
                                          "bright twilight), steeper below, so at night the camera follows only 40 %% "
                                          "of the scene's darkening (UDS: 80 %%). Among dark palms the sky through the "
                                          "leaves no longer lights up: the camera opened 1.3 stops there, now 0.6. "
                                          "The open night comes out ~0.6 stops darker -- Time of Day > Night "
                                          "Exposure to taste.");
                    }
                    ImGui::PopID();
                }
                InspectorHelp("Extra compensation, in EV, by how much light the eye is adapted to -- the "
                              "scene's EV100 in this renderer's numbers (log2 cd/m2 + 3; Live below shows it "
                              "as Eye): wind_test's beach meters 15 at noon, 2.5 at the end of civil twilight, "
                              "-3.5 under a full moon. NEGATIVE DARKENS, like the "
                              "Compensation above, and adds to it.\n\n"
                              "Why: percentile metering lands any correctly lit scene on the same middle "
                              "grey, so under auto exposure a moonlit beach reads as a dim day. The eye "
                              "adapts only part of the way and a night stays dark; this curve is the rest "
                              "(UE's Exposure Compensation Curve). Piecewise linear through up to six "
                              "points, flat past its ends. It follows the eye's SMOOTHED adaptation, so a "
                              "dark room darkens the picture as the eye adapts rather than on one frame. "
                              "Min/Max EV100 still bound the camera. Night vision reads the eye's "
                              "adaptation, not the darkened camera. The SLOPE is the adaptation's strength: where the "
                              "compensation rises s stops a stop, the camera follows (1 - s) of the scene's change. "
                              "--set=exposure.curve:<0 none|1 eye|2 uds|3 uds steady night>");
                ImGui::SeparatorText("Metering (which pixels set the exposure)");
                dragF("Low Percentile", "lowPercentile", 0.15f, 0.005f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("The histogram window the meter averages between. Widening it makes "
                              "exposure react to more of the frame; narrowing it makes the meter "
                              "pickier and steadier.");
                InspectorHelp("Measured on the canonical views: 0.15 is what the scene was "
                              "flown at and preferred; about 0.30 lands nearest the reference "
                              "photograph if a little more contrast is wanted. Raise it toward 0.5 "
                              "ONLY if shaded interiors read over-exposed and the weight mask below "
                              "is already doing its job - the two solve different problems and "
                              "stacking them over-corrects.");
                dragF("High Percentile", "highPercentile", 0.80f, 0.005f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("The top of that window. Lowering it makes the meter ignore more of "
                              "the bright end, which is the blunt version of what the mask below "
                              "does selectively: it discards bright samples EVERYWHERE, including "
                              "ones that are the subject.");
                ImGui::SeparatorText("Weight mask (centre-weighted metering)");
                dragF("Meter Mask Strength", "meterMaskStrength", 0.7f, 0.01f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("How strongly the frame edges are discounted. 0 meters the whole "
                              "frame evenly, which lets a bright sky at the top drag the whole "
                              "image dark.");
                dragF("Meter Mask Inner", "meterMaskInnerRadius", 0.35f, 0.01f, 0.0f, 2.0f, "%.3f");
                dragF("Meter Mask Outer", "meterMaskOuterRadius", 1.0f, 0.01f, 0.0f, 2.0f, "%.3f");
                InspectorHelp("Where the falloff runs, as fractions of the HALF-DIAGONAL: 1.0 is "
                              "the frame corner. Inside the inner radius a sample counts fully, "
                              "past the outer one it counts least. The shader floors the weight at "
                              "0.05 (as UE does), so the edges always contribute a little - a "
                              "subject filling the border must still be metered, not dropped.");
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
                InspectorHelp("Distance from the target, IN STOPS, where adaptation stops running "
                              "at a constant rate and starts easing in. Far away it holds the "
                              "stops/second above, so a big transition is time-bounded; inside this "
                              "distance the last fraction of a stop decelerates instead of arriving "
                              "at full speed and stopping dead. UE's "
                              "r.EyeAdaptation.ExponentialTransitionDistance, same default of 1.5.");
                dragF("Black Bucket Influence", "blackBucketInfluence", 1.0f, 0.01f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("Weight of the DARKEST histogram bucket. 1 counts it normally. Lower "
                              "it when a scene has large regions of pure black - letterboxing, an "
                              "unlit interior - which would otherwise drag the meter down and open "
                              "the camera on nothing. UE call it "
                              "r.EyeAdaptation.BlackHistogramBucketInfluence.");
            }
            else
            {
                // P16.6: the camera, as three settings a photographer knows. EV100 is DERIVED and
                // is shown read-only below -- there is no second control for the same number.
                dragF("Aperture f/", "apertureFStop", 16.0f, 0.05f, 1.0f, 32.0f, "%.1f");
                InspectorHelp(
                    "The iris, as an f-number. Every whole stop is a factor of two in light: "
                    "f/1.4, 2, 2.8, 4, 5.6, 8, 11, 16, 22.");
                dragF("Shutter (s)", "shutterSpeedSec", 1.0f / 125.0f, 1.0f, 1.0f / 8000.0f, 30.0f,
                      "%.5f", ImGuiSliderFlags_Logarithmic);
                InspectorHelp(
                    "Exposure time in SECONDS: 1/125 is 0.008, 1/500 is 0.002. It does not blur "
                    "anything here -- the renderer has no shutter -- it only meters light.");
                dragF("ISO", "isoSensitivity", 100.0f, 1.0f, 25.0f, 6400.0f, "%.0f",
                      ImGuiSliderFlags_Logarithmic);
                InspectorHelp("Sensor sensitivity. 100 is the reference; doubling it is one stop.");
                dragF("Exposure Compensation (EV)", "manualCompensationEv", 0.0f, 0.02f, -5.0f, 5.0f,
                      "%+.2f");
                InspectorHelp(
                    "A quick +/- on top of the three settings above. POSITIVE BRIGHTENS.\n\n"
                    "It is the trim you reach for while looking at the frame, so you do not have to "
                    "re-solve the shutter in your head to make a scene half a stop warmer.\n\n"
                    "P16.13: THIS IS MANUAL MODE'S OWN FIELD (`manualCompensationEv`). Auto mode "
                    "has a separate one, because they are different jobs -- auto offsets what the "
                    "METER decided, this offsets a number you solved by hand -- and while they "
                    "shared a field, trimming one mode silently re-trimmed the other and back "
                    "again. Switching modes no longer carries a trim across.");
                InspectorHelp("Held exactly, with no metering at all.\n\n"
                              "THE DEFAULTS ARE SUNNY-16 -- f/16, 1/125, ISO 100, EV 14.97 -- and since P16 the lights are in real lux, so an outdoor scene is correctly exposed with nobody typing anything.\n\n"
                              "EV100 = log2(N^2/t) - log2(ISO/100). It is shown below, read-only: it is derived from these three and having it as a fourth control would be two names for one number.\n\n"
                              "A level authored before P16.6 carried a hand-solved `manualEv100`; it is migrated by holding the shutter and ISO and solving the aperture, so the frame does not move.");
            }

            // Plan section 6.2 wants both representations visible, because EV is the authored
            // quantity but the linear multiplier is what a shader bug would show up in.
            ImGui::Separator();
            // P16.13: in manual the readout is the EV that ACTUALLY RUNS, i.e. the solved camera EV
            // minus the manual trim -- the same arithmetic exposure_solve_cs does. Showing the
            // untrimmed value would have this row disagree with the image as soon as the trim moved.
            const float shownEv = automatic
                ? JsonFloat(tgt(), "compensationEv", 0.0f)
                : (render::Ev100FromCamera(JsonFloat(tgt(), "apertureFStop", 16.0f),
                                           JsonFloat(tgt(), "shutterSpeedSec", 1.0f / 125.0f),
                                           JsonFloat(tgt(), "isoSensitivity", 100.0f)) -
                   JsonFloat(tgt(), "manualCompensationEv", 0.0f));
            const float multiplier = tgt().value("enabled", false)
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
            InspectorHelp("Stops ABOVE middle grey before the highlight scaling starts. 0 applies "
                          "it everywhere brighter than grey.");
            dragF("Local SH Threshold", "localShadowThreshold", 0.0f, 0.05f, 0.0f, 8.0f, "%.2f");
            InspectorHelp("Stops away from middle grey before the effect starts, so mid-tones -- "
                          "usually the subject -- are left alone.");
            dragF("Local Blur Blend", "localBlurredBlend", 0.6f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("What the operator judges a neighbourhood by (UE Blurred Luminance "
                          "Blend, default 0.6). 0 = the bilateral grid only: each pixel's "
                          "neighbourhood is the nearby pixels about AS BRIGHT as itself, so a "
                          "bright sky does not lift the base of the water just under the horizon "
                          "(the dark band). 1 = a plain blur only, which bleeds across every "
                          "high-contrast edge -- what shipped before the grid.");
            dragF("Local Blur Size %", "localBlurredKernelPercent", 50.0f, 0.5f, 0.0f, 100.0f, "%.1f %%");
            InspectorHelp("The width of that blur as a percent of the view (UE Blurred Luminance "
                          "Kernel Size Percent, default 50). Narrow makes the operator more local; "
                          "it also lets one tiny, very bright source -- the sun's disc -- lift the "
                          "neighbourhood round itself, and the highlight compression then draws a "
                          "dark ring round it (seen at the ~2 % this used to be).");
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
                        nlohmann::json after = withField("localHighlightContrast", preset.hl);
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

            // Time-of-day plan E2.
            ImGui::SeparatorText("Night vision");
            dragF("Night Vision", "nightVision", 1.0f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("The eye's rods taking over in the dark (the Purkinje shift): a cold cast falls "
                          "over the scene and its colours weaken -- why a moonlit night looks COLD although "
                          "moonlight is warmer than sunlight. How much follows the light by itself: none "
                          "above ~3 cd/m2 (a day and its shadows are untouched), full below ~0.005, per "
                          "pixel against what the eye is adapted to, so the moon's disc and a lamp keep "
                          "their colour. 1 = that estimate, 0 = a camera's colour vision at any light. "
                          "Colour only -- how DARK the night reads is the compensation curve's. "
                          "--set=exposure.nightVision:<0..1>");
            {
                const render::CameraExposureSettings defaults{};
                const Math::float3 t = JsonFloat3(tgt(), "nightTint",
                    Math::float3(defaults.nightTint[0], defaults.nightTint[1], defaults.nightTint[2]));
                float tv[3] = { t.x, t.y, t.z };
                const bool changed = ImGui::ColorEdit3("Night Tint", tv);
                beginContinuousEdit(changed);
                if (changed) { tgt()["nightTint"] = { tv[0], tv[1], tv[2] }; }
                trackContinuousEdit(changed);
                InspectorHelp("The cold cast of night vision. Only its HUE matters -- it is applied at unit "
                              "luminance, so it never brightens or darkens. The default is the rods' "
                              "blue-cyan, mild: the cold LIGHT itself is the moon's (Time of Day > Moon > "
                              "Light Colour). White = no cast.");
                dragF("Night Desaturation", "nightDesaturation", defaults.nightDesaturation, 0.01f, 0.0f, 1.0f, "%.2f");
                InspectorHelp("How much colour the rods take at full night vision. 0 = the cold tint alone, "
                              "every hue kept (UDS's look: green palms stay green under blue light); 1 = "
                              "the rods' colourless grey, the physiology taken literally -- which renders a "
                              "moonlit night black and white. --set=exposure.nightDesaturation:<0..1>");
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
                if (automatic)
                {
                    // E1: the curve's share is the solve's whole compensation less the static trim.
                    ImGui::TextDisabled("Eye %+.3f EV100, curve %+.2f EV",
                        readback.eyeEv100 + JsonFloat(tgt(), "compensationEv", 0.0f),
                        readback.compensationEv - JsonFloat(tgt(), "compensationEv", 0.0f));
                }
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
                    const float lowPct = JsonFloat(tgt(), "lowPercentile", 0.15f);
                    const float highPct = JsonFloat(tgt(), "highPercentile", 0.80f);
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
        };
        const auto drawColorPipeline = [&]()
        {
            // P3C. Same set the dev window exposes, so a look tuned live can be written into the
            // level here rather than only through the clipboard button.
            const std::string curve = tgt().value("toneCurve", std::string("legacy"));
            int curveIndex = (curve == "agx") ? 1 : ((curve == "filmic" || curve == "film") ? 2 : 0);
            {
                const char* kCurveNames[] = { "Legacy (ACES fit)", "AgX", "Filmic (Unreal)" };
                const bool changed = ImGui::Combo("Tone Curve", &curveIndex, kCurveNames, 3);
                beginContinuousEdit(changed);
                if (changed)
                {
                    tgt()["toneCurve"] = (curveIndex == 1) ? "agx" : (curveIndex == 2 ? "filmic" : "legacy");
                }
                trackContinuousEdit(changed);
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
                        nlohmann::json after = withField("gradeSaturation", preset.sat);
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
                    nlohmann::json after = withField("filmSlope", 0.88f);
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
                    nlohmann::json after = withField("agxSlope", 1.0f); after["agxPower"] = 1.0f; after["agxSaturation"] = 1.0f;
                    executeChange(std::move(after), historyLabel);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Punchy"))
                {
                    nlohmann::json after = withField("agxSlope", 1.0f); after["agxPower"] = 1.35f; after["agxSaturation"] = 1.4f;
                    executeChange(std::move(after), historyLabel);
                }
                ImGui::PopID();
            }
        };
        const auto drawGtao = [&]()
        {
            // P6B. Every tooltip states the PERFORMANCE consequence as well as the look one, because
            // three of these knobs are linear in GPU cost and that is not guessable from the name.
            {
                bool gtaoEnabled = tgt().value("enabled", false);
                const bool changed = ImGui::Checkbox("Enabled", &gtaoEnabled);
                beginContinuousEdit(changed);
                if (changed) { tgt()["enabled"] = gtaoEnabled; }
                trackContinuousEdit(changed);
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
            dragF("Sky Radius", "skyRadius", 25.0f, 0.2f, 0.0f, 60.0f, "%.1f m");
            InspectorHelp("P16.4. A SECOND, much wider occlusion radius that damps the SKY FILL only "
                          "- whether this patch of ground is under a canopy or inside a doorway. "
                          "World Radius above cannot answer that: it is a contact radius, and with "
                          "one radius the sky reaches under a palm crown as freely as it reaches "
                          "open sand. At or below World Radius this is OFF and the pass is bit-for-"
                          "bit what it was. COSTS a second horizon walk, so roughly doubles the raw "
                          "GTAO pass when on.");
            dragF("Sky Intensity", "skyIntensity", 1.0f, 0.02f, 0.0f, 3.0f, "%.2f");
            InspectorHelp("The MID-RANGE channel's own strength, scaling its darkening exponent "
                          "independently of the contact channel (1 = the shared-Intensity "
                          "behaviour). At 0 the second walk's COMPUTE PATH is off entirely - the "
                          "same exact no-op as Sky Radius at the bottom, kept as its own switch.");
            {
                int skyMip = static_cast<int>(tgt().value("skyMipBias", 2u));
                const bool changed = ImGui::SliderInt("Sky Mip Bias", &skyMip, 0, 5);
                beginContinuousEdit(changed);
                if (changed) { tgt()["skyMipBias"] = static_cast<std::uint32_t>(skyMip < 0 ? 0 : skyMip); }
                trackContinuousEdit(changed);
            }
            InspectorHelp("Depth-pyramid level the WIDE walk starts from (the contact walk keeps its "
                          "own bias below). Its taps are tens of pixels apart, so a mip-0 fetch lands "
                          "nowhere near the previous one; a coarser level caches better and "
                          "aggregates, which is what you want at a scale where one texel of leaf does "
                          "not decide whether the ground is sheltered. Higher is CHEAPER. Inert while "
                          "Sky Radius is off or Use HZB is unchecked.");
            dragF("Intensity", "intensity", 1.0f, 0.01f, 0.1f, 4.0f, "%.2f");
            InspectorHelp("Exponent on the occlusion term - above 1 deepens, below 1 lifts. FREE.");
            dragF("Thickness", "thickness", 0.6f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("How solid an occluder is assumed to be behind its silhouette. 1 = fully "
                          "solid, so foliage casts a slab behind every leaf; low values keep leaves "
                          "thin. UE equivalent works out to 0.75. FREE.");

            ImGui::SeparatorText("Quality (these cost GPU time)");
            {
                int angles = static_cast<int>(tgt().value("numAngles", 2u));
                const bool changed = ImGui::SliderInt("Directions", &angles, 1, 8);
                beginContinuousEdit(changed);
                if (changed) { tgt()["numAngles"] = static_cast<std::uint32_t>(angles < 1 ? 1 : angles); }
                trackContinuousEdit(changed);
            }
            InspectorHelp("Screen directions searched per pixel. Cost is LINEAR in this - doubling it "
                          "roughly doubles the raw pass. 2 is the UE default (r.GTAO.NumAngles) and "
                          "leans on the temporal stage to average the rest over time.");
            {
                int steps = static_cast<int>(tgt().value("numSteps", 6u));
                const bool changed = ImGui::SliderInt("Steps", &steps, 2, 16);
                beginContinuousEdit(changed);
                if (changed) { tgt()["numSteps"] = static_cast<std::uint32_t>(steps < 1 ? 1 : steps); }
                trackContinuousEdit(changed);
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
                bool denoise = tgt().value("denoise", true);
                const bool changed = ImGui::Checkbox("Denoise", &denoise);
                beginContinuousEdit(changed);
                if (changed) { tgt()["denoise"] = denoise; }
                trackContinuousEdit(changed);
            }
            InspectorHelp("Bilateral 5x5 across depth and normal, about 0.017 ms. Removes roughly 40% "
                          "of the raw noise; off is immediately visible as per-pixel grain.");
            {
                bool temporal = tgt().value("temporal", true);
                const bool changed = ImGui::Checkbox("Temporal", &temporal);
                beginContinuousEdit(changed);
                if (changed) { tgt()["temporal"] = temporal; }
                trackContinuousEdit(changed);
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
                bool gbufferNormal = tgt().value("useGBufferNormal", false);
                const bool changed = ImGui::Checkbox("Normal from G-buffer", &gbufferNormal);
                beginContinuousEdit(changed);
                if (changed) { tgt()["useGBufferNormal"] = gbufferNormal; }
                trackContinuousEdit(changed);
            }
            {
                bool useHzb = tgt().value("useHzb", true);
                const bool changed = ImGui::Checkbox("Use depth pyramid (HZB)", &useHzb);
                beginContinuousEdit(changed);
                if (changed) { tgt()["useHzb"] = useHzb; }
                trackContinuousEdit(changed);
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
        };
        const auto drawHeightFog = [&]()
        {
            // P7. The model is transcribed from UE's HeightFogCommon.ush, so each tooltip names the
            // UE parameter it corresponds to -- and, where a UE default does NOT carry over, says
            // why. That distinction is the whole reason these numbers are not simply theirs.
            {
                bool fogEnabled = tgt().value("enabled", false);
                const bool changed = ImGui::Checkbox("Enabled", &fogEnabled);
                beginContinuousEdit(changed);
                if (changed) { tgt()["enabled"] = fogEnabled; }
                trackContinuousEdit(changed);
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

            ImGui::SeparatorText("Volumetric fog");
            checkB("Volumetric (froxel) fog", "volumetric", false);
            InspectorHelp("UE's VolumetricFog on top of the analytic model: inside the volume distance the "
                          "air is a lit, sun-shadowed volume (light shafts through the palms), beyond it the "
                          "analytic fog continues. --set=fog.volumetric:1");
            dragF("Volume Distance", "volumetricDistance", 300.0f, 1.0f, 10.0f, 2000.0f, "%.0f m");
            dragF("Albedo", "albedo", 1.0f, 0.01f, 0.0f, 1.0f, "%.2f");
            dragF("Extinction Scale", "extinctionScale", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
            dragF("Phase G", "phaseG", 0.2f, 0.01f, -0.99f, 0.99f, "%.2f");
            InspectorHelp("Henyey-Greenstein anisotropy (UE VolumetricFogScatteringDistribution, their "
                          "default 0.2): towards 1 the shafts brighten when looking into the sun.");
            dragF("Volume Sun Intensity", "sunScatter", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f"); // the sun INSIDE the volume; "Sun Scatter" above is the analytic lobe beyond it
            dragF("Volume Sky Intensity", "skyScatter", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
            dragF("History Weight", "historyWeight", 0.9f, 0.01f, 0.0f, 0.99f, "%.2f");
            checkB("Temporal reprojection", "temporal", true);
            checkB("Jitter (with the history)", "jitter", true);
            dragF("Samples Per Cell", "samplesPerCell", 2.0f, 0.05f, 1.0f, 4.0f, "%.0f");
            InspectorHelp("Jittered samples per froxel every frame. 1 = UE (one sample + TAA); 2 = antithetic pair, "
                          "removes the flicker at a spot tip; 3-4 = more, at the pass cost.");
            checkB("Conservative depth (skip cells behind geometry)", "conservativeDepth", true);
            checkB("Local lights in the volume", "localLights", true);
            dragF("Local Light Intensity", "localLightScatter", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
            dragF("Local Cone Soft Fading", "localSoftFading", 1.0f, 0.05f, 0.0f, 4.0f, "%.2f froxels");
            InspectorHelp("UE r.VolumetricFog.LightSoftFading: a spot cone edge fades over this many froxels, "
                          "which is what removes the stepped, flickering edge. 0 = hard edge (UE default).");
            dragF("Local Distance Bias", "localDistanceBias", 1.0f, 0.05f, 0.0f, 4.0f, "%.2f");

            ImGui::SeparatorText("Sky sampling");
            dragF("Sky Blur", "skyBlur", 0.5f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("How blurred the sky is where it is read as the fog's COLOUR, expressed as "
                          "a roughness. 0 samples it sharp, and then the fog is literally a picture "
                          "of what stands behind the surface - cloud edges and the sunset band print "
                          "themselves onto the palms in front of them. Only the lightly-fogged end "
                          "is blurred: a fully fogged pixel always converges on the sharp sky, "
                          "because the sky IS fog of infinite depth and anything else seams at the "
                          "horizon. FREE - it is a mip choice, not an extra sample.");
            dragF("Non-Directional Dist", "nonDirectionalDistance", 0.0f, 0.5f, 0.0f, 10000.0f, "%.0f m");
            dragF("Fully Directional Dist", "fullyDirectionalDistance", 0.0f, 5.0f, 0.0f, 100000.0f, "%.0f m");
            InspectorHelp("Where the sky sample stops being a direction and becomes an average. "
                          "Nearer than the first distance the fog is coloured by the WHOLE SPHERE; "
                          "past the second it is the sky in the direction you are looking, and "
                          "between them it crossfades. BOTH ZERO = off, which is how this ships. "
                          "UE run it at 10 m / 1000 m, but they MULTIPLY the cubemap into an "
                          "authored fog colour while we use the sample as the colour itself, so "
                          "their safe near-field average turns into milk-white water here. Raise "
                          "them only with a before/after in hand.");

            ImGui::TextDisabled("Fog colour comes from the SKY along the view ray - the same shape as");
            ImGui::TextDisabled("UE's InscatteringColorCubemap, generated from the sky instead");
            ImGui::TextDisabled("of authored. That is what removes the horizon seam by construction.");
            // The debug views deliberately live only in the dev window: they are a viewing mode,
            // not level data, and putting them on the object would invite them into the save.
            ImGui::TextDisabled("Debug views (transmittance / in-scattering) are in the F1 window,");
            ImGui::TextDisabled("Render tab - they are a viewing mode, not level data.");
            ImGui::TextDisabled("Fix exposure when comparing: auto-exposure reacts to fog and");
            ImGui::TextDisabled("shifts the WHOLE frame, sky included.");
        };
        const auto drawBloom = [&]()
        {
            // P8. Every tooltip says where the number is measured, because the two that matter --
            // threshold and intensity -- are in units that are easy to assume wrongly.
            {
                bool bloomEnabled = tgt().value("enabled", false);
                const bool changed = ImGui::Checkbox("Enabled", &bloomEnabled);
                beginContinuousEdit(changed);
                if (changed) { tgt()["enabled"] = bloomEnabled; }
                trackContinuousEdit(changed);
            }
            InspectorHelp("HDR bloom composited into the image BEFORE the tone curve, in either of UE's "
                          "two forms (Method). Off schedules no pass at all rather than a pass that "
                          "adds zero, so off genuinely costs nothing.");

            ImGui::SeparatorText("Method");
            {
                int method = static_cast<int>(tgt().value("method", 0u));
                const char* kMethods[] = { "Standard (pyramid)", "Convolution (FFT)" };
                const bool changed = ImGui::Combo("Method", &method, kMethods, 2);
                beginContinuousEdit(changed);
                if (changed) { tgt()["method"] = static_cast<std::uint32_t>(method < 0 ? 0 : method); }
                trackContinuousEdit(changed);
            }
            InspectorHelp("STANDARD is UE's Gaussian bloom on a mip pyramid: cheap, symmetric, and "
                          "structurally incapable of a streak -- a stack of Gaussians has no shape "
                          "to give.\n\n"
                          "CONVOLUTION is UE's FFT bloom: the whole scene convolved with a kernel "
                          "IMAGE. Halo, starburst and rays all come out of that ONE kernel. It costs "
                          "more and runs at Resolution % of the display.\n\n"
                          "Only the knobs of the chosen method are shown.");

            dragF("Intensity", "intensity", 0.675f, 0.0005f, 0.0f, 64.0f, "%.4f",
                  ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
            InspectorHelp("UE's BloomIntensity, default 0.675. 0 is an exact no-op and skips the "
                          "chain. It also scales the lens ghosts (UE: LensFlareIntensity * "
                          "BloomIntensity). Logarithmic: the useful range spans orders of magnitude.\n\n"
                          "STANDARD: the weight of UE's six Gaussians, Bloom{n}Tint * Intensity / 6, "
                          "laid on the pyramid's levels by their sizes.\n\n"
                          "CONVOLUTION: UE's ScatterDispersionIntensity, the multiplier on the "
                          "fraction of the kernel's energy that SCATTERS. That fraction is a "
                          "property of the kernel IMAGE; every kernel in textures/ scatters what UE's "
                          "DefaultBloomKernel does (~1.75 % at the default size -- "
                          "tools/bloom_kernel_delta.py), so 0.1-1 is the range for all of them. The "
                          "light is PARTITIONED, not added: the scene is dimmed by exactly what the "
                          "flare takes.");

            if (tgt().value("method", 0u) == 0u)
            {
            ImGui::SeparatorText("Standard (pyramid) only");
            dragF("Threshold", "threshold", -1.0f, 0.01f, -1.0f, 16.0f, "%.2f");
            InspectorHelp("UE's FFT bloom has no threshold, and neither does the Convolution here "
                          "(its filter is the bright-pixel gain).\n\n"
                          "Luminance at which bloom starts, in the units the viewer sees -- UE's "
                          "BloomThreshold. NEGATIVE DISABLES IT, and that is UE's own default "
                          "(-1 in Scene.cpp), documented there as \"all pixels affect bloom "
                          "equally (physically correct)\". A lens scatters light from EVERYTHING "
                          "in front of it, not only from whatever passes a brightness test.\n\n"
                          "It also decides how bloom answers to EXPOSURE. With a threshold, raising "
                          "exposure both scales the pixels already over the line and pushes new "
                          "ones over it, so bloom grows faster than the image does. At -1 the "
                          "response is exactly linear: twice the exposure, twice the bloom.");
            dragF("Soft Knee", "softKnee", 0.5f, 0.01f, 0.05f, 4.0f, "%.2f");
            InspectorHelp("Slope of the ramp above the threshold; UE hardwire 0.5, which is the "
                          "default here. Lower = a longer shoulder, so highlights ease into the "
                          "bloom instead of switching on at a fixed brightness.");
            {
                bool firefly = tgt().value("fireflyClamp", false);
                const bool changed = ImGui::Checkbox("Firefly Clamp", &firefly);
                beginContinuousEdit(changed);
                if (changed) { tgt()["fireflyClamp"] = firefly; }
                trackContinuousEdit(changed);
            }
            InspectorHelp("OURS, off by default -- UE's bloom downsample has none. A Karis average on "
                          "the FIRST downsample only: each tap is weighted by 1/(1+luma), so one "
                          "blown-out texel is averaged DOWN instead of surviving the whole pyramid. "
                          "Turn it on if moving sun glints on water pump the entire frame.");
            dragF("Radius", "radius", 1.0f, 0.01f, 0.0f, 4.0f, "%.2f");
            InspectorHelp("Tap spacing of the tent upsample, in DESTINATION texels, so it means the "
                          "same thing at every level and at every resolution. It spreads the same "
                          "energy wider rather than adding any: widen it and the halo gets larger "
                          "and fainter, it does not get brighter.");
            }

            if (tgt().value("method", 0u) == 1u)
            {
                ImGui::SeparatorText("Kernel (convolution only)");
                // P8C-2r: WHICH image. Everything downstream -- placement, the core clamp, the
                // centre/scatter split -- is derived from the pixels, so this one pick changes the
                // entire character of the glare with nothing else to retune. The list is whatever
                // square FP16 DDS sits in textures/; it is scanned once and cached, because an
                // inspector that touches the filesystem every frame is a stutter waiting to happen.
                {
                    static std::vector<std::string> kernels;
                    static bool scanned = false;
                    if (!scanned)
                    {
                        scanned = true;
                        std::error_code ec;
                        for (const auto& e : std::filesystem::directory_iterator("textures", ec))
                        {
                            if (ec) { break; }
                            if (!e.is_regular_file()) { continue; }
                            const std::filesystem::path& fp = e.path();
                            std::string ext = fp.extension().string();
                            for (char& c : ext) { c = static_cast<char>(::tolower(c)); }
                            if (ext == ".dds") { kernels.push_back("textures/" + fp.filename().string()); }
                        }
                        std::sort(kernels.begin(), kernels.end());
                    }
                    const std::string cur = tgt().value("convKernel", std::string("textures/BloomKernelStar.dds"));
                    int sel = -1;
                    std::vector<const char*> items;
                    items.reserve(kernels.size());
                    for (size_t i = 0; i < kernels.size(); ++i)
                    {
                        items.push_back(kernels[i].c_str());
                        if (kernels[i] == cur) { sel = static_cast<int>(i); }
                    }
                    if (!items.empty())
                    {
                        const bool changed = ImGui::Combo("Kernel Image", &sel, items.data(),
                                                          static_cast<int>(items.size()));
                        beginContinuousEdit(changed);
                        if (changed && sel >= 0) { tgt()["convKernel"] = kernels[static_cast<size_t>(sel)]; }
                        trackContinuousEdit(changed);
                    }
                }
                InspectorHelp("The kernel photograph itself. Anything square and FP16 in textures/ "
                              "works: BloomKernelStar is the DERIVED one (nine-bladed diffraction, "
                              "18 rays, measured 7.6-12.3x ray contrast), DefaultBloomKernel is "
                              "UE's original photograph (2.6-4.2x -- their weak rays are the "
                              "arithmetic consequence of their broad glow, not a defect). "
                              "Switching reloads the image and re-surveys it; nothing else needs "
                              "retuning, though the centre/scatter fractions differ per image so "
                              "Intensity will not mean quite the same thing across a swap.");
                // P8C-2: the kernel is UE's photographed DefaultBloomKernel. The star, its rays,
                // the halo and the rainbow dispersion are IN the image -- the aperture generator
                // and its shape controls (kernel radius, spokes, chroma) are retired.
                dragF("Kernel Size", "convSize", 1.0f, 0.01f, 0.02f, 1.0f, "%.2f");
                InspectorHelp("The kernel photograph's width as a fraction of the viewport -- "
                              "UE's BloomConvolutionSize, same units and same default of 1.0. "
                              "This is the ONE size control: the visible glare is the kernel "
                              "image, scaled.");
                {
                    float ktint[3] = { 1.0f, 1.0f, 1.0f };
                    if (tgt().contains("convKernelTint") && tgt()["convKernelTint"].is_array() &&
                        tgt()["convKernelTint"].size() == 3)
                    {
                        for (int i = 0; i < 3; ++i)
                        {
                            ktint[i] = tgt()["convKernelTint"][i].get<float>();
                        }
                    }
                    const bool changed = ImGui::ColorEdit3("Kernel Tint", ktint,
                        ImGuiColorEditFlags_Float);
                    beginContinuousEdit(changed);
                    if (changed) { tgt()["convKernelTint"] = { ktint[0], ktint[1], ktint[2] }; }
                    trackContinuousEdit(changed);
                }
                InspectorHelp("How strongly each channel SCATTERS (ours; UE's ScatterDispersion is "
                              "one number): a bluish tint gives a bluish flare, and the light a "
                              "channel does not scatter stays in the scene, so the frame's colour "
                              "balance is kept. It cannot sit in the kernel image: the transform "
                              "normalises each channel by its own sum, as UE's does.");

                ImGui::SeparatorText("Bright-pixel gain (UE's prefilter)");
                dragF("Gain Slope", "convPreFilterMult", 15.0f, 0.05f, 0.0f, 64.0f, "%.2f");
                InspectorHelp("UE's BloomConvolutionPreFilterMult (default 15), and the ONLY "
                              "filtering their FFT bloom has -- there is no threshold anywhere in "
                              "that path. Nothing is cut: a pixel below Min passes untouched, a "
                              "pixel above Min has its luminance remapped with this slope and "
                              "clamped at Max. 0 = off (under UE's own gate 0 would clamp every "
                              "pixel above Min to Min).");
                dragF("Gain Min", "convPreFilterMin", 7.0f, 0.05f, 0.0f, 1000.0f, "%.2f",
                      ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
                InspectorHelp("Where the boost starts (UE default 7), in the PRE-EXPOSED units the "
                              "scene is stored in -- UE's FilterPixel runs on the pre-exposed scene "
                              "colour too. Below it, pixels are left exactly as they are. Off when "
                              "Min >= Max (UE's gate).");
                dragF("Gain Max", "convPreFilterMax", 15000.0f, 10.0f, 0.0f, 65504.0f, "%.1f",
                      ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
                InspectorHelp("The CEILING the boosted luminance is clamped to (UE default 15000, "
                              "pre-exposed) -- what any single pixel can contribute, however bright "
                              "it gets. The physical sun disc sits far above everything else in "
                              "the frame; this is what bounds its flare.");
                dragF("Resolution %", "convPercent", 50.0f, 0.5f, 10.0f, 50.0f, "%.0f %%");
                InspectorHelp("Resolution of the convolution as a percent of the display -- UE's "
                              "r.Bloom.ScreenPercentage (their default is 100; 50 is this "
                              "engine's grid ceiling). The first P8C ran at 12.5, and a 1-2 texel "
                              "diffraction ray upscaled 4x per axis is a dashed line of squares "
                              "-- the 'ragged crown'. Lowering this buys the transform cost back.");
                checkB("Before the upscaler", "convBeforeUpscale", true);
                InspectorHelp("Convolve the scene BEFORE DLSS. DLSS takes a glint of a few pixels "
                              "for aliasing and flattens it -- a chrome sphere's sun glint kept about "
                              "a tenth of its energy, so the kernel's star never formed. No effect "
                              "without DLSS.");
                dragF("Temporal Weight", "convTemporalWeight", 0.1f, 0.005f, 0.01f, 1.0f, "%.3f");
                InspectorHelp("With Before the upscaler: the bloom's light (the convolution, the ghosts, "
                              "the streak) is accumulated over frames, and this is the new frame's share. "
                              "The scene before DLSS is jittered, so a glint smaller than a pixel lands in "
                              "it one frame and misses the next -- its star flickered. UE's bloom reads "
                              "TSR's output, which does this averaging. Lower = steadier, and a moving "
                              "glint's flare lags more. 1 = off.");
                dragF("Sun Kernel Scale", "convSunKernelScale", 1.0f, 0.005f, 0.0f, 10.0f, "%.3f",
                      ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
                InspectorHelp("ART, not physics: the sun disc's own share of the kernel's light (its "
                              "star, halo and rays). The disc carries thousands of times a glint's "
                              "light, so with one Intensity the sun's star dominates and a glint's "
                              "barely shows -- raise Intensity for everything and pull the sun back "
                              "here. The sun is the sky within Sun Radius of it (with the ghosts), "
                              "applied after Boost. 1 = the sun like any other source.");
            }

            // P8C-2l: THE FLARES ARE NOT PART OF EITHER BLOOM METHOD. They read the HDR image
            // and add into the same bloom target the pyramid and the convolution both write, so
            // they live outside the method's own block -- and outside its `if`, or half of them
            // would vanish from the UI the moment someone chose Standard.
            ImGui::SeparatorText("Anamorphic Streak (any bloom method)");
            // P8C-2h/l: an anisotropic PYRAMID of its own (KinoStreak's structure), reading the
            // HDR image and adding into the bloom target -- it belongs to neither bloom method
            // and runs with both.
            dragF("Anamorphic Intensity", "convAnamorphicIntensity", 0.0f, 0.02f, 0.0f, 8.0f, "%.2f");
            InspectorHelp("Direct brightness of the streak. 0 = off (the passes do not run).");
            dragF("Anamorphic Threshold", "convAnamorphicThreshold", 1.5f, 0.05f, 0.1f, 10000.0f, "%.2f",
                  ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
            InspectorHelp("The streak's OWN threshold, and since P8C-2h the ONLY narrowing "
                          "control -- the vertical erosion that used to sit below was deleted "
                          "with the cascade (a min-filter cannot taper at a frame edge). In the "
                          "PRE-EXPOSED units the scene is stored in, as the ghost threshold and "
                          "UE's LensFlareThreshold. Higher values take only the CORE of a "
                          "source, so a bright sky round the sun stops throwing a screen-tall "
                          "band; the physical sun disc clears any sensible value by thousands.");
            dragF("Anamorphic Length", "convAnamorphicLength", 0.28f, 0.005f, 0.01f, 1.0f, "%.3f");
            InspectorHelp("How far the band reaches, as a fraction of the screen width -- "
                          "the VISIBLE extent, so 0.1 draws a band about a tenth of the "
                          "screen long (plus the source's own width, which no filter can "
                          "shorten). It was authored as a 1/e until P8C-2e, and a 1/e lies by "
                          "3.4x. It is realised by WEIGHTING PYRAMID LEVELS -- the level whose "
                          "reach matches the number, with a tent across its neighbours for the "
                          "fraction -- and verified in numpy against the exact tap pattern: "
                          "asked 128/256/512/768 px delivered 92/204/420/788, i.e. 0.72-1.03x. "
                          "The floor is level 0's own reach, about 30 px; the ceiling is the "
                          "deepest level's, about 3200 px, so the whole slider is live.");

            dragF("Anamorphic Width", "convAnamorphicWidth", 3.0f, 0.1f, 0.5f, 30.0f, "%.1f px");
            InspectorHelp("The band's final soft width: a small vertical Gaussian applied at "
                          "composite, in display pixels. Works on the already-blurred band, "
                          "so it can be narrow without aliasing.");
            dragF("Anamorphic Chroma", "convAnamorphicChroma", 0.5f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("Spreads the per-channel 1/e lengths: blue runs ~45% farther and "
                          "red ~30% shorter at 1.0, so the tail shifts white -> blue along "
                          "its length -- the look of real cylindrical-element coatings.");
            {
                float tint[3] = { 1.0f, 1.0f, 1.0f };
                if (tgt().contains("convAnamorphicTint") &&
                    tgt()["convAnamorphicTint"].is_array() &&
                    tgt()["convAnamorphicTint"].size() == 3)
                {
                    for (int i = 0; i < 3; ++i)
                    {
                        tint[i] = tgt()["convAnamorphicTint"][i].get<float>();
                    }
                }
                const bool changed = ImGui::ColorEdit3("Anamorphic Tint", tint,
                    ImGuiColorEditFlags_Float);
                beginContinuousEdit(changed);
                if (changed) { tgt()["convAnamorphicTint"] = { tint[0], tint[1], tint[2] }; }
                trackContinuousEdit(changed);
            }
            InspectorHelp("A plain colour multiplier on the whole band, on top of the "
                          "chroma's own gradient.");

            ImGui::SeparatorText("Lens Ghosts (any bloom method)");
            {
                int ghosts = static_cast<int>(tgt().value("convGhosts", 0u));
                const bool changed = ImGui::SliderInt("Ghost Count", &ghosts, 0, 8);
                beginContinuousEdit(changed);
                if (changed) { tgt()["convGhosts"] = static_cast<std::uint32_t>(ghosts < 0 ? 0 : ghosts); }
                trackContinuousEdit(changed);
            }
            InspectorHelp("P8C-2: UE's actual mechanism, both halves. A bokeh SCATTER splats "
                          "one iris sprite per bright pixel of the thresholded scene -- the "
                          "output is the real defocused image of the real sources -- and the "
                          "composite lays N copies of that image scaled about the screen "
                          "centre (UE's LensFlareTints table: two on the source's side, the "
                          "rest mirrored through the centre). No sun position and no sprite "
                          "atlas exist anywhere: two suns give two chains for free.");
            {
                int blades = static_cast<int>(tgt().value("convBlades", 6u));
                const bool changed = ImGui::SliderInt("Blades", &blades, 0, 12);
                beginContinuousEdit(changed);
                if (changed) { tgt()["convBlades"] = static_cast<std::uint32_t>(blades < 0 ? 0 : blades); }
                trackContinuousEdit(changed);
            }
            InspectorHelp("Number of iris blades in the BOKEH SPRITE the scatter splats -- 0 "
                          "is a round bokeh. It shapes the ghosts only: the bloom kernel is a "
                          "photograph and carries its own star.\n\n"
                          "MEASURED, so you know what to expect. Against a 1/255 noise floor, "
                          "3 blades vs 8 moves the frame by 8/255 and 0 (round) vs 8 by 2/255 "
                          "-- an octagon IS very nearly a circle, so the low counts are where "
                          "this control lives. It also needs Ghost Size to be LARGER than the "
                          "source: a ghost is source-convolved-with-sprite, and the bigger of "
                          "the two wins the shape. Blade ROTATION used to sit here and was "
                          "removed in P8C-2d for measuring 4/255 against that same floor -- "
                          "superposition over an extended source washes an orientation out "
                          "entirely.");
            dragF("Ghost Size", "convGhostBokeh", 3.0f, 0.05f, 0.0f, 32.0f, "%.2f %%");
            InspectorHelp("Bokeh sprite radius as a percent of frame width -- UE's "
                          "LensFlareBokehSize, same units and same default of 3. Bigger "
                          "copies are correspondingly dimmer: same light over more area.\n\n"
                          "IT IS ALSO THE SHAPE CONTROL, which is not obvious. A ghost is the "
                          "source CONVOLVED with this sprite, so whichever of the two is "
                          "bigger wins: with the sprite smaller than the source, every ghost "
                          "reproduces the SOURCE's outline -- at 0.5% the sun's ragged corona "
                          "and the water's glitter path came out as recognisable mirrored "
                          "smears in the sky. At UE's 3 the sprite dominates and the chain "
                          "reads as bokeh discs again.");
            dragF("Ghost Intensity", "convGhostIntensity", 1.0f, 0.001f, 0.0f, 1000.0f, "%.4f",
                  ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
            InspectorHelp("UE's LensFlareIntensity (default 1); the chain gets it times the "
                          "bloom Intensity, as theirs does. The colour comes from the sources "
                          "themselves, so a dimmer sun throws dimmer ghosts and a sky with "
                          "nothing above the threshold throws none at all.");
            dragF("Ghost Threshold", "convGhostThreshold", 8.0f, 0.05f, 0.0f, 65504.0f, "%.2f",
                  ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
            InspectorHelp("UE's LensFlareThreshold (default 8): a BINARY gate on R+G+B of the "
                          "pre-exposed scene, as theirs. Ghosts are images of SOURCES -- too low "
                          "and sunlit foliage becomes one (upside-down palms in the sky, "
                          "observed). The physical sun disc clears it by thousands, so its ghost "
                          "is the disc defocused through the iris. Raising it also cuts the "
                          "scatter's cost directly: collapsed quads rasterize nothing.");
            dragF("Sun Ghost Scale", "convSunGhostScale", 1.0f, 0.005f, 0.0f, 10.0f, "%.3f",
                  ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
            InspectorHelp("ART, not physics: the sun disc's own share of the ghosts. The disc outshines "
                          "a glint by thousands, so its chain is what you see while a glint's barely "
                          "shows -- raise Ghost Intensity for everything and pull the sun back here. "
                          "Found by direction (see Sun Radius). 1 = the sun like any other source.");
            dragF("Sun Radius (deg)", "convSunSplitRadiusDeg", 8.0f, 0.05f, 0.1f, 45.0f, "%.1f");
            InspectorHelp("What Sun Kernel Scale and Sun Ghost Scale call the sun: the SKY within this "
                          "angle of it (fully to half of it, fading out to all of it). The disc alone is "
                          "not enough -- the bright sky round it makes as much of its halo and ghosts. "
                          "Geometry never counts, so a glint next to the sun on screen (the sea under "
                          "a low sun) keeps the common share.");

            ImGui::SeparatorText("Sun glare");
            dragF("Sun Glare", "sunGlareIntensity", 0.005f, 0.0002f, 0.0f, 1.0f, "%.4f",
                  ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
            InspectorHelp("A corona around the sun that the convolution cannot give: an analytic "
                          "halo about the sun's direction, added to the bloom. 0 = off.\n\n"
                          "The SHARE OF THE SUN DISC'S LIGHT it carries: the disc's brightness "
                          "times its solid angle, spread over the halo -- so the radii change its "
                          "size, not its total. Its colour and strength are the "
                          "frame's own pixels over the disc, so a palm in front of the sun, a "
                          "cloud over it or a sunset dims and tints it by itself. Works with "
                          "either bloom method.");
            dragF("Glare Radius", "sunGlareRadiusDeg", 2.0f, 0.05f, 0.05f, 30.0f, "%.2f deg");
            InspectorHelp("The bright core's size, in degrees from the sun's centre (its "
                          "e-folding angle). Degrees, not pixels: the same at any resolution "
                          "or field of view.");
            dragF("Glare Veil", "sunGlareVeil", 0.15f, 0.005f, 0.0f, 2.0f, "%.3f");
            InspectorHelp("Weight of the WIDE, faint veil under the core -- the haze of light "
                          "a real lens or eye spreads far across the frame. 0 = core only.");
            dragF("Veil Radius", "sunGlareVeilRadiusDeg", 12.0f, 0.1f, 0.1f, 90.0f, "%.1f deg");
            InspectorHelp("Angle at which the veil has fallen to half.");
            dragF("Low-Sun Below", "sunGlareLowSunDeg", 12.0f, 0.1f, 0.0f, 90.0f, "%.1f deg");
            InspectorHelp("Sun elevation below which the glare starts to ease down (0 = never). The "
                          "exposure opens up as the sun sets, so a glare sized for noon washes a "
                          "sunset frame white.");
            dragF("Low-Sun Floor", "sunGlareLowSunFloor", 0.2f, 0.005f, 0.0f, 1.0f, "%.3f");
            InspectorHelp("What is left of the glare with the sun on the horizon, as a fraction of "
                          "Sun Glare (1 = no fade).");
            dragF("Glare Smoothing", "sunGlareSmoothingSec", 0.1f, 0.005f, 0.0f, 2.0f, "%.3f s");
            InspectorHelp("How slowly the glare AND the corona follow what shows of the sun disc: the time "
                          "constant of a temporal blend of the disc's metering. Leaves crossing the disc "
                          "made both dance; 0 = every frame as it is.");

            ImGui::SeparatorText("Sun corona");
            dragF("Corona", "sunRaysIntensity", 0.008f, 0.0002f, 0.0f, 2.0f, "%.4f",
                  ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
            InspectorHelp("The glare structure around the sun, drawn at full resolution in the tone "
                          "curve pass after Ritschel's \"Temporal Glare\": dense bright bundles with "
                          "dim gaps, fine needles inside them, warm tips, a faint rainbow halo ring. "
                          "0 = off.\n\n"
                          "Scaled by the sun disc's ENERGY (its solid angle over the corona's "
                          "nominal area), so it follows the disc's light, not its brightness per "
                          "pixel. Colour and strength are the frame's own pixels over the sun "
                          "disc, so anything in front of the sun takes the corona with it.");
            {
                static std::vector<std::string> coronaImages;
                static bool coronaScanned = false;
                if (!coronaScanned)
                {
                    coronaScanned = true;
                    std::error_code ec;
                    for (const auto& e : std::filesystem::directory_iterator("textures", ec))
                    {
                        if (ec) { break; }
                        if (!e.is_regular_file()) { continue; }
                        std::string name = e.path().filename().string();
                        std::string lower = name;
                        for (char& ch : lower) { ch = static_cast<char>(::tolower(ch)); }
                        const bool image = lower.size() > 4 &&
                            (lower.ends_with(".png") || lower.ends_with(".dds") || lower.ends_with(".jpg"));
                        if (image && (lower.find("corona") != std::string::npos || lower.find("glare") != std::string::npos))
                        {
                            coronaImages.push_back("textures/" + name);
                        }
                    }
                    std::sort(coronaImages.begin(), coronaImages.end());
                }
                const std::string cur = tgt().value("sunRaysTexture", std::string());
                std::vector<const char*> items{ "(procedural)" };
                int sel = cur.empty() ? 0 : -1;
                for (size_t i = 0; i < coronaImages.size(); ++i)
                {
                    items.push_back(coronaImages[i].c_str());
                    if (coronaImages[i] == cur) { sel = static_cast<int>(i) + 1; }
                }
                const bool changed = ImGui::Combo("Corona Image", &sel, items.data(), static_cast<int>(items.size()));
                beginContinuousEdit(changed);
                if (changed && sel >= 0) { tgt()["sunRaysTexture"] = sel == 0 ? std::string() : coronaImages[static_cast<size_t>(sel) - 1]; }
                trackContinuousEdit(changed);
            }
            InspectorHelp("Draw the corona from a painted image (grayscale on black, for additive "
                          "use; any textures/*corona* or *glare* file) or procedurally. The image is "
                          "one texture fetch a pixel; the procedural one has the knobs below.");
            const bool coronaFromImage = !tgt().value("sunRaysTexture", std::string()).empty();
            if (coronaFromImage)
            {
                dragF("Corona Size", "sunRaysTextureSizeDeg", 15.0f, 0.1f, 0.5f, 90.0f, "%.1f deg");
                InspectorHelp("The image's half-width on the sky, in degrees: its core should sit on "
                              "the sun disc.");
                dragF("Corona Rotation", "sunRaysRotationDeg", 0.0f, 0.5f, -180.0f, 180.0f, "%.1f deg");
            }
            else
            {
            {
                int bundles = static_cast<int>(tgt().value("sunRaysBundles", 12u));
                const bool changed = ImGui::SliderInt("Bundles", &bundles, 1, 64);
                beginContinuousEdit(changed);
                if (changed) { tgt()["sunRaysBundles"] = static_cast<std::uint32_t>(bundles < 1 ? 1 : bundles); }
                trackContinuousEdit(changed);
            }
            InspectorHelp("How many bright wedges around the circle, each at a random angle and "
                          "brightness; the gaps between them stay dim. A bright bundle also reaches "
                          "further.");
            dragF("Bundle Width", "sunRaysBundleWidthDeg", 5.0f, 0.05f, 0.1f, 90.0f, "%.1f deg");
            InspectorHelp("A bundle's width as an ANGLE around the sun (full width at half maximum), "
                          "so it opens out with distance like a wedge. At most about the spacing "
                          "between bundles (360 / Bundles): wider than that they are one glow.");
            dragF("Bundle Strength", "sunRaysBundleStrength", 0.5f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("How much the bundles rule. 0 = needles evenly round the circle and all "
                          "alike; 1 = every needle gathered into a bundle, dark between them. It "
                          "brightens and gathers together, so the corona's light stays the same "
                          "(measured within 20 % from 0 to 1). On a small corona a high strength "
                          "packs a bundle's needles into one beam.");
            {
                int needles = static_cast<int>(tgt().value("sunRaysCount", 100u));
                const bool changed = ImGui::SliderInt("Needles", &needles, 16, 1000);
                beginContinuousEdit(changed);
                if (changed) { tgt()["sunRaysCount"] = static_cast<std::uint32_t>(needles < 16 ? 16 : needles); }
                trackContinuousEdit(changed);
            }
            InspectorHelp("How many needles around the circle, lit by the bundles they cross; each has "
                          "its own brightness and length, and one in ten reaches about twice as far.\n\n"
                          "Only so many fit side by side: at a distance d from the sun the circle "
                          "holds 2 pi d / width of them. Closer in than that they merge into glow "
                          "-- so a higher count shows as more LONG needles, with the glow reaching "
                          "further out, not as more needles at the core.");
            dragF("Needle Width", "sunRaysWidthDeg", 0.15f, 0.001f, 0.005f, 2.0f, "%.3f deg");
            InspectorHelp("A needle's thickness on the sky (full width at half maximum), the same "
                          "all along it. 0.045 deg is about a pixel at 1440p with a 90-degree field "
                          "of view; a needle is never drawn thinner than a pixel -- below that it "
                          "gets fainter instead. Wider needles also merge into glow sooner.");
            dragF("Corona Length", "sunRaysLengthDeg", 2.5f, 0.05f, 0.1f, 30.0f, "%.2f deg");
            InspectorHelp("Where a needle has fallen to half, in degrees on the sky. The fall-off is a "
                          "power law: a dense core with a long faint tail.");
            dragF("Halo", "sunRaysHalo", 0.08f, 0.005f, 0.0f, 1.0f, "%.3f");
            InspectorHelp("The lenticular halo: a faint rainbow ring, red outside. 0 = none.");
            dragF("Halo Radius", "sunRaysHaloRadiusDeg", 3.5f, 0.05f, 0.1f, 30.0f, "%.2f deg");
            dragF("Star Spikes", "sunRaysSpikes", 0.0f, 0.01f, 0.0f, 3.0f, "%.2f");
            InspectorHelp("An extra REGULAR star on top (eyelashes, an aperture's spikes). 0 = none.");
            {
                int spikes = static_cast<int>(tgt().value("sunRaysSpikeCount", 6u));
                const bool changed = ImGui::SliderInt("Star Spike Count", &spikes, 2, 32);
                beginContinuousEdit(changed);
                if (changed) { tgt()["sunRaysSpikeCount"] = static_cast<std::uint32_t>(spikes < 2 ? 2 : spikes); }
                trackContinuousEdit(changed);
            }
            dragF("Star Spike Width", "sunRaysSpikeWidthDeg", 0.1f, 0.001f, 0.005f, 2.0f, "%.3f deg");
            InspectorHelp("A star spike's thickness on the sky, as Needle Width.");
            dragF("Star Spike Length", "sunRaysSpikeLengthDeg", 10.0f, 0.05f, 0.1f, 60.0f, "%.1f deg");
            InspectorHelp("Where a star spike has fallen to half, in degrees on the sky; a tenth at "
                          "three lengths, gone by three and a half.");
            dragF("Corona Rotation", "sunRaysRotationDeg", 0.0f, 0.5f, -180.0f, 180.0f, "%.1f deg");
            InspectorHelp("Turns the whole pattern on screen.");
            }

            ImGui::TextDisabled("Runs after the upscaler on the image the tone curve reads,");
            ImGui::TextDisabled("so the pyramid is sized off the DISPLAY resolution and does");
            ImGui::TextDisabled("not change shape with the DLSS quality mode.");
            ImGui::TextDisabled("Fix exposure when comparing: bloom moves average luminance");
            ImGui::TextDisabled("and auto-exposure will chase it.");
            ImGui::TextDisabled("Streak and ghosts run with EITHER bloom method: they read the");
            ImGui::TextDisabled("HDR image and add into the same target both methods write.");
        };

        if (env.type == "pointLight")
        {
            colorEdit();
            dragF("Luminous Flux (lm)", "luminousFluxLm", 1000.0f, 1.0f, 0.0f, 200000.0f,
                  "%.0f", ImGuiSliderFlags_Logarithmic);
            dragF("Volumetric Intensity", "volumetricIntensity", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
            InspectorHelp("This light's weight in the volumetric fog (UE VolumetricScatteringIntensity): 0 = lights "
                          "surfaces only, no cone / halo in the air. Multiplied by the fog's Local Light Intensity.");
            InspectorHelp(
                "Luminous flux, in LUMENS -- the number on a light bulb's box. 800 = a 60 W "
                "incandescent, 1600 = 100 W, 3000-5000 a shop fitting, 10000-50000 a "
                "floodlight.\n\n"
                "It is converted to intensity as flux/(4*pi) and falls off as inverse-square, so "
                "what reaches a surface is LUX -- the same unit the sun's illuminance is in. That "
                "is what lets a lamp and daylight be compared instead of guessed at.\n\n"
                "The cone angle does NOT change a spot light's brightness: the same lumens give "
                "the same peak intensity as a point light would. Physically a reflector does "
                "concentrate flux, but tying brightness to the cone makes the two controls fight.\n\n"
                "IT REPLACED `intensity`, which had no unit and did not fall off physically -- the "
                "old curve was (1 - d/r)^2, a window, so a light's brightness moved whenever the "
                "radius was dragged. A level authored before this is migrated by matching the old "
                "brightness at HALF the range; nearer than that it is now brighter and further it "
                "falls off faster, which is the correction, not a regression.");
            dragF("Radius", "radius", 1.0f, 0.05f, 0.0f, 1000.0f);
            dragF3("Position", "position", Math::float3(0.0f, 0.0f, 0.0f), 0.05f);
            checkB("Cast Shadows", "shadowsEnabled", false);
            dragF("Shadow Bias", "shadowBias", 0.5f, 0.01f, 0.0f, 10.0f, "%.2f");
            dragF("Shadow Slope Bias", "shadowSlopeBias", 0.5f, 0.01f, 0.0f, 10.0f, "%.2f");
            InspectorHelp("UE's Shadow Bias / Shadow Slope Bias (0.5 each). With the texel bias (the default) a "
                          "surface looks its shadow up moved off itself by this light's own texels: 3 along its "
                          "geometric normal x Shadow Bias / 0.5, and 0.5 toward the light x Slope Bias / 0.5 x the "
                          "slope (1 / N.L, up to 6). Larger = no acne on rough or grazing ground, but the shadow "
                          "lifts off its caster's foot. With shadow.localBiasTexel:0 they are UE's own: the lookup "
                          "moves toward the light by 0.02 x 512/Res x 2 x Shadow Bias of the distance, times "
                          "1 + 3 x Slope Bias x (1 - N.L).");
            dragF("Shadow Sharpen", "shadowSharpen", 0.0f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("UE's Shadow Sharpen: narrows the filtered penumbra -- the 29-tap disc's result is "
                          "stretched about its middle by 7 x this + 1. 0 = UE's soft edge.");
            onAtCombo();

            bool flickerEnabled = tgt().contains("flicker") && tgt()["flicker"].is_object();
            if (ImGui::Checkbox("Flicker", &flickerEnabled))
            {
                nlohmann::json after = props;
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
                    float value = JsonFloat(tgt()["flicker"], key, def);
                    const bool changed = ImGui::DragFloat(label, &value, speed, lo, hi);
                    beginContinuousEdit(changed);
                    if (changed) { tgt()["flicker"][key] = value; }
                    trackContinuousEdit(changed);
                };
                dragFlicker("Amplitude", "amplitude", 0.35f, 0.01f, 0.0f, 1.0f);
                dragFlicker("Frequency (Hz)", "frequencyHz", 7.0f, 0.1f, 0.0f, 60.0f);

                int seed = tgt()["flicker"].value("seed", 3);
                const bool seedChanged = ImGui::DragInt("Seed", &seed, 1.0f, 0, 1000000);
                beginContinuousEdit(seedChanged);
                if (seedChanged) { tgt()["flicker"]["seed"] = std::max(seed, 0); }
                trackContinuousEdit(seedChanged);
            }
        }
        else if (env.type == "spotLight")
        {
            colorEdit();
            dragF("Luminous Flux (lm)", "luminousFluxLm", 1000.0f, 1.0f, 0.0f, 200000.0f,
                  "%.0f", ImGuiSliderFlags_Logarithmic);
            dragF("Volumetric Intensity", "volumetricIntensity", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
            InspectorHelp("This light's weight in the volumetric fog (UE VolumetricScatteringIntensity): 0 = lights "
                          "surfaces only, no cone / halo in the air. Multiplied by the fog's Local Light Intensity.");
            InspectorHelp(
                "Luminous flux, in LUMENS -- the number on a light bulb's box. 800 = a 60 W "
                "incandescent, 1600 = 100 W, 3000-5000 a shop fitting, 10000-50000 a "
                "floodlight.\n\n"
                "It is converted to intensity as flux/(4*pi) and falls off as inverse-square, so "
                "what reaches a surface is LUX -- the same unit the sun's illuminance is in. That "
                "is what lets a lamp and daylight be compared instead of guessed at.\n\n"
                "The cone angle does NOT change a spot light's brightness: the same lumens give "
                "the same peak intensity as a point light would. Physically a reflector does "
                "concentrate flux, but tying brightness to the cone makes the two controls fight.\n\n"
                "IT REPLACED `intensity`, which had no unit and did not fall off physically -- the "
                "old curve was (1 - d/r)^2, a window, so a light's brightness moved whenever the "
                "radius was dragged. A level authored before this is migrated by matching the old "
                "brightness at HALF the range; nearer than that it is now brighter and further it "
                "falls off faster, which is the correction, not a regression.");
            dragF("Range", "range", 10.0f, 0.1f, 0.0f, 10000.0f);
            dragF("Inner Angle (deg)", "innerAngleDeg", 15.0f, 0.2f, 0.0f, 89.0f);
            dragF("Outer Angle (deg)", "outerAngleDeg", 25.0f, 0.2f, 0.0f, 89.0f);
            dragF3("Position", "position", Math::float3(0.0f, 0.0f, 0.0f), 0.05f);
            dragF3("Direction", "direction", Math::float3(0.0f, -1.0f, 0.0f), 0.01f);
            checkB("Cast Shadows", "shadowsEnabled", false);
            onAtCombo();
            dragF("Shadow Bias", "shadowBias", 0.5f, 0.01f, 0.0f, 10.0f, "%.2f");
            dragF("Shadow Slope Bias", "shadowSlopeBias", 0.5f, 0.01f, 0.0f, 10.0f, "%.2f");
            InspectorHelp("UE's Shadow Bias / Shadow Slope Bias (0.5 each). With the texel bias (the default) a "
                          "surface looks its shadow up moved off itself by this light's own texels: 3 along its "
                          "geometric normal x Shadow Bias / 0.5, and 0.5 toward the light x Slope Bias / 0.5 x the "
                          "slope (1 / N.L, up to 6). Larger = no acne, but the shadow lifts off its caster's foot. "
                          "With shadow.localBiasTexel:0 they are UE's own: the map is written pushed away from the "
                          "light by 15.36/Res metres x 2 x Shadow Bias, plus that x 3 x Slope Bias on steep "
                          "surfaces.");
            dragF("Shadow Sharpen", "shadowSharpen", 0.0f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("UE's Shadow Sharpen: narrows the filtered penumbra -- the 6x6 tent's result is stretched "
                          "about its middle by 7 x this + 1. 0 = UE's soft edge.");
            // Plan S8: read only by UE's / the old bias -- the texel bias moves the receiver by texels, so with it on the
            // field is shown but moves nothing, and says so.
            ImGui::BeginDisabled(render::LocalShadowTexelBias());
            dragF("Shadow Normal Bias", "shadowNormalBias", 0.05f, 0.001f, 0.0f, 10.0f, "%.5f");
            ImGui::EndDisabled();
            InspectorHelp("Ours, on top of UE's bias: the receiver is looked up this many metres off its surface, "
                          "along the normal. Only with shadow.localBiasTexel:0 -- the texel bias (the default) "
                          "pushes by this light's texels instead (Shadow Bias above).");
            // Plan S4: absent = automatic, shown as the near the light then uses (render::SpotShadowNear).
            dragF("Shadow Near (m)", "shadowNear", render::SpotShadowNear(0.0f), 0.001f, render::kSpotShadowMinNear,
                  5.0f, "%.3f");
            InspectorHelp("The shadow's near plane: a caster nearer the light than this is not in its map (a lamp's "
                          "own bulb or frame inside it does not shadow the light -- or switch that mesh's Cast Shadow "
                          "off). Unset, it is UE's 1 mm on the atlases' linear depth and 0.1 m under VSM / "
                          "shadow.spotLinearDepth:0, whose perspective depth loses precision far away when it is lower.");
        }
        else if (env.type == "timeOfDay")
        {
            // Time-of-day plan T3: the level's light. The clock and the paths are the object's own
            // properties; the sun's light is the `sun` sub-object (the keys of the old Directional
            // Light, which a converted level carries over unchanged), the moon's the `moon` one.
            {
                const TimeOfDay& live = ctx.scene.GetTimeOfDay();
                const float now = live.Settings().timeHours;
                const int minutes = static_cast<int>(now * 60.0f) % (24 * 60);
                const char* lit = live.LitBy() == TimeOfDay::Body::Sun ? "the sun"
                                : live.LitBy() == TimeOfDay::Body::Moon ? "the moon" : "nobody (both down)";
                ImGui::TextDisabled("Now %02d:%02d - sun %.1f deg, moon %.1f deg (phase angle %.0f) - lit by %s",
                                    minutes / 60, minutes % 60, live.SunElevationDeg(), live.MoonElevationDeg(),
                                    live.MoonPhaseAngleDeg(), lit);
                if (live.HandOverFade() < 1.0f)
                {
                    ImGui::TextDisabled("Hand-over: its light at %.0f %%", 100.0f * live.HandOverFade());
                }
            }

            ImGui::SeparatorText("Clock");
            dragF("Time (h)", "time", 12.0f, 0.01f, 0.0f, 24.0f, "%.2f");
            {
                const float t = JsonFloat(tgt(), "time", 12.0f);
                const int m = static_cast<int>(t * 60.0f + 0.5f) % (24 * 60);
                ImGui::SameLine();
                ImGui::TextDisabled("%02d:%02d", m / 60, m % 60);
            }
            InspectorHelp("Local time, 0-24. The sun is placed on its path for this hour, the moon on "
                          "its own; whichever is up lights the scene (the sun while any of its disc is "
                          "above the horizon). --time=HH:MM on the command line, --set=tod.time:<h>.");
            checkB("Animate", "animate", false);
            InspectorHelp("Run the clock. Off, the level is one instant -- what is saved is what you see. "
                          "On, edits that do not touch the time keep the running clock where it is.");
            const bool real = tgt().value("realPositions", false);
            if (!real)
            {
                dragF("Day Length (min)", "dayLengthMin", 30.0f, 0.1f, 0.01f, 10000.0f, "%.1f");
                dragF("Night Length (min)", "nightLengthMin", 15.0f, 0.1f, 0.01f, 10000.0f, "%.1f");
                InspectorHelp("Real minutes from dawn to dusk and from dusk to dawn while animating "
                              "(Ultra Dynamic Sky's split: 30 and 15).");
            }
            else
            {
                dragF("Time Scale", "timeScale", 60.0f, 1.0f, 0.0f, 1.0e6f, "%.0f",
                      ImGuiSliderFlags_Logarithmic);
                InspectorHelp("Game seconds per real second while animating: 1 = real time, 60 = an hour "
                              "a minute, 1440 = a day a minute.");
            }

            ImGui::SeparatorText("Where the sun and the moon are");
            checkB("Real Positions", "realPositions", false);
            InspectorHelp("OFF (Simple): the sun rides a tilted circle from Dawn to Dusk, the moon its own "
                          "circle placed by the phase -- art-directable, and how an old level's authored "
                          "sun is reproduced exactly. ON (Real): date, latitude and longitude, the sun from "
                          "NOAA's calculator (as UE's SunPosition), the moon from Schlyter's elements with "
                          "the parallax and its real phase.");
            if (!real)
            {
                dragF("Dawn (h)", "dawn", 6.0f, 0.01f, 0.0f, 24.0f, "%.2f");
                dragF("Dusk (h)", "dusk", 18.0f, 0.01f, 0.0f, 24.0f, "%.2f");
                dragF("Path Yaw (deg)", "pathYawDeg", 180.0f, 0.5f, -360.0f, 360.0f, "%.1f");
                InspectorHelp("The azimuth the path leans toward: the noon sun stands there (180 = south). "
                              "It rises 90 degrees to the left of it and sets 90 degrees to the right.");
                dragF("Path Tilt (deg)", "pathTiltDeg", 30.0f, 0.1f, -89.0f, 89.0f, "%.1f");
                InspectorHelp("The path's tilt from the zenith: the noon sun stands 90 - tilt high.");
                dragF("Height Offset", "heightOffset", 0.0f, 0.005f, -1.0f, 1.0f, "%.3f");
                dragF("Extend Twilight", "extendTwilight", 0.0f, 0.01f, 0.0f, 1.0f, "%.2f");
                InspectorHelp("Flattens the path near the horizon, so dawn and dusk last longer.");
            }
            else
            {
                int date[3] = { 2026, 6, 21 };
                if (const auto it = tgt().find("date"); it != tgt().end() && it->is_array() && it->size() >= 3)
                {
                    for (int i = 0; i < 3; ++i) { if ((*it)[i].is_number()) { date[i] = static_cast<int>(std::lround((*it)[i].get<double>())); } }
                }
                const bool changed = ImGui::InputInt3("Date (Y M D)", date);
                beginContinuousEdit(changed);
                if (changed)
                {
                    tgt()["date"] = { std::clamp(date[0], 1, 9999), std::clamp(date[1], 1, 12), std::clamp(date[2], 1, 31) };
                }
                trackContinuousEdit(changed);
                dragF("Latitude (deg)", "latitude", -16.5f, 0.05f, -90.0f, 90.0f, "%.2f");
                dragF("Longitude (deg, +E)", "longitude", -151.75f, 0.05f, -180.0f, 180.0f, "%.2f");
                dragF("UTC Offset (h)", "utcOffset", -10.0f, 0.25f, -14.0f, 14.0f, "%.2f");
            }
            dragF("North Yaw (deg)", "northYawDeg", 0.0f, 0.5f, -360.0f, 360.0f, "%.1f");
            InspectorHelp("Which world direction is north, turned clockwise seen from above; 0 = +Z.");

            // Each body's rows in their own ImGui ID scope: the sun and the moon share labels ("Light Source
            // Angle (deg)", "Temperature (K)") and ImGui otherwise treats the two as one widget.
            groupKey = "sun";
            ImGui::PushID("sun");
            ImGui::SeparatorText("Sun");
            colorEdit();
            checkB("Use Colour Temperature", "useSunTemperature", false);
            if (tgt().value("useSunTemperature", false))
            {
                dragF("Temperature (K)", "sunTemperatureK", 6500.0f, 25.0f, 1000.0f, 15000.0f, "%.0f");
            }
            InspectorHelp(
                "Tints the sun by a black-body temperature, multiplying the colour above. OFF by "
                "default, so the authored colour is used as-is.\n\n"
                "1700 candle, 2700 tungsten, 4000 cool white, 5500 midday sun, 6500 D65 (neutral "
                "here), 7500+ overcast/shade blue.\n\n"
                "The locus is Unreal's, minus its Stefan-Boltzmann brightness term, and the result "
                "is renormalised to unit luminance -- so this changes HUE ONLY.\n\n"
                "Valid 1000-15000K; outside that the fit bends back on itself, so it is clamped.");
            dragF("Sun Illuminance (lux)", "sunIlluminanceLux", 120000.0f, 1.0f, 0.0f, 200000.0f,
                  "%.0f", ImGuiSliderFlags_Logarithmic);
            InspectorHelp(
                "How bright the sun is OUTSIDE the atmosphere, in LUX, perpendicular to the beam: the "
                "atmosphere and the cloud deck take their share on the way down, so the same number "
                "gives a full midday and a red, dim sunset. ~127000 is the real solar constant; "
                "100000-120000 is the usual range.\n\n"
                "Physical values need the photographic camera ON (Post Process > Camera Exposure).");
            dragF("Ambient", "ambient", 0.05f, 0.005f, 0.0f, 10.0f);
            InspectorHelp("The flat fallback fill as a fraction of the light's colour -- only on levels whose "
                          "sky has no irradiance derivatives. Shared by the moon.");
            dragF("Sky Fill Intensity", "skyFillIntensity", 1.0f, 0.01f, 0.0f, 4.0f, "%.3f");
            InspectorHelp(
                "How much of the SKY'S OWN measured irradiance reaches diffuse surfaces. 1 = the "
                "irradiance cube at face value, the physical answer. Shared by the moon, so the fill "
                "does not jump when the light changes hands.");
            {
                const Math::float3 g = JsonFloat3(tgt(), "groundAlbedo", Math::float3(0.25f, 0.25f, 0.25f));
                float gv[3] = { g.x, g.y, g.z };
                const bool changed = ImGui::ColorEdit3("Ground Bounce Albedo", gv);
                beginContinuousEdit(changed);
                if (changed) { tgt()["groundAlbedo"] = { gv[0], gv[1], gv[2] }; }
                trackContinuousEdit(changed);
            }
            InspectorHelp(
                "The DIFFUSE REFLECTANCE OF THE GROUND -- the light that comes back up off the floor "
                "and fills everything facing downward. A reflectance, not a light: it follows the sun "
                "(and the moon) and cannot brighten a night. Dry sand 0.4, grass 0.2, water 0.06.");

            ImGui::SeparatorText("Sun: Light Shafts");
            checkB("Light Shafts", "lightShaftsEnabled", true);
            InspectorHelp(
                "Unreal's Light Shaft Bloom on the sun (plan A7): the bright sky near the sun is radially "
                "blurred towards its screen position and added into the scene -- the god rays through the "
                "crowns. Screen-space: runs only while the sun is in front of the camera. About 0.1 ms.");
            dragF("Bloom Scale", "lightShaftBloomScale", 0.3f, 0.01f, 0.0f, 10.0f, "%.2f");
            InspectorHelp("UE BloomScale: the strength of the shafts (Ultra Dynamic Sky 0.3, UE 0.2; ~1 for rays "
                          "you cannot miss). 0 switches them off.");
            dragF("Bloom Threshold", "lightShaftBloomThreshold", 0.75f, 0.01f, 0.0f, 50.0f, "%.2f");
            InspectorHelp("UE BloomThreshold: only pixels brighter than this seed the shafts, on the pre-exposed "
                          "scale (1 = display white). UDS 0.75: the disc and the bright sky round it.");
            dragF("Bloom Max Brightness", "lightShaftBloomMaxBrightness", 0.35f, 0.01f, 0.0f, 1000.0f, "%.2f");
            InspectorHelp("UE BloomMaxBrightness: caps one pixel's part. UDS 0.35 -- the cap makes their look: "
                          "the sky round the sun seeds wide but flat, so the rays run long across the scene and "
                          "the disc does not burst into a star. UE's 100 = a starburst and short rays.");
            {
                const Math::float3 tint = JsonFloat3(tgt(), "lightShaftBloomTint", Math::float3(1.0f, 1.0f, 1.0f));
                float tv[3] = { tint.x, tint.y, tint.z };
                const bool changed = ImGui::ColorEdit3("Bloom Tint", tv);
                beginContinuousEdit(changed);
                if (changed) { tgt()["lightShaftBloomTint"] = { tv[0], tv[1], tv[2] }; }
                trackContinuousEdit(changed);
            }
            InspectorHelp("UE BloomTint: multiplies the shaft colour. White = the sky's own colour.");
            dragF("Occlusion Depth Range (m)", "lightShaftOcclusionDepthRange", 1000.0f, 10.0f, 1.0f, 20000.0f, "%.0f");
            InspectorHelp("UE LightShaftOcclusionDepthRange (their 1000 m): only pixels in the FAR half of this "
                          "range seed the shafts, so nearby lit surfaces do not streak.");
            dragF("Smoothing (s)", "lightShaftSmoothing", 0.15f, 0.005f, 0.0f, 2.0f, "%.3f");
            InspectorHelp("The time constant of a temporal blend of the shafts' mask: 0 = every frame as it is "
                          "(the rays tremble with every leaf); higher = more inertia.");
            dragF("Glare Seed", "lightShaftGlareSeed", 1.0f, 0.01f, 0.0f, 4.0f, "%.2f");
            InspectorHelp("How much of the Sun Glare (Post Process > Bloom) joins the shafts' seed. Unreal seeds on "
                          "the scene over the threshold, and Ultra Dynamic Sky's bright sky lets a wide disc of it "
                          "through -- its rays run long through crowns; our darker sky lets only the disc through. "
                          "The glare is that bright sky as the picture shows it, so with it the gaps in the leaves "
                          "seed too. 0 = Unreal's seed, 1 = the glare as drawn.");
            dragF("Shaft Length", "lightShaftLength", 1.0f, 0.01f, 0.05f, 2.0f, "%.2f");
            InspectorHelp("1 = Unreal's rays. Over 1 = LONGER: Unreal's fade as 1/distance from the sun, and this "
                          "slows the fade -- 1.5 as 1/sqrt(distance), 2 not at all (Bloom Scale may then want "
                          "lowering). Under 1 = shorter: the blur stops short of the sun, at 0.5 a ray ends about "
                          "3x its gap's distance from it, at 0.3 about 1.7x. The moon's shafts use it too.");
            dragF("Light Source Angle (deg)", "lightSourceAngle", 0.5357f, 0.005f, 0.0f, 10.0f, "%.4f");
            InspectorHelp("UE LightSourceAngle: the sun's angular DIAMETER, 0.5357 for the real sun. It draws "
                          "the disc and floors the width of the specular lobe; the sun sets when the whole "
                          "disc is under the horizon.");
            dragF("Light Source Soft Angle (deg)", "lightSourceSoftAngle", 0.0f, 0.005f, 0.0f, 10.0f, "%.3f");
            InspectorHelp("UE LightSourceSoftAngle (0 by default; Ultra Dynamic Sky's sun 0.5): softens the edge "
                          "of the sun's glint on glossy surfaces -- the roughness is widened by it before the disc "
                          "acts -- with the glint's energy kept. Specular only; the disc and the shadows keep "
                          "Light Source Angle.");

            ImGui::PopID();
            groupKey = "moon";
            ImGui::PushID("moon");
            ImGui::SeparatorText("Moon");
            checkB("Moon", "enabled", true);
            InspectorHelp("Off, nobody lights the night: the scene is lit by the sky alone.");
            dragF("Full Moon Illuminance (lux)", "illuminanceLux", 0.32f, 0.001f, 0.0f, 1000.0f, "%.3f",
                  ImGuiSliderFlags_Logarithmic);
            InspectorHelp("A FULL moon outside the atmosphere at its mean distance: the sun's 127 klx times the "
                          "full moon's 14 magnitudes fainter = 0.32 lux (~0.27 on the ground with the moon "
                          "overhead). The phase law scales it down (a quarter moon is ~0.1 of full); Real "
                          "positions add the distance.");
            dragF("New Moon Brightness", "newMoonBrightness", 0.0f, 0.001f, 0.0f, 1.0f, "%.3f");
            InspectorHelp("The least the moon's LIGHT falls to with the phase, as a share of the full moon's. 0 = "
                          "physics: a new moon sheds nothing and the night is the airglow and the stars. Ultra "
                          "Dynamic Sky's New Moon Light Brightness is 0.015 -- its moonless night stays a dark blue "
                          "sky. The drawn disc follows the bare phase law either way.");
            // No Temperature row (removed 2026-10-15): every hue it set has its own row below
            // (TimeOfDaySettings, "NO MOON TEMPERATURE").
            {
                const TimeOfDaySettings defaults{};
                const Math::float3 dc = JsonFloat3(tgt(), "discColor",
                    Math::float3(defaults.moonDiscColor[0], defaults.moonDiscColor[1], defaults.moonDiscColor[2]));
                float dv[3] = { dc.x, dc.y, dc.z };
                const bool changed = ImGui::ColorEdit3("Disc Colour", dv);
                beginContinuousEdit(changed);
                if (changed) { tgt()["discColor"] = { dv[0], dv[1], dv[2] }; }
                trackContinuousEdit(changed);
            }
            InspectorHelp("The colour the drawn DISC is seen in (its light shafts grow out of it). Only "
                          "its hue counts (unit luminance: the disc stays as bright as its light). White, as Ultra "
                          "Dynamic Sky draws its moon; a touch of blue reads silver against the night.");
            dragF("Glow", "glow", 0.02f, 0.001f, 0.0f, 2.0f, "%.3f");
            InspectorHelp("A soft glow round the disc, as Ultra Dynamic Sky draws one (its Moon Glow): the peak, as a "
                          "fraction of the full moon's mean brightness, fading to nothing at the radius below; it "
                          "follows the phase. The real moon's is the aureole of the haze in front of it. The light "
                          "shafts seed on it -- without it they drew the whole halo themselves. 0 = none.");
            dragF("Glow Radius (deg)", "glowRadiusDeg", 11.6f, 0.1f, 0.1f, 90.0f, "%.1f");
            InspectorHelp("How far the glow reaches from the moon's centre (UDS 11.6).");
            {
                const TimeOfDaySettings defaults{};
                const Math::float3 lc = JsonFloat3(tgt(), "lightColor",
                    Math::float3(defaults.moonLightColor[0], defaults.moonLightColor[1], defaults.moonLightColor[2]));
                float lv[3] = { lc.x, lc.y, lc.z };
                const bool changed = ImGui::ColorEdit3("Light Colour", lv);
                beginContinuousEdit(changed);
                if (changed) { tgt()["lightColor"] = { lv[0], lv[1], lv[2] }; }
                trackContinuousEdit(changed);
            }
            InspectorHelp("The colour the moon LIGHTS the scene AND the sky in -- the cold light of a moonlit night, "
                          "the hue of the moonlit sky (with Twilight > Night Rayleigh Tint, as Ultra Dynamic Sky's "
                          "moon lights its atmosphere). Only its hue counts (used at unit luminance; the lux stay "
                          "physical). The default is UDS's Moon Light Colour. White = the physical moonlight, "
                          "which reads warm on a beige beach.");
            dragF("Light Source Angle (deg)", "lightSourceAngle", 0.52f, 0.005f, 0.0f, 20.0f, "%.3f");
            InspectorHelp("The moon's angular DIAMETER: the drawn disc and the light's size (the specular "
                          "floor, the soft shadow). 0.52 is the real moon's mean.");
            dragF("Light Source Soft Angle (deg)", "lightSourceSoftAngle", 0.0f, 0.005f, 0.0f, 10.0f, "%.3f");
            InspectorHelp("UE LightSourceSoftAngle of the moon light (Ultra Dynamic Sky's moon 0.285): softens "
                          "the edge of its glint on glossy surfaces, the energy kept. 0 = Unreal's default.");
            dragF("Disc Adaptation", "discAdaptation", 1.0f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("How the drawn disc is seen. 1 = by the eye, which adapts to it: brought into the "
                          "display's range, its maria and relief visible, its edge and terminator smooth. 0 = "
                          "by a camera under the night's exposure: ten thousand times the white, clipped flat "
                          "and hard-edged. Only the disc -- the light it sheds is physical either way.");
            dragF("Shaft Bloom Scale", "lightShaftBloomScale", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
            InspectorHelp("The moon's light shafts, which take over once the sun's are gone and the moon lights the "
                          "ground: their strength (Unreal BloomScale on the moon's light). 0 = none. The length, "
                          "smoothing and depth range are the Sun's; Sun > Light Shafts switches both.");
            dragF("Shaft Bloom Threshold", "lightShaftBloomThreshold", 0.008f, 0.001f, 0.0f, 50.0f, "%.3f");
            InspectorHelp("Only pixels brighter than this seed them, on the exposed scale: just above the moonlit "
                          "sky, so the disc AND its glow seed (as Ultra Dynamic Sky's 0.15 does on its ~10x "
                          "brighter night). A brighter night exposure may want it higher -- or the whole sky seeds.");
            dragF("Shaft Max Brightness", "lightShaftBloomMaxBrightness", 0.03f, 0.001f, 0.0f, 1000.0f, "%.3f");
            InspectorHelp("Caps one pixel's part -- at the glow's level next to the disc, as UDS's 0.15 is at its "
                          "glow's: the seed is wide and flat, a soft halo and long soft rays. High (100) = the "
                          "disc alone seeds and the blur draws its copies.");
            {
                const TimeOfDaySettings defaults{};
                const Math::float3 st = JsonFloat3(tgt(), "lightShaftBloomTint",
                    Math::float3(defaults.moonLightShaftTint[0], defaults.moonLightShaftTint[1], defaults.moonLightShaftTint[2]));
                float sv[3] = { st.x, st.y, st.z };
                const bool changed = ImGui::ColorEdit3("Shaft Tint", sv);
                beginContinuousEdit(changed);
                if (changed) { tgt()["lightShaftBloomTint"] = { sv[0], sv[1], sv[2] }; }
                trackContinuousEdit(changed);
            }
            InspectorHelp("The moon's shafts are its light scattered by the air, so they come in the Light "
                          "Colour above; this multiplies that, in place of the sun's Bloom Tint (Unreal's "
                          "BloomTint on the moon's light; Ultra Dynamic Sky's Moon Light Shaft Bloom Tint).");
            dragF("Glint Phase Falloff", "glintPhaseFalloff", 0.5f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("The moon's glints on the water, faded again by its phase: 0 = physics (the path follows "
                          "the moon's light alone -- a crescent's only ~3 stops dimmer on screen, the night's "
                          "exposure rising), 1 = faded once more by the phase law. 0.5: the full moon's path as "
                          "it is, a quarter's 1.7 stops down, a thin crescent's 3.3.");
            dragF("Earthshine", "earthshine", 1.0f, 0.05f, 0.0f, 100.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            InspectorHelp("The ashen light on the moon's dark side -- sunlight reflected by the Earth. 1 = the "
                          "physical estimate (about 1/8000 of the full disc at new moon, fading as the Earth's "
                          "own phase wanes toward full moon). Seen on a thin crescent.");
            if (!real)
            {
                dragF("Phase", "phase", 0.5f, 0.005f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("0 new, 0.25 first quarter, 0.5 full, 0.75 last quarter. It changes the moon's FACE "
                              "and its light, not its place: the moon rides opposite the sun (turned by Orbit "
                              "Offset) at any phase, as Ultra Dynamic Sky's does. Real positions take both from "
                              "the date.");
                checkB("Phase Advances", "phaseAuto", true);
                InspectorHelp("While the clock runs, the phase advances one cycle per 29.53 days.");
                dragF("Moon Path Yaw (deg)", "pathYawDeg", 180.0f, 0.5f, -360.0f, 360.0f, "%.1f");
                dragF("Moon Path Tilt (deg)", "pathTiltDeg", 35.0f, 0.1f, -89.0f, 89.0f, "%.1f");
                dragF("Orbit Offset (deg)", "orbitOffsetDeg", 0.0f, 0.5f, -360.0f, 360.0f, "%.1f");
                InspectorHelp("Turns the moon along its circle away from opposite the sun.");
            }

            ImGui::PopID();
            // Time-of-day plan N2: the night sky's own layers.
            groupKey = "nightSky";
            ImGui::PushID("nightSky");
            ImGui::SeparatorText("Night sky");
            {
                const TimeOfDaySettings defaults{};
                checkB("Night Sky", "enabled", defaults.nightSkyEnabled);
                InspectorHelp("What a sky has without sun or moon -- the airglow, the stars, the Milky Way -- "
                              "procedural, physical by default (a moonless sky far from any town). Drawn in "
                              "the sky, in its picture the water and the probes see, and in the clouds' light "
                              "from below; not while the sun stands above 10 degrees, where none of it can show.");
                dragF("Airglow (cd/m2)", "airglowZenith", defaults.airglowZenith, 1.0e-6f, 0.0f, 1.0f, "%.6f",
                      ImGuiSliderFlags_Logarithmic);
                InspectorHelp("The night sky's own glow at the zenith: airglow and zodiacal light, ~1.2e-4 cd/m2 "
                              "at a dark site. Brighter toward the horizon (a layer 90 km up seen edge-on, x6) "
                              "until the air takes it back -- which is the warm band low over the sea.");
                const Math::float3 ag = JsonFloat3(tgt(), "airglowColor",
                    Math::float3(defaults.airglowColor[0], defaults.airglowColor[1], defaults.airglowColor[2]));
                float av[3] = { ag.x, ag.y, ag.z };
                const bool changed = ImGui::ColorEdit3("Airglow Colour", av);
                beginContinuousEdit(changed);
                if (changed) { tgt()["airglowColor"] = { av[0], av[1], av[2] }; }
                trackContinuousEdit(changed);
                InspectorHelp("Its hue only (used at unit luminance); the oxygen line makes it faintly green.");
                dragF("Stars", "stars", defaults.starsBrightness, 0.01f, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
                InspectorHelp("A multiplier on the stars: 1 = the real sky's counts and brightness (~4500 brighter "
                              "than magnitude 6, down to ~9.7). Each a point of exactly its light, so they do not "
                              "grow with the resolution or flicker under DLSS.");
                dragF("Milky Way", "milkyWay", defaults.milkyWayBrightness, 0.01f, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
                InspectorHelp("A multiplier on the galaxy's band (1 = physical: about the airglow's brightness, "
                              "which is why it shows only far from towns and to an adapted eye).");
                dragF("Twinkle", "twinkle", defaults.starTwinkle, 0.01f, 0.0f, 1.0f, "%.2f");
                InspectorHelp("Scintillation, 0..1, stronger toward the horizon. On the scene's wind clock: "
                              "--wind-freeze stills it.");
                if (!real)
                {
                    dragF("Sidereal Time at Midnight (h)", "siderealAtMidnight", defaults.siderealAtMidnightHours,
                          0.05f, -48.0f, 48.0f, "%.2f");
                    InspectorHelp("Which stars stand overhead at midnight in Simple mode (Real takes it from the "
                                  "date and longitude). 18 = a southern winter's night, the galaxy's core high.");
                    // The section's own latitude (Real mode's field), the only thing Simple mode reads it for.
                    groupKey.clear();
                    dragF("Latitude (deg)", "latitude", -16.5f, 0.05f, -90.0f, 90.0f, "%.2f");
                    InspectorHelp("How high the celestial pole stands -- the stars turn about it. Real positions "
                                  "use it for the sun and the moon as well.");
                    groupKey = "nightSky";
                }
            }
            ImGui::PopID();

            // The twilight as Ultra Dynamic Sky draws it.
            groupKey = "twilight";
            ImGui::PushID("twilight");
            ImGui::SeparatorText("Twilight");
            {
                const TimeOfDaySettings defaults{};
                dragF("Sun Leaves Sky From (deg)", "sunSkyStartDeg", defaults.sunSkyStartDeg, 0.05f, -90.0f, 90.0f, "%.2f");
                dragF("Sun Fully In Sky At (deg)", "sunSkyFullDeg", defaults.sunSkyFullDeg, 0.05f, -90.0f, 90.0f, "%.2f");
                InspectorHelp("The sun's light INTO THE SKY -- the glow of the twilight, the haze, the ambient the "
                              "sky sheds, the clouds -- is 0 with the sun at or below the first elevation and full "
                              "from the second, linear between. Ultra Dynamic Sky's: -9.03 and -6.49. Physics keeps "
                              "a glow down to -18, which over a physical moonlit sky draws a bright band low over "
                              "a black sky; UDS's moon is far brighter and its sun is gone. Both -90 = physics. The "
                              "ground's light is not this: the sun lights it only while its disc is up.");
                const Math::float3 rt = JsonFloat3(tgt(), "rayleighNightTint",
                    Math::float3(defaults.rayleighNightTint[0], defaults.rayleighNightTint[1], defaults.rayleighNightTint[2]));
                float rv[3] = { rt.x, rt.y, rt.z };
                const bool changed = ImGui::DragFloat3("Night Rayleigh Tint", rv, 0.005f, 0.0f, 10.0f, "%.3f");
                beginContinuousEdit(changed);
                if (changed) { tgt()["rayleighNightTint"] = { rv[0], rv[1], rv[2] }; }
                trackContinuousEdit(changed);
                InspectorHelp("The atmosphere's Rayleigh scattering, per channel, times this while the sun is down: "
                              "UDS's night Rayleigh colour against its day one (1.43, 0.85, 0.61) -- less blue "
                              "scattered, a paler moonlit sky and a less purple twilight. The sun's own reddening "
                              "follows, as in UDS. (1, 1, 1) = none. A multiplier, not a colour: it may exceed 1.");
                dragF("Day Rayleigh From (deg)", "rayleighDayStartDeg", defaults.rayleighDayStartDeg, 0.05f, -90.0f, 90.0f, "%.2f");
                dragF("Day Rayleigh Full At (deg)", "rayleighDayFullDeg", defaults.rayleighDayFullDeg, 0.05f, -90.0f, 90.0f, "%.2f");
                InspectorHelp("Where the night tint gives way to the authored Rayleigh: from the first sun elevation, "
                              "gone by the second, easing out (UDS's blend: -2.07 to 3.68).");
                const auto tint3 = [&](const char* label, const char* key, const float* fallback) {
                    const Math::float3 v = JsonFloat3(tgt(), key, Math::float3(fallback[0], fallback[1], fallback[2]));
                    float ov[3] = { v.x, v.y, v.z };
                    const bool edited = ImGui::DragFloat3(label, ov, 0.01f, 0.0f, 20.0f, "%.2f");
                    beginContinuousEdit(edited);
                    if (edited) { tgt()[key] = { ov[0], ov[1], ov[2] }; }
                    trackContinuousEdit(edited);
                };
                tint3("Night Ozone Tint", "ozoneNightTint", defaults.ozoneNightTint);
                tint3("Sunrise Ozone Tint", "ozoneSunriseTint", defaults.ozoneSunriseTint);
                dragF("Ozone Blend (deg)", "ozoneBlendDeg", defaults.ozoneBlendDeg, 0.01f, 0.001f, 90.0f, "%.2f");
                InspectorHelp("The atmosphere's ozone, per channel, times the night tint with the sun at or below minus "
                              "this many degrees, the sunrise tint at 0, the level's own ozone from plus this up. "
                              "Earth's ozone absorbs green most, so a twilight sky -- lit by light that crossed the "
                              "ozone layer edge-on -- comes out magenta; UDS's absorbs red as hard and its twilight "
                              "is blue. UDS's values: x(6.15, 1.81, 1.43) at night, x(4.62, 1.56, 4.03) at sunrise, "
                              "1.95 degrees. (1, 1, 1) twice = none.");
                dragF("Light Hand-over (deg)", "lightHandOverDeg", defaults.lightHandOverDeg, 0.05f, 0.0f, 20.0f, "%.2f");
                InspectorHelp("When the light passes between the moon and the sun (the sun's disc wholly under the "
                              "horizon), the body that has it fades out over this many degrees of the sun's height "
                              "before, and the one taking it fades in over as many after -- one light at a time, "
                              "no jump of the glint on the water. The sun's disc and the sky are not faded. 0 = the "
                              "hard switch.");
            }
            ImGui::PopID();

            // Plan L: when the lamps marked "On At: night" (lights, glowing meshes) switch, and how they come on.
            groupKey = "lamps";
            ImGui::PushID("lamps");
            ImGui::SeparatorText("Lamps");
            {
                const TimeOfDaySettings defaults{};
                dragF("Switch At Sun (deg)", "sunElevationDeg", defaults.lampSunElevationDeg, 0.05f, -90.0f, 90.0f, "%.2f");
                InspectorHelp("The lamps marked On At: night (lights, and meshes that glow) come on when the sun's centre "
                              "goes under this elevation and go out when it comes back over it. 0 = sunset and "
                              "sunrise, the moment Ultra Dynamic Sky fires its Sunset / Sunrise events; -3 = a little "
                              "into the dusk, as a street's light sensors do.");
                dragF("Random Delay (s)", "randomDelaySec", defaults.lampRandomDelaySec, 0.01f, 0.0f, 600.0f, "%.2f");
                InspectorHelp("Each lamp switches up to this many seconds after the sun crosses -- a smooth noise of where "
                              "it stands, so a street's lamps come on one by one while a light and the glowing bulb "
                              "beside it come on together. UDS's Random Delay: 1. 0 = all at once.");
                dragF("Turn On (s)", "turnOnSec", defaults.lampTurnOnSec, 0.01f, 0.0f, 600.0f, "%.2f");
                dragF("Turn On Flicker", "turnOnFlicker", defaults.lampTurnOnFlicker, 0.01f, 0.0f, 1.0f, "%.2f");
                dragF("Turn On Exponent", "turnOnExponent", defaults.lampTurnOnExponent, 0.01f, 0.01f, 10.0f, "%.2f");
                InspectorHelp("How a lamp comes on: over Turn On seconds, its brightness along t^Exponent, flickering by "
                              "up to Flicker while it warms (UDS's Light Day/Night Toggle: 1.5 s, 0.6, 0.2). It goes "
                              "out at once. A clock set by hand -- the time slider, setTime -- switches every lamp at "
                              "once.");
                ImGui::TextDisabled("Now: %s", ctx.scene.GetNightLamps().Night() ? "night (lamps on)" : "day (lamps off)");
            }
            ImGui::PopID();

            groupKey.clear();
            ImGui::SeparatorText("Night");
            dragF("Night Brightness", "nightBrightness", 1.0f, 0.01f, 0.0f, 100000.0f, "%.2f",
                  ImGuiSliderFlags_Logarithmic);
            InspectorHelp("A manual multiplier on the night's light: the moon's, the night sky's (airglow, "
                          "stars, the Milky Way) and the night lamps' (lights and glows set On At: night), so a "
                          "lamp keeps its place against the moonlight. 1 = physics: the full moon 1/375000 of the sun. Ultra Dynamic "
                          "Sky's moon is 1/33 of its sun -- about 11400 here -- and that is what makes its dawn a "
                          "moonlit blue sky until the sun is nearly up; pair it with an exposure curve that keeps "
                          "the night darker than the day (Camera Exposure > Compensation curve).");
            dragF("Day Sky Brightness", "daySkyBrightness", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
            InspectorHelp("A manual multiplier on the sky BY DAY: the sky, its haze on distant things, its "
                          "reflections and the light it sheds, together (the sky's luminance scale). It comes in "
                          "with the day sky -- Twilight > Day Rayleigh From / Full At -- so the night and the "
                          "moonlit dawn keep their darkness. 1 = as the atmosphere gives it.");
            dragF("Night Exposure (stops)", "nightExposureEv", 0.0f, 0.05f, -10.0f, 10.0f, "%+.2f");
            InspectorHelp("The night darker or lighter: stops added to the camera's exposure at night -- none "
                          "with the sun above -6 degrees, all of it from -12, smooth between. Only the picture: "
                          "the light, the meter, the exposure curve and the night vision stay as they are. "
                          "Negative darkens; 0 = none.");
            ImGui::SeparatorText("Cost");
            dragF("Light Update Threshold (deg)", "lightUpdateThresholdDeg", 0.05f, 0.005f, 0.0f, 5.0f, "%.3f");
            InspectorHelp("While the clock runs, the scene light (and with it every shadow) turns only once "
                          "the body has moved this far; the sky follows exactly. 0.05 degrees is 12 s of a "
                          "real sun.");
        }
        else if (env.type == "camera")
        {
            dragF("H FOV (deg)", "hfovDeg", 90.0f, 0.5f, 1.0f, 179.0f);
            dragF("Z Near", "zNear", 0.01f, 0.001f, 0.0001f, 100.0f);
            dragF("Z Far", "zFar", 10000.0f, 1.0f, 0.1f, 1000000.0f);
        }
        else if (env.type == "cameraExposure")
        {
            drawCameraExposure();
        }
        else if (env.type == "gtao")
        {
            drawGtao();
        }
        else if (env.type == "heightFog")
        {
            drawHeightFog();
        }
        else if (env.type == "bloom")
        {
            drawBloom();
        }
        else if (env.type == "colorPipeline")
        {
            drawColorPipeline();
        }
        else if (env.type == "postProcess")
        {
            // P8B. One object, five collapsing sections. Each section sets `groupKey` so the shared
            // widgets write into `properties[<group>]`, and clears it afterwards -- an escaped
            // groupKey would send the NEXT object's edits into a sub-object that does not exist.
            struct Section { const char* key; const char* label; };
            const Section sections[] = {
                { "cameraExposure", "Camera Exposure" },
                { "colorPipeline",  "Color Pipeline" },
                { "gtao",           "Ambient Occlusion (GTAO)" },
                { "heightFog",      "Exponential Height Fog" },
                { "bloom",          "Bloom" },
            };
            for (const Section& section : sections)
            {
                // Missing sections use defaults without authoring empty groups just by viewing.
                missingGroup = nlohmann::json::object();
                if (ImGui::CollapsingHeader(section.label))
                {
                    ImGui::PushID(section.key);
                    groupKey = section.key;
                    if (std::string_view(section.key) == "cameraExposure") { drawCameraExposure(); }
                    else if (std::string_view(section.key) == "colorPipeline") { drawColorPipeline(); }
                    else if (std::string_view(section.key) == "gtao") { drawGtao(); }
                    else if (std::string_view(section.key) == "heightFog") { drawHeightFog(); }
                    else { drawBloom(); }
                    groupKey.clear();
                    ImGui::PopID();
                }
            }
        }
        else if (env.type == "skyAtmosphere")
        {
            // B6.2. Having this object AT ALL is what makes the level procedural -- the same shape
            // as UE, where a SkyAtmosphere is an actor you add rather than a flag on the sky. Delete
            // it and the level goes back to its cubemap. `enabled` is here so it can be switched off
            // without losing the tuning, exactly as the other look sections have it.
            checkB("Enabled", "enabled", true);
            InspectorHelp(
                "Runs the Hillaire atmosphere -- transmittance, multi-scattering, SkyView and "
                "distant-light LUTs -- from the sun's direction and the parameters below, instead "
                "of reading the level's cubemap.\n\n"
                "The cubemap is still used while this is off, and remains the fallback the fog and "
                "the water read before the procedural capture exists, so switching costs no reload. "
                "A procedural level STILL NEEDS a Skybox texture: the atmosphere is drawn by the Skybox "
                "object, which the level loader creates only when one is named -- without it the sky "
                "renders black.\n\n"
                "--set=sky.mode overrides this for a headless run.");

            dragF("Sky Luminance Scale", "luminanceScale", 2.13f, 0.01f, 0.0f, 10.0f, "%.3f");
            InspectorHelp(
                "UE SkyAndAerialPerspectiveLuminanceFactor: multiplies the sun's illuminance on the way "
                "INTO every atmosphere table, so it brightens the sky, the aerial-perspective haze on "
                "geometry AND the ambient the sky's probes deliver -- all of them together, relative to "
                "the direct sun. It is NOT sky-only (the row below is), and it is never the camera's "
                "exposure. 1 = physics.");
            dragF("Sky-only Luminance", "skyLuminanceFactor", 1.0f, 0.01f, 0.0f, 10.0f, "%.3f");
            InspectorHelp(
                "The PICTURE of the sky only: the pixels of sky you see, multiplied when they are drawn. "
                "Nothing the sky lights moves with it -- not the ambient on the sand, not the reflections, "
                "not the haze on geometry, not the sun disc. (UE's SkyLuminanceFactor also feeds their "
                "sky-light capture; measured here, that made this row and the one above the same knob, "
                "so ours does not.)");

            ImGui::SeparatorText("What the atmosphere feeds");
            checkB("Environment lighting", "environmentLighting", false);
            InspectorHelp("Rebuilds the reflection cube and the sky lighting from this atmosphere "
                          "whenever the sun or the parameters change. OFF leaves both on the "
                          "cubemap's, so a procedural sky would light the scene with a different "
                          "sky than the one you can see.");
            checkB("Aerial perspective", "aerialPerspective", false);
            InspectorHelp(
                "The atmosphere's own camera volume, 32 x 32 x 16 over 96 km, applied to distant "
                "OPAQUE geometry. It is not the height fog: that is Post Process > Exponential "
                "Height Fog, and it is a separate layer the fog is composed over "
                "(SkyAtmosphereCommon.ush:148-151).");
            dragF("Aerial Start Depth (m)", "aerialStartDepthMetres", 100.0f, 1.0f, 0.0f, 5000.0f, "%.0f");
            InspectorHelp(
                "UE AerialPerspectiveStartDepth (their 0.1 km): view depth before which the volume adds "
                "nothing. Its own knob since the part-B review -- it used to ride on the height fog's "
                "volumetric distance, a subsystem that can be off while this is on.");
            dragF("Aerial View Distance Scale", "aerialViewDistanceScale", 1.0f, 0.1f, 0.01f, 100.0f, "%.2f");
            InspectorHelp(
                "UE AerialPerspectiveViewDistanceScale: how many metres of Earth's air one metre of view "
                "distance counts as. 1 = Earth, and Earth's air over a few hundred metres is invisible "
                "(measured 0.8 % on the far water, nothing on the island). 5-20 gives an island the haze "
                "of kilometres -- this is the knob that makes aerial perspective READ, not the start depth.");

            ImGui::SeparatorText("Medium");
            dragF("Rayleigh Scale", "rayleighScale", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
            InspectorHelp("Multiplies Earth's Rayleigh coefficients, so 1.0 is Earth. It is what "
                          "makes the sky blue and the horizon red; more of it deepens both.");
            dragF("Mie Scale", "mieScale", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
            InspectorHelp("Multiplies Earth's Mie scattering AND absorption together -- haze and "
                          "aerosol. It is what puts the white glare around the sun.");
            dragF("Mie Anisotropy", "mieAnisotropy", 0.8f, 0.005f, 0.0f, 0.999f, "%.3f");
            InspectorHelp("How forward-peaked the Mie phase is -- UE's MieAnisotropy, same range "
                          "and default. 0 scatters uniformly; closer to 1 throws light forward and "
                          "puts a HALO around the sun.\n\n"
                          "This is the knob for the bright dome that sits over the horizon at a low "
                          "sun, and it is not the same as Mie Scale: that one changes how much haze "
                          "there is, which takes the sunset's warmth with it. This changes only how "
                          "tightly that haze aims the light at you.");
            dragF("Ozone Scale", "ozoneScale", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
            InspectorHelp("Multiplies Earth's ozone absorption, which is what keeps a twilight sky "
                          "blue overhead instead of grey.");
            dragF("Ground Albedo", "groundAlbedo", 0.402f, 0.01f, 0.0f, 1.0f, "%.3f");
            InspectorHelp("The planet's reflectance, as it enters multi-scattering. UE's default is "
                          "FColor(170) converted from sRGB, which is 0.402 linear -- not 170/255.");
            dragF("Multi-scattering Factor", "multiScatteringFactor", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
            InspectorHelp("Weight on the multi-scattering LUT's contribution. 0 leaves single "
                          "scattering alone, which reads as a much darker sky away from the sun.");
        }
        else if (env.type == "volumetricCloud")
        {
            // Plan part C. Having this object is what gives the level clouds, as adding UE's
            // VolumetricCloud actor is; it needs the Sky Atmosphere object (the clouds are lit by its
            // distant light and hazed by its aerial perspective) and does nothing on a cubemap sky.
            // Every default below is VolumetricCloudSettings.h's (shown when the level leaves a key out).
            checkB("Enabled", "enabled", true);
            InspectorHelp(
                "A layer of volumetric clouds, traced at half resolution one pixel of each 2x2 block a "
                "frame and rebuilt at full resolution over four frames, with their shadow on the island "
                "and in the fog. Ultra Dynamic Sky's density scheme on textures this engine generates. "
                "Needs the Sky Atmosphere object: on a cubemap sky this stays off and the session log "
                "says so.\n\n"
                "--set=cloud.enabled overrides this for a headless run; every row below is "
                "--set=cloud.<key> too.");

            ImGui::SeparatorText("Clouds");
            dragF("Coverage", "coverage", 0.5f, 0.005f, 0.0f, 1.0f, "%.3f");
            InspectorHelp("How much of the sky is cloud: 0 clear .. 1 closed (Ultra Dynamic Sky's Cloud Density "
                          "/ 3). It also sets how TALL the clouds grow -- light cover is low fair-weather puffs, "
                          "heavy cover fills the layer. The single most visible knob.");
            dragF("Cloud Type", "cloudType", 0.0f, 0.01f, -1.0f, 1.0f, "%.2f");
            InspectorHelp("Flatter (towards -1) or taller (towards +1) clouds at the same coverage: shifts which "
                          "column of the height profile Coverage reads.");
            dragF("Macro Variation", "macroVariation", 0.0f, 0.005f, 0.0f, 1.0f, "%.3f");
            InspectorHelp("Moves Coverage up and down by this much over Macro Tile, so the sky has cloudy "
                          "regions and clear ones instead of an even scatter (and the distance is not one "
                          "continuous bank). Clouds in the cloudy regions are taller too.");
            dragF("Macro Tile (km)", "macroTileKm", 60.0f, 0.5f, 1.0f, 1000.0f, "%.0f");
            InspectorHelp("Size of Macro Variation's cloudy and clear regions.");
            dragF("Overcast From", "overcastFrom", 0.85f, 0.005f, 0.0f, 0.99f, "%.3f");
            InspectorHelp("The Coverage where the clouds start closing into one overcast deck: from here to 1 "
                          "the erosion smooths out, Macro Variation fades and the gaps fill, so Coverage 1 is a "
                          "solid sheet -- and over the last of it the sun leaves the ground (no sunlight, no sun "
                          "shadows, no glints on the water). The clouds' tops stay in the sun.");
            dragF("Overcast Evenness", "overcastEvenness", 0.7f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("How evenly the closed deck covers the sky: 0 keeps the cloud field's thick and thin "
                          "patches and the lumpy floor under the overcast, 1 is one uniform grey sheet with a "
                          "flat base. Acts only as the cover closes (from Overcast From to 1).");
            dragF("Formation Tile (km)", "weatherTileKm", 12.0f, 0.1f, 1.0f, 1000.0f, "%.1f");
            InspectorHelp("The scale of the cloud field: the size of the formation volume on the ground, i.e. "
                          "how far apart the cloud groups sit and how big they are. Ultra Dynamic Sky's is 12 km.");
            dragF("Wind Speed (km/h)", "windKmH", 20.0f, 0.5f, 0.0f, 500.0f, "%.0f");
            InspectorHelp("How fast the whole field drifts along the level's wind heading (the Wind "
                          "object), on the shared clock --wind-freeze pins.");
            dragF("Formation Change Speed", "formationChangeSpeed", 1.0f, 0.01f, 0.0f, 100.0f, "%.2f");
            InspectorHelp("How fast the cloud shapes evolve relative to their drift, as Ultra Dynamic Sky's: 1 "
                          "runs the volume's whole time loop while the clouds travel 16 formation tiles -- at "
                          "30 km/h over a 12 km tile a cloud lives 15-30 minutes. No wind, no change.");
            {
                int seed = static_cast<int>(tgt().value("seed", 0u));
                if (ImGui::DragInt("Seed", &seed, 1.0f, 0, 100000))
                {
                    nlohmann::json after = withField("seed", static_cast<unsigned>(std::max(seed, 0)));
                    executeChange(std::move(after), historyLabel);
                }
            }
            InspectorHelp("Regenerates every cloud texture from another seed: a different sky, the same weather.");

            // The shape knobs are FINE TUNING: their defaults are Ultra Dynamic Sky's values or fits to
            // its look (VolumetricCloudSettings.h), and a level that leaves them alone looks like that.
            // Folded, so the knobs above are what the panel shows.
            if (ImGui::TreeNodeEx("Fine tuning (shape)", ImGuiTreeNodeFlags_SpanAvailWidth))
            {
                InspectorHelp("The density model's shape knobs. Their defaults reproduce Ultra Dynamic Sky's look; "
                              "change them only to move away from it.");
                dragF("Formation Roughness", "formationRoughness", 0.85f, 0.005f, 0.3f, 0.9f, "%.3f");
                InspectorHelp("The formation field's fBm gain. Lower = smooth, peaked formations: tall, round "
                              "clouds in big groups; higher = flatter, more ragged ones. Regenerates the 32 MB "
                              "volume (a few ms).");
                dragF("Floor Variation Clear", "floorVariationClear", 0.08f, 0.005f, 0.0f, 2.0f, "%.3f");
                InspectorHelp("How much the cloud bases undulate at light cover (UDS Floor Variation Clear): 0 = "
                              "every base on the layer floor, higher lifts some of them.");
                dragF("Floor Variation Cloudy", "floorVariationCloudy", 0.64f, 0.005f, 0.0f, 2.0f, "%.3f");
                InspectorHelp("The same at heavy cover (Coverage above 0.5): broken, ragged undersides.");
                dragF("Erosion Strength", "erosionStrength", 1.2f, 0.01f, 0.0f, 4.0f, "%.2f");
                InspectorHelp("UDS 3D Erosion: how deep the cells volume carves the formations -- the cauliflower. "
                              "0 leaves smooth blobs.");
                dragF("Erosion Power", "erosionPower", 3.0f, 0.05f, 0.25f, 8.0f, "%.2f");
                InspectorHelp("UDS Erosion Power: higher = only the cells' brightest parts carve (rounder bulges "
                              "with sharp gaps), lower = everything erodes a little (softer, fuzzier).");
                dragF("Erosion Tile (km)", "erosionTileKm", 1.2f, 0.01f, 0.1f, 20.0f, "%.2f");
                InspectorHelp("World size of the cells volume (vertically 0.58 of it): the size of the bulges.");
                dragF("Fine Detail Strength", "highFrequencyStrength", 0.24f, 0.005f, 0.0f, 2.0f, "%.3f");
                InspectorHelp("UDS High Frequency Noise: the fine, wispy detail on edges near the camera.");
                dragF("Fine Detail Distance (km)", "highFrequencyDistanceKm", 1.5f, 0.05f, 0.1f, 20.0f, "%.2f");
                InspectorHelp("The fine detail fades out by this distance (it would only shimmer further away).");

                ImGui::SeparatorText("Height profile");
                InspectorHelp("The generated profile LUT: per coverage (column) and height, how far up a formation "
                              "reaches, how dense it is and how much it erodes. Each change regenerates it (cheap). "
                              "--cloud-profile=<png> replaces it for the session.");
                dragF("Profile Height", "profileHeight", 1.0f, 0.01f, 0.2f, 2.0f, "%.2f");
                InspectorHelp("How tall the formations grow at every coverage (1 = heavy cover reaches the top of "
                              "the layer).");
                dragF("Dome", "profileDome", 0.60f, 0.005f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("How fast a formation narrows with height: 0 = flat-topped slabs, 1 = steep domes "
                              "where only the dense core reaches up (towers). Heavy cover flattens it on its own.");
                dragF("Top Softness", "profileTopSoftness", 0.27f, 0.005f, 0.01f, 1.0f, "%.3f");
                InspectorHelp("The fraction of a formation's height its top closes over: low = hard tops.");
                dragF("Base Softness", "profileBaseSoftness", 0.29f, 0.005f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("The height over which the underside gains density: low = crisp flat bases, high = "
                              "soft, hazy ones.");
                dragF("Small Cloud Density", "profileSmallDensity", 0.09f, 0.005f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("Density at no coverage, i.e. of the smallest clouds (applied cubed): low = thin, wispy "
                              "puffs; high = solid little cumulus.");
                dragF("Body Density", "profileBodyDensity", 0.65f, 0.005f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("Density at full coverage (cubed: 0.65 = 0.27 of Extinction); a developed cumulus "
                              "field at coverage 0.45 is 80 % of the way there. Higher = darker cores.");
                dragF("Base Erosion", "profileBaseErosion", 0.20f, 0.005f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("Erosion at the cloud floor: low = flat, firm undersides; high = ragged ones. Heavy cover "
                              "(above ~0.6) never erodes its floor: an overcast deck's underside is smooth.");
                dragF("Erosion Rise", "profileErosionRise", 0.53f, 0.005f, 0.05f, 1.0f, "%.3f");
                InspectorHelp("How far up (a fraction of the formation's top) the erosion reaches full strength: "
                              "low = cauliflower right above the base.");
                dragF("Heavy Cover Erosion Cut", "profileHeavyErosionCut", 0.35f, 0.005f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("How much less heavy cover erodes: smoother overcast.");
                dragF("Heavy Cover Thinning", "profileHeavyThinning", 0.66f, 0.005f, 0.0f, 1.0f, "%.3f");
                InspectorHelp("How much cover above ~0.4 thins with height: a dense base under a softer top.");
                ImGui::TreePop();
            }

            ImGui::SeparatorText("Layer");
            dragF("Bottom Altitude (km)", "layerBottomKm", 0.6f, 0.05f, 0.1f, 20.0f, "%.2f");
            InspectorHelp("Height of the cloud bases above sea level. Trade-wind cumulus over a tropical sea "
                          "sits at ~0.6 km; UE's default 5 km is for a big continental sky.");
            dragF("Layer Height (km)", "layerHeightKm", 0.7f, 0.05f, 0.1f, 20.0f, "%.2f");
            InspectorHelp("Thickness of the layer: the tallest clouds (heavy cover) fill it, light cover uses the "
                          "bottom fifth.");

            ImGui::SeparatorText("Medium");
            dragF("Extinction (1/m)", "extinctionScale", 10.0f, 0.05f, 0.0f, 20.0f, "%.2f");
            InspectorHelp("How dense the cloud is at the height profile's density 1, per metre. The profile's "
                          "density is applied cubed (a developed cloud's ~0.5 is 1/8 of this), so it sits ~8x a "
                          "real cumulus' 0.04-0.1; Ultra Dynamic Sky runs 10. Lower reads as haze, higher as a "
                          "hard-edged wall.");
            dragF("Albedo", "albedo", 0.95f, 0.005f, 0.0f, 1.0f, "%.3f");
            InspectorHelp("Single-scattering albedo of the droplets -- how much of the light a "
                          "particle scatters rather than absorbs. Water is close to 1; lower darkens "
                          "the shadowed sides.");

            ImGui::SeparatorText("Lighting");
            dragF("Phase G", "phaseG", 0.85f, 0.005f, -0.99f, 0.99f, "%.3f");
            InspectorHelp("Henyey-Greenstein anisotropy of the main lobe, POSITIVE forward: the silver lining "
                          "when the sun is behind the cloud. The same sign as UE's material node -- a UE or "
                          "Ultra Dynamic Sky value is entered as is.");
            dragF("Phase G2", "phaseG2", 0.4f, 0.005f, -0.99f, 0.99f, "%.3f");
            InspectorHelp("A second, broader lobe blended in by Phase Blend: a gentler forward glow, or "
                          "(negative) some light thrown back towards the sun-facing side.");
            dragF("Phase Blend", "phaseBlend", 0.65f, 0.005f, 0.0f, 1.0f, "%.3f");
            InspectorHelp("How much of the second lobe is mixed in. 0 = the main lobe alone.");
            dragF("Multi-scatter Contribution", "msContribution", 0.85f, 0.005f, 0.0f, 1.0f, "%.3f");
            InspectorHelp("UE MultiScatteringContribution: how much light each further scattering "
                          "octave adds. Brightens the thick, self-shadowed middle of a cloud.");
            dragF("Multi-scatter Occlusion", "msOcclusion", 0.5f, 0.005f, 0.0f, 1.0f, "%.3f");
            InspectorHelp("UE MultiScatteringOcclusion: how much less each octave is shadowed. "
                          "Lower lets light deeper into the cloud.");
            dragF("Multi-scatter Eccentricity", "msEccentricity", 0.4f, 0.005f, 0.0f, 1.0f, "%.3f");
            InspectorHelp("UE MultiScatteringEclipse: how much each octave keeps the phase's "
                          "direction. Lower makes the extra light more even.");
            {
                int octaves = static_cast<int>(tgt().value("msOctaves", 2u));
                if (ImGui::SliderInt("Multi-scatter Octaves", &octaves, 0, 2))
                {
                    nlohmann::json after = withField("msOctaves", static_cast<unsigned>(std::clamp(octaves, 0, 2)));
                    executeChange(std::move(after), historyLabel);
                }
            }
            InspectorHelp("UE MultiScatteringApproximationOctaveCount: extra scattering octaves beyond the "
                          "first bounce. Each is shadowed less (Occlusion squared), so the second carries the "
                          "sun's own colour into the base of a dense cloud -- a lighter, greyer underside. "
                          "Ultra Dynamic Sky runs 2.");
            dragF("Sky Light Bottom Occlusion", "skyLightBottomOcclusion", 0.0f, 0.005f, 0.0f, 1.0f, "%.3f");
            InspectorHelp("UE SkyLightCloudBottomOcclusion: how much of the sky's light the bottom of the layer "
                          "loses. 0 lights the underside like the top (Ultra Dynamic Sky), 1 leaves it only the "
                          "sun.");
            dragF("Sky Light Intensity", "skyLightIntensity", 1.0f, 0.01f, 0.0f, 10.0f, "%.2f");
            InspectorHelp("Scales the blue sky's light on the clouds -- the light their shadowed sides get. "
                          "Higher = lighter AND bluer shading (weaker shadows), lower = darker, greyer "
                          "undersides. 1 = the atmosphere as computed. Under a closing overcast it reaches the "
                          "inside only through the deck.");
            dragF("Light Through Cloud", "diffuseLight", 0.25f, 0.01f, 0.0f, 10.0f, "%.2f");
            InspectorHelp("Sunlight diffused down through a cloud's depth -- what lights the base of a thick "
                          "cloud and an overcast deck from below (thick clouds get darker bases than thin ones). "
                          "0.25 = Ultra Dynamic Sky's cool slate overcast and contrasty bases; 1 = the physical "
                          "estimate (a lighter, warmer deck, paler bases); 0 = none, the scattering octaves alone "
                          "(UE's model): a closed deck is then lit by the blue sky only and reads as a blue sky.");
            dragF("Reflection Horizon Fade (deg)", "reflectionHorizonFadeDeg", 5.0f, 0.1f, 0.0f, 45.0f, "%.1f");
            InspectorHelp("The clouds in the sky's captured picture -- what the WATER reflects (and the probes and "
                          "the fog's sky) -- fade out below this elevation, so the sea along the horizon reflects "
                          "the clear haze instead of blotches of the far cloud band. Not under a closing deck, "
                          "where the horizon is cloud. 0 = keep them all. The clouds on screen are not touched.");

            ImGui::SeparatorText("Tracing");
            {
                int samples = static_cast<int>(tgt().value("viewSampleCountMax", 128u));
                if (ImGui::DragInt("View Samples (max)", &samples, 1.0f, 4, 768))
                {
                    nlohmann::json after = withField("viewSampleCountMax", static_cast<unsigned>(std::clamp(samples, 4, 768)));
                    executeChange(std::move(after), historyLabel);
                }
            }
            InspectorHelp("Samples along a view ray as long as Sample Distance; the step is that distance "
                          "divided by this, constant for shorter rays (3 km / 128 = 23 m). This is where the "
                          "cost is: 0.43 ms at 128, 0.71 at 256 (2560 x 1440, DLSS Balanced).");
            {
                int samples = static_cast<int>(tgt().value("viewSampleCountMin", 2u));
                if (ImGui::DragInt("View Samples (min)", &samples, 1.0f, 1, 768))
                {
                    nlohmann::json after = withField("viewSampleCountMin", static_cast<unsigned>(std::clamp(samples, 1, 768)));
                    executeChange(std::move(after), historyLabel);
                }
            }
            InspectorHelp("UE SampleMinCount: no ray takes fewer samples, however short its path through the layer "
                          "-- the step shrinks instead. Ultra Dynamic Sky sets 135 (UE 2). It is what removes the "
                          "\"snow\" beside the sun: wisps thinner than a step, caught by a ray now and then. On "
                          "wind_test (2026-10-14, a ~1.7 km path = ~72 steps at 23 m): specks in the hole by the sun "
                          "5.5 -> 0 a frame in trace mode 0, 15.5 -> 2.4 in mode 1, for +0.12 / +0.19 ms of GPU frame.");
            dragF("Sample Distance (km)", "distanceToSampleCountMaxKm", 3.0f, 0.05f, 0.1f, 1000.0f, "%.2f");
            InspectorHelp("UE DistanceToSampleMaxCount: the ray length View Samples are spread over (UE 15 km). "
                          "Shorter = finer steps, the same count: sharper edges through a thin layer.");
            {
                int samples = static_cast<int>(tgt().value("shadowSampleCount", 4u));
                if (ImGui::DragInt("Sun Shadow Samples", &samples, 1.0f, 1, 80))
                {
                    nlohmann::json after = withField("shadowSampleCount", static_cast<unsigned>(std::clamp(samples, 1, 80)));
                    executeChange(std::move(after), historyLabel);
                }
            }
            InspectorHelp("Samples of the march towards the sun taken at EVERY lit view sample -- the "
                          "self-shadowing that gives a cloud its dark belly. Multiplies the cost.");
            dragF("Sun Shadow Distance (km)", "shadowTracingDistanceKm", 0.156f, 0.005f, 0.1f, 100.0f, "%.3f");
            InspectorHelp("UE ShadowTracingDistance: how far towards the sun the self-shadow march looks (UE 15 km, "
                          "Ultra Dynamic Sky 0.156). Longer = darker, deeper self-shadow.");
            dragF("Stop Transmittance", "stopTracingTransmittance", 0.005f, 0.0005f, 0.0f, 0.5f, "%.4f");
            InspectorHelp("UE StopTracingTransmittanceThreshold: the ray stops once this little light "
                          "would get through. Higher is cheaper and lets less light punch through.");
            dragF("Tracing Max Distance (km)", "tracingMaxDistanceKm", 50.0f, 0.5f, 1.0f, 1000.0f, "%.0f");
            InspectorHelp("UE TracingMaxDistance: how far past the layer's entry point a ray is followed.");
            dragF("Tracing Start Max (km)", "tracingStartMaxDistanceKm", 350.0f, 1.0f, 1.0f, 10000.0f, "%.0f");
            InspectorHelp("UE TracingStartMaxDistance: a ray whose layer entry is further than this is "
                          "not traced at all.");
            checkB("Temporal", "temporal", true);
            InspectorHelp("Blend each pixel's new sample into its history (reprojected by the cloud's own depth). "
                          "OFF: every pixel shows its latest sample alone -- the trace's noise, unfiltered.");
            dragF("History Weight", "historyWeight", 0.9f, 0.005f, 0.0f, 0.99f, "%.2f");
            InspectorHelp("How much of the history survives each frame: 0.9 is about a ten-frame history, "
                          "lower follows a fast sun or wind more closely at the price of noise.");

            ImGui::SeparatorText("Shadow map");
            checkB("Cloud Shadows", "shadowMap", true);
            InspectorHelp("UE r.VolumetricCloud.ShadowMap: an orthographic map from the sun over the "
                          "ground under the camera, multiplied into the sun wherever it lights the "
                          "scene -- the island, the volumetric fog. This is the cloud's shadow "
                          "crawling over the sand, and it works even when no cloud is in frame.");
            dragF("Shadow Extent (km)", "shadowExtentKm", 20.0f, 0.5f, 1.0f, 500.0f, "%.0f");
            InspectorHelp("Half-width of the map on the ground. 512 texels across twice this: 20 km "
                          "is 78 m a texel. UE default 150 km (585 m a texel) is for a continent.");
            dragF("Shadow Strength", "shadowStrength", 1.0f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("UE CloudShadowStrength: scales the optical depth the map reports. 1 = the "
                          "cloud's own density.");
            dragF("Ocean Body Shadow", "oceanBodyShadow", 0.35f, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorHelp("How much of the cloud shadow the WATER BODY takes; the sun glints and the foam "
                          "always lose the sun. 0 = the water's own light ignores the cloud, 1 = it darkens "
                          "with the sun like the land. Under broken cloud the bright cloud sides keep the "
                          "sea lit far more than the shadow map alone says.");
            dragF("Shadow Horizon Fade (deg)", "shadowHorizonFadeDeg", 6.0f, 0.1f, 0.0f, 45.0f, "%.1f");
            InspectorHelp("The cloud shadow on the ground and the water fades out as the sun (or the moon) goes "
                          "down: full from this height, none below 1 degree (the volumetric fog keeps all of it). "
                          "A light at the horizon throws a cloud's shadow tens of km, and every small turn of it "
                          "moves the shadow by up to a kilometre: under it the glitter path flickered. "
                          "0 = the shadow at any height (UE).");
            dragF("Shadow Snap (km)", "shadowSnapKm", 2.0f, 0.1f, 0.01f, 100.0f, "%.2f");
            InspectorHelp("UE ShadowMap.SnapLength: the map's centre moves in steps of this, so the "
                          "shadow does not shimmer as the camera walks.");
            dragF("Shadow Depth Bias (km)", "shadowDepthBiasKm", 0.0f, 0.01f, -5.0f, 5.0f, "%.2f");
            InspectorHelp("UE CloudShadowDepthBias: pushes the recorded cloud front along the light.");
            {
                int samples = static_cast<int>(tgt().value("shadowMapSampleCount", 16u));
                if (ImGui::DragInt("Shadow Map Samples", &samples, 1.0f, 4, 128))
                {
                    nlohmann::json after = withField("shadowMapSampleCount", static_cast<unsigned>(std::clamp(samples, 4, 128)));
                    executeChange(std::move(after), historyLabel);
                }
            }
            InspectorHelp("Samples per map texel through the layer (UE base 16, doubled towards the horizon).");
        }
        else if (env.type == "skybox")
        {
            // BOTH ROWS BELOW MEET IN ONE LINE, `Skybox::GetExposure()`:
            //     return HasEnvironment() ? 1.0f : exposure_ * PhysicalScale();
            // `exposure_` is Intensity, `PhysicalScale()` is Sky Illuminance / the cube's own
            // measured up-illuminance. When the procedural atmosphere supplies the environment --
            // `sky.mode` on AND `environmentLighting` on, the exact gate in
            // `SkyAtmosphere::BuildEnvironment` -- their product is discarded and the whole sky,
            // its ambient fill, its reflections and the fog's sky term run at a hard 1.0.
            //
            // Asking the Skybox itself rather than re-deriving the condition here: this is the same
            // object `GetExposure()` branches on, so the greying cannot drift from the behaviour.
            // Measured before shipping the grey: `--set=light.skyIlluminanceLux` 12000 vs 40000 on
            // a procedural level moved the water by 0.007 % of pixels against a 0.007 % same-build
            // noise floor, and the frame median by 0.05 of a code.
            const Skybox* liveSky = ctx.scene.GetSkybox();
            const bool skyInert = liveSky != nullptr && liveSky->HasEnvironment();
            if (skyInert)
            {
                ImGui::TextDisabled("Procedural sky owns the environment -- the two rows below do nothing.");
                InspectorHelp(
                    "The Sky Atmosphere object is on with Environment Lighting enabled, so it "
                    "supplies this level's environment cubes and `Skybox::GetExposure()` returns a "
                    "hard 1.0. Intensity and Sky Illuminance are multiplied into that value and "
                    "nowhere else, so neither changes anything -- including the drawn sky.\n\n"
                    "What to use instead: Sky Atmosphere > Sky Luminance Scale for how bright the "
                    "sky reads, and the directional light's Sky Fill Intensity for how hard it "
                    "lights the scene.\n\n"
                    "Turn Environment Lighting off (or delete the Sky Atmosphere object) and these "
                    "two come back, because the level is then lit by the cubemap again.");
            }
            ImGui::BeginDisabled(skyInert);
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

            // P16.3b. Logarithmic for the same reason the sun's row is: the useful range runs from
            // a few hundred lux to twenty thousand.
            dragF("Sky Illuminance (lux)", "illuminanceLux", 0.0f, 1.0f, 0.0f, 50000.0f,
                  "%.0f", ImGuiSliderFlags_Logarithmic);
            InspectorHelp(
                "How much light this sky puts on a HORIZONTAL surface, in lux. It is the sky's half "
                "of the same physical scale the sun's illuminance uses, and it is what puts the two "
                "on one ruler.\n\n"
                "12000 clear sky with the sun about 30 degrees up, 20000 clear sky with the sun "
                "overhead, 10000-20000 heavy overcast (where the sky IS the light), 2000 deep "
                "twilight.\n\n"
                "0 = NOT AUTHORED, and then nothing changes: the sky keeps the level's old "
                "behaviour, scaled only by Intensity above. That is what keeps an unconverted "
                "level rendering exactly what it renders today.\n\n"
                "The scale that realises the number is DERIVED, never authored -- the engine reads "
                "this sky's own irradiance cube and divides -- so the same 12000 means the same "
                "thing on a cube that came out of the importer bright and one that came out dim. "
                "Check the Session log window's [ibl] line for the measured value and the factor it produced.\n\n"
                "Needs the F7 IBL siblings (_spec/_diffuse). A sky imported without them has "
                "nothing to measure and this row does nothing.");

            // P16.6b -- WHAT TO PUT IN IT. Switching skyboxes should not mean guessing a
            // four-figure number, and the answer does not come from the sky asset: it comes from
            // WHERE THE SUN IS. The clear-day model is the one the levels were converted with, so
            // the recommendation and the content agree by construction.
            {
                const Math::float3 sunDir =
                    ctx.scene.GetDirectionalLight().GetDirection().Normalized();
                const float sinE = std::clamp(-sunDir.y, 0.02f, 1.0f);
                const float sunPerp = 128000.0f * std::exp(-0.21f / sinE);
                const float f = 0.32f - 0.18f * sinE;
                const float recommended = f / (1.0f - f) * sunPerp * sinE;
                const float elevDeg = std::asin(sinE) * 180.0f / 3.14159265f;
                ImGui::TextDisabled("Sun is %.0f deg up -> a clear sky delivers about %.0f lx",
                                    elevDeg, recommended);
                ImGui::SameLine();
                if (ImGui::SmallButton("Use it"))
                {
                    nlohmann::json after = withField("illuminanceLux", recommended);
                    executeChange(std::move(after), historyLabel);
                }
                InspectorHelp(
                    "The number depends on the SUN, not on which photograph you picked: a clear sky "
                    "at 30 degrees delivers about 12000 lx, at the horizon a tenth of that.\n\n"
                    "AN OVERCAST SKY IS THE OTHER CASE ENTIRELY -- there the sky IS the light, so "
                    "use 10000-20000 AND take the directional sun down to near zero. Pasting a "
                    "bright sun over a photograph of an overcast evening is what makes the ground "
                    "come out brighter than the sky above it.\n\n"
                    "The import log says which kind you have: 'sky sun REMOVED ... the sun was N%%' "
                    "for a clear sky, 'NOT REMOVED ... no sun to take out' for an overcast one.");
            }
            // The Texture Asset row below stays LIVE even when the two above are greyed: a
            // procedural level still needs a cubemap named here, because `JsonLevel` only creates
            // the Skybox object -- the thing that draws the atmosphere -- when one is.
            ImGui::EndDisabled();
            const std::string current = tgt().value("texture", std::string());
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
                        nlohmann::json after = withField("texture", NormalizePath(rec.id.key));
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
            std::snprintf(buf, sizeof(buf), "%s", tgt().value("texture", std::string()).c_str());
            const float applyButtonWidth = ImGui::CalcTextSize("Apply Texture").x +
                ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetNextItemWidth(std::max(120.0f,
                ImGui::GetContentRegionAvail().x - applyButtonWidth - ImGui::GetStyle().ItemSpacing.x));
            const bool textureChanged = ImGui::InputText("Texture", buf, sizeof(buf));
            beginContinuousEdit(textureChanged);
            if (textureChanged)
            {
                tgt()["texture"] = NormalizePath(buf);
                ctx.document.SetDirty(true);
            }

            const bool textureCommitted = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine();
            const bool applyTexture = ImGui::Button("Apply Texture");
            if (textureCommitted && activeEditObject.value == env.id.value)
            {
                commandStack.Execute(ctx, std::make_unique<EditEnvironmentCommand>(
                    env.id,
                    propertiesBeforeEdit,
                    props,
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
                const std::string curPreset = tgt().value("preset", std::string());
                const std::string curLabel = std::filesystem::path(curPreset).filename().string();
                if (ImGui::BeginCombo("Preset", curLabel.empty() ? "(none)" : curLabel.c_str()))
                {
                    for (const std::string& pr : EnvironmentRuntime::OceanPresets())
                    {
                        const bool sel = (pr == curPreset);
                        const std::string label = std::filesystem::path(pr).filename().string();
                        if (ImGui::Selectable(label.c_str(), sel) && !sel)
                        {
                            nlohmann::json after = withField("preset", pr);
                            executeChange(std::move(after), "Set Ocean Preset");
                        }
                    }
                    ImGui::EndCombo();
                }

                OceanRenderConfig render = ocean->GetRenderConfig();
                const auto renderIt = tgt().find("render");
                if (renderIt != tgt().end() && renderIt->is_object())
                {
                    OceanRenderConfigJson::ApplyOverrides(*renderIt, render);
                }

                const auto storeRender = [&]()
                {
                    tgt()["render"] = OceanRenderConfigJson::ToJson(render);
                };
                const auto beginRenderContinuousEdit = [&]()
                {
                    if (ImGui::IsItemActivated())
                    {
                        activeEditObject = env.id;
                        propertiesBeforeEdit = props;
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
                                props;
                        commandStack.Execute(ctx, std::make_unique<EditEnvironmentCommand>(
                            env.id,
                            std::move(before),
                            props,
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
                    renderDrag("Low-Sun Body Light", render.lowSunBodyLight, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How much of the sun still lights the water body with the sun on the horizon.\n"
                                          "Below 12 degrees the sun's share fades by what actually gets into the water;\n"
                                          "this is the floor of that fade. 1 = no fade. Above 12 degrees: no effect.");
                }

                if (ImGui::CollapsingHeader("Specular and reflection", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    renderDrag("Specular Strength", render.specularStrength, 0.01f, 0.0f, 10.0f);
                    renderDrag("Roughness Scale", render.roughnessScale, 0.005f, 0.0f, 1.0f);
                    renderDrag("Roughness Distance", render.roughnessDistance, 1.0f, 1.0f, 5000.0f, "%.1f");
                    renderDrag("Reflection Normal Flattening", render.reflectionNormalStrength, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Pulls the reflection normal toward +Y (1 = a flat mirror) for BOTH the sky\n"
                                          "sample and the screen-space trace. Higher values bring back reflections of a\n"
                                          "distant shore at grazing angles, but flatten the sky's ripples.");
                    renderDrag("Sky Reflection Horizon Pull", render.reflectionSkyHorizonPull, 0.005f, 0.05f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Compresses the SKY reflection ray toward the horizon (1 = off).\n"
                                          "A clear-sky HDRI is much darker at the zenith, and a wave's facets\n"
                                          "swing the reflected ray between horizon and zenith - at grazing angles\n"
                                          "Fresnel passes that contrast through at full strength, so it reads as\n"
                                          "hard dark streaks along the crests. Lower values keep the water in the\n"
                                          "bright band near the horizon, which is also where real water reflects\n"
                                          "from at these angles. ~0.4-0.6 is the useful range. Affects the ocean's\n"
                                          "environment sample only: the screen-space reflection, land IBL and sky untouched.");
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
                            nlohmann::json after = withField("render", OceanRenderConfigJson::ToJson(afterRender));
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
                        renderDrag("Surf Foam Coverage Multiplier", render.surfSimFoamCoverageMultiplier, 0.005f, 0.0f, 2.0f);
                        tip("Multiplies final surf-sim foam coverage after breakup and window fade.\n"
                            "0 = hidden, 1 = unchanged, above 1 = stronger (clamped to full coverage).\n"
                            "Does not change foam generation, lifetime, contact foam or FFT whitecaps.");
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
                        nlohmann::json after = withField("render", OceanRenderConfigJson::ToJson(afterRender));
                        executeChange(std::move(after), "Set Ocean Caustics Enabled");
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip(
                            "Sun caustics on everything below the water line. Applied in the\n"
                            "deferred lighting pass, so they follow the sun shadow.");
                    }
                    const auto selectCausticsTexture = [&](const std::string& path)
                    {
                        OceanRenderConfig afterRender = render;
                        afterRender.causticsTexture = NormalizePath(path);
                        nlohmann::json after = withField("render", OceanRenderConfigJson::ToJson(afterRender));
                        executeChange(std::move(after), "Set Ocean Caustics Texture");
                        render = afterRender;
                    };
                    if (ImGui::BeginCombo("Flipbook Asset", render.causticsTexture.c_str()))
                    {
                        for (const EditorAssetRecord& rec : registry.Assets())
                        {
                            if (rec.id.type != EditorAssetType::Texture || !rec.texture.valid ||
                                rec.texture.kind != EditorTextureKind::Texture2D)
                                continue;
                            const bool selected = rec.id.key == render.causticsTexture;
                            if (ImGui::Selectable(rec.id.key.c_str(), selected) && !selected)
                                selectCausticsTexture(rec.id.key);
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(EditorDragDrop::kAssetPayloadType))
                        {
                            EditorAssetId id;
                            if (EditorDragDrop::DecodeAssetPayload(payload, id))
                            {
                                const auto* rec = registry.FindById(id);
                                if (rec && rec->id.type == EditorAssetType::Texture && rec->texture.valid &&
                                    rec->texture.kind == EditorTextureKind::Texture2D)
                                    selectCausticsTexture(rec->id.key);
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    char causticsPath[1024];
                    std::snprintf(causticsPath, sizeof(causticsPath), "%s", render.causticsTexture.c_str());
                    const bool pathChanged = ImGui::InputText("Flipbook Path", causticsPath, sizeof(causticsPath));
                    beginRenderContinuousEdit();
                    if (pathChanged)
                    {
                        render.causticsTexture = NormalizePath(causticsPath);
                        storeRender();
                        ctx.document.SetDirty(true);
                    }
                    // Commit once focus leaves the field, rather than uploading on each keystroke.
                    finishRenderContinuousEdit(false);
                    const auto causticsInt = [&](const char* label, int& value, int minimum, int maximum)
                    {
                        const bool edited = ImGui::InputInt(label, &value);
                        beginRenderContinuousEdit();
                        if (edited)
                        {
                            value = std::clamp(value, minimum, maximum);
                            render.causticsFrameCount = std::clamp(render.causticsFrameCount, 0,
                                render.causticsColumns * render.causticsRows);
                            storeRender();
                        }
                        finishRenderContinuousEdit(edited);
                    };
                    causticsInt("Columns", render.causticsColumns, 1, 1024);
                    causticsInt("Rows", render.causticsRows, 1, 1024);
                    causticsInt("Frame Count (0 = all)", render.causticsFrameCount, 0,
                        render.causticsColumns * render.causticsRows);
                    ImGui::TextDisabled("Frames: left to right, then top to bottom.");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("The image width/height must divide evenly by Columns/Rows.\n"
                                          "Both bundled caustics atlases use 4 columns and 4 rows.\n"
                                          "Texture selection keeps Dark Bias and the other lighting controls.");
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
                        nlohmann::json after = withField("render", OceanRenderConfigJson::ToJson(afterRender));
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
                        nlohmann::json after = withField("render", OceanRenderConfigJson::ToJson(afterRender));
                        executeChange(std::move(after), "Add Ocean Absorption Key");
                    }
                    if (render.absorptionColors.size() > 1u)
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("Remove Key"))
                        {
                            OceanRenderConfig afterRender = render;
                            afterRender.absorptionColors.pop_back();
                            nlohmann::json after = withField("render", OceanRenderConfigJson::ToJson(afterRender));
                            executeChange(std::move(after), "Remove Ocean Absorption Key");
                        }
                    }
                }

                ImGui::SeparatorText("Simulation overrides");
                float windForce = JsonFloat(tgt(), "windForce", ocean->GetWindForce01());
                const bool windForceChanged =
                    ImGui::SliderFloat("Wind Force", &windForce, 0.0f, 1.0f);
                beginContinuousEdit(windForceChanged);
                if (windForceChanged) { tgt()["windForce"] = windForce; }
                trackContinuousEdit(windForceChanged);

                float windDirection = JsonFloat(
                    tgt(),
                    "windDirectionDeg",
                    ocean->GetLocalWindDirectionDegrees());
                const bool windDirectionChanged = ImGui::DragFloat(
                    "Wind Direction",
                    &windDirection,
                    0.5f,
                    -360.0f,
                    360.0f,
                    "%.1f deg");
                beginContinuousEdit(windDirectionChanged);
                if (windDirectionChanged) { tgt()["windDirectionDeg"] = windDirection; }
                trackContinuousEdit(windDirectionChanged);

                float swellDirection = JsonFloat(
                    tgt(),
                    "swellDirectionDeg",
                    ocean->GetSwellDirectionDegrees());
                const bool swellDirectionChanged = ImGui::DragFloat(
                    "Swell Direction",
                    &swellDirection,
                    0.5f,
                    -360.0f,
                    360.0f,
                    "%.1f deg");
                beginContinuousEdit(swellDirectionChanged);
                if (swellDirectionChanged) { tgt()["swellDirectionDeg"] = swellDirection; }
                trackContinuousEdit(swellDirectionChanged);
            }
        }
        else if (env.type == "wind")
        {
            const vfx::WindState& wind = ctx.scene.GetWindState();

            ImGui::SeparatorText("Flow");
            dragF("Direction", "directionDeg", wind.directionDeg,
                0.5f, -360.0f, 360.0f, "%.1f deg");

            float strength = JsonFloat(tgt(), "strength", wind.strength);
            const bool strengthChanged = ImGui::SliderFloat("Strength", &strength, 0.0f, 1.0f);
            beginContinuousEdit(strengthChanged);
            if (strengthChanged) { tgt()["strength"] = strength; }
            trackContinuousEdit(strengthChanged);

            dragF("Sway Frequency", "swayFrequency", wind.swayFrequency,
                0.01f, 0.0f, 10.0f, "%.2f");
            dragF("Foliage Sway", "foliageSwayMeters", wind.foliageSwayMeters,
                0.01f, 0.0f, 5.0f, "%.2f m");

            ImGui::SeparatorText("Gusts");
            const auto dragGust = [&](const char* label, const char* key, float def,
                float speed, float lo, float hi, const char* fmt)
            {
                float value = def;
                const auto gustIt = tgt().find("gust");
                if (gustIt != tgt().end() && gustIt->is_object())
                {
                    value = JsonFloat(*gustIt, key, def);
                }
                const bool changed = ImGui::DragFloat(label, &value, speed, lo, hi, fmt);
                beginContinuousEdit(changed);
                if (changed)
                {
                    if (!tgt().contains("gust") || !tgt()["gust"].is_object())
                    {
                        tgt()["gust"] = nlohmann::json::object();
                    }
                    tgt()["gust"][key] = value;
                }
                trackContinuousEdit(changed);
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

            // W8 debug freeze. Deliberately NOT written into the object properties: this is a viewing
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
    const std::string current = object.properties.value("shape", "circle");
    int shapeIndex = current == "rect" ? 1 : current == "spline" ? 2 : 0;
    if (ImGui::Combo("Shape", &shapeIndex, shapes, IM_ARRAYSIZE(shapes)))
    {
        object.properties["shape"] = shapes[shapeIndex];
        // Switching INTO a spline with no points would give a zone that silently falls back
        // to a circle. Seed it from the size it already had, so the change is visible.
        if (shapeIndex == 2 && editorzone::LocalPoints(object).size() < 2)
        {
            const float size = std::max(object.properties.value("radius", 25.0f),
                object.properties.value("halfX", 25.0f));
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
        bool closed = object.properties.value("closed", false);
        if (ImGui::Checkbox("Closed loop", &closed))
        {
            object.properties["closed"] = closed;
            changed = true;
        }

        const char* fills[] = { "along the line", "inside the outline" };
        int fillIndex = object.properties.value("fill", std::string("along")) == "inside" ? 1 : 0;
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
            float halfWidth = object.properties.value("halfWidth", 8.0f);
            if (ImGui::DragFloat("Half width", &halfWidth, 0.1f, 0.25f, 500.0f, "%.1f m"))
            {
                object.properties["halfWidth"] = std::max(0.25f, halfWidth);
                changed = true;
            }
        }

        const char* planes[] = { "XZ (ground plan)", "XY (front)", "ZY (side)" };
        const std::string planeNow = object.properties.value("plane", std::string("xz"));
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

        float pointRadius = object.properties.value("pointRadius", 1.5f);
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
            float radius = object.properties.value("radius", 25.0f);
            if (ImGui::DragFloat("Radius", &radius, 0.1f, 0.1f, 2000.0f, "%.1f m"))
            {
                object.properties["radius"] = std::max(0.1f, radius);
                changed = true;
            }
        }
        else
        {
            float half[2] = { object.properties.value("halfX", 25.0f),
                object.properties.value("halfZ", 25.0f) };
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
