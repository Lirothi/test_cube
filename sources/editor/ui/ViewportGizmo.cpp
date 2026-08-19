#include "editor/ui/ViewportGizmo.h"
#if WITH_EDITOR

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <cmath>

#include <DirectXMath.h>

#include "app/camera/Camera.h"
#include "app/scene/Scene.h"
#include "core/math/Math.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "editor/EditorContext.h"
#include "editor/EditorExtensionRegistry.h"
#include "editor/scene/EnvironmentRuntime.h"
#include "editor/commands/CompositeCommand.h"
#include "editor/commands/EditEnvironmentCommand.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/SetMaterialCommand.h"
#include "editor/commands/SpawnMeshCommand.h"
#include "app/scene/SceneObjectFactory.h"
#include "rendering/core/UploadBatch.h"
#include "editor/commands/TransformObjectCommand.h"
#include "editor/ui/EditorDragDrop.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/debug/DebugDraw.h"
#include "rendering/renderables/RenderableObject.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo/ImGuizmo.h"

namespace
{
    // Store a mat4 as a column-of-16 floats for ImGuizmo (it accepts the engine's
    // row-major bytes; for the same transform they match GL column-major layout).
    void ToFloat16(const Math::mat4& m, float out[16])
    {
        DirectX::XMFLOAT4X4 f;
        DirectX::XMStoreFloat4x4(&f, m.xm());
        std::memcpy(out, &f.m[0][0], sizeof(float) * 16);
    }

    Math::mat4 FromFloat16(const float values[16])
    {
        DirectX::XMFLOAT4X4 matrix;
        std::memcpy(&matrix, values, sizeof(matrix));
        return Math::mat4(matrix);
    }

    EditorTransform TransformFromMatrix(const Math::mat4& matrix)
    {
        float values[16];
        ToFloat16(matrix, values);
        float translation[3];
        float rotation[3];
        float scale[3];
        ImGuizmo::DecomposeMatrixToComponents(values, translation, rotation, scale);

        EditorTransform transform;
        transform.position = Math::float3(translation[0], translation[1], translation[2]);
        transform.rotationDeg = Math::float3(rotation[0], rotation[1], rotation[2]);
        transform.scale = Math::float3(scale[0], scale[1], scale[2]);
        return transform;
    }

    bool TransformMatches(const EditorTransform& lhs, const EditorTransform& rhs)
    {
        constexpr float epsilon = 1.0e-4f;
        const auto close = [epsilon](float a, float b)
        {
            return std::fabs(a - b) <= epsilon;
        };
        return close(lhs.position.x, rhs.position.x) &&
            close(lhs.position.y, rhs.position.y) &&
            close(lhs.position.z, rhs.position.z) &&
            close(lhs.rotationDeg.x, rhs.rotationDeg.x) &&
            close(lhs.rotationDeg.y, rhs.rotationDeg.y) &&
            close(lhs.rotationDeg.z, rhs.rotationDeg.z) &&
            close(lhs.scale.x, rhs.scale.x) &&
            close(lhs.scale.y, rhs.scale.y) &&
            close(lhs.scale.z, rhs.scale.z);
    }

    bool BuildViewportCursorRay(const Camera& camera,
        const ImVec2& mousePosition,
        const ImVec2& viewportOrigin,
        float width,
        float height,
        Math::float3& outOrigin,
        Math::float3& outDirection)
    {
        if (width <= 0.0f || height <= 0.0f)
        {
            return false;
        }

        const float localX = mousePosition.x - viewportOrigin.x;
        const float localY = mousePosition.y - viewportOrigin.y;
        const float ndcX = 2.0f * (localX / width) - 1.0f;
        const float ndcY = 1.0f - 2.0f * (localY / height);
        const Math::float3 viewPoint =
            camera.GetInvProjMatrixNoJitter().TransformPoint(Math::float3(ndcX, ndcY, 1.0f));
        const Math::float3 worldPoint = camera.GetInvViewMatrix().TransformPoint(viewPoint);

        outOrigin = camera.GetPosition();
        outDirection = (worldPoint - outOrigin).Normalized();
        return outDirection.Length() > Math::EPS;
    }

    ImGuizmo::OPERATION ToImGuizmo(ViewportGizmo::Op op)
    {
        switch (op)
        {
        case ViewportGizmo::Op::Select:
        case ViewportGizmo::Op::Translate: return ImGuizmo::TRANSLATE;
        case ViewportGizmo::Op::Rotate: return ImGuizmo::ROTATE;
        case ViewportGizmo::Op::Scale:  return ImGuizmo::SCALE;
        default:                        return ImGuizmo::TRANSLATE;
        }
    }

    Math::float3 ReadFloat3(const nlohmann::json& p, const char* key, const Math::float3& def)
    {
        const auto it = p.find(key);
        if (it != p.end() && it->is_array() && it->size() >= 3u)
        {
            return Math::float3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
        }
        return def;
    }

