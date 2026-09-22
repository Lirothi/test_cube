#include "rendering/rt/BindlessTable.h"
#include "core/logging/Log.h"

#include "rendering/meshes/Mesh.h"

#include <algorithm>
#include <cstring>

namespace rt {

size_t BindlessTable::KeyHash::operator()(const GeometryKey& key) const
{
    uint64_t h = 1469598103934665603ull;
    h = (h ^ reinterpret_cast<uintptr_t>(key.owner)) * 1099511628211ull;
    h = (h ^ reinterpret_cast<uintptr_t>(key.mesh)) * 1099511628211ull;
    return static_cast<size_t>(h);
}

size_t BindlessTable::KeyHash::operator()(const DescriptorKey& key) const
{
    uint64_t h = 1469598103934665603ull;
    h = (h ^ reinterpret_cast<uintptr_t>(key.mesh)) * 1099511628211ull;
    h = (h ^ key.albedo) * 1099511628211ull;
    h = (h ^ key.mr) * 1099511628211ull;
    h = (h ^ key.albedoGen) * 1099511628211ull;
    h = (h ^ key.mrGen) * 1099511628211ull;
    return static_cast<size_t>(h);
}

size_t BindlessTable::KeyHash::operator()(const MaterialKey& key) const
{
    uint64_t h = 1469598103934665603ull;
    h = (h ^ reinterpret_cast<uintptr_t>(key.mesh)) * 1099511628211ull;
    h = (h ^ key.albedo) * 1099511628211ull;
    h = (h ^ key.mr) * 1099511628211ull;
    return static_cast<size_t>(h);
}

void BindlessTable::BeginFrame(uint64_t frameNo)
{
    frameNo_ = frameNo;
    for (const RetiredSet& r : retiredSets_) { if (r.frame <= frameNo) { freeSets_.push_back(r.slot); } }
    std::erase_if(retiredSets_, [frameNo](const RetiredSet& r) { return r.frame <= frameNo; });
}

void BindlessTable::Init(ID3D12Device* device, render::BindlessHeap* shared)
{
    device_ = device;
    if (!device_ || Ready()) {
        return;
    }
    static_assert(kMaxDescriptors == render::BindlessHeap::kRtRegionCapacity,
                  "the RT region of the shared heap is sized for this table");
    if (shared && shared->Ready()) {
        shared_ = shared;
        base_ = render::BindlessHeap::kRtRegionBase;
        incr_ = shared->Increment();
        return;
    }
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = kMaxDescriptors;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (SUCCEEDED(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_)))) {
        incr_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    } else {
        buildFailed_ = true;
    }
}

