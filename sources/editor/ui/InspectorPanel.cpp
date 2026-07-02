#include "editor/ui/InspectorPanel.h"
#if WITH_EDITOR

#include <cstdio>
#include <memory>
#include <string>

#include "app/scene/Scene.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/scene/EnvironmentRuntime.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/SetEnabledCommand.h"
#include "editor/commands/SetMaterialCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "app/camera/Camera.h"
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

    struct LayerOption { const char* name; RenderLayer layer; };
    constexpr LayerOption kLayers[] = {
        { "Default", RenderLayer::Default },
        { "Terrain", RenderLayer::Terrain },
        { "Transparent", RenderLayer::Transparent },
        { "Sky", RenderLayer::Sky },
        { "Lights", RenderLayer::Lights },
        { "Gizmo", RenderLayer::Gizmo },
        { "Debug", RenderLayer::Debug },
    };

    void DrawStaticMeshSetup(EditorContext& ctx, EditorCommandStack& commandStack,
        const AssetRegistry& registry, EditorObject& obj)
    {
        // Material preset: changing it respawns the runtime (handled by the command).
        const std::string current = obj.properties.value("material", std::string());
        if (ImGui::BeginCombo("Material", current.empty() ? "(none)" : current.c_str()))
        {
            for (const EditorAssetRecord& rec : registry.Assets())
            {
                if (rec.id.type != EditorAssetType::MaterialPreset) { continue; }
                const bool selected = (rec.id.key == current);
                if (ImGui::Selectable(rec.id.key.c_str(), selected) && !selected)
                {
                    commandStack.Execute(ctx, std::make_unique<SetMaterialCommand>(obj.id, rec.id.key));
                }
            }
            ImGui::EndCombo();
        }

        // Re-fetch after the combo: a material change above may have respawned it.
        RenderableObjectBase* runtime = ctx.scene.FindEditorObject(obj.id.value);

        // Render layer.
        const std::string currentLayer = obj.properties.value("renderLayer", std::string("Default"));
        if (ImGui::BeginCombo("Render Layer", currentLayer.c_str()))
        {
            for (const LayerOption& opt : kLayers)
            {
                const bool selected = (currentLayer == opt.name);
                if (ImGui::Selectable(opt.name, selected) && !selected)
                {
                    obj.properties["renderLayer"] = opt.name;
                    if (runtime) { runtime->SetRenderLayer(opt.layer); }
                    ctx.document.SetDirty(true);
                }
            }
            ImGui::EndCombo();
        }

        // Material params live on the runtime (demo objects have none).
        GBufferRenderable* gb = runtime ? runtime->AsGBufferRenderable() : nullptr;
        if (!gb)
        {
            ImGui::TextDisabled("Material params editable on spawned objects.");
            return;
        }
        MaterialParams& mp = gb->MaterialParamsRef();

        float texOffsScale[4] = { mp.texOffsScale.x, mp.texOffsScale.y, mp.texOffsScale.z, mp.texOffsScale.w };
        if (ImGui::DragFloat4("Tex Offset/Scale", texOffsScale, 0.01f))
        {
            mp.texOffsScale = Math::float4(texOffsScale[0], texOffsScale[1], texOffsScale[2], texOffsScale[3]);
            obj.properties["texOffsScale"] = { texOffsScale[0], texOffsScale[1], texOffsScale[2], texOffsScale[3] };
            ctx.document.SetDirty(true);
        }

        float normalStrength = mp.texFlags.w;
        if (ImGui::DragFloat("Normal Strength", &normalStrength, 0.01f, 0.0f, 10.0f))
        {
            mp.SetNormalStrength(normalStrength);
            obj.properties["normalStrength"] = normalStrength;
            ctx.document.SetDirty(true);
        }

        bool useMR = mp.texFlags.y > 0.5f;
        if (ImGui::Checkbox("Use MR Texture", &useMR))
        {
            mp.SetUseMR(useMR);
            obj.properties["useMR"] = useMR;
            ctx.document.SetDirty(true);
        }

        float metalRough[2] = { mp.metalRough.x, mp.metalRough.y };
        if (ImGui::DragFloat2("Metallic / Roughness", metalRough, 0.01f, 0.0f, 1.0f))
        {
            mp.metalRough = Math::float2(metalRough[0], metalRough[1]);
            obj.properties["metalRough"] = { metalRough[0], metalRough[1] };
            ctx.document.SetDirty(true);
        }
    }

    void DrawTransparentMeshSetup(EditorContext& ctx, EditorObject& obj)
    {
        RenderableObjectBase* runtime = ctx.scene.FindEditorObject(obj.id.value);
        TransparentStaticMesh* glass = runtime ? runtime->AsTransparentStaticMesh() : nullptr;

        const Math::float3 tint = JsonFloat3(obj.properties, "tint", Math::float3(0.85f, 0.93f, 1.0f));
        float tintv[3] = { tint.x, tint.y, tint.z };
        if (ImGui::ColorEdit3("Tint", tintv))
        {
            obj.properties["tint"] = { tintv[0], tintv[1], tintv[2] };
            if (glass) { glass->SetTint(Math::float3(tintv[0], tintv[1], tintv[2])); }
            ctx.document.SetDirty(true);
        }

        const Math::float3 absorption = JsonFloat3(obj.properties, "absorption", Math::float3(0.25f, 0.08f, 0.04f));
        float absv[3] = { absorption.x, absorption.y, absorption.z };
        if (ImGui::DragFloat3("Absorption", absv, 0.01f, 0.0f, 10.0f))
        {
            obj.properties["absorption"] = { absv[0], absv[1], absv[2] };
            if (glass) { glass->SetAbsorption(Math::float3(absv[0], absv[1], absv[2])); }
            ctx.document.SetDirty(true);
        }

        float thickness = JsonFloat(obj.properties, "thickness", 0.6f);
        if (ImGui::DragFloat("Thickness", &thickness, 0.01f, 0.0f, 100.0f))
        {
            obj.properties["thickness"] = thickness;
            if (glass) { glass->SetThickness(thickness); }
            ctx.document.SetDirty(true);
        }

        float reflection = JsonFloat(obj.properties, "reflectionStrength", 1.0f);
        if (ImGui::DragFloat("Reflection Strength", &reflection, 0.01f, 0.0f, 10.0f))
        {
            obj.properties["reflectionStrength"] = reflection;
            if (glass) { glass->SetReflectionStrength(reflection); }
            ctx.document.SetDirty(true);
        }

        float distortion = JsonFloat(obj.properties, "refractionDistortion", 0.015f);
        if (ImGui::DragFloat("Refraction Distortion", &distortion, 0.001f, 0.0f, 10.0f))
        {
            obj.properties["refractionDistortion"] = distortion;
            if (glass) { glass->SetRefractionDistortion(distortion); }
            ctx.document.SetDirty(true);
        }

        float roughness = JsonFloat(obj.properties, "roughness", 0.07f);
        if (ImGui::DragFloat("Roughness", &roughness, 0.01f, 0.0f, 1.0f))
        {
            obj.properties["roughness"] = roughness;
            if (glass) { glass->SetRoughness(roughness); }
            ctx.document.SetDirty(true);
        }

        float ior = JsonFloat(obj.properties, "ior", 1.52f);
        if (ImGui::DragFloat("IOR", &ior, 0.01f, 1.0f, 3.0f))
        {
            obj.properties["ior"] = ior;
            if (glass) { glass->SetIor(ior); }
            ctx.document.SetDirty(true);
        }

        const std::string normalMap = obj.properties.value("normalMap", std::string());
        ImGui::Text("Normal Map: %s", normalMap.empty() ? "(none)" : normalMap.c_str());
    }

    // Inspector for the top-level environment sections (Step 24). Edits write the
    // entity's `properties` (round-tripped on save via the entity-driven
    // serializer) and patch the live runtime; lights/camera update instantly,
    // skybox/ocean apply on the next level load.
    void DrawEnvironmentInspector(EditorContext& ctx, EditorObject& env)
    {
        nlohmann::json& p = env.properties;

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

        bool changed = false;
        if (env.type == "pointLight")
        {
            changed |= colorEdit();
            changed |= dragF("Intensity", "intensity", 1.0f, 0.1f, 0.0f, 1000.0f);
            changed |= dragF("Radius", "radius", 1.0f, 0.05f, 0.0f, 1000.0f);
            changed |= dragF3("Position", "position", Math::float3(0.0f, 0.0f, 0.0f), 0.05f);
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
            char buf[260];
            std::snprintf(buf, sizeof(buf), "%s", p.value("texture", std::string()).c_str());
            if (ImGui::InputText("Texture", buf, sizeof(buf))) { p["texture"] = std::string(buf); ctx.document.SetDirty(true); }
            ImGui::TextDisabled("Applies on reload.");
        }
        else if (env.type == "ocean")
        {
            char buf[260];
            std::snprintf(buf, sizeof(buf), "%s", p.value("preset", std::string()).c_str());
            if (ImGui::InputText("Preset", buf, sizeof(buf))) { p["preset"] = std::string(buf); ctx.document.SetDirty(true); }
            ImGui::TextDisabled("Applies on reload.");
        }
        else
        {
            ImGui::TextDisabled("No editable properties.");
        }
    }
}

void InspectorPanel::Draw(EditorContext& ctx, EditorCommandStack& commandStack, const AssetRegistry& registry, bool* open)
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
            DrawEnvironmentInspector(ctx, env);
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

    if (obj->type == "staticMesh")
    {
        ImGui::Separator();
        DrawStaticMeshSetup(ctx, commandStack, registry, *obj);
    }
    else if (obj->type == "transparentMesh")
    {
        ImGui::Separator();
        DrawTransparentMeshSetup(ctx, *obj);
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