    // Build a row-major model matrix for a light: translation = pos, row2 (Z) =
    // the light direction (so ROTATE edits direction, extracted back from row2).
    void BuildLightMatrix(const Math::float3& pos, const Math::float3& dir, float out[16])
    {
        using namespace DirectX;
        XMVECTOR fwd = XMVector3Normalize(XMVectorSet(dir.x, dir.y, dir.z, 0.0f));
        if (XMVector3Equal(fwd, XMVectorZero())) { fwd = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f); }
        const XMVECTOR up0 = (std::fabs(XMVectorGetY(fwd)) > 0.99f)
            ? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)
            : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(up0, fwd));
        const XMVECTOR up = XMVector3Cross(fwd, right);
        XMFLOAT3 r, u, f;
        XMStoreFloat3(&r, right);
        XMStoreFloat3(&u, up);
        XMStoreFloat3(&f, fwd);
        out[0] = r.x;  out[1] = r.y;  out[2] = r.z;  out[3] = 0.0f;
        out[4] = u.x;  out[5] = u.y;  out[6] = u.z;  out[7] = 0.0f;
        out[8] = f.x;  out[9] = f.y;  out[10] = f.z; out[11] = 0.0f;
        out[12] = pos.x; out[13] = pos.y; out[14] = pos.z; out[15] = 1.0f;
    }

    const IEditorObjectFactory* FindDefaultMeshFactory(const EditorExtensionRegistry& extensions,
        const EditorAssetRecord* record)
    {
        for (const std::unique_ptr<IEditorObjectFactory>& factory : extensions.ObjectFactories())
        {
            if (factory && factory->CanBuildFromAsset(record))
            {
                return factory.get();
            }
        }
        return nullptr;
    }

    // ---- drag-to-spawn preview -------------------------------------------------------------
    //
    // The real mesh, spawned into the SCENE (not the document) while the drag is over the
    // viewport, so what you see under the cursor is what lands. Scene-only means the outliner and
    // the save path never see it, and the command stack stays clean: the actual spawn on drop is
    // still the ordinary SpawnMeshCommand, so undo behaves exactly as before.
    //
    // The id sits far above anything the document allocates. It has to be excluded from the
    // placement raycast as well -- otherwise the preview is the surface under the cursor and it
    // climbs its own back, which is what Scene::RaycastEditorObject's `ignoredObjectId` is for.
    constexpr Scene::SceneObjectId kSpawnPreviewId = 0xFFFF'FFFF'0000'0001ull;

    struct SpawnPreviewState
    {
        bool alive = false;
        std::string assetKey; // rebuild only when the dragged asset actually changes
    };
    SpawnPreviewState g_spawnPreview;

    void ClearSpawnPreview(EditorContext& ctx)
    {
        if (!g_spawnPreview.alive)
        {
            return;
        }
        ctx.scene.RemoveEditorObject(kSpawnPreviewId);
        g_spawnPreview = SpawnPreviewState{};
    }

    // Places the preview at `position`, building it first if this is a new asset. Returns false
    // when the mesh could not be created -- the caller then just shows the tooltip, as before.
    bool UpdateSpawnPreview(EditorContext& ctx,
                            const IEditorObjectFactory& factory,
                            const EditorAssetRecord& record,
                            const AssetRegistry& registry,
                            const Math::float3& position)
    {
        if (g_spawnPreview.alive && g_spawnPreview.assetKey == record.id.key)
        {
            RenderableObjectBase* runtime = ctx.scene.FindEditorObject(kSpawnPreviewId);
            if (RenderableObject* ro = runtime ? runtime->AsRenderableObject() : nullptr)
            {
                ro->SetPosition(position); // the cheap path: every frame of the drag lands here
                return true;
            }
            ClearSpawnPreview(ctx); // it went away underneath us; fall through and rebuild
        }

        ClearSpawnPreview(ctx);
        const nlohmann::json objectJson =
            factory.BuildDefaultJson(&record, ctx, registry, &position);
        std::unique_ptr<RenderableObjectBase> runtime =
            SceneObjectFactory::CreateStaticMeshFromJson(objectJson);
        if (!runtime)
        {
            return false;
        }
        UploadBatch uploads;
        if (!uploads.Begin(&ctx.renderer))
        {
            return false;
        }
        const bool added = ctx.scene.AddInitializedEditorObject(
            ctx.renderer, uploads, kSpawnPreviewId, std::move(runtime));
        uploads.SubmitAndWait(&ctx.renderer);
        if (!added)
        {
            return false;
        }
        g_spawnPreview.alive = true;
        g_spawnPreview.assetKey = record.id.key;
        return true;
    }

    void DrawViewportDropTarget(EditorContext& ctx,
        EditorCommandStack& commandStack,
        const AssetRegistry& registry,
        const EditorExtensionRegistry& extensions,
        float width,
        float height)
    {
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow |
                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        {
            return;
        }

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (!viewport)
        {
            return;
        }

        // The drag can end anywhere -- Esc, a drop over a panel, a release outside the window --
        // and none of those reach the delivery branch below. Tying teardown to "is a drag in
        // flight at all" is what stops a cancelled drag leaving a ghost mesh in the level.
        if (!ImGui::IsDragDropActive())
        {
            ClearSpawnPreview(ctx);
        }

        const ImRect targetRect(
            viewport->Pos,
            ImVec2(viewport->Pos.x + width, viewport->Pos.y + height));
        if (!ImGui::BeginDragDropTargetViewport(viewport, &targetRect))
        {
            ClearSpawnPreview(ctx); // dragged back out of the viewport
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

            const IEditorObjectFactory* factory = nullptr;
            EditorObject* selectedObject = nullptr;
            const char* reason = nullptr;
            if (!record)
            {
                reason = "Dragged asset is no longer in the registry.";
            }
            else if (record->id.type == EditorAssetType::Mesh)
            {
                factory = FindDefaultMeshFactory(extensions, record);
                if (!factory)
                {
                    reason = "No object factory can spawn this mesh.";
                }
            }
            else if (record->id.type == EditorAssetType::MaterialPreset)
            {
                selectedObject = ctx.document.Find(ctx.selection.Primary());
                if (!selectedObject)
                {
                    reason = "Select a static mesh before dropping a material.";
                }
                else if (selectedObject->type != "staticMesh")
                {
                    reason = "Selected object does not support material assignment.";
                }
            }
            else
            {
                reason = "Only mesh and material assets can be dropped in the viewport.";
            }

            if (reason)
            {
                ImGui::SetTooltip("%s", reason);
            }
            else
            {
                ImGui::GetForegroundDrawList()->AddRect(targetRect.Min,
                    targetRect.Max,
                    ImGui::GetColorU32(ImGuiCol_DragDropTarget),
                    0.0f,
                    0,
                    2.0f);
                if (record->id.type == EditorAssetType::Mesh)
                {
                    // The placement point, recomputed every frame of the drag. It was already
                    // computed here on delivery; hoisting it out of that branch is what lets the
                    // preview stand exactly where the spawn will land, by construction rather
                    // than by two copies of the same arithmetic agreeing.
                    Math::float3 positionHint;
                    const Math::float3* positionHintPtr = nullptr;
                    Math::float3 rayOrigin;
                    Math::float3 rayDirection;
                    if (BuildViewportCursorRay(ctx.scene.CameraRef(),
                            ImGui::GetIO().MousePos,
                            viewport->Pos,
                            width,
                            height,
                            rayOrigin,
                            rayDirection))
                    {
                        float hitDistance = 0.0f;
                        // Ignore the preview itself: it is a scene object like any other, so
                        // without this it becomes the surface under the cursor and climbs itself.
                        if (ctx.scene.RaycastEditorObject(rayOrigin, rayDirection, &hitDistance,
                                kSpawnPreviewId) != 0 &&
                            std::isfinite(hitDistance))
                        {
                            positionHint = rayOrigin + rayDirection * hitDistance;
                            positionHintPtr = &positionHint;
                        }
                    }

                    if (payload->IsDelivery())
                    {
                        // Remove the preview BEFORE the real spawn, so the two never coexist.
                        ClearSpawnPreview(ctx);
                        nlohmann::json objectJson =
                            factory->BuildDefaultJson(record, ctx, registry, positionHintPtr);
                        commandStack.Execute(ctx,
                            std::make_unique<SpawnMeshCommand>(std::move(objectJson)));
                    }
                    else if (positionHintPtr)
                    {
                        ImGui::SetTooltip("Spawn %s", record->displayName.c_str());
                        UpdateSpawnPreview(ctx, *factory, *record, registry, positionHint);
                    }
                    else
                    {
                        // Nothing under the cursor to place on: no preview, and say so rather
                        // than leaving a stale ghost from the last position that did hit.
                        ClearSpawnPreview(ctx);
                        ImGui::SetTooltip("Spawn %s  (point at a surface)",
                            record->displayName.c_str());
                    }
                }
                else
                {
                    ImGui::SetTooltip("Assign %s to %s",
                        record->displayName.c_str(),
                        selectedObject->name.c_str());
                    if (payload->IsDelivery())
                    {
                        commandStack.Execute(ctx,
                            std::make_unique<SetMaterialCommand>(
                                selectedObject->id,
                                record->id.key));
                    }
                }
            }
        }

        if (ImGui::AcceptDragDropPayload(EditorDragDrop::kFolderPayloadType, flags))
        {
            ImGui::SetTooltip("Folder drops do not move files or spawn objects.");
        }

        ImGui::EndDragDropTarget();
    }
}

