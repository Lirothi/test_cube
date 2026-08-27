#include "rendering/rt/BindlessTable.h"

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
    return static_cast<size_t>(h);
}

void BindlessTable::Init(ID3D12Device* device)
{
    device_ = device;
    if (!device_ || heap_) {
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
    geomInfo_.clear();
    frameGeometry_ = {};
    geomVersion_ = 0;
    buildFailed_ = false;
    heap_.Reset();
    incr_ = 0;
    device_ = nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE BindlessTable::CpuHandle(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * incr_;
    return h;
}

void BindlessTable::WriteSceneDescriptor(UINT frameIndex, UINT which, D3D12_CPU_DESCRIPTOR_HANDLE srcCpu)
{
    if (!heap_ || frameIndex >= render::kFrameCount || which >= kScenePerFrame || srcCpu.ptr == 0) {
        return;
    }
    device_->CopyDescriptorsSimple(1, CpuHandle(SceneIndex(frameIndex, which)), srcCpu,
                                   D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

uint32_t BindlessTable::GetOrUpdateMesh(const void* owner, Mesh* mesh, D3D12_CPU_DESCRIPTOR_HANDLE albedoSrv,
                                          D3D12_CPU_DESCRIPTOR_HANDLE mrSrv,
                                          const float* baseColor4, float roughness, float metalness,
                                          bool mrMultiply)
{
    const SlotMaterial one{ albedoSrv, mrSrv, baseColor4, roughness, metalness, mrMultiply };
    return GetOrUpdateMesh(owner, mesh, &one, 1);
}

uint32_t BindlessTable::GetOrRegisterDescriptors(Mesh* mesh, const SlotMaterial& material)
{
    const DescriptorKey key{ mesh, material.albedoSrv.ptr, material.mrSrv.ptr };
    auto it = descriptorCache_.find(key);
    if (it != descriptorCache_.end()) {
        return it->second;
    }
    if (descriptorCache_.size() >= (kMaxDescriptors - kGeoBase) / kDescPerGeom) {
        buildFailed_ = true;
        OutputDebugStringA("[RT] Bindless descriptor capacity exhausted; refusing out-of-bounds write.\n");
        return kInvalidGeometry;
    }
    const UINT geoSlot = kGeoBase + kDescPerGeom * static_cast<UINT>(descriptorCache_.size());

    // Raw (ByteAddressBuffer) SRVs over the whole VB/IB.
    auto makeRawSrv = [&](ID3D12Resource* res, UINT slot) {
        if (!res || !heap_) {
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

        GeometryInfoGPU& current = geomInfo_[base + s];
        if (std::memcmp(&current, &rec, sizeof(rec)) != 0) {
            current = rec;
            changed = true;
        }
    }

    if (changed) { ++geomVersion_; }
    return base;
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
