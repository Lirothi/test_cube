#include "rendering/renderables/ShadowGpuData.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/math/Frustum.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/ComputeDispatch.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/IInstanceable.h"

// ---- Ring: upload-heap kFrameCount-region structured buffer ----------------

std::uint8_t* ShadowGpuData::Ring::Region(UINT f) const
{
    if (!mapped || f >= render::kFrameCount) { return nullptr; }
    return mapped + static_cast<size_t>(f) * capacity * stride;
}

D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::Ring::Srv(UINT f) const
{
    if (f >= render::kFrameCount) { return {}; }
    return srvHandles[f];
}

void ShadowGpuData::ReleaseRing(Renderer* renderer, Ring& ring)
{
    if (ring.buffer)
    {
        if (renderer) { renderer->ClearResourceState(ring.buffer.Get()); }
        ring.buffer->Unmap(0, nullptr);
        ring.buffer.Reset();
    }
    ring.mapped = nullptr;
    ring.capacity = 0;
    ring.stride = 0;
    ring.srvHeap.Reset();
    ring.srvHandles.fill({});
}

bool ShadowGpuData::EnsureRing(Renderer* renderer, Ring& ring, size_t elements, UINT stride,
                               const wchar_t* name)
{
    if (!renderer || !renderer->GetDevice() || stride == 0)
    {
        return false;
    }

    if (ring.buffer && ring.mapped && ring.srvHandles[0].ptr != 0 &&
        ring.capacity >= elements && ring.stride == stride)
    {
        return true; // existing allocation fits — reuse (no realloc)
    }

    ReleaseRing(renderer, ring);

    const size_t newCapacity = std::max<size_t>(elements, 1);
    // Ring buffer: render::kFrameCount contiguous regions of newCapacity elements.
    const size_t totalElements = newCapacity * render::kFrameCount;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(totalElements) * stride;
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
        srvDesc.Buffer.StructureByteStride = stride;
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        D3D12_CPU_DESCRIPTOR_HANDLE h{ srvBase.ptr + static_cast<SIZE_T>(f) * srvIncr };
        renderer->GetDevice()->CreateShaderResourceView(buffer.Get(), &srvDesc, h);
        ring.srvHandles[f] = h;
    }

    renderer->SetResourceState(buffer.Get(), D3D12_RESOURCE_STATE_GENERIC_READ);
    buffer->SetName(name);

    ring.capacity = newCapacity;
    ring.stride = stride;
    ring.buffer = buffer;
    ring.mapped = static_cast<std::uint8_t*>(mapped);
    ring.srvHeap = srvHeap;
    return true;
}

// ---- UavRing: default-heap UAV kFrameCount-region buffer -------------------

void ShadowGpuData::ReleaseUavRing(Renderer* renderer, UavRing& ring)
{
    if (ring.buffer)
    {
        if (renderer) { renderer->ClearResourceState(ring.buffer.Get()); }
        ring.buffer.Reset();
    }
    ring.regionBytes = 0;
}

bool ShadowGpuData::EnsureUavRing(Renderer* renderer, UavRing& ring, size_t regionBytes,
                                  const wchar_t* name)
{
    if (!renderer || !renderer->GetDevice())
    {
        return false;
    }

    regionBytes = std::max<size_t>(regionBytes, 16); // never a zero-size region
    if (ring.buffer && ring.regionBytes >= regionBytes)
    {
        return true; // existing allocation fits — reuse (consumers index via regionBytes getter)
    }

    ReleaseUavRing(renderer, ring);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(regionBytes) * render::kFrameCount;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    HRESULT hr = renderer->GetDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(buffer.GetAddressOf()));
    if (FAILED(hr) || !buffer)
    {
        return false;
    }

    renderer->SetResourceState(buffer.Get(), D3D12_RESOURCE_STATE_COMMON);
    buffer->SetName(name);
    ring.buffer = buffer;
    ring.regionBytes = regionBytes;
    return true;
}

// ---- Caster filtering + fills ----------------------------------------------

bool ShadowGpuData::IsCaster(const RenderableObjectBase* obj)
{
    // Mirror the CastsShadow() filter shadowCasterSource_.Bucketize uses. NOT filtered by
    // IsVisible()/layer mask: the caster id must be STABLE across frames, and per-view
    // visibility is the job of the GPU cull pass (Step 4), not of this buffer. Requires a
    // mesh: a mesh-less "caster" draws nothing and has no mesh-group to slot into. GPU-instanced
    // casters are excluded (one object → many GPU-side instances can't be a single entry); they
    // keep drawing via their own RenderShadow (Step 6 draws them alongside the indirect path).
    if (!obj || !obj->CastsShadow() || obj->IsGpuInstancedCaster()) { return false; }
    const RenderableObject* ro = obj->AsRenderableObject();
    return ro && ro->GetMesh() != nullptr;
}

void ShadowGpuData::FillInstance(const RenderableObjectBase* obj, render::InstancePerObject& out)
{
    std::memset(&out, 0, sizeof(out));
    if (!obj) { return; }

    // Instanceable casters carry the full per-instance payload; use it so the entry matches
    // the CPU-marshalled path byte-for-byte.
    if (const IInstanceable* inst = obj->AsInstanceable())
    {
        inst->FillInstanceData(out);
        return;
    }

    // Other casters: shadows are depth-only, so only world/prevWorld matter.
    if (const RenderableObject* ro = obj->AsRenderableObject())
    {
        out.world = ro->GetModelMatrix().m;
        out.prevWorld = ro->GetPreviousModelMatrix().m;
        const std::uint64_t id = obj->GetEditorObjectId();
        out.objectId = id > 0xffffffffull ? 0xffffffffu : static_cast<std::uint32_t>(id);
    }
}