const char* ViewportGizmo::ModeLabel(Op op)
{
    switch (op)
    {
    case Op::Select:    return "Select";
    case Op::Translate: return "Translate";
    case Op::Rotate:    return "Rotate";
    case Op::Scale:     return "Scale";
    default:            return "Unknown";
    }
}

const char* ViewportGizmo::ModeLabel() const
{
    return ModeLabel(op_);
}

ViewportGizmo::PersistentState ViewportGizmo::GetPersistentState() const
{
    return PersistentState{
        snapEnabled_,
        translationIncrement_,
        rotationIncrement_,
        scaleIncrement_,
        transformSpace_
    };
}

void ViewportGizmo::SetPersistentState(const PersistentState& state)
{
    snapEnabled_ = state.snapEnabled;
    translationIncrement_ = std::isfinite(state.translationIncrement) ?
        std::clamp(state.translationIncrement, 0.001f, 10000.0f) : 0.5f;
    rotationIncrement_ = std::isfinite(state.rotationIncrement) ?
        std::clamp(state.rotationIncrement, 0.1f, 180.0f) : 15.0f;
    scaleIncrement_ = std::isfinite(state.scaleIncrement) ?
        std::clamp(state.scaleIncrement, 0.001f, 10.0f) : 0.1f;
    transformSpace_ = state.transformSpace == TransformSpace::Local ?
        TransformSpace::Local :
        TransformSpace::World;
}

