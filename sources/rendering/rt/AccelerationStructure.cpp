#include "rendering/rt/AccelerationStructure.h"

#include "rendering/meshes/Mesh.h"

namespace rt {

namespace {

// DEFAULT-heap UAV buffer for AS result/scratch. Both need ALLOW_UNORDERED_ACCESS;
// result is created directly in the RAYTRACING_ACCELERATION_STRUCTURE state.
Microsoft::WRL::ComPtr<ID3D12Resource> CreateUavBuffer(ID3D12Device* device,
                                                       UINT64 size,
                                                       D3D12_RESOURCE_STATES initialState)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               initialState, nullptr, IID_PPV_ARGS(&res)))) {
        return nullptr;
    }
    return res;
}

} // namespace

const Blas& AccelerationStructureManager::GetOrBuildBlas(Mesh* mesh, ID3D12GraphicsCommandList4* cmdList4)
{
    Blas& slot = blasCache_[mesh];
    if (slot.result) {
        return slot; // already built (static geometry — build once, cache)
    }

    ID3D12Resource* vb = mesh ? mesh->GetVertexBufferResource() : nullptr;
    ID3D12Resource* ib = mesh ? mesh->GetIndexBufferResource() : nullptr;
    const UINT stride = mesh ? mesh->GetVertexStride() : 0;
    if (!device5_ || !cmdList4 || !vb || stride == 0) {
        return slot; // null result; caller checks Address()
    }

    // Position is at offset 0 of VertexPNTUV; the mesh exposes no vertex count,
    // so derive it from the (exactly sized) vertex buffer.
    const UINT vertexCount = static_cast<UINT>(vb->GetDesc().Width / stride);

    D3D12_RAYTRACING_GEOMETRY_DESC geom{};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
    geom.Triangles.VertexBuffer.StrideInBytes = stride;
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexCount = vertexCount;
    if (ib && mesh->GetIndexCount() > 0) {
        geom.Triangles.IndexBuffer = ib->GetGPUVirtualAddress();
        geom.Triangles.IndexFormat = mesh->GetIndexFormat();
        geom.Triangles.IndexCount = mesh->GetIndexCount();
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = 1;
    inputs.pGeometryDescs = &geom;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0) {
        return slot;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> result =
        CreateUavBuffer(device5_, info.ResultDataMaxSizeInBytes,
                        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    Microsoft::WRL::ComPtr<ID3D12Resource> scratch =
        CreateUavBuffer(device5_, info.ScratchDataSizeInBytes,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!result || !scratch) {
        return slot;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs; // pGeometryDescs (geom) stays valid for this record call
    build.DestAccelerationStructureData = result->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    cmdList4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

    // Make the build's writes visible before the TLAS (or any reader) consumes it.
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = result.Get();
    cmdList4->ResourceBarrier(1, &uav);

    slot.result = std::move(result);
    pendingScratch_.push_back(std::move(scratch)); // freed via ReleaseCompletedScratch() post-fence
    return slot;
}

} // namespace rt
