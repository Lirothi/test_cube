#include "editor/ui/ViewportGizmo.h"
#if WITH_EDITOR

#include <cstring>

#include <DirectXMath.h>

#include "app/camera/Camera.h"
#include "app/scene/Scene.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/TransformObjectCommand.h"
#include "rendering/core/Renderer.h"
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

    bool gizmoBusy = false;

    // Gizmo for the selected object (only if it has a live editor-owned runtime).
    EditorObject* obj = ctx.document.Find(ctx.selectedObject);
    RenderableObjectBase* base = obj ? ctx.scene.FindEditorObject(ctx.selectedObject.value) : nullptr;
    RenderableObject* ro = base ? base->AsRenderableObject() : nullptr;
    if (obj && ro)
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