void ShadowGpuData::FillBounds(const RenderableObjectBase* obj, render::CasterBounds& out)
{
    std::memset(&out, 0, sizeof(out));
    if (!obj) { return; }
    const RenderableObject* ro = obj->AsRenderableObject();
    if (!ro) { return; }

    const AABB& b = ro->GetWorldBounds();
    if (!b.IsValid()) { return; } // degenerate/absent → zeroed (cull treats as a point)

    const Math::float3 c = b.GetCenter();
    const Math::float3 e = b.GetHalfExtents();
    out.center = DirectX::XMFLOAT4(c.x, c.y, c.z, b.GetRadius());
    out.halfExtents = DirectX::XMFLOAT4(e.x, e.y, e.z, 0.0f);
}

// ---- Public API ------------------------------------------------------------

D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::InstanceSrv(UINT frameIndex) const { return instances_.Srv(frameIndex); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::BoundsSrv(UINT frameIndex) const { return bounds_.Srv(frameIndex); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::ViewFrustumSrv(UINT frameIndex) const { return viewFrustums_.Srv(frameIndex); }

D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::UnifiedInstanceSrv(UINT f) const
{
    return (f < render::kFrameCount) ? unifiedSrv_[f] : D3D12_CPU_DESCRIPTOR_HANDLE{};
}
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::UnifiedBoundsSrv(UINT f) const
{
    return (f < render::kFrameCount) ? unifiedSrv_[render::kFrameCount + f] : D3D12_CPU_DESCRIPTOR_HANDLE{};
}
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::InstanceReadSrv(UINT f) const
{
    const D3D12_CPU_DESCRIPTOR_HANDLE u = UnifiedInstanceSrv(f);
    return u.ptr != 0 ? u : instances_.Srv(f);
}
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::BoundsReadSrv(UINT f) const
{
    const D3D12_CPU_DESCRIPTOR_HANDLE u = UnifiedBoundsSrv(f);
    return u.ptr != 0 ? u : bounds_.Srv(f);
}

void ShadowGpuData::Rebuild(Renderer* renderer,
    const std::vector<std::unique_ptr<RenderableObjectBase>>& objects)
{
    if (!renderer) { return; }

    size_t casterCount = 0;
    for (const auto& obj : objects)
    {
        if (IsCaster(obj.get())) { ++casterCount; }
    }

    if (!EnsureRing(renderer, instances_, casterCount, sizeof(render::InstancePerObject), L"ShadowGpuData.Instances") ||
        !EnsureRing(renderer, bounds_, casterCount, sizeof(render::CasterBounds), L"ShadowGpuData.Bounds"))
    {
        count_ = 0;
        cpuInstances_.clear();
        cpuBounds_.clear();
        pending_.clear();
        return;
    }

    count_ = static_cast<std::uint32_t>(casterCount);
    cpuInstances_.assign(casterCount, render::InstancePerObject{});
    cpuBounds_.assign(casterCount, render::CasterBounds{});
    pending_.assign(casterCount, 0);

    // Fill per-caster data + assign mesh-groups (the cull groups draws by mesh, so the indirect
    // arg/count buffers are sized per (view, mesh-group)). Group ids are dense, first-seen order.
    std::unordered_map<const Mesh*, std::uint32_t> meshToGroup;
    std::vector<const Mesh*> casterMesh(casterCount, nullptr);
    size_t idx = 0;
    for (const auto& objPtr : objects)
    {
        const RenderableObjectBase* obj = objPtr.get();
        if (!IsCaster(obj)) { continue; }
        FillInstance(obj, cpuInstances_[idx]);
        FillBounds(obj, cpuBounds_[idx]);
        const RenderableObject* ro = obj->AsRenderableObject();
        const Mesh* mesh = ro ? ro->GetMesh() : nullptr; // non-null: IsCaster requires it
        casterMesh[idx] = mesh;
        if (mesh && meshToGroup.find(mesh) == meshToGroup.end())
        {
            meshToGroup.emplace(mesh, static_cast<std::uint32_t>(meshToGroup.size()));
        }
        ++idx;
    }
    numMeshGroups_ = static_cast<std::uint32_t>(meshToGroup.size());

    // Reverse map (group id -> Mesh*) for the Step 6 indirect draws' per-group VB/IB binding.
    groupMesh_.assign(numMeshGroups_, nullptr);
    for (const auto& kv : meshToGroup)
    {
        if (kv.second < numMeshGroups_) { groupMesh_[kv.second] = kv.first; }
    }

    // Per-group caster count + index count, and per-caster group id.
    std::vector<std::uint32_t> groupCount(numMeshGroups_, 0);
    std::vector<std::uint32_t> groupIndexCount(numMeshGroups_, 0);
    std::vector<std::uint32_t> casterGroupId(casterCount, 0);
    for (size_t i = 0; i < casterCount; ++i)
    {
        const Mesh* mesh = casterMesh[i];
        const std::uint32_t g = mesh ? meshToGroup[mesh] : 0u;
        casterGroupId[i] = g;
        if (g < numMeshGroups_)
        {
            ++groupCount[g];
            if (mesh) { groupIndexCount[g] = static_cast<std::uint32_t>(mesh->GetIndexCount()); }
        }
    }
    // Prefix-sum group caster counts -> each group's base slice offset within a view's region
    // (sum of counts == casterCount, so a group's visible run never overflows its slice).
    std::vector<std::uint32_t> groupBase(numMeshGroups_, 0);
    for (std::uint32_t g = 1; g < numMeshGroups_; ++g)
    {
        groupBase[g] = groupBase[g - 1] + groupCount[g - 1];
    }

    // Upload the static cull inputs (region 0; never rewritten after Rebuild).
    if (EnsureRing(renderer, casterGroup_, casterCount, sizeof(std::uint32_t), L"ShadowGpuData.CasterGroup") &&
        casterCount > 0)
    {
        std::memcpy(casterGroup_.Region(0), casterGroupId.data(), casterCount * sizeof(std::uint32_t));
    }
    if (EnsureRing(renderer, perGroup_, std::max<size_t>(numMeshGroups_, 1), 2 * sizeof(std::uint32_t), L"ShadowGpuData.PerGroup") &&
        numMeshGroups_ > 0)
    {
        auto* pg = reinterpret_cast<std::uint32_t*>(perGroup_.Region(0));
        for (std::uint32_t g = 0; g < numMeshGroups_; ++g)
        {
            pg[g * 2 + 0] = groupBase[g];
            pg[g * 2 + 1] = groupIndexCount[g];
        }
    }

    // Rung 2 mega-buffer layout: concatenate all group meshes into one VB + one IB so the VSM
    // per-page render binds geometry once + issues a single ExecuteIndirect(maxCount=groups) per
    // page. Only when every group shares a vertex stride + R32 index format (the MeshManager/PNTUV
    // case); else megaWanted_ stays false and the VSM path keeps its per-group binding. The GPU copy
    // itself is deferred to EnsureMegaBuffer (needs a command list). Offsets are in vertices/indices.
    // Don't free megaVB_/megaIB_ here: Rebuild can run mid-game (editor spawn/delete, not GPU-idle),
    // and a freed buffer might still be referenced by an in-flight frame. Clearing megaReady_ makes
    // RecordPageRender fall back to per-group binding; EnsureMegaBuffer (GPU-idle level load only)
    // frees + reallocates. So a mid-game caster change just drops the mega opt until the next load.
    megaWanted_ = megaBuilt_ = megaReady_ = false;
    megaVBBytes_ = megaIBBytes_ = megaStride_ = 0;
    megaIndexFormat_ = DXGI_FORMAT_R32_UINT;
    baseVertex_.assign(numMeshGroups_, 0u);
    startIndex_.assign(numMeshGroups_, 0u);
    groupVBBytes_.assign(numMeshGroups_, 0u);
    groupIBBytes_.assign(numMeshGroups_, 0u);
    constexpr std::uint32_t kMaxMegaGroups = 64u; // matches VSM_MAX_SETUP_GROUPS in vsm_page_setup_cs.hlsl
    if (numMeshGroups_ > 0 && numMeshGroups_ <= kMaxMegaGroups)
    {
        bool uniform = true;
        UINT stride0 = 0;
        DXGI_FORMAT fmt0 = DXGI_FORMAT_UNKNOWN;
        for (std::uint32_t g = 0; g < numMeshGroups_; ++g)
        {
            const Mesh* m = groupMesh_[g];
            ID3D12Resource* vb = m ? m->GetVertexBufferResource() : nullptr;
            ID3D12Resource* ib = m ? m->GetIndexBufferResource() : nullptr;
            if (!vb || !ib) { uniform = false; break; }
            if (g == 0) { stride0 = m->GetVertexStride(); fmt0 = m->GetIndexFormat(); }
            else if (m->GetVertexStride() != stride0 || m->GetIndexFormat() != fmt0) { uniform = false; break; }
        }
        if (uniform && stride0 > 0 && fmt0 == DXGI_FORMAT_R32_UINT) // consolidated path is R32-only
        {
            const UINT idxBytes = 4u;
            UINT voff = 0, ioff = 0;
            for (std::uint32_t g = 0; g < numMeshGroups_; ++g)
            {
                const Mesh* m = groupMesh_[g];
                const UINT vbw = static_cast<UINT>(m->GetVertexBufferResource()->GetDesc().Width);
                const UINT ibw = static_cast<UINT>(m->GetIndexBufferResource()->GetDesc().Width);
                baseVertex_[g] = voff / stride0;   // exact: widths are stride*count
                startIndex_[g] = ioff / idxBytes;  // exact: widths are 4*count
                groupVBBytes_[g] = vbw;
                groupIBBytes_[g] = ibw;
                voff += vbw; ioff += ibw;
            }
            megaVBBytes_ = voff; megaIBBytes_ = ioff;
            megaStride_ = stride0; megaIndexFormat_ = fmt0;
            megaWanted_ = (megaVBBytes_ > 0 && megaIBBytes_ > 0);
        }
    }

    // Prime ALL ring regions — after this a static scene re-uploads nothing.
    if (casterCount > 0)
    {
        for (UINT f = 0; f < render::kFrameCount; ++f)
        {
            std::memcpy(instances_.Region(f), cpuInstances_.data(), casterCount * sizeof(render::InstancePerObject));
            std::memcpy(bounds_.Region(f), cpuBounds_.data(), casterCount * sizeof(render::CasterBounds));
        }
    }

    // Step 3: allocate the GPU-driven indirect-execution buffers, sized per (view, mesh-group).
    // Worst case: every caster visible in every view (visible list) and one draw per
    // (view, mesh-group) (args), one draw count per view. Still unused (no descriptors).
    const size_t numViews = render::kMaxShadowViews;
    const size_t groups = std::max<size_t>(numMeshGroups_, 1);
    const size_t casters = std::max<size_t>(casterCount, 1);
    EnsureUavRing(renderer, indirectArgs_, numViews * groups * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), L"ShadowGpuData.IndirectArgs");
    EnsureUavRing(renderer, visibleList_, numViews * casters * sizeof(std::uint32_t), L"ShadowGpuData.VisibleList");
    EnsureUavRing(renderer, indirectCounts_, numViews * sizeof(std::uint32_t), L"ShadowGpuData.IndirectCounts");
    // Step 2 (GI→VSM): DEFAULT-heap mirrors of instances_/bounds_, sized to `casters` per region.
    // RecordCull copies the ring's region into these each frame (verbatim at this step; Step 4 also
    // scatters GI casters into them). Their per-region SRVs feed the cull (bounds) + indirect VS (t0).
    EnsureUavRing(renderer, instancesUnified_, casters * sizeof(render::InstancePerObject), L"ShadowGpuData.InstancesUnified");
    EnsureUavRing(renderer, boundsUnified_, casters * sizeof(render::CasterBounds), L"ShadowGpuData.BoundsUnified");
    // Instantiate the shared DRAW_INDEXED indirect command signature so it exists post-load.
    renderer->GetDrawIndexedCommandSignature();
    // Step 4: per-region UAVs for the cull outputs (depend on the just-decided sizes).
    RebuildCullDescriptors(renderer);
    RebuildUnifiedDescriptors(renderer); // Step 2: per-region SRVs onto the unified buffers
    valState_ = 0; // re-validate after a caster-set change

    char buf[224];
    std::snprintf(buf, sizeof(buf),
        "[ShadowGpuData] rebuilt: %u casters, %u mesh-groups; %.2f KB instance + %.2f KB bounds x%u regions.\n",
        count_, numMeshGroups_,
        (instances_.capacity * sizeof(render::InstancePerObject)) / 1024.0,
        (bounds_.capacity * sizeof(render::CasterBounds)) / 1024.0,
        render::kFrameCount);
    OutputDebugStringA(buf);
    logFramesRemaining_ = 5;
}

std::uint32_t ShadowGpuData::UpdateForFrame(Renderer* renderer,
    const std::vector<std::unique_ptr<RenderableObjectBase>>& objects)
{
    if (!renderer) { return 0; }

    size_t newCount = 0;
    for (const auto& obj : objects)
    {
        if (IsCaster(obj.get())) { ++newCount; }
    }

    // First use, or the caster set changed (level switch / editor spawn-delete) → full
    // rebuild. Safe in Steps 1-2: the buffers are unused, so a reallocation strands no reader.
    if (!instances_.Valid() || !bounds_.Valid() || newCount != count_ ||
        newCount > instances_.capacity || newCount > bounds_.capacity)
    {
        Rebuild(renderer, objects);
        lastMoverCount_ = count_; // full rebuild -> treat everything as changed (don't skip VSM)
        return count_;
    }

    const UINT region = renderer->GetCurrentFrameIndex();
    if (region >= render::kFrameCount) { return 0; }
    auto* instBase = reinterpret_cast<render::InstancePerObject*>(instances_.Region(region));
    auto* boundBase = reinterpret_cast<render::CasterBounds*>(bounds_.Region(region));

    std::uint32_t idx = 0;
    std::uint32_t uploaded = 0;
    for (const auto& objPtr : objects)
    {
        const RenderableObjectBase* obj = objPtr.get();
        if (!IsCaster(obj)) { continue; }

        // Step 7: only movers are recomputed + re-uploaded. A static caster is skipped entirely
        // (no fill, no compare) — the per-frame cost is O(movers), not O(casters). The move
        // signal tracks transform changes, which is all a depth-only shadow entry depends on.
        const RenderableObject* ro = obj->AsRenderableObject();
        if (ro && ro->MovedThisFrame())
        {
            FillInstance(obj, cpuInstances_[idx]);
            FillBounds(obj, cpuBounds_[idx]);
            pending_[idx] = static_cast<std::uint8_t>(render::kFrameCount);
        }
        // pending>0 propagates a recent change across all kFrameCount ring regions (even after
        // the object stops moving), so every region converges to the latest transform.
        if (pending_[idx] > 0)
        {
            instBase[idx] = cpuInstances_[idx];
            boundBase[idx] = cpuBounds_[idx];
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
            "[ShadowGpuData] frame update: %u/%u casters re-uploaded.\n", uploaded, count_);
        OutputDebugStringA(buf);
    }

    lastMoverCount_ = uploaded;
    return uploaded;
}

void ShadowGpuData::UpdateViewFrustums(Renderer* renderer, const Frustum* const* frustums, size_t count)
{
    if (!renderer) { return; }
    if (count == 0 || !frustums)
    {
        viewFrustumCount_ = 0;
        return;
    }

    if (!EnsureRing(renderer, viewFrustums_, count, sizeof(render::ShadowViewFrustum), L"ShadowGpuData.ViewFrustums"))
    {
        viewFrustumCount_ = 0;
        return;
    }
    viewFrustumCount_ = static_cast<std::uint32_t>(count);

    // Build into a CPU mirror (kept for Step 4 validation), then upload to this frame's region.
    // The frustum buffer is fully rewritten each frame (views move every frame). An inactive/
    // invalid slot gets a reject-all sentinel plane (0,0,0,-1): signedDist=-1, projRadius=0 ->
    // culled, so the cull emits zero for that slot (matches a cleared shadow view).
    cpuViewFrustums_.assign(count, render::ShadowViewFrustum{});
    for (size_t i = 0; i < count; ++i)
    {
        render::ShadowViewFrustum vf{};
        const Frustum* f = frustums[i];
        if (f && f->IsValid())
        {
            const Math::float4* p = f->Planes();
            for (int j = 0; j < 6; ++j)
            {
                vf.planes[j] = DirectX::XMFLOAT4(p[j].x, p[j].y, p[j].z, p[j].w);
            }
        }
        else
        {
            vf.planes[0] = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, -1.0f);
        }
        cpuViewFrustums_[i] = vf;
    }

    const UINT region = renderer->GetCurrentFrameIndex();
    if (region < render::kFrameCount)
    {
        auto* base = reinterpret_cast<render::ShadowViewFrustum*>(viewFrustums_.Region(region));
        std::memcpy(base, cpuViewFrustums_.data(), count * sizeof(render::ShadowViewFrustum));
    }
}