void ViewportGizmo::CycleTransformMode()
{
    switch (op_)
    {
    case Op::Translate: op_ = Op::Rotate; break;
    case Op::Rotate:    op_ = Op::Scale; break;
    case Op::Scale:     op_ = Op::Translate; break;
    case Op::Select:
    default:            op_ = Op::Translate; break;
    }
}

void ViewportGizmo::DrawModeButtons(const char* hotkeyHintText)
{
    int mode = static_cast<int>(op_);
    ImGui::Text("Transform mode: %s", ModeLabel());
    if (hotkeyHintText && hotkeyHintText[0] != '\0')
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", hotkeyHintText);
        ImGui::PopStyleColor();
    }
    ImGui::TextUnformatted("Gizmo:");
    ImGui::SameLine();
    ImGui::RadioButton("Select", &mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Translate", &mode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Rotate", &mode, 2);
    ImGui::SameLine();
    ImGui::RadioButton("Scale", &mode, 3);
    if (mode >= static_cast<int>(Op::Select) && mode <= static_cast<int>(Op::Scale))
    {
        op_ = static_cast<Op>(mode);
    }

    ImGui::TextUnformatted("Space:");
    ImGui::SameLine();
    const char* spaceLabel = transformSpace_ == TransformSpace::Local ? "LOCAL" : "WORLD";
    if (ImGui::Button(spaceLabel, ImVec2(62.0f, 0.0f)))
    {
        transformSpace_ = transformSpace_ == TransformSpace::Local ?
            TransformSpace::World :
            TransformSpace::Local;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Object gizmo transform space. Environment lights always use WORLD.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &snapEnabled_);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Hold Ctrl during a gizmo drag to temporarily invert snapping.");
    }

    ImGui::SetNextItemWidth(54.0f);
    ImGui::DragFloat("Move##translationSnap", &translationIncrement_, 0.05f,
        0.001f, 10000.0f, "%.3f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Translation snap increment in scene units.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(54.0f);
    ImGui::DragFloat("Rotate##rotationSnap", &rotationIncrement_, 0.5f,
        0.1f, 180.0f, "%.1f deg");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Rotation snap increment in degrees.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(54.0f);
    ImGui::DragFloat("Scale##scaleSnap", &scaleIncrement_, 0.01f,
        0.001f, 10.0f, "%.3f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Scale snap increment.");
    }
    translationIncrement_ = std::isfinite(translationIncrement_) ?
        std::clamp(translationIncrement_, 0.001f, 10000.0f) : 0.5f;
    rotationIncrement_ = std::isfinite(rotationIncrement_) ?
        std::clamp(rotationIncrement_, 0.1f, 180.0f) : 15.0f;
    scaleIncrement_ = std::isfinite(scaleIncrement_) ?
        std::clamp(scaleIncrement_, 0.001f, 10.0f) : 0.1f;
}

