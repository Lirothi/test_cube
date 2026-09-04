#include "rendering/visibility/HzbOcclusionTester.h"

#include <cstring>

#include "core/logging/Log.h"
#include "materials/Material.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/RenderContext.h"
#include "rendering/core/RenderContextPool.h"
#include "rendering/core/Renderer.h"

namespace vis
{
namespace
{
    // Mirrors TestBox of vis_test_cs.hlsl.
    struct TestBox
    {
        float center[4];
        float extent[4];
    };
    constexpr UINT kBoxBytes = sizeof(TestBox);
    constexpr UINT kBoxRegionBytes = kMaxHzbTests * kBoxBytes;
    constexpr UINT kResultBytes = sizeof(std::uint32_t);
    constexpr UINT kResultRegionBytes = kMaxHzbTests * kResultBytes;
    constexpr UINT kThreadsPerGroup = 64; // numthreads(64, 1, 1)

    D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags)
    {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        return rd;
    }
}

void HzbOcclusionTester::Reset()
{
    if (boxes_ && boxesMapped_) { boxes_->Unmap(0, nullptr); }
    boxesMapped_ = nullptr;
    boxes_.Reset();
    results_.Reset();
    readback_.Reset();
    descHeap_.Reset();
    boxesSrv_.fill({});
    resultsUav_ = {};
    material_.reset();
    recordedFrame_.fill(0);
    recordedCount_.fill(0);
    pendingCount_.fill(0);
    failed_ = false;
}

bool HzbOcclusionTester::EnsureResources(Renderer* renderer)
{
    if (failed_) { return false; }
    if (Ready()) { return true; }
    ID3D12Device* device = renderer ? renderer->GetDevice() : nullptr;
    if (!device) { return false; }

    const auto fail = [&](const char* what)
    {
        LOG_ERROR(logging::LogCategory::Render, "hzb occlusion tester: {} failed; method off", what);
        failed_ = true;
        return false;
    };

    D3D12_HEAP_PROPERTIES hp{};

    // The boxes: one region per frame in flight, written by the CPU while the GPU reads the
    // older ones (the pattern of the frame upload buffer, but with a per-region SRV).
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    {
        const D3D12_RESOURCE_DESC rd = BufferDesc(static_cast<UINT64>(kBoxRegionBytes) * kOcclusionBufferedFrames, D3D12_RESOURCE_FLAG_NONE);
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&boxes_))))
        {
            return fail("box ring");
        }
        boxes_->SetName(L"VisTest.Boxes");
        void* mapped = nullptr;
        const D3D12_RANGE noRead{ 0, 0 };
        if (FAILED(boxes_->Map(0, &noRead, &mapped)) || !mapped) { return fail("box ring map"); }
        boxesMapped_ = static_cast<std::uint8_t*>(mapped);
    }

    // The verdicts: written by the dispatch, copied out in the same pass, so one region does.
    // Buffers are created in COMMON whatever is asked; the declared canonical state is where the
    // frame LEAVES it -- COPY_SOURCE after the readback copy (barrier plan step 6b).
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    {
        const D3D12_RESOURCE_DESC rd = BufferDesc(kResultRegionBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer))))
        {
            return fail("results buffer");
        }
        results_.Attach(renderer->Declarations(), buffer, D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_COPY_SOURCE, L"VisTest.Results");
    }

    hp.Type = D3D12_HEAP_TYPE_READBACK;
    {
        const D3D12_RESOURCE_DESC rd = BufferDesc(static_cast<UINT64>(kResultRegionBytes) * kOcclusionBufferedFrames, D3D12_RESOURCE_FLAG_NONE);
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_))))
        {
            return fail("readback buffer");
        }
        readback_->SetName(L"VisTest.Readback");
    }

    // Descriptors: a box SRV per region and the results UAV, staged into the shader-visible heap
    // per dispatch (Renderer::StageSrvUavTable) like every other pass's tables.
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kOcclusionBufferedFrames + 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descHeap_)))) { return fail("descriptor heap"); }
        const UINT incr = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_CPU_DESCRIPTOR_HANDLE base = descHeap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT f = 0; f < kOcclusionBufferedFrames; ++f)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_UNKNOWN;
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Buffer.FirstElement = static_cast<UINT64>(f) * kMaxHzbTests;
            sd.Buffer.NumElements = kMaxHzbTests;
            sd.Buffer.StructureByteStride = kBoxBytes;
            const D3D12_CPU_DESCRIPTOR_HANDLE h{ base.ptr + static_cast<SIZE_T>(f) * incr };
            device->CreateShaderResourceView(boxes_.Get(), &sd, h);
            boxesSrv_[f] = h;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = 0;
        ud.Buffer.NumElements = kMaxHzbTests;
        ud.Buffer.StructureByteStride = kResultBytes;
        resultsUav_ = D3D12_CPU_DESCRIPTOR_HANDLE{ base.ptr + static_cast<SIZE_T>(kOcclusionBufferedFrames) * incr };
        device->CreateUnorderedAccessView(results_.Get(), nullptr, &ud, resultsUav_);
    }

    Material::ComputeDesc cd{};
    cd.shaderFile = L"shaders/vis_test_cs.hlsl";
    cd.csEntry = "CSMain";
    material_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
    if (!material_ || !material_->GetPipelineState()) { return fail("vis_test_cs.hlsl PSO"); }

    LOG_INFO(logging::LogCategory::Render, "hzb occlusion tester: {} boxes x {} frames, box ring {} KB, readback {} KB",
             kMaxHzbTests, kOcclusionBufferedFrames,
             (static_cast<UINT64>(kBoxRegionBytes) * kOcclusionBufferedFrames) / 1024u,
             (static_cast<UINT64>(kResultRegionBytes) * kOcclusionBufferedFrames) / 1024u);
    return true;
}