// ---- Step 4: GPU cull dispatch + validation --------------------------------

void ShadowGpuData::EnsureShaderResources(Renderer* renderer)
{
    if (shaderResourcesTried_) { return; }
    shaderResourcesTried_ = true;
    if (!renderer || !renderer->GetMaterialManager()) { return; }

    auto* mm = renderer->GetMaterialManager();
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/shadow_cull_clear_cs.hlsl";
        cd.csEntry = "CSMain";
        cullClearMat_ = mm->GetOrCreateCompute(renderer, cd);
    }
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/shadow_cull_cs.hlsl";
        cd.csEntry = "CSMain";
        cullMat_ = mm->GetOrCreateCompute(renderer, cd);
    }
    // Step 5: the indirect depth-only shadow PSO. Depth-only (numRT 0, D16, LESS_EQUAL) mirroring
    // ConfigureShadowPipeline; the PosOnly_InstCasterId layout binds the mesh vertex stream (slot
    // 0) + the visible-list caster-id stream (slot 1, per-instance). Compiled now; drawn in Step 6.
    {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/shadow_indirect_csm.hlsl";
        gd.vsEntry = "VSMain";
        gd.psEntry = "PSMain";
        gd.inputLayoutKey = "PosOnly_InstCasterId";
        gd.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gd.numRT = 0;
        gd.dsvFormat = DXGI_FORMAT_D16_UNORM;
        gd.depth.DepthEnable = TRUE;
        gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        gd.depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        gd.raster.CullMode = D3D12_CULL_MODE_BACK;
        gd.blend.RenderTarget[0].BlendEnable = FALSE;
        indirectShadowMat_ = mm->GetOrCreateGraphics(renderer, gd);
    }

    const bool cullOk = cullClearMat_ && cullClearMat_->GetPipelineState() &&
                        cullMat_ && cullMat_->GetPipelineState();
    const bool drawOk = indirectShadowMat_ && indirectShadowMat_->GetPipelineState();
    if (!cullOk)
    {
        OutputDebugStringA("[ShadowGpuData] cull compute PSO creation FAILED (shader compile?).\n");
        cullClearMat_.reset();
        cullMat_.reset();
    }
    if (!drawOk)
    {
        OutputDebugStringA("[ShadowGpuData] indirect shadow PSO creation FAILED (shader compile?).\n");
        indirectShadowMat_.reset();
    }
    if (cullOk && drawOk)
    {
        OutputDebugStringA("[ShadowGpuData] shaders ready: cull (clear+cull) + indirect-shadow PSOs created.\n");
    }
}