void BindlessTable::Reset()
{
    geomCache_.clear();
    descriptorCache_.clear();
    latestSet_.clear();
    retiredSets_.clear();
    freeSets_.clear();
    setSlotsUsed_ = 0;
    frameNo_ = 0;
    geomInfo_.clear();
    frameGeometry_ = {};
    geomVersion_ = 0;
    buildFailed_ = false;
    heap_.Reset();
    shared_ = nullptr;
    base_ = 0;
    incr_ = 0;
    device_ = nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE BindlessTable::CpuHandle(UINT index) const
{
    if (shared_) { return shared_->Cpu(index); }
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * incr_;
    return h;
}

void BindlessTable::WriteSceneDescriptor(UINT frameIndex, UINT which, D3D12_CPU_DESCRIPTOR_HANDLE srcCpu)
{
    if (!Ready() || frameIndex >= render::kFrameCount || which >= kScenePerFrame || srcCpu.ptr == 0) {
        return;
    }
    device_->CopyDescriptorsSimple(1, CpuHandle(SceneIndex(frameIndex, which)), srcCpu,
                                   D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

uint32_t BindlessTable::GetOrUpdateMesh(const void* owner, Mesh* mesh, D3D12_CPU_DESCRIPTOR_HANDLE albedoSrv,
                                          D3D12_CPU_DESCRIPTOR_HANDLE mrSrv,
                                          const float* baseColor4, float roughness, float metalness,
                                          bool mrMultiply, float alphaCutoff,
                                          uint32_t albedoGen, uint32_t mrGen)
{
    const SlotMaterial one{ albedoSrv, mrSrv, baseColor4, roughness, metalness, mrMultiply, alphaCutoff, albedoGen, mrGen };
    return GetOrUpdateMesh(owner, mesh, &one, 1);
}

uint32_t BindlessTable::GetOrRegisterDescriptors(Mesh* mesh, const SlotMaterial& material)
{
    const DescriptorKey key{ mesh, material.albedoSrv.ptr, material.mrSrv.ptr, material.albedoGen, material.mrGen };
    auto it = descriptorCache_.find(key);
    if (it != descriptorCache_.end()) {
        return it->second;
    }
    // A2: same material, new texture generation -> the old set is retired (frames in flight still
    // index it) and this registration takes a fresh slot. Sets are never rewritten in place.
    const MaterialKey mk{ mesh, material.albedoSrv.ptr, material.mrSrv.ptr };
    auto latest = latestSet_.find(mk);
    if (latest != latestSet_.end()) {
        auto old = descriptorCache_.find(latest->second);
        if (old != descriptorCache_.end()) {
            retiredSets_.push_back(RetiredSet{ old->second, frameNo_ + render::kFrameCount });
            descriptorCache_.erase(old);
        }
        latestSet_.erase(latest);
    }
    UINT geoSlot = 0;
    if (!freeSets_.empty()) {
        geoSlot = freeSets_.back();
        freeSets_.pop_back();
    }
    else if (setSlotsUsed_ >= (kMaxDescriptors - kGeoBase) / kDescPerGeom) {
        // On the transition only: every registration after exhaustion lands here again, and the
        // renderer has already switched to SSR on the first one.
        if (!buildFailed_) {
            LOG_ERROR(logging::LogCategory::RenderRt,
                      "bindless descriptor capacity exhausted ({} geometries); refusing out-of-bounds write, RT off until the next level",
                      descriptorCache_.size());
        }
        buildFailed_ = true;
        return kInvalidGeometry;
    }
    else {
        geoSlot = base_ + kGeoBase + kDescPerGeom * setSlotsUsed_++;
    }

    // Raw (ByteAddressBuffer) SRVs over the whole VB/IB.
    auto makeRawSrv = [&](ID3D12Resource* res, UINT slot) {
        if (!res || !Ready()) {
            return;
        }
        const UINT64 bytes = res->GetDesc().Width;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements = static_cast<UINT>(bytes / 4);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device_->CreateShaderResourceView(res, &srv, CpuHandle(slot));
    };

    makeRawSrv(mesh ? mesh->GetVertexBufferResource() : nullptr, geoSlot);
    makeRawSrv(mesh ? mesh->GetIndexBufferResource() : nullptr, geoSlot + 1u);
    if (material.albedoSrv.ptr != 0) {
        device_->CopyDescriptorsSimple(1, CpuHandle(geoSlot + 2u), material.albedoSrv,
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (material.mrSrv.ptr != 0) {
        device_->CopyDescriptorsSimple(1, CpuHandle(geoSlot + 3u), material.mrSrv,
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    descriptorCache_.emplace(key, geoSlot);
    latestSet_[mk] = key;
    return geoSlot;
}

uint32_t BindlessTable::GetOrUpdateMesh(const void* owner, Mesh* mesh,
                                       const SlotMaterial* slots, size_t slotCount)
{
    if (!Ready() || buildFailed_) { return kInvalidGeometry; }
    static const SlotMaterial kDefaultSlot{};
    if (!slots || slotCount == 0) { slots = &kDefaultSlot; slotCount = 1; }

    const size_t submeshCount = mesh ? std::max<size_t>(mesh->GetSubmeshCount(), 1u) : 1u;
    const GeometryKey key{ owner, mesh };
    auto it = geomCache_.find(key);
    const bool newOwner = it == geomCache_.end();
    const uint32_t base = newOwner ? static_cast<uint32_t>(geomInfo_.size()) : it->second;
    if (newOwner) {
        // InstanceID is 24 bits. Check the entire contiguous submesh run before appending.
        if (geomInfo_.size() + submeshCount > (1u << 24)) {
            buildFailed_ = true;
            return kInvalidGeometry;
        }
        geomInfo_.resize(geomInfo_.size() + submeshCount);
        geomCache_.emplace(key, base);
    }

    bool changed = newOwner;
    static const std::vector<Mesh::Submesh> kNoSubs;
    const std::vector<Mesh::Submesh>& subs = mesh ? mesh->GetSubmeshes() : kNoSubs;
    for (size_t s = 0; s < submeshCount; ++s) {
        const SlotMaterial& sm = slots[s < slotCount ? s : slotCount - 1];
        const UINT geoSlot = GetOrRegisterDescriptors(mesh, sm);
        if (geoSlot == kInvalidGeometry) { return kInvalidGeometry; }

        GeometryInfoGPU rec{};
        rec.vbIndex = geoSlot;
        rec.ibIndex = geoSlot + 1u;
        rec.indexIs32 = (mesh && mesh->GetIndexFormat() == DXGI_FORMAT_R32_UINT) ? 1u : 0u;
        rec.albedoTexIndex = sm.albedoSrv.ptr ? geoSlot + 2u : 0xFFFFFFFFu;
        rec.mrTexIndex = sm.mrSrv.ptr ? geoSlot + 3u : 0xFFFFFFFFu;
        rec.roughness = sm.roughness;
        rec.metalness = sm.metalness;
        rec.mrMultiply = sm.mrMultiply ? 1u : 0u;
        if (sm.baseColor4) {
            rec.baseColor[0] = sm.baseColor4[0]; rec.baseColor[1] = sm.baseColor4[1];
            rec.baseColor[2] = sm.baseColor4[2]; rec.baseColor[3] = sm.baseColor4[3];
        }
        rec.firstTri = (s < subs.size()) ? subs[s].indexOffset / 3u : 0u;
        rec.vertexStride = mesh ? mesh->GetVertexStride() : 0u;
        // Part C: the cutoff only matters when there is an albedo texture to test against — with
        // no texture the alpha is a per-slot constant and the raster path never authors that as
        // MASK. Keeping the record opaque then matches the BLAS mask in RtSceneAs.
        rec.alphaCutoff = (rec.albedoTexIndex != 0xFFFFFFFFu) ? sm.alphaCutoff : -1.0f;

        GeometryInfoGPU& current = geomInfo_[base + s];
        rec.flags = current.flags; // owned by the RW reuse-deny pass, not by material state
        if (std::memcmp(&current, &rec, sizeof(rec)) != 0) {
            current = rec;
            changed = true;
        }
    }

    if (changed) { ++geomVersion_; }
    return base;
}

void BindlessTable::SetScreenReuseDenied(uint32_t baseRecord, uint32_t count, bool denied)
{
    if (!Ready() || buildFailed_) { return; }
    const size_t end = std::min(geomInfo_.size(), static_cast<size_t>(baseRecord) + count);
    bool changed = false;
    for (size_t i = baseRecord; i < end; ++i) {
        const uint32_t next = denied ? (geomInfo_[i].flags | kGeomFlagNoScreenReuse)
                                     : (geomInfo_[i].flags & ~kGeomFlagNoScreenReuse);
        if (next != geomInfo_[i].flags) {
            geomInfo_[i].flags = next;
            changed = true;
        }
    }
    if (changed) { ++geomVersion_; }
}

bool BindlessTable::FrameReady(UINT frameIndex) const
{
    return Ready() && !buildFailed_ && frameIndex < render::kFrameCount &&
        frameGeometry_[frameIndex].buffer && frameGeometry_[frameIndex].version == geomVersion_;
}

bool BindlessTable::UploadGeometryInfo(UINT frameIndex)
{
    if (!Ready() || buildFailed_ || frameIndex >= render::kFrameCount || geomInfo_.empty()) {
        return false;
    }
    auto& frame = frameGeometry_[frameIndex];
    if (FrameReady(frameIndex)) { return true; }

    const UINT count = static_cast<UINT>(geomInfo_.size());
    const UINT64 bytes = static_cast<UINT64>(count) * sizeof(GeometryInfoGPU);

    if (frame.capacity < count || !frame.buffer) {
        const UINT capacity = std::max(count, std::max(64u, frame.capacity * 2u));
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = static_cast<UINT64>(capacity) * sizeof(GeometryInfoGPU);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&buffer)))) {
            buildFailed_ = true;
            return false;
        }
        // Only this slot is fence-safe. The other slots must retain both resource and SRV.
        frame.buffer = std::move(buffer);
        frame.capacity = capacity;
    }

    void* mapped = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(frame.buffer->Map(0, &noRead, &mapped))) {
        buildFailed_ = true;
        return false;
    }
    std::memcpy(mapped, geomInfo_.data(), static_cast<size_t>(bytes));
    const D3D12_RANGE written{ 0, static_cast<SIZE_T>(bytes) };
    frame.buffer->Unmap(0, &written);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = 0;
    srv.Buffer.NumElements = count;
    srv.Buffer.StructureByteStride = sizeof(GeometryInfoGPU);
    device_->CreateShaderResourceView(frame.buffer.Get(), &srv, CpuHandle(GeomInfoIndex(frameIndex)));
    frame.version = geomVersion_;
    return true;
}

} // namespace rt
