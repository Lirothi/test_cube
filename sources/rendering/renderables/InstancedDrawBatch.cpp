#include "rendering/renderables/InstancedDrawBatch.h"

#include <algorithm>
#include <utility>

#include "rendering/core/Renderer.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/renderables/IInstanceable.h"
#include "rendering/meshes/Mesh.h"
#include "materials/Material.h"
#include "materials/MaterialData.h"

void InstancedDrawBatch::Configure(std::vector<RenderableObjectBase*> members,
                                   Material* gfx, Material* shadow, MaterialData* matData, Mesh* mesh,
                                   bool simple)
{
    members_ = std::move(members);
    gfxMat_ = gfx;
    shadowMat_ = shadow;
    matData_ = matData;
    mesh_ = mesh;
    simple_ = simple;
}

void InstancedDrawBatch::Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Camera& /*camera*/, D3D12_GPU_VIRTUAL_ADDRESS viewCB)
{
    if (!renderer || !cl || !gfxMat_ || !mesh_ || members_.empty()) { return; }
    RecordInstanced(renderer, cl, gfxMat_, viewCB, /*gbuffer=*/true);
}

void InstancedDrawBatch::RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& /*lightView*/, const mat4& /*lightProj*/, D3D12_GPU_VIRTUAL_ADDRESS viewCB)
{
    if (!renderer || !cl || !shadowMat_ || !mesh_ || members_.empty()) { return; }
    RecordInstanced(renderer, cl, shadowMat_, viewCB, /*gbuffer=*/false);
}

void InstancedDrawBatch::RecordInstanced(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                         Material* material, D3D12_GPU_VIRTUAL_ADDRESS viewCB, bool gbuffer)
{
    const size_t total = members_.size();
    const bool wireframe = gbuffer && renderer->GetWireframeMode();

    // Split runs larger than the shader's instance-array capacity into multiple draws.
    for (size_t base = 0; base < total; base += render::kMaxInstancesPerDraw)
    {
        const UINT count = static_cast<UINT>(std::min<size_t>(render::kMaxInstancesPerDraw, total - base));
        const UINT bytes = count * static_cast<UINT>(sizeof(render::InstancePerObject));

        auto alloc = renderer->GetFrameResource()->AllocDynamic(bytes, render::kConstantBufferAlignment);
        auto* dst = static_cast<render::InstancePerObject*>(alloc.cpu);
        for (UINT i = 0; i < count; ++i)
        {
            // Members are guaranteed instanceable by the detection in SceneRenderQueue.
            if (const IInstanceable* inst = members_[base + i]->AsInstanceable())
            {
                inst->FillInstanceData(dst[i]);
            }
        }

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& ctx = h.ref();
        ctx.cbv[0] = alloc.gpu; // b0: per-instance array
        ctx.cbv[1] = viewCB;    // b1: shared per-pass view CB

        if (gbuffer && matData_)
        {
            // Shared material textures (t0..t2) + sampler (s0); instances live in b0, so no
            // t0 conflict with the GpuInstancedModels structured-buffer path.
            matData_->StageGBufferBindings(renderer, ctx, 0, 0);
        }

        material->Bind(cl, ctx, wireframe);
        mesh_->DrawInstanced(cl, count);
    }
}