void ShadowGpuData::RebuildCullDescriptors(Renderer* renderer)
{
    cullUav_.fill({});
    cullUavHeap_.Reset();
    if (!renderer || !renderer->GetDevice()) { return; }
    if (!indirectArgs_.Valid() || !visibleList_.Valid() || !indirectCounts_.Valid()) { return; }

    ID3D12Device* dev = renderer->GetDevice();
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 3 * render::kFrameCount;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(cullUavHeap_.GetAddressOf()))) || !cullUavHeap_)
    {
        cullUavHeap_.Reset();
        return;
    }
    const UINT incr = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE base = cullUavHeap_->GetCPUDescriptorHandleForHeapStart();
    auto slotHandle = [&](UINT slot) { D3D12_CPU_DESCRIPTOR_HANDLE h{ base.ptr + static_cast<SIZE_T>(slot) * incr }; return h; };

    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        // args: RAW UAV covering region f (byte-addressable for InterlockedAdd + Store).
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Buffer.FirstElement = static_cast<UINT64>(f) * (indirectArgs_.regionBytes / 4);
            ud.Buffer.NumElements = static_cast<UINT>(indirectArgs_.regionBytes / 4);
            ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            const D3D12_CPU_DESCRIPTOR_HANDLE h = slotHandle(f);
            dev->CreateUnorderedAccessView(indirectArgs_.buffer.Get(), nullptr, &ud, h);
            cullUav_[f] = h;
        }
        // visibleList: structured uint UAV, region f.
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Buffer.FirstElement = static_cast<UINT64>(f) * (visibleList_.regionBytes / 4);
            ud.Buffer.NumElements = static_cast<UINT>(visibleList_.regionBytes / 4);
            ud.Buffer.StructureByteStride = sizeof(std::uint32_t);
            const D3D12_CPU_DESCRIPTOR_HANDLE h = slotHandle(render::kFrameCount + f);
            dev->CreateUnorderedAccessView(visibleList_.buffer.Get(), nullptr, &ud, h);
            cullUav_[render::kFrameCount + f] = h;
        }
        // counts: structured uint UAV, region f.
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Buffer.FirstElement = static_cast<UINT64>(f) * (indirectCounts_.regionBytes / 4);
            ud.Buffer.NumElements = static_cast<UINT>(indirectCounts_.regionBytes / 4);
            ud.Buffer.StructureByteStride = sizeof(std::uint32_t);
            const D3D12_CPU_DESCRIPTOR_HANDLE h = slotHandle(2 * render::kFrameCount + f);
            dev->CreateUnorderedAccessView(indirectCounts_.buffer.Get(), nullptr, &ud, h);
            cullUav_[2 * render::kFrameCount + f] = h;
        }
    }
}

