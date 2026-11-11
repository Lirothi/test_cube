#include "editor/ui/MeshEditorPanel.h"
#if WITH_EDITOR

#include "editor/assets/AssetRegistry.h"
#include "editor/assets/MaterialFileGen.h"
#include "editor/assets/MeshRestPose.h"
#include "rendering/meshes/MeshManager.h"
#include "meshoptimizer.h" // lodPermissive/lodDropSmallParts -> meshopt simplify flags

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "editor/EditorContext.h"
#include "editor/scene/EditorSceneDocument.h"
#include "editor/ui/EditorLightDirection.h"
#include "ocean/BuoyancyJson.h"
#include "physics/CollisionJson.h"
#include "rendering/core/ExposureMetering.h"
#include "rendering/core/PhotographicSettings.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/renderables/RenderableObjectBase.h"
#include "rendering/shadows/TessySettings.h" // the Tessy section: the process's DAG prune / fold and tessy.lod

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <system_error>
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

std::string ToLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// A searchable combo over ("auto" + every preset). Returns true and writes `value` when the user
// picks a new entry. `allowAuto` offers the glTF "resolve from the asset" sentinel.
bool MaterialCombo(const char* label, std::string& value, const std::vector<std::string>& presets, bool allowAuto)
{
    static ImGuiID openCombo = 0;
    static char filter[128] = {};

    bool changed = false;
    const ImGuiID comboId = ImGui::GetID(label);
    if (ImGui::BeginCombo(label,
        value.empty() ? "(none)" : value.c_str(),
        ImGuiComboFlags_HeightLargest))
    {
        if (openCombo != comboId)
        {
            openCombo = comboId;
            filter[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##materialFilter", "Search materials...", filter, sizeof(filter));
        ImGui::Separator();

        const std::string needle = ToLowerCopy(filter);
        int visibleCount = 0;
        if (ImGui::BeginChild("##materialList", ImVec2(0.0f, 220.0f), false))
        {
            if (allowAuto && (needle.empty() || std::string("auto").find(needle) != std::string::npos))
            {
                ++visibleCount;
                if (ImGui::Selectable("auto", value == "auto") && value != "auto")
                {
                    value = "auto";
                    changed = true;
                }
            }
            for (const std::string& p : presets)
            {
                if (!needle.empty() && ToLowerCopy(p).find(needle) == std::string::npos) { continue; }
                ++visibleCount;
                if (ImGui::Selectable(p.c_str(), value == p) && value != p)
                {
                    value = p;
                    changed = true;
                }
            }
            if (visibleCount == 0) { ImGui::TextDisabled("No matching materials."); }
        }
        ImGui::EndChild();
        ImGui::EndCombo();
    }
    else if (openCombo == comboId)
    {
        openCombo = 0;
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

void DrawLocalAxesOverlay(ImDrawList* drawList,
    const ImVec2& previewMin,
    const ImVec2& previewMax,
    const MeshEditorPreviewCamera& camera)
{
    const float pitch = std::clamp(camera.pitch, -1.55334f, 1.55334f);
    const float cosPitch = std::cos(pitch);
    const Math::float3 cameraOffset(
        cosPitch * std::sin(camera.yaw),
        std::sin(pitch),
        -cosPitch * std::cos(camera.yaw));
    const Math::float3 worldUp(0.0f, 1.0f, 0.0f);
    const Math::float3 right = cameraOffset.Cross(worldUp).Normalized();
    const Math::float3 cameraUp = right.Cross(cameraOffset).Normalized();

    const ImVec2 origin(previewMin.x + 34.0f, previewMax.y - 34.0f);
    constexpr float kAxisLength = 23.0f;
    struct Axis
    {
        Math::float3 direction;
        ImU32 color;
        const char* label;
    };
    const Axis axes[] = {
        { Math::float3(1.0f, 0.0f, 0.0f), IM_COL32(235, 80, 80, 255), "X" },
        { Math::float3(0.0f, 1.0f, 0.0f), IM_COL32(90, 210, 105, 255), "Y" },
        { Math::float3(0.0f, 0.0f, 1.0f), IM_COL32(80, 145, 255, 255), "Z" }
    };

    drawList->PushClipRect(previewMin, previewMax, true);
    for (const Axis& axis : axes)
    {
        const ImVec2 projected(
            axis.direction.Dot(right),
            -axis.direction.Dot(cameraUp));
        const ImVec2 end(
            origin.x + projected.x * kAxisLength,
            origin.y + projected.y * kAxisLength);
        drawList->AddLine(origin, end, IM_COL32(20, 20, 22, 220), 4.0f);
        drawList->AddLine(origin, end, axis.color, 2.0f);
        drawList->AddCircleFilled(end, 2.5f, axis.color);
        drawList->AddText(ImVec2(end.x + 3.0f, end.y - 7.0f), axis.color, axis.label);
    }
    drawList->AddCircleFilled(origin, 2.5f, IM_COL32(230, 230, 230, 255));
    drawList->PopClipRect();
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
    const std::vector<std::uint32_t>& recomputeNormalSlots,
    const Math::float4* texOffsScaleOverride,
    int highlightMaterialSlot,
    int highlightSubmeshOrdinal,
    const TextureCube* environment,
    float environmentExposure,
    const EditorPreviewRenderer::PhysicalLighting* physical,
    const std::vector<buoyancy::Pontoon>* pontoons,
    int hoveredPontoon,
    CollisionEditorTab* collisionTab,
    nlohmann::json* collisionDoc)
{
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float controlsHeight = ImGui::GetFrameHeightWithSpacing() +
        ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 canvasSize(
        std::max(1.0f, available.x),
        std::max(64.0f, available.y - controlsHeight));
    // A gizmo over the picture takes the mouse only with no item hovered (ImGuizmo CanActivate), and the button is an item: while the
    // gizmo holds the mouse there is no button, and the camera stays where it is.
    if (collisionTab && collisionTab->GizmoWantsMouse())
    {
        ImGui::Dummy(canvasSize);
    }
    else
    {
        ImGui::InvisibleButton("##MeshPreview", canvasSize,
            ImGuiButtonFlags_MouseButtonLeft |
            ImGuiButtonFlags_MouseButtonRight |
            ImGuiButtonFlags_MouseButtonMiddle);
    }

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
    EditorPreviewRenderer::PreviewLines collisionLines;
    if (collisionTab)
    {
        collisionLines.plain = &collisionTab->Lines();
        collisionLines.selected = &collisionTab->SelectedLines();
    }
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
        lod,
        texOffsScaleOverride,
        highlightMaterialSlot,
        highlightSubmeshOrdinal,
        environment,
        environmentExposure,
        physical,
        collisionTab ? &collisionLines : nullptr,
        collisionTab ? collisionTab->LinesRevision() : 0u);

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

    DrawLocalAxesOverlay(drawList, min, max, camera);

    // Pontoons over the picture, projected with the very matrix the picture was rendered with.
    // A sphere is drawn as its silhouette circle: centre plus the radius along the camera's right.
    if (pontoons && !pontoons->empty() && preview.state == MeshEditorPreviewScene::State::Ready)
    {
        const float pitch = std::clamp(camera.pitch, -1.55334f, 1.55334f);
        const float cosPitch = std::cos(pitch);
        const Math::float3 offset(cosPitch * std::sin(camera.yaw), std::sin(pitch), -cosPitch * std::cos(camera.yaw));
        const Math::float3 right = offset.Cross(Math::float3(0.0f, 1.0f, 0.0f)).Normalized();
        const auto project = [&](const Math::float3& p, ImVec2& out)
        {
            const Math::float4 clip = preview.viewProj.Transform(Math::float4(p, 1.0f));
            if (clip.w <= 1e-5f) { return false; }
            out = ImVec2(min.x + (clip.x / clip.w * 0.5f + 0.5f) * (max.x - min.x),
                         min.y + (0.5f - clip.y / clip.w * 0.5f) * (max.y - min.y));
            return true;
        };
        drawList->PushClipRect(min, max, true);
        for (size_t i = 0; i < pontoons->size(); ++i)
        {
            const buoyancy::Pontoon& pontoon = (*pontoons)[i];
            ImVec2 centre, edge;
            if (!project(pontoon.center, centre) || !project(pontoon.center + right * pontoon.radius, edge)) { continue; }
            const float radius = std::hypot(edge.x - centre.x, edge.y - centre.y);
            const bool hot = static_cast<int>(i) == hoveredPontoon;
            const ImU32 color = hot ? IM_COL32(255, 205, 60, 255) : IM_COL32(70, 200, 255, 210);
            drawList->AddCircle(centre, radius, IM_COL32(20, 20, 22, 160), 40, hot ? 4.0f : 3.0f);
            drawList->AddCircle(centre, radius, color, 40, hot ? 2.5f : 1.5f);
            drawList->AddCircleFilled(centre, hot ? 3.5f : 2.5f, color);
        }
        drawList->PopClipRect();
    }

    // The selected primitive's gizmo over the picture, with the matrices it was rendered with. The shape itself is in the picture.
    if (collisionTab && preview.state == MeshEditorPreviewScene::State::Ready)
    {
        collisionTab->DrawGizmo(*collisionDoc, drawList, min, max, preview.viewMatrix, preview.projMatrix);
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
    ImGui::SameLine();
    ImGui::TextDisabled("Triangles: %u | Indices: %u",
        preview.triangleCount,
        preview.indexCount);
    ImGui::TextDisabled("LMB orbit | RMB/MMB pan | Wheel zoom | Double-click frame");
}

// Base name for a material file promoted from a glTF (parent folder when the file stem is the
// generic "scene"/"model"/"mesh", else the stem) — mirrors the importer's naming (I3).
std::string MaterialBaseName(const std::string& geometry)
{
    const size_t hash = geometry.find('#');
    const std::string p = geometry.substr(0, hash);
    const fs::path fp(p);
    const std::string stem = fp.stem().string();
    std::string lo = stem;
    std::transform(lo.begin(), lo.end(), lo.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string folder = fp.parent_path().filename().string();
    std::string base =
        ((lo == "scene" || lo == "model" || lo == "mesh") && !folder.empty()) ? folder : stem;

    // A split asset's source carries "#node:X". The importer names its materials
    // "<asset>_<node>_<ordinal>", so the base must include the node too — otherwise every split of
    // one glTF promotes to the SAME name and they overwrite each other.
    if (hash != std::string::npos)
    {
        const std::string frag = geometry.substr(hash + 1);
        const std::string kNodePrefix = "node:";
        if (frag.rfind(kNodePrefix, 0) == 0)
        {
            // Mirror ImportPanel::MeshAssetFileComponent exactly (invalid runs collapse to one '_'),
            // or the promoted name will not match the one the importer wrote.
            std::string node;
            for (const unsigned char ch : frag.substr(kNodePrefix.size()))
            {
                if (std::isalnum(ch) || ch == '_' || ch == '-') { node.push_back(static_cast<char>(ch)); }
                else if (node.empty() || node.back() != '_') { node.push_back('_'); }
            }
            if (!node.empty()) { base += "_" + node; }
        }
    }
    return base;
}

} // namespace

void MeshEditorPanel::Open(const std::string& meshAssetPath)
{
    // Re-opening the SAME asset is a refresh (the post-import reload path), not a context
    // switch: keep the camera and the previewed LOD, or every re-bake kicks the user from the
    // LOD 3 they were A/B-ing back to LOD 0. The preview scene clamps the LOD to the new
    // geometry's count, so a shorter chain after a re-bake is safe.
    const bool refresh = meshAssetPath == path_;
    path_ = meshAssetPath;
    doc_ = nlohmann::json::object();
    loaded_ = false;
    status_.clear();

    slots_.clear();
    recomputeNormalSlots_.clear();
    collisionTab_.Reset(); // a re-bake moves the shape's lines under the same settings
    if (!refresh)
    {
        previewCamera_ = {};
        previewLod_ = 0;
    }

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
    // A CHUNKED mesh's submeshes are spatial tiles that all carry material slot 0 by construction
    // (ChunkifyLod0 inherits it), so auto-sizing the material list to the submesh count would put
    // one useless picker per tile — 46 of them on the island, 800+ on a fine grid. Collapse to the
    // single real slot and describe the tiles separately instead.
    chunkGrid_ = doc_.value("chunkGrid", 0);
    hoveredChunk_ = -1;
    // The bin's numbers for every mesh (the LODs' deviation, Tessy's clusters and DAG), not only a chunked one's: it was
    // read for a chunked mesh alone, and "Baked deviation from LOD0" below never showed for any other.
    binInfo_ = {};
    const bool described = MeshManager::DescribeMeshBinary(geometry, binInfo_);
    const bool chunked = chunkGrid_ > 1 && described && !binInfo_.lods.empty() && binInfo_.lods[0].submeshTris.size() > 1;
    if (!chunked) { chunkGrid_ = 0; }
    const size_t slotCount = chunked
        ? size_t{ 1 }
        : std::max<size_t>(1, MeshManager::CountSubmeshes(geometry));
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
    previewLight_.direction = EditorLightDirection::NormalizedRay(
        previewLight_.direction, MeshEditorPreviewLight{}.direction);
    previewLight_.color.x = std::clamp(previewLight_.color.x, 0.0f, 1.0f);
    previewLight_.color.y = std::clamp(previewLight_.color.y, 0.0f, 1.0f);
    previewLight_.color.z = std::clamp(previewLight_.color.z, 0.0f, 1.0f);
    previewLight_.exposure = std::clamp(previewLight_.exposure, 0.0f, 100.0f);
    previewLight_.ambient = std::clamp(previewLight_.ambient, 0.0f, 10.0f);
    previewLight_.positionDistance = std::clamp(
        previewLight_.positionDistance, 0.25f, 8.0f);
    previewLight_.exposureCompensationEv = std::clamp(previewLight_.exposureCompensationEv, -16.0f, 16.0f);
    previewLight_.manualEv100 = std::clamp(previewLight_.manualEv100, -16.0f, 24.0f);
}

void MeshEditorPanel::Draw(EditorContext& ctx, AssetRegistry& registry, bool* open,
    const OpenMaterialHandler& openMaterial)
{
    ImGui::SetNextWindowSize(ImVec2(1080.0f, 720.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(700.0f, 420.0f),
        ImVec2(FLT_MAX, FLT_MAX));
    // While the Collision tab's gizmo holds the mouse a drag is the gizmo's, not the window's: there is no item under it to tell ImGui so.
    if (!ImGui::Begin("Mesh Editor", open, collisionTab_.GizmoWantsMouse() ? ImGuiWindowFlags_NoMove : ImGuiWindowFlags_None))
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
    // Live mesh-asset tiling. It is not part of any material, so the preview cannot discover it —
    // pass the document's current value so dragging Tex Offset/Scale retiles immediately instead of
    // only showing up after Save + reload.
    Math::float4 texOffsScale(0.0f, 0.0f, 1.0f, 1.0f);
    bool hasTexOffsScale = false;
    {
        const auto it = doc_.find("texOffsScale");
        if (it != doc_.end() && it->is_array() && it->size() == 4)
        {
            float v[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
            for (int k = 0; k < 4; ++k) { if ((*it)[k].is_number()) { v[k] = (*it)[k].get<float>(); } }
            texOffsScale = Math::float4(v[0], v[1], v[2], v[3]);
            hasTexOffsScale = true;
        }
    }
    float previewSkyExposure = 1.0f;
    const TextureCube* previewSky = ResolvePreviewSky(ctx, previewSkyExposure);
    EditorPreviewRenderer::PhysicalLighting levelLighting;
    const bool useLevelLighting = BuildPreviewLighting(ctx, levelLighting);
    // The pontoons the Buoyancy section edits, over the preview while that section is open.
    buoyancy::Layout pontoonOverlay;
    if (buoyancyOpen_)
    {
        buoyancy::Settings settings;
        if (const auto block = doc_.find("buoyancy"); block != doc_.end()) { buoyancy::ReadSettings(*block, settings); }
        pontoonOverlay = buoyancy::Resolve(settings, doc_.value("geometry", std::string()));
    }
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
        recomputeNormalSlots_,
        hasTexOffsScale ? &texOffsScale : nullptr,
        hoveredSlot_,
        hoveredChunk_,
        previewSky,
        previewSkyExposure,
        useLevelLighting ? &levelLighting : nullptr,
        buoyancyOpen_ ? &pontoonOverlay.pontoons : nullptr,
        hoveredPontoon_,
        collisionTabOpen_ ? &collisionTab_ : nullptr,
        collisionTabOpen_ ? &doc_ : nullptr);
    ImGui::EndChild();

    ImGui::SameLine(0.0f, kSplitterWidth);
    ImGui::BeginChild("##meshSettingsPane",
        ImVec2(0.0f, workspaceSize.y),
        true);
    ImGui::TextUnformatted(fs::path(path_).filename().string().c_str());
    ImGui::TextDisabled("Geometry: %s",
        doc_.value("geometry", std::string("(none)")).c_str());
    ImGui::Spacing();

    int hoveredSlotThisFrame = -1;
    int hoveredChunkThisFrame = -1;
    int hoveredPontoonThisFrame = -1;

    bool collisionTabOpen = false;
    if (ImGui::BeginTabBar("##meshEditorTabs"))
    {
        if (ImGui::BeginTabItem("Mesh")) { ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Collision", nullptr, expandCollision_ ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
        {
            collisionTabOpen = true;
            ImGui::EndTabItem();
        }
        expandCollision_ = false;
        ImGui::EndTabBar();
    }
    collisionTabOpen_ = collisionTabOpen;
    if (collisionTabOpen)
    {
        collisionTab_.Draw(doc_, slots_);
    }
    else // the Mesh tab: everything to the Save row below, not indented so the block stays as it was
    {
    DrawPreviewSkyControls(registry);

    ImGui::SeparatorText("Preview Directional Light");
    if (previewLight_.levelLighting)
    {
        // The level's sun: its colour, lux and fill are not the preview's to change, so there are
        // no controls for them here -- only what they are.
        if (levelReadout_.built)
        {
            ImGui::TextDisabled("Level sun: %.0f lx", levelReadout_.sunLux);
        }
    }
    else
    {
        ImGui::ColorEdit3("Color", &previewLight_.color.x);
        ImGui::DragFloat("Exposure", &previewLight_.exposure, 0.05f, 0.0f, 100.0f);
        ImGui::DragFloat("Ambient", &previewLight_.ambient, 0.005f, 0.0f, 10.0f);
    }
    float sourceAzimuth = 0.0f;
    float sourceElevation = 0.0f;
    EditorLightDirection::SourceAngles(
        previewLight_.direction, sourceAzimuth, sourceElevation);
    const bool azimuthChanged = ImGui::DragFloat("Source azimuth (Y)",
        &sourceAzimuth, 0.5f, -180.0f, 180.0f, "%.1f deg",
        ImGuiSliderFlags_AlwaysClamp);
    const bool elevationChanged = ImGui::DragFloat("Source elevation",
        &sourceElevation, 0.5f, -89.0f, 89.0f, "%.1f deg",
        ImGuiSliderFlags_AlwaysClamp);
    if (azimuthChanged || elevationChanged)
    {
        previewLight_.direction = EditorLightDirection::RayFromSourceAngles(
            sourceAzimuth, sourceElevation);
    }
    Math::float3 normalizedRay = EditorLightDirection::NormalizedRay(
        previewLight_.direction, MeshEditorPreviewLight{}.direction);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::InputFloat3("Normalized ray", &normalizedRay.x, "%.3f",
        ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Mesh-local direction in which the light rays travel.");
    }
    ImGui::Checkbox("Show light position", &previewLight_.showPosition);
    if (previewLight_.showPosition)
    {
        ImGui::DragFloat("Marker distance", &previewLight_.positionDistance,
            0.05f, 0.25f, 8.0f, "%.2f radii");
    }
    if (ImGui::Button("Reset preview light"))
    {
        previewLight_ = {};
    }
    ImGui::Spacing();

    const std::vector<std::string> presets = CollectPresets(registry);
    const std::string geometry = doc_.value("geometry", std::string());
    // W7.1b moved geometry to our baked `.mesh.bin` and left the glTF in import_staging/, so the
    // material source is `source`, NOT `geometry` — reading `geometry` here made IsGltfGeometry
    // false for every migrated asset and the promote button vanished entirely. Fall back to
    // `geometry` for legacy assets that still reference a glTF directly.
    const std::string materialSource = doc_.contains("source") && doc_["source"].is_string() &&
                                       !doc_["source"].get<std::string>().empty()
        ? doc_["source"].get<std::string>()
        : geometry;
    const bool geometryIsGltf = IsGltfGeometry(materialSource);

    // Collected while drawing the settings pane; consumed by the preview on the NEXT frame.

    // --- Chunks (chunked meshes only) -------------------------------------------------------------
    // These submeshes are spatial TILES of one surface, not material slots, so they get their own
    // section: how many, how big, and whether the tiles are still coarse enough for the LOD chain to
    // reduce them. Hovering a row highlights that one tile in the preview.
    if (chunkGrid_ > 1 && !binInfo_.lods.empty())
    {
        const auto& lod0 = binInfo_.lods[0];
        const size_t chunkCount = lod0.submeshTris.size();
        const std::uint32_t lodCount = static_cast<std::uint32_t>(binInfo_.lods.size());
        ImGui::SeparatorText("Chunks");
        ImGui::Text("%d x %d grid, %zu non-empty tiles, %u triangles total",
            chunkGrid_, chunkGrid_, chunkCount, lod0.totalTris);

        // Tiles simplify with their shared borders LOCKED, so only a tile's interior can reduce.
        // The coarsest LOD as a fraction of LOD0 is the health number: measured on the island,
        // 384 tris/tile -> 0.17, 176 -> 0.33, 102 -> 0.43, 46 -> 0.57, 11 -> 1.00 (LODs dead).
        if (lodCount > 1 && lod0.totalTris > 0)
        {
            const double ratio = static_cast<double>(binInfo_.lods.back().totalTris) / lod0.totalTris;
            const double perTile = chunkCount ? static_cast<double>(lod0.totalTris) / chunkCount : 0.0;
            const ImVec4 bad(0.92f, 0.35f, 0.30f, 1.0f), warn(0.96f, 0.62f, 0.16f, 1.0f);
            if (ratio > 0.5)
            {
                ImGui::TextColored(bad, "~%.0f tris/tile - LOD%u is %.0f%% of LOD0: the grid is too"
                    " fine for the LOD chain to reduce anything.", perTile, lodCount - 1u, ratio * 100.0);
            }
            else if (ratio > 0.25)
            {
                ImGui::TextColored(warn, "~%.0f tris/tile - LOD%u is %.0f%% of LOD0: the coarse LODs"
                    " are losing their reduction (locked tile borders).", perTile, lodCount - 1u, ratio * 100.0);
            }
            else
            {
                ImGui::TextDisabled("~%.0f tris/tile, LOD%u is %.0f%% of LOD0.",
                    perTile, lodCount - 1u, ratio * 100.0);
            }
        }

        // Per-LOD totals, then the per-tile table. Both read straight out of the baked .bin.
        std::string totals = "Totals:";
        for (std::uint32_t L = 0; L < lodCount; ++L)
        {
            totals += "  LOD" + std::to_string(L) + " " + std::to_string(binInfo_.lods[L].totalTris);
        }
        ImGui::TextDisabled("%s", totals.c_str());

        if (ImGui::TreeNode("Per-tile triangles"))
        {
            ImGui::TextDisabled("Hover a row to highlight that tile in the preview.");
            const int cols = 1 + static_cast<int>(lodCount);
            if (ImGui::BeginTable("##chunkTable", cols,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                    ImGuiTableFlags_SizingFixedFit,
                    ImVec2(0.0f, 220.0f)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("tile");
                for (std::uint32_t L = 0; L < lodCount; ++L)
                {
                    const std::string h = "LOD" + std::to_string(L);
                    ImGui::TableSetupColumn(h.c_str());
                }
                ImGui::TableHeadersRow();
                // Clipper: a fine grid is hundreds of tiles and only the visible rows matter.
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(chunkCount));
                while (clipper.Step())
                {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%d", i);
                        if (ImGui::IsItemHovered()) { hoveredChunkThisFrame = i; }
                        for (std::uint32_t L = 0; L < lodCount; ++L)
                        {
                            ImGui::TableSetColumnIndex(1 + static_cast<int>(L));
                            const auto& tris = binInfo_.lods[L].submeshTris;
                            const std::uint32_t t = (static_cast<size_t>(i) < tris.size()) ? tris[i] : 0u;
                            ImGui::Text("%u", t);
                            if (ImGui::IsItemHovered()) { hoveredChunkThisFrame = i; }
                        }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
    }

    // One material picker per submesh — the slot count is auto-detected from the geometry (Open()).
    ImGui::SeparatorText(slots_.size() == 1 ? "Material" : "Material slots (per submesh)");
    for (size_t i = 0; i < slots_.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        const std::string label = (slots_.size() == 1) ? std::string("Material") : ("slot " + std::to_string(i));
        MaterialCombo(label.c_str(), slots_[i], presets, /*allowAuto=*/true); // edits slots_[i] in place
        if (ImGui::IsItemHovered()) { hoveredSlotThisFrame = static_cast<int>(i); }

        // An auto glTF slot has no editable file yet. Promote it first; named presets can be
        // opened directly in Material Editor without returning to the Content Browser.
        if (geometryIsGltf && slots_[i] == "auto")
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Save as material"))
            {
                const std::string name = MaterialBaseName(materialSource) + "_" + std::to_string(i);
                const std::string written = materialgen::WriteFromGltf(
                    materialSource, static_cast<int>(i), name, /*overwrite=*/false);
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

        // mesh.json "lod1/2/3DropSlots": this slot VANISHES at that LOD (empty submesh keeps
        // the table aligned). The surgical answer for trims welded to a bigger surface (husk
        // scales on a trunk) that no error budget can drop without eating the silhouette
        // first. Levels are independent — every LOD builds from the base indices. Applied on
        // the next re-bake (Save).
        {
            ImGui::SameLine();
            ImGui::TextUnformatted("Drop at LOD");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("This slot's geometry VANISHES at the checked LODs. For trims\n"
                                  "welded to a bigger surface - a date palm's husk scales survive every\n"
                                  "error budget while the trunk collapses. Takes effect on re-bake\n"
                                  "(Save). Each level is independent.");
            }
            static const char* kDropKeys[3] = { "lod1DropSlots", "lod2DropSlots", "lod3DropSlots" };
            static const char* kDropLabels[3] = { "1", "2", "3" };
            const auto slotVal = static_cast<uint32_t>(i);
            for (int lvl = 0; lvl < 3; ++lvl)
            {
                const char* key = kDropKeys[lvl];
                nlohmann::json& dropArr = doc_[key];
                if (!dropArr.is_array()) { dropArr = nlohmann::json::array(); }
                bool dropped = false;
                for (const nlohmann::json& v : dropArr)
                {
                    if (v.is_number_integer() && v.get<uint32_t>() == slotVal) { dropped = true; break; }
                }
                ImGui::SameLine();
                ImGui::PushID(key);
                if (ImGui::Checkbox(kDropLabels[lvl], &dropped))
                {
                    nlohmann::json next = nlohmann::json::array();
                    for (const nlohmann::json& v : dropArr)
                    {
                        if (!(v.is_number_integer() && v.get<uint32_t>() == slotVal)) { next.push_back(v); }
                    }
                    if (dropped) { next.push_back(slotVal); }
                    if (next.empty()) { doc_.erase(key); }
                    else { doc_[key] = std::move(next); }
                }
                else if (dropArr.empty()) { doc_.erase(key); }
                ImGui::PopID();
            }
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

    // Everything wind lives here - including the PER-SLOT foliage weights, which used to sit
    // inside the material-slot loop. They are one coherent set of knobs (a trunk/foliage split
    // means nothing without the strength above it), so they get tuned together.
    //
    // Collapsed by default: only foliage assets use any of this, so on the average mesh it is dead
    // space above the controls people actually reach for. CollapsingHeader is closed unless
    // ImGuiTreeNodeFlags_DefaultOpen is passed; ImGui then persists the open/closed state per header
    // id in imgui.ini, so a user who opens it keeps it open.
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Wind"))
    {
        float strength = doc_.value("windStrength", 0.0f);
        if (ImGui::SliderFloat("Wind Strength", &strength, 0.0f, 1.0f, "%.2f"))
        {
            if (strength <= 0.0f) { doc_.erase("windStrength"); } // absent == rigid
            else { doc_["windStrength"] = strength; }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Master sway amount for this asset. 0 = rigid (the field is dropped\n"
                              "from the file, so the asset is byte-identical to a pre-wind one).");
        }

        float stiffness = doc_.value("windTrunkStiffness", 1.0f);
        if (ImGui::SliderFloat("Trunk Stiffness", &stiffness, 0.1f, 4.0f, "%.2f"))
        {
            if (std::abs(stiffness - 1.0f) < 1.0e-4f) { doc_.erase("windTrunkStiffness"); }
            else { doc_["windTrunkStiffness"] = stiffness; }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Divides the main bend: >1 stiffer woody trunk, <1 whippier stem.\n"
                              "Only affects the trunk-driven bend, not the foliage streaming.");
        }

        // Per-slot foliage weight. Geometry alone cannot tell a frond hanging down against the trunk
        // from the trunk itself, so this is authored rather than inferred; leaving every slot at 0
        // drops the key entirely and the runtime falls back to the slot alpha-mask flag, which
        // misses OPAQUE foliage (a palm frond bases).
        ImGui::Spacing();
        ImGui::TextDisabled("Foliage weight per material slot (0 = woody, 1 = leaves)");
        for (size_t i = 0; i < slots_.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(1000 + i));
            const std::string label = slots_.size() == 1
                ? std::string("Foliage")
                : ("slot " + std::to_string(i) + " (" + slots_[i] + ")");
            float foliage = WindFoliageForSlot(i);
            if (ImGui::SliderFloat(label.c_str(), &foliage, 0.0f, 1.0f, "%.2f"))
            {
                SetWindFoliageForSlot(i, foliage);
            }
            if (ImGui::IsItemHovered())
            {
                hoveredSlotThisFrame = static_cast<int>(i);
                ImGui::SetTooltip("How much this submesh behaves as leaves in the wind.\n"
                                  "0 = woody (trunk/branch): only the main bend.\n"
                                  "1 = foliage: streams downwind and flutters on top of the bend.");
            }
            ImGui::PopID();
        }
    }

    // Buoyancy: where the mesh floats and how it moves, for every placed instance whose "Floats on
    // the ocean" is on. Closed by default like Wind; while open the preview draws the pontoons.
    ImGui::Spacing();
    const bool scrollToBuoyancy = expandBuoyancy_;
    if (expandBuoyancy_)
    {
        ImGui::SetNextItemOpen(true);
        expandBuoyancy_ = false;
    }
    buoyancyOpen_ = ImGui::CollapsingHeader("Buoyancy");
    if (buoyancyOpen_)
    {
        if (scrollToBuoyancy) { ImGui::SetScrollHereY(0.0f); }
        DrawBuoyancySection(hoveredPontoonThisFrame);
    }

    // Rest pose: how a NEW copy lies. Its numbers act where objects are created (spawn, place, a
    // drop into the viewport -- the drag ghost shows it), not on this preview and not on copies
    // already placed, and the section says so rather than letting a control look inert.
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Rest pose"))
    {
        DrawRestPoseSection();
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Vessel"))
    {
        DrawVesselSection();
    }

    // The water kept out (ocean/WaterMask.h): the ocean's surface is not drawn inside this mesh's columns (its LOD 0 seen from
    // above, depressions filled to their rim) -- an open boat's cockpit under the sea's level stays dry.
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Water"))
    {
        bool mask = doc_.value("waterMask", false);
        if (ImGui::Checkbox("Keeps the water out", &mask))
        {
            if (mask) { doc_["waterMask"] = true; }
            else { doc_.erase("waterMask"); } // absent == off
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("The ocean's surface is not drawn inside this mesh (LOD 0, every slot, seen from above): under its\n"
                              "skin, and in every depression up to where water poured on it would spill over its lowest edge --\n"
                              "an open boat's cockpit filled to its gunwale (openings under 1 m, a walkthrough, closed first).\n"
                              "A swim platform keeps only its own slab: the sea over it stays. From outside and from the helm.\n"
                              "--set ocean.waterMask:2 paints the kept-out water magenta. Applies to placed instances on Save.");
        }
    }

    // Ray tracing: how much of the light a cutout card (an alpha-tested slot: a leaf, a frond) stops for the VISIBILITY
    // rays -- the sky visibility, the GI, the fog (rt_geometry.hlsli RtVisibilityTransmittance). Absent = 1, every card
    // solid: a door or a curtain with an alpha test must block as a wall does. The palms take 0.8.
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Ray tracing"))
    {
        float opacity = doc_.value("rtCutoutOpacity", 1.0f);
        if (ImGui::SliderFloat("Cutout opacity", &opacity, 0.0f, 1.0f, "%.2f"))
        {
            if (opacity >= 0.995f) { doc_.erase("rtCutoutOpacity"); } // absent == solid
            else { doc_["rtCutoutOpacity"] = std::round(opacity * 100.0f) / 100.0f; }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("How much of the light this mesh's alpha-tested cards (leaves, fronds) stop for the sky\n"
                              "visibility, GI and fog rays: a ray that hits one carries 1 - this on and goes no further.\n"
                              "1 = solid cards (the field is dropped from the file). Opaque geometry always blocks.\n"
                              "The palms: 0.8 (20 % through). Applies to placed instances on Save.");
        }
    }

    // Tessy: what the bake stored for the cluster paths (read-only).
    ImGui::Spacing();
    const bool scrollToTessy = expandTessy_;
    if (expandTessy_)
    {
        ImGui::SetNextItemOpen(true);
        expandTessy_ = false;
    }
    if (ImGui::CollapsingHeader("Tessy (clusters)"))
    {
        if (scrollToTessy) { ImGui::SetScrollHereY(0.0f); } // opened by --edit-mesh=...#tessy: below the slots, off screen
        ImGui::PushTextWrapPos(0.0f); // its lines are long and the pane is narrow
        DrawTessySection();
        ImGui::PopTextWrapPos();
    }

    // Texturing (separate from Wind above — unrelated knobs, and mixing them made the wind block
    // look like it owned the tiling).
    ImGui::Spacing();
    ImGui::SeparatorText("Texturing");
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

    // LOD generation — the same knobs the import dialog offers, editing the mesh.json keys
    // directly so the whole tune -> bake -> inspect loop lives in this one window: Save
    // re-bakes the .mesh.bin whenever the effective options differ from what it was baked
    // with (BinaryNeedsRebake), then live-applies to placed instances. Values are stored
    // non-default-only, mirroring the import dialog's writer.
    ImGui::Spacing();
    ImGui::SeparatorText("LOD generation");
    {
        ImGui::TextDisabled("Same knobs as the import dialog. Save re-bakes the geometry.");
        const auto lodFloat = [this](const char* label, const char* key, float def,
            float lo, float hi, const char* fmt, const char* tip)
        {
            float v = def;
            const auto it = doc_.find(key);
            if (it != doc_.end() && it->is_number()) { v = it->get<float>(); }
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::SliderFloat(label, &v, lo, hi, fmt))
            {
                if (v != def) { doc_[key] = v; } else { doc_.erase(key); }
            }
            if (tip && ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", tip); }
        };
        const auto lodBool = [this](const char* label, const char* key, bool def, const char* tip)
        {
            bool v = def;
            const auto it = doc_.find(key);
            if (it != doc_.end() && it->is_boolean()) { v = it->get<bool>(); }
            if (ImGui::Checkbox(label, &v))
            {
                if (v != def) { doc_[key] = v; } else { doc_.erase(key); }
            }
            if (tip && ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", tip); }
        };
        // Read from the DOC, not from chunkGrid_: that member is zeroed when the .bin on disk is
        // not chunked yet, and the warning is about the intent the manifest already states.
        const int docChunkGrid = doc_.value("chunkGrid", 0);
        bool lod3Aggr = true;
        {
            const auto it = doc_.find("lod3Aggressive");
            if (it != doc_.end() && it->is_boolean()) { lod3Aggr = it->get<bool>(); }
        }
        if (docChunkGrid > 1 && lod3Aggr)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.62f, 0.16f, 1.0f));
            ImGui::TextWrapped("Chunking is ON and so is Aggressive LOD3. Terrain manifests turn "
                               "LOD3 off: its loose 0.25 error budget deforms dune silhouettes, and "
                               "the far-clipmap shadow casters are tuned against low-sun banding.");
            ImGui::PopStyleColor();
            if (ImGui::SmallButton("Turn off Aggressive LOD3")) { doc_["lod3Aggressive"] = false; }
            ImGui::Spacing();
        }

        // RUNTIME selection, not a bake knob: it changes nothing in the .bin and needs no re-import,
        // which is why it sits above the separator that starts the bake settings.
        lodFloat("LOD switch distance", "lodDistanceScale", 1.0f, 0.25f, 6.0f, "x%.2f",
            "Per-asset multiplier on the DISTANCE at which every LOD switch of this mesh happens.\n"
            "2 = each level starts twice as far away (keeps detail longer), 0.5 = half -- UE's\n"
            "per-component LOD factor. Applies live - no re-import.");
        lodFloat("LOD pixel error (0 = global)", "lodPixelError", 0.0f, 0.0f, 64.0f, "%.1f px",
            "UE's reduction PixelError for THIS mesh (0 = the global one, dev window LOD tab, default 8).\n"
            "With 'UE screen size' on, each LOD's screen size is estimated from its baked deviation:\n"
            "the level switches in where that deviation spans this many pixels at 1080p, 90 deg.\n"
            "Higher = coarser sooner. mesh.json \"lodScreenSize\" (authored, LOD0 first) replaces the\n"
            "estimate altogether. Applies live - no re-import.");

        // UE's per-LOD ScreenSize, AUTHORED (mesh.json "lodScreenSize", LOD0 first; UE's
        // bAutoComputeLODScreenSize off). Unticked, the mesh takes what the line below the box says:
        // the automatic estimate from its baked deviation, or -- foliage -- the global ratio curve.
        {
            bool foliage = false;
            for (size_t s = 0; s < slots_.size(); ++s) { foliage = foliage || WindFoliageForSlot(s) > 0.0f; }
            float pixelError = render::g_lodPixelError;
            if (const auto it = doc_.find("lodPixelError"); it != doc_.end() && it->is_number() && it->get<float>() > 0.0f)
            {
                pixelError = it->get<float>();
            }
            float estimate[render::kLodScreenSizes] = {};
            const bool haveEstimate = previewScene_.GetLodScreenSizes(pixelError, estimate);
            unsigned int lodCount = 1u;
            for (unsigned int i = 1; i < render::kLodScreenSizes; ++i) { if (estimate[i] > 0.0f) { lodCount = i + 1u; } }
            // What the mesh selects with when nothing is authored -- the seed when the box is ticked.
            // The global curve is the distance/radius bounds as screen sizes at 90 degrees, 16:9.
            float effective[render::kLodScreenSizes] = { 1.0f, 0.0f, 0.0f, 0.0f };
            const float bounds[3] = { render::g_lodBound0, render::g_lodBound1, render::g_lodBound2 };
            for (unsigned int i = 1; i < lodCount; ++i)
            {
                effective[i] = foliage ? (2.0f * 0.8889f) / std::max(bounds[i - 1u], 0.5f) : estimate[i];
            }

            const auto it = doc_.find("lodScreenSize");
            bool authored = it != doc_.end() && it->is_array();
            if (ImGui::Checkbox("Author LOD screen sizes (UE)", &authored))
            {
                if (authored)
                {
                    nlohmann::json arr = nlohmann::json::array();
                    for (unsigned int i = 0; i < lodCount; ++i)
                    {
                        arr.push_back(std::round(effective[i] * 1000.0f) / 1000.0f);
                    }
                    doc_["lodScreenSize"] = arr;
                }
                else
                {
                    doc_.erase("lodScreenSize");
                }
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("UE's ScreenSize per LOD: a level is drawn while the object's screen size\n"
                                  "(bounding-sphere diameter / screen height) is at most its value, so a SMALLER\n"
                                  "number keeps the finer level longer. Ticking seeds the list with what the mesh\n"
                                  "uses now. The per-asset 'LOD switch distance' still multiplies on top (UE's\n"
                                  "per-component factor). Only used with 'UE screen size' on (dev window LOD tab).\n"
                                  "Applies live - no re-import.");
            }
            if (authored)
            {
                nlohmann::json& arr = doc_["lodScreenSize"];
                while (arr.size() < lodCount) { arr.push_back(0.0f); }
                for (unsigned int i = 1; i < lodCount && i < arr.size(); ++i)
                {
                    float v = arr[i].is_number() ? arr[i].get<float>() : 0.0f;
                    char label[32];
                    std::snprintf(label, sizeof(label), "LOD%u screen size", i);
                    ImGui::SetNextItemWidth(150.0f);
                    if (ImGui::SliderFloat(label, &v, 0.005f, 2.0f, "%.3f", ImGuiSliderFlags_Logarithmic))
                    {
                        arr[i] = std::round(v * 1000.0f) / 1000.0f;
                    }
                    if (haveEstimate && !foliage)
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(auto %.3f)", static_cast<double>(estimate[i]));
                    }
                }
            }
            else if (haveEstimate && lodCount > 1u)
            {
                char line[160];
                std::snprintf(line, sizeof(line), "%s:  LOD1 %.3f   LOD2 %.3f   LOD3 %.3f",
                              foliage ? "Foliage, the global curve" : "Automatic (UE estimate)",
                              static_cast<double>(effective[1]), static_cast<double>(lodCount > 2u ? effective[2] : 0.0f),
                              static_cast<double>(lodCount > 3u ? effective[3] : 0.0f));
                ImGui::TextDisabled("%s", line);
            }
        }

        // The baked per-level deviation, which UE's automatic screen size is estimated from
        // (render::AutoLodScreenSizes). Measured across this project's assets it holds palms (a lot
        // of deviation for their size) finer for longer than spheres and rocks -- geometric error is
        // not perceptual salience; an asset the eye disagrees with takes a per-asset pixel error or
        // an authored "lodScreenSize".
        if (!binInfo_.lods.empty())
        {
            ImGui::TextDisabled("Baked deviation from LOD0 (object units):");
            ImGui::SameLine();
            std::string errs;
            for (size_t i = 1; i < binInfo_.lods.size(); ++i)
            {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "%sLOD%zu %.4f", i > 1 ? "   " : "", i,
                              static_cast<double>(binInfo_.lods[i].error));
                errs += buf;
            }
            ImGui::TextUnformatted(errs.empty() ? "not measured" : errs.c_str());
        }

        // Runtime, not a bake: the pixel error this asset takes, a share of the frame's -- the discrete LODs' and the
        // cluster DAG's (Mesh::SetLodPixelErrorScale).
        lodFloat("Pixel error scale", "lodPixelErrorScale", 1.0f, 0.05f, 4.0f, "x%.2f",
            "This asset's pixel error, a share of the frame's (lod.pixelError for the LODs, tessy.lodPixelError\n"
            "for the cluster DAG): under 1 it keeps its detail nearer -- a glossy hull shows a collapse in its\n"
            "shading that the geometric error calls small. Runtime: no rebake; applies to placed copies on Save.");

        ImGui::SeparatorText("Every level (LOD1-3)");
        lodFloat("Triangle target", "lodRatioScale", 1.0f, 0.25f, 4.0f, "x%.2f",
            "Chain-wide multiplier over the 0.5/0.25/0.12 per-level triangle targets.");
        lodFloat("Error budget", "lodErrorScale", 1.0f, 0.25f, 4.0f, "x%.2f",
            "Chain-wide multiplier over the 0.02/0.05/0.12 per-level error budgets.");
        lodFloat("Normal weight", "lodNormalWeight", 0.0f, 0.0f, 2.0f, "%.2f",
            "Attribute-aware simplification on EVERY slot, weighting the NORMAL. LODs are index\n"
            "buffers over ONE shared vertex buffer, so a surviving vertex keeps the normal that was\n"
            "computed for the LOD0 surface around it; a position-only collapse is blind to that and\n"
            "the shading normal stops matching the geometry it is drawn on -- the lighting shifts\n"
            "between levels even where the silhouette holds, and a low sun magnifies it.\n"
            "Terrain under a low sun and smooth glossy surfaces show it (small_yacht's hull: 0.5).\n"
            "The Tessy DAG takes it too. A new import starts at 0.5; absent = 0, the old\n"
            "position-only bake, byte for byte.");
        lodBool("Allow collapses across attribute seams", "lodPermissive", false,
            "meshopt_SimplifyPermissive. WRECKS masked foliage (blades collapse into spikes);\n"
            "fine for solid props with UV seams and real volume.");
        lodBool("Drop small disconnected parts", "lodDropSmallParts", false,
            "meshopt_SimplifyPrune. Removes whole loose components once their removal\n"
            "fits the error budget.");
        ImGui::SeparatorText("LOD3 only (the last, harshest level)");
        lodBool("Aggressive LOD3", "lod3Aggressive", true,
            "The LAST level cuts hard: solid slots to ~5%% at a loose budget, foliage via the\n"
            "leaf prune. TURN OFF FOR TERRAIN - far shadow casters are tuned against\n"
            "low-sun banding.");
        // Both scale LOD3's AGGRESSIVE bases and are read nowhere else, so with the toggle off they
        // are inert. Greyed rather than hidden: the value still round-trips through mesh.json.
        ImGui::BeginDisabled(!lod3Aggr);
        lodFloat("LOD3 triangle target", "lod3RatioScale", 1.0f, 0.25f, 4.0f, "x%.2f",
            "LOD3's OWN multiplier over its harsh base target (5%% solid; also scales the\n"
            "kept-leaf interior decimation).");
        lodFloat("LOD3 error budget", "lod3ErrorScale", 1.0f, 0.25f, 4.0f, "x%.2f",
            "LOD3's OWN multiplier over its loose base error (0.25 of mesh extent).");
        ImGui::EndDisabled();

        ImGui::SeparatorText("Foliage only (slots marked in windFoliage)");
        lodFloat("Leaf UV weight", "foliageUvWeight", 0.0f, 0.0f, 2.0f, "%.2f",
            "Every level. Attribute-aware simplification for foliage slots: UV distortion costs\n"
            "like position error, so collapses stop smearing the leaf texture into streaks.\n"
            "~0.5-1.0 is the useful range; 0 = position-only collapse.");

        // The leaf prune is LOD3-only AND never runs on a chunked mesh (BuildLodsCpu gates it on
        // !chunkedSubsets, because it APPENDS vertices and a chunk's submesh table cannot absorb
        // that). On terrain the whole group is dead weight, so say so.
        const bool pruneDead = docChunkGrid > 1;
        ImGui::BeginDisabled(pruneDead);
        ImGui::TextDisabled(pruneDead ? "Leaf prune (LOD3) - not run on a chunked mesh"
                                      : "Leaf prune - LOD3 only");
        lodFloat("Foliage prune keep", "foliagePruneKeep", 0.35f, 0.0f, 1.0f, "%.2f",
            "LOD3 foliage: fraction of whole leaves KEPT. 0 or 1 = no prune.");
        const float pruneKeep = doc_.value("foliagePruneKeep", 0.35f);
        ImGui::BeginDisabled(!(pruneKeep > 0.0f && pruneKeep < 1.0f));
        lodFloat("Leaf inner target", "foliageInnerRatio", 0.5f, 0.05f, 1.0f, "%.2f",
            "LOD3: fraction of the KEPT leaves' triangles surviving the interior decimation\n"
            "(outline pinned, inside crushed).");
        lodFloat("Leaf inner error", "foliageInnerError", 0.15f, 0.02f, 0.6f, "%.2f",
            "Error budget for the leaf interior decimation.");
        lodFloat("Leaf grow", "foliageGrow", 1.0f, 0.0f, 1.5f, "%.2f",
            "How much the kept leaves inflate to compensate the pruned area. 1 = full\n"
            "1/sqrt(keep) compensation (can read FLUFFIER than the source), 0 = authored size.");
        ImGui::EndDisabled(); // prune keep out of range
        ImGui::EndDisabled(); // chunked mesh
    }

    } // the Mesh tab

    hoveredSlot_ = hoveredSlotThisFrame;
    hoveredPontoon_ = hoveredPontoonThisFrame;
    hoveredChunk_ = hoveredChunkThisFrame;

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
        // No live runtime: nothing to respawn. (A disabled object HAS one -- loaded hidden -- and
        // the respawn below keeps it hidden.)
        if (ctx.scene.FindEditorObject(id.value) == nullptr) { continue; }
        const EditorObject* obj = ctx.document.Find(id);
        if (!obj) { continue; }
        const nlohmann::json json = EditorSceneDocument::ObjectToJson(*obj);
        std::unique_ptr<RenderableObjectBase> runtime = SceneObjectFactory::CreateStaticMeshFromJson(json);
        if (!runtime) { continue; }
        ctx.scene.RemoveEditorObject(id.value);
        ctx.scene.AddInitializedEditorObject(ctx.renderer, uploads, id.value, std::move(runtime), obj->enabled);
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

float MeshEditorPanel::WindFoliageForSlot(size_t slot) const
{
    const auto it = doc_.find("windFoliage");
    if (it == doc_.end() || !it->is_array() || slot >= it->size()) { return 0.0f; }
    const auto& v = (*it)[slot];
    return v.is_number() ? v.get<float>() : 0.0f;
}

void MeshEditorPanel::DrawBuoyancySection(int& hoveredPontoonThisFrame)
{
    buoyancy::Settings settings;
    if (const auto block = doc_.find("buoyancy"); block != doc_.end()) { buoyancy::ReadSettings(*block, settings); }
    const std::string geometry = doc_.value("geometry", std::string());
    const buoyancy::Layout layout = buoyancy::Resolve(settings, geometry);
    bool changed = false;

    ImGui::TextDisabled("Used by placed instances whose Inspector says \"Floats on the ocean\".");
    if (!layout.Valid())
    {
        ImGui::TextColored(ImVec4(0.96f, 0.62f, 0.16f, 1.0f),
            "No pontoons: nothing of this mesh is below its waterline at this draft.");
    }

    // --- How it floats ------------------------------------------------------------------------
    bool autoDraft = settings.draft < 0.0f;
    if (ImGui::Checkbox("Automatic draft", &autoDraft))
    {
        settings.draft = autoDraft ? -1.0f : layout.draft;
        changed = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Automatic = a quarter of the hull's height (keel to gunwale, %.2f m here).",
            layout.hullHeight);
    }
    ImGui::BeginDisabled(autoDraft);
    float draft = autoDraft ? layout.draft : settings.draft;
    if (ImGui::SliderFloat("Draft (m)", &draft, 0.01f, std::max(layout.hullHeight, 0.05f), "%.3f"))
    {
        settings.draft = std::max(draft, 0.0f);
        changed = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("How deep the hull sits in calm water: the waterline above the mesh's lowest\n"
                          "point. Automatic pontoons sit ON this waterline and follow it.");
    }
    if (ImGui::SliderFloat("Inertia", &settings.inertia, 0.25f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
    {
        changed = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Added mass. 1 = the calibrated hull. Higher = the SAME draft, but heave, pitch\n"
                          "and roll answer the waves sqrt(x) slower: a heavy boat wallows, a float bobs.");
    }
    if (ImGui::SliderFloat("Damping", &settings.damping, 0.0f, 1.0f, "%.2f"))
    {
        changed = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Damping ratio of the heave: 0 rings on forever, 1 settles without overshoot.");
    }

    // --- Where it floats ----------------------------------------------------------------------
    ImGui::SeparatorText("Pontoons");
    if (settings.pontoons.empty())
    {
        ImGui::TextDisabled("Automatic: %d on the hull's waterplane (re-placed with the draft).",
            static_cast<int>(layout.pontoons.size()));
    }
    else
    {
        ImGui::TextDisabled("Authored: %d.", static_cast<int>(settings.pontoons.size()));
    }
    if (ImGui::Button("Place automatically"))
    {
        buoyancy::Settings automatic = settings;
        automatic.pontoons.clear();
        settings.pontoons = buoyancy::Resolve(automatic, geometry).pontoons;
        changed = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Write the automatic layout into the mesh as pontoons you can edit.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(settings.pontoons.empty());
    if (ImGui::Button("Use automatic"))
    {
        settings.pontoons.clear();
        changed = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Drop the authored pontoons: the runtime places them from the hull itself.");
    }

    // Editing an automatic pontoon turns the whole set into authored ones -- the row you touched
    // cannot stay automatic while it holds your number.
    std::vector<buoyancy::Pontoon> rows = settings.pontoons.empty() ? layout.pontoons : settings.pontoons;
    int removeIndex = -1;
    bool rowsChanged = false;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(2000 + i));
        float v[4] = { rows[i].center.x, rows[i].center.y, rows[i].center.z, rows[i].radius };
        ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::DragFloat4("##pontoon", v, 0.005f, 0.0f, 0.0f, "%.3f"))
        {
            rows[i].center = Math::float3(v[0], v[1], v[2]);
            rows[i].radius = std::max(v[3], 0.01f);
            rowsChanged = true;
        }
        if (ImGui::IsItemHovered())
        {
            hoveredPontoonThisFrame = static_cast<int>(i);
            ImGui::SetTooltip("Pontoon %d: centre x, y, z and radius, in the mesh's metres.", static_cast<int>(i));
        }
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0f))) { removeIndex = static_cast<int>(i); }
        if (ImGui::IsItemHovered()) { hoveredPontoonThisFrame = static_cast<int>(i); }
        ImGui::PopID();
    }
    if (removeIndex >= 0)
    {
        rows.erase(rows.begin() + removeIndex);
        rowsChanged = true;
    }
    if (ImGui::Button("Add pontoon"))
    {
        // At the middle of the set, the average size: a starting point to drag, not a guess.
        buoyancy::Pontoon added;
        added.center = Math::float3(0.0f, layout.waterline, 0.0f);
        added.radius = 0.25f;
        if (!rows.empty())
        {
            Math::float3 sum(0.0f);
            float radius = 0.0f;
            for (const buoyancy::Pontoon& p : rows) { sum += p.center; radius += p.radius; }
            added.center = sum / static_cast<float>(rows.size());
            added.radius = radius / static_cast<float>(rows.size());
        }
        rows.push_back(added);
        rowsChanged = true;
    }
    if (rowsChanged)
    {
        settings.pontoons = std::move(rows);
        changed = true;
    }

    if (changed)
    {
        const nlohmann::json block = buoyancy::WriteSettings(settings);
        if (block.empty()) { doc_.erase("buoyancy"); } // absent == automatic everything
        else { doc_["buoyancy"] = block; }
    }
    ImGui::TextDisabled("Applies to placed instances on Save.");
}

// mesh.json "vessel": is this asset a boat, and what kind (docs/play_mode_plan.md). Saved with the rest; a placed copy takes it at its next spawn, a running Play with --set vessel.*.
void MeshEditorPanel::DrawVesselSection()
{
    bool isVessel = doc_.contains("vessel") && doc_["vessel"].is_object();
    if (ImGui::Checkbox("This is a powered boat", &isVessel))
    {
        if (isVessel) { doc_["vessel"] = nlohmann::json::object(); }
        else { doc_.erase("vessel"); }
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("A copy floats and moves in Play, and E takes its helm: W S throttle, A D rudder."); }
    if (!isVessel) { return; }
    nlohmann::json& block = doc_["vessel"];
    physics::VesselSettings settings;
    physics::ReadVesselSettings(block, settings);
    const auto drawPoint = [&block](const char* label, const char* key, Math::float3 value)
    {
        if (ImGui::DragFloat3(label, &value.x, 0.01f)) { block[key] = physics::WriteVec3(value); }
    };
    drawPoint("Bow's way", "forward", settings.forward);
    drawPoint("Propeller (m)", "propeller", settings.propeller);
    drawPoint("Helmsman's eyes (m)", "helmEye", settings.helmEye);
    for (const physics::VesselKey& key : physics::VesselKeys())
    {
        float value = settings.*(key.member);
        if (ImGui::DragFloat(key.label, &value, key.speed, key.lo, key.hi, "%.3f")) { block[key.key] = value; }
    }
}

// Tessy (docs/tessy_plan.md): every bake stores each LOD cut into clusters (<= 128 triangles) and, from v5 on, the cluster
// DAG over LOD0; a load cuts the clusters into meshlets (<= 64 vertices) for the mesh shaders. None of it has a switch of
// its own per asset: the DAG takes the LOD section's weights, the process's prune and fold, and the frame's tessy.* knobs
// decide who draws it. This section shows what the bin holds.
void MeshEditorPanel::DrawTessySection()
{
    if (binInfo_.lods.empty())
    {
        ImGui::TextDisabled("No .mesh.bin to read yet (Save bakes it).");
        return;
    }
    const MeshManager::BinaryInfo& info = binInfo_;
    const MeshManager::BinaryDagInfo& dag = info.dag;
    const double lod0Tris = static_cast<double>(std::max<std::uint32_t>(info.lods[0].totalTris, 1u));
    ImGui::TextDisabled(".mesh.bin v%u, %u vertices. Every bake stores these; there is no per-asset switch.", info.version,
                        info.vertexCount);

    // The discrete LODs: what the classic paths draw, and what the cluster paths (VSM pages, the lamps' atlases, the SDSM
    // partitions, the camera's G-buffer) draw while tessy.lod is off.
    ImGui::PushTextWrapPos(-1.0f); // a cell does not wrap: its column fits its widest value
    if (ImGui::BeginTable("##tessyLods", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("LOD");
        ImGui::TableSetupColumn("Triangles");
        ImGui::TableSetupColumn("Clusters");
        ImGui::TableSetupColumn("Tris/cluster");
        ImGui::TableSetupColumn("Meshlets");
        ImGui::TableSetupColumn("Deviation (m)");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < info.lods.size(); ++i)
        {
            const MeshManager::BinaryLodInfo& lod = info.lods[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%zu", i);
            ImGui::TableNextColumn(); ImGui::Text("%u", lod.totalTris);
            ImGui::TableNextColumn();
            if (lod.clusters > 0) { ImGui::Text("%u", lod.clusters); } else { ImGui::TextDisabled("none"); }
            ImGui::TableNextColumn();
            if (lod.clusters > 0) { ImGui::Text("%.1f", static_cast<double>(lod.totalTris) / lod.clusters); }
            ImGui::TableNextColumn();
            if (lod.meshlets > 0) { ImGui::Text("%u", lod.meshlets); }
            ImGui::TableNextColumn();
            if (i == 0) { ImGui::TextDisabled("-"); } else { ImGui::Text("%.4f", static_cast<double>(lod.error)); }
        }
        ImGui::EndTable();
    }
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("A cluster is a run of <= 128 triangles / 256 vertices of one LOD inside one material slot; a load\n"
                          "cuts each into meshlets of <= 64 vertices (one mesh shader group each). Deviation = the LOD's\n"
                          "worst distance from LOD0, object space.");
    }

    ImGui::Spacing();
    if (dag.clusters == 0)
    {
        ImGui::TextDisabled(info.version < 5 ? "No cluster DAG in this file (v%u): a load builds one only under\n"
                                               "--tessy-lod-build or mesh.json \"tessyLod\": true; --tessy-upgrade adds it."
                                             : "No cluster DAG in this file (v%u): the bake built none (see its log line).",
                            info.version);
    }
    else
    {
        const MeshManager::BinaryDagLevel& leaves = dag.levels.empty() ? MeshManager::BinaryDagLevel{} : dag.levels[0];
        ImGui::Text("Cluster DAG: %u clusters in %zu levels over %u slot(s), %u meshlets", dag.clusters, dag.levels.size(),
                    dag.submeshes, dag.meshlets);
        ImGui::Text("Triangles over all levels %llu (x%.2f of LOD0); roots %u clusters, %llu tris (%.1f %% of LOD0) at %.4f m",
                    static_cast<unsigned long long>(dag.triangles), static_cast<double>(dag.triangles) / lod0Tris,
                    dag.rootClusters, static_cast<unsigned long long>(dag.rootTriangles),
                    100.0 * static_cast<double>(dag.rootTriangles) / lod0Tris, static_cast<double>(dag.rootError));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("The roots are the coarsest the DAG goes: under tessy.lod a far copy draws no fewer triangles\n"
                              "than this. Leaf cards (foliage slots) leave whole once the error passes their size (prune);\n"
                              "a surface simplifies until its borders lock it.");
        }
        if (leaves.triangles != static_cast<std::uint64_t>(info.lods[0].totalTris))
        {
            ImGui::TextDisabled("(the DAG's level 0 holds %llu triangles, LOD0 %u)",
                                static_cast<unsigned long long>(leaves.triangles), info.lods[0].totalTris);
        }
        ImGui::PushTextWrapPos(-1.0f);
        if (ImGui::BeginTable("##tessyDag", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("Level");
            ImGui::TableSetupColumn("Clusters");
            ImGui::TableSetupColumn("Triangles");
            ImGui::TableSetupColumn("% of LOD0");
            ImGui::TableSetupColumn("Error (m)");
            ImGui::TableHeadersRow();
            for (size_t l = 0; l < dag.levels.size(); ++l)
            {
                const MeshManager::BinaryDagLevel& lv = dag.levels[l];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%zu", l);
                ImGui::TableNextColumn(); ImGui::Text("%u", lv.clusters);
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(lv.triangles));
                ImGui::TableNextColumn(); ImGui::Text("%.1f", 100.0 * static_cast<double>(lv.triangles) / lod0Tris);
                ImGui::TableNextColumn();
                if (lv.maxError <= 0.0f) { ImGui::TextDisabled("0"); }
                else if (lv.minError < lv.maxError * 0.999f)
                {
                    ImGui::Text("%.4f .. %.4f", static_cast<double>(lv.minError), static_cast<double>(lv.maxError));
                }
                else { ImGui::Text("%.4f", static_cast<double>(lv.maxError)); }
            }
            ImGui::EndTable();
        }
        ImGui::PopTextWrapPos();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Level 0 = LOD0's clusters; each level simplifies groups of ~16 clusters to half. A slot's own\n"
                              "levels are counted in the row of their depth, so a slot that stops early leaves later rows\n"
                              "thinner. Error = the deviation each cluster of the level carries (object space): the cut takes\n"
                              "a cluster where that error spans tessy.lodPixelError pixels.");
        }
    }

    // What shaped it and who uses it. The weights are the LOD section's own keys (below), so a change there rebakes the
    // DAG with the LODs on Save; prune and fold are the process's (--tessy-lod-prune / --tessy-lod-fold).
    ImGui::Spacing();
    size_t foliageSlots = 0;
    for (size_t s = 0; s < slots_.size(); ++s) { foliageSlots += WindFoliageForSlot(s) > 0.0f ? 1u : 0u; }
    ImGui::TextDisabled("Shaped by: normal weight %.2f, leaf UV weight %.2f (%zu foliage slot(s)) -- LOD generation below;",
                        static_cast<double>(doc_.value("lodNormalWeight", 0.0f)),
                        static_cast<double>(doc_.value("foliageUvWeight", 0.0f)), foliageSlots);
    ImGui::TextDisabled("leaf prune %s, double-sided fold %s (this process: --tessy-lod-prune / --tessy-lod-fold).",
                        tessy::g_lodPrune_game ? "on" : "off", tessy::g_lodFold_game ? "on" : "off");
    ImGui::TextDisabled("Drawn: tessy.lod %s, DAG pixel error %.2f px x this asset's scale %.2f (LOD generation below).",
                        tessy::g_lod_game ? "ON" : "off", static_cast<double>(tessy::g_lodPixelError_game),
                        static_cast<double>(doc_.value("lodPixelErrorScale", 1.0f)));
}

