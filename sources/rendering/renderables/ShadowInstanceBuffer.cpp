#include "rendering/renderables/ShadowInstanceBuffer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "rendering/core/Renderer.h"
#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/IInstanceable.h"

bool ShadowInstanceBuffer::IsCaster(const RenderableObjectBase* obj)
{
    // Mirror the CastsShadow() filter shadowCasterSource_.Bucketize uses. NOT
    // filtered by IsVisible()/layer mask: the caster id must be STABLE across
    // frames, and per-view visibility is the job of the future GPU cull pass
    // (Step 4), not of this persistent buffer. AsRenderableObject() guards that
    // the caster has a CPU model matrix to marshal.
    return obj && obj->CastsShadow() && obj->AsRenderableObject() != nullptr;
}

void ShadowInstanceBuffer::FillEntry(const RenderableObjectBase* obj, render::InstancePerObject& out)
{
    std::memset(&out, 0, sizeof(out));
    if (!obj)
    {
        return;
    }

    // Instanceable casters (default gbuffer objects) carry the full per-instance
    // payload; use it so the entry matches the CPU-marshalled path byte-for-byte.
    if (const IInstanceable* inst = obj->AsInstanceable())
    {
        inst->FillInstanceData(out);
        return;
    }

    // Other casters (e.g. glass, meshes without an instanced variant): shadows are
    // depth-only, so only world/prevWorld matter. Material fields stay zero.
    if (const RenderableObject* ro = obj->AsRenderableObject())
    {
        out.world = ro->GetModelMatrix().m;
        out.prevWorld = ro->GetPreviousModelMatrix().m;
        const std::uint64_t id = obj->GetEditorObjectId();
        out.objectId = id > 0xffffffffull ? 0xffffffffu : static_cast<std::uint32_t>(id);
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE ShadowInstanceBuffer::Srv(UINT frameIndex) const
{
    if (frameIndex >= render::kFrameCount)
    {
        return {};
    }
    return srvHandles_[frameIndex];
}

bool ShadowInstanceBuffer::EnsureCapacity(Renderer* renderer, size_t requiredCasters)
{
    if (!renderer || !renderer->GetDevice())
    {
        return false;
    }

    if (buffer_ && mapped_ && srvHandles_[0].ptr != 0 && capacity_ >= requiredCasters)
    {
        return true; // existing allocation is large enough — reuse (no realloc)
    }

    // Release the old allocation. Only ever runs on GROWTH, which in Step 1 happens
    // at level load (this buffer is not yet bound by any pass, so there is no
    // in-flight reader to strand — cf. the LightManager use-after-free lesson).
    if (buffer_)
    {
        renderer->ClearResourceState(buffer_.Get());
        buffer_->Unmap(0, nullptr);
        buffer_.Reset();
    }
    mapped_ = nullptr;
    capacity_ = 0;
    srvHeap_.Reset();
    srvHandles_.fill({});

    const size_t newCapacity = std::max<size_t>(requiredCasters, 1);
    // Ring buffer: render::kFrameCount contiguous regions of newCapacity elements.
    const size_t totalElements = newCapacity * render::kFrameCount;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(totalElements * sizeof(render::InstancePerObject));
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    HRESULT hr = renderer->GetDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(buffer.GetAddressOf()));
    if (FAILED(hr) || !buffer)
    {
        return false;
    }

    D3D12_RANGE range{ 0, 0 };
    void* mapped = nullptr;
    hr = buffer->Map(0, &range, &mapped);
    if (FAILED(hr) || !mapped)
    {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = render::kFrameCount; // one SRV per ring region
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
    hr = renderer->GetDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(srvHeap.GetAddressOf()));
    if (FAILED(hr) || !srvHeap)
    {
        buffer->Unmap(0, nullptr);
        return false;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE srvBase = srvHeap->GetCPUDescriptorHandleForHeapStart();
    if (srvBase.ptr == 0)
    {
        buffer->Unmap(0, nullptr);
        return false;
    }
    const UINT srvIncr = renderer->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = static_cast<UINT64>(f) * newCapacity; // region f
        srvDesc.Buffer.NumElements = static_cast<UINT>(newCapacity);
        srvDesc.Buffer.StructureByteStride = sizeof(render::InstancePerObject);
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        D3D12_CPU_DESCRIPTOR_HANDLE h{ srvBase.ptr + static_cast<SIZE_T>(f) * srvIncr };
        renderer->GetDevice()->CreateShaderResourceView(buffer.Get(), &srvDesc, h);
        srvHandles_[f] = h;
    }

    renderer->SetResourceState(buffer.Get(), D3D12_RESOURCE_STATE_GENERIC_READ);
    buffer->SetName(L"ShadowInstanceBuffer");

    capacity_ = newCapacity;
    buffer_ = buffer;
    mapped_ = static_cast<render::InstancePerObject*>(mapped);
    srvHeap_ = srvHeap;
    return true;
}

void ShadowInstanceBuffer::Rebuild(Renderer* renderer,
    const std::vector<std::unique_ptr<RenderableObjectBase>>& objects)
{
    if (!renderer)
    {
        return;
    }

    size_t casterCount = 0;
    for (const auto& obj : objects)
    {
        if (IsCaster(obj.get())) { ++casterCount; }
    }

    if (!EnsureCapacity(renderer, casterCount))
    {
        count_ = 0;
        cpuData_.clear();
        pending_.clear();
        return;
    }

    count_ = static_cast<std::uint32_t>(casterCount);
    cpuData_.assign(casterCount, render::InstancePerObject{});
    pending_.assign(casterCount, 0);

    size_t idx = 0;
    for (const auto& objPtr : objects)
    {
        const RenderableObjectBase* obj = objPtr.get();
        if (!IsCaster(obj)) { continue; }
        FillEntry(obj, cpuData_[idx]);
        ++idx;
    }

    // Prime ALL ring regions with the current data — after this a static scene
    // re-uploads nothing.
    if (mapped_ && casterCount > 0)
    {
        for (UINT f = 0; f < render::kFrameCount; ++f)
        {
            render::InstancePerObject* regionBase = mapped_ + static_cast<size_t>(f) * capacity_;
            std::memcpy(regionBase, cpuData_.data(), casterCount * sizeof(render::InstancePerObject));
        }
    }

    char buf[192];
    std::snprintf(buf, sizeof(buf),
        "[ShadowInstanceBuffer] rebuilt: %u casters, capacity %zu, %.2f KB x%u regions.\n",
        count_, capacity_,
        (capacity_ * sizeof(render::InstancePerObject)) / 1024.0, render::kFrameCount);
    OutputDebugStringA(buf);
    logFramesRemaining_ = 5;
}

std::uint32_t ShadowInstanceBuffer::UpdateForFrame(Renderer* renderer,
    const std::vector<std::unique_ptr<RenderableObjectBase>>& objects)
{
    if (!renderer)
    {
        return 0;
    }

    size_t newCount = 0;
    for (const auto& obj : objects)
    {
        if (IsCaster(obj.get())) { ++newCount; }
    }

    // First use, or the caster set changed (level switch / editor spawn-delete) →
    // full rebuild. Safe in Step 1: the buffer is unused, so a reallocation strands
    // no in-flight reader.
    if (!mapped_ || newCount != count_ || newCount > capacity_)
    {
        Rebuild(renderer, objects);
        return count_; // a full rebuild re-uploaded every entry
    }

    const UINT region = renderer->GetCurrentFrameIndex();
    if (region >= render::kFrameCount)
    {
        return 0;
    }
    render::InstancePerObject* regionBase = mapped_ + static_cast<size_t>(region) * capacity_;

    std::uint32_t idx = 0;
    std::uint32_t uploaded = 0;
    for (const auto& objPtr : objects)
    {
        const RenderableObjectBase* obj = objPtr.get();
        if (!IsCaster(obj)) { continue; }

        render::InstancePerObject entry;
        FillEntry(obj, entry);
        if (std::memcmp(&entry, &cpuData_[idx], sizeof(render::InstancePerObject)) != 0)
        {
            // Changed: refresh the authoritative value and schedule propagation into
            // every ring region (this frame's region + the next kFrameCount-1).
            cpuData_[idx] = entry;
            pending_[idx] = static_cast<std::uint8_t>(render::kFrameCount);
        }
        if (pending_[idx] > 0)
        {
            regionBase[idx] = cpuData_[idx];
            --pending_[idx];
            ++uploaded;
        }
        ++idx;
    }

    if (logFramesRemaining_ > 0)
    {
        --logFramesRemaining_;
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "[ShadowInstanceBuffer] frame update: %u/%u entries re-uploaded.\n",
            uploaded, count_);
        OutputDebugStringA(buf);
    }

    return uploaded;
}

void ShadowInstanceBuffer::Reset()
{
    // Retain the GPU buffer + SRV descriptors across a level unload (the
    // LightManager lesson: a pass may reference the SRV while frames are in
    // flight). Only the CPU-side state is dropped; the next Rebuild reuses the
    // allocation when its capacity still suffices.
    count_ = 0;
    cpuData_.clear();
    pending_.clear();
    logFramesRemaining_ = 5;
}
