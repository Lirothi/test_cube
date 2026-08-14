#include "editor/ui/MaterialEditorPanel.h"
#if WITH_EDITOR

#include "editor/assets/AssetRegistry.h"
#include "editor/ui/EditorDragDrop.h"

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "editor/EditorContext.h"
#include "editor/scene/EditorSceneDocument.h"
#include "materials/MaterialDataManager.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/renderables/RenderableObjectBase.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<std::string> CollectTextures(const AssetRegistry& registry)
{
    std::vector<std::string> out;
    for (const EditorAssetRecord& rec : registry.Assets())
    {
        if (rec.id.type == EditorAssetType::Texture) { out.push_back(rec.path); }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string ToLowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Texture picker over ("(none)" + every registry texture) with a live search box at the top of the
// dropdown (case-insensitive substring — the texture list is long). Edits doc_[key] in place: sets
// the chosen path, or erases the key for "(none)". H2 resolves the .dds sibling at load, so either a
// .dds or .png path works.
void TexturePicker(const char* label, nlohmann::json& doc, const char* key,
    const std::vector<std::string>& textures)
{
    // Which picker's dropdown is currently open (one at a time) + its filter text. Function-static
    // so the open/close transition is visible across both BeginCombo branches.
    static const void* openPicker = nullptr;
    static char filter[128] = {};

    const std::string cur = doc.contains(key) && doc[key].is_string()
        ? doc[key].get<std::string>() : std::string();
    if (ImGui::BeginCombo(label, cur.empty() ? "(none)" : cur.c_str()))
    {
        if (openPicker != key)  // just opened -> reset + focus the search box
        {
            openPicker = key;
            filter[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##texfilter", "search...", filter, sizeof(filter));
        ImGui::Separator();

        const std::string needle = ToLowerCopy(filter);
        if (ImGui::BeginChild("##texlist", ImVec2(0.0f, 220.0f), false))
        {
            if (needle.empty() && ImGui::Selectable("(none)", cur.empty())) { doc.erase(key); }
            for (const std::string& t : textures)
            {
                if (!needle.empty() && ToLowerCopy(t).find(needle) == std::string::npos) { continue; }
                if (ImGui::Selectable(t.c_str(), cur == t)) { doc[key] = t; }
            }
        }
        ImGui::EndChild();
        ImGui::EndCombo();
    }
    else if (openPicker == key)
    {
        openPicker = nullptr; // this picker's dropdown closed -> re-focus/reset on next open
    }

    // Drop target: drag a texture from the content browser onto the field to assign it. The CB's
    // asset payload carries the texture's key (= its path for texture records), which is exactly
    // what the material file stores. Non-texture payloads are refused with a hint.
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                EditorDragDrop::kAssetPayloadType, ImGuiDragDropFlags_AcceptBeforeDelivery))
        {
            EditorAssetId id;
            if (EditorDragDrop::DecodeAssetPayload(payload, id))
            {
                if (id.type != EditorAssetType::Texture)
                {
                    ImGui::SetTooltip("Only textures can be dropped here.");
                }
                else if (payload->IsDelivery())
                {
                    doc[key] = id.key;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
}

// Read an N-float array from doc[key] into `v`, filling from `def` where absent/short.
void ReadFloats(const nlohmann::json& doc, const char* key, float* v, int n, const float* def)
{
    for (int i = 0; i < n; ++i) { v[i] = def[i]; }
    const auto it = doc.find(key);
    if (it != doc.end() && it->is_array())
    {
        for (int i = 0; i < n && i < static_cast<int>(it->size()); ++i)
        {
            if ((*it)[i].is_number()) { v[i] = (*it)[i].get<float>(); }
        }
    }
}

void WriteFloats(nlohmann::json& doc, const char* key, const float* v, int n)
{
    nlohmann::json arr = nlohmann::json::array();
    for (int i = 0; i < n; ++i) { arr.push_back(v[i]); }
    doc[key] = std::move(arr);
}

void CollectMaterialNames(const nlohmann::json& eff, std::vector<std::string>& out)
{
    if (eff.contains("material") && eff["material"].is_string())
    {
        out.push_back(eff["material"].get<std::string>());
    }
    if (eff.contains("materials") && eff["materials"].is_array())
    {
        for (const auto& e : eff["materials"])
        {
            if (e.is_string()) { out.push_back(e.get<std::string>()); }
        }
    }
}

} // namespace

void MaterialEditorPanel::Open(const std::string& materialName, const std::string& filePath)
{
    name_ = materialName;
    path_ = filePath;
    doc_ = nlohmann::json::object();
    loaded_ = false;
    status_.clear();

    std::ifstream f(path_);
    if (f)
    {
        std::stringstream ss; ss << f.rdbuf();
        nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false, /*ignore_comments=*/true);
        if (!j.is_discarded() && j.is_object()) { doc_ = std::move(j); loaded_ = true; }
    }
    if (!loaded_) { status_ = "Failed to load " + path_; }
}

void MaterialEditorPanel::Draw(EditorContext& ctx, AssetRegistry& registry, bool* open)
{
    if (!ImGui::Begin("Material Editor", open))
    {
        ImGui::End();
        return;
    }

    if (name_.empty())
    {
        ImGui::TextDisabled("Double-click a material in the content browser to edit it.");
        ImGui::End();
        return;
    }

    ImGui::Text("%s", name_.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", path_.c_str());
    ImGui::Separator();

    if (!loaded_)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", status_.c_str());
        ImGui::End();
        return;
    }

    const std::vector<std::string> textures = CollectTextures(registry);

    ImGui::SeparatorText("Textures");
    TexturePicker("Albedo", doc_, "albedo", textures);
    TexturePicker("Metal/Rough", doc_, "mr", textures);
    bool useMR = doc_.value("useMR", true);
    if (ImGui::Checkbox("Use MR Texture", &useMR)) { doc_["useMR"] = useMR; }
    TexturePicker("Normal", doc_, "normal", textures);
    TexturePicker("Emissive", doc_, "emissive", textures);

    ImGui::SeparatorText("Parameters");
    {
        static const float kTintDef[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float tint[4]; ReadFloats(doc_, "tint", tint, 4, kTintDef);
        if (ImGui::ColorEdit4("Tint", tint)) { WriteFloats(doc_, "tint", tint, 4); }

        static const float kMrDef[2] = { 0.0f, 0.35f };
        float mr[2]; ReadFloats(doc_, "metalRough", mr, 2, kMrDef);
        if (ImGui::DragFloat2("Metal / Rough", mr, 0.01f, 0.0f, 1.0f)) { WriteFloats(doc_, "metalRough", mr, 2); }

        bool multiplyMR = doc_.value("multiplyMR", false);
        ImGui::BeginDisabled(!useMR);
        if (ImGui::Checkbox("Multiply MR texture by values", &multiplyMR)) { doc_["multiplyMR"] = multiplyMR; }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Checked: final MR = texture * Metal/Rough values.\n"
                              "Unchecked: the texture overrides the values.");
        }
        ImGui::EndDisabled();

        float ns = doc_.value("normalStrength", 1.0f);
        if (ImGui::DragFloat("Normal Strength", &ns, 0.01f, 0.0f, 4.0f)) { doc_["normalStrength"] = ns; }

        static const float kTosDef[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        float tos[4]; ReadFloats(doc_, "texOffsScale", tos, 4, kTosDef);
        if (ImGui::DragFloat4("Tex Offset / Scale", tos, 0.01f)) { WriteFloats(doc_, "texOffsScale", tos, 4); }
    }

    ImGui::SeparatorText("Emissive");
    {
        static const float kEmDef[3] = { 0.0f, 0.0f, 0.0f };
        float em[3]; ReadFloats(doc_, "emissiveColor", em, 3, kEmDef);
        if (ImGui::ColorEdit3("Emissive Color", em)) { WriteFloats(doc_, "emissiveColor", em, 3); }
        float es = doc_.value("emissiveStrength", 0.0f);
        if (ImGui::DragFloat("Emissive Strength", &es, 0.05f, 0.0f, 100.0f)) { doc_["emissiveStrength"] = es; }
    }

    ImGui::SeparatorText("Surface flags");
    {
        ShadingModel shadingModel = ShadingModel::DefaultLit;
        bool shadingModelValid = true;
        if (const auto it = doc_.find("shadingModel"); it != doc_.end())
        {
            shadingModelValid = it->is_string() &&
                TryParseShadingModel(it->get_ref<const std::string&>(), shadingModel);
        }

        const char* shadingModelPreview = "Default Lit";
        if (!shadingModelValid) { shadingModelPreview = "Invalid (Default Lit fallback)"; }
        else if (shadingModel == ShadingModel::TwoSidedFoliage) { shadingModelPreview = "Two-Sided Foliage"; }
        else if (shadingModel == ShadingModel::Terrain) { shadingModelPreview = "Terrain"; }

        if (ImGui::BeginCombo("Shading Model", shadingModelPreview))
        {
            struct ShadingModelOption
            {
                ShadingModel value;
                const char* label;
            };
            constexpr ShadingModelOption kOptions[] = {
                { ShadingModel::DefaultLit, "Default Lit" },
                { ShadingModel::TwoSidedFoliage, "Two-Sided Foliage" },
                { ShadingModel::Terrain, "Terrain" }
            };
            for (const ShadingModelOption& option : kOptions)
            {
                const bool selected = shadingModelValid && shadingModel == option.value;
                if (ImGui::Selectable(option.label, selected))
                {
                    shadingModel = option.value;
                    shadingModelValid = true;
                    doc_["shadingModel"] = ShadingModelToString(option.value);
                    doc_["twoSided"] = option.value == ShadingModel::TwoSidedFoliage;
                }
                if (selected) { ImGui::SetItemDefaultFocus(); }
            }
            ImGui::EndCombo();
        }
        if (!shadingModelValid)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                "Unknown shadingModel. Runtime uses Default Lit until a valid value is selected.");
        }

        const bool foliage = shadingModelValid && shadingModel == ShadingModel::TwoSidedFoliage;
        const bool terrain = shadingModelValid && shadingModel == ShadingModel::Terrain;
        if (foliage) { doc_["twoSided"] = true; }

        bool twoSided = foliage || doc_.value("twoSided", false);
        ImGui::BeginDisabled(foliage);
        if (ImGui::Checkbox("Two Sided (render backfaces)", &twoSided))
        {
            doc_["twoSided"] = twoSided;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(foliage
                ? "Two-Sided Foliage always renders both faces."
                : "Disable back-face culling and render both sides of every triangle.");
        }

        if (foliage)
        {
            ImGui::Spacing();
            ImGui::SeparatorText("Foliage");

            static const float kSubsurfaceDef[3] = { 1.0f, 1.0f, 1.0f };
            float subsurface[3];
            ReadFloats(doc_, "subsurfaceColor", subsurface, 3, kSubsurfaceDef);
            if (ImGui::ColorEdit3("Subsurface Color", subsurface))
            {
                WriteFloats(doc_, "subsurfaceColor", subsurface, 3);
            }
            float transmission = doc_.value("transmissionStrength", 0.0f);
            if (ImGui::DragFloat("Transmission Strength", &transmission, 0.01f, 0.0f, 1.0f))
            {
                doc_["transmissionStrength"] = transmission;
            }
            const float serializedAlbedoPower = doc_.value("transmissionAlbedoPower", 0.6f);
            float albedoPower = std::clamp(serializedAlbedoPower, 0.0f, 4.0f);
            if (albedoPower != serializedAlbedoPower)
            {
                doc_["transmissionAlbedoPower"] = albedoPower;
            }
            if (ImGui::DragFloat("Transmission Albedo Power", &albedoPower, 0.01f, 0.0f, 4.0f,
                                 "%.3f", ImGuiSliderFlags_AlwaysClamp))
            {
                doc_["transmissionAlbedoPower"] = std::clamp(albedoPower, 0.0f, 4.0f);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Nonlinear absorption derived from linear albedo (T = albedo^power).\n"
                                  "0 gives uniform transmission; larger values darken stems and veins.");
            }
            const float serializedNormalWeight = doc_.value("transmissionNormalWeight", 0.35f);
            float normalWeight = std::clamp(serializedNormalWeight, 0.0f, 1.0f);
            if (normalWeight != serializedNormalWeight)
            {
                doc_["transmissionNormalWeight"] = normalWeight;
            }
            if (ImGui::DragFloat("Transmission Normal Weight", &normalWeight, 0.01f, 0.0f, 1.0f,
                                 "%.3f", ImGuiSliderFlags_AlwaysClamp))
            {
                doc_["transmissionNormalWeight"] = std::clamp(normalWeight, 0.0f, 1.0f);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("0 keeps broad two-sided wrap; 1 uses abs(N.L) projected-area weighting.\n"
                                  "Fresnel still suppresses grazing-angle transmission.");
            }
        }
        if (terrain)
        {
            ImGui::Spacing();
            ImGui::SeparatorText("Terrain de-tiling");

            float zoneSize = std::clamp(doc_.value("terrainZoneSize", 4.0f), 0.25f, 64.0f);
            if (ImGui::DragFloat("Zone Size (texture repeats)", &zoneSize, 0.05f, 0.25f, 64.0f,
                                 "%.2f", ImGuiSliderFlags_AlwaysClamp))
            {
                doc_["terrainZoneSize"] = std::clamp(zoneSize, 0.25f, 64.0f);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Average Voronoi zone size after Tex Offset / Scale.\n"
                                  "Larger zones preserve longer continuous stretches of the source texture.");
            }

            float rotation = std::clamp(doc_.value("terrainRotation", 180.0f), 0.0f, 180.0f);
            if (ImGui::DragFloat("Random Rotation (degrees)", &rotation, 1.0f, 0.0f, 180.0f,
                                 "%.0f", ImGuiSliderFlags_AlwaysClamp))
            {
                doc_["terrainRotation"] = std::clamp(rotation, 0.0f, 180.0f);
            }

            float scaleVariation =
                std::clamp(doc_.value("terrainScaleVariation", 0.25f), 0.0f, 0.75f);
            if (ImGui::DragFloat("Random Scale Variation", &scaleVariation, 0.01f, 0.0f, 0.75f,
                                 "%.2f", ImGuiSliderFlags_AlwaysClamp))
            {
                doc_["terrainScaleVariation"] = std::clamp(scaleVariation, 0.0f, 0.75f);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("0 keeps scale at 1. A value of 0.25 chooses a stable per-zone\n"
                                  "scale in the range 0.75..1.25.");
            }

            float blend = std::clamp(doc_.value("terrainBlend", 0.35f), 0.0f, 1.0f);
            if (ImGui::DragFloat("Zone Edge Blend", &blend, 0.01f, 0.0f, 1.0f,
                                 "%.2f", ImGuiSliderFlags_AlwaysClamp))
            {
                doc_["terrainBlend"] = std::clamp(blend, 0.0f, 1.0f);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Blends the three nearest transformed zones.\n"
                                  "0 is a hard boundary; larger values hide seams more broadly.");
            }

            float edgeBreakup =
                std::clamp(doc_.value("terrainEdgeBreakup", 0.09f), 0.0f, 0.45f);
            if (ImGui::DragFloat("Edge Breakup", &edgeBreakup, 0.005f, 0.0f, 0.45f,
                                 "%.3f", ImGuiSliderFlags_AlwaysClamp))
            {
                doc_["terrainEdgeBreakup"] = std::clamp(edgeBreakup, 0.0f, 0.45f);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Warps only the Voronoi zone mask, producing torn irregular edges.\n"
                                  "0 restores clean geometric cell boundaries.\n"
                                  "Capped against Edge Detail so the zone map cannot fold.");
            }

            float edgeDetail =
                std::clamp(doc_.value("terrainEdgeDetail", 3.5f), 0.5f, 12.0f);
            if (ImGui::DragFloat("Edge Detail", &edgeDetail, 0.05f, 0.5f, 12.0f,
                                 "%.2f", ImGuiSliderFlags_AlwaysClamp))
            {
                doc_["terrainEdgeDetail"] = std::clamp(edgeDetail, 0.5f, 12.0f);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Noise frequency along zone borders. Higher values make smaller,\n"
                                  "more ragged notches without changing source texture UVs.");
            }
        }
        const float serializedIndirectSpecularScale = doc_.value("indirectSpecularScale", 1.0f);
        float indirectSpecularScale = std::clamp(serializedIndirectSpecularScale, 0.0f, 1.0f);
        if (indirectSpecularScale != serializedIndirectSpecularScale)
        {
            doc_["indirectSpecularScale"] = indirectSpecularScale;
        }
        if (ImGui::DragFloat("Indirect Specular Scale", &indirectSpecularScale, 0.01f, 0.0f, 1.0f,
                             "%.3f", ImGuiSliderFlags_AlwaysClamp))
        {
            doc_["indirectSpecularScale"] = std::clamp(indirectSpecularScale, 0.0f, 1.0f);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Scales only indirect sky/SSR/RT reflections.\n"
                              "Direct specular, diffuse, transmission and emissive are unchanged.");
        }
        float ambientOcclusion = doc_.value("ambientOcclusion", 1.0f);
        if (ImGui::DragFloat("Ambient Occlusion", &ambientOcclusion, 0.01f, 0.0f, 1.0f))
        {
            doc_["ambientOcclusion"] = ambientOcclusion;
        }

        bool alphaTest = doc_.value("alphaTest", false);
        if (ImGui::Checkbox("Alpha test (masked)", &alphaTest)) { doc_["alphaTest"] = alphaTest; }
        if (alphaTest)
        {
            float cutoff = doc_.value("alphaCutoff", 0.5f);
            if (ImGui::DragFloat("Alpha cutoff", &cutoff, 0.01f, 0.0f, 1.0f)) { doc_["alphaCutoff"] = cutoff; }
        }
        bool normalIsRG = doc_.value("normalIsRG", true);
        if (ImGui::Checkbox("Normal map is RG (BC5)", &normalIsRG)) { doc_["normalIsRG"] = normalIsRG; }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Shading model / alpha test / two-sided / normal-RG change the slot's PSO — Save\n"
                              "respawns referencing objects so it takes effect.");
        }
    }

    ImGui::SeparatorText("Feature shader (optional)");
    {
        char buf[260] = {};
        const std::string cur = doc_.value("shader", std::string());
        std::snprintf(buf, sizeof(buf), "%s", cur.c_str());
        if (ImGui::InputTextWithHint("##shader", "shaders/gbuffer.hlsl (default)", buf, sizeof(buf)))
        {
            if (buf[0] == '\0') { doc_.erase("shader"); }
            else { doc_["shader"] = std::string(buf); }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Override the gbuffer shader for feature materials (e.g. vegetation\n"
                              "sway). Must keep the PerObject b0 layout and ship a <name>_csm.hlsl\n"
                              "shadow counterpart. Leave empty for the default.");
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Save & Apply"))
    {
        Save(ctx, registry);
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert"))
    {
        Open(name_, path_); // reload from disk
    }
    if (!status_.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", status_.c_str());
    }

    ImGui::End();
}

int MaterialEditorPanel::ApplyToScene(EditorContext& ctx) const
{
    // Collect placed static meshes whose EFFECTIVE material (after mesh-asset + slot resolution) is
    // this one. Resolving via ResolveMeshAsset means both inline `material`/`materials` overrides
    // AND materials inherited from a `.mesh.json` are covered.
    std::vector<EditorObjectId> targets;
    for (const EditorObject& obj : ctx.document.Objects())
    {
        if (obj.type != "staticMesh") { continue; }
        const nlohmann::json eff =
            SceneObjectFactory::ResolveMeshAsset(EditorSceneDocument::ObjectToJson(obj));
        std::vector<std::string> names;
        CollectMaterialNames(eff, names);
        if (std::find(names.begin(), names.end(), name_) != names.end())
        {
            targets.push_back(obj.id);
        }
    }
    if (targets.empty()) { return 0; }

    // One GPU sync for the whole batch. The cache was already evicted, so each respawn's
    // GetOrCreate rebuilds this material fresh (new textures / params / PSO flags).
    ctx.renderer.WaitForPreviousFrame();
    UploadBatch uploads;
    if (!uploads.Begin(&ctx.renderer)) { return 0; }

    int applied = 0;
    for (const EditorObjectId id : targets)
    {
        if (ctx.scene.FindEditorObject(id.value) == nullptr) { continue; } // disabled -> no runtime
        const EditorObject* obj = ctx.document.Find(id);
        if (!obj) { continue; }
        const nlohmann::json json = EditorSceneDocument::ObjectToJson(*obj);
        std::unique_ptr<RenderableObjectBase> runtime = SceneObjectFactory::CreateStaticMeshFromJson(json);
        if (!runtime) { continue; }
        ctx.scene.RemoveEditorObject(id.value);
        ctx.scene.AddInitializedEditorObject(ctx.renderer, uploads, id.value, std::move(runtime));
        ++applied;
    }
    uploads.SubmitAndWait(&ctx.renderer);

    if (applied > 0)
    {
        // This direct live-apply bypasses EditorCommandStack. Rebuild the shadow caster data now:
        // masked palm groups cache MaterialData-owned albedo SRVs, and destroying the old runtime
        // objects invalidated those handles even though the caster count stayed unchanged.
        ctx.scene.RefreshShadowGpuForEditor(ctx.renderer);

        // The rebuilt material has NEW albedo/MR SRVs; the RT bindless caches the old ones per-mesh,
        // so drop the RT caches too. Next RT frame re-registers with the current SRVs.
        ctx.scene.InvalidateRaytracing();
    }
    return applied;
}

void MaterialEditorPanel::Save(EditorContext& ctx, AssetRegistry& registry)
{
    std::ofstream out(path_, std::ios::trunc);
    if (!out)
    {
        status_ = "Save FAILED (not writable): " + path_;
        return;
    }
    out << doc_.dump(2) << "\n";
    out.close();

    // Re-register the definition + drop the stale built MaterialData, THEN respawn users so their
    // GetOrCreate rebuilds from the new file. Order matters: register -> evict -> respawn.
    MaterialDataManager* mgr = ctx.renderer.GetMaterialDataManager();
    if (mgr)
    {
        mgr->LoadPresetFromFile(std::wstring(path_.begin(), path_.end()));
        mgr->EvictCached(name_);
    }
    registry.Refresh(); // re-index for CB thumbnails/badges

    const int applied = ApplyToScene(ctx);
    status_ = "Saved — updated " + std::to_string(applied) + " object(s).";
}

#endif // WITH_EDITOR
