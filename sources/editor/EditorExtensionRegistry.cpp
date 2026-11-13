#include "editor/commands/EditObjectPropertiesCommand.h"
#include "editor/scene/EditorPlayerStart.h"
#include "editor/EditorExtensionRegistry.h"
#if WITH_EDITOR

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/camera/Camera.h"
#include "app/game/VesselStats.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/assets/MeshRestPose.h"
#include "editor/commands/CompositeCommand.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/SetBuoyantCommand.h"
#include "editor/commands/SetPhysicsCommand.h"
#include "editor/commands/SetCastShadowCommand.h"
#include "editor/commands/SetRaceKeyCommand.h"
#include "editor/commands/SetVesselTuneCommand.h"
#include "editor/commands/SetLampSwitchCommand.h"
#include "editor/commands/SetMaterialCommand.h"
#include "editor/commands/SetMaterialSlotCommand.h"
#include "editor/commands/SetParticlePresetCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "imgui.h"
#include "ocean/OceanRenderable.h"
#include "rendering/RenderLayers.h"
#include "rendering/renderables/GBufferRenderable.h"
#include "rendering/renderables/RenderableObject.h"
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

    // J1: a `models/<name>/<name>.mesh.json` record is spawned as a compact reference (the mesh asset
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
                // An asset with a rest pose (a shell, a starfish) lies in it, its lowest point on the
                // spot the new object is put at. Spawn and place lay it again after their own scale
                // and yaw; the Content Browser and a viewport drop keep this one.
                restpose::ApplyToNewObject(o, 0.0f, o["position"][1].get<float>());
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
                gb->SetObjectTexOffsScale(Math::float4(texOffsScale[0], texOffsScale[1], texOffsScale[2], texOffsScale[3]));
                obj.properties["texOffsScale"] = { texOffsScale[0], texOffsScale[1], texOffsScale[2], texOffsScale[3] };
                ctx.document.SetDirty(true);
            }

            float normalStrength = mp.texFlags.w;
            if (ImGui::DragFloat("Normal Strength", &normalStrength, 0.01f, 0.0f, 10.0f))
            {
                gb->SetObjectNormalStrength(normalStrength); // every material slot, not slot 0 alone
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
                gb->SetObjectUseMR(useMR);
                obj.properties["useMR"] = useMR;
                ctx.document.SetDirty(true);
            }

            float metalRough[2] = { mp.metalRough.x, mp.metalRough.y };
            if (ImGui::DragFloat2("Metallic / Roughness", metalRough, 0.01f, 0.0f, 1.0f))
            {
                gb->SetObjectMetalRough(Math::float2(metalRough[0], metalRough[1]));
                obj.properties["metalRough"] = { metalRough[0], metalRough[1] };
                ctx.document.SetDirty(true);
            }

            // The object's baseColor: times the albedo texture while it is on (a tint -- a white-albedo mesh such as
            // ocean_buoy_white takes the colour with its detail), the flat colour when "Use Albedo" is off.
            {
                float tint[3] = { mp.baseColor.x, mp.baseColor.y, mp.baseColor.z };
                const bool textured = mp.texFlags.x > 0.5f;
                if (ImGui::ColorEdit3(textured ? "Tint" : "Colour", tint, ImGuiColorEditFlags_Float))
                {
                    gb->SetObjectBaseColorRgb(Math::float3(tint[0], tint[1], tint[2]));
                    obj.properties["baseColor"] = { tint[0], tint[1], tint[2] };
                    ctx.document.SetDirty(true);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(textured ? "Multiplied into the albedo texture: white = as the texture is. A white-albedo mesh\n"
                                                 "(ocean_buoy_white) takes this colour with its detail. MCP: setSurface {tint: [r, g, b]}."
                                               : "The flat colour in place of the albedo texture (Use Albedo is off).");
                }
            }

            DrawCastShadow(ctx, commandStack, obj);
            DrawPhysics(ctx, commandStack, obj);
            DrawBuoyancy(ctx, commandStack, obj, runtime);
            DrawRace(ctx, commandStack, obj);
            DrawBoat(ctx, commandStack, obj, runtime);
            DrawGlowOnAt(ctx, commandStack, obj, gb);
        }

    private:
        // A boat's own numbers -- how it races (physics::VesselGroupPerCopy) -- over its asset's (Mesh Editor > Vessel):
        // the level object's "vessel" block (SetVesselTuneCommand). On top what the boat does by game::StatsOf, the model
        // its card in Play and a race bot count on, against its asset's; it follows a drag before the drag is let go.
        static void DrawBoat(EditorContext& ctx, EditorCommandStack& commandStack, const EditorObject& obj, RenderableObjectBase* runtime)
        {
            RenderableObject* object = runtime ? runtime->AsRenderableObject() : nullptr;
            const physics::VesselSettings* live = object ? object->GetVesselSettings() : nullptr;
            if (!live) { return; }
            ImGui::SeparatorText("Boat");
            ImGui::PushID("boat");

            // The asset's numbers: its mesh.json, read again every second at most (a Mesh Editor save shows within it).
            struct AssetNumbers
            {
                std::uint64_t object = ~0ull;
                int frame = 0;
                physics::VesselSettings settings;
            };
            static AssetNumbers asset;
            if (asset.object != obj.id.value || ImGui::GetFrameCount() - asset.frame > 60)
            {
                asset.object = obj.id.value;
                asset.frame = ImGui::GetFrameCount();
                asset.settings = SetVesselTuneCommand::AssetSettings(obj);
            }
            // A drag in progress, one number (`key`) or one bar of the card (`bar` >= 0, with the numbers SolveCard found): what
            // the rows show and the card counts on until it is let go (one undo entry then).
            struct Pending
            {
                std::uint64_t object = 0;
                const physics::VesselKey* key = nullptr;
                float value = 0.0f;
                int bar = -1;
                physics::VesselSettings solved;
            };
            static Pending pending;
            if ((pending.key || pending.bar >= 0) && pending.object != obj.id.value) { pending = Pending{}; }
            physics::VesselSettings shown = pending.bar >= 0 ? pending.solved : *live;
            if (pending.key) { shown.*(pending.key->member) = pending.value; }

            const auto selectedBoats = [&ctx, &obj]()
            {
                std::vector<EditorObjectId> ids;
                for (const EditorObjectId id : ctx.selection.Ordered())
                {
                    const EditorObject* selected = ctx.document.Find(id);
                    if (selected && SetPhysicsCommand::VesselAsset(ctx, id)) { ids.push_back(id); }
                }
                if (ids.empty()) { ids.push_back(obj.id); }
                return ids;
            };
            const auto runAll = [&ctx, &commandStack](const char* label, std::vector<std::unique_ptr<SetVesselTuneCommand>> commands)
            {
                auto composite = std::make_unique<CompositeCommand>(label);
                for (std::unique_ptr<SetVesselTuneCommand>& command : commands)
                {
                    const EditorObject* target = ctx.document.Find(command->Target());
                    if (target && command->Changes(*target)) { composite->Add(std::move(command)); }
                }
                if (!composite->Empty()) { commandStack.Execute(ctx, std::move(composite)); }
            };
            const ImVec4 accent(1.0f, 0.78f, 0.3f, 1.0f);

            // The card's four bars, as the player reads them over the boat in Play -- and dragged as they read (owner: "I
            // want to drag these characteristics; if one moves a set of parameters, that must happen by itself"). Each has
            // its number (game::SolveCard), the other three are held: a heavier boat gets the thrust and the rudder that
            // keep its acceleration and its circle.
            SetVesselTuneCommand::HullShape shape;
            if (!SetVesselTuneCommand::ShapeOf(ctx, obj.id, shape))
            {
                ImGui::TextDisabled("No card: not a boat now (Body Dynamic and floating make it one).");
            }
            else if (ImGui::BeginTable("##boatCard", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
            {
                const game::VesselCard now = game::CardOf(shown, shape.halfLength, shape.rudderLever, shape.draft);
                const game::VesselCard live0 = game::CardOf(*live, shape.halfLength, shape.rudderLever, shape.draft);
                const game::VesselCard was = game::CardOf(asset.settings, shape.halfLength, shape.rudderLever, shape.draft);
                // The Inspector is narrow: the names keep their width, the values are short (the tooltip says what each is).
                ImGui::TableSetupColumn("The card", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Sea keeping").x + 4.0f);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.6f);
                ImGui::TableSetupColumn("This boat", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch, 0.7f);
                ImGui::TableHeadersRow();
                struct Bar
                {
                    const char* name;
                    unsigned bit;
                    float scale;     // the card's unit per the model's
                    float lo, hi, speed;
                    const char* format;
                    const char* hint;
                };
                const float leastSea = 100.0f * (game::SeaKeeping(150.0f, shape.draft));
                const Bar bars[] = {
                    { "Top speed", SetVesselTuneCommand::kCardTopSpeed, 1.94384f, 22.0f, 80.0f, 0.05f, "%.1f kn",
                      "Flat out. Moves the drag ahead (and the thrust, to keep its acceleration). The bot runs its straights at it." },
                    { "Acceleration", SetVesselTuneCommand::kCardToPlane, 1.0f, 1.0f, 30.0f, 0.01f, "%.1f s",
                      "Seconds from rest to 20 kn, full ahead (the card: \"20 kn in ... s\"). Moves the thrust (and the drag ahead\n"
                      "with it, to keep its top speed): more power, a quicker start, harder braking for the bends." },
                    { "Handling", SetVesselTuneCommand::kCardCircle, 1.0f, 15.0f, 400.0f, 0.2f, "%.0f m",
                      "Metres across its tightest circle, hard over, full ahead (the card: \"a ... m circle flat out\"). Moves the\n"
                      "rudder force and the hull's side grip together: flat out the grip sets the circle, slower the rudder does.\n"
                      "The bot takes its bends at a share of what this circle asks across its way: tighter, faster bends." },
                    { "Sea keeping", SetVesselTuneCommand::kCardSeaKeeping, 100.0f, leastSea, 200.0f, 0.2f, "%.0f %%",
                      "Half its draft (the hull's: Mesh Editor), half its weight. Moves the mass -- a heavier boat is thrown\n"
                      "about less by the waves -- and the thrust and the rudder with it, to keep the other three bars." },
                };
                const float values[] = { now.topSpeed, now.toPlane, now.circle, now.seaKeeping };
                const float current[] = { live0.topSpeed, live0.toPlane, live0.circle, live0.seaKeeping };
                const float assets[] = { was.topSpeed, was.toPlane, was.circle, was.seaKeeping };
                const float shares[] = { game::TopSpeedBar(now.topSpeed), game::AccelerationBar(now.toPlane), game::HandlingBar(now.circle), now.seaKeeping };
                for (int i = 0; i < 4; ++i)
                {
                    const Bar& bar = bars[i];
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(bar.name);
                    ImGui::TableNextColumn();
                    ImGui::ProgressBar(std::clamp(shares[i], 0.02f, 1.0f), ImVec2(-FLT_MIN, 0.0f), "");
                    ImGui::TableNextColumn();
                    float value = values[i] * bar.scale;
                    const bool differs = std::fabs(values[i] - assets[i]) > 0.002f * std::max(std::fabs(assets[i]), 0.01f);
                    if (differs) { ImGui::PushStyleColor(ImGuiCol_Text, accent); }
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::DragFloat("##bar", &value, bar.speed, bar.lo, bar.hi, bar.format, ImGuiSliderFlags_AlwaysClamp))
                    {
                        // Solve again only when the bar moved: SolveCard runs the model some 50 times.
                        game::VesselCard want{ current[0], current[1], current[2], current[3] };
                        float* const slot[] = { &want.topSpeed, &want.toPlane, &want.circle, &want.seaKeeping };
                        *slot[i] = value / bar.scale;
                        pending = Pending{};
                        pending.object = obj.id.value;
                        pending.bar = i;
                        pending.value = value / bar.scale;
                        pending.solved = *live;
                        game::SolveCard(pending.solved, shape.halfLength, shape.rudderLever, shape.draft, want);
                    }
                    if (differs) { ImGui::PopStyleColor(); }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s\n\nCtrl+click types a number. MCP: run_action setBoatCard {topSpeedKn, secondsTo20Kn, circleM, seaKeeping}", bar.hint);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit() && pending.bar == i)
                    {
                        // Every selected boat, each solved from its own numbers, one undo entry.
                        game::VesselCard want;
                        float* const slot[] = { &want.topSpeed, &want.toPlane, &want.circle, &want.seaKeeping };
                        *slot[i] = pending.value;
                        std::vector<std::unique_ptr<SetVesselTuneCommand>> commands;
                        for (const EditorObjectId id : selectedBoats())
                        {
                            for (std::unique_ptr<SetVesselTuneCommand>& command : SetVesselTuneCommand::Card(ctx, id, want, bar.bit)) { commands.push_back(std::move(command)); }
                        }
                        pending = Pending{};
                        runAll((std::string("Boat: ") + bar.name).c_str(), std::move(commands));
                    }
                    else if (pending.bar == i && !ImGui::IsItemActive())
                    {
                        pending = Pending{};
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled(bar.format, static_cast<double>(assets[i] * bar.scale));
                    ImGui::PopID();
                }
                ImGui::EndTable();
                ImGui::TextDisabled("%.2f t, %.0f kN, %.2f m deep", shown.mass * 0.001f, shown.thrust * 0.001f, shape.draft);
            }

            // The numbers themselves, by group. Accent = this boat's own; the button before it puts the asset's back.
            ImGui::TextDisabled("The numbers behind it");
            const float resetWidth = ImGui::GetFrameHeight();
            bool anyOwn = false;
            for (const physics::VesselGroup group : { physics::VesselGroup::Engine, physics::VesselGroup::Hull, physics::VesselGroup::Helm,
                     physics::VesselGroup::Turn, physics::VesselGroup::Sea })
            {
                ImGui::SetNextItemOpen(false, ImGuiCond_Once); // the card above is the casual edit
                const bool open = ImGui::TreeNode(physics::VesselGroupName(group));
                for (const physics::VesselKey& key : physics::VesselKeys())
                {
                    if (key.group != group) { continue; }
                    const bool own = SetVesselTuneCommand::HasOwn(obj, key);
                    anyOwn = anyOwn || own;
                    if (!open) { continue; }
                    ImGui::PushID(key.key);
                    if (own)
                    {
                        if (ImGui::Button("<", ImVec2(resetWidth, 0.0f)))
                        {
                            std::vector<std::unique_ptr<SetVesselTuneCommand>> commands;
                            for (const EditorObjectId id : selectedBoats()) { commands.push_back(SetVesselTuneCommand::Reset(id, &key)); }
                            runAll("Boat: back to the asset's", std::move(commands));
                        }
                        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Back to the asset's %g", static_cast<double>(asset.settings.*(key.member))); }
                    }
                    else
                    {
                        ImGui::Dummy(ImVec2(resetWidth, ImGui::GetFrameHeight()));
                    }
                    ImGui::SameLine();
                    const bool editing = pending.key == &key;
                    float value = editing ? pending.value : shown.*(key.member); // a bar dragged above: what it moves, live
                    if (own) { ImGui::PushStyleColor(ImGuiCol_Text, accent); }
                    if (ImGui::DragFloat(key.label, &value, key.speed, key.lo, key.hi, key.hi >= 1000.0f ? "%.0f" : "%.3f", ImGuiSliderFlags_AlwaysClamp))
                    {
                        pending = Pending{ obj.id.value, &key, value };
                    }
                    if (own) { ImGui::PopStyleColor(); }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s\n\nThe asset's: %g%s\n--set vessel.%s (every boat in Play, transient); MCP run_action setVessel {key: \"%s\", value}",
                            key.hint, static_cast<double>(asset.settings.*(key.member)), own ? "   (this boat has its own: the button on the left puts it back)" : "",
                            key.key, key.key);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit() && editing)
                    {
                        // Every selected boat, one undo entry.
                        std::vector<std::unique_ptr<SetVesselTuneCommand>> commands;
                        for (const EditorObjectId id : selectedBoats()) { commands.push_back(SetVesselTuneCommand::Key(id, key, pending.value)); }
                        pending = Pending{};
                        runAll((std::string("Boat: ") + key.label).c_str(), std::move(commands));
                    }
                    else if (editing && !ImGui::IsItemActive())
                    {
                        pending = Pending{}; // let go with nothing changed, or Esc
                    }
                    ImGui::PopID();
                }
                if (open) { ImGui::TreePop(); }
            }
            if (anyOwn)
            {
                if (ImGui::Button("All back to the asset's"))
                {
                    std::vector<std::unique_ptr<SetVesselTuneCommand>> commands;
                    for (const EditorObjectId id : selectedBoats()) { commands.push_back(SetVesselTuneCommand::Reset(id, nullptr)); }
                    runAll("Boat: all as the asset's", std::move(commands));
                }
                ImGui::SameLine();
            }
            ImGui::TextDisabled(anyOwn ? "This boat's own numbers are in the level." : "All its asset's (Mesh Editor > Vessel).");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "A number set here is this copy's alone: the level object's \"vessel\" block, laid key by key over the asset's\n"
                    "mesh.json. Every selected boat takes it (one undo entry). The asset's own numbers -- for every copy -- and how it\n"
                    "floats (draft, centre of mass) are the Mesh Editor's. A race bot counts on these the moment they change.");
            }
            ImGui::PopID();
        }

        // The race (app/game/RaceCourse.h): a mark's gate number; on a boat, a bot at its helm.
        static void DrawRace(EditorContext& ctx, EditorCommandStack& commandStack, const EditorObject& obj)
        {
            if (!SetRaceKeyCommand::AppliesTo(obj)) { return; }
            ImGui::SeparatorText("Race");
            int gate = SetRaceKeyCommand::DocumentGate(obj);
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::InputInt("Race gate", &gate, 1, 1, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit())
            {
                gate = std::max(gate, 0);
                if (gate != SetRaceKeyCommand::DocumentGate(obj))
                {
                    // Every selected mesh, one undo entry: select a gate's two buoys and number them together.
                    auto composite = std::make_unique<CompositeCommand>(gate > 0 ? "Race Gate " + std::to_string(gate) : "Clear Race Gate");
                    for (const EditorObjectId id : ctx.selection.Ordered())
                    {
                        const EditorObject* selected = ctx.document.Find(id);
                        if (selected && SetRaceKeyCommand::AppliesTo(*selected)) { composite->Add(SetRaceKeyCommand::Gate(id, gate)); }
                    }
                    if (composite->Empty()) { composite->Add(SetRaceKeyCommand::Gate(obj.id, gate)); }
                    commandStack.Execute(ctx, std::move(composite));
                }
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "0 = not part of a race. Two meshes (buoys) with the same number are a GATE: a boat passes\n"
                    "between them. Gates are run in the order of their numbers; the last leads back to gate 1,\n"
                    "the start and the finish. Move a buoy and the course moves: the bots and the lap clock\n"
                    "read it when Play starts. In Play, `--set bot.debug:1` draws the gates and the bot's line.");
            }
            if (SetRaceKeyCommand::DocumentGate(obj) > 0)
            {
                // A level may hold several courses: the mark's own, by name (empty = the level's first).
                char track[64];
                std::snprintf(track, sizeof(track), "%s", SetRaceKeyCommand::DocumentTrack(obj).c_str());
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::InputText("Race track", track, sizeof(track), ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit())
                {
                    const std::string name = track;
                    if (name != SetRaceKeyCommand::DocumentTrack(obj))
                    {
                        auto composite = std::make_unique<CompositeCommand>(name.empty() ? "Race Track: the first" : "Race Track " + name);
                        for (const EditorObjectId id : ctx.selection.Ordered())
                        {
                            const EditorObject* selected = ctx.document.Find(id);
                            if (selected && SetRaceKeyCommand::DocumentGate(*selected) > 0) { composite->Add(SetRaceKeyCommand::Track(id, name)); }
                        }
                        if (composite->Empty()) { composite->Add(SetRaceKeyCommand::Track(obj.id, name)); }
                        commandStack.Execute(ctx, std::move(composite));
                    }
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "Whose course this mark is: empty = the level's first, a name = another course with its own gates\n"
                        "1, 2, 3 ... and its own start zone. In Play every course's zone is lit: the one you drive into is\n"
                        "raced (`race.track` picks it for a script's race.start).");
                }
            }
            if (!SetPhysicsCommand::VesselAsset(ctx, obj.id)) { return; }
            bool bot = SetRaceKeyCommand::DocumentBot(obj);
            if (ImGui::Checkbox("Bot at the helm", &bot))
            {
                commandStack.Execute(ctx, SetRaceKeyCommand::Bot(obj.id, bot));
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "In a race a bot takes this boat's helm and runs the level's race gates. Take the helm\n"
                    "yourself (E) and it stands down until you leave. At any helm B hands the boat to the same\n"
                    "bot (autopilot). It drives by THIS boat's numbers (Boat below: its top speed, its circle,\n"
                    "its braking). How it drives: Developer Controls > Physics, bot.*.");
            }
        }

        // UE's Lighting > Cast Shadow. Every selected mesh, one undo entry.
        static void DrawCastShadow(EditorContext& ctx, EditorCommandStack& commandStack, const EditorObject& obj)
        {
            ImGui::SeparatorText("Lighting");
            bool castShadow = SetCastShadowCommand::DocumentCastsShadow(obj);
            if (ImGui::Checkbox("Cast Shadow", &castShadow))
            {
                auto composite = std::make_unique<CompositeCommand>(
                    castShadow ? "Enable Cast Shadow" : "Disable Cast Shadow");
                for (const EditorObjectId id : ctx.selection.Ordered())
                {
                    const EditorObject* selected = ctx.document.Find(id);
                    if (selected && SetCastShadowCommand::AppliesTo(*selected))
                    {
                        composite->Add(std::make_unique<SetCastShadowCommand>(id, castShadow));
                    }
                }
                if (composite->Empty()) { composite->Add(std::make_unique<SetCastShadowCommand>(obj.id, castShadow)); }
                commandStack.Execute(ctx, std::move(composite));
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Off: the mesh stays visible and lit but casts no shadow -- not the sun's, not a lamp's\n"
                    "(atlas or VSM), not a ray-traced one. For grass and small clutter (cheaper shadow\n"
                    "passes) or a bulb inside its own lamp. Contact shadows still see it: they trace\n"
                    "the depth buffer (UE keeps those on a separate switch).");
            }
        }

        // docs/physics_plan.md: UE's Collision + Simulate Physics on one row. Every selected mesh, one undo entry.
        static void DrawPhysics(EditorContext& ctx, EditorCommandStack& commandStack, const EditorObject& obj)
        {
            ImGui::SeparatorText("Physics");
            // A copy of a "vessel" asset defaults to Dynamic and floating (SceneObjectFactory) -- a boat -- and takes the
            // row like any mesh: Static and floating rocks on the waves (the editor too), no helm; Static or None alone is
            // a mesh. Until 2026-11-09 the row was hidden for a vessel, so a placed yacht could be nothing but a boat.
            const bool vesselAsset = SetPhysicsCommand::VesselAsset(ctx, obj.id);
            const char* const labels[] = { "None", "Static", "Dynamic" };
            const physics::Motion motions[] = { physics::Motion::None, physics::Motion::Static, physics::Motion::Dynamic };
            const physics::Motion current = SetPhysicsCommand::DocumentMotion(obj, vesselAsset);
            float mass = SetPhysicsCommand::DocumentMass(obj);
            int index = current == physics::Motion::None ? 0 : current == physics::Motion::Dynamic ? 2 : 1;
            bool changed = ImGui::Combo("Body", &index, labels, 3);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Static: things collide with it (the default). Dynamic: it falls and is pushed in Play\n"
                    "(Level Editor > Play), as a convex hull of its solid slots.\n"
                    "None: no body. How it collides -- shape, slots, friction -- is the asset's: Mesh Editor > Collision.");
            }
            // A boat's body weighs its vessel's mass (PhysicsVessel SetVesselBody): this row moved nothing on one. Boat > Mass.
            if (motions[index] == physics::Motion::Dynamic && !vesselAsset)
            {
                ImGui::DragFloat("Mass (kg)", &mass, 0.5f, 0.0f, 100000.0f, mass > 0.0f ? "%.1f" : "from density");
                changed = changed || ImGui::IsItemDeactivatedAfterEdit();
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("0 = from the asset's density (water's by default)."); }
            }
            if (changed)
            {
                auto composite = std::make_unique<CompositeCommand>("Set Physics");
                for (const EditorObjectId id : ctx.selection.Ordered())
                {
                    const EditorObject* selected = ctx.document.Find(id);
                    if (selected && SetPhysicsCommand::AppliesTo(*selected))
                    {
                        composite->Add(std::make_unique<SetPhysicsCommand>(id, motions[index], mass));
                    }
                }
                if (composite->Empty()) { composite->Add(std::make_unique<SetPhysicsCommand>(obj.id, motions[index], mass)); }
                commandStack.Execute(ctx, std::move(composite));
            }
            if (vesselAsset)
            {
                const bool floats = SetBuoyantCommand::DocumentBuoyant(obj, true);
                const physics::Motion now = SetPhysicsCommand::DocumentMotion(obj, true);
                ImGui::TextDisabled("%s", now == physics::Motion::Dynamic && floats
                    ? "A boat: it floats and moves in Play, E takes the helm (Mesh Editor > Vessel)."
                    : floats ? "Not a boat: it rocks on the waves (in the editor too), no helm."
                             : "Not a boat: a mesh, it does not float.");
            }
        }

        // Plan L: offered only on a mesh that glows -- on any other the switch would move nothing.
        static void DrawGlowOnAt(EditorContext& ctx, EditorCommandStack& commandStack,
            const EditorObject& obj, const GBufferRenderable* gb)
        {
            if (!gb || !gb->HasEmissive()) { return; }
            ImGui::SeparatorText("Night Lamp");
            const LampSwitch current = gb->GetLampSwitch();
            static const char* const kLabels[] = { "Always", "At night", "By day" };
            int index = static_cast<int>(current);
            if (ImGui::Combo("Glow On At", &index, kLabels, 3) && index != static_cast<int>(current))
            {
                // Every selected glowing static mesh, one undo entry.
                const LampSwitch lampSwitch = static_cast<LampSwitch>(index);
                auto composite = std::make_unique<CompositeCommand>("Set Glow On At");
                for (const EditorObjectId id : ctx.selection.Ordered())
                {
                    const EditorObject* selected = ctx.document.Find(id);
                    RenderableObjectBase* runtime = selected ? ctx.scene.FindEditorObject(id.value) : nullptr;
                    const GBufferRenderable* other = runtime ? runtime->AsGBufferRenderable() : nullptr;
                    if (selected && selected->type == "staticMesh" && other && other->HasEmissive())
                    {
                        composite->Add(std::make_unique<SetLampSwitchCommand>(id, lampSwitch));
                    }
                }
                if (composite->Empty()) { composite->Add(std::make_unique<SetLampSwitchCommand>(obj.id, lampSwitch)); }
                commandStack.Execute(ctx, std::move(composite));
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "This mesh's glow (its emissive: a bulb, a lit window) comes on with the night lamps: when the\n"
                    "sun goes down past Time of Day > Lamps > Switch At, after the same delay and warm-up as a light\n"
                    "standing beside it. By day = the opposite.");
            }
            if (current != LampSwitch::Always)
            {
                const NightLamps& lamps = ctx.scene.GetNightLamps();
                ImGui::TextDisabled("Now: %s, glow at %.0f %%", lamps.Night() ? "night" : "day",
                                    lamps.Level(current, gb->GetPosition()) * 100.0f);
            }
        }

        // Offered only in a level that HAS an ocean: a switch with nothing to float on is not shown.
        static void DrawBuoyancy(EditorContext& ctx, EditorCommandStack& commandStack,
            const EditorObject& obj, RenderableObjectBase* runtime)
        {
            OceanRenderable* ocean = ctx.scene.FindOceanRenderable();
            if (!ocean) { return; }
            ImGui::SeparatorText("Buoyancy");
            // What it does: the document's "buoyant", else the asset's default -- a vessel floats unless it says false.
            bool buoyant = SetBuoyantCommand::DocumentBuoyant(obj, SetPhysicsCommand::VesselAsset(ctx, obj.id));
            if (ImGui::Checkbox("Floats on the ocean", &buoyant))
            {
                // Every selected static mesh, one undo entry.
                auto composite = std::make_unique<CompositeCommand>(
                    buoyant ? "Enable Buoyancy" : "Disable Buoyancy");
                for (const EditorObjectId id : ctx.selection.Ordered())
                {
                    const EditorObject* selected = ctx.document.Find(id);
                    if (selected && selected->type == "staticMesh")
                    {
                        composite->Add(std::make_unique<SetBuoyantCommand>(id, buoyant));
                    }
                }
                if (composite->Empty()) { composite->Add(std::make_unique<SetBuoyantCommand>(obj.id, buoyant)); }
                commandStack.Execute(ctx, std::move(composite));
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Heave, pitch and roll on the ocean's waves. Position and heading stay where you put\n"
                    "them; the saved transform is never touched.\n\n"
                    "Where it floats (pontoons, draft) and how it moves (inertia, damping) belong to the\n"
                    "MESH: Mesh Editor > Buoyancy. Waves need the ocean preset's readbackCascades set to\n"
                    "One or Two; with None everything floats on a flat surface.");
            }
            if (!ocean->IsVisibleGame())
            {
                ImGui::TextDisabled("The ocean is disabled -- nothing floats.");
                return;
            }
            if (!buoyant) { return; }
            // A DYNAMIC body floats in the physics world, by its shape's own volume (a vessel: its hull), and only in
            // Play; the pontoons are OceanBuoyancy's, for a Static body. Saying "no pontoons could be placed" lied.
            const bool vesselAsset = SetPhysicsCommand::VesselAsset(ctx, obj.id);
            if (SetPhysicsCommand::DocumentMotion(obj, vesselAsset) == physics::Motion::Dynamic)
            {
                ImGui::TextDisabled(vesselAsset
                    ? "Floats as a boat in Play: the physics world's hull. Pontoons are not used."
                    : "Floats in Play: the physics world, by its body's volume. Pontoons are not used.");
                return;
            }
            RenderableObject* object = runtime ? runtime->AsRenderableObject() : nullptr;
            std::vector<Math::float4> pontoons;
            if (object && ctx.scene.Buoyancy().GetWorldPontoons(object, pontoons))
            {
                const buoyancy::Settings& settings = object->GetBuoyancySettings();
                ImGui::TextDisabled("%d %s pontoons, inertia %.2f, damping %.2f",
                    static_cast<int>(pontoons.size()), settings.pontoons.empty() ? "automatic" : "authored",
                    settings.inertia, settings.damping);
            }
            else
            {
                ImGui::TextDisabled("Not floating yet (no pontoons could be placed on this mesh).");
            }
            const auto mesh = obj.properties.find("mesh");
            if (mesh != obj.properties.end() && mesh->is_string() && ctx.openMeshEditor &&
                ImGui::SmallButton("Edit pontoons in the Mesh Editor"))
            {
                ctx.openMeshEditor(mesh->get<std::string>() + "#buoyancy");
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

    // Where it stands is the transform above. What the player is like is here, in the level: a row never touched stays out of the file (docs/play_mode_plan.md).
    class PlayerStartPropertyDrawer final : public IEditorPropertyDrawer
    {
    public:
        std::string_view Type() const override { return game::kPlayerStartType; }

        void Draw(EditorContext& ctx,
            EditorCommandStack& commandStack,
            const AssetRegistry& registry,
            EditorObject& obj) const override
        {
            (void)registry;

            if (ImGui::Button("Where the camera looks"))
            {
                commandStack.Execute(ctx, std::make_unique<TransformObjectCommand>(
                    obj.id, obj.transform, editorplayerstart::BuildObject(ctx.scene).transform));
            }
            ImGui::SeparatorText("The player");
            const physics::PlayerSettings defaults;
            for (const game::PlayerKey& key : game::PlayerKeys())
            {
                float value = obj.properties.value(key.key, defaults.*(key.member));
                const bool edited = ImGui::DragFloat(key.label, &value, key.speed, key.lo, key.hi, key.format);
                if (ImGui::IsItemActivated()) { before_ = obj.properties; }
                if (edited) { obj.properties[key.key] = value; }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    commandStack.Execute(ctx, std::make_unique<EditObjectPropertiesCommand>(obj.id, before_, obj.properties, "Edit Player Start"));
                }
            }
            if (ImGui::Button("Back to the defaults"))
            {
                nlohmann::json after = obj.properties;
                for (const game::PlayerKey& key : game::PlayerKeys()) { after.erase(key.key); }
                commandStack.Execute(ctx, std::make_unique<EditObjectPropertiesCommand>(obj.id, obj.properties, std::move(after), "Edit Player Start"));
            }
        }

    private:
        mutable nlohmann::json before_; // what a drag started from: one undo entry for all of it
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
    registry.RegisterPropertyDrawer(std::make_unique<PlayerStartPropertyDrawer>());
}

#endif // WITH_EDITOR