void MeshEditorPanel::DrawRestPoseSection()
{
    const std::string geometry = doc_.value("geometry", std::string());
    const std::optional<Math::float3> rest = restpose::Read(doc_);
    bool pivotAtBase = doc_.value("pivot", std::string()) == "base";
    if (ImGui::Checkbox("Pivot at the base (re-bakes on Save)", &pivotAtBase))
    {
        if (pivotAtBase) { doc_["pivot"] = "base"; }
        else { doc_.erase("pivot"); }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Moves the model origin to where the mesh meets the ground: the lowest point,\n"
                          "centred on its footing. A part split out of a bigger file keeps that file's layout\n"
                          "otherwise -- a pivot a metre beside the plant, so wind and yaw swing it round.\n"
                          "Copies already placed move with the geometry by the same offset.");
    }
    ImGui::TextWrapped("How a NEW copy lies when it is spawned, placed or dropped into the viewport "
        "(the drag ghost shows it). Copies already in a level keep their own rotation.");
    const auto write = [this](const Math::float3& r)
    {
        const auto round2 = [](float v) { return std::round(v * 100.0f) / 100.0f; };
        doc_["restRotationDeg"] = nlohmann::json::array({ round2(r.x), round2(r.y), round2(r.z) });
    };
    if (rest)
    {
        float v[3] = { rest->x, rest->y, rest->z };
        if (ImGui::DragFloat3("Rotation (deg)", v, 0.5f, -180.0f, 180.0f, "%.1f"))
        {
            write(Math::float3(v[0], v[1], v[2]));
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("pitch, yaw, roll -- the level's rotationDeg order. Spawn turns it further\n"
                              "by its random yaw about the vertical; place by the item's yawDeg.");
        }
        ImGui::Text("Pivot raised %.3f m so the lowest point rests on the ground", restpose::Lift(geometry, *rest));
    }
    else
    {
        ImGui::TextDisabled("Upright, as authored.");
    }
    if (ImGui::Button("Lay flat (auto)"))
    {
        if (const std::optional<Math::float3> automatic = restpose::Auto(geometry)) { write(*automatic); }
        else { restPoseStatus_ = "Could not read the geometry: " + geometry; }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Onto its broadest side: the axis its surface is thinnest along points up,\n"
                          "a cupped shape rim-down (how a wave leaves a shell), a long one along its length.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Upright") && rest)
    {
        doc_.erase("restRotationDeg");
    }
    if (!restPoseStatus_.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", restPoseStatus_.c_str()); }
    ImGui::TextDisabled("Applies to new copies on Save.");
}

