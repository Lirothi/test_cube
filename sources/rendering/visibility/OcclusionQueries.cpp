#include "rendering/visibility/OcclusionQueries.h"

#include <cstring>

#include "core/logging/Log.h"
#include "materials/Material.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/RenderContext.h"
#include "rendering/core/Renderer.h"

namespace vis
{
namespace
{
    // UE GCubeIndices over the corner order BatchPrimitive writes: bit 2 = max x, bit 1 = max y,
    // bit 0 = max z. Winding is irrelevant to a both-faces PSO.
    constexpr std::uint16_t kCubeIndices[36] = {
        0, 2, 3,  0, 3, 1,
        4, 5, 7,  4, 7, 6,
        0, 1, 5,  0, 5, 4,
        2, 6, 7,  2, 7, 3,
        0, 4, 6,  0, 6, 2,
        1, 3, 7,  1, 7, 5,
    };
    constexpr UINT kIndicesPerBox = 36;
    constexpr UINT kVerticesPerBox = 8;
    constexpr UINT kIndexBufferBytes = kOccludedPrimitiveQueryBatchSize * kIndicesPerBox * sizeof(std::uint16_t);
    constexpr UINT kHeapQueries = kMaxOcclusionQueries * kOcclusionBufferedFrames;

    struct QueryCB
    {
        Math::mat4 viewProj;
    };
}

void OcclusionQueryHeap::Reset()
{
    heap_.Reset();
    readback_.Reset();
    indexBuffer_.Reset();
    material_.reset();
    recordedFrame_.fill(0);
    recordedCount_.fill(0);
    failed_ = false;
}

bool OcclusionQueryHeap::EnsureResources(Renderer* renderer)
{
    if (failed_) { return false; }
    if (heap_ && readback_ && indexBuffer_ && material_) { return true; }
    ID3D12Device* device = renderer ? renderer->GetDevice() : nullptr;
    if (!device) { return false; }

    D3D12_QUERY_HEAP_DESC qh{};
    qh.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    qh.Count = kHeapQueries;
    if (FAILED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&heap_))))
    {
        LOG_ERROR(logging::LogCategory::Render, "occlusion queries: CreateQueryHeap({}) failed; method off", kHeapQueries);
        failed_ = true;
        return false;
    }
    heap_->SetName(L"Occlusion.QueryHeap");

    D3D12_HEAP_PROPERTIES hp{};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hp.Type = D3D12_HEAP_TYPE_READBACK;
    rd.Width = static_cast<UINT64>(kHeapQueries) * sizeof(std::uint64_t);
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_))))
    {
        LOG_ERROR(logging::LogCategory::Render, "occlusion queries: readback buffer failed; method off");
        failed_ = true;
        return false;
    }
    readback_->SetName(L"Occlusion.Readback");

    // The 16-box index buffer, written once. An UPLOAD-heap buffer is a valid index buffer
    // (GENERIC_READ covers INDEX_BUFFER) and spares an upload command list.
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    rd.Width = kIndexBufferBytes;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&indexBuffer_))))
    {
        LOG_ERROR(logging::LogCategory::Render, "occlusion queries: index buffer failed; method off");
        failed_ = true;
        return false;
    }
    indexBuffer_->SetName(L"Occlusion.BoxIndices");
    {
        void* mapped = nullptr;
        const D3D12_RANGE noRead{ 0, 0 };
        if (SUCCEEDED(indexBuffer_->Map(0, &noRead, &mapped)) && mapped)
        {
            auto* dst = static_cast<std::uint16_t*>(mapped);
            for (UINT box = 0; box < kOccludedPrimitiveQueryBatchSize; ++box)
            {
                for (UINT i = 0; i < kIndicesPerBox; ++i)
                {
                    dst[box * kIndicesPerBox + i] = static_cast<std::uint16_t>(box * kVerticesPerBox + kCubeIndices[i]);
                }
            }
            indexBuffer_->Unmap(0, nullptr);
        }
    }

    Material::GraphicsDesc gd{};
    gd.shaderFile = L"shaders/occlusion_query.hlsl";
    gd.vsEntry = "VSMain";
    gd.psEntry = "PSMain";
    gd.inputLayoutKey = "PosOnly";
    gd.numRT = 0;
    gd.dsvFormat = render::kDeferredDepthFormat;
    gd.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gd.raster.CullMode = D3D12_CULL_MODE_NONE; // both faces -- see the header
    gd.raster.FillMode = D3D12_FILL_MODE_SOLID;
    gd.raster.DepthClipEnable = TRUE;
    gd.depth.DepthEnable = TRUE;
    gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    gd.depth.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    gd.depth.StencilEnable = FALSE;
    for (int i = 0; i < 8; ++i) { gd.blend.RenderTarget[i].RenderTargetWriteMask = 0; }
    material_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
    if (!material_ || !material_->GetPipelineState())
    {
        LOG_ERROR(logging::LogCategory::Render, "occlusion queries: occlusion_query.hlsl did not build a PSO; method off");
        failed_ = true;
        return false;
    }
    LOG_INFO(logging::LogCategory::Render, "occlusion queries: heap {} queries x {} frames, readback {} KB",
             kMaxOcclusionQueries, kOcclusionBufferedFrames, (kHeapQueries * sizeof(std::uint64_t)) / 1024u);
    return true;
}

