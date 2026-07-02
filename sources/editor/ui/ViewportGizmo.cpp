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
        case ViewportGizmo::Op::Rotate: return ImGuizmo::ROTATE;
        case ViewportGizmo::Op::Scale:  return ImGuizmo::SCALE;
        default:                        return ImGuizmo::TRANSLATE;
        }
    }
}

void ViewportGizmo::DrawModeButtons()
{
    int mode = static_cast<int>(op_);
    ImGui::TextUnformatted("Gizmo:");
    ImGui::SameLine();
    ImGui::RadioButton("Translate", &mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Rotate", &mode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Scale", &mode, 2);
    op_ = static_cast<Op>(mode);
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

    // Right mouse = camera look (LookToggle). While flying, keep DRAWING the gizmo
    // but disable its mouse handling (ImGuizmo::Enable(false) still renders it,
    // grayed, but skips hit-testing) so it can't grab the frozen cursor and set
    // WantCaptureMouse, which would interrupt the camera look.
    const bool flying = ImGui::IsMouseDown(ImGuiMouseButton_Right);

    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(0.0f, 0.0f, width, height);
    ImGuizmo::Enable(!flying);

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

    if (obj && ro && inFront)
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
