#include "editor/ui/InspectorPanel.h"
#if WITH_EDITOR

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "app/scene/Scene.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/EditorExtensionRegistry.h"
#include "editor/scene/EnvironmentRuntime.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/SetEnabledCommand.h"
#include "editor/commands/SetMaterialCommand.h"
#include "editor/commands/TransformObjectCommand.h"
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

    // Inspector for the top-level environment sections (Step 24). Edits write the
    // entity's `properties` (round-tripped on save via the entity-driven
    // serializer) and patch the live runtime; lights/camera update instantly,
    // skybox texture edits rebuild the live skybox.
    void DrawEnvironmentInspector(EditorContext& ctx, const AssetRegistry& registry, EditorObject& env)
    {
        nlohmann::json& p = env.properties;

        // Enable toggle for lights + ocean. A disabled entity does not render (a
        // disabled light also frees its runtime slot); the entity still persists
        // in the document so it saves and can be re-enabled. Applied live.
        const bool supportsEnable =
            env.type == "pointLight" || env.type == "spotLight" ||
            env.type == "directionalLight" || env.type == "ocean";
        if (supportsEnable)
        {
            bool enabled = p.value("enabled", true);
            if (ImGui::Checkbox("Enabled", &enabled))
            {
                EnvironmentRuntime::SetEnabled(ctx, env, enabled);
            }
            ImGui::Separator();
        }

        auto colorEdit = [&]()
        {
            const Math::float3 c = JsonFloat3(p, "color", Math::float3(1.0f, 1.0f, 1.0f));
            float cv[3] = { c.x, c.y, c.z };
            if (ImGui::ColorEdit3("Color", cv)) { p["color"] = { cv[0], cv[1], cv[2] }; return true; }
            return false;
        };
        auto dragF = [&](const char* label, const char* key, float def, float speed, float lo, float hi)
        {
            float v = JsonFloat(p, key, def);
            if (ImGui::DragFloat(label, &v, speed, lo, hi)) { p[key] = v; return true; }
            return false;
        };
        auto dragF3 = [&](const char* label, const char* key, const Math::float3& def, float speed)
        {
            const Math::float3 d3 = JsonFloat3(p, key, def);
            float v[3] = { d3.x, d3.y, d3.z };
            if (ImGui::DragFloat3(label, v, speed)) { p[key] = { v[0], v[1], v[2] }; return true; }
            return false;
        };
        auto checkB = [&](const char* label, const char* key, bool def)
        {
            bool v = p.value(key, def);
            if (ImGui::Checkbox(label, &v)) { p[key] = v; return true; }
            return false;
        };

        bool changed = false;
        if (env.type == "pointLight")
        {
            changed |= colorEdit();
            changed |= dragF("Intensity", "intensity", 1.0f, 0.1f, 0.0f, 1000.0f);
            changed |= dragF("Radius", "radius", 1.0f, 0.05f, 0.0f, 1000.0f);
            changed |= dragF3("Position", "position", Math::float3(0.0f, 0.0f, 0.0f), 0.05f);
            changed |= checkB("Cast Shadows", "shadowsEnabled", false);
            if (changed) { EnvironmentRuntime::Apply(ctx, env); ctx.document.SetDirty(true); }
        }
        else if (env.type == "spotLight")
        {
            changed |= colorEdit();
            changed |= dragF("Intensity", "intensity", 5.0f, 0.1f, 0.0f, 1000.0f);
            changed |= dragF("Range", "range", 10.0f, 0.1f, 0.0f, 10000.0f);
            changed |= dragF("Inner Angle (deg)", "innerAngleDeg", 15.0f, 0.2f, 0.0f, 89.0f);
            changed |= dragF("Outer Angle (deg)", "outerAngleDeg", 25.0f, 0.2f, 0.0f, 89.0f);
            changed |= dragF3("Position", "position", Math::float3(0.0f, 0.0f, 0.0f), 0.05f);
            changed |= dragF3("Direction", "direction", Math::float3(0.0f, -1.0f, 0.0f), 0.01f);
            changed |= checkB("Cast Shadows", "shadowsEnabled", false);
            changed |= dragF("Shadow Normal Bias", "shadowNormalBias", 0.05f, 0.001f, 0.0f, 10.0f);
            changed |= dragF("Shadow Depth Bias", "shadowDepthBias", 0.0001f, 0.00005f, 0.0f, 1.0f);
            if (changed) { EnvironmentRuntime::Apply(ctx, env); ctx.document.SetDirty(true); }
        }
        else if (env.type == "directionalLight")
        {
            changed |= colorEdit();
            changed |= dragF("Exposure", "exposure", 1.0f, 0.05f, 0.0f, 100.0f);
            changed |= dragF("Ambient", "ambient", 0.05f, 0.005f, 0.0f, 10.0f);
            changed |= dragF3("Direction", "direction", Math::float3(-1.0f, -1.0f, -1.0f), 0.01f);
            if (changed) { EnvironmentRuntime::Apply(ctx, env); ctx.document.SetDirty(true); }
        }
        else if (env.type == "camera")
        {
            const bool camChanged =
                dragF("H FOV (deg)", "hfovDeg", 90.0f, 0.5f, 1.0f, 179.0f) |
                dragF("Z Near", "zNear", 0.01f, 0.001f, 0.0001f, 100.0f) |
                dragF("Z Far", "zFar", 10000.0f, 1.0f, 0.1f, 1000000.0f);
            if (camChanged)
            {
                EnvironmentRuntime::Apply(ctx, env);
                ctx.document.SetDirty(true);
            }
        }
        else if (env.type == "skybox")
        {
            const std::string current = p.value("texture", std::string());
            const std::string currentLabel = current.empty()
                ? std::string("(none)")
                : std::filesystem::path(current).filename().string();

            if (ImGui::BeginCombo("Texture Asset", currentLabel.c_str()))
            {
                for (const EditorAssetRecord& rec : registry.Assets())
                {
                    if (rec.id.type != EditorAssetType::Texture || rec.extension != ".dds")
                    {
                        continue;
                    }

                    const bool selected = rec.id.key == current;
                    if (ImGui::Selectable(rec.id.key.c_str(), selected) && !selected)
                    {
                        p["texture"] = NormalizePath(rec.id.key);
                        EnvironmentRuntime::Apply(ctx, env);
                        ctx.document.SetDirty(true);
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s", p.value("texture", std::string()).c_str());
            ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 72.0f));
            if (ImGui::InputText("Texture", buf, sizeof(buf)))
            {
                p["texture"] = NormalizePath(buf);
                ctx.document.SetDirty(true);
            }

            bool applyTexture = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine();
            applyTexture |= ImGui::Button("Apply");
            if (applyTexture)
            {
                EnvironmentRuntime::Apply(ctx, env);
                ctx.document.SetDirty(true);
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
                            p["preset"] = pr;
                            ocean->LoadConfig(&ctx.renderer, std::wstring(pr.begin(), pr.end()));
                            ctx.document.SetDirty(true);
                        }
                    }
                    ImGui::EndCombo();
                }

                // Inline wind overrides (live). Show the sim's current values.
                float windForce = ocean->GetWindForce01();
                float windDir = ocean->GetLocalWindDirectionDegrees();
                float swellDir = ocean->GetSwellDirectionDegrees();
                bool windChanged = false;
                windChanged |= ImGui::SliderFloat("Wind Force", &windForce, 0.0f, 1.0f);
                windChanged |= ImGui::DragFloat("Wind Direction", &windDir, 0.5f, -360.0f, 360.0f, "%.1f deg");
                windChanged |= ImGui::DragFloat("Swell Direction", &swellDir, 0.5f, -360.0f, 360.0f, "%.1f deg");
                if (windChanged)
                {
                    p["windForce"] = windForce;
                    p["windDirectionDeg"] = windDir;
                    p["swellDirectionDeg"] = swellDir;
                    EnvironmentRuntime::Apply(ctx, env);
                    ctx.document.SetDirty(true);
                }
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
    ImGui::SetNextWindowSize(ImVec2(340.0f, 460.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector", open))
    {
        ImGui::End();
        return;
    }

    EditorObject* obj = ctx.document.Find(ctx.selectedObject);
    if (!obj)
    {
        // Environment entities (camera/lights/skybox/ocean) live in a separate list.
        for (EditorObject& env : ctx.document.Environment())
        {
            if (env.id.value != ctx.selectedObject.value) { continue; }
            ImGui::Text("ID: %llu", static_cast<unsigned long long>(env.id.value));
            ImGui::Text("Type: %s", env.type.c_str());
            ImGui::TextUnformatted(env.name.c_str());
            ImGui::Separator();
            DrawEnvironmentInspector(ctx, registry, env);
            ImGui::End();
            return;
        }

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

    if (const IEditorPropertyDrawer* drawer = extensions.FindPropertyDrawer(obj->type))
    {
        ImGui::Separator();
        drawer->Draw(ctx, commandStack, registry, *obj);
    }

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