UINT OcclusionQueryHeap::SlotOfFrame(std::uint64_t frameNumber) const
{
    for (UINT s = 0; s < kOcclusionBufferedFrames; ++s)
    {
        if (recordedCount_[s] != 0 && recordedFrame_[s] == frameNumber) { return s; }
    }
    return kNoSlot;
}

void OcclusionQueryHeap::Record(Renderer* renderer, ID3D12GraphicsCommandList* cl, const OcclusionQueryPlan& plan, UINT frameSlot)
{
    if (frameSlot >= kOcclusionBufferedFrames) { return; }
    recordedFrame_[frameSlot] = plan.frameNumber;
    recordedCount_[frameSlot] = 0;
    if (!renderer || !cl || plan.batches.empty() || plan.queryCount == 0 || !EnsureResources(renderer)) { return; }

    // Corners of every box of the plan, 8 per box in BatchPrimitive's order.
    const UINT vbBytes = static_cast<UINT>(plan.boxes.size()) * kVerticesPerBox * sizeof(Math::float3);
    auto vb = renderer->GetFrameResource()->AllocDynamic(vbBytes, 16);
    auto cb = renderer->GetFrameResource()->AllocDynamic(sizeof(QueryCB), render::kConstantBufferAlignment);
    if (!vb.cpu || vb.gpu == 0 || !cb.cpu || cb.gpu == 0) { return; }
    {
        auto* v = static_cast<Math::float3*>(vb.cpu);
        for (const OcclusionBox& b : plan.boxes)
        {
            for (UINT c = 0; c < kVerticesPerBox; ++c)
            {
                *v++ = Math::float3((c & 4u) ? b.max.x : b.min.x,
                                    (c & 2u) ? b.max.y : b.min.y,
                                    (c & 1u) ? b.max.z : b.min.z);
            }
        }
        QueryCB k{};
        k.viewProj = plan.viewProj;
        std::memcpy(cb.cpu, &k, sizeof(k));
    }

    auto h = renderer->GetRenderContextPool()->Acquire();
    RenderContext& ctx = h.ref();
    ctx.cbv[0] = cb.gpu;
    if (!material_->Bind(cl, ctx)) { return; }

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = vb.gpu;
    vbv.SizeInBytes = vbBytes;
    vbv.StrideInBytes = sizeof(Math::float3);
    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = indexBuffer_->GetGPUVirtualAddress();
    ibv.SizeInBytes = kIndexBufferBytes;
    ibv.Format = DXGI_FORMAT_R16_UINT;
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->IASetVertexBuffers(0, 1, &vbv);
    cl->IASetIndexBuffer(&ibv);

    const UINT base = frameSlot * kMaxOcclusionQueries;
    for (const OcclusionBatch& batch : plan.batches)
    {
        if (batch.boxCount == 0 || batch.queryIndex >= kMaxOcclusionQueries) { continue; }
        cl->BeginQuery(heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, base + batch.queryIndex);
        cl->DrawIndexedInstanced(kIndicesPerBox * batch.boxCount, 1, 0,
                                 static_cast<INT>(batch.firstBox * kVerticesPerBox), 0);
        cl->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, base + batch.queryIndex);
    }
    cl->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, base, plan.queryCount, readback_.Get(),
                         static_cast<UINT64>(base) * sizeof(std::uint64_t));
    recordedCount_[frameSlot] = plan.queryCount;
}

bool OcclusionQueryHeap::ReadResults(std::uint64_t frameNumber, std::vector<std::uint64_t>& out)
{
    out.clear();
    const UINT slot = SlotOfFrame(frameNumber);
    if (slot == kNoSlot || !readback_) { return false; }
    const UINT count = recordedCount_[slot];
    const UINT64 begin = static_cast<UINT64>(slot) * kMaxOcclusionQueries * sizeof(std::uint64_t);
    const D3D12_RANGE range{ begin, begin + static_cast<UINT64>(count) * sizeof(std::uint64_t) };
    void* mapped = nullptr;
    if (FAILED(readback_->Map(0, &range, &mapped)) || !mapped) { return false; }
    out.resize(count);
    std::memcpy(out.data(), static_cast<const std::uint8_t*>(mapped) + begin, static_cast<size_t>(count) * sizeof(std::uint64_t));
    const D3D12_RANGE noWrite{ 0, 0 };
    readback_->Unmap(0, &noWrite);
    return true;
}
} // namespace vis