UINT HzbOcclusionTester::SlotOfFrame(std::uint64_t frameNumber) const
{
    for (UINT s = 0; s < kOcclusionBufferedFrames; ++s)
    {
        if (recordedCount_[s] != 0 && recordedFrame_[s] == frameNumber) { return s; }
    }
    return kNoSlot;
}

void HzbOcclusionTester::RecordTest(Renderer* renderer, ID3D12GraphicsCommandList* cl, const OcclusionQueryPlan& plan,
                                    UINT frameSlot, UINT viewWidth, UINT viewHeight,
                                    D3D12_CPU_DESCRIPTOR_HANDLE hzbSrv, UINT hzbWidth, UINT hzbHeight)
{
    if (frameSlot >= kOcclusionBufferedFrames) { return; }
    recordedFrame_[frameSlot] = plan.frameNumber;
    recordedCount_[frameSlot] = 0;
    pendingCount_[frameSlot] = 0;
    const UINT count = static_cast<UINT>(std::min<size_t>(plan.boxes.size(), kMaxHzbTests));
    if (!renderer || !cl || count == 0 || !Ready() || hzbSrv.ptr == 0) { return; }

    // This frame's boxes into this slot's region: min/max -> centre/extent, the shape the
    // library's BoxCullFrustum takes (and the self-test feeds).
    {
        auto* dst = reinterpret_cast<TestBox*>(boxesMapped_ + static_cast<size_t>(frameSlot) * kBoxRegionBytes);
        for (UINT i = 0; i < count; ++i)
        {
            const OcclusionBox& b = plan.boxes[i];
            const Math::float3 c = (b.min + b.max) * 0.5f;
            const Math::float3 e = (b.max - b.min) * 0.5f;
            dst[i].center[0] = c.x; dst[i].center[1] = c.y; dst[i].center[2] = c.z; dst[i].center[3] = 0.0f;
            dst[i].extent[0] = e.x; dst[i].extent[1] = e.y; dst[i].extent[2] = e.z; dst[i].extent[3] = 1.0f;
        }
    }

    auto cb = renderer->GetFrameResource()->AllocDynamic(sizeof(HzbTestConstants), render::kConstantBufferAlignment);
    if (!cb.cpu || cb.gpu == 0) { return; }
    {
        HzbTestConstants k{};
        k.worldToClip = plan.viewProj;
        k.viewToClip = plan.viewToClip;
        k.viewRect[0] = 0;
        k.viewRect[1] = 0;
        k.viewRect[2] = static_cast<std::int32_t>(viewWidth);
        k.viewRect[3] = static_cast<std::int32_t>(viewHeight);
        k.hzbWidth = hzbWidth;
        k.hzbHeight = hzbHeight;
        k.boxCount = count;
        std::memcpy(cb.cpu, &k, sizeof(k));
    }

    auto h = renderer->GetRenderContextPool()->Acquire();
    RenderContext& ctx = h.ref();
    ctx.cbv[0] = cb.gpu;
    ctx.srvTable[0] = renderer->StageSrvUavTable({ boxesSrv_[frameSlot], hzbSrv }).gpu;
    ctx.uavTable[0] = renderer->StageSrvUavTable({ resultsUav_ }).gpu;
    if (!material_->Bind(cl, ctx)) { return; }
    cl->Dispatch((count + kThreadsPerGroup - 1u) / kThreadsPerGroup, 1, 1);
    pendingCount_[frameSlot] = count;
}

void HzbOcclusionTester::RecordReadback(ID3D12GraphicsCommandList* cl, UINT frameSlot)
{
    if (frameSlot >= kOcclusionBufferedFrames || !cl || !readback_ || !results_) { return; }
    const UINT count = pendingCount_[frameSlot];
    pendingCount_[frameSlot] = 0;
    if (count == 0) { return; }
    cl->CopyBufferRegion(readback_.Get(), static_cast<UINT64>(frameSlot) * kResultRegionBytes,
                         results_.Get(), 0, static_cast<UINT64>(count) * kResultBytes);
    recordedCount_[frameSlot] = count;
}

bool HzbOcclusionTester::ReadResults(std::uint64_t frameNumber, std::vector<std::uint32_t>& out)
{
    out.clear();
    const UINT slot = SlotOfFrame(frameNumber);
    if (slot == kNoSlot || !readback_) { return false; }
    const UINT count = recordedCount_[slot];
    const UINT64 begin = static_cast<UINT64>(slot) * kResultRegionBytes;
    const D3D12_RANGE range{ begin, begin + static_cast<UINT64>(count) * kResultBytes };
    void* mapped = nullptr;
    if (FAILED(readback_->Map(0, &range, &mapped)) || !mapped) { return false; }
    out.resize(count);
    std::memcpy(out.data(), static_cast<const std::uint8_t*>(mapped) + begin, static_cast<size_t>(count) * kResultBytes);
    const D3D12_RANGE noWrite{ 0, 0 };
    readback_->Unmap(0, &noWrite);
    return true;
}
} // namespace vis
