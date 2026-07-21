#include "rendering/rt/BindlessTable.h"

#include "rendering/meshes/Mesh.h"

#include <algorithm>
#include <cstring>

namespace rt {

namespace {
// FNV-1a hash of the fields that make a geometry-info record unique: the mesh
// (geometry) plus the material (albedo SRV, base color, roughness, metalness).
// Two instances with the same mesh AND material share one record; different
// materials on a shared mesh get distinct records.
uint64_t FloatBits(float f) { uint32_t b; std::memcpy(&b, &f, sizeof(b)); return b; }
uint64_t MakeGeomKey(Mesh* mesh, SIZE_T albedoSrvPtr, SIZE_T mrSrvPtr, const float* baseColor4,
                     float roughness, float metalness, bool mrMultiply)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(reinterpret_cast<uint64_t>(mesh));
    mix(static_cast<uint64_t>(albedoSrvPtr));
    mix(static_cast<uint64_t>(mrSrvPtr));
    mix(FloatBits(roughness));
    mix(FloatBits(metalness));
    mix(mrMultiply ? 1ull : 0ull);
    if (baseColor4) { for (int i = 0; i < 4; ++i) { mix(FloatBits(baseColor4[i])); } }
    return h;
}
} // namespace

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
    }
}

void BindlessTable::Reset()
{
    geomCache_.clear();
    geomInfo_.clear();
    geomInfoBuffer_.Reset();
    geomInfoCapacity_ = 0;
    heap_.Reset();
    incr_ = 0;
}

D3D12_CPU_DESCRIPTOR_HANDLE BindlessTable::CpuHandle(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * incr_;
    return h;
}

void BindlessTable::WriteSceneDescriptor(UINT frameIndex, UINT which, D3D12_CPU_DESCRIPTOR_HANDLE srcCpu)
{
    if (!heap_ || srcCpu.ptr == 0) {
        return;
    }
    device_->CopyDescriptorsSimple(1, CpuHandle(SceneIndex(frameIndex, which)), srcCpu,
                                   D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

uint32_t BindlessTable::GetOrRegisterMesh(Mesh* mesh, D3D12_CPU_DESCRIPTOR_HANDLE albedoSrv,
                                          D3D12_CPU_DESCRIPTOR_HANDLE mrSrv,
                                          const float* baseColor4, float roughness, float metalness,
                                          bool mrMultiply)
{
    const SlotMaterial one{ albedoSrv, mrSrv, baseColor4, roughness, metalness, mrMultiply };
    return GetOrRegisterMesh(mesh, &one, 1);
}

uint32_t BindlessTable::GetOrRegisterMesh(Mesh* mesh, const SlotMaterial* slots, size_t slotCount)
{
    static const SlotMaterial kDefaultSlot{};
    if (!slots || slotCount == 0) { slots = &kDefaultSlot; slotCount = 1; }

    // Key = mesh + every slot's material (two objects sharing a mesh but overriding a slot get
    // distinct record runs).
    uint64_t key = MakeGeomKey(mesh, slots[0].albedoSrv.ptr, slots[0].mrSrv.ptr,
                               slots[0].baseColor4, slots[0].roughness, slots[0].metalness,
                               slots[0].mrMultiply);
    for (size_t s = 1; s < slotCount; ++s) {
        key ^= MakeGeomKey(mesh, slots[s].albedoSrv.ptr, slots[s].mrSrv.ptr,
                           slots[s].baseColor4, slots[s].roughness, slots[s].metalness,
                           slots[s].mrMultiply) + s;
    }
    auto it = geomCache_.find(key);
    if (it != geomCache_.end()) {
        return it->second;
    }

    ID3D12Resource* vb = mesh ? mesh->GetVertexBufferResource() : nullptr;
    ID3D12Resource* ib = mesh ? mesh->GetIndexBufferResource() : nullptr;

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

    // One record per submesh (contiguous — hits resolve via geom[InstanceID + GeometryIndex]),
    // each with its OWN kDescPerGeom descriptor block (VB/IB duplicated per record for uniform
    // spacing; albedo/MR from that submesh's slot — palms reflect per-slot, not slot-0).
    const uint32_t base = static_cast<uint32_t>(geomInfo_.size());
    const size_t submeshCount = mesh ? std::max<size_t>(mesh->GetSubmeshCount(), 1u) : 1u;
    static const std::vector<Mesh::Submesh> kNoSubs;
    const std::vector<Mesh::Submesh>& subs = mesh ? mesh->GetSubmeshes() : kNoSubs;
    for (size_t s = 0; s < submeshCount; ++s) {
        const SlotMaterial& sm = slots[s < slotCount ? s : slotCount - 1];
        const uint32_t geomIndex = static_cast<uint32_t>(geomInfo_.size());
        const UINT geoSlot = kGeoBase + kDescPerGeom * geomIndex;

        GeometryInfoGPU rec{};
        rec.vbIndex = geoSlot;
        rec.ibIndex = geoSlot + 1u;
        rec.indexIs32 = (mesh && mesh->GetIndexFormat() == DXGI_FORMAT_R32_UINT) ? 1u : 0u;
        rec.albedoTexIndex = 0xFFFFFFFFu;
        rec.roughness = sm.roughness;
        rec.metalness = sm.metalness;
        rec.mrMultiply = sm.mrMultiply ? 1u : 0u;
        if (sm.baseColor4) {
            rec.baseColor[0] = sm.baseColor4[0]; rec.baseColor[1] = sm.baseColor4[1];
            rec.baseColor[2] = sm.baseColor4[2]; rec.baseColor[3] = sm.baseColor4[3];
        }
        rec.firstTri = (s < subs.size()) ? subs[s].indexOffset / 3u : 0u;

        makeRawSrv(vb, rec.vbIndex);
        makeRawSrv(ib, rec.ibIndex);
        if (sm.albedoSrv.ptr != 0 && heap_) {
            const UINT albedoSlot = geoSlot + 2u;
            device_->CopyDescriptorsSimple(1, CpuHandle(albedoSlot), sm.albedoSrv,
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            rec.albedoTexIndex = albedoSlot;
        }
        if (sm.mrSrv.ptr != 0 && heap_) {
            const UINT mrSlot = geoSlot + 3u;
            device_->CopyDescriptorsSimple(1, CpuHandle(mrSlot), sm.mrSrv,
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            rec.mrTexIndex = mrSlot;
        }
        geomInfo_.push_back(rec);
    }

    geomCache_.emplace(key, base);
    return base;
}

void BindlessTable::UploadGeometryInfo()
{
    if (!heap_ || geomInfo_.empty()) {
        return;
    }

    const UINT count = static_cast<UINT>(geomInfo_.size());
    const UINT64 bytes = static_cast<UINT64>(count) * sizeof(GeometryInfoGPU);

    if (geomInfoCapacity_ < count || !geomInfoBuffer_) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        geomInfoBuffer_.Reset();
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&geomInfoBuffer_)))) {
            return;
        }
        geomInfoCapacity_ = count;
    }

    void* mapped = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    if (SUCCEEDED(geomInfoBuffer_->Map(0, &noRead, &mapped))) {
        std::memcpy(mapped, geomInfo_.data(), static_cast<size_t>(bytes));
        geomInfoBuffer_->Unmap(0, nullptr);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = 0;
    srv.Buffer.NumElements = count;
    srv.Buffer.StructureByteStride = sizeof(GeometryInfoGPU);
    device_->CreateShaderResourceView(geomInfoBuffer_.Get(), &srv, CpuHandle(kGeomInfoSlot));
}

} // namespace rt