void ShadowGpuData::RebuildUnifiedDescriptors(Renderer* renderer)
{
    unifiedSrv_.fill({});
    unifiedSrvHeap_.Reset();
    if (!renderer || !renderer->GetDevice()) { return; }
    if (!instancesUnified_.Valid() || !boundsUnified_.Valid() || count_ == 0) { return; }

    ID3D12Device* dev = renderer->GetDevice();
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 2 * render::kFrameCount; // [0..k)=instances, [k..2k)=bounds
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(unifiedSrvHeap_.GetAddressOf()))) || !unifiedSrvHeap_)
    {
        unifiedSrvHeap_.Reset();
        return;
    }
    const UINT incr = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE base = unifiedSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    auto slotHandle = [&](UINT slot) { D3D12_CPU_DESCRIPTOR_HANDLE h{ base.ptr + static_cast<SIZE_T>(slot) * incr }; return h; };

    // Region f is spaced by the buffer's physical regionBytes (>= count_*stride when the allocation
    // was reused from a larger prior level); NumElements is the live count_.
    const UINT instStride = static_cast<UINT>(sizeof(render::InstancePerObject));
    const UINT boundStride = static_cast<UINT>(sizeof(render::CasterBounds));
    const UINT64 instElemsPerRegion = instancesUnified_.regionBytes / instStride;
    const UINT64 boundElemsPerRegion = boundsUnified_.regionBytes / boundStride;

    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_UNKNOWN;
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Buffer.FirstElement = static_cast<UINT64>(f) * instElemsPerRegion;
            sd.Buffer.NumElements = count_;
            sd.Buffer.StructureByteStride = instStride;
            const D3D12_CPU_DESCRIPTOR_HANDLE h = slotHandle(f);
            dev->CreateShaderResourceView(instancesUnified_.buffer.Get(), &sd, h);
            unifiedSrv_[f] = h;
        }
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_UNKNOWN;
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Buffer.FirstElement = static_cast<UINT64>(f) * boundElemsPerRegion;
            sd.Buffer.NumElements = count_;
            sd.Buffer.StructureByteStride = boundStride;
            const D3D12_CPU_DESCRIPTOR_HANDLE h = slotHandle(render::kFrameCount + f);
            dev->CreateShaderResourceView(boundsUnified_.buffer.Get(), &sd, h);
            unifiedSrv_[render::kFrameCount + f] = h;
        }
    }
}