void MeshEditorPanel::SetWindFoliageForSlot(size_t slot, float value)
{
    // Keep the array exactly slot-count long so entry i always addresses submesh i; a short array
    // would silently shift every weight past the gap onto the wrong submesh.
    std::vector<float> weights(slots_.size(), 0.0f);
    for (size_t i = 0; i < weights.size(); ++i) { weights[i] = WindFoliageForSlot(i); }
    if (slot < weights.size()) { weights[slot] = value; }

    bool allZero = true;
    for (float w : weights) { if (w > 0.0f) { allZero = false; break; } }
    // All-zero means "no foliage authored" — drop the key so the runtime's alpha-mask default
    // applies again instead of pinning every slot to woody.
    if (allZero) { doc_.erase("windFoliage"); }
    else { doc_["windFoliage"] = weights; }
}

const TextureCube* MeshEditorPanel::ResolvePreviewSky(EditorContext& ctx, float& outExposure)
{
    const float intensity = std::max(0.0f, previewLight_.skyIntensity);
    outExposure = intensity;
    if (previewLight_.sky == "none")
    {
        return nullptr;
    }
    if (previewLight_.sky.empty())
    {
        const Skybox* skybox = ctx.scene.GetSkybox();
        // The authored trim, not GetExposure(): the preview has no tonemapper, and the physical
        // calibration folded into GetExposure() saturates the whole image to white. Skybox.h.
        outExposure = (skybox ? skybox->GetIntensity() : 1.0f) * intensity;
        return skybox ? skybox->GetTex() : nullptr;
    }
    if (previewSkyCubePath_ != previewLight_.sky)
    {
        // A different cube. The old one may still be read by a frame in flight, so the GPU is
        // drained before it goes: one stall on a click, instead of a texture freed under a
        // command list that still points at it.
        ctx.renderer.WaitForPreviousFrame();
        previewSkyCube_.reset();
        previewSkySpec_.reset();
        previewSkyIrradiance_.reset();
        previewSkyUpIlluminance_ = 0.0f;
        previewSkyError_.clear();
        previewSkyCubePath_ = previewLight_.sky;
        auto cube = std::make_unique<TextureCube>();
        UploadBatch batch;
        if (batch.Begin(&ctx.renderer) &&
            cube->CreateFromDDS(&ctx.renderer, batch.CommandList(),
                fs::path(previewLight_.sky).wstring(), batch.KeepAlive()))
        {
            // The derivatives, by name, as Skybox::Init finds the level's: both or neither, since a
            // prefiltered cube without its irradiance is a different lighting model, not a lesser one.
            fs::path stem(previewLight_.sky);
            stem.replace_extension();
            const std::wstring specPath = stem.wstring() + L"_spec.dds";
            const std::wstring diffusePath = stem.wstring() + L"_diffuse.dds";
            std::error_code ec;
            if (fs::exists(specPath, ec) && fs::exists(diffusePath, ec))
            {
                auto spec = std::make_unique<TextureCube>();
                auto irradiance = std::make_unique<TextureCube>();
                if (spec->CreateFromDDS(&ctx.renderer, batch.CommandList(), specPath,
                        batch.KeepAlive()) &&
                    irradiance->CreateFromDDS(&ctx.renderer, batch.CommandList(), diffusePath,
                        batch.KeepAlive()))
                {
                    previewSkySpec_ = std::move(spec);
                    previewSkyIrradiance_ = std::move(irradiance);
                    previewSkyUpIlluminance_ = Skybox::MeasureUpIlluminance(diffusePath);
                }
            }
            batch.SubmitAndWait(&ctx.renderer);
            previewSkyCube_ = std::move(cube);
        }
        else
        {
            previewSkyError_ = "Could not load " + previewLight_.sky + " as a cubemap.";
        }
    }
    return previewSkyCube_ ? previewSkyCube_.get() : nullptr;
}

