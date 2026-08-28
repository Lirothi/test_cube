#include "rendering/rt/AccelerationStructure.h"

#include <algorithm>
#include <atomic>
#include <vector>

#include "rendering/core/BarrierTranslation.h"
#include "rendering/meshes/Mesh.h"

namespace rt {

namespace {

// S13 test/dev hook: forces every AS buffer allocation to fail so the graceful
// disable → SSR-fallback path can be exercised (set via the rt-force-as-fail flag).
std::atomic<bool> g_forceAsAllocFailure{ false };

Microsoft::WRL::ComPtr<ID3D12Resource> CreateBufferHelper(ID3D12Device* device,
                                                         UINT64 size,
                                                         D3D12_HEAP_TYPE heapType,
                                                         D3D12_RESOURCE_STATES initialState,
                                                         D3D12_RESOURCE_FLAGS flags)
{
    if (g_forceAsAllocFailure.load(std::memory_order_relaxed)) {
        return nullptr; // simulate out-of-memory
    }

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

const Blas& AccelerationStructureManager::GetOrBuildBlas(Mesh* mesh, uint64_t nonOpaqueSlots,
                                                         ID3D12GraphicsCommandList4* cmdList4)
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

    // B3: one geometry desc per submesh (LOD0 table; every mesh has >= 1 spanning the buffer).
    // Same triangles as the old whole-buffer desc, but hits report GeometryIndex() so per-slot
    // decisions (Part C: masked/exclusion flags, per-slot materials) become possible. The hit
    // shaders index the bindless geometry-info table with InstanceID + GeometryIndex.
    const std::vector<Mesh::Submesh>& subs = mesh->GetSubmeshes();
    const UINT idxBytes = (mesh->GetIndexFormat() == DXGI_FORMAT_R32_UINT) ? 4u : 2u;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geoms(std::max<size_t>(subs.size(), 1));
    for (size_t s = 0; s < geoms.size(); ++s) {
        D3D12_RAYTRACING_GEOMETRY_DESC& geom = geoms[s];
        geom = {};
        geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        // Part C: masked submeshes stay non-opaque so traversal surfaces them as
        // CANDIDATE_NON_OPAQUE_TRIANGLE for the RayQuery alpha test; everything else keeps the
        // OPAQUE fast path (no candidate cost). UE's equivalent decision is per instance
        // (bForceOpaque only when every segment is BLEND_Opaque); per-geometry is finer.
        const bool masked = s < 64u && ((nonOpaqueSlots >> s) & 1ull) != 0ull;
        geom.Flags = masked ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE
                            : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geom.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
        geom.Triangles.VertexBuffer.StrideInBytes = stride;
        geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geom.Triangles.VertexCount = vertexCount;
        if (ib && mesh->GetIndexCount() > 0) {
            const UINT indexOffset = (s < subs.size()) ? subs[s].indexOffset : 0u;
            const UINT indexCount = (s < subs.size()) ? subs[s].indexCount
                                                      : static_cast<UINT>(mesh->GetIndexCount());
            geom.Triangles.IndexBuffer = ib->GetGPUVirtualAddress() +
                                         static_cast<UINT64>(indexOffset) * idxBytes;
            geom.Triangles.IndexFormat = mesh->GetIndexFormat();
            geom.Triangles.IndexCount = indexCount;
        }
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = static_cast<UINT>(geoms.size());
    inputs.pGeometryDescs = geoms.data();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0) {
        return slot;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> result =
        CreateUavBuffer(device5_, info.ResultDataMaxSizeInBytes,
                        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    // Scratch is created in COMMON: D3D12 ignores (and warns about) a non-COMMON
    // initial state on buffers, and the build's UAV write implicitly promotes it.
    Microsoft::WRL::ComPtr<ID3D12Resource> scratch =
        CreateUavBuffer(device5_, info.ScratchDataSizeInBytes,
                        D3D12_RESOURCE_STATE_COMMON);
    if (!result || !scratch) {
        buildFailed_ = true; // out of VRAM / device lost -> caller disables RT, falls back to SSR
        return slot;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs; // pGeometryDescs (geom) stays valid for this record call
    build.DestAccelerationStructureData = result->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    cmdList4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

    // Make the build's writes visible before the TLAS (or any reader) consumes it. Barrier plan
    // step 14: an AS gets its own emitter — RTAS access, not UAV access — because no legacy state
    // can spell an AS write, so the generic translation has nothing correct to say about it.
    barriers::EmitAccelerationStructureBuildBarrier(cmdList4, result.Get());

    slot.result = std::move(result);
    pendingScratch_.push_back(std::move(scratch)); // freed via ReleaseCompletedScratch() post-fence
    return slot;
}

void AccelerationStructureManager::EnsureWindDescHeap()
{
    if (windDescHeap_ || !device5_) { return; }
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = kMaxWindBlasSlots * 2;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-only; staged into the frame heap per use
    if (SUCCEEDED(device5_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&windDescHeap_)))) {
        windDescIncrement_ =
            device5_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
}

bool AccelerationStructureManager::PrepareWindSlot(UINT slot, Mesh* mesh,
                                                   ID3D12GraphicsCommandList4* cmdList4,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE& srcVbSrvCpu,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE& dstUavCpu,
                                                   UINT& vertexCount)
{
    srcVbSrvCpu = {};
    dstUavCpu = {};
    vertexCount = 0;
    if (!device5_ || !cmdList4 || !mesh || slot >= kMaxWindBlasSlots) { return false; }
    ID3D12Resource* vb = mesh->GetVertexBufferResource();
    const UINT stride = mesh->GetVertexStride();
    if (!vb || stride == 0) { return false; }
    EnsureWindDescHeap();
    if (!windDescHeap_) { buildFailed_ = true; return false; }

    WindBlasSlot& s = windSlots_[slot];
    const UINT count = static_cast<UINT>(vb->GetDesc().Width / stride);
    if (s.mesh != mesh || s.vertexCount != count || !s.deformedVb)
    {
        // Mesh (re)assignment. The old stream/BLAS/scratch may still be REFERENCED IN FLIGHT --
        // the previous frames' TLASes point at the old BLAS and their command lists read the old
        // stream -- so they retire through the same fence-guarded bin as one-time BLAS scratch
        // instead of being destroyed here. Releasing them inline was a live device removal on
        // wind_test: 610 palms at near-equal distances churned the slot binding every frame.
        if (s.deformedVb) { pendingScratch_.push_back(std::move(s.deformedVb)); }
        if (s.blas) { pendingScratch_.push_back(std::move(s.blas)); }
        if (s.scratch) { pendingScratch_.push_back(std::move(s.scratch)); }
        s.deformedVb = CreateUavBuffer(device5_, static_cast<UINT64>(count) * 12ull,
                                       D3D12_RESOURCE_STATE_COMMON);
        if (!s.deformedVb) { buildFailed_ = true; return false; }
        s.mesh = mesh;
        s.vertexCount = count;
        s.builtOnce = false;
        s.blasSize = 0;
        s.scratchSize = 0;

        // (Re)write the slot's descriptor pair: raw SRV over the source VB, raw UAV over the
        // position stream. Raw views address in 4-byte units.
        D3D12_CPU_DESCRIPTOR_HANDLE base = windDescHeap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{ base.ptr + static_cast<SIZE_T>(slot) * 2 * windDescIncrement_ };
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpu{ srvCpu.ptr + windDescIncrement_ };

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = static_cast<UINT>(vb->GetDesc().Width / 4ull);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device5_->CreateShaderResourceView(vb, &srv, srvCpu);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = count * 3u;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device5_->CreateUnorderedAccessView(s.deformedVb.Get(), nullptr, &uav, uavCpu);
    }

    // No barrier here ON PURPOSE: buffers DECAY to COMMON at every ExecuteCommandLists, so at
    // the top of this (first-in-frame) pass the stream is always COMMON and the deform's UAV
    // write promotes it. Tracking its state across frames would assert a StateBefore the
    // resource no longer holds.
    D3D12_CPU_DESCRIPTOR_HANDLE base = windDescHeap_->GetCPUDescriptorHandleForHeapStart();
    srcVbSrvCpu = { base.ptr + static_cast<SIZE_T>(slot) * 2 * windDescIncrement_ };
    dstUavCpu = { srcVbSrvCpu.ptr + windDescIncrement_ };
    vertexCount = s.vertexCount;
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS AccelerationStructureManager::BuildOrRefitWindSlot(
    UINT slot, uint64_t nonOpaqueSlots, uint64_t frameNumber, ID3D12GraphicsCommandList4* cmdList4)
{
    if (!device5_ || !cmdList4 || slot >= kMaxWindBlasSlots) { return 0; }
    WindBlasSlot& s = windSlots_[slot];
    if (!s.deformedVb || !s.mesh) { return 0; }

    // The caller batched the UAV->NON_PIXEL transitions for EVERY slot in one ResourceBarrier
    // and emits the AS-read barriers after ALL builds: 24 interleaved build+barrier pairs
    // serialize on the GPU (measured ~34 us per palm), back-to-back builds pipeline.

    // Same per-submesh geometry table as GetOrBuildBlas, but positions come from the deformed
    // stream (tightly packed float3) and the build allows updates. The INDEX table must be LOD 0:
    // the bindless records carry LOD 0 firstTri offsets and a hit's PrimitiveIndex is local to
    // THIS geometry -- building from a coarser LOD made hit shading read garbage triangles (a
    // LOD 1 experiment saved only ~0.09 ms once the barriers were batched; not worth the split
    // index space).
    Mesh* mesh = s.mesh;
    ID3D12Resource* ib = mesh->GetIndexBufferResource();
    const std::vector<Mesh::Submesh>& subs = mesh->GetSubmeshes();
    const UINT idxBytes = (mesh->GetIndexFormat() == DXGI_FORMAT_R32_UINT) ? 4u : 2u;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geoms(std::max<size_t>(subs.size(), 1));
    for (size_t g = 0; g < geoms.size(); ++g) {
        D3D12_RAYTRACING_GEOMETRY_DESC& geom = geoms[g];
        geom = {};
        geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        const bool masked = g < 64u && ((nonOpaqueSlots >> g) & 1ull) != 0ull;
        geom.Flags = masked ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE
                            : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geom.Triangles.VertexBuffer.StartAddress = s.deformedVb->GetGPUVirtualAddress();
        geom.Triangles.VertexBuffer.StrideInBytes = 12;
        geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geom.Triangles.VertexCount = s.vertexCount;
        if (ib && mesh->GetIndexCount() > 0) {
            const UINT indexOffset = (g < subs.size()) ? subs[g].indexOffset : 0u;
            const UINT indexCount = (g < subs.size()) ? subs[g].indexCount
                                                      : static_cast<UINT>(mesh->GetIndexCount());
            geom.Triangles.IndexBuffer = ib->GetGPUVirtualAddress() +
                                         static_cast<UINT64>(indexOffset) * idxBytes;
            geom.Triangles.IndexFormat = mesh->GetIndexFormat();
            geom.Triangles.IndexCount = indexCount;
        }
    }

    // Round-robin full rebuilds, staggered by slot so they never land on the same frame: frond
    // tips move on the order of a metre, which is a large deformation for a pure refit chain.
    const bool cadenceRebuild = ((frameNumber + slot) % 16ull) == 0ull;
    const bool update = s.builtOnce && !cadenceRebuild;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    // FAST_BUILD: this BLAS is rebuilt/refit every frame, so build speed dominates trace quality
    // (UE make the same choice for their WPO dynamic geometry).
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD |
                   D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    inputs.NumDescs = static_cast<UINT>(geoms.size());
    inputs.pGeometryDescs = geoms.data();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0) { return 0; }
    const UINT64 wantScratch = std::max(info.ScratchDataSizeInBytes,
                                        info.UpdateScratchDataSizeInBytes);
    if (!s.blas || s.blasSize < info.ResultDataMaxSizeInBytes)
    {
        s.blas = CreateUavBuffer(device5_, info.ResultDataMaxSizeInBytes,
                                 D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        s.blasSize = info.ResultDataMaxSizeInBytes;
        s.builtOnce = false; // fresh result buffer cannot be PERFORM_UPDATEd
    }
    if (!s.scratch || s.scratchSize < wantScratch)
    {
        s.scratch = CreateUavBuffer(device5_, wantScratch, D3D12_RESOURCE_STATE_COMMON);
        s.scratchSize = wantScratch;
    }
    if (!s.blas || !s.scratch) { buildFailed_ = true; return 0; }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs;
    if (s.builtOnce && update)
    {
        build.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        build.SourceAccelerationStructureData = s.blas->GetGPUVirtualAddress();
        ++s.updatesSinceBuild;
    }
    else
    {
        s.updatesSinceBuild = 0;
    }
    build.DestAccelerationStructureData = s.blas->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = s.scratch->GetGPUVirtualAddress();
    cmdList4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    s.builtOnce = true;
    return s.blas->GetGPUVirtualAddress();
}

ID3D12Resource* AccelerationStructureManager::WindStreamResource(UINT slot) const
{
    return slot < kMaxWindBlasSlots ? windSlots_[slot].deformedVb.Get() : nullptr;
}

ID3D12Resource* AccelerationStructureManager::WindBlasResource(UINT slot) const
{
    return slot < kMaxWindBlasSlots ? windSlots_[slot].blas.Get() : nullptr;
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
    const UINT prevCount = f.instanceCount; // this slot's last build (refit needs an unchanged set)
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
        buildFailed_ = true;
        return;
    }

    D3D12_RAYTRACING_INSTANCE_DESC* descs = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(f.instanceUpload->Map(0, &noRead, reinterpret_cast<void**>(&descs)))) {
        buildFailed_ = true;
        return;
    }
    // Authored object lists commonly keep repeated meshes adjacent (wind_test has 610 palms).
    // Avoid repeating the Debug-build hash-table lookup for the same cached BLAS.
    Mesh* lastMesh = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS lastBlasAddress = 0;
    bool lastBlasResolved = false;
    for (UINT i = 0; i < count; ++i) {
        const InstanceEntry& e = instances[i];
        D3D12_RAYTRACING_INSTANCE_DESC d{};
        // world is row-major / row-vector (p*M). DXR's 3x4 Transform is
        // column-vector (M*p) with translation in column 3, i.e. the transpose
        // of the upper 3x4: Transform[r][c] = world.m[c][r].
        const DirectX::XMFLOAT4X4& w = e.world;
        // Spell out the fixed-size transpose: in an unoptimized Debug build the nested loops
        // otherwise contribute noticeable overhead across hundreds of TLAS instances.
        d.Transform[0][0] = w.m[0][0]; d.Transform[0][1] = w.m[1][0];
        d.Transform[0][2] = w.m[2][0]; d.Transform[0][3] = w.m[3][0];
        d.Transform[1][0] = w.m[0][1]; d.Transform[1][1] = w.m[1][1];
        d.Transform[1][2] = w.m[2][1]; d.Transform[1][3] = w.m[3][1];
        d.Transform[2][0] = w.m[0][2]; d.Transform[2][1] = w.m[1][2];
        d.Transform[2][2] = w.m[2][2]; d.Transform[2][3] = w.m[3][2];
        d.InstanceID = e.instanceId & 0xFFFFFFu;
        d.InstanceMask = 0xFF;
        d.InstanceContributionToHitGroupIndex = e.instanceId & 0xFFFFFFu;
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        if (e.blasOverride != 0) {
            // RW: this instance's wind-deformed BLAS, already built earlier in this command list.
            d.AccelerationStructure = e.blasOverride;
        } else {
            if (!lastBlasResolved || e.mesh != lastMesh) {
                lastMesh = e.mesh;
                lastBlasAddress = GetOrBuildBlas(e.mesh, e.nonOpaqueSlots, cmdList4).Address();
                lastBlasResolved = true;
            }
            d.AccelerationStructure = lastBlasAddress;
        }
        if (d.AccelerationStructure == 0) {
            buildFailed_ = true; // a referenced BLAS failed to build (alloc/device) — disable RT
        }
        descs[i] = d;
    }
    f.instanceUpload->Unmap(0, nullptr);

    // 2) Prebuild info -> (re)allocate result + scratch (grow on demand; the
    //    per-frame slot is reused only once its prior GPU work has completed).
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    // ALLOW_UPDATE so the built TLAS can be refit in place next frame (see the PERFORM_UPDATE path
    // below). It also drives the prebuild sizes (a touch larger result; scratch covers the update).
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
                   D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    inputs.NumDescs = count;
    inputs.InstanceDescs = f.instanceUpload->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0) {
        return;
    }

    bool freshResult = false;
    if (f.resultSize < info.ResultDataMaxSizeInBytes || !f.result) {
        f.result = CreateUavBuffer(device5_, info.ResultDataMaxSizeInBytes,
                                   D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        f.resultSize = f.result ? info.ResultDataMaxSizeInBytes : 0;
        freshResult = true; // a new buffer holds no prior AS — must full-build, can't refit
    }
    if (f.scratchSize < info.ScratchDataSizeInBytes || !f.scratch) {
        // COMMON: avoids the buffer-initial-state-ignored warning; the build's
        // UAV write implicitly promotes it to UNORDERED_ACCESS.
        f.scratch = CreateUavBuffer(device5_, info.ScratchDataSizeInBytes,
                                    D3D12_RESOURCE_STATE_COMMON);
        f.scratchSize = f.scratch ? info.ScratchDataSizeInBytes : 0;
    }
    if (!f.result || !f.scratch) {
        buildFailed_ = true; // out of VRAM / device lost -> caller disables RT, falls back to SSR
        return;
    }

    // 3) Build (full) or refit (in-place PERFORM_UPDATE) + UAV barrier. Refit when the instance set
    //    is unchanged (same count into an existing updatable result) and we haven't drifted too far
    //    since the last full rebuild. The instanced casters only rotate, so this is the common path.
    constexpr UINT kMaxRefits = 64u; // bound BVH-quality drift; full-rebuild this slot afterwards
    const bool canRefit = !freshResult && f.canUpdate && (count == prevCount) &&
                          (f.refitsSinceBuild < kMaxRefits);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs;
    if (canRefit) {
        build.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        build.SourceAccelerationStructureData = f.result->GetGPUVirtualAddress(); // in-place source == dest
        ++f.refitsSinceBuild;
    } else {
        f.refitsSinceBuild = 0;
    }
    build.DestAccelerationStructureData = f.result->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = f.scratch->GetGPUVirtualAddress();
    cmdList4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    f.canUpdate = true; // built with ALLOW_UPDATE -> refit-eligible next time this slot is used

    // Step 14, as in GetOrBuildBlas: the TLAS result's reader is the inline-RayQuery compute
    // shader, and the barrier now says RTAS rather than UAV.
    barriers::EmitAccelerationStructureBuildBarrier(cmdList4, f.result.Get());

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
    for (WindBlasSlot& s : windSlots_) {
        s = WindBlasSlot{};
    }
    windDescHeap_.Reset();
    windDescIncrement_ = 0;
    for (PerFrameTlas& f : tlasFrames_) {
        f = PerFrameTlas{};
    }
    srvHeap_.Reset();
    srvIncrement_ = 0;
    buildFailed_ = false; // a fresh scene gets a fresh chance at RT
}

uint64_t AccelerationStructureManager::GetAsMemoryBytes() const
{
    uint64_t total = 0;
    for (const auto& kv : blasCache_) {
        if (kv.second.result) { total += kv.second.result->GetDesc().Width; }
    }
    for (const PerFrameTlas& f : tlasFrames_) {
        if (f.result) { total += f.result->GetDesc().Width; }
        if (f.scratch) { total += f.scratch->GetDesc().Width; }
        if (f.instanceUpload) { total += f.instanceUpload->GetDesc().Width; }
    }
    for (const auto& s : pendingScratch_) {
        if (s) { total += s->GetDesc().Width; }
    }
    for (const WindBlasSlot& s : windSlots_) {
        if (s.deformedVb) { total += s.deformedVb->GetDesc().Width; }
        if (s.blas) { total += s.blas->GetDesc().Width; }
        if (s.scratch) { total += s.scratch->GetDesc().Width; }
    }
    return total;
}

void AccelerationStructureManager::SetForceAllocFailureForTest(bool enable)
{
    g_forceAsAllocFailure.store(enable, std::memory_order_relaxed);
}

} // namespace rt