void ShadowGpuData::EnsureReadback(Renderer* renderer, size_t bytes)
{
    if (!renderer || !renderer->GetDevice() || bytes == 0) { return; }
    if (valReadback_ && valReadback_->GetDesc().Width >= bytes) { return; }
    valReadback_.Reset();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    renderer->GetDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(valReadback_.GetAddressOf()));
}

void ShadowGpuData::RecordCull(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer || !cl) { return; }
    EnsureShaderResources(renderer);
    if (!cullClearMat_ || !cullMat_) { return; }
    if (count_ == 0 || numMeshGroups_ == 0) { return; }
    if (!indirectArgs_.Valid() || !visibleList_.Valid() || !indirectCounts_.Valid()) { return; }
    if (!bounds_.Valid() || !viewFrustums_.Valid() || !casterGroup_.Valid() || !perGroup_.Valid()) { return; }
    if (!cullUavHeap_) { return; }

    const UINT f = renderer->GetCurrentFrameIndex();
    if (f >= render::kFrameCount) { return; }

    const std::uint32_t numViews = render::kMaxShadowViews;
    const std::uint32_t numGroups = numMeshGroups_;
    const std::uint32_t numCasters = count_;

    // The cull outputs must be UNORDERED_ACCESS before the dispatches (created COMMON, or left
    // COPY_SOURCE by a prior validation frame). This pass declares no states -> transition here.
    renderer->Transition(cl, indirectArgs_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, visibleList_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, indirectCounts_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Step 2 (GI→VSM): before the cull, mirror this frame's per-caster ring region into the
    // DEFAULT-heap unified buffers so the cull (bounds) + indirect VS (t0 instances) read a
    // GPU-local copy a compute shader can also write. At this step it's a verbatim copy of
    // [0,count_) (no GI casters yet), so the cull/VS results are byte-identical to reading the
    // upload ring — a pure no-op refactor. The upload rings stay in GENERIC_READ (which includes
    // COPY_SOURCE) so no source barrier is needed. Left in NON_PIXEL_SHADER_RESOURCE for the cull's
    // compute SRV read and the parallel shadow passes' VS t0 read (both non-pixel shader stages).
    const bool useUnified = instancesUnified_.Valid() && boundsUnified_.Valid() && unifiedSrvHeap_ &&
                            UnifiedInstanceSrv(f).ptr != 0 && UnifiedBoundsSrv(f).ptr != 0;
    if (useUnified)
    {
        renderer->Transition(cl, instancesUnified_.buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        renderer->Transition(cl, boundsUnified_.buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        const UINT64 instSrc = static_cast<UINT64>(f) * instances_.capacity * instances_.stride;
        const UINT64 boundSrc = static_cast<UINT64>(f) * bounds_.capacity * bounds_.stride;
        cl->CopyBufferRegion(instancesUnified_.buffer.Get(), static_cast<UINT64>(f) * instancesUnified_.regionBytes,
                             instances_.buffer.Get(), instSrc,
                             static_cast<UINT64>(numCasters) * sizeof(render::InstancePerObject));
        cl->CopyBufferRegion(boundsUnified_.buffer.Get(), static_cast<UINT64>(f) * boundsUnified_.regionBytes,
                             bounds_.buffer.Get(), boundSrc,
                             static_cast<UINT64>(numCasters) * sizeof(render::CasterBounds));
        renderer->Transition(cl, instancesUnified_.buffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(cl, boundsUnified_.buffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    struct CullCB { std::uint32_t numCasters, numViews, numGroups, pad; };
    auto writeCB = [&](std::uint8_t* dst) { CullCB c{ numCasters, numViews, numGroups, 0u }; std::memcpy(dst, &c, sizeof(c)); };
    const UINT cbSize = static_cast<UINT>(sizeof(CullCB));
    const D3D12_GPU_DESCRIPTOR_HANDLE noSampler{};

    // Clear/init the per-(view, mesh-group) indirect args + per-view draw counts.
    RecordComputeDispatch(renderer, cl, cullClearMat_.get(), cbSize, writeCB,
        { perGroup_.Srv(0) },
        { cullUav_[f], cullUav_[2 * render::kFrameCount + f] },
        noSampler,
        numViews * numGroups, 1,
        indirectArgs_.buffer.Get()); // UAV barrier: args init visible to the cull's InterlockedAdd

    // Cull: frustum-test every caster into the visible list + InstanceCounts. Reads bounds from the
    // unified buffer (Step 2) when built, else the upload ring (fallback).
    RecordComputeDispatch(renderer, cl, cullMat_.get(), cbSize, writeCB,
        { BoundsReadSrv(f), viewFrustums_.Srv(f), casterGroup_.Srv(0), perGroup_.Srv(0) },
        { cullUav_[f], cullUav_[render::kFrameCount + f] },
        noSampler,
        numCasters, 1,
        indirectArgs_.buffer.Get());

    // Step 4 validation (temporary, one-shot after warmup): snapshot the inputs + read back
    // this region's args to compare against a CPU cull kFrameCount frames later.
    if (valState_ == 0 && renderer->GetTotalFrameNumber() > render::kFrameCount)
    {
        valBounds_ = cpuBounds_;
        valFrustums_ = cpuViewFrustums_;
        valCasters_ = numCasters;
        valViews_ = numViews;
        valGroups_ = numGroups;
        EnsureReadback(renderer, indirectArgs_.regionBytes);
        if (valReadback_)
        {
            renderer->Transition(cl, indirectArgs_.buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            cl->CopyBufferRegion(valReadback_.Get(), 0, indirectArgs_.buffer.Get(),
                                 static_cast<UINT64>(f) * indirectArgs_.regionBytes, indirectArgs_.regionBytes);
            valFrame_ = renderer->GetTotalFrameNumber();
            valState_ = 1;
        }
    }

    // Step 6: leave the args in INDIRECT_ARGUMENT and the visible list as a vertex buffer so the
    // shadow passes (chained after this pass) can ExecuteIndirect + bind the per-instance stream
    // without touching state on their parallel CLs. Done every frame (harmless when the toggle is
    // off — nothing reads them); next frame's start transitions them back to UAV. Counts stays
    // UAV (Step 6 uses maxCount=1 per draw, so no count buffer is read).
    renderer->Transition(cl, indirectArgs_.buffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    renderer->Transition(cl, visibleList_.buffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
}

bool ShadowGpuData::IndirectDrawReady() const
{
    return indirectShadowMat_ && indirectShadowMat_->GetPipelineState() &&
           count_ > 0 && numMeshGroups_ > 0 && !groupMesh_.empty() &&
           indirectArgs_.Valid() && visibleList_.Valid() && instances_.Valid();
}

bool ShadowGpuData::RecordIndirectShadowDraws(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                              std::uint32_t viewSlot, D3D12_GPU_VIRTUAL_ADDRESS viewCB)
{
    if (!renderer || !cl || !IndirectDrawReady()) { return false; }
    const UINT f = renderer->GetCurrentFrameIndex();
    if (f >= render::kFrameCount) { return false; }
    ID3D12CommandSignature* sig = renderer->GetDrawIndexedCommandSignature();
    if (!sig) { return false; }

    // Bind the depth-only indirect PSO + root args: b1 = light viewProj, t0 = instance buffer
    // SRV for this frame's region.
    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    RenderContext& ctx = ctxHandle.ref();
    ctx.cbv[1] = viewCB;
    ctx.srvTable[0] = renderer->StageSrvUavTable({ InstanceReadSrv(f) }).gpu; // unified copy (Step 2), else ring
    indirectShadowMat_->Bind(cl, ctx, false);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Slot 1 = this frame's visible-list region as a per-instance stream; each draw's
    // StartInstanceLocation (baked by the cull) offsets into it.
    D3D12_VERTEX_BUFFER_VIEW visVBV{};
    visVBV.BufferLocation = visibleList_.buffer->GetGPUVirtualAddress() +
                            static_cast<UINT64>(f) * visibleList_.regionBytes;
    visVBV.SizeInBytes = static_cast<UINT>(visibleList_.regionBytes);
    visVBV.StrideInBytes = sizeof(std::uint32_t);
    cl->IASetVertexBuffers(1, 1, &visVBV);

    const UINT64 argRegionBase = static_cast<UINT64>(f) * indirectArgs_.regionBytes;
    for (std::uint32_t g = 0; g < numMeshGroups_; ++g)
    {
        const Mesh* mesh = (g < groupMesh_.size()) ? groupMesh_[g] : nullptr;
        if (!mesh) { continue; }
        ID3D12Resource* vb = mesh->GetVertexBufferResource();
        ID3D12Resource* ib = mesh->GetIndexBufferResource();
        if (!vb || !ib) { continue; }

        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = vb->GetGPUVirtualAddress();
        vbv.SizeInBytes = static_cast<UINT>(vb->GetDesc().Width);
        vbv.StrideInBytes = mesh->GetVertexStride();
        cl->IASetVertexBuffers(0, 1, &vbv);

        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = ib->GetGPUVirtualAddress();
        ibv.SizeInBytes = static_cast<UINT>(ib->GetDesc().Width);
        ibv.Format = mesh->GetIndexFormat();
        cl->IASetIndexBuffer(&ibv);

        // One indirect draw per (view, mesh-group). InstanceCount (from the cull) may be 0 -> a
        // free no-op, so empty groups cost nothing beyond the binding.
        const UINT64 argOffset = argRegionBase +
            static_cast<UINT64>(viewSlot * numMeshGroups_ + g) * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        renderer->ExecuteIndirect(cl, sig, 1, indirectArgs_.buffer.Get(), argOffset, nullptr, 0);
    }
    return true;
}

void ShadowGpuData::EnsureMegaBuffer(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer || !renderer->GetDevice() || !cl || megaBuilt_ || !megaWanted_) { return; }
    megaBuilt_ = true; // one-shot: never re-attempt (a failed alloc falls back to per-group binding)

    auto makeBuf = [&](UINT bytes, Microsoft::WRL::ComPtr<ID3D12Resource>& out, const wchar_t* name) -> bool
    {
        out.Reset(); // free any prior-level mega buffer (safe: EnsureMegaBuffer runs GPU-idle at load)
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        HRESULT hr = renderer->GetDevice()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(out.GetAddressOf()));
        if (FAILED(hr) || !out) { return false; }
        renderer->SetResourceState(out.Get(), D3D12_RESOURCE_STATE_COMMON);
        out->SetName(name);
        return true;
    };
    if (!makeBuf(megaVBBytes_, megaVB_, L"ShadowGpuData.MegaVB") ||
        !makeBuf(megaIBBytes_, megaIB_, L"ShadowGpuData.MegaIB"))
    {
        megaVB_.Reset(); megaIB_.Reset();
        return; // fall back to per-group binding
    }

    // Concatenate each group's mesh VB/IB into the mega buffers. This runs on the GPU-idle
    // level-load CL, where the mesh buffers (created in COMMON) + the freshly-created mega buffers
    // are all in COMMON, so the copies rely on implicit COMMON->COPY_SOURCE / COMMON->COPY_DEST
    // promotion — no barriers, and robust whether a mesh is fresh this load or a cached reuse. The
    // load CL's execute+fence (before any frame renders) is the write->read sync; the mega buffers
    // decay back to COMMON and the per-page draw's IA bind promotes them to VERTEX_/INDEX_BUFFER.
    UINT voff = 0, ioff = 0;
    for (std::uint32_t g = 0; g < numMeshGroups_; ++g)
    {
        const Mesh* m = groupMesh_[g];
        ID3D12Resource* vb = m ? m->GetVertexBufferResource() : nullptr;
        ID3D12Resource* ib = m ? m->GetIndexBufferResource() : nullptr;
        if (vb && ib)
        {
            cl->CopyBufferRegion(megaVB_.Get(), voff, vb, 0, groupVBBytes_[g]);
            cl->CopyBufferRegion(megaIB_.Get(), ioff, ib, 0, groupIBBytes_[g]);
        }
        voff += groupVBBytes_[g];
        ioff += groupIBBytes_[g];
    }
    megaReady_ = true;
}

void ShadowGpuData::PollValidation(Renderer* renderer)
{
    if (valState_ != 1 || !renderer || !valReadback_) { return; }
    // Wait until the fence for the readback frame has surely passed (BeginFrame waits on the
    // slot's fence kFrameCount frames later), so the copy has completed on the GPU.
    if (renderer->GetTotalFrameNumber() < valFrame_ + render::kFrameCount) { return; }

    const size_t argBytes = static_cast<size_t>(valViews_) * valGroups_ * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    D3D12_RANGE readRange{ 0, argBytes };
    void* mapped = nullptr;
    if (FAILED(valReadback_->Map(0, &readRange, &mapped)) || !mapped)
    {
        valState_ = 2;
        return;
    }
    const auto* args = reinterpret_cast<const D3D12_DRAW_INDEXED_ARGUMENTS*>(mapped);

    std::vector<std::uint32_t> gpuTotal(valViews_, 0);
    for (std::uint32_t v = 0; v < valViews_; ++v)
    {
        for (std::uint32_t g = 0; g < valGroups_; ++g)
        {
            gpuTotal[v] += args[v * valGroups_ + g].InstanceCount;
        }
    }
    const D3D12_RANGE noWrite{ 0, 0 };
    valReadback_->Unmap(0, &noWrite);

    // CPU per-view totals from the snapshot (same positive-vertex test as shadow_cull_cs.hlsl).
    std::uint32_t mismatchViews = 0, firstView = 0, firstCpu = 0, firstGpu = 0;
    for (std::uint32_t v = 0; v < valViews_; ++v)
    {
        const render::ShadowViewFrustum& vf = valFrustums_[v];
        std::uint32_t cpu = 0;
        for (std::uint32_t c = 0; c < valCasters_; ++c)
        {
            const render::CasterBounds& b = valBounds_[c];
            bool visible = true;
            for (int i = 0; i < 6; ++i)
            {
                const DirectX::XMFLOAT4& p = vf.planes[i];
                const float signedDist = p.x * b.center.x + p.y * b.center.y + p.z * b.center.z + p.w;
                const float projRadius = std::abs(p.x) * b.halfExtents.x + std::abs(p.y) * b.halfExtents.y + std::abs(p.z) * b.halfExtents.z;
                if (signedDist + projRadius < 0.0f) { visible = false; break; }
            }
            if (visible) { ++cpu; }
        }
        if (cpu != gpuTotal[v])
        {
            if (mismatchViews == 0) { firstView = v; firstCpu = cpu; firstGpu = gpuTotal[v]; }
            ++mismatchViews;
        }
    }

    char buf[256];
    if (mismatchViews == 0)
    {
        std::snprintf(buf, sizeof(buf),
            "[ShadowGpuData] cull validation PASS: %u views match CPU (%u casters, %u groups).\n",
            valViews_, valCasters_, valGroups_);
    }
    else
    {
        std::snprintf(buf, sizeof(buf),
            "[ShadowGpuData] cull validation MISMATCH: %u/%u views differ (first view %u: cpu=%u gpu=%u).\n",
            mismatchViews, valViews_, firstView, firstCpu, firstGpu);
    }
    OutputDebugStringA(buf);
    valState_ = 2;
}

void ShadowGpuData::Reset()
{
    // Retain the GPU buffers + SRV descriptors across a level unload (a pass may reference an
    // SRV while frames are in flight). Only CPU-side state is dropped; the next Rebuild reuses
    // the allocations when their capacity still suffices.
    count_ = 0;
    viewFrustumCount_ = 0;
    numMeshGroups_ = 0;
    cpuInstances_.clear();
    cpuBounds_.clear();
    pending_.clear();
    cpuViewFrustums_.clear();
    groupMesh_.clear();
    megaWanted_ = megaReady_ = false; // groupMesh_ gone; next Rebuild frees + rebuilds the mega buffers
    valState_ = 0;
    logFramesRemaining_ = 5;
}
