#include "rendering/rt/AccelerationStructure.h"

#include "rendering/meshes/Mesh.h"

namespace rt {

namespace {

Microsoft::WRL::ComPtr<ID3D12Resource> CreateBufferHelper(ID3D12Device* device,
                                                         UINT64 size,
                                                         D3D12_HEAP_TYPE heapType,
                                                         D3D12_RESOURCE_STATES initialState,
                                                         D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;

    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               initialState, nullptr, IID_PPV_ARGS(&res)))) {
        return nullptr;
    }
    return res;
}

// DEFAULT-heap UAV buffer for AS result/scratch. Both need ALLOW_UNORDERED_ACCESS;
// result is created directly in the RAYTRACING_ACCELERATION_STRUCTURE state.
Microsoft::WRL::ComPtr<ID3D12Resource> CreateUavBuffer(ID3D12Device* device,
                                                       UINT64 size,
                                                       D3D12_RESOURCE_STATES initialState)
{
    return CreateBufferHelper(device, size, D3D12_HEAP_TYPE_DEFAULT, initialState,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
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

void AccelerationStructureManager::EnsureSrvHeap()
{
    if (srvHeap_ || !device5_) {
        return;
    }
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = render::kFrameCount;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-only; renderer stages it into a shader-visible heap
    if (SUCCEEDED(device5_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&srvHeap_)))) {
        srvIncrement_ = device5_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE AccelerationStructureManager::TlasSrvCpu(UINT frameIndex) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (srvHeap_ && frameIndex < tlasFrames_.size()) {
        handle = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(frameIndex) * srvIncrement_;
    }
    return handle;
}

void AccelerationStructureManager::BuildTlas(std::span<const InstanceEntry> instances,
                                             ID3D12GraphicsCommandList4* cmdList4, UINT frameIndex)
{
    if (!device5_ || !cmdList4 || frameIndex >= tlasFrames_.size()) {
        return;
    }
    EnsureSrvHeap();

    PerFrameTlas& f = tlasFrames_[frameIndex];
    const UINT count = static_cast<UINT>(instances.size());
    f.instanceCount = count;
    if (count == 0) {
        return; // nothing to build; leave the previous frame's SRV untouched
    }

    // 1) Instance descs in a per-frame UPLOAD buffer (grow on demand).
    const UINT64 instBytes = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * count;
    if (f.instanceCapacity < count || !f.instanceUpload) {
        f.instanceUpload = CreateBufferHelper(device5_, instBytes, D3D12_HEAP_TYPE_UPLOAD,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
        f.instanceCapacity = f.instanceUpload ? count : 0;
    }
    if (!f.instanceUpload) {
        return;
    }

    D3D12_RAYTRACING_INSTANCE_DESC* descs = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(f.instanceUpload->Map(0, &noRead, reinterpret_cast<void**>(&descs)))) {
        return;
    }
    for (UINT i = 0; i < count; ++i) {
        const InstanceEntry& e = instances[i];
        D3D12_RAYTRACING_INSTANCE_DESC d{};
        // world is row-major / row-vector (p*M). DXR's 3x4 Transform is
        // column-vector (M*p) with translation in column 3, i.e. the transpose
        // of the upper 3x4: Transform[r][c] = world.m[c][r].
        const DirectX::XMFLOAT4X4& w = e.world;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                d.Transform[r][c] = w.m[c][r];
            }
        }
        d.InstanceID = e.instanceId & 0xFFFFFFu;
        d.InstanceMask = 0xFF;
        d.InstanceContributionToHitGroupIndex = e.instanceId & 0xFFFFFFu;
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        d.AccelerationStructure = GetOrBuildBlas(e.mesh, cmdList4).Address();
        descs[i] = d;
    }
    f.instanceUpload->Unmap(0, nullptr);

    // 2) Prebuild info -> (re)allocate result + scratch (grow on demand; the
    //    per-frame slot is reused only once its prior GPU work has completed).
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = count;
    inputs.InstanceDescs = f.instanceUpload->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0) {
        return;
    }

    if (f.resultSize < info.ResultDataMaxSizeInBytes || !f.result) {
        f.result = CreateUavBuffer(device5_, info.ResultDataMaxSizeInBytes,
                                   D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        f.resultSize = f.result ? info.ResultDataMaxSizeInBytes : 0;
    }
    if (f.scratchSize < info.ScratchDataSizeInBytes || !f.scratch) {
        f.scratch = CreateUavBuffer(device5_, info.ScratchDataSizeInBytes,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        f.scratchSize = f.scratch ? info.ScratchDataSizeInBytes : 0;
    }
    if (!f.result || !f.scratch) {
        return;
    }

    // 3) Build + UAV barrier.
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs;
    build.DestAccelerationStructureData = f.result->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = f.scratch->GetGPUVirtualAddress();
    cmdList4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = f.result.Get();
    cmdList4->ResourceBarrier(1, &uav);

    // 4) (Re)create the per-frame TLAS SRV at this frame's result VA. AS SRVs are
    //    created with a NULL resource and reference the AS via Location.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.RaytracingAccelerationStructure.Location = f.result->GetGPUVirtualAddress();
    device5_->CreateShaderResourceView(nullptr, &srv, TlasSrvCpu(frameIndex));
}

void AccelerationStructureManager::Reset()
{
    blasCache_.clear();
    pendingScratch_.clear();
    for (PerFrameTlas& f : tlasFrames_) {
        f = PerFrameTlas{};
    }
    srvHeap_.Reset();
    srvIncrement_ = 0;
}

} // namespace rt
