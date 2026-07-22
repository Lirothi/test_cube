#include "editor/ui/MeshEditorPanel.h"
#if WITH_EDITOR

#include "editor/assets/AssetRegistry.h"
#include "editor/assets/MaterialFileGen.h"
#include "rendering/meshes/MeshManager.h"

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "editor/EditorContext.h"
#include "editor/scene/EditorSceneDocument.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/renderables/RenderableObjectBase.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Material presets available to assign to a slot (harvested from the AssetRegistry file records).
std::vector<std::string> CollectPresets(const AssetRegistry& registry)
{
    std::vector<std::string> out;
    for (const EditorAssetRecord& rec : registry.Assets())
    {
        if (rec.id.type == EditorAssetType::MaterialPreset) { out.push_back(rec.id.key); }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// A combo over ("auto" + every preset). Returns true and writes `value` when the user picks a new
// entry. `allowAuto` offers the glTF "resolve from the asset" sentinel.
bool MaterialCombo(const char* label, std::string& value, const std::vector<std::string>& presets, bool allowAuto)
{
    bool changed = false;
    if (ImGui::BeginCombo(label, value.empty() ? "(none)" : value.c_str()))
    {
        if (allowAuto && ImGui::Selectable("auto", value == "auto")) { value = "auto"; changed = true; }
        for (const std::string& p : presets)
        {
            if (ImGui::Selectable(p.c_str(), value == p)) { value = p; changed = true; }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// True when the mesh-asset geometry is a glTF/GLB (only those carry auto-materials to promote).
// The geometry may carry a "#node:X"/"#N" selector suffix — strip it before checking the ext.
bool IsGltfGeometry(const std::string& geometry)
{
    std::string p = geometry.substr(0, geometry.find('#'));
    std::transform(p.begin(), p.end(), p.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return p.size() >= 5 && (p.compare(p.size() - 5, 5, ".gltf") == 0 ||
                             p.compare(p.size() - 4, 4, ".glb") == 0);
}

const char* PreviewModeLabel(EditorPreviewMode mode)
{
    switch (mode)
    {
    case EditorPreviewMode::Wireframe: return "Wireframe";
    case EditorPreviewMode::VertexNormals: return "Vertex Normals";
    default: return "Lit";
    }
}

void DrawMeshPreview(EditorContext& ctx,
    AssetRegistry& registry,
    MeshEditorPreviewScene& previewScene,
    MeshEditorPreviewCamera& camera,
    const MeshEditorPreviewLight& light,
    EditorPreviewMode& mode,
    std::uint32_t& lod,
    const std::string& path,
    const std::string& geometry,
    const std::vector<std::string>& materialSlots,
    const std::vector<std::uint32_t>& recomputeNormalSlots)
{
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float controlsHeight = ImGui::GetFrameHeightWithSpacing() +
        ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 canvasSize(
        std::max(1.0f, available.x),
        std::max(64.0f, available.y - controlsHeight));
    ImGui::InvisibleButton("##MeshPreview", canvasSize,
        ImGuiButtonFlags_MouseButtonLeft |
        ImGuiButtonFlags_MouseButtonRight |
        ImGuiButtonFlags_MouseButtonMiddle);

    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
    {
        camera.yaw -= io.MouseDelta.x * 0.01f;
        camera.yaw = std::remainder(camera.yaw, 6.2831853f);
        camera.pitch = std::clamp(camera.pitch + io.MouseDelta.y * 0.01f,
            -1.55334f, 1.55334f);
    }
    if (active &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)))
    {
        const float panSpeed = 0.0025f * camera.zoom;
        camera.panX -= io.MouseDelta.x * panSpeed;
        camera.panY += io.MouseDelta.y * panSpeed;
    }
    if (hovered && io.MouseWheel != 0.0f)
    {
        camera.zoom = std::clamp(camera.zoom * std::pow(0.84f, io.MouseWheel),
            0.12f, 8.0f);
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        camera = {};
    }

    const float framebufferScaleX = std::max(1.0f, io.DisplayFramebufferScale.x);
    const float framebufferScaleY = std::max(1.0f, io.DisplayFramebufferScale.y);
    const float requestedWidth = std::max(1.0f, canvasSize.x * framebufferScaleX);
    const float requestedHeight = std::max(1.0f, canvasSize.y * framebufferScaleY);
    const float resolutionScale = std::min(1.0f,
        1024.0f / std::max(requestedWidth, requestedHeight));
    const std::uint32_t renderWidth = static_cast<std::uint32_t>(std::clamp(
        std::round(requestedWidth * resolutionScale), 64.0f, 1024.0f));
    const std::uint32_t renderHeight = static_cast<std::uint32_t>(std::clamp(
        std::round(requestedHeight * resolutionScale), 64.0f, 1024.0f));
    const MeshEditorPreviewScene::View preview = previewScene.Update(ctx.renderer,
        path,
        geometry,
        materialSlots,
        recomputeNormalSlots,
        registry.Revision(),
        renderWidth,
        renderHeight,
        camera,
        light,
        mode,
        lod);

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg));

    if (preview.state == MeshEditorPreviewScene::State::Ready &&
        preview.texture != ImTextureID_Invalid)
    {
        drawList->AddImage(preview.texture, min, max);
    }
    else
    {
        const char* message = "Rendering preview...";
        if (preview.state == MeshEditorPreviewScene::State::Failed)
        {
            message = preview.error ? preview.error : "Preview rendering failed.";
        }
        const float wrapWidth = std::max(1.0f, canvasSize.x - 16.0f);
        const ImVec2 textSize = ImGui::CalcTextSize(message, nullptr, false, wrapWidth);
        drawList->AddText(nullptr, 0.0f,
            ImVec2(min.x + std::max(8.0f, (canvasSize.x - textSize.x) * 0.5f),
                min.y + std::max(8.0f, (canvasSize.y - textSize.y) * 0.5f)),
            ImGui::GetColorU32(ImGuiCol_TextDisabled),
            message,
            nullptr,
            wrapWidth);
    }

    drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border));
    if (ImGui::Button("Frame"))
    {
        camera = {};
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Reset the preview camera to fit the whole mesh.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    if (ImGui::BeginCombo("##meshPreviewMode", PreviewModeLabel(mode)))
    {
        constexpr EditorPreviewMode kModes[] = {
            EditorPreviewMode::Lit,
            EditorPreviewMode::Wireframe,
            EditorPreviewMode::VertexNormals
        };
        for (const EditorPreviewMode candidate : kModes)
        {
            const bool selected = candidate == mode;
            if (ImGui::Selectable(PreviewModeLabel(candidate), selected))
            {
                mode = candidate;
            }
            if (selected) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Choose lit material rendering, topology wireframe, or vertex-normal lines.");
    }
    ImGui::SameLine();
    const std::uint32_t lodCount = std::max(1u, preview.lodCount);
    lod = std::min(lod, lodCount - 1u);
    const std::string lodLabel = "LOD " + std::to_string(lod) + "/" +
        std::to_string(lodCount - 1u);
    ImGui::SetNextItemWidth(115.0f);
    ImGui::BeginDisabled(lodCount <= 1u);
    if (ImGui::BeginCombo("##meshPreviewLod", lodLabel.c_str()))
    {
        for (std::uint32_t candidate = 0; candidate < lodCount; ++candidate)
        {
            const bool selected = candidate == lod;
            const std::string candidateLabel = "LOD " + std::to_string(candidate);
            if (ImGui::Selectable(candidateLabel.c_str(), selected))
            {
                lod = candidate;
            }
            if (selected) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(lodCount > 1u
            ? "Render the mesh with the selected explicit LOD."
            : "This mesh has no generated LODs.");
    }
    ImGui::TextDisabled("LMB orbit | RMB/MMB pan | Wheel zoom | Double-click frame");
}

// Base name for a material file promoted from a glTF (parent folder when the file stem is the
// generic "scene"/"model"/"mesh", else the stem) — mirrors the importer's naming (I3).
std::string MaterialBaseName(const std::string& geometry)
{
    const std::string p = geometry.substr(0, geometry.find('#'));
    const fs::path fp(p);
    const std::string stem = fp.stem().string();
    std::string lo = stem;
    std::transform(lo.begin(), lo.end(), lo.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string folder = fp.parent_path().filename().string();
    return ((lo == "scene" || lo == "model" || lo == "mesh") && !folder.empty()) ? folder : stem;
}

} // namespace

void MeshEditorPanel::Open(const std::string& meshAssetPath)
{
    path_ = meshAssetPath;
    doc_ = nlohmann::json::object();
    loaded_ = false;
    status_.clear();

    slots_.clear();
    recomputeNormalSlots_.clear();
    previewCamera_ = {};
    previewLod_ = 0;

    std::ifstream f(path_);
    if (f)
    {
        std::stringstream ss; ss << f.rdbuf();
        nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false, /*ignore_comments=*/true);
        if (!j.is_discarded() && j.is_object()) { doc_ = std::move(j); loaded_ = true; }
    }
    if (!loaded_) { status_ = "Failed to load " + path_; return; }

    // Auto-size the material slots to the geometry's submesh count, then seed each from the file:
    // the `materials` array wins per-slot; else the scalar `material` seeds slot 0; rest default auto.
    const std::string geometry = doc_.value("geometry", std::string());
    const size_t slotCount = std::max<size_t>(1, MeshManager::CountSubmeshes(geometry));
    slots_.assign(slotCount, "auto");
    if (doc_.contains("materials") && doc_["materials"].is_array())
    {
        const auto& arr = doc_["materials"];
        for (size_t i = 0; i < slotCount && i < arr.size(); ++i)
        {
            if (arr[i].is_string()) { slots_[i] = arr[i].get<std::string>(); }
        }
    }
    else if (doc_.contains("material") && doc_["material"].is_string())
    {
        slots_[0] = doc_["material"].get<std::string>();
    }

    const auto normalSlots = doc_.find("recomputeNormalSlots");
    if (normalSlots != doc_.end() && normalSlots->is_array())
    {
        for (const nlohmann::json& value : *normalSlots)
        {
            if (!value.is_number_integer()) { continue; }
            const int64_t slot = value.get<int64_t>();
            if (slot >= 0 && static_cast<size_t>(slot) < slotCount)
            {
                recomputeNormalSlots_.push_back(static_cast<uint32_t>(slot));
            }
        }
        std::sort(recomputeNormalSlots_.begin(), recomputeNormalSlots_.end());
        recomputeNormalSlots_.erase(
            std::unique(recomputeNormalSlots_.begin(), recomputeNormalSlots_.end()),
            recomputeNormalSlots_.end());
    }
}

void MeshEditorPanel::SetPersistentState(const PersistentState& state)
{
    previewPaneRatio_ = std::clamp(state.previewPaneRatio, 0.1f, 0.9f);
    previewLight_ = state.previewLight;
    previewLight_.color.x = std::clamp(previewLight_.color.x, 0.0f, 1.0f);
    previewLight_.color.y = std::clamp(previewLight_.color.y, 0.0f, 1.0f);
    previewLight_.color.z = std::clamp(previewLight_.color.z, 0.0f, 1.0f);
    previewLight_.exposure = std::clamp(previewLight_.exposure, 0.0f, 100.0f);
    previewLight_.ambient = std::clamp(previewLight_.ambient, 0.0f, 10.0f);
}

void MeshEditorPanel::Draw(EditorContext& ctx, AssetRegistry& registry, bool* open,
    const OpenMaterialHandler& openMaterial)
{
    ImGui::SetNextWindowSize(ImVec2(1080.0f, 720.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(700.0f, 420.0f),
        ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin("Mesh Editor", open))
    {
        ImGui::End();
        return;
    }

    if (path_.empty())
    {
        ImGui::TextDisabled("Double-click a mesh asset (.mesh.json) in the content browser to edit it.");
        ImGui::End();
        return;
    }

    if (!loaded_)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", status_.c_str());
        ImGui::End();
        return;
    }

    constexpr float kMinPreviewWidth = 260.0f;
    constexpr float kMinSettingsWidth = 300.0f;
    constexpr float kSplitterWidth = 6.0f;
    const ImVec2 workspaceSize = ImGui::GetContentRegionAvail();
    const float splitWidth = std::max(1.0f, workspaceSize.x - kSplitterWidth);
    const float maxPreviewWidth = std::max(kMinPreviewWidth,
        workspaceSize.x - kMinSettingsWidth - kSplitterWidth);
    float previewPaneWidth = std::clamp(previewPaneRatio_ * splitWidth,
        kMinPreviewWidth, maxPreviewWidth);
    float settingsWidth = std::max(kMinSettingsWidth,
        workspaceSize.x - previewPaneWidth - kSplitterWidth);

    const ImVec2 workspaceOrigin = ImGui::GetCursorScreenPos();
    const ImRect splitterRect(
        ImVec2(workspaceOrigin.x + previewPaneWidth, workspaceOrigin.y),
        ImVec2(workspaceOrigin.x + previewPaneWidth + kSplitterWidth,
            workspaceOrigin.y + workspaceSize.y));
    if (ImGui::SplitterBehavior(splitterRect,
            ImGui::GetID("##meshEditorSplitter"),
            ImGuiAxis_X,
            &previewPaneWidth,
            &settingsWidth,
            kMinPreviewWidth,
            kMinSettingsWidth,
            3.0f,
            0.1f))
    {
        previewPaneRatio_ = std::clamp(previewPaneWidth / splitWidth, 0.1f, 0.9f);
    }

    ImGui::BeginChild("##meshPreviewPane",
        ImVec2(previewPaneWidth, workspaceSize.y),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawMeshPreview(ctx,
        registry,
        previewScene_,
        previewCamera_,
        previewLight_,
        previewMode_,
        previewLod_,
        path_,
        doc_.value("geometry", std::string()),
        slots_,
        recomputeNormalSlots_);
    ImGui::EndChild();

    ImGui::SameLine(0.0f, kSplitterWidth);
    ImGui::BeginChild("##meshSettingsPane",
        ImVec2(0.0f, workspaceSize.y),
        true);
    ImGui::TextUnformatted(fs::path(path_).filename().string().c_str());
    ImGui::TextDisabled("Geometry: %s",
        doc_.value("geometry", std::string("(none)")).c_str());
    ImGui::Spacing();

    ImGui::SeparatorText("Preview Directional Light");
    ImGui::ColorEdit3("Color", &previewLight_.color.x);
    ImGui::DragFloat("Exposure", &previewLight_.exposure, 0.05f, 0.0f, 100.0f);
    ImGui::DragFloat("Ambient", &previewLight_.ambient, 0.005f, 0.0f, 10.0f);
    ImGui::DragFloat3("Direction", &previewLight_.direction.x, 0.01f);
    if (ImGui::Button("Reset preview light"))
    {
        previewLight_ = {};
    }
    ImGui::Spacing();

    const std::vector<std::string> presets = CollectPresets(registry);
    const std::string geometry = doc_.value("geometry", std::string());
    const bool geometryIsGltf = IsGltfGeometry(geometry);

    // One material picker per submesh — the slot count is auto-detected from the geometry (Open()).
    ImGui::SeparatorText(slots_.size() == 1 ? "Material" : "Material slots (per submesh)");
    for (size_t i = 0; i < slots_.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        const std::string label = (slots_.size() == 1) ? std::string("Material") : ("slot " + std::to_string(i));
        MaterialCombo(label.c_str(), slots_[i], presets, /*allowAuto=*/true); // edits slots_[i] in place

        // An auto glTF slot has no editable file yet. Promote it first; named presets can be
        // opened directly in Material Editor without returning to the Content Browser.
        if (geometryIsGltf && slots_[i] == "auto")
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Save as material"))
            {
                const std::string name = MaterialBaseName(geometry) + "_" + std::to_string(i);
                const std::string written = materialgen::WriteFromGltf(
                    geometry, static_cast<int>(i), name, /*overwrite=*/false);
                if (!written.empty() && written != "auto")
                {
                    slots_[i] = written;
                    registry.Refresh();
                    status_ = "Created data/materials/" + written + ".json - Save to apply.";
                }
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Bake this glTF auto-material into an editable data/materials file.");
            }
        }
        else if (!slots_[i].empty() && slots_[i] != "auto")
        {
            const EditorAssetId materialId{ EditorAssetType::MaterialPreset, slots_[i] };
            const EditorAssetRecord* material = registry.FindById(materialId);
            ImGui::SameLine();
            ImGui::BeginDisabled(material == nullptr || !openMaterial);
            if (ImGui::SmallButton("Edit material") && material && openMaterial)
            {
                openMaterial(material->id.key, material->path);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip(material
                    ? "Open this slot in Material Editor."
                    : "The material preset is not present in the Asset Registry.");
            }
        }

        const uint32_t normalSlot = static_cast<uint32_t>(i);
        bool recomputeNormals = std::binary_search(
            recomputeNormalSlots_.begin(), recomputeNormalSlots_.end(), normalSlot);
        if (ImGui::Checkbox("Recompute vertex normals", &recomputeNormals))
        {
            if (recomputeNormals)
            {
                recomputeNormalSlots_.push_back(normalSlot);
                std::sort(recomputeNormalSlots_.begin(), recomputeNormalSlots_.end());
            }
            else
            {
                recomputeNormalSlots_.erase(std::remove(
                    recomputeNormalSlots_.begin(), recomputeNormalSlots_.end(), normalSlot),
                    recomputeNormalSlots_.end());
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Discard authored normals for this submesh and regenerate them from triangles.\n"
                              "Tangents are regenerated from the resulting normals and UVs.");
        }

        ImGui::PopID();
    }

    // Render layer (absent = Default/opaque).
    ImGui::Spacing();
    {
        static const char* kLayers[] = { "(default)", "Terrain", "Transparent", "Sky" };
        const std::string cur = doc_.value("renderLayer", std::string());
        const std::string curLabel = cur.empty() ? "(default)" : cur;
        if (ImGui::BeginCombo("Render Layer", curLabel.c_str()))
        {
            for (const char* layer : kLayers)
            {
                const bool isDefault = (std::string(layer) == "(default)");
                const bool selected = isDefault ? cur.empty() : (cur == layer);
                if (ImGui::Selectable(layer, selected))
                {
                    if (isDefault) { doc_.erase("renderLayer"); }
                    else { doc_["renderLayer"] = layer; }
                }
            }
            ImGui::EndCombo();
        }
    }

    // Spawn scale (default placement scale written into new level entries).
    {
        float scale = doc_.value("spawnScale", 1.0f);
        if (ImGui::DragFloat("Spawn Scale", &scale, 0.001f, 0.0001f, 10000.0f, "%.4f"))
        {
            doc_["spawnScale"] = scale;
        }
    }

    // Texture offset/scale tiling [offsX, offsY, scaleX, scaleY].
    {
        float tos[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        const auto& j = doc_["texOffsScale"];
        if (j.is_array() && j.size() == 4)
        {
            for (int k = 0; k < 4; ++k) { tos[k] = j[k].is_number() ? j[k].get<float>() : tos[k]; }
        }
        if (ImGui::DragFloat4("Tex Offset/Scale", tos, 0.01f))
        {
            doc_["texOffsScale"] = { tos[0], tos[1], tos[2], tos[3] };
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Save"))
    {
        Save(ctx, registry);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(applies live to placed instances that don't override the field)");
    if (!status_.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", status_.c_str());
    }

    ImGui::EndChild();
    ImGui::End();
}

// Paths compare equal regardless of slash direction (registry uses '/', level JSON may vary).
static std::string NormalizeSlashes(std::string s)
{
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

int MeshEditorPanel::ApplyToScene(EditorContext& ctx) const
{
    const std::string wantMesh = NormalizeSlashes(path_);

    // Collect placed static-mesh objects that reference THIS mesh asset. (Transparent meshes would
    // need CreateTransparentMeshFromJson; they're not the mesh-asset use case — mirror the material
    // commands, which are staticMesh-only.)
    std::vector<EditorObjectId> targets;
    for (const EditorObject& obj : ctx.document.Objects())
    {
        if (obj.type != "staticMesh") { continue; }
        const std::string m = NormalizeSlashes(obj.properties.value("mesh", std::string()));
        if (!m.empty() && m == wantMesh) { targets.push_back(obj.id); }
    }
    if (targets.empty()) { return 0; }

    // Respawn each (CreateStaticMeshFromJson re-runs ResolveMeshAsset, which re-reads the just-saved
    // mesh.json — so the new defaults land, while any per-object override still wins). One GPU sync
    // for the whole batch rather than per-object (a grove can be dozens of instances).
    ctx.renderer.WaitForPreviousFrame();
    UploadBatch uploads;
    if (!uploads.Begin(&ctx.renderer)) { return 0; }

    int applied = 0;
    for (const EditorObjectId id : targets)
    {
        if (ctx.scene.FindEditorObject(id.value) == nullptr) { continue; } // no live runtime (disabled)
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
        // Like Material Editor, this direct live-apply bypasses EditorCommandStack's automatic
        // caster refresh. Rebuild mesh groups and material-owned masked SRV handles explicitly.
        ctx.scene.RefreshShadowGpuForEditor(ctx.renderer);
        ctx.scene.InvalidateRaytracing();
    }
    return applied;
}

void MeshEditorPanel::Save(EditorContext& ctx, AssetRegistry& registry)
{
    // Fold the per-slot picks back into the document. Compact form when nothing overrides the
    // default: a single slot, or an all-"auto" glTF, collapses to the scalar `material` (for glTF
    // that scalar "auto" still resolves every submesh from the glTF's own materials).
    bool allAuto = true;
    for (const std::string& s : slots_) { if (s != "auto") { allAuto = false; break; } }

    if (slots_.size() <= 1 || allAuto)
    {
        doc_.erase("materials");
        doc_["material"] = slots_.empty() ? std::string("auto") : slots_[0];
    }
    else
    {
        doc_.erase("material");
        doc_["materials"] = slots_; // per-slot list; slot i = entry i
    }

    if (recomputeNormalSlots_.empty()) { doc_.erase("recomputeNormalSlots"); }
    else { doc_["recomputeNormalSlots"] = recomputeNormalSlots_; }

    std::ofstream out(path_, std::ios::trunc);
    if (!out)
    {
        status_ = "Save FAILED (not writable): " + path_;
        return;
    }
    out << doc_.dump(2) << "\n";
    out.close();
    registry.Refresh(); // re-index so any dependent views pick up the change

    // Live-apply to placed instances (must happen AFTER the file is written — the respawn re-reads it).
    const int applied = ApplyToScene(ctx);
    status_ = applied > 0 ? ("Saved — updated " + std::to_string(applied) + " placed instance(s).")
                          : "Saved.";
}

#endif // WITH_EDITOR
