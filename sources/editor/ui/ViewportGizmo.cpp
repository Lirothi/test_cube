#include "editor/ui/ViewportGizmo.h"
#if WITH_EDITOR

#include <cstdint>
#include <cstring>
#include <vector>
#include <cmath>

#include <DirectXMath.h>

#include "app/camera/Camera.h"
#include "app/scene/Scene.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/scene/EnvironmentRuntime.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/TransformObjectCommand.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/debug/DebugDraw.h"
#include "rendering/renderables/RenderableObject.h"
#include "imgui.h"
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
}

void ViewportGizmo::Update(EditorContext& ctx, EditorCommandStack& commandStack)
{
    ImGuiIO& io = ImGui::GetIO();

    const float width = static_cast<float>(ctx.renderer.GetWidth());
    const float height = static_cast<float>(ctx.renderer.GetHeight());
    if (width <= 0.0f || height <= 0.0f) { return; }

    struct IconHit { ImVec2 mn; ImVec2 mx; EditorObjectId id; };
    std::vector<IconHit> iconHits;

    uint32_t pickedObjectId = 0;
    if (ctx.renderer.ConsumeObjectIdPick(pickedObjectId) && pickedObjectId != 0)
    {
        EditorObjectId id{ static_cast<std::uint64_t>(pickedObjectId) };
        if (ctx.document.Find(id) && ctx.scene.FindEditorObject(id.value))
        {
            ctx.selectedObject = id;
        }
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

    // --- Editor icon billboards for world-positioned environment entities ---
    // (point / spot lights). Screen-space, always-on-top ImGui overlay images from
    // the icon atlas; clickable to select the entity. Directional/camera have no
    // world position, so they get no billboard here.
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
        const ImTextureID iconTex = ctx.renderer.CreateImGuiTextureId(iconAtlas_.GetResource(), srvDesc);
        if (iconTex != ImTextureID_Invalid)
        {
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            const Math::mat4& vp = camera.GetViewProjMatrixNoJitter();
            constexpr float kIconHalf = 15.0f;
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

                const DirectX::XMVECTOR clip =
                    DirectX::XMVector4Transform(DirectX::XMVectorSet(px, py, pz, 1.0f), vp.xm());
                const float w = DirectX::XMVectorGetW(clip);
                if (w <= 1e-3f) { continue; } // behind the camera
                const float ndcX = DirectX::XMVectorGetX(clip) / w;
                const float ndcY = DirectX::XMVectorGetY(clip) / w;
                if (ndcX < -1.5f || ndcX > 1.5f || ndcY < -1.5f || ndcY > 1.5f) { continue; }
                const float sx = (ndcX * 0.5f + 0.5f) * width;
                const float sy = (1.0f - (ndcY * 0.5f + 0.5f)) * height;

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

                const ImVec2 mn(sx - kIconHalf, sy - kIconHalf);
                const ImVec2 mx(sx + kIconHalf, sy + kIconHalf);
                if (ctx.selectedObject.value == env.id.value)
                {
                    dl->AddRect(ImVec2(mn.x - 2.0f, mn.y - 2.0f), ImVec2(mx.x + 2.0f, mx.y + 2.0f),
                                IM_COL32(255, 200, 40, 255), 3.0f, 0, 2.0f);
                }
                dl->AddImage(iconTex, mn, mx, uv0, uv1, tint);
                iconHits.push_back({ mn, mx, env.id });
            }
        }
    }

    // Wireframe shape for the SELECTED light (point = sphere at radius, spot =
    // cone at range/outer-angle), via the debug-draw system. Independent of the
    // icon atlas and of on-screen culling; the debug-draw pass clips it in 3D.
    if (DebugDrawSystem* dd = ctx.renderer.GetDebugDrawSystem())
    {
        for (EditorObject& env : ctx.document.Environment())
        {
            if (ctx.selectedObject.value != env.id.value) { continue; }

            const auto posIt = env.properties.find("position");
            if (posIt == env.properties.end() || !posIt->is_array() || posIt->size() < 3u) { break; }
            const Math::float3 pos((*posIt)[0].get<float>(), (*posIt)[1].get<float>(), (*posIt)[2].get<float>());

            Math::float4 col(1.0f, 0.85f, 0.25f, 1.0f);
            const auto colIt = env.properties.find("color");
            if (colIt != env.properties.end() && colIt->is_array() && colIt->size() >= 3u)
            {
                col = Math::float4((*colIt)[0].get<float>(), (*colIt)[1].get<float>(), (*colIt)[2].get<float>(), 1.0f);
            }

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

    // Gizmo for the selected object (only if it has a live editor-owned runtime).
    EditorObject* obj = ctx.document.Find(ctx.selectedObject);
    RenderableObjectBase* base = obj ? ctx.scene.FindEditorObject(ctx.selectedObject.value) : nullptr;
    RenderableObject* ro = base ? base->AsRenderableObject() : nullptr;

    // Only when the object is in front of the camera. ImGuizmo's own behind-camera
    // cull misfires under our reverse-Z projection (the gizmo would appear mirrored
    // in front when you look away), so gate on the camera-forward dot instead.
    bool inFront = false;
    if (obj && ro)
    {
        const Math::float3 forward = camera.GetDirection();
        const Math::float3 toObj = ro->GetPosition() - camera.GetPosition();
        inFront = (toObj.x * forward.x + toObj.y * forward.y + toObj.z * forward.z) > 0.0f;
    }

    if (gizmoVisible && obj && ro && inFront)
    {
        float model[16];
        ToFloat16(ro->GetModelMatrix(), model);

        ImGuizmo::Manipulate(view, proj, ToImGuizmo(op_), ImGuizmo::WORLD, model);

        const bool usingNow = ImGuizmo::IsUsing();
        if (usingNow && !wasUsing_)
        {
            transformBeforeDrag_ = obj->transform; // drag start
        }
        if (usingNow)
        {
            float t[3], r[3], s[3];
            ImGuizmo::DecomposeMatrixToComponents(model, t, r, s);
            EditorTransform nt;
            nt.position = Math::float3(t[0], t[1], t[2]);
            nt.rotationDeg = Math::float3(r[0], r[1], r[2]);
            nt.scale = Math::float3(s[0], s[1], s[2]);
            TransformObjectCommand::ApplyTransform(ctx, ctx.selectedObject, nt); // live
        }
        if (!usingNow && wasUsing_)
        {
            // Drag finished: record one undo entry for the whole gesture.
            commandStack.Execute(ctx, std::make_unique<TransformObjectCommand>(
                ctx.selectedObject, transformBeforeDrag_, obj->transform));
        }
        wasUsing_ = usingNow;
        gizmoBusy = usingNow || ImGuizmo::IsOver();
    }
    else
    {
        wasUsing_ = false;
    }

    // Env light gizmo: translate point/spot position, rotate spot/directional
    // direction. Env entities have no RenderableObject, so drive a synthetic matrix
    // (persisted across the drag) and write the result back to the entity's
    // properties, patching the live runtime via the shared helper. Non-undoable
    // (like the env inspector edits).
    if (gizmoVisible && !obj)
    {
        EditorObject* light = nullptr;
        for (EditorObject& e : ctx.document.Environment())
        {
            if (e.id.value == ctx.selectedObject.value &&
                (e.type == "pointLight" || e.type == "spotLight" || e.type == "directionalLight"))
            {
                light = &e;
                break;
            }
        }

        if (light)
        {
            const bool hasPos = (light->type != "directionalLight");
            const bool hasDir = (light->type != "pointLight");
            const Math::float3 camPos = camera.GetPosition();
            const Math::float3 camFwd = camera.GetDirection();
            const Math::float3 pos = hasPos
                ? ReadFloat3(light->properties, "position", Math::float3(0.0f, 0.0f, 0.0f))
                : camPos + camFwd * 8.0f; // directional has no position: anchor in front of the camera
            const Math::float3 dir = hasDir
                ? ReadFloat3(light->properties, "direction", Math::float3(0.0f, -1.0f, 0.0f))
                : Math::float3(0.0f, -1.0f, 0.0f);

            const Math::float3 toLight = pos - camPos;
            const bool inFrontLight =
                (toLight.x * camFwd.x + toLight.y * camFwd.y + toLight.z * camFwd.z) > 0.0f;

            if (inFrontLight)
            {
                if (!ImGuizmo::IsUsing()) { BuildLightMatrix(pos, dir, envGizmoMatrix_); }

                ImGuizmo::OPERATION giz = ToImGuizmo(op_);
                if (giz == ImGuizmo::SCALE) { giz = ImGuizmo::TRANSLATE; }
                if (!hasDir && giz == ImGuizmo::ROTATE) { giz = ImGuizmo::TRANSLATE; }
                if (!hasPos && giz == ImGuizmo::TRANSLATE) { giz = ImGuizmo::ROTATE; }

                ImGuizmo::Manipulate(view, proj, giz, ImGuizmo::WORLD, envGizmoMatrix_);
                if (ImGuizmo::IsUsing())
                {
                    if (hasPos)
                    {
                        light->properties["position"] = { envGizmoMatrix_[12], envGizmoMatrix_[13], envGizmoMatrix_[14] };
                    }
                    if (hasDir)
                    {
                        const Math::float3 nd =
                            Math::float3(envGizmoMatrix_[8], envGizmoMatrix_[9], envGizmoMatrix_[10]).Normalized();
                        light->properties["direction"] = { nd.x, nd.y, nd.z };
                    }
                    EnvironmentRuntime::Apply(ctx, *light);
                    ctx.document.SetDirty(true);
                }
                gizmoBusy = gizmoBusy || ImGuizmo::IsUsing() || ImGuizmo::IsOver();
            }
        }
    }

    // Click-to-select: only over the 3D view, not on the gizmo, not while flying.
    if (flying || io.WantCaptureMouse || gizmoBusy) { return; }
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { return; }

    // Editor icon billboards (lights) take click priority over mesh id-buffer picking.
    for (auto it = iconHits.rbegin(); it != iconHits.rend(); ++it)
    {
        if (io.MousePos.x >= it->mn.x && io.MousePos.x <= it->mx.x &&
            io.MousePos.y >= it->mn.y && io.MousePos.y <= it->mx.y)
        {
            ctx.selectedObject = it->id;
            return;
        }
    }

    if (ctx.renderer.RequestObjectIdPick(io.MousePos.x, io.MousePos.y))
    {
        return;
    }

    const float ndcX = 2.0f * (io.MousePos.x / width) - 1.0f;
    const float ndcY = 1.0f - 2.0f * (io.MousePos.y / height);
    const Math::float3 origin = camera.GetPosition();
    const Math::float3 viewPt = camera.GetInvProjMatrixNoJitter().TransformPoint(Math::float3(ndcX, ndcY, 1.0f));
    const Math::float3 worldPt = camera.GetInvViewMatrix().TransformPoint(viewPt);
    const Math::float3 dir = (worldPt - origin).Normalized();

    const Scene::SceneObjectId hit = ctx.scene.RaycastEditorObject(origin, dir);
    if (hit != 0)
    {
        ctx.selectedObject = EditorObjectId{ hit };
    }
}

#endif // WITH_EDITOR