bool MeshEditorPanel::BuildPreviewLighting(EditorContext& ctx,
    EditorPreviewRenderer::PhysicalLighting& out)
{
    levelReadout_ = {};
    if (!previewLight_.levelLighting)
    {
        return false;
    }
    const Scene& scene = ctx.scene;
    const DirectionalLight& sun = scene.GetDirectionalLight();
    const Skybox* skybox = scene.GetSkybox();
    const float trim = std::max(0.0f, previewLight_.skyIntensity);

    // The sun, as FillLightingConstants hands it to lighting_cs. Only its direction is the
    // preview's own.
    out.sunIlluminance = sun.GetEffectiveColor();
    out.sunHalfApex = sun.GetSunHalfApexRadians();
    out.sunSoftRadius = sun.GetSoftSourceRadius();
    out.flatAmbient = sun.GetAmbient();
    out.lightExposure = sun.GetExposure();
    out.groundAlbedo = sun.GetGroundAlbedo();
    out.skyFill = sun.GetSkyFillIntensity();
    const auto addResource = [&out](ID3D12Resource* resource)
    {
        for (ID3D12Resource*& slot : out.resources)
        {
            if (slot == nullptr) { slot = resource; return; }
        }
    };
    const Texture2D* brdfLut = skybox ? skybox->GetBrdfLut() : nullptr;
    const bool hasLut = brdfLut && brdfLut->GetResource();

    bool calibrated = true;
    if (previewLight_.sky.empty() && skybox)
    {
        // The level's sky, whichever kind it is: the procedural environment's three cubes when the
        // sky atmosphere supplies them, else the file sky and its F7 siblings.
        out.sky = skybox->EnvironmentSrv();
        out.skyMips = skybox->HasEnvironment() ? 8u : std::max(skybox->GetTex()->GetMips(), 1u);
        if (skybox->HasIbl())
        {
            out.specular = skybox->SpecularSrv();
            out.specularMips = skybox->GetSpecMips();
            out.irradiance = skybox->IrradianceSrv();
        }
        out.skyIntensity = skybox->GetExposure() * trim;
        if (skybox->HasEnvironment())
        {
            for (ID3D12Resource* resource : skybox->EnvironmentResources()) { addResource(resource); }
        }
        else
        {
            addResource(skybox->GetTex()->GetResource());
            addResource(skybox->GetSpecTex()->GetResource());
            addResource(skybox->GetIrradianceTex()->GetResource());
        }
    }
    else if (previewLight_.sky != "none" && previewSkyCube_)
    {
        // A picked cube, calibrated the way Skybox calibrates the level's: its measured horizontal
        // illuminance is scaled to the level sky's authored lux (a clear sky's 12,000 when the
        // level authored none). Without a `_diffuse.dds` there is nothing to measure, and the cube
        // goes in raw -- next to a sun of 10^5 lux that is black, which the panel then says.
        out.sky = previewSkyCube_->GetSRVCPU();
        out.skyMips = std::max(previewSkyCube_->GetMips(), 1u);
        addResource(previewSkyCube_->GetResource());
        if (previewSkySpec_ && previewSkyIrradiance_ && hasLut)
        {
            out.specular = previewSkySpec_->GetSRVCPU();
            out.specularMips = previewSkySpec_->GetMips();
            out.irradiance = previewSkyIrradiance_->GetSRVCPU();
            addResource(previewSkySpec_->GetResource());
            addResource(previewSkyIrradiance_->GetResource());
        }
        const float levelLux = skybox ? skybox->GetIlluminanceLux() : 0.0f;
        const float targetLux = levelLux > 0.0f ? levelLux : 12000.0f;
        calibrated = previewSkyUpIlluminance_ > 1.0e-8f;
        out.skyIntensity = (calibrated ? targetLux / previewSkyUpIlluminance_ : 1.0f) * trim;
    }
    if (out.specularMips > 0 && hasLut)
    {
        out.brdfLut = brdfLut->GetSRVCPU();
        addResource(brdfLut->GetResource());
    }

    // The camera is the preview's own (MeshEditorPreviewLight::autoExposure). Auto: what a meter reads off middle grey lying under this light
    // (EV100 = log2(0.18 E / pi) + 3, as TimeOfDay::NightEv100 and the meter's own log2(L) + 3): the sun on a level card by the PREVIEW's direction
    // and the sky's horizontal illuminance. The EV is rounded to 1/64 stop so a sky that is still moving does not re-render the preview every frame
    // for an invisible change. The colour pipeline below is the level's.
    const float sunLuminance = 0.2126f * out.sunIlluminance.x + 0.7152f * out.sunIlluminance.y + 0.0722f * out.sunIlluminance.z;
    float ev = previewLight_.manualEv100;
    if (previewLight_.autoExposure)
    {
        constexpr float kPi = 3.14159265358979f;
        float skyLux = 0.0f;
        if (previewLight_.sky.empty() && skybox)
        {
            // The procedural sky's distant light is the luminance of a sky of even brightness (a few frames old; before its first readback 0, and the sun alone meters).
            const float4& distant = g_fogDistantSkyLight;
            skyLux = skybox->HasEnvironment()
                ? kPi * (0.2126f * distant.x + 0.7152f * distant.y + 0.0722f * distant.z) * out.skyIntensity
                : skybox->GetIlluminanceLux() * trim;
        }
        else if (previewLight_.sky != "none" && previewSkyCube_)
        {
            const float levelLux = skybox ? skybox->GetIlluminanceLux() : 0.0f;
            skyLux = (levelLux > 0.0f ? levelLux : 12000.0f) * trim;
        }
        const float lux = sunLuminance * std::max(-previewLight_.direction.y, 0.0f) + skyLux * out.skyFill;
        constexpr float kMiddleGreyAlbedo = 0.18f;
        ev = std::log2(std::max(kMiddleGreyAlbedo * lux / kPi, 1.0e-4f)) + 3.0f - previewLight_.exposureCompensationEv;
    }
    ev = std::round(ev * 64.0f) / 64.0f;
    out.exposure = render::ExposureMultiplierFromEv100(ev);
    levelReadout_.ev100 = ev;
    const render::ColorPipelineSettings& color = scene.GetColorPipeline();
    out.toneCurve = static_cast<std::uint32_t>(color.toneCurve);
    out.gradeSaturation = color.gradeSaturation;
    out.gradeContrast = color.gradeContrast;
    out.gradeGamma = color.gradeGamma;
    out.gradeGain = color.gradeGain;
    out.gradeOffset = color.gradeOffset;
    out.agxSlope = color.agxSlope;
    out.agxPower = color.agxPower;
    out.agxSaturation = color.agxSaturation;
    out.filmSlope = color.filmSlope;
    out.filmToe = color.filmToe;
    out.filmShoulder = color.filmShoulder;
    out.filmBlackClip = color.filmBlackClip;
    out.filmWhiteClip = color.filmWhiteClip;

    levelReadout_.built = true;
    levelReadout_.sunLux = sunLuminance;
    levelReadout_.skyIbl = out.irradiance.ptr != 0;
    levelReadout_.skyScale = out.skyIntensity;
    levelReadout_.skyCalibrated = calibrated;
    return true;
}