void ViewportGizmo::Update(EditorContext& ctx,
    EditorCommandStack& commandStack,
    const AssetRegistry& registry,
    const EditorExtensionRegistry& extensions)
{
    CPU_SCOPE(ProfilerScopes::kViewportGizmoUpdate);
    ImGuiIO& io = ImGui::GetIO();

    const float width = static_cast<float>(ctx.renderer.GetWidth());
    const float height = static_cast<float>(ctx.renderer.GetHeight());
    if (width <= 0.0f || height <= 0.0f) { return; }

    DrawViewportDropTarget(ctx, commandStack, registry, extensions, width, height);

    struct IconHit { ImVec2 mn; ImVec2 mx; EditorObjectId id; };
    std::vector<IconHit> iconHits;

    uint32_t pickedObjectId = 0;
    if (ctx.renderer.ConsumeObjectIdPick(pickedObjectId))
    {
        EditorObjectId id{ static_cast<std::uint64_t>(pickedObjectId) };
        if (pickedObjectId != 0 && ctx.document.Find(id) && ctx.scene.FindEditorObject(id.value))
        {
            if (pendingPickToggle_) { ctx.selection.Toggle(id); }
            else { ctx.selection.Replace(id); }
        }
        else if (!pendingPickToggle_)
        {
            ctx.selection.Clear();
        }
        pendingPickToggle_ = false;
    }

    // Right mouse = camera look (LookToggle). While flying, keep drawing active
    // gizmos but disable their mouse handling so they cannot grab the frozen
    // cursor and interrupt camera look.
    const bool flying = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    const bool gizmoVisible = op_ != Op::Select;

    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(0.0f, 0.0f, width, height);
    ImGuizmo::Enable(!flying && gizmoVisible);

    const Camera& camera = ctx.scene.CameraRef();
    float view[16];
    float proj[16];
    ToFloat16(camera.GetViewMatrix(), view);
    ToFloat16(camera.GetProjMatrixNoJitter(), proj);

    // --- Editor icon billboards for world-positioned editor entities ---
    // Screen-space, always-on-top ImGui overlay images from the icon atlas;
    // clickable to select the entity. Directional/camera have no world position,
    // so they get no billboard here.
    if (!iconAtlasTried_)
    {
        iconAtlasTried_ = true;
        ctx.renderer.WaitForPreviousFrame();
        UploadBatch up;
        if (up.Begin(&ctx.renderer))
        {
            Texture2D::CreateDesc desc;
            desc.path = L"textures/editor/editor_icons.png";
            desc.usage = Texture2D::Usage::LinearData;
            iconAtlasReady_ = iconAtlas_.CreateFromFile(&ctx.renderer, up.CommandList(), desc, up.KeepAlive());
            up.SubmitAndWait(&ctx.renderer);
        }
    }

    if (iconAtlasReady_)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = iconAtlas_.GetSrvFormat();
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        ctx.renderer.MarkImGuiTextureShaderReadable(iconAtlas_.GetResource());
        const ImTextureID iconTex = ctx.renderer.CreateImGuiTextureId(iconAtlas_.GetResource(), srvDesc);
        if (iconTex != ImTextureID_Invalid)
        {
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            const Math::mat4& vp = camera.GetViewProjMatrixNoJitter();
            constexpr float kIconHalf = 15.0f;

            auto drawWorldIcon = [&](const Math::float3& pos,
                EditorObjectId id,
                ImVec2 uv0,
                ImVec2 uv1,
                ImU32 tint)
            {
                const DirectX::XMVECTOR clip =
                    DirectX::XMVector4Transform(DirectX::XMVectorSet(pos.x, pos.y, pos.z, 1.0f), vp.xm());
                const float w = DirectX::XMVectorGetW(clip);
                if (w <= 1e-3f) { return; } // behind the camera
                const float ndcX = DirectX::XMVectorGetX(clip) / w;
                const float ndcY = DirectX::XMVectorGetY(clip) / w;
                if (ndcX < -1.5f || ndcX > 1.5f || ndcY < -1.5f || ndcY > 1.5f) { return; }
                const float sx = (ndcX * 0.5f + 0.5f) * width;
                const float sy = (1.0f - (ndcY * 0.5f + 0.5f)) * height;

                const ImVec2 mn(sx - kIconHalf, sy - kIconHalf);
                const ImVec2 mx(sx + kIconHalf, sy + kIconHalf);
                if (ctx.selection.Contains(id))
                {
                    dl->AddRect(ImVec2(mn.x - 2.0f, mn.y - 2.0f), ImVec2(mx.x + 2.0f, mx.y + 2.0f),
                                IM_COL32(255, 200, 40, 255), 3.0f, 0, 2.0f);
                }
                dl->AddImage(iconTex, mn, mx, uv0, uv1, tint);
                iconHits.push_back({ mn, mx, id });
            };

            for (EditorObject& env : ctx.document.Environment())
            {
                // Atlas cells: [dirlight | point] top row, [spot | camera] bottom row.
                ImVec2 uv0, uv1;
                if (env.type == "pointLight")     { uv0 = ImVec2(0.5f, 0.0f); uv1 = ImVec2(1.0f, 0.5f); }
                else if (env.type == "spotLight") { uv0 = ImVec2(0.0f, 0.5f); uv1 = ImVec2(0.5f, 1.0f); }
                else                              { continue; }

                const auto posIt = env.properties.find("position");
                if (posIt == env.properties.end() || !posIt->is_array() || posIt->size() < 3u) { continue; }
                const float px = (*posIt)[0].get<float>();
                const float py = (*posIt)[1].get<float>();
                const float pz = (*posIt)[2].get<float>();

                // Tint by the light color (icons are white; RGB carries the tint).
                ImU32 tint = IM_COL32(255, 255, 255, 255);
                const auto colIt = env.properties.find("color");
                if (colIt != env.properties.end() && colIt->is_array() && colIt->size() >= 3u)
                {
                    auto ch = [&](int i)
                    {
                        float v = (*colIt)[i].get<float>();
                        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                        return static_cast<int>(v * 255.0f + 0.5f);
                    };
                    tint = IM_COL32(ch(0), ch(1), ch(2), 255);
                }

                drawWorldIcon(Math::float3(px, py, pz), env.id, uv0, uv1, tint);
            }

            for (EditorObject& obj : ctx.document.Objects())
            {
                if (obj.type != "freeCameraStart")
                {
                    continue;
                }

                const ImU32 tint = obj.enabled
                    ? IM_COL32(255, 255, 255, 255)
                    : IM_COL32(180, 180, 180, 160);
                drawWorldIcon(
                    obj.transform.position,
                    obj.id,
                    ImVec2(0.5f, 0.5f),
                    ImVec2(1.0f, 1.0f),
                    tint);
            }
        }
    }

    // Wireframe shape for the SELECTED light (point = sphere at radius, spot =
    // cone at range/outer-angle), via the debug-draw system. Independent of the
    // icon atlas and of on-screen culling; the debug-draw pass clips it in 3D.
    // WHERE THE SUN IS AIMED, drawn against the sky so the authored direction can be matched to the
    // sun the HDRI actually contains. The two are independent -- one is a vector in the level, the
    // other is painted into the texture -- and when they disagree the shadows come from a sun that
    // is not the one on screen. Measured in wind_test: the light pointed at uv (0.638, 0.633) while
    // the sun sat at (0.341, 0.501), and nothing in the editor said so.
    const auto DrawDirectionalLightProxy = [](EditorContext& c, EditorObject& env,
                                              const Math::float4& col, DebugDrawSystem& dd)
    {
        Math::float3 dir(0.0f, -1.0f, 0.0f);
        const auto dirIt = env.properties.find("direction");
        if (dirIt != env.properties.end() && dirIt->is_array() && dirIt->size() >= 3u)
        {
            dir = Math::float3((*dirIt)[0].get<float>(), (*dirIt)[1].get<float>(),
                               (*dirIt)[2].get<float>());
        }
        // The authored vector is where the light TRAVELS, so the sun is the other way -- the same
        // negation the shaders apply to sunDirAmbient.
        const Math::float3 toSun = (dir * -1.0f).Normalized();

        // A directional light has no position, so the marker rides with the camera: that keeps it
        // at a fixed place on screen for a given direction, which is what makes it comparable to
        // the sky behind it. Well inside the 10000 far plane.
        const Math::float3 eye = c.scene.CameraRef().GetPosition();
        const float kDistance = 3000.0f;
        // THE SUN'S REAL ANGULAR DIAMETER, 0.53 degrees. Sizing the ring to it turns "close enough?"
        // into a yes/no: the ring either covers the disc or it does not.
        const float kSunHalfAngle = 0.00462f;
        const Math::float3 centre = eye + toSun * kDistance;
        dd.AddSphere(centre, kDistance * kSunHalfAngle, col, /*wireframe=*/true);
        // A wider guide ring, so there is something to steer by while the small one is still off
        // screen or lost in the glare.
        dd.AddSphere(centre, kDistance * kSunHalfAngle * 10.0f,
                     Math::float4(col.x, col.y, col.z, 0.35f), /*wireframe=*/true);
    };

    if (DebugDrawSystem* dd = ctx.renderer.GetDebugDrawSystem())
    {
        for (EditorObject& env : ctx.document.Environment())
        {
            if (ctx.selection.Primary().value != env.id.value) { continue; }

            Math::float4 col(1.0f, 0.85f, 0.25f, 1.0f);
            const auto colIt = env.properties.find("color");
            if (colIt != env.properties.end() && colIt->is_array() && colIt->size() >= 3u)
            {
                col = Math::float4((*colIt)[0].get<float>(), (*colIt)[1].get<float>(), (*colIt)[2].get<float>(), 1.0f);
            }

            // BEFORE the position lookup, because a directional light HAS no position -- it is the
            // one environment type EditorSceneDocument marks that way, and the guard below breaks
            // out of the loop when `position` is missing. Putting the branch after it made it
            // unreachable for the only object it was written for.
            if (env.type == "directionalLight")
            {
                DrawDirectionalLightProxy(ctx, env, col, *dd);
                break;
            }

            const auto posIt = env.properties.find("position");
            if (posIt == env.properties.end() || !posIt->is_array() || posIt->size() < 3u) { break; }
            const Math::float3 pos((*posIt)[0].get<float>(), (*posIt)[1].get<float>(), (*posIt)[2].get<float>());

            if (env.type == "pointLight")
            {
                const float radius = env.properties.value("radius", 1.0f);
                dd->AddSphere(pos, radius, col, /*wireframe=*/true);
            }
            else if (env.type == "spotLight")
            {
                Math::float3 dir(0.0f, -1.0f, 0.0f);
                const auto dirIt = env.properties.find("direction");
                if (dirIt != env.properties.end() && dirIt->is_array() && dirIt->size() >= 3u)
                {
                    dir = Math::float3((*dirIt)[0].get<float>(), (*dirIt)[1].get<float>(), (*dirIt)[2].get<float>());
                }
                dir = dir.Normalized();
                const float range = env.properties.value("range", 10.0f);
                const float outerDeg = env.properties.value("outerAngleDeg", 25.0f);
                const float coneR = range * std::tan(outerDeg * 0.01745329252f); // deg->rad
                dd->AddCone(pos, dir, range, coneR, col, /*wireframe=*/true);
            }
            break;
        }
    }

    bool gizmoBusy = false;

    const auto snapForOperation = [this](ImGuizmo::OPERATION operation, float values[3]) -> const float*
    {
        if (snapEnabled_ == temporarySnapInvertHeld_)
        {
            return nullptr;
        }

        const float increment = operation == ImGuizmo::ROTATE ? rotationIncrement_ :
            operation == ImGuizmo::SCALE ? scaleIncrement_ :
            translationIncrement_;
        values[0] = increment;
        values[1] = increment;
        values[2] = increment;
        return values;
    };

    const auto captureDragSnapshots = [&]()
    {
        dragSnapshots_.clear();
        dragSnapshots_.reserve(ctx.selection.Size());
        for (const EditorObjectId id : ctx.selection.Ordered())
        {
            if (EditorObject* object = ctx.document.Find(id))
            {
                RenderableObjectBase* runtime = ctx.scene.FindEditorObject(id.value);
                RenderableObject* renderable = runtime ? runtime->AsRenderableObject() : nullptr;
                if (!renderable)
                {
                    continue;
                }

                DragSnapshot snapshot;
                snapshot.id = id;
                snapshot.transform = object->transform;
                snapshot.model = renderable->GetModelMatrix();
                dragSnapshots_.push_back(std::move(snapshot));
                continue;
            }

            for (EditorObject& environment : ctx.document.Environment())
            {
                if (environment.id.value != id.value ||
                    (environment.type != "pointLight" && environment.type != "spotLight" &&
                        environment.type != "directionalLight"))
                {
                    continue;
                }

                DragSnapshot snapshot;
                snapshot.id = id;
                snapshot.environment = true;
                snapshot.hasPosition = environment.type != "directionalLight";
                snapshot.hasDirection = environment.type != "pointLight";
                snapshot.properties = environment.properties;
                const Math::float3 pos = snapshot.hasPosition ?
                    ReadFloat3(snapshot.properties, "position", Math::float3(0.0f, 0.0f, 0.0f)) :
                    camera.GetPosition() + camera.GetDirection() * 8.0f;
                const Math::float3 dir = snapshot.hasDirection ?
                    ReadFloat3(snapshot.properties, "direction", Math::float3(0.0f, -1.0f, 0.0f)) :
                    Math::float3(0.0f, -1.0f, 0.0f);
                float lightMatrix[16];
                BuildLightMatrix(pos, dir, lightMatrix);
                snapshot.model = FromFloat16(lightMatrix);
                dragSnapshots_.push_back(std::move(snapshot));
                break;
            }
        }
    };

    const auto applyDragDelta = [&](const Math::mat4& currentPrimaryMatrix)
    {
        const Math::mat4 delta = Math::mat4::Inverse(primaryMatrixBeforeDrag_) * currentPrimaryMatrix;
        for (const DragSnapshot& snapshot : dragSnapshots_)
        {
            if (!snapshot.environment)
            {
                TransformObjectCommand::ApplyTransform(
                    ctx,
                    snapshot.id,
                    TransformFromMatrix(snapshot.model * delta));
                continue;
            }

            for (EditorObject& environment : ctx.document.Environment())
            {
                if (environment.id.value != snapshot.id.value)
                {
                    continue;
                }

                nlohmann::json properties = snapshot.properties;
                if (snapshot.hasPosition)
                {
                    const Math::float3 position = delta.TransformPoint(
                        ReadFloat3(snapshot.properties, "position", Math::float3(0.0f, 0.0f, 0.0f)));
                    properties["position"] = { position.x, position.y, position.z };
                }
                if (snapshot.hasDirection)
                {
                    const Math::float3 direction = delta.TransformDirection(
                        ReadFloat3(snapshot.properties, "direction", Math::float3(0.0f, -1.0f, 0.0f))).Normalized();
                    properties["direction"] = { direction.x, direction.y, direction.z };
                }
                environment.properties = std::move(properties);
                EnvironmentRuntime::Apply(ctx, environment);
                ctx.document.SetDirty(true);
                break;
            }
        }
    };

    const auto commitDrag = [&]()
    {
        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(dragSnapshots_.size());
        for (const DragSnapshot& snapshot : dragSnapshots_)
        {
            if (!snapshot.environment)
            {
                const EditorObject* object = ctx.document.Find(snapshot.id);
                if (object && !TransformMatches(snapshot.transform, object->transform))
                {
                    commands.push_back(std::make_unique<TransformObjectCommand>(
                        snapshot.id, snapshot.transform, object->transform));
                }
                continue;
            }

            for (const EditorObject& environment : ctx.document.Environment())
            {
                if (environment.id.value == snapshot.id.value &&
                    environment.properties != snapshot.properties)
                {
                    commands.push_back(std::make_unique<EditEnvironmentCommand>(
                        snapshot.id,
                        snapshot.properties,
                        environment.properties,
                        "Transform Environment Light"));
                    break;
                }
            }
        }

        if (commands.size() == 1)
        {
            commandStack.Execute(ctx, std::move(commands.front()));
        }
        else if (!commands.empty())
        {
            auto composite = std::make_unique<CompositeCommand>(
                "Transform " + std::to_string(commands.size()) + " Objects");
            for (std::unique_ptr<EditorCommand>& command : commands)
            {
                composite->Add(std::move(command));
            }
            commandStack.Execute(ctx, std::move(composite));
        }
        dragSnapshots_.clear();
        dragPrimary_ = EditorObjectId{};
    };

    const EditorObjectId primary = ctx.selection.Primary();
    EditorObject* primaryObject = ctx.document.Find(primary);
    RenderableObjectBase* primaryRuntime = primaryObject ? ctx.scene.FindEditorObject(primary.value) : nullptr;
    RenderableObject* primaryRenderable = primaryRuntime ? primaryRuntime->AsRenderableObject() : nullptr;
    EditorObject* primaryEnvironment = nullptr;
    if (!primaryObject)
    {
        for (EditorObject& environment : ctx.document.Environment())
        {
            if (environment.id.value == primary.value &&
                (environment.type == "pointLight" || environment.type == "spotLight" ||
                    environment.type == "directionalLight"))
            {
                primaryEnvironment = &environment;
                break;
            }
        }
    }

    bool gizmoHandled = false;
    if (gizmoVisible && (primaryRenderable || primaryEnvironment))
    {
        bool primaryHasPosition = false;
        bool primaryHasDirection = false;
        Math::float3 primaryPosition;
        Math::mat4 primaryModel;
        if (primaryRenderable)
        {
            primaryPosition = primaryRenderable->GetPosition();
            primaryModel = primaryRenderable->GetModelMatrix();
        }
        else
        {
            primaryHasPosition = primaryEnvironment->type != "directionalLight";
            primaryHasDirection = primaryEnvironment->type != "pointLight";
            primaryPosition = primaryHasPosition ?
                ReadFloat3(primaryEnvironment->properties, "position", Math::float3(0.0f, 0.0f, 0.0f)) :
                camera.GetPosition() + camera.GetDirection() * 8.0f;
            const Math::float3 direction = primaryHasDirection ?
                ReadFloat3(primaryEnvironment->properties, "direction", Math::float3(0.0f, -1.0f, 0.0f)) :
                Math::float3(0.0f, -1.0f, 0.0f);
            float lightMatrix[16];
            BuildLightMatrix(primaryPosition, direction, lightMatrix);
            primaryModel = FromFloat16(lightMatrix);
        }

        const Math::float3 toPrimary = primaryPosition - camera.GetPosition();
        const Math::float3 cameraForward = camera.GetDirection();
        const bool inFront =
            (toPrimary.x * cameraForward.x + toPrimary.y * cameraForward.y + toPrimary.z * cameraForward.z) > 0.0f;
        if (inFront)
        {
            float model[16];
            ToFloat16(primaryModel, model);
            ImGuizmo::OPERATION operation = ToImGuizmo(op_);
            if (primaryEnvironment)
            {
                if (operation == ImGuizmo::SCALE) { operation = ImGuizmo::TRANSLATE; }
                if (!primaryHasDirection && operation == ImGuizmo::ROTATE) { operation = ImGuizmo::TRANSLATE; }
                if (!primaryHasPosition && operation == ImGuizmo::TRANSLATE) { operation = ImGuizmo::ROTATE; }
            }

            const ImGuizmo::MODE mode = primaryEnvironment ||
                transformSpace_ == TransformSpace::World ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
            float snapValues[3];
            ImGuizmo::Manipulate(view, proj, operation, mode, model, nullptr,
                snapForOperation(operation, snapValues));
            const bool usingNow = ImGuizmo::IsUsing();
            if (usingNow && !wasUsing_)
            {
                captureDragSnapshots();
                primaryMatrixBeforeDrag_ = primaryModel;
                dragPrimary_ = primary;
            }
            if (usingNow && !dragSnapshots_.empty())
            {
                applyDragDelta(FromFloat16(model));
            }
            if (!usingNow && wasUsing_)
            {
                commitDrag();
            }
            wasUsing_ = usingNow;
            gizmoBusy = usingNow || ImGuizmo::IsOver();
            gizmoHandled = true;
        }
    }
    if (!gizmoHandled && wasUsing_)
    {
        commitDrag();
        wasUsing_ = false;
    }

    // Click-to-select: only over the 3D view, not on the gizmo, not while flying.
    if (flying || io.WantCaptureMouse || gizmoBusy) { return; }
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { return; }

    // Editor icon billboards take click priority over mesh id-buffer picking.
    for (auto it = iconHits.rbegin(); it != iconHits.rend(); ++it)
    {
        if (io.MousePos.x >= it->mn.x && io.MousePos.x <= it->mx.x &&
            io.MousePos.y >= it->mn.y && io.MousePos.y <= it->mx.y)
        {
            if (io.KeyCtrl) { ctx.selection.Toggle(it->id); }
            else { ctx.selection.Replace(it->id); }
            return;
        }
    }

    if (ctx.renderer.RequestObjectIdPick(io.MousePos.x, io.MousePos.y))
    {
        pendingPickToggle_ = io.KeyCtrl;
        return;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    Math::float3 origin;
    Math::float3 dir;
    if (!viewport || !BuildViewportCursorRay(
            camera, io.MousePos, viewport->Pos, width, height, origin, dir))
    {
        return;
    }

    const Scene::SceneObjectId hit = ctx.scene.RaycastEditorObject(origin, dir);
    if (hit != 0)
    {
        const EditorObjectId id{ hit };
        if (io.KeyCtrl) { ctx.selection.Toggle(id); }
        else { ctx.selection.Replace(id); }
    }
    else if (!io.KeyCtrl)
    {
        ctx.selection.Clear();
    }
}

#endif // WITH_EDITOR
