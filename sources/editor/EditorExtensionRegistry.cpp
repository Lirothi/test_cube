#include "editor/EditorExtensionRegistry.h"
#if WITH_EDITOR

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/camera/Camera.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/SetMaterialCommand.h"
#include "editor/commands/SetMaterialSlotCommand.h"
#include "editor/commands/SetParticlePresetCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "imgui.h"
#include "rendering/RenderLayers.h"
#include "rendering/renderables/GBufferRenderable.h"
#include "rendering/renderables/RenderableObjectBase.h"
#include "rendering/renderables/TransparentStaticMesh.h"
#include "vfx/ParticleEmitterObject.h"

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

    bool IsGltfModel(const std::string& model)
    {
        const size_t frag = model.find('#'); // strip a "#N"/"#node:" selector before matching
        std::string ext = (frag == std::string::npos) ? model : model.substr(0, frag);
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto ends = [&](const char* s, size_t n) {
            return ext.size() >= n && ext.compare(ext.size() - n, n, s) == 0;
        };
        return ends(".gltf", 5) || ends(".glb", 4);
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

    std::string ToLowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    // Material picker used by both the single-material and per-slot static-mesh inspectors.
    // Returns the selected preset (or "auto") through outSelection without mutating the scene;
    // callers execute the appropriate undoable command after the popup has finished drawing.
    bool MaterialCombo(const char* label,
        const std::string& current,
        const AssetRegistry& registry,
        bool allowAuto,
        std::string& outSelection)
    {
        static ImGuiID openCombo = 0;
        static char filter[128] = {};

        outSelection.clear();
        const ImGuiID comboId = ImGui::GetID(label);
        if (ImGui::BeginCombo(label,
            current.empty() ? "(none)" : current.c_str(),
            ImGuiComboFlags_HeightLargest))
        {
            if (openCombo != comboId)
            {
                openCombo = comboId;
                filter[0] = '\0';
                ImGui::SetKeyboardFocusHere();
            }

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##materialFilter", "Search materials...", filter, sizeof(filter));
            ImGui::Separator();

            const std::string needle = ToLowerCopy(filter);
            int visibleCount = 0;
            if (ImGui::BeginChild("##materialList", ImVec2(0.0f, 220.0f), false))
            {
                constexpr const char* autoLabel = "auto (from glTF)";
                if (allowAuto &&
                    (needle.empty() || ToLowerCopy(autoLabel).find(needle) != std::string::npos))
                {
                    ++visibleCount;
                    if (ImGui::Selectable(autoLabel, current == "auto") && current != "auto")
                    {
                        outSelection = "auto";
                    }
                }

                for (const EditorAssetRecord& rec : registry.Assets())
                {
                    if (rec.id.type != EditorAssetType::MaterialPreset) { continue; }
                    if (!needle.empty() &&
                        ToLowerCopy(rec.id.key).find(needle) == std::string::npos)
                    {
                        continue;
                    }

                    ++visibleCount;
                    const bool selected = rec.id.key == current;
                    if (ImGui::Selectable(rec.id.key.c_str(), selected) && !selected)
                    {
                        outSelection = rec.id.key;
                    }
                }

                if (visibleCount == 0)
                {
                    ImGui::TextDisabled("No matching materials.");
                }
            }
            ImGui::EndChild();
            ImGui::EndCombo();
        }
        else if (openCombo == comboId)
        {
            openCombo = 0;
        }

        return !outSelection.empty();
    }

    const EditorAssetRecord* FindMaterialAsset(const AssetRegistry& registry,
        const std::string& materialName)
    {
        if (materialName.empty() || materialName == "auto")
        {
            return nullptr;
        }
        for (const EditorAssetRecord& record : registry.Assets())
        {
            if (record.id.type == EditorAssetType::MaterialPreset &&
                record.id.key == materialName)
            {
                return &record;
            }
        }
        return nullptr;
    }

    void DrawOpenMaterialButton(EditorContext& ctx,
        const AssetRegistry& registry,
        const std::string& materialName)
    {
        const EditorAssetRecord* material = FindMaterialAsset(registry, materialName);
        const bool canOpen = material && static_cast<bool>(ctx.openMaterialEditor);
        ImGui::SameLine();
        ImGui::BeginDisabled(!canOpen);
        if (ImGui::SmallButton("Edit"))
        {
            ctx.openMaterialEditor(material->id.key, material->path);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            if (canOpen)
            {
                ImGui::SetTooltip("Open %s in Material Editor.", material->id.key.c_str());
            }
            else
            {
                ImGui::SetTooltip("This slot has no editable material asset.");
            }
        }
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

    std::vector<std::string> ListParticlePresetPaths()
    {
        std::vector<std::string> paths;
        std::error_code ec;
        std::filesystem::directory_iterator it("data/particles", ec);
        if (ec)
        {
            return paths;
        }

        const std::filesystem::directory_iterator end;
        while (it != end)
        {
            const std::filesystem::directory_entry& entry = *it;
            std::error_code entryEc;
            if (entry.is_regular_file(entryEc) && entry.path().extension() == ".json")
            {
                paths.push_back(entry.path().generic_string());
            }

            it.increment(ec);
            if (ec)
            {
                break;
            }
        }

        std::sort(paths.begin(), paths.end());
        return paths;
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

    std::string EffectiveRenderLayerName(const EditorObject& object,
        const RenderableObjectBase* runtime)
    {
        if (runtime)
        {
            const uint32_t runtimeMask = runtime->GetRenderLayerMask();
            for (const LayerOption& option : kLayers)
            {
                if (runtimeMask == RenderLayerMask(option.layer))
                {
                    return option.name;
                }
            }
        }

        const nlohmann::json effective = SceneObjectFactory::ResolveMeshAsset(object.properties);
        return effective.value("renderLayer", std::string("Default"));
    }

    // H-importer spawn-scale normalizer: a mesh import can record a default spawn scale in
    // its sibling `.assetimport.json` ("spawnScale" — longest side normalized to the panel's
    // target size). cm-authored glTFs (a "rock" arriving ~115 m) then spawn at a sane size.
    float ImportedSpawnScale(const std::string& modelKey)
    {
        namespace fs = std::filesystem;
        std::string path = modelKey;
        const size_t selector = path.find('#'); // strip "#node:"/"#N" mesh selectors
        if (selector != std::string::npos) { path.resize(selector); }
        if (path.empty()) { return 0.0f; }

        std::ifstream file(fs::path(path).parent_path() / ".assetimport.json");
        if (!file) { return 0.0f; }
        const nlohmann::json manifest = nlohmann::json::parse(file, nullptr, false, true);
        if (manifest.is_discarded() || !manifest.is_object()) { return 0.0f; }
        return manifest.value("spawnScale", 0.0f);
    }

    // J1: a `models/<name>.mesh.json` record is spawned as a compact reference (the mesh asset
    // owns geometry/layout/shader/material); raw .obj/.gltf records keep the legacy inline form.
    bool IsMeshAsset(const std::string& key)
    {
        constexpr std::string_view kExt = ".mesh.json";
        return key.size() >= kExt.size() &&
            std::equal(kExt.rbegin(), kExt.rend(), key.rbegin());
    }

    // Spawn scale from the mesh asset file (its own `spawnScale`, else 0 = no baked scale).
    float MeshAssetSpawnScale(const std::string& meshAssetPath)
    {
        std::ifstream file(meshAssetPath);
        if (!file) { return 0.0f; }
        const nlohmann::json asset = nlohmann::json::parse(file, nullptr, false, true);
        if (asset.is_discarded() || !asset.is_object()) { return 0.0f; }
        return asset.value("spawnScale", 0.0f);
    }

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
            o["position"] = SpawnPositionJson(ctx.scene, worldPositionHint);
            const std::string assetKey = sourceAsset ? sourceAsset->id.key : std::string{};

            // J1: a mesh asset spawns as a compact reference — geometry/layout/shader/material all
            // live in the file, so the level entry is just `mesh` + placement.
            if (IsMeshAsset(assetKey))
            {
                o["mesh"] = assetKey;
                const float meshScale = MeshAssetSpawnScale(assetKey);
                o["scale"] = meshScale > 0.0f
                    ? nlohmann::json::array({ meshScale, meshScale, meshScale })
                    : nlohmann::json::array({ 1.0f, 1.0f, 1.0f });
                return o;
            }

            // Legacy path: raw .obj/.gltf record — write the inline render plumbing.
            const std::string model = assetKey;
            o["model"] = model;
            const float importScale = ImportedSpawnScale(model);
            o["scale"] = importScale > 0.0f
                ? nlohmann::json::array({ importScale, importScale, importScale })
                : nlohmann::json::array({ 1.0f, 1.0f, 1.0f });
            // B4: glTF assets carry their own PBR materials — spawn with "auto" so every submesh
            // gets one slot from the glTF (B2). OBJ/text meshes keep a default preset.
            o["material"] = IsGltfModel(model) ? std::string("auto") : PickDefaultStaticMaterial(registry);
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
            RenderableObjectBase* runtime = ctx.scene.FindEditorObject(obj.id.value);
            GBufferRenderable* gbMat = runtime ? runtime->AsGBufferRenderable() : nullptr;
            size_t slotCount = gbMat ? gbMat->SlotCount() : 0;

            // Per-slot material list for multi-material (glTF) objects; a single "Material" combo
            // otherwise (back-compat). "auto" = pull the slot's material from the glTF (B2).
            bool multiSlot = slotCount > 1;
            if (ctx.selection.Size() > 1)
            {
                // Only expose slots that exist on EVERY selected mesh. A single-slot
                // primary can still share Slot 0 with a multi-material recipient.
                for (const auto id : ctx.selection.Ordered())
                {
                    auto* other = ctx.scene.FindEditorObject(id.value);
                    const auto* gb = other ? other->AsGBufferRenderable() : nullptr;
                    const size_t count = gb ? gb->SlotCount() : 0;
                    multiSlot |= count > 1;
                    slotCount = std::min(slotCount, count);
                }
            }

            if (multiSlot)
            {
                // Resolve each slot's EFFECTIVE material from the runtime NOW, before the combo
                // loop. Two reasons: (1) slotPresets_ already merges the object's own "materials"
                // override over the mesh-asset defaults, so a material promoted via the Mesh
                // Editor's "Save as material" (which lives on the mesh asset, never on the object
                // JSON) shows up here — the object's own properties alone would still read "auto".
                // (2) A per-slot combo command respawns the object mid-loop, dangling gbMat; caching
                // the names first keeps the loop from dereferencing a freed pointer.
                std::vector<std::string> resolvedSlots(slotCount);
                for (size_t i = 0; i < slotCount; ++i) { resolvedSlots[i] = gbMat->SlotPreset(i); }

                ImGui::TextDisabled("Material slots (%zu)", slotCount);
                for (size_t i = 0; i < slotCount; ++i)
                {
                    const std::string cur = resolvedSlots[i];
                    ImGui::PushID(static_cast<int>(i));
                    char label[32];
                    std::snprintf(label, sizeof(label), "Slot %zu", i);
                    std::string selectedMaterial;
                    if (MaterialCombo(label, cur, registry, /*allowAuto=*/true, selectedMaterial))
                    {
                        commandStack.Execute(ctx, std::make_unique<SetMaterialSlotCommand>(
                            obj.id, static_cast<int>(i), std::move(selectedMaterial)));
                    }
                    DrawOpenMaterialButton(ctx, registry, cur);
                    // "Save as material" (promote a still-"auto" glTF slot to a named file) lives
                    // in the Mesh Editor, not here — it's a per-mesh-asset op whose Save fans out
                    // to every placed instance, not a per-instance override.
                    ImGui::PopID();
                }
            }
            else
            {
                // Effective material for the single slot: the runtime's resolved name (mesh-asset
                // default folded in) when available, else the object's own override. Same reason as
                // the multi-slot branch — a mesh-asset-level material won't appear in obj JSON.
                const std::string current = gbMat && slotCount == 1
                    ? gbMat->SlotPreset(0)
                    : obj.properties.value("material", std::string());
                std::string selectedMaterial;
                if (MaterialCombo("Material", current, registry, /*allowAuto=*/false, selectedMaterial))
                {
                    commandStack.Execute(ctx, std::make_unique<SetMaterialCommand>(
                        obj.id, std::move(selectedMaterial)));
                }
                DrawOpenMaterialButton(ctx, registry, current);
            }

            // A material command above respawns the object (materials load at Init, no live
            // setter), which frees the runtime fetched at the top — re-fetch before using it for
            // the render layer + live params below, or we dereference a dangling pointer.
            runtime = ctx.scene.FindEditorObject(obj.id.value);

            // Mesh assets own defaults such as the atoll's Terrain layer. Show the effective
            // runtime value instead of claiming Default merely because this instance has no
            // explicit override in the level JSON.
            const std::string currentLayer = EffectiveRenderLayerName(obj, runtime);
            if (ImGui::BeginCombo("Render Layer", currentLayer.c_str()))
            {
                for (const LayerOption& opt : kLayers)
                {
                    const bool selected = (currentLayer == opt.name);
                    if (ImGui::Selectable(opt.name, selected) && !selected)
                    {
                        obj.properties["renderLayer"] = opt.name;
                        if (runtime) { runtime->SetRenderLayer(opt.layer); }
                        ctx.scene.NotifyEditorShadowCasterVisibilityChanged();
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
            const MaterialParams& mp = static_cast<const GBufferRenderable*>(gb)->MaterialParamsRef();

            float texOffsScale[4] = { mp.texOffsScale.x, mp.texOffsScale.y, mp.texOffsScale.z, mp.texOffsScale.w };
            if (ImGui::DragFloat4("Tex Offset/Scale", texOffsScale, 0.01f))
            {
                gb->MaterialParamsRef().texOffsScale = Math::float4(texOffsScale[0], texOffsScale[1], texOffsScale[2], texOffsScale[3]);
                obj.properties["texOffsScale"] = { texOffsScale[0], texOffsScale[1], texOffsScale[2], texOffsScale[3] };
                ctx.document.SetDirty(true);
            }

            float normalStrength = mp.texFlags.w;
            if (ImGui::DragFloat("Normal Strength", &normalStrength, 0.01f, 0.0f, 10.0f))
            {
                gb->MaterialParamsRef().SetNormalStrength(normalStrength);
                obj.properties["normalStrength"] = normalStrength;
                ctx.document.SetDirty(true);
            }

            // W3: per-OBJECT sway strength (0 = rigid). The setter re-propagates to every slot so
            // all submeshes stay in lockstep; the asset-level default lives in the Mesh Editor's
            // Wind section (mesh.json), and this overrides it for this object only.
            float windStrength = gb->GetWindStrength();
            if (ImGui::DragFloat("Wind Strength", &windStrength, 0.01f, 0.0f, 1.0f))
            {
                gb->SetWindStrength(windStrength);
                obj.properties["windStrength"] = windStrength;
                ctx.document.SetDirty(true);
            }

            bool useMR = mp.texFlags.y > 0.5f;
            if (ImGui::Checkbox("Use MR Texture", &useMR))
            {
                gb->MaterialParamsRef().SetUseMR(useMR);
                obj.properties["useMR"] = useMR;
                ctx.document.SetDirty(true);
            }

            float metalRough[2] = { mp.metalRough.x, mp.metalRough.y };
            if (ImGui::DragFloat2("Metallic / Roughness", metalRough, 0.01f, 0.0f, 1.0f))
            {
                gb->MaterialParamsRef().metalRough = Math::float2(metalRough[0], metalRough[1]);
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

    class ParticleEmitterPropertyDrawer final : public IEditorPropertyDrawer
    {
    public:
        std::string_view Type() const override { return "particleEmitter"; }

        void Draw(EditorContext& ctx,
            EditorCommandStack& commandStack,
            const AssetRegistry& registry,
            EditorObject& obj) const override
        {
            (void)commandStack;
            (void)registry;

            RenderableObjectBase* runtime = ctx.scene.FindEditorObject(obj.id.value);
            ParticleEmitterObject* emitter = runtime ? runtime->AsParticleEmitter() : nullptr;
            if (!emitter)
            {
                ImGui::TextDisabled("Particle properties editable on spawned emitters.");
                return;
            }

            const std::string selectedPreset = obj.properties.value("preset", std::string());
            const std::vector<std::string> presetPaths = ListParticlePresetPaths();
            bool assignedPreset = false;
            if (ImGui::BeginCombo("Preset", selectedPreset.empty() ? "(inline emitter)" : selectedPreset.c_str()))
            {
                for (const std::string& presetPath : presetPaths)
                {
                    const bool selected = presetPath == selectedPreset;
                    if (ImGui::Selectable(presetPath.c_str(), selected) && !selected)
                    {
                        assignedPreset = commandStack.Execute(ctx,
                            std::make_unique<SetParticlePresetCommand>(obj.id, presetPath));
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                if (presetPaths.empty())
                {
                    ImGui::TextDisabled("No JSON presets found in data/particles.");
                }
                ImGui::EndCombo();
            }

            // Assigning respawns this object, so the current runtime pointer is no longer valid.
            if (assignedPreset)
            {
                return;
            }

            // Preset-based emitters keep the preset as the base and store per-instance tweaks in
            // "overrides"; inline emitters edit top-level fields. Either way the runtime desc is
            // edited live (RecordCompute refills the CB from it every frame).
            const bool preset = obj.properties.contains("preset");
            auto setProp = [&](const char* key, nlohmann::json value) {
                if (preset) { obj.properties["overrides"][key] = std::move(value); }
                else { obj.properties[key] = std::move(value); }
                ctx.document.SetDirty(true);
            };

            vfx::EmitterDesc& d = emitter->DescRef();

            if (preset && obj.properties["preset"].is_string())
            {
                ImGui::TextDisabled("Selecting a preset resets old per-instance overrides.");
            }

            float spawnRate = d.spawnRate;
            if (ImGui::DragFloat("Spawn Rate", &spawnRate, 1.0f, 0.0f, 100000.0f))
            {
                d.spawnRate = spawnRate; setProp("spawnRate", spawnRate);
            }

            float lifetime[2] = { d.lifetimeMin, d.lifetimeMax };
            if (ImGui::DragFloat2("Lifetime (min/max)", lifetime, 0.02f, 0.0f, 60.0f))
            {
                d.lifetimeMin = lifetime[0]; d.lifetimeMax = lifetime[1];
                setProp("lifetime", nlohmann::json::array({ lifetime[0], lifetime[1] }));
            }

            float speed[2] = { d.speedMin, d.speedMax };
            if (ImGui::DragFloat2("Speed (min/max)", speed, 0.02f))
            {
                d.speedMin = speed[0]; d.speedMax = speed[1];
                setProp("speed", nlohmann::json::array({ speed[0], speed[1] }));
            }

            float gravity = d.gravity;
            if (ImGui::DragFloat("Gravity (- = buoyant)", &gravity, 0.05f))
            {
                d.gravity = gravity; setProp("gravity", gravity);
            }

            float drag = d.drag;
            if (ImGui::DragFloat("Drag", &drag, 0.01f, 0.0f, 20.0f))
            {
                d.drag = drag; setProp("drag", drag);
            }

            // W8: m/s^2 of horizontal push at wind strength 1, scaled live by the gust envelope.
            // Per emitter because there is no particle mass: smoke rides the wind, sparks do not.
            float windInfluence = d.windInfluence;
            if (ImGui::DragFloat("Wind Influence", &windInfluence, 0.02f, 0.0f, 20.0f, "%.2f m/s2"))
            {
                d.windInfluence = windInfluence; setProp("windInfluence", windInfluence);
            }

            float coneAngle = d.coneAngleDeg;
            if (ImGui::DragFloat("Cone Angle (deg)", &coneAngle, 0.5f, 0.0f, 180.0f))
            {
                d.coneAngleDeg = coneAngle; setProp("coneAngleDeg", coneAngle);
            }

            float coneDir[3] = { d.coneDir.x, d.coneDir.y, d.coneDir.z };
            if (ImGui::DragFloat3("Cone Dir", coneDir, 0.02f))
            {
                d.coneDir = Math::float3(coneDir[0], coneDir[1], coneDir[2]);
                setProp("coneDir", nlohmann::json::array({ coneDir[0], coneDir[1], coneDir[2] }));
            }

            float size[2] = { d.sizeStart, d.sizeEnd };
            if (ImGui::DragFloat2("Size (start/end)", size, 0.01f, 0.0f, 100.0f))
            {
                d.sizeStart = size[0]; d.sizeEnd = size[1];
                setProp("size", nlohmann::json::array({ size[0], size[1] }));
            }

            float softFade = d.softFade;
            if (ImGui::DragFloat("Soft Fade", &softFade, 0.01f, 0.0f, 10.0f))
            {
                d.softFade = softFade; setProp("softFade", softFade);
            }

            ImGui::Separator();
            ImGui::TextDisabled("Color over life (RGBA; A fades)");
            bool colorChanged = false;
            for (int k = 0; k < 4; ++k)
            {
                float c[4] = { d.colorKeys[k].x, d.colorKeys[k].y, d.colorKeys[k].z, d.colorKeys[k].w };
                ImGui::PushID(k);
                if (ImGui::ColorEdit4("##colorKey", c,
                        ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_Float))
                {
                    d.colorKeys[k] = Math::float4(c[0], c[1], c[2], c[3]);
                    colorChanged = true;
                }
                ImGui::PopID();
            }
            if (colorChanged)
            {
                nlohmann::json keys = nlohmann::json::array();
                for (int k = 0; k < 4; ++k)
                {
                    keys.push_back(nlohmann::json::array({
                        d.colorKeys[k].x, d.colorKeys[k].y, d.colorKeys[k].z, d.colorKeys[k].w }));
                }
                setProp("colorKeys", std::move(keys));
            }

            // Structural fields (buffer sizes / PSO / sprite) are baked at spawn; show read-only.
            ImGui::Separator();
            ImGui::TextDisabled("Max particles: %u  |  blend: %s%s",
                d.maxParticles, d.additive ? "additive" : "alpha",
                d.sortParticles ? " (sorted)" : "");
            ImGui::TextDisabled("Texture: %s", d.texture.empty() ? "(procedural disc)" : d.texture.c_str());
            ImGui::TextDisabled("(re-create the emitter to change the above)");
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
    registry.RegisterPropertyDrawer(std::make_unique<ParticleEmitterPropertyDrawer>());
    registry.RegisterPropertyDrawer(std::make_unique<FreeCameraStartPropertyDrawer>());
}

#endif // WITH_EDITOR
