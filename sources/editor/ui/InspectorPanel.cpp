#include "editor/ui/InspectorPanel.h"
#if WITH_EDITOR

#include <cstdio>
#include <memory>
#include <string>

#include "app/scene/Scene.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/SetEnabledCommand.h"
#include "editor/commands/SetMaterialCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "rendering/RenderLayers.h"
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
