#include "rendering/rt/BindlessTable.h"

#include "rendering/meshes/Mesh.h"

#include <cstring>

namespace rt {

namespace {
// FNV-1a hash of the fields that make a geometry-info record unique: the mesh
// (geometry) plus the material (albedo SRV, base color, roughness, metalness).
// Two instances with the same mesh AND material share one record; different
// materials on a shared mesh get distinct records.
uint64_t FloatBits(float f) { uint32_t b; std::memcpy(&b, &f, sizeof(b)); return b; }
uint64_t MakeGeomKey(Mesh* mesh, SIZE_T albedoSrvPtr, SIZE_T mrSrvPtr, const float* baseColor4, float roughness, float metalness)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(reinterpret_cast<uint64_t>(mesh));
    mix(static_cast<uint64_t>(albedoSrvPtr));
    mix(static_cast<uint64_t>(mrSrvPtr));
    mix(FloatBits(roughness));
    mix(FloatBits(metalness));
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
                                          const float* baseColor4, float roughness, float metalness)
{
    const uint64_t key = MakeGeomKey(mesh, albedoSrv.ptr, mrSrv.ptr, baseColor4, roughness, metalness);
    auto it = geomCache_.find(key);
    if (it != geomCache_.end()) {
        return it->second;
    }

    const uint32_t geomIndex = static_cast<uint32_t>(geomInfo_.size());
    ID3D12Resource* vb = mesh ? mesh->GetVertexBufferResource() : nullptr;
    ID3D12Resource* ib = mesh ? mesh->GetIndexBufferResource() : nullptr;
    const UINT geoSlot = kGeoBase + kDescPerGeom * geomIndex;

    GeometryInfoGPU rec{};
    rec.vbIndex = geoSlot;
    rec.ibIndex = geoSlot + 1u;
    rec.indexIs32 = (mesh && mesh->GetIndexFormat() == DXGI_FORMAT_R32_UINT) ? 1u : 0u;
    rec.albedoTexIndex = 0xFFFFFFFFu;
    rec.roughness = roughness;
    rec.metalness = metalness;
    if (baseColor4) {
        rec.baseColor[0] = baseColor4[0]; rec.baseColor[1] = baseColor4[1];
        rec.baseColor[2] = baseColor4[2]; rec.baseColor[3] = baseColor4[3];
    }

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
    makeRawSrv(vb, rec.vbIndex);
    makeRawSrv(ib, rec.ibIndex);

    // Albedo + MR texture SRVs (copied from the material's CPU SRVs) at slots 2 and 3.
    if (albedoSrv.ptr != 0 && heap_) {
        const UINT albedoSlot = geoSlot + 2u;
        device_->CopyDescriptorsSimple(1, CpuHandle(albedoSlot), albedoSrv,
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        rec.albedoTexIndex = albedoSlot;
    }
    if (mrSrv.ptr != 0 && heap_) {
        const UINT mrSlot = geoSlot + 3u;
        device_->CopyDescriptorsSimple(1, CpuHandle(mrSlot), mrSrv,
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        rec.mrTexIndex = mrSlot;
    }

    geomCache_.emplace(key, geomIndex);
    geomInfo_.push_back(rec);
    return geomIndex;
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
