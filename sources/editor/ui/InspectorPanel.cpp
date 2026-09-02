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
            env.type == "directionalLight" ? "Edit Directional Light" :
            env.type == "camera" ? "Edit Camera" :
            env.type == "skybox" ? "Edit Skybox" :
            env.type == "ocean" ? "Edit Ocean" :
            env.type == "wind" ? "Edit Wind" :
            env.type == "cameraExposure" ? "Edit Camera Exposure" :
            env.type == "colorPipeline" ? "Edit Color Pipeline" :
            env.type == "gtao" ? "Edit Ambient Occlusion" :
            env.type == "atmosphere" ? "Edit Aerial Perspective" :
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
            env.type == "pointLight" || env.type == "spotLight" ||
            env.type == "directionalLight" || env.type == "ocean";
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
                              "and clamping the result flattens rather than fixes that. The plan "
                              "records an exposure-compensation CURVE as the real answer.");
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
        const auto drawAtmosphere = [&]()
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
            InspectorHelp("Exposure-aware HDR bloom: a threshold pass, a half-resolution pyramid "
                          "and a tent reconstruction, composited into the image BEFORE the tone "
                          "curve. Off schedules no pass at all rather than a pass that adds zero, "
                          "so off genuinely costs nothing.");

            ImGui::SeparatorText("Extraction");
            dragF("Threshold", "threshold", 1.0f, 0.01f, -1.0f, 16.0f, "%.2f");
            InspectorHelp("Luminance at which bloom starts, measured AFTER exposure - UE's "
                          "BloomThreshold, in the same units. Being post-exposure is what makes one "
                          "number mean the same thing across a sunset and a noon.\n\n"
                          "NEGATIVE DISABLES IT, and that is UE's own default (-1 in Scene.cpp), "
                          "documented there as \"all pixels affect bloom equally (physically "
                          "correct)\". A lens scatters light from EVERYTHING in front of it, not "
                          "only from whatever passes a brightness test.\n\n"
                          "It also decides how bloom answers to EXPOSURE. With a threshold, raising "
                          "exposure both scales the pixels already over the line and pushes new "
                          "ones over it, so bloom grows faster than the image does. At -1 the "
                          "response is exactly linear: twice the exposure, twice the bloom, no "
                          "step.\n\n"
                          "ON THIS PROJECT'S CONTENT, -1 COSTS YOU THE RAYS. Measured on "
                          "sun_glint: the sun region is only about 20x the sky region, where a real "
                          "HDR sky puts the solar disc thousands of times above it. The sky fills "
                          "half the frame, so its TOTAL energy swamps the sun and the convolution "
                          "reads as a uniform veil -- the veil lifted the DARKS by 8.3/255 while "
                          "lifting the area around the sun by only 6.6. A positive threshold is "
                          "what isolates the point source, and it is why this ships at 1.0 rather "
                          "than at UE's default.\n\n"
                          "UNITS CAVEAT (P16): the buffers are pre-exposed, so this compares "
                          "against ADAPTED values and one number reads differently as the camera "
                          "adapts. The ghost and streak thresholds below were moved to absolute "
                          "units for exactly that reason; this one keeps UE's semantics for now.");
            dragF("Soft Knee", "softKnee", 0.5f, 0.01f, 0.05f, 4.0f, "%.2f");
            InspectorHelp("Slope of the ramp above the threshold; UE hardwire 0.5, which is the "
                          "default here. Lower = a longer shoulder, so highlights ease into the "
                          "bloom instead of switching on at a fixed brightness.");
            {
                bool firefly = tgt().value("fireflyClamp", true);
                const bool changed = ImGui::Checkbox("Firefly Clamp", &firefly);
                beginContinuousEdit(changed);
                if (changed) { tgt()["fireflyClamp"] = firefly; }
                trackContinuousEdit(changed);
            }
            InspectorHelp("Karis average on the FIRST downsample only: each tap is weighted by "
                          "1/(1+luma), so one blown-out texel is averaged DOWN instead of surviving "
                          "the whole pyramid. This is the setting that decides whether moving sun "
                          "glints on water sparkle or pump the entire frame, which is the specific "
                          "failure this step was warned about. Deeper levels do not use it - there "
                          "it would only eat energy the tent needs.");

            ImGui::SeparatorText("Reconstruction");
            dragF("Intensity", "intensity", 0.25f, 0.02f, 0.0f, 64.0f, "%.3f");
            InspectorHelp("Weight of the bloom added back. It receives the same GLOBAL exposure "
                          "the scene does, but not the local exposure and not the colour grade -- a "
                          "halo that spread from elsewhere is not part of this pixel's "
                          "neighbourhood. 0 is an exact no-op and skips the chain.\n\n"
                          "ON THE CONVOLUTION IT IS UE'S ScatterDispersionIntensity: the multiplier "
                          "on the fraction of the kernel's energy that SCATTERS. That fraction is a "
                          "property of the kernel image (7.5% for BloomKernelStar, 2.1% for UE's "
                          "own), so the same number is dimmer on a kernel that scatters less -- "
                          "which is the point, not a quirk. With Threshold below zero the scene is "
                          "dimmed by whatever the flare takes, and the knob saturates once ALL of "
                          "the light has moved into the flare.");
            dragF("Radius", "radius", 1.0f, 0.01f, 0.0f, 4.0f, "%.2f");
            InspectorHelp("Tap spacing of the tent upsample, in DESTINATION texels, so it means the "
                          "same thing at every level and at every resolution. It spreads the same "
                          "energy wider rather than adding any: widen it and the halo gets larger "
                          "and fainter, it does not get brighter.");

            ImGui::SeparatorText("Method");
            {
                int method = static_cast<int>(tgt().value("method", 0u));
                const char* kMethods[] = { "Standard (pyramid)", "Convolution (FFT)" };
                const bool changed = ImGui::Combo("Method", &method, kMethods, 2);
                beginContinuousEdit(changed);
                if (changed) { tgt()["method"] = static_cast<std::uint32_t>(method < 0 ? 0 : method); }
                trackContinuousEdit(changed);
            }
            InspectorHelp("STANDARD is the mip pyramid: cheap, symmetric, and structurally incapable "
                          "of a streak -- a stack of Gaussians has no shape to give.\n\n"
                          "CONVOLUTION replaces every bright pixel with a copy of an aperture image, "
                          "via two Fourier transforms and one complex multiply. Halo, starburst and "
                          "the rays from the iris blades all come out of that ONE kernel and are "
                          "consistent with each other by construction, instead of being three "
                          "effects tuned into agreement. It costs more and runs at Resolution % "
                          "of the display (50 by default); a scene with no bright highlights "
                          "cannot tell them apart, which is why Standard is the default.");

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
                InspectorHelp("A colour multiplier on the kernel IMAGE, the runtime equivalent of "
                              "re-authoring the photograph. It survives the normalisation: the DC "
                              "divide is by the LARGEST channel sum, exactly so a kernel's own "
                              "colour balance is not washed out -- so tinting the kernel tints the "
                              "glare rather than being divided back out.\n\n"
                              "It costs nothing per frame: the kernel spectrum is rebuilt only when "
                              "its key moves, and the tint is part of that key.");

                ImGui::SeparatorText("Bright-pixel gain (UE's prefilter)");
                dragF("Gain Slope", "convPreFilterMult", 0.0f, 0.05f, 0.0f, 32.0f, "%.2f");
                InspectorHelp("UE's BloomConvolutionPreFilterMult, and the ONLY filtering their FFT "
                              "bloom has -- there is no threshold anywhere in that path.\n\n"
                              "0 turns it off and the Threshold above takes over. Anything above 0 "
                              "REPLACES the threshold: nothing is cut, a pixel below Min passes "
                              "through untouched, and a pixel above Min has its luminance remapped "
                              "with this slope. That is the difference that matters -- a threshold "
                              "decides MEMBERSHIP, so every source in the frame moves together and "
                              "one number cannot serve two shots; measured on this level, at 5 both "
                              "a beach sun and a palm grove bloomed and at 6 both died.\n\n"
                              "Pair it with Threshold below zero, which is how UE ship it: no "
                              "threshold, this gain, and the centre/scatter split for the energy.");
                dragF("Gain Min", "convPreFilterMin", 1.5f, 0.05f, 0.0f, 64.0f, "%.2f");
                InspectorHelp("Where the boost starts, in the same ABSOLUTE units as every other "
                              "threshold here: brightness at EV100 = 14, so it means the same thing "
                              "from any viewpoint. Below it, pixels are left exactly as they are.");
                dragF("Gain Max", "convPreFilterMax", 6.0f, 0.25f, 0.0f, 512.0f, "%.1f");
                InspectorHelp("The CEILING the boosted luminance is clamped to -- the reason one "
                              "setting can serve an open sun and a grove full of bright gaps. A "
                              "threshold has no such thing: it can only include or exclude, so a "
                              "frame with a thousand medium-bright sources runs away. This caps "
                              "what any single source can contribute, however bright it gets.");
                dragF("Resolution %", "convPercent", 50.0f, 0.5f, 10.0f, 50.0f, "%.0f %%");
                InspectorHelp("Resolution of the convolution as a percent of the display -- UE's "
                              "r.Bloom.ScreenPercentage (their default is 100; 50 is this "
                              "engine's grid ceiling). The first P8C ran at 12.5, and a 1-2 texel "
                              "diffraction ray upscaled 4x per axis is a dashed line of squares "
                              "-- the 'ragged crown'. Lowering this buys the transform cost back.");
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
            dragF("Anamorphic Threshold", "convAnamorphicThreshold", 1.5f, 0.05f, 0.1f, 24.0f, "%.2f");
            InspectorHelp("The streak's OWN threshold, and since P8C-2h the ONLY narrowing "
                              "control -- the vertical erosion that used to sit below was deleted "
                              "with the cascade (a min-filter cannot taper at a frame edge). In "
                              "ABSOLUTE units: authored as stored "
                          "brightness at EV100 = 14 and rescaled by the frame's pre-exposure, "
                          "so the same sun crosses it from any viewpoint -- being a light "
                          "source is a property of the scene, not the camera. On this level: "
                          "sky ~1, sun corona 4-6, sun core 10-12, glints above that. This is "
                          "also the narrowness control that actually works: higher values "
                          "take only the CORE of a source, so the corona stops throwing a "
                          "screen-tall band.");
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
            dragF("Ghost Intensity", "convGhostIntensity", 0.6f, 0.01f, 0.0f, 3.0f, "%.2f");
            InspectorHelp("Brightness of the chain. The colour comes from the sources "
                          "themselves, so a dimmer sun throws dimmer ghosts and a sky with "
                          "nothing above the threshold throws none at all.");
            dragF("Ghost Threshold", "convGhostThreshold", 7.0f, 0.05f, 0.0f, 24.0f, "%.2f");
            InspectorHelp("UE's LensFlareThreshold, in the same ABSOLUTE units as the streak "
                          "threshold above (stored brightness at EV100 = 14, rescaled by the "
                          "frame's pre-exposure). Ghosts are images of SOURCES -- too low and "
                          "sunlit foliage becomes one (upside-down palms in the sky, "
                          "observed).\n\n"
                          "SOFT KNEE, unlike UE's binary gate: a source's ghost is scaled by "
                          "how far it sits ABOVE this, so one at the threshold contributes "
                          "nothing and one ten times over it 90%. That is what stops a chain "
                          "popping into existence as the sun brightens -- and it means this "
                          "number wants to be LOWER than a binary gate's: 7 here matches what "
                          "10 gave before the knee. Raising it also cuts the scatter's cost "
                          "directly: collapsed quads rasterize nothing.");

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
                "is renormalised to unit luminance -- so this changes HUE ONLY. Without that, 3000K "
                "would arrive about a stop darker than 6500K and you would have to chase every "
                "temperature change with an intensity change.\n\n"
                "Valid 1000-15000K; outside that the fit bends back on itself, so it is clamped.");

            // P16.2. The default handed to the drag walks the same chain the loader does --
            // `sunIntensity` if this level went through P4, otherwise the original `exposure` --
            // so the row opens showing what is on screen and the first drag writes the new key
            // without the image jumping.
            const float legacySun = JsonFloat(tgt(), "sunIntensity",
                                              JsonFloat(tgt(), "exposure", 1.0f));
            dragF("Sun Illuminance (lux)", "sunIlluminanceLux", legacySun, 1.0f, 0.0f, 200000.0f,
                  "%.0f", ImGuiSliderFlags_Logarithmic);
            InspectorHelp(
                "How bright the sun is, in LUX, measured perpendicular to the beam. This is NOT a "
                "camera control -- the camera lives on the Camera Exposure object and meters the "
                "frame by itself.\n\n"
                "100000 sunny midday (the default), 125000 full bright sun, 20000 heavy overcast, "
                "1000 a very dark day, 400 sunrise/sunset, 0.25 full moon.\n\n"
                "THE NUMBER DID NOT CHANGE WHEN THIS FIELD GOT ITS UNIT. The engine's linear light "
                "unit was already photometric -- the shading carries the 1/PI and the metering "
                "solves an EV100, which is defined against cd/m2 -- so a level that authored "
                "'intensity 2' had been authoring two lux all along. If this row opens on a number "
                "like that, the level is not mis-migrated: it is telling you its sun is authored at "
                "twilight strength, which is exactly the defect P16 is working through.\n\n"
                "Physical values need the photographic camera ON. With it off the exposure "
                "multiplier is exactly 1.0 and a five-figure sun writes a white screen.");
            dragF("Ambient", "ambient", 0.05f, 0.005f, 0.0f, 10.0f);
            InspectorHelp("Sky fill intensity -- the light everything gets from the sky rather than "
                          "from the sun. Independent of Sun Intensity.");

            dragF("Sky Fill Intensity", "skyFillIntensity", 1.0f, 0.01f, 0.0f, 4.0f, "%.3f");
            InspectorHelp(
                "How much of the SKY'S OWN measured irradiance reaches diffuse surfaces. Only "
                "active when this level's sky was imported with its IBL derivatives -- check "
                "the session log (F3, the [ibl] line) if you are not sure which path a level took.\n\n"
                "1 = the irradiance cube at face value, which is the physical answer. It is a "
                "separate control from Ambient above on purpose: Ambient means 'this fraction of "
                "the SUN colour bounces around', a number authored against a different equation "
                "entirely, and reusing it here would bury the fill about twenty times too deep.\n\n"
                "Ambient still drives the flat fallback fill on levels whose sky has no "
                "derivatives.");

            {
                const Math::float3 g = JsonFloat3(tgt(), "groundAlbedo",
                                                  Math::float3(0.25f, 0.25f, 0.25f));
                float gv[3] = { g.x, g.y, g.z };
                const bool changed = ImGui::ColorEdit3("Ground Bounce Albedo", gv);
                beginContinuousEdit(changed);
                if (changed) { tgt()["groundAlbedo"] = { gv[0], gv[1], gv[2] }; }
                trackContinuousEdit(changed);
            }
            InspectorHelp(
                "P16.12. The DIFFUSE REFLECTANCE OF THE GROUND -- the light that comes back UP off "
                "the floor and fills everything facing downward. Sky Fill above answers what "
                "arrives from the sky; nothing answered this, because the irradiance cube's lower "
                "hemisphere carries the HDRI's own ground, not the ground this scene is standing "
                "on. Measured on wind_test: about 1.2 stops missing on shaded vertical surfaces "
                "over sunlit sand, which is what makes a bright day look like it has dead shadows."
                "\n\n"
                "It is a REFLECTANCE, not a light: it only ever scales illuminance the scene "
                "already has, so it follows the sun automatically and cannot brighten a night. "
                "Black switches the whole term off. Physical values: dry sand 0.4, dead grass 0.3, "
                "concrete 0.25, green grass 0.2, asphalt 0.1, water 0.06. TINT IT toward the ground "
                "you actually have -- warm sand throwing warm light into the shadows is half of "
                "what makes the effect read.\n\n"
                "Applies to indirect DIFFUSE only, and it is occluded by the same AO the sky fill "
                "is, so it does not light the inside of a closed room. FREE in GPU cost.");

            if (tgt().contains("sunIlluminanceLux") &&
                (tgt().contains("sunIntensity") || tgt().contains("exposure")))
            {
                ImGui::TextDisabled("legacy 'sunIntensity'/'exposure' present but ignored");
                InspectorHelp("This object carries the new Sun Illuminance, so the older "
                              "`sunIntensity` and whole-scene `exposure` fields no longer do "
                              "anything. Delete them from the level JSON whenever you next "
                              "hand-edit the file.");
            }

            Math::float3 rayDirection = EditorLightDirection::NormalizedRay(
                JsonFloat3(tgt(), "direction", Math::float3(-1.0f, -1.0f, -1.0f)));
            float sourceAzimuth = 0.0f;
            float sourceElevation = 0.0f;
            EditorLightDirection::SourceAngles(
                rayDirection, sourceAzimuth, sourceElevation);

            {
                const bool changed = ImGui::DragFloat("Source azimuth (Y)",
                    &sourceAzimuth, 0.5f, -180.0f, 180.0f, "%.1f deg",
                    ImGuiSliderFlags_AlwaysClamp);
                beginContinuousEdit(changed);
                if (changed)
                {
                    rayDirection = EditorLightDirection::RayFromSourceAngles(
                        sourceAzimuth, sourceElevation);
                    tgt()["direction"] = {
                        rayDirection.x, rayDirection.y, rayDirection.z };
                }
                trackContinuousEdit(changed);
            }
            {
                const bool changed = ImGui::DragFloat("Source elevation",
                    &sourceElevation, 0.5f, -89.0f, 89.0f, "%.1f deg",
                    ImGuiSliderFlags_AlwaysClamp);
                beginContinuousEdit(changed);
                if (changed)
                {
                    rayDirection = EditorLightDirection::RayFromSourceAngles(
                        sourceAzimuth, sourceElevation);
                    tgt()["direction"] = {
                        rayDirection.x, rayDirection.y, rayDirection.z };
                }
                trackContinuousEdit(changed);
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
            drawCameraExposure();
        }
        else if (env.type == "gtao")
        {
            drawGtao();
        }
        else if (env.type == "atmosphere")
        {
            drawAtmosphere();
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
                { "atmosphere",     "Aerial Perspective" },
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
                    else if (std::string_view(section.key) == "atmosphere") { drawAtmosphere(); }
                    else { drawBloom(); }
                    groupKey.clear();
                    ImGui::PopID();
                }
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
                "Check the session log's [ibl] line (F3) for the measured value and the factor it produced.\n\n"
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
                }

                if (ImGui::CollapsingHeader("Specular and reflection", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    renderDrag("Specular Strength", render.specularStrength, 0.01f, 0.0f, 10.0f);
                    renderDrag("Roughness Scale", render.roughnessScale, 0.005f, 0.0f, 1.0f);
                    renderDrag("Roughness Distance", render.roughnessDistance, 1.0f, 1.0f, 5000.0f, "%.1f");
                    renderDrag("Reflection Normal Flattening", render.reflectionNormalStrength, 0.005f, 0.0f, 1.0f);
                    renderDrag("Sky Reflection Horizon Pull", render.reflectionSkyHorizonPull, 0.005f, 0.05f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Compresses the SKY reflection ray toward the horizon (1 = off).\n"
                                          "A clear-sky HDRI is much darker at the zenith, and a wave's facets\n"
                                          "swing the reflected ray between horizon and zenith - at grazing angles\n"
                                          "Fresnel passes that contrast through at full strength, so it reads as\n"
                                          "hard dark streaks along the crests. Lower values keep the water in the\n"
                                          "bright band near the horizon, which is also where real water reflects\n"
                                          "from at these angles. ~0.4-0.6 is the useful range. Affects the ocean's\n"
                                          "environment sample only: planar reflection, land IBL and sky untouched.");
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
