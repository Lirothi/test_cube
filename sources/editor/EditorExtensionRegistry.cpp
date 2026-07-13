#include "editor/EditorExtensionRegistry.h"
#if WITH_EDITOR

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "app/camera/Camera.h"
#include "app/scene/Scene.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/SetMaterialCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "imgui.h"
#include "rendering/RenderLayers.h"
#include "rendering/renderables/GBufferRenderable.h"
#include "rendering/renderables/RenderableObjectBase.h"
#include "rendering/renderables/TransparentStaticMesh.h"

namespace
{
    nlohmann::json SpawnPositionJson(const Scene& scene, const Math::float3* worldPositionHint)
    {
        if (worldPositionHint)
        {
            return nlohmann::json::array({
                worldPositionHint->x,
                worldPositionHint->y,
                worldPositionHint->z });
        }

        const Math::float3& camPos = scene.CameraRef().GetPosition();
        const Math::float3& camDir = scene.CameraRef().GetDirection();
        return nlohmann::json::array({
            camPos.x + camDir.x * 5.0f,
            camPos.y + camDir.y * 5.0f,
            camPos.z + camDir.z * 5.0f });
    }

    std::string PickDefaultStaticMaterial(const AssetRegistry& registry)
    {
        const EditorAssetRecord* first = nullptr;
        for (const EditorAssetRecord& rec : registry.Assets())
        {
            if (rec.id.type != EditorAssetType::MaterialPreset)
            {
                continue;
            }
            if (rec.id.key == "damaged_plaster")
            {
                return "damaged_plaster";
            }
            if (!first)
            {
                first = &rec;
            }
        }
        return first ? first->id.key : std::string{};
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

    class StaticMeshObjectFactory final : public IEditorObjectFactory
    {
    public:
        std::string_view Type() const override { return "staticMesh"; }
        std::string_view MenuLabel() const override { return "Spawn Static Mesh"; }

        bool CanBuildFromAsset(const EditorAssetRecord* sourceAsset) const override
        {
            return sourceAsset && sourceAsset->id.type == EditorAssetType::Mesh;
        }

        nlohmann::json BuildDefaultJson(const EditorAssetRecord* sourceAsset,
            const EditorContext& ctx,
            const AssetRegistry& registry,
            const Math::float3* worldPositionHint) const override
        {
            nlohmann::json o = nlohmann::json::object();
            o["type"] = std::string(Type());
            o["model"] = sourceAsset ? sourceAsset->id.key : std::string{};
            o["position"] = SpawnPositionJson(ctx.scene, worldPositionHint);
            o["scale"] = nlohmann::json::array({ 1.0f, 1.0f, 1.0f });
            o["material"] = PickDefaultStaticMaterial(registry);
            o["shader"] = "shaders/gbuffer.hlsl";
            o["inputLayout"] = "PosNormTanUV";
            return o;
        }
    };

    class TransparentMeshObjectFactory final : public IEditorObjectFactory
    {
    public:
        std::string_view Type() const override { return "transparentMesh"; }
        std::string_view MenuLabel() const override { return "Spawn Transparent Mesh"; }

        bool CanBuildFromAsset(const EditorAssetRecord* sourceAsset) const override
        {
            return sourceAsset && sourceAsset->id.type == EditorAssetType::Mesh;
        }

        nlohmann::json BuildDefaultJson(const EditorAssetRecord* sourceAsset,
            const EditorContext& ctx,
            const AssetRegistry& registry,
            const Math::float3* worldPositionHint) const override
        {
            (void)registry;

            nlohmann::json o = nlohmann::json::object();
            o["type"] = std::string(Type());
            o["model"] = sourceAsset ? sourceAsset->id.key : std::string{};
            o["position"] = SpawnPositionJson(ctx.scene, worldPositionHint);
            o["scale"] = nlohmann::json::array({ 1.0f, 1.0f, 1.0f });

            // Default glass params: copy from an existing transparentMesh in the
            // document if one exists, keeping our own model and transform.
            for (const EditorObject& existing : ctx.document.Objects())
            {
                if (existing.type == "transparentMesh")
                {
                    for (auto it = existing.properties.begin(); it != existing.properties.end(); ++it)
                    {
                        if (it.key() != "model" &&
                            it.key() != "position" &&
                            it.key() != "rotationDeg" &&
                            it.key() != "scale")
                        {
                            o[it.key()] = it.value();
                        }
                    }
                    break;
                }
            }

            return o;
        }
    };

    class StaticMeshPropertyDrawer final : public IEditorPropertyDrawer
    {
    public:
        std::string_view Type() const override { return "staticMesh"; }

        void Draw(EditorContext& ctx,
            EditorCommandStack& commandStack,
            const AssetRegistry& registry,
            EditorObject& obj) const override
        {
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

            RenderableObjectBase* runtime = ctx.scene.FindEditorObject(obj.id.value);

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
    };

    class TransparentMeshPropertyDrawer final : public IEditorPropertyDrawer
    {
    public:
        std::string_view Type() const override { return "transparentMesh"; }

        void Draw(EditorContext& ctx,
            EditorCommandStack& commandStack,
            const AssetRegistry& registry,
            EditorObject& obj) const override
        {
            (void)commandStack;
            (void)registry;

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
    };

    class FreeCameraStartPropertyDrawer final : public IEditorPropertyDrawer
    {
    public:
        std::string_view Type() const override { return "freeCameraStart"; }

        void Draw(EditorContext& ctx,
            EditorCommandStack& commandStack,
            const AssetRegistry& registry,
            EditorObject& obj) const override
        {
            (void)registry;

            if (ImGui::Button("Copy camera params"))
            {
                EditorTransform after = obj.transform;
                const Camera& camera = ctx.scene.CameraRef();
                after.position = camera.GetPosition();
                after.rotationDeg = Math::float3(
                    camera.GetPitch() * Math::RAD2DEG,
                    camera.GetYaw() * Math::RAD2DEG,
                    0.0f);
                commandStack.Execute(ctx, std::make_unique<TransformObjectCommand>(
                    obj.id, obj.transform, after));
            }
        }
    };
}

EditorLambdaPanel::EditorLambdaPanel(std::string id,
    std::string label,
    bool* visible,
    bool showInWindowList,
    DrawFn draw)
    : id_(std::move(id))
    , label_(std::move(label))
    , visible_(visible)
    , showInWindowList_(showInWindowList)
    , draw_(std::move(draw))
{
}

bool EditorLambdaPanel::IsVisible() const
{
    return visible_ ? *visible_ : true;
}

void EditorLambdaPanel::SetVisible(bool visible)
{
    if (visible_)
    {
        *visible_ = visible;
    }
}

void EditorLambdaPanel::Draw(EditorContext& ctx)
{
    if (draw_)
    {
        draw_(ctx);
    }
}

void EditorExtensionRegistry::RegisterPanel(std::unique_ptr<IEditorPanel> panel)
{
    if (panel)
    {
        panels_.push_back(std::move(panel));
    }
}

void EditorExtensionRegistry::RegisterObjectFactory(std::unique_ptr<IEditorObjectFactory> factory)
{
    if (factory)
    {
        objectFactories_.push_back(std::move(factory));
    }
}

void EditorExtensionRegistry::RegisterPropertyDrawer(std::unique_ptr<IEditorPropertyDrawer> drawer)
{
    if (drawer)
    {
        propertyDrawers_.push_back(std::move(drawer));
    }
}

IEditorPanel* EditorExtensionRegistry::FindPanel(std::string_view id)
{
    for (const std::unique_ptr<IEditorPanel>& panel : panels_)
    {
        if (panel && panel->Id() == id)
        {
            return panel.get();
        }
    }
    return nullptr;
}

const IEditorPanel* EditorExtensionRegistry::FindPanel(std::string_view id) const
{
    for (const std::unique_ptr<IEditorPanel>& panel : panels_)
    {
        if (panel && panel->Id() == id)
        {
            return panel.get();
        }
    }
    return nullptr;
}

const IEditorObjectFactory* EditorExtensionRegistry::FindObjectFactory(std::string_view type) const
{
    for (const std::unique_ptr<IEditorObjectFactory>& factory : objectFactories_)
    {
        if (factory && factory->Type() == type)
        {
            return factory.get();
        }
    }
    return nullptr;
}

const IEditorPropertyDrawer* EditorExtensionRegistry::FindPropertyDrawer(std::string_view type) const
{
    for (const std::unique_ptr<IEditorPropertyDrawer>& drawer : propertyDrawers_)
    {
        if (drawer && drawer->Type() == type)
        {
            return drawer.get();
        }
    }
    return nullptr;
}

void EditorExtensionRegistry::RegisterBuiltins(EditorExtensionRegistry& registry)
{
    registry.RegisterObjectFactory(std::make_unique<StaticMeshObjectFactory>());
    registry.RegisterObjectFactory(std::make_unique<TransparentMeshObjectFactory>());
    registry.RegisterPropertyDrawer(std::make_unique<StaticMeshPropertyDrawer>());
    registry.RegisterPropertyDrawer(std::make_unique<TransparentMeshPropertyDrawer>());
    registry.RegisterPropertyDrawer(std::make_unique<FreeCameraStartPropertyDrawer>());
}

#endif // WITH_EDITOR