void MeshEditorPanel::DrawPreviewSkyControls(AssetRegistry& registry)
{
    ImGui::SeparatorText("Preview Lighting");
    ImGui::Checkbox("Level lighting", &previewLight_.levelLighting);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Light the preview the way the open level lights the scene: its sun in "
            "lux, its sky's diffuse and specular (split-sum IBL), and its grade and tone curve. The "
            "light's direction and the exposure below stay the preview's own.\nOff: the preview's "
            "own studio light, with no camera.");
    }
    if (previewLight_.levelLighting && levelReadout_.built)
    {
        ImGui::Checkbox("Auto exposure", &previewLight_.autoExposure);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("On: exposed for the preview's own light -- a grey card lying under this sun and sky. "
                "The viewport does not come into it:\nit may stand in a closed room, 6 stops under the sun lighting this preview.\n"
                "Off: the EV100 you give it.");
        }
        if (previewLight_.autoExposure)
        {
            ImGui::DragFloat("Compensation (EV)", &previewLight_.exposureCompensationEv, 0.05f, -16.0f, 16.0f, "%+.2f");
            ImGui::TextDisabled("Camera: EV100 %.2f", levelReadout_.ev100);
        }
        else
        {
            ImGui::DragFloat("EV100", &previewLight_.manualEv100, 0.05f, -16.0f, 24.0f, "%.2f");
        }
        ImGui::TextDisabled("Sky fill: %s", levelReadout_.skyIbl
            ? "the sky's irradiance" : "the level's flat ambient (no irradiance cube)");
    }
    const auto labelFor = [](const std::string& sky) -> std::string
    {
        if (sky.empty())
        {
            return "Level sky";
        }
        if (sky == "none")
        {
            return "None (light + ambient only)";
        }
        return fs::path(sky).filename().string();
    };
    if (ImGui::BeginCombo("Sky", labelFor(previewLight_.sky).c_str()))
    {
        int id = 0;
        const auto option = [&](const std::string& value, const char* hint)
        {
            // By index: two cubes with the same file name in different folders would otherwise
            // be two widgets with one ImGui identity.
            ImGui::PushID(id++);
            const bool selected = previewLight_.sky == value;
            if (ImGui::Selectable(labelFor(value).c_str(), selected))
            {
                previewLight_.sky = value;
            }
            if (hint != nullptr && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", hint);
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        };
        option("", "The open level's own sky, at its own intensity.");
        option("none", "No sky: the directional light and the ambient fill only.");
        for (const EditorAssetRecord& record : registry.Assets())
        {
            if (record.texture.kind != EditorTextureKind::TextureCube)
            {
                continue;
            }
            // A sky's F7 derivatives are cubes too, but they are its lighting, not a sky: picking
            // one shows a blurred sky that no level would ever display. They come with their sky.
            const std::string stem = fs::path(record.path).stem().string();
            const auto endsWith = [&stem](const char* suffix)
            {
                const std::size_t n = std::strlen(suffix);
                return stem.size() > n && stem.compare(stem.size() - n, n, suffix) == 0;
            };
            if (endsWith("_spec") || endsWith("_diffuse"))
            {
                continue;
            }
            option(record.path, record.path.c_str());
        }
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(previewLight_.sky == "none");
    ImGui::DragFloat("Sky intensity", &previewLight_.skyIntensity, 0.01f, 0.0f, 16.0f, "%.2f",
        ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Multiplies the sky in this preview only: the background and the "
            "reflections, and under Level lighting its diffuse fill too. On the level sky it "
            "rides on top of that sky's own intensity.");
    }
    if (!previewSkyError_.empty() && previewLight_.sky == previewSkyCubePath_)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "%s", previewSkyError_.c_str());
    }
    else if (previewLight_.levelLighting && levelReadout_.built && !levelReadout_.skyCalibrated)
    {
        // Said, not hidden: raw cube values next to a sun of 10^5 lux read as a black sky, which
        // looks like a bug in the preview rather than a cube with nothing to calibrate it by.
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
            "No _diffuse.dds beside this cube: its brightness is uncalibrated (x1).");
    }
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

    // Which slots are foliage is a BAKE input, not just a runtime multiplier: it seeds the along-limb
    // distance field that keeps the streaming term continuous across submesh junctions. Moving a slot
    // between 0 and non-zero therefore invalidates the baked geometry, and leaving it stale makes the
    // tree kink where the modeller happened to cut a leaf in two. The weights themselves are runtime,
    // so nudging 0.6 -> 0.8 costs nothing here (BinaryNeedsRebake only sees the zero/non-zero pattern).
    bool rebaked = false, rebakeFailed = false;
    {
        const std::string src = doc_.value("source", std::string());
        const std::string geom = doc_.value("geometry", std::string());
        const bool isBaked = geom.size() > 9 && geom.rfind(".mesh.bin") == geom.size() - 9;
        if (!src.empty() && isBaked)
        {
            MeshLoadOptions opt;
            opt.generateTangentSpace = true;
            opt.wantCW = false; // match the runtime load, as ImportPanel does
            opt.recomputeNormalSlots = recomputeNormalSlots_;
            const auto wf = doc_.find("windFoliage");
            if (wf != doc_.end() && wf->is_array())
            {
                for (const nlohmann::json& v : *wf)
                {
                    if (v.is_number()) { opt.slotFoliage.push_back(v.get<float>()); }
                }
            }
            // The whole-chain LOD knobs the import dialog persisted (see WriteImportedMeshAsset):
            // this re-bake MUST use them or Save rolls the geometry back to defaults.
            const auto lrs = doc_.find("lodRatioScale");
            if (lrs != doc_.end() && lrs->is_number()) { opt.lodRatioScale = lrs->get<float>(); }
            const auto les = doc_.find("lodErrorScale");
            if (les != doc_.end() && les->is_number()) { opt.lodErrorScale = les->get<float>(); }
            if (doc_.value("lodPermissive", false)) { opt.lodSimplifyOptions |= meshopt_SimplifyPermissive; }
            if (doc_.value("lodDropSmallParts", false)) { opt.lodSimplifyOptions |= meshopt_SimplifyPrune; }
            // Optional per-asset override of the LOD3 foliage prune (default 0.35, 0 = off).
            const auto pk = doc_.find("foliagePruneKeep");
            if (pk != doc_.end() && pk->is_number()) { opt.foliagePruneKeep = pk->get<float>(); }
            // Per-asset opt-out of the harsh LOD3 solid budget (terrain carries false).
            const auto la = doc_.find("lod3Aggressive");
            if (la != doc_.end() && la->is_boolean()) { opt.lod3Aggressive = la->get<bool>(); }
            const auto l3r = doc_.find("lod3RatioScale");
            if (l3r != doc_.end() && l3r->is_number()) { opt.lod3RatioScale = l3r->get<float>(); }
            const auto l3e = doc_.find("lod3ErrorScale");
            if (l3e != doc_.end() && l3e->is_number()) { opt.lod3ErrorScale = l3e->get<float>(); }
            const auto fir = doc_.find("foliageInnerRatio");
            if (fir != doc_.end() && fir->is_number()) { opt.foliageInnerRatio = fir->get<float>(); }
            const auto fie = doc_.find("foliageInnerError");
            if (fie != doc_.end() && fie->is_number()) { opt.foliageInnerError = fie->get<float>(); }
            const auto fg = doc_.find("foliageGrow");
            if (fg != doc_.end() && fg->is_number()) { opt.foliageGrow = fg->get<float>(); }
            const auto fuv = doc_.find("foliageUvWeight");
            if (fuv != doc_.end() && fuv->is_number()) { opt.foliageUvWeight = fuv->get<float>(); }
            const auto lnw = doc_.find("lodNormalWeight");
            if (lnw != doc_.end() && lnw->is_number()) { opt.lodNormalWeight = lnw->get<float>(); }
            const auto readDropList = [this](const char* key, std::vector<uint32_t>& out)
            {
                const auto it = doc_.find(key);
                if (it == doc_.end() || !it->is_array()) { return; }
                for (const nlohmann::json& v : *it)
                {
                    if (v.is_number_integer()) { out.push_back(v.get<uint32_t>()); }
                }
            };
            readDropList("lod1DropSlots", opt.lod1DropSlots);
            readDropList("lod2DropSlots", opt.lod2DropSlots);
            readDropList("lod3DropSlots", opt.lod3DropSlots);
            // Unit correction folded into the vertices at import time. Without it a Save
            // re-bake reverts the mesh to the SOURCE unit scale.
            const auto bs = doc_.find("bakeScale");
            if (bs != doc_.end() && bs->is_number()) { opt.bakeScale = bs->get<float>(); }
            // The pivot move is folded into the vertices the same way.
            opt.pivotToBase = doc_.value("pivot", std::string()) == "base";
            // Mesh chunking is a bake input too: omitting it here would re-bake a chunked
            // terrain into one giant caster while mesh.json keeps claiming chunks.
            const auto cg = doc_.find("chunkGrid");
            if (cg != doc_.end() && cg->is_number_integer() && cg->get<int>() > 0)
            {
                opt.chunkGrid = static_cast<unsigned int>(cg->get<int>());
            }
            if (MeshManager::BinaryNeedsRebake(geom, opt))
            {
                MeshManager mm;
                std::vector<float> uvDensity;
                if (mm.BakeToBinary(src, geom, opt, &uvDensity))
                {
                    rebaked = true;
                    // Texture streaming A1: the manifest went to disk above, before the bake; the
                    // density the bake measured follows it now, in memory and in the file.
                    if (!uvDensity.empty()) { doc_["uvDensity"] = uvDensity; }
                    else { doc_.erase("uvDensity"); }
                    MeshManager::WriteManifestUvDensity(path_, uvDensity);
                }
                else { rebakeFailed = true; }
            }
        }
    }

    // A re-bake rewrote the .mesh.bin, but the respawn below re-loads through MeshManager,
    // which CACHES geometry by bin path — without dropping the cache every "updated" instance
    // gets the pre-bake mesh handed straight back (the import flow clears it in
    // ApplyImportInvalidation; this path must too — user-hit 2026-08-21: Save re-baked, the
    // panel preview showed the new LODs, the level kept the old ones).
    if (rebaked)
    {
        if (MeshManager* meshes = ctx.renderer.GetMeshManager()) { meshes->Clear(); }
        buoyancy::ForgetGeometry(doc_.value("geometry", std::string()));
        // The bake's own numbers (deviation, clusters, DAG) for the sections that show them.
        MeshManager::BinaryInfo fresh;
        if (MeshManager::DescribeMeshBinary(doc_.value("geometry", std::string()), fresh)) { binInfo_ = std::move(fresh); }
    }

    // Live-apply to placed instances (must happen AFTER the file is written AND after any re-bake —
    // the respawn re-reads both).
    const int applied = ApplyToScene(ctx);
    status_ = applied > 0 ? ("Saved — updated " + std::to_string(applied) + " placed instance(s).")
                          : "Saved.";
    if (rebaked) { status_ += " Re-baked wind weights."; }
    if (rebakeFailed) { status_ += " Wind weight re-bake FAILED (geometry left stale)."; }
}

#endif // WITH_EDITOR
