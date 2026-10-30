#include "rendering/shadows/ShadowGpuData.h"
#include "core/logging/Log.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/profiling/Profiler.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/math/Frustum.h"
#include "materials/MaterialData.h" // C2: per-slot alphaMask/alphaCutoff/albedo for masked shadows
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderGraph.h" // PrepareCullPass takes a RenderGraphPassContext&
#include "rendering/core/ComputeDispatch.h"
#include "rendering/core/CommandListBindState.h" // A6: the merged draws bypass the bind cache
#include "rendering/core/RasterSettings.h"       // A6: raster.bindless gates the merged path
#include "rendering/core/GraphicsDevice.h"       // GbvEnabled: GBV takes the per-group path (id=1044)
#include "rendering/core/GpuAlloc.h"
#include <chrono>                                // A6: the throttled readout
#include "rendering/meshes/Mesh.h"
#include "rendering/meshes/LodSelect.h" // render::g_shadowLodBias + ShadowTierBaseLod (per-view LOD)
#include "rendering/shadows/VirtualShadowMap.h" // vsm::kNumCascades / kNumClipmapLevels (view tiers)
#include "rendering/lighting/LightManager.h"
#include "rendering/renderables/GBufferRenderable.h" // C2: per-slot MaterialData accessor
#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/IInstanceable.h"
#include "rendering/renderables/InstanceTypes.h" // Tessy N5: MaterialSurfaceParamsGpu by mesh group
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/shadows/TessySettings.h" // Tessy N4: tessy::g_atlas
#include "vfx/WindState.h" // W8: g_windFadeStart/End (distance-fade mover re-check)

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
        (void)renderer; // the wrapper unregisters itself
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
    HRESULT hr = gpu::CreateCommittedResource(renderer->GetDevice(),
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        MemTag::ShadowGpuData, name,
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
    hr = gpu::CreateDescriptorHeap(renderer->GetDevice(), &heapDesc, MemTag::ShadowGpuData, L"ShadowGpuData.RingSrvHeap", IID_PPV_ARGS(srvHeap.GetAddressOf()));
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

    // Upload-heap rings never leave GENERIC_READ, so creation and resting state match.

    ring.capacity = newCapacity;
    ring.stride = stride;
    ring.buffer.Attach(renderer->Declarations(), buffer, D3D12_RESOURCE_STATE_GENERIC_READ, name);
    ring.mapped = static_cast<std::uint8_t*>(mapped);
    ring.srvHeap = srvHeap;
    return true;
}

// ---- UavRing: default-heap UAV kFrameCount-region buffer -------------------

void ShadowGpuData::ReleaseUavRing(Renderer* renderer, UavRing& ring)
{
    if (ring.buffer)
    {
        (void)renderer; // the wrapper unregisters itself
        ring.buffer.Reset();
    }
    ring.regionBytes = 0;
}

bool ShadowGpuData::EnsureUavRing(Renderer* renderer, UavRing& ring, size_t regionBytes,
                                  D3D12_RESOURCE_STATES canonical,
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
    HRESULT hr = gpu::CreateCommittedResource(renderer->GetDevice(),
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        MemTag::ShadowGpuData, name,
        IID_PPV_ARGS(buffer.GetAddressOf()));
    if (FAILED(hr) || !buffer)
    {
        return false;
    }

    ring.buffer.Attach(renderer->Declarations(), buffer,
        D3D12_RESOURCE_STATE_COMMON, canonical, name);
    ring.regionBytes = regionBytes;
    return true;
}

// shadow_cull_cs.hlsl's CullParams (the cull-clear reads its first three words). S4: camView is the camera's slot in
// the frustum ring (the last one, past the shadow views), or ~0 when the camera row is off. S7: localSplit -- the cull
// splits the local views' static casters into their own rows. S9e: the clipmap slots (the views past the cascades that
// are NOT local), the first GI caster (exempt from the radius threshold) and that threshold: the camera's position and
// (r.Shadow.RadiusThreshold x LODDistanceFactor)^2, 0 = off.
// The unified instance buffer's read (and resting) state: the shadow / G-buffer vertex shaders and the culls read it, and
// the indirect G-buffer's pixel shader its material values (gbuffer_indirect.hlsl, 2026-10-23).
constexpr D3D12_RESOURCE_STATES kInstancesReadState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// render::g_localShadowWindRange: windRigid -- one bit per view slot, set for a local view whose light is past the range
// (its wind-only casters go to its static rows).
struct ShadowCullCB
{
    std::uint32_t numCasters = 0, numViews = 0, numGroups = 0, camView = 0xFFFFFFFFu;
    std::uint32_t localSplit = 0, clipFirst = render::ShadowViewClipmapSlot(0u),
                  clipEnd = render::ShadowViewClipmapSlot(render::kShadowViewClipmapLevels), giFirst = 0;
    float camPos[3] = { 0.0f, 0.0f, 0.0f };
    float radiusThresholdSq = 0.0f;
    ShadowGpuData::ViewMask windRigid{};
};
static_assert(sizeof(ShadowCullCB) == 48 + sizeof(ShadowGpuData::ViewMask),
              "ShadowCullCB mirrors shadow_cull_cs.hlsl's CullParams");

// ---- Caster filtering + fills ----------------------------------------------

// B3: a caster object registers one caster SLOT per submesh of its mesh — the cull/args/draw
// pipeline works on (mesh, submesh-range) groups so the depth passes issue ranged draws (what
// lets Part C bind per-slot masked materials). Single-submesh meshes (every OBJ/.txt mesh)
// keep the exact pre-B3 one-slot layout.
static size_t CasterSlots(const RenderableObjectBase* obj)
{
    const RenderableObject* ro = obj ? obj->AsRenderableObject() : nullptr;
    const Mesh* mesh = ro ? ro->GetMesh() : nullptr;
    const size_t n = mesh ? mesh->GetSubmeshCount() : 1u;
    return n > 0 ? n : 1u;
}

bool ShadowGpuData::IsCaster(const RenderableObjectBase* obj)
{
    // Match SceneRenderQueue's visibility predicate. A visibility change changes the count seen by
    // UpdateForFrame, which rebuilds this cache before VSM or indirect draws consume it. In no-editor
    // builds ordinary level objects remain visible. GPU-instanced casters are handled separately by
    // IsGiFoldable below.
    // A mesh with Cast Shadow OFF is in the table too: the table is also the camera's indirect G-buffer, and a mesh
    // left out of it falls back to the CPU G-buffer draw (grass with its shadow switched off would cost CPU recording).
    // Its bounds carry the verdict instead (halfExtents.w = 1, FillBounds), which every shadow consumer tests --
    // shadow_cull_cs's shadow views, vsm_page_scatter_cs, the setup's brute-force fallback, the cull validator.
    // Only the switch: a type that never casts (the ocean) stays out -- its mesh would make the merged caster VB/IB
    // heterogeneous and turn VSM's single-draw page render off (measured: the per-page loop, 45 ms CPU on wind_test).
    if (!obj || !obj->IsVisible() || obj->IsGpuInstancedCaster()) { return false; }
    const RenderableObject* ro = obj->AsRenderableObject();
    if (!ro || ro->GetMesh() == nullptr) { return false; }
    return obj->CastsShadow() || ro->ShadowSwitchedOff();
}

bool ShadowGpuData::IsGiFoldable(const RenderableObjectBase* obj)
{
    // A GPU-instanced caster we can fold into the consolidated caster set (Step 3): it casts
    // shadow, exposes a per-instance transform SRV + non-zero instance count, and has a mesh.
    if (!obj || !obj->IsVisible() || !obj->CastsShadow() || !obj->IsGpuInstancedCaster()) { return false; }
    if (obj->GetInstanceCasterSrv().ptr == 0 || obj->GetInstanceCasterCount() == 0) { return false; }
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

std::uint32_t ShadowGpuData::MaterialSlotOf(const Mesh* mesh, size_t ordinal, size_t slotCount)
{
    // GBufferRenderable::Render's clamp: submesh -> its materialSlot, the last slot past the end.
    const std::vector<Mesh::Submesh>* subs = mesh ? &mesh->GetSubmeshes() : nullptr;
    std::uint32_t slot = (subs && ordinal < subs->size()) ? (*subs)[ordinal].materialSlot : 0u;
    if (slotCount > 0 && slot >= slotCount) { slot = static_cast<std::uint32_t>(slotCount - 1); }
    return slot;
}

void ShadowGpuData::FillInstanceSlot(const RenderableObjectBase* obj, size_t ordinal, render::InstancePerObject& out)
{
    const GBufferRenderable* gb = obj ? obj->AsGBufferRenderable() : nullptr;
    const RenderableObject* ro = obj ? obj->AsRenderableObject() : nullptr;
    if (gb && ro)
    {
        std::memset(&out, 0, sizeof(out));
        gb->FillInstanceDataForSlot(out, MaterialSlotOf(ro->GetMesh(), ordinal, gb->SlotCount()));
        return;
    }
    FillInstance(obj, out);
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

    // DELIBERATELY not padded for the wind sway. The shadow VS does displace a swaying caster's
    // vertices beyond this static AABB (by up to ~4.7x the authored sway amplitude at the worst-case
    // gust/grove/bend stack), so in principle the per-page / per-view cull can clip a leaning frond
    // at a page edge. In practice that artifact was never observed in months of use — while the W5
    // worst-case pad (~3m/axis on wind_test) measured a permanent 2.5x on Pass_VsmPageRender GPU
    // (+1.4 ms) via caster-page coverage. Decision + numbers: docs/bug_shadow_lod_bias_perf.md.
    // If popping IS ever seen, reintroduce the pad DIRECTIONALLY (downwind XZ + small Y), not as the
    // old all-axis worst case — and bake it at level load too, or the pad hysteresis bug returns.
    out.center = DirectX::XMFLOAT4(c.x, c.y, c.z, b.GetRadius());
    // w = 1: Cast Shadow off -- in the table for the camera only (IsCaster); no shadow view takes it.
    out.halfExtents = DirectX::XMFLOAT4(e.x, e.y, e.z, obj->CastsShadow() ? 0.0f : 1.0f);
}

// Terrain chunking: bounds for ONE slot of a chunked mesh (Mesh::IsChunkedSubmeshes). Its submeshes
// tile one surface, so each gets its own world box instead of the object's — which is the whole
// point of chunking: a shadow page then rasterizes only the tiles that reach it, instead of the
// island's full LOD range once per resident page.
//
// Same conservative 8-corner transform the object bounds use (AABB::Transform), so a chunk box and
// the object box are produced by ONE piece of math; a slot with no valid box falls back to the
// object's (conservative, merely not tight).
static void FillChunkBounds(const RenderableObject* ro, size_t slot,
    const render::CasterBounds& objectBounds, render::CasterBounds& out)
{
    const Mesh* mesh = ro ? ro->GetMesh() : nullptr;
    const std::vector<AABB>* boxes = mesh ? &mesh->GetSubmeshBounds() : nullptr;
    if (!boxes || slot >= boxes->size() || !(*boxes)[slot].IsValid())
    {
        out = objectBounds;
        return;
    }
    const AABB w = (*boxes)[slot].Transform(ro->GetModelMatrix());
    if (!w.IsValid()) { out = objectBounds; return; }
    const Math::float3 c = w.GetCenter();
    const Math::float3 e = w.GetHalfExtents();
    out.center = DirectX::XMFLOAT4(c.x, c.y, c.z, w.GetRadius());
    out.halfExtents = DirectX::XMFLOAT4(e.x, e.y, e.z, objectBounds.halfExtents.w); // the object's Cast Shadow verdict
}

// ---- Public API ------------------------------------------------------------

D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::InstanceSrv(UINT frameIndex) const { return instances_.Srv(frameIndex); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::BoundsSrv(UINT frameIndex) const { return bounds_.Srv(frameIndex); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::ViewFrustumSrv(UINT frameIndex) const { return viewFrustums_.Srv(frameIndex); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::CasterGroupSrv() const { return casterGroup_.Srv(0); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::CasterMetaSrv(UINT frameIndex) const { return casterMeta_.Srv(frameIndex); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::PerGroupSrv() const { return perGroup_.Srv(0); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::GroupLodMegaSrv() const { return groupLodMegaBuf_.Srv(0); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::GroupLodOverrideSrv(UINT f) const { return groupLodOverrideBuf_.Srv(f); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::PerGroupVgSrv(UINT f) const { return perGroupVg_.Srv(f); }

D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::UnifiedInstanceSrv(UINT f) const
{
    return (f < render::kFrameCount) ? unifiedDescr_[f] : D3D12_CPU_DESCRIPTOR_HANDLE{};
}
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::UnifiedBoundsSrv(UINT f) const
{
    return (f < render::kFrameCount) ? unifiedDescr_[render::kFrameCount + f] : D3D12_CPU_DESCRIPTOR_HANDLE{};
}
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::UnifiedInstanceUav(UINT f) const
{
    return (f < render::kFrameCount) ? unifiedDescr_[2 * render::kFrameCount + f] : D3D12_CPU_DESCRIPTOR_HANDLE{};
}
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::UnifiedBoundsUav(UINT f) const
{
    return (f < render::kFrameCount) ? unifiedDescr_[3 * render::kFrameCount + f] : D3D12_CPU_DESCRIPTOR_HANDLE{};
}

// C2: the masked variant serves the whole caster set whenever any group is alpha-masked (its PS
// early-outs for opaque groups); falls back to the null-PS PSO otherwise or when it failed.
Material* ShadowGpuData::IndirectShadowMaterial() const
{
    return MaskedShadowsActive() ? indirectShadowMaskedMat_.get() : indirectShadowMat_.get();
}

// S15: SDSM draws with the LEGACY atlas PSOs -- its per-partition projection arrives as the b1
// root CBV like everyone else's, from a buffer the analyze pass wrote (SdsmShadows::ViewCbAddress).
// There is no SDSM permutation to be ready or not, so the gate is the plain indirect one.
bool ShadowGpuData::SdsmDrawReady() const
{
    return IndirectDrawReady();
}

// Same masked-selection rule, pool-format (D32) twins — the VSM per-page loop draws into the pool.
Material* ShadowGpuData::IndirectShadowPoolMaterial() const
{
    return MaskedShadowsActive() ? indirectShadowPoolMaskedMat_.get() : indirectShadowPoolMat_.get();
}

// Per-caster shadow LOD (Unreal's per-primitive rule, transcribed): every caster SLOT gets the LOD
// its RECEIVER draws this frame, and the VSM scatter buckets each instance into a virtual draw
// group by it. Runs every frame; no rebuild. Two transports are refreshed here:
//   casterLod_          one byte per caster slot (bit 7 = chunk EXACT) -> vsm_page_scatter_cs
//   groupLodOverride_   per-group chunk EXACT only -> the setup shader's BRUTE-FORCE fallback and
//                       nothing else (per-group is representable there because a chunk group has
//                       exactly one instance)
//
// The receiver LOD is computed FRESH from the camera rather than read from cameraLod_: SelectLod
// only runs over VISIBLE buckets, so an off-screen instance's stored LOD is stale — and a stale
// value here is not cosmetic. The tent repro (2026-08-25): both tents boot at LOD0, the far one
// gets its real coarse LOD the first time the camera sweeps across it, and from then on any
// per-GROUP answer was poisoned for the near tent. Fresh per-instance math has no such state.
void ShadowGpuData::RefreshCasterLods(Renderer* renderer,
    const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
    const Math::float3& cameraPos, bool cameraOcclusionValid, std::uint64_t frameNumber)
{
    lastCameraPos_ = DirectX::XMFLOAT3(cameraPos.x, cameraPos.y, cameraPos.z);
    groupLodOverride_.assign(std::max<size_t>(numMeshGroups_, 1), -1);
    // count_ includes the GI tail. The default is EXACT LOD0, not plain 0: a GI group only has
    // LOD0 geometry registered (its GroupLodMega rows past lod 0 are empty), so letting the
    // scatter's max(lod, viewLod) push a GI instance into a lod>0 bucket would drop it from
    // every far clipmap. The static walk below overwrites its own slots.
    casterLod_.assign(std::max<std::uint32_t>(count_, 1u), render::kCasterLodExactBit);

    // Walk objects EXACTLY like Rebuild does — the caster slot cursor is the mapping. UpdateForFrame
    // rebuilds on any caster-count change before this runs, so the orders agree; the bounds guard
    // below is for the one frame a mismatch could slip through, not a supported state.
    std::optional<Profiler::ScopedCpu> walkScope(std::in_place, ProfilerScopes::kRefreshCasterLodsWalk);
    std::uint32_t idx = 0;
    for (const auto& objPtr : objects)
    {
        const RenderableObjectBase* obj = objPtr.get();
        if (!IsCaster(obj)) { continue; }
        const std::uint32_t slots = static_cast<std::uint32_t>(CasterSlots(obj));
        if (idx + slots > casterLod_.size()) { break; }
        const RenderableObject* ro = obj->AsRenderableObject();
        const Mesh* mesh = ro ? ro->GetMesh() : nullptr;
        if (!mesh) { idx += slots; continue; }

        if (mesh->IsChunkedSubmeshes())
        {
            const std::vector<std::uint8_t>& lods = ro->ChunkCameraLods();
            const auto it = meshFirstGroup_.find(mesh);
            for (std::uint32_t s = 0; s < slots; ++s)
            {
                const unsigned int lod =
                    mesh->ClampExplicitLod(s < lods.size() ? lods[s] : 0u);
                casterLod_[idx + s] = lod | render::kCasterLodExactBit;
                if (it != meshFirstGroup_.end() && it->second + s < groupLodOverride_.size())
                {
                    groupLodOverride_[it->second + s] = static_cast<std::int32_t>(lod);
                }
            }
        }
        else
        {
            const unsigned int lod = mesh->ClampExplicitLod(
                render::EffectiveDrawLod(ro->ComputeReceiverLodTier(cameraPos)));
            for (std::uint32_t s = 0; s < slots; ++s) { casterLod_[idx + s] = lod; }
        }
        idx += slots;
    }
    walkScope.reset();
    // S4: this frame's camera flags + crossfade per caster -- zero on a frame the indirect
    // G-buffer does not run, so the cull emits nothing for the camera. Same object walk, same
    // cursor as the LOD table above.
    std::optional<Profiler::ScopedCpu> cameraScope(std::in_place, ProfilerScopes::kRefreshCasterLodsCamera);
    casterCamCpu_.assign(casterLod_.size(), 0u);
    casterFadeCpu_.assign(casterLod_.size(), 0.0f);
    if (gbufferIndirectFrame_)
    {
        std::uint32_t c = 0;
        for (const auto& objPtr : objects)
        {
            const RenderableObjectBase* obj = objPtr.get();
            if (!IsCaster(obj)) { continue; }
            const std::uint32_t slots = static_cast<std::uint32_t>(CasterSlots(obj));
            if (c + slots > casterCamCpu_.size()) { break; }
            const RenderableObject* ro = obj->AsRenderableObject();
            const Mesh* mesh = ro ? ro->GetMesh() : nullptr;
            // Candidates: eligible AND kept by the camera's CPU frustum cull this frame. The
            // rest carry stale verdicts and would only cost an empty ExecuteIndirect per group
            // (measured: the wall camera at K=4 issued ~all 448 groups for 7 visible objects).
            if (obj->GBufferIndirect() && mesh && obj->CameraVisibleAt(frameNumber))
            {
                const bool chunked = mesh->IsChunkedSubmeshes();
                const bool occluded = cameraOcclusionValid && obj->CameraOccluded();
                const std::vector<std::uint8_t>& mask = ro->ChunkCameraVisible();
                // The CPU path's crossfade rule (RenderableObject::Render): never for a chunked
                // mesh, only when a next tier exists. Buckets stop at kMaxShadowLods.
                const unsigned int lod = casterLod_[c] & render::kCasterLodMask;
                const float fadeWeight = ro->GetCameraLodFade();
                const bool fading = !chunked && fadeWeight > 0.0f && (lod + 1u) < mesh->GetLodCount() &&
                                    (lod + 1u) < render::kMaxShadowLods;
                for (std::uint32_t s = 0; s < slots; ++s)
                {
                    // A chunk's mask is its frustum AND occlusion verdict (S1 + S3a); a whole
                    // object takes the history's verdict.
                    const bool skip = chunked ? (s < mask.size() && mask[s] == 0u) : occluded;
                    casterCamCpu_[c + s] = 1u | (skip ? 2u : 0u);
                    casterFadeCpu_[c + s] = fading ? fadeWeight : 0.0f;
                }
            }
            c += slots;
        }
    }
    // Change detection for the page cache: any LOD move invalidates cached pages (they hold
    // the old geometry). Rebuild resizes the table, which reads as a change — correct, a new
    // caster set must flush too.
    cameraScope.reset();
    std::optional<Profiler::ScopedCpu> uploadScope(std::in_place, ProfilerScopes::kRefreshCasterLodsUpload);
    if (casterLodPrev_ != casterLod_)
    {
        casterLodsChanged_ = true;
        casterLodPrev_ = casterLod_;
    }
    UploadCasterLods(renderer);
    uploadScope.reset();
    CPU_SCOPE(ProfilerScopes::kRefreshCasterLodsGroups);
    RefreshVirtualGroups(renderer);
}

// S3.6: bucket every caster into its VIRTUAL group (real group * kMaxShadowLods + receiver LOD) and
// publish this frame's per-bucket visible-list bases. Done on the CPU because both inputs already
// live here, and because the bucket is VIEW-INDEPENDENT: the caster LOD comes from the receiver, so
// unlike the VSM scatter there is nothing per-view to decide. The prefix sum still totals count_,
// which is why the visible list does NOT grow -- each caster occupies exactly one slot per view.
void ShadowGpuData::RefreshVirtualGroups(Renderer* renderer)
{
    if (!renderer || numVirtualGroups_ == 0) { return; }
    const UINT f = renderer->GetCurrentFrameIndex();
    if (f >= render::kFrameCount) { return; }

    vgCasterCount_.assign(numVirtualGroups_, 0u);
    const std::uint32_t n = std::min<std::uint32_t>(count_,
        static_cast<std::uint32_t>(std::min(casterGroupCpu_.size(), casterLod_.size())));
    for (std::uint32_t c = 0; c < n; ++c)
    {
        const std::uint32_t g = casterGroupCpu_[c];
        if (g >= numMeshGroups_) { continue; }
        const std::uint32_t lod = casterLod_[c] & render::kCasterLodMask;
        const std::uint32_t vg = g * render::kMaxShadowLods +
            (lod < render::kMaxShadowLods ? lod : render::kMaxShadowLods - 1u);
        ++vgCasterCount_[vg];
    }

    if (!EnsureRing(renderer, perGroupVg_, numVirtualGroups_, 4 * sizeof(std::uint32_t),
                    L"ShadowGpuData.PerGroupVg"))
    {
        return;
    }
    auto* dst = reinterpret_cast<std::uint32_t*>(perGroupVg_.Region(f));
    if (!dst) { return; }
    std::uint32_t base = 0;
    for (std::uint32_t vg = 0; vg < numVirtualGroups_; ++vg)
    {
        // Ranges are STATIC per (group, lod) -- groupLodMega_ already carries them, mesh-IB-relative:
        // [.1] = start within that LOD's own index buffer, [.2] = index count.
        const size_t o = static_cast<size_t>(vg) * 4u;
        const std::uint32_t count = (o + 2 < groupLodMega_.size()) ? groupLodMega_[o + 2] : 0u;
        const std::uint32_t start = (o + 1 < groupLodMega_.size()) ? groupLodMega_[o + 1] : 0u;
        dst[vg * 4 + 0] = base;
        dst[vg * 4 + 1] = count;
        dst[vg * 4 + 2] = start;
        dst[vg * 4 + 3] = vgCasterCount_[vg];
        base += vgCasterCount_[vg];
    }

    // S4: the camera's buckets -- eligible, unskipped casters; a crossfading one in its own tier
    // AND the next (two visible-list slots). Its own base table, so the shadow views' slices
    // (which the VSM per-page cull also reads) never learn about the second slot.
    vgCamCount_.assign(numVirtualGroups_, 0u);
    camListCount_ = 0;
    if (!gbufferIndirectFrame_) { return; }
    const std::uint32_t nc = std::min<std::uint32_t>(n, static_cast<std::uint32_t>(std::min(casterCamCpu_.size(), casterFadeCpu_.size())));
    for (std::uint32_t c = 0; c < nc; ++c)
    {
        if ((casterCamCpu_[c] & 3u) != 1u) { continue; }
        const std::uint32_t g = casterGroupCpu_[c];
        if (g >= numMeshGroups_) { continue; }
        const std::uint32_t lod = casterLod_[c] & render::kCasterLodMask;
        const std::uint32_t vg = g * render::kMaxShadowLods +
            (lod < render::kMaxShadowLods ? lod : render::kMaxShadowLods - 1u);
        ++vgCamCount_[vg];
        ++camListCount_;
        if (casterFadeCpu_[c] > 0.0f && (lod + 1u) < render::kMaxShadowLods)
        {
            ++vgCamCount_[vg + 1u];
            ++camListCount_;
        }
    }
    if (!EnsureRing(renderer, perGroupVgCam_, numVirtualGroups_, 4 * sizeof(std::uint32_t),
                    L"ShadowGpuData.PerGroupVgCam"))
    {
        return;
    }
    auto* cam = reinterpret_cast<std::uint32_t*>(perGroupVgCam_.Region(f));
    if (!cam) { return; }
    std::uint32_t camBase = 0;
    for (std::uint32_t vg = 0; vg < numVirtualGroups_; ++vg)
    {
        // The ranges from groupLodMega_ again, never read back from `dst`: it is write-combined upload memory, and
        // reading it back cost ~0.13 ms a frame on wind_test.
        const size_t o = static_cast<size_t>(vg) * 4u;
        cam[vg * 4 + 0] = camBase;
        cam[vg * 4 + 1] = (o + 2 < groupLodMega_.size()) ? groupLodMega_[o + 2] : 0u;
        cam[vg * 4 + 2] = (o + 1 < groupLodMega_.size()) ? groupLodMega_[o + 1] : 0u;
        cam[vg * 4 + 3] = vgCamCount_[vg];
        camBase += vgCamCount_[vg];
    }
}

// The override table's home is a PER-FRAME ring region: it is rewritten every frame, so writing it
// into region f is the same WAR discipline the instance/bounds rings use. Sized by numMeshGroups_,
// which is what took the group cap off this table.
void ShadowGpuData::CollectLocalMovables()
{
    localMovables_.clear();
    localMovableGi_ = count_ > staticCount_;
    const std::uint32_t n = std::min<std::uint32_t>(staticCount_, static_cast<std::uint32_t>(cpuBounds_.size()));
    for (std::uint32_t i = 0; i < n; ++i)
    {
        // UploadCasterLods' "movable" without the wind: IsDynamicCaster, or moved within kFrameCount + 1 frames. A caster
        // with its shadow off (halfExtents.w = 1) casts into no light.
        const bool flagged = i < cpuCasterMeta_.size() && (cpuCasterMeta_[i] & 1u) != 0u;
        const bool moved = i < pending_.size() && pending_[i] > 0;
        if ((flagged || moved) && cpuBounds_[i].halfExtents.w == 0.0f) { localMovables_.push_back(i); }
    }
}

bool ShadowGpuData::LocalMovableTouches(const Math::float3& centre, float radius) const
{
    if (localMovableGi_) { return true; }
    for (const std::uint32_t i : localMovables_)
    {
        const render::CasterBounds& b = cpuBounds_[i];
        const float dx = std::max(std::abs(centre.x - b.center.x) - b.halfExtents.x, 0.0f);
        const float dy = std::max(std::abs(centre.y - b.center.y) - b.halfExtents.y, 0.0f);
        const float dz = std::max(std::abs(centre.z - b.center.z) - b.halfExtents.z, 0.0f);
        if (dx * dx + dy * dy + dz * dz <= radius * radius) { return true; }
    }
    return false;
}

void ShadowGpuData::UploadCasterLods(Renderer* renderer)
{
    if (!renderer || groupLodOverride_.empty()) { return; }
    if (!EnsureRing(renderer, groupLodOverrideBuf_, groupLodOverride_.size(),
                    sizeof(std::int32_t), L"ShadowGpuData.GroupLodOverride"))
    {
        return;
    }
    const UINT f = renderer->GetCurrentFrameIndex();
    if (f >= render::kFrameCount) { return; }
    if (std::uint8_t* dst = groupLodOverrideBuf_.Region(f))
    {
        std::memcpy(dst, groupLodOverride_.data(), groupLodOverride_.size() * sizeof(std::int32_t));
    }
    // The per-instance table rides its own ring with the same WAR discipline.
    if (casterLod_.empty() ||
        !EnsureRing(renderer, casterLodBuf_, casterLod_.size(),
                    sizeof(std::uint32_t), L"ShadowGpuData.CasterLod"))
    {
        return;
    }
    if (std::uint8_t* dst = casterLodBuf_.Region(f))
    {
        // Local-light plan S7: bit kCasterLocalDynamicBit of the UPLOADED copy (never casterLod_: its compare against
        // last frame is the LOD-change signal that re-renders VSM) = movable for the local shadow cache -- GI, an
        // IsDynamicCaster object, one that moved within kFrameCount + 1 frames (pending_); one that sways in the wind
        // (UE: Movable, or a WPO material with r.Shadow.CacheWPOPrimitives 0) carries kCasterLocalWindBit instead, movable
        // only for the lights inside render::g_localShadowWindRange. Every caster that changes side changes
        // the static set, so the cached pictures go stale: a move invalidates on its first frame (the old position is
        // baked in) and again when it settles (it has to be baked in at the new one).
        // Built in CPU memory and copied ONCE: the ring region is write-combined upload memory, and setting the bit
        // there (`|=` reads it back, uncached) cost 0.33 ms a frame on wind_test -- the main thread's unscoped hole
        // between Scene::PrepareViews and the graph (owner's trace, 2026-10-21).
        casterLodUpload_.assign(casterLod_.begin(), casterLod_.end());
        std::uint32_t* lods = casterLodUpload_.data();
        const std::uint32_t n = static_cast<std::uint32_t>(casterLod_.size());
        if (localDynamicPrev_.size() != n) { localDynamicPrev_.assign(n, 0u); ++localStaticVersion_; }
        bool changed = false;
        std::uint32_t movable = 0, gi = 0, flagged = 0, moved = 0, windy = 0;
        for (std::uint32_t i = 0; i < n; ++i)
        {
            const bool isGi = i >= staticCount_;
            const bool isFlagged = i < cpuCasterMeta_.size() && (cpuCasterMeta_[i] & 1u) != 0u;
            const bool isMoved = i < pending_.size() && pending_[i] > 0;
            const bool isWindy = !render::g_localShadowCacheWind && i < casterWindy_.size() && casterWindy_[i] != 0u;
            const bool dyn = isGi || isFlagged || isMoved;
            // render::g_localShadowWindRange: a caster that is movable only for its sway is static for the lights past
            // the range (shadow_cull_cs reads the view's verdict), so it keeps its own bit.
            if (dyn) { lods[i] |= render::kCasterLocalDynamicBit; }
            else if (isWindy) { lods[i] |= render::kCasterLocalWindBit; }
            // Either static set changes when a caster changes class: 0 static, 1 wind only, 2 movable.
            const std::uint8_t d = dyn ? 2u : (isWindy ? 1u : 0u);
            if (localDynamicPrev_[i] != d) { localDynamicPrev_[i] = d; changed = true; }
            movable += d != 0u ? 1u : 0u;
            gi += isGi ? 1u : 0u;
            flagged += isFlagged ? 1u : 0u;
            moved += isMoved ? 1u : 0u;
            windy += isWindy ? 1u : 0u;
        }
        std::memcpy(dst, lods, static_cast<size_t>(n) * sizeof(std::uint32_t));
        if (changed)
        {
            ++localStaticVersion_;
            LOG_DEBUG_ONCE_PER_MESSAGE(logging::LogCategory::RenderShadow,
                "local shadow cache: {} of {} casters movable (GI {}, IsDynamicCaster {}, moved {}, wind {})",
                movable, n, gi, flagged, moved, windy);
        }
        // Tessy N7e: the lamps' atlases cut the cluster DAG (tessy.lod) at tessy.lodShadowTexels -- a change of either
        // draws other geometry into every cached static picture.
        const float atlasDagKey = (tessy::g_enabled && tessy::g_atlas && tessy::g_lod && tessyDagGroups_ > 0u)
                                      ? std::max(tessy::g_lodShadowTexels, 0.01f) : 0.0f;
        if (atlasDagKey != atlasDagKey_)
        {
            atlasDagKey_ = atlasDagKey;
            ++localStaticVersion_;
        }
    }
    // S4: the camera flags + crossfade tables, same ring discipline (rewritten every frame).
    if (!casterCamCpu_.empty() &&
        EnsureRing(renderer, casterCam_, casterCamCpu_.size(), sizeof(std::uint32_t), L"ShadowGpuData.CasterCam"))
    {
        if (std::uint8_t* dst = casterCam_.Region(f))
        {
            std::memcpy(dst, casterCamCpu_.data(), casterCamCpu_.size() * sizeof(std::uint32_t));
        }
    }
    if (!casterFadeCpu_.empty() &&
        EnsureRing(renderer, casterFade_, casterFadeCpu_.size(), sizeof(float), L"ShadowGpuData.CasterFade"))
    {
        if (std::uint8_t* dst = casterFade_.Region(f))
        {
            std::memcpy(dst, casterFadeCpu_.data(), casterFadeCpu_.size() * sizeof(float));
        }
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::CasterLodSrv(UINT f) const { return casterLodBuf_.Srv(f); }

// Mirrors the rule above for the VSM single-draw permutations. MaskedShadowsActive() keys off the
// LOOP path's masked PSO on purpose: it answers "does this caster set contain masked groups", and
// both permutation pairs are built from the same source, so they succeed or fail together in
// practice. A null here is handled by the caller (it keeps the per-page loop).
ShadowGpuData::CullDecisions ShadowGpuData::PrepareCullPass(RenderGraphPassContext& ctx)
{
    // pass-flow S7a: this used to MIRROR RecordCull in body order, including every one of its
    // early returns, and the two shared a pair of predicate helpers so they could not drift.
    // Now it DECIDES: the gates below are the pass's gates, the declarations are made from those
    // decisions, and RecordCull is handed the same values. Registering on a frame the body will
    // skip advances the compile past barriers nobody emits, which is a wrong before-state for the
    // next pass that touches them (measured: VsmPageRender reading IndirectArgs right after a
    // level switch, when the cull bails on count_ == 0).
    CullDecisions dec{};
    cmdFrameReady_ = false; // A6: decided below for this frame; a frame the cull skips has no merged draws
    cmdMerge_ = { false, false };
    localCompactFrame_ = false; // S6a: likewise -- a frame the cull skips has no compacted rows
    tessySunFrame_ = false;     // tessy.sdsm: decided by Main_SdsmCull's builder, after this one
    tessyCountersCleared_ = false;
    localCompactViews_ = 0;
    localSplitFrame_ = false;   // S7: and no static rows
    tessyViewFrame_ = false;    // Tessy: and no cluster view for Main_DebugDraw
    tessyGbufFrame_ = false;    // Tessy N5: and no cluster G-buffer
    tessyGbufRows_ = 0;
    tessyCamHzbFrame_ = false;
    if (!cullClearMat_ || !cullMat_) { return dec; }
    if (count_ == 0 || numMeshGroups_ == 0) { return dec; }
    // S9e: `--set shadow.cullValidate:1` -- the one-shot validation once more, at whatever the view is now.
    if (render::g_shadowCullRevalidate && valState_ == 2)
    {
        render::g_shadowCullRevalidate = false;
        valState_ = 0;
    }
    // A6: this frame's GroupMask copy for the bindless masked PSO (heap indices change with
    // streaming swaps and fades). Main-thread builder, before any shadow pass records.
    RefreshGroupMaskForFrame(ctx.renderer);
    // A6: the merged G-buffer's static command part (VBV/IBV/SurfaceParams/LOD per command).
    FillGBufferCommandsForFrame(ctx.renderer);
    if (!indirectArgs_.Valid() || !visibleList_.Valid() || !indirectCounts_.Valid()) { return dec; }
    if (!bounds_.Valid() || !viewFrustums_.Valid() || !casterGroup_.Valid() || !perGroup_.Valid() ||
        !perGroupVg_.Valid()) { return dec; }
    if (!cullUavHeap_) { return dec; }
    if (ctx.renderer == nullptr || ctx.renderer->GetCurrentFrameIndex() >= render::kFrameCount) { return dec; }
    dec.active = true;

    // S5b, decided FIRST because its declarations belong to the base point: the cascade casters
    // are HZB-tested when the pyramids are live (Scene's SetCascadeHzbViews said so), the rings
    // exist, and this is not a validation-readback frame -- the CPU reference of that readback is
    // the frustum alone, so a frame with the HZB stage would read as a MISMATCH. `readback`
    // proper is decided below with the same inputs; on the one frame the two could disagree
    // (readback allocation failed) the HZB stage merely sits out a frame.
    {
        const bool readbackWanted = valState_ == 0 && !IsGiIndirectActive() &&
                                    ctx.renderer->GetTotalFrameNumber() > render::kFrameCount;
        dec.hzb = cascadeHzb_.Active() && HzbRingsValid() && cullPostMat_ != nullptr && !readbackWanted;
    }
    hzbCullThisFrame_ = dec.hzb;
    // S4: the camera row -- Scene's decision for the frame, provided the camera rings exist and
    // this frame's tables put something in a bucket. Independent of the validation frame: the
    // camera row is validated against its own snapshot (see below).
    dec.gbuffer = gbufferIndirectFrame_ && camArgs_.Valid() && camVisibleList_.Valid() && perGroupVgCam_.Valid() &&
                  casterCam_.Valid() && casterFade_.Valid() && numVirtualGroups_ > 0 && camListCount_ > 0;
    // S5: the camera candidates are also tested against last frame's pyramid when SceneRenderer
    // said the pyramids are live (SetCameraHzb), the post-cull PSO and the rings exist. A frame
    // with an invalid previous pyramid (cut, resize, first frame) still runs the three passes --
    // the main cull defers nothing then (prevValid = 0), pass B is empty, and the frame's shape
    // does not depend on history. The validation readback is NOT skipped: the camera row's CPU
    // reference adds the deferred counters back (see PollValidation).
    // Tessy's cluster view reads the camera row, so every caster the camera draws must be IN it: pass B (the casters
    // last frame's pyramid hid) does not run while the view is asked for.
    const bool tessyViewWanted = tessy::g_enabled && tessy::g_debugView != 0u && dec.gbuffer;
    dec.camHzb = !tessyViewWanted && dec.gbuffer && camHzbParams_.on != 0u && camCullPostMat_ != nullptr &&
                 camDeferred_.Valid() && camArgsB_.Valid() && camVisibleListB_.Valid() && deferredCount_.Valid() &&
                 camHzbPrev_ != nullptr && camHzbCur_ != nullptr && camHzbPrevSrv_.ptr != 0 && camHzbCurSrv_.ptr != 0;
    camHzbThisFrame_ = dec.camHzb;
    // The matrices and the prev-valid bits as they stand NOW: the Main_CsmHzb builder, which runs
    // after this one and before any record, marks this frame's pyramids built, and a record-time
    // read would take that mark for last frame's.
    cascadeHzb_.FillParams(hzbParams_, dec.hzb);
    {
        std::uint32_t mask = 0;
        for (unsigned c = 0; c < render::CascadeHzb::kCascades; ++c) { mask |= hzbParams_.prevValid[c] ? (1u << c) : 0u; }
        if (mask != hzbPrevValidMask_)
        {
            LOG_INFO(logging::LogCategory::RenderShadow, "cascade hzb cull: on={} prev-valid cascades {}{}{}{} (frame {})",
                     dec.hzb ? 1 : 0, (mask & 1u) ? '0' : '-', (mask & 2u) ? '1' : '-', (mask & 4u) ? '2' : '-',
                     (mask & 8u) ? '3' : '-', ctx.renderer->GetTotalFrameNumber());
            hzbPrevValidMask_ = mask;
        }
    }

    // Local-light plan S6a: the atlas local views' rows compacted for one ExecuteIndirect each. Atlas modes only (VSM
    // renders the local views from these rows per page), with the mega buffer the compacted records point into. The
    // draws ask localCompactFrame_, set HERE on the main thread like cmdMerge_ (they record on workers in parallel).
    {
        const std::uint32_t views = activeViews_ > render::kShadowViewCascades ? activeViews_ - render::kShadowViewCascades : 0u;
        dec.localCompact = render::g_localShadowCompact && !render::VsmActive() && views > 0u &&
                           localCompactMat_ && localCompactMat_->GetPipelineState() &&
                           localArgs_.Valid() && localCounts_.Valid() &&
                           MegaReady() && megaVB_.Get() && megaIB_.Get() && GroupLodMegaSrv().ptr != 0;
        dec.localViews = dec.localCompact ? views : 0u;
        localCompactFrame_ = dec.localCompact;
        localCompactViews_ = dec.localViews;
        // Local-light plan S7: the local views' static casters in their own rows, for the shadow cache. Never on the
        // validation-readback frame (decided the same way below): the CPU reference counts every caster in one row.
        const bool readbackWanted = valState_ == 0 && !IsGiIndirectActive() &&
                                    ctx.renderer->GetTotalFrameNumber() > render::kFrameCount;
        dec.localSplit = dec.localCompact && render::g_localShadowCache && !readbackWanted &&
                         staticArgs_.Valid() && staticVisibleList_.Valid() && localStaticArgs_.Valid() &&
                         localStaticCounts_.Valid();
        localSplitFrame_ = dec.localSplit;
        // Plan S9e: the radius threshold's inputs, this frame's (atlas modes only -- VSM caches pages, UE skips it there).
        dec.localCamPos = localCamPos_;
        dec.localRadiusSq = render::VsmActive() ? 0.0f : localRadiusThresholdSq_;
        // The wind range only splits rows, so only a split frame reads it (VSM never splits).
        if (dec.localSplit) { dec.localWindRigid = localWindRigid_; } else { dec.localWindRigid.fill(0u); }
        dec.tessyStaticViews = localStaticDrawn_;
        // Tessy N4: the atlas views' clusters. The mesh shader draws them (no VS path here), so it needs one; never on
        // the validation-readback frame (the kernel zeroes the records it takes, and the readback compares the rows
        // with a CPU count). The views' wind is filled in by the pass builder (SceneRenderer_Graph).
        Material* atlasMat = MaskedShadowsActive() ? tessyAtlasMeshMaskedMat_.get() : tessyAtlasMeshMat_.get();
        dec.tessyAtlas = tessy::g_enabled && tessy::g_atlas && dec.localCompact && !readbackWanted && TessyReady() &&
                         tessyAtlasCullMat_ && atlasMat && atlasMat->GetPipelineState() && tessyAtlasHeap_ &&
                         dec.localViews <= render::kShadowViewCeiling && ctx.renderer &&
                         ctx.renderer->GetDispatchMeshCommandSignature() != nullptr;
        tessyAtlasFrame_ = dec.tessyAtlas;
        // Tessy N7e: the atlas views cut the cluster DAG (tessy.lod) by each view's light and texels (viewDag_).
        dec.tessyAtlasDag = dec.tessyAtlas && tessy::g_lod && tessyDagGroups_ > 0u && viewDag_.Valid();
        if (!readbackWanted && dec.tessyAtlasDag != tessyAtlasDagLogged_)
        {
            LOG_INFO(logging::LogCategory::RenderShadow, "tessy.lod: the lamps' atlases draw {} (frame {})",
                     dec.tessyAtlasDag ? "the cluster DAG's cut" : "the discrete LODs", ctx.renderer->GetTotalFrameNumber());
            tessyAtlasDagLogged_ = dec.tessyAtlasDag;
        }
        {
            const char* verdict = dec.tessyAtlas ? "on" : !tessy::g_enabled ? "off (tessy.enabled 0)"
                : !tessy::g_atlas ? "off (tessy.atlas 0)"
                : !dec.localCompact ? "classic -- no compacted atlas views this frame (VSM, or the compaction is off)"
                : readbackWanted ? nullptr // the one validation frame: not an event
                : !TessyReady() ? "classic -- no caster mesh has clusters"
                : (!atlasMat || !tessyAtlasCullMat_) ? "classic -- no atlas mesh shader PSO (no mesh shader tier, or it failed)"
                : "classic -- the buffers or the DispatchMesh signature are missing";
            if (verdict && verdict != tessyAtlasLogged_)
            {
                LOG_INFO(logging::LogCategory::RenderShadow, "tessy.atlas: {} (frame {})", verdict, ctx.renderer->GetTotalFrameNumber());
                tessyAtlasLogged_ = verdict;
            }
        }
    }
    // Tessy's cluster view: the same kernel on the camera's row (row set 2: no cull, nothing zeroed, each record
    // carrying its cluster), drawn in colour by Main_DebugDraw. Independent of the atlas path (VSM has no atlases).
    {
        Material* viewMat = MaskedShadowsActive() ? tessyViewMaskedMat_.get() : tessyViewMat_.get();
        const bool viewPso = tessyAtlasCullMat_ && viewMat && viewMat->GetPipelineState();
        dec.tessyView = tessyViewWanted && TessyReady() && viewPso && tessyAtlasHeap_ && ctx.renderer &&
                        ctx.renderer->GetDispatchMeshCommandSignature() != nullptr;
        tessyViewFrame_ = dec.tessyView;
        tessyViewMode_ = tessy::g_debugView;
        tessyViewVerdict_ = dec.tessyView ? (tessyViewMode_ == 2u ? "on (meshlets)" : tessyViewMode_ == 3u ? "on (levels)" : "on (clusters)")
            : !tessy::g_enabled ? "off (tessy.enabled 0)"
            : tessy::g_debugView == 0u ? "off"
            : !dec.gbuffer ? "off -- the camera's indirect G-buffer is off this frame (the view reads its cull row)"
            : !TessyReady() ? "off -- no caster mesh has clusters"
            : !viewPso ? "off -- no cluster view PSO (no mesh shader tier, or it failed)"
            : "off -- the buffers or the DispatchMesh signature are missing";
        if (tessyViewVerdict_ != tessyViewLogged_)
        {
            LOG_INFO(logging::LogCategory::RenderShadow, "tessy.view: {} (frame {})", tessyViewVerdict_,
                     ctx.renderer->GetTotalFrameNumber());
            tessyViewLogged_ = tessyViewVerdict_;
        }
    }
    // Tessy N5: the camera's G-buffer by clusters -- the camera row through the same kernel by PSO run (culled by the
    // camera frustum, its rows zeroed), each run's mesh shader twin drawing them in pass A. The per-frame tables are
    // filled whenever the clusters exist (the kernel binds VgRun on every row set), so a switch allocates nothing. Not
    // on the validation frame (the readback sums the camera row's InstanceCount), not in wireframe (the twins have no
    // wireframe PSO). Pass B (the casters last frame's pyramid hid) stays classic.
    {
        EnsureTessyGBufferMaterials_(ctx.renderer);
        const bool filled = TessyReady() && FillTessyGBufferForFrame_(ctx.renderer);
        const bool readbackWanted = valState_ == 0 && !IsGiIndirectActive() &&
                                    ctx.renderer->GetTotalFrameNumber() > render::kFrameCount;
        const bool wireframe = ctx.renderer->GetWireframeMode();
        const std::uint32_t rows = static_cast<std::uint32_t>(cmdRuns_.size());
        // N7d: the DAG's cut reaches the camera only through the clusters, so tessy.lod turns them on for itself.
        const bool dagWanted = tessy::g_lod && tessyDagGroups_ > 0u;
        dec.tessyGBuffer = tessy::g_enabled && (tessy::g_gbuffer || dagWanted) && dec.gbuffer && !readbackWanted && !wireframe &&
                           filled && tessyGbufTwins_ > 0 && tessyAtlasCullMat_ && tessyAtlasHeap_ &&
                           rows <= render::kShadowViewCeiling && numVirtualGroups_ <= 0x10000u &&
                           ctx.renderer->GetDispatchMeshCommandSignature() != nullptr;
        dec.tessyCamRows = dec.tessyGBuffer ? rows : 0u;
        tessyGbufFrame_ = dec.tessyGBuffer;
        tessyGbufRows_ = dec.tessyCamRows;
        // The camera's two-pass HZB on: the clusters the previous pyramid hid wait for pass B (pass A's rows end where
        // pass B's begin: two rows a run must fit the row set).
        dec.tessyCamHzb = tessy::g_gbufferHzb && dec.tessyGBuffer && dec.camHzb && tessyCameraPostMat_ &&
                          tessyCamDeferred_.Get() && 2u * rows <= render::kShadowViewCeiling;
        tessyCamHzbFrame_ = dec.tessyCamHzb;
        // N7d: the camera position LodSelect selected the tiers from, and its projection of an error: pixels per unit at
        // depth 1 over lod.pixelError and lod.distanceScale (a discrete LOD of the same deviation switches where this
        // cut does -- ViewDistance + R).
        dec.tessyCamDag = dec.tessyGBuffer && dagWanted;
        tessyCamDagFrame_ = dec.tessyCamDag;
        if (!readbackWanted && dec.tessyCamDag != tessyCamDagLogged_)
        {
            LOG_INFO(logging::LogCategory::RenderShadow, "tessy.lod: the camera draws {} (frame {})",
                     dec.tessyCamDag ? "the cluster DAG's cut" : "the discrete LODs", ctx.renderer->GetTotalFrameNumber());
            tessyCamDagLogged_ = dec.tessyCamDag;
        }
        if (dec.tessyCamDag)
        {
            dec.tessyDagCam[0] = lastCameraPos_.x;
            dec.tessyDagCam[1] = lastCameraPos_.y;
            dec.tessyDagCam[2] = lastCameraPos_.z;
            dec.tessyDagCam[3] = render::g_lodPixelsPerUnit / std::max(render::g_lodPixelError, 1e-3f) /
                                 std::max(render::g_lodDistanceScale, 1e-3f);
        }
        for (int i = 0; i < 4; ++i) { tessyDagCamFrame_[i] = dec.tessyDagCam[i]; } // the SDSM partitions cut it the same way
        const char* verdict = dec.tessyGBuffer ? (tessy::g_gbuffer ? "on" : "on (tessy.lod: the DAG's cut is drawn by clusters)")
            : !tessy::g_enabled ? "off (tessy.enabled 0)"
            : !tessy::g_gbuffer && !dagWanted ? "off (tessy.gbuffer 0)"
            : !dec.gbuffer ? "classic -- the camera's indirect G-buffer is off this frame"
            : readbackWanted ? nullptr // the one validation frame: not an event
            : wireframe ? "classic -- wireframe"
            : !TessyReady() ? "classic -- no caster mesh has clusters"
            : tessyGbufTwins_ == 0 ? "classic -- no G-buffer PSO run has a mesh shader twin (no mesh shader tier, raster.bindless off, or the builds failed)"
            : "classic -- the buffers, the kernel or the DispatchMesh signature are missing";
        if (verdict) { tessyGbufVerdict_ = verdict; }
        if (verdict && verdict != tessyGbufLogged_)
        {
            LOG_INFO(logging::LogCategory::RenderShadow, "tessy.gbuffer: {} (frame {})", verdict, ctx.renderer->GetTotalFrameNumber());
            tessyGbufLogged_ = verdict;
        }
    }

    dec.base = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(indirectArgs_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(visibleList_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(indirectCounts_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (dec.localCompact)
    {
        ctx.Use(localArgs_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(localCounts_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (dec.localSplit) // S7: the static rows, seeded, filled by the cull and compacted here
    {
        ctx.Use(staticArgs_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(staticVisibleList_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(localStaticArgs_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(localStaticCounts_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // RecordCull zeroes the whole counter region on these frames (the SDSM part too: Main_SdsmCull then skips its clear).
    tessyCountersCleared_ = dec.tessyAtlas || dec.tessyView || dec.tessyGBuffer;
    if (dec.tessyAtlas || dec.tessyView || dec.tessyGBuffer) // Tessy: written by tessy_atlas_cull_cs here, read by the draws after the consume
    {
        ctx.Use(tessyCamDeferred_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); // N5 (in the kernel's table on every row set)
        ctx.Use(tessyAtlasVisible_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(tessyAtlasInfo_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(tessyAtlasArgs_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(tessyAtlasCounter_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // S5b: the counters are zeroed by the cull-clear every frame (bound unconditionally, so the
    // table has no hole), the rest only on a testing frame -- the post pass returns those to
    // their canonical states, and on other frames nothing touches them.
    if (deferredCount_.Valid()) { ctx.Use(deferredCount_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
    if (dec.hzb)
    {
        ctx.Use(deferredList_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(indirectArgsB_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(visibleListB_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (unsigned c = 0; c < render::CascadeHzb::kCascades; ++c)
        {
            ctx.Use(cascadeHzb_.Pyramid(c), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
    if (dec.gbuffer)
    {
        // S4: the camera's pair, written here, consumed by Main_GBuffer's indirect pass.
        ctx.Use(camArgs_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(camVisibleList_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // A6: the merged commands the expand kernel writes after the camera cull. The decision the
        // record bodies (RecordCull's expand, the G-buffer pass's draws) both read.
        cmdMerge_[0] = cmdFrameReady_ && camCmds_.Valid();
        if (cmdMerge_[0]) { ctx.Use(camCmds_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
    }
    if (dec.camHzb)
    {
        // S5: the deferred list (written here), pass B's pair (seeded here, filled by the post
        // cull), last frame's pyramid (read). The previous frame slot's pyramid rests NON_PIXEL
        // (Main_Hzb's restore), so this is a no-op transition on a steady frame.
        ctx.Use(camDeferred_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(camArgsB_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(camVisibleListB_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(camHzbPrev_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // Unified instance/bounds: uploaded, then scattered into, then read by the cull. The NESTING
    // below is the pass's own control flow now, and RecordCull walks the points it produced.
    // The unified mirror needs the per-frame SRVs, not just Valid() — otherwise this over-declares
    // on the frames the descriptors are not up yet.
    {
        const UINT f = ctx.renderer->GetCurrentFrameIndex();
        dec.useUnified = instancesUnified_.Valid() && boundsUnified_.Valid() && unifiedSrvHeap_ &&
                         UnifiedInstanceSrv(f).ptr != 0 && UnifiedBoundsSrv(f).ptr != 0;
    }
    if (dec.useUnified)
    {
        ctx.NextPoint();
        dec.unifiedCopy = ctx.usePoint ? *ctx.usePoint : 0u;
        ctx.Use(instancesUnified_.buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        ctx.Use(boundsUnified_.buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST);

        // The COPY_DEST -> UAV pair belongs to the GI SCATTER, which the body runs only when GI
        // folding is on. Registering it on `useUnified` alone compiled a point no request ever
        // named on a GI-off frame — and because a request may only match the CURRENT point, that
        // stalled every later request behind it: the closing IndirectArgs -> INDIRECT_ARGUMENT
        // never fired while the compile (and therefore VsmPageRender's compiled before-state) had
        // already advanced past it. That is exactly D3D12 error 527 on 'VsmPageRender', plus 538
        // on the validation readback's CopyBufferRegion when that frame also carried one.
        dec.giOn = IsGiIndirectActive();
        if (dec.giOn)
        {
            ctx.NextPoint();
            dec.giWrite = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(instancesUnified_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.Use(boundsUnified_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            // GI scatter reads each folded object's own instance buffer — resource identity is
            // per-object (census category D). The filtering happens ONCE, here: `giCasterIdx` is
            // what the declarations were made from and what RecordCull will walk, so the two
            // cannot skip different entries.
            ctx.NextPoint();
            dec.giRead = ctx.usePoint ? *ctx.usePoint : 0u;
            for (std::size_t i = 0; i < giCasters_.size(); ++i)
            {
                const GiCaster& gc = giCasters_[i];
                if (!gc.obj || gc.count == 0) { continue; }
                ID3D12Resource* giBuf = gc.obj->GetInstanceCasterResource();
                if (!giBuf || gc.obj->GetInstanceCasterSrv().ptr == 0) { continue; }
                if (dec.giCasterIdx.size() < dec.giCasterIdx.capacity())
                {
                    dec.giCasterIdx.push_back(static_cast<std::uint16_t>(i));
                }
                ctx.Use(giBuf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }

            // ...and hand each one BACK. This buffer belongs to the object, not to the cull: its
            // owner declares the resting state and its own draw restores it. But that draw is
            // CONDITIONAL — a GPU-instanced object the camera does not draw this frame never runs
            // GpuInstancedModels::PrepareRender, while this scatter reads it regardless. The flip
            // trace shows exactly that: on `demo` the only two touches in a frame are the rotation
            // compute (UAV) and this scatter (NON_PIXEL), and nothing ever moves it back, so the
            // frame ended off-canonical. Restoring to the OWNER'S declared state rather than a
            // literal keeps this honest if that owner ever changes where it rests.
            ctx.NextPoint();
            dec.giRestore = ctx.usePoint ? *ctx.usePoint : 0u;
            for (std::uint16_t i : dec.giCasterIdx)
            {
                ID3D12Resource* giBuf = giCasters_[i].obj->GetInstanceCasterResource();
                ctx.Use(giBuf, ctx.renderer->GetCanonicalState(giBuf));
            }
        }

        ctx.NextPoint();
        dec.unifiedRead = ctx.usePoint ? *ctx.usePoint : 0u;
        // The instances rest PIXEL | NON_PIXEL (kInstancesReadState): the indirect G-buffer's PIXEL shader reads its
        // material values out of them by caster id (gbuffer_indirect.hlsl), the shadow / G-buffer VS and the culls the rest.
        ctx.Use(instancesUnified_.buffer.Get(), kInstancesReadState);
        ctx.Use(boundsUnified_.buffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // Step 4 validation (temporary, one-shot after warmup): snapshot the inputs so a CPU cull can
    // be compared against this frame's args kFrameCount frames later. Skipped when GI folding is
    // active — the CPU reference (cpuBounds_) only covers static casters, so it cannot validate
    // the GPU-scattered GI bounds. Toggle GI off (Ctrl+G) to exercise the validator.
    //
    // ALL of it happens here, not in the body: the readback buffer's allocation is what decides
    // whether the copy can run at all, and deciding that mid-record meant a declared COPY_SOURCE
    // point could go unemitted. The snapshot fields are cross-frame state, which the builder owns.
    dec.readback = valState_ == 0 && !dec.giOn &&
                   ctx.renderer->GetTotalFrameNumber() > render::kFrameCount;
    if (dec.readback)
    {
        // S4: the camera row rides behind the shadow rows in the same readback; S5: the counters
        // behind that (the deferred entries the camera row is short by).
        EnsureReadback(ctx.renderer, indirectArgs_.regionBytes + camArgs_.regionBytes + kHzbStatsBytes);
        dec.readback = valReadback_ != nullptr;
    }
    if (dec.readback)
    {
        valBounds_ = cpuBounds_;
        valFrustums_ = cpuViewFrustums_;
        valLocalCamPos_ = dec.localCamPos; // S9e: the threshold the cull applies this frame
        valLocalRadiusSq_ = dec.localRadiusSq;
        valGiFirst_ = staticCount_;
        // S4: the camera row's inputs of THIS frame -- eligibility, the occlusion/chunk skip and
        // the crossfade (a fading caster counts twice), so the CPU reference reproduces the
        // camera branch of shadow_cull_cs.hlsl exactly.
        valCamOn_ = dec.gbuffer;
        valCamHzbOn_ = dec.camHzb;
        valCam_ = casterCamCpu_;
        valFade_ = casterFadeCpu_;
        valCamGroups_ = numVirtualGroups_;
        valCasters_ = dec.giOn ? count_ : staticCount_;
        valViews_ = activeViews_; // S9: the views this frame's cull walked
        // The args are laid out per VIRTUAL group (S3.6: real group * kMaxShadowLods + receiver
        // LOD), so a view's row is numVirtualGroups_ entries wide. Reading it as numMeshGroups_
        // (the pre-S3.6 layout) folded view v's LOD buckets into reader rows 4v..4v+3 and reported
        // every view as a mismatch -- which is what this validator did from S3.6 until 2026-09-03.
        valGroups_ = numVirtualGroups_;
        valFrame_ = ctx.renderer->GetTotalFrameNumber();
        valState_ = 1;
        ctx.NextPoint();
        dec.valCopy = ctx.usePoint ? *ctx.usePoint : 0u;
        ctx.Use(indirectArgs_.buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (dec.gbuffer) { ctx.Use(camArgs_.buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE); }
        if (dec.camHzb) { ctx.Use(deferredCount_.buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE); }
    }
    ctx.NextPoint();
    dec.consume = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(indirectArgs_.buffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    ctx.Use(visibleList_.buffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    if (dec.localCompact)
    {
        ctx.Use(localArgs_.buffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        ctx.Use(localCounts_.buffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
    if (dec.localSplit)
    {
        ctx.Use(staticArgs_.buffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        ctx.Use(staticVisibleList_.buffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        ctx.Use(localStaticArgs_.buffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        ctx.Use(localStaticCounts_.buffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
    if (dec.tessyAtlas || dec.tessyView || dec.tessyGBuffer) // the G-buffer's and the cluster view's draws read them in these states
    {
        ctx.Use(tessyCamDeferred_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); // N5: Main_CamCullPost reads it
        ctx.Use(tessyAtlasVisible_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ctx.Use(tessyAtlasInfo_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ctx.Use(tessyAtlasArgs_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        EnsureTessyAtlasStatsReadback(ctx.renderer);
        const UINT f = ctx.renderer->GetCurrentFrameIndex();
        dec.tessyAtlasStats = tessyAtlasStatsReadback_ != nullptr && f < render::kFrameCount;
        if (dec.tessyAtlasStats)
        {
            ctx.Use(tessyAtlasCounter_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            tessyAtlasStatsFrame_[f] = ctx.renderer->GetTotalFrameNumber(); // cross-frame state: the builder's
        }
    }
    if (dec.gbuffer)
    {
        ctx.Use(camArgs_.buffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        ctx.Use(camVisibleList_.buffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        if (cmdMerge_[0]) { ctx.Use(camCmds_.buffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT); } // A6
    }
    // S5: the counters back to UAV after the validation copy (the post cull adds to them); the
    // deferred list and pass B's pair stay UAV for the post cull, which consumes them.
    if (dec.readback && dec.camHzb) { ctx.Use(deferredCount_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
    return dec;
}

// ---- S5: the camera's two-pass HZB occlusion ---------------------------------------------------

void ShadowGpuData::SetCameraHzb(const CameraHzbParams& params, ID3D12Resource* prevHzb, D3D12_CPU_DESCRIPTOR_HANDLE prevHzbSrv,
                                 ID3D12Resource* curHzb, D3D12_CPU_DESCRIPTOR_HANDLE curHzbSrv)
{
    camHzbParams_ = params;
    camHzbPrev_ = prevHzb;
    camHzbPrevSrv_ = prevHzbSrv;
    camHzbCur_ = curHzb;
    camHzbCurSrv_ = curHzbSrv;
}

ShadowGpuData::CamCullPostDecisions ShadowGpuData::PrepareCamCullPostPass(RenderGraphPassContext& ctx)
{
    CamCullPostDecisions dec{};
    if (!camHzbThisFrame_ || !camCullPostMat_ || !camHzbCur_) { return dec; }
    if (ctx.renderer == nullptr || ctx.renderer->GetCurrentFrameIndex() >= render::kFrameCount) { return dec; }
    const UINT f = ctx.renderer->GetCurrentFrameIndex();
    dec.active = true;

    // Where the main cull left them (declared again so the point is complete on its own), plus
    // this frame's pyramid, rebuilt by Main_HzbA from pass A's depth.
    dec.base = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(camArgsB_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(camVisibleListB_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(camDeferred_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(deferredCount_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(camHzbCur_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // A6: pass B's merged commands -- the decision RecordCamCullPost (expand) and the G-buffer B
    // pass (draws) both read.
    cmdMerge_[1] = cmdFrameReady_ && camCmdsB_.Valid();
    if (cmdMerge_[1]) { ctx.Use(camCmdsB_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
    // Tessy N5: the camera clusters' retest writes pass B's records, rows and claims into the cluster buffers (read by
    // Main_GBufferB after the consume); the deferred list rests readable since Main_ShadowCull's consume.
    dec.tessy = tessyCamHzbFrame_;
    if (dec.tessy)
    {
        ctx.Use(tessyCamDeferred_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ctx.Use(tessyAtlasVisible_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(tessyAtlasInfo_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(tessyAtlasArgs_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(tessyAtlasCounter_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Pass B consumes the pair exactly as pass A's; the counters go out to the readout and back
    // to their resting state. The cascade post cull owns the same readback ring on frames it
    // runs; the two never run in one frame with different stamps because both copy the whole
    // 48 bytes of the same frame -- the later copy simply wins with identical content for the
    // cascade slots and this pass's for the camera slots.
    ctx.NextPoint();
    dec.consume = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(camArgsB_.buffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    ctx.Use(camVisibleListB_.buffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    if (cmdMerge_[1]) { ctx.Use(camCmdsB_.buffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT); } // A6
    if (dec.tessy) // N5: as Main_ShadowCull's consume leaves them (every cluster draw after reads them so)
    {
        ctx.Use(tessyAtlasVisible_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ctx.Use(tessyAtlasInfo_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ctx.Use(tessyAtlasArgs_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
    EnsureHzbStatsReadback(ctx.renderer);
    dec.stats = hzbStatsReadback_ != nullptr;
    if (dec.stats)
    {
        ctx.Use(deferredCount_.buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        hzbStatsFrame_[f] = ctx.renderer->GetTotalFrameNumber(); // cross-frame state: the builder's
    }
    ctx.NextPoint();
    dec.restore = ctx.usePoint ? *ctx.usePoint : 0u;
    if (dec.stats) { ctx.Use(deferredCount_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
    return dec;
}

void ShadowGpuData::RecordCamCullPost(Renderer* renderer, ID3D12GraphicsCommandList* cl, const CamCullPostDecisions& dec)
{
    if (!renderer || !cl || !dec.active) { return; }
    const UINT f = renderer->GetCurrentFrameIndex();
    const std::uint32_t numCasters = cullCastersThisFrame_;
    const std::uint32_t numGroups = numVirtualGroups_;

    renderer->EmitPoint(cl, dec.base);
    if (numCasters > 0 && numGroups > 0)
    {
        struct CullCB { std::uint32_t numCasters, numViews, numGroups, camView; };
        auto cb0 = renderer->GetFrameResource()->AllocDynamic(static_cast<UINT>(sizeof(CullCB)), render::kConstantBufferAlignment);
        auto cb1 = renderer->GetFrameResource()->AllocDynamic(static_cast<UINT>(sizeof(CameraHzbParams)), render::kConstantBufferAlignment);
        if (cb0.cpu && cb1.cpu)
        {
            const CullCB c{ numCasters, 1u, numGroups, render::MaxShadowViews() }; // S9e: the camera slot follows the views
            std::memcpy(cb0.cpu, &c, sizeof(c));
            std::memcpy(cb1.cpu, &camHzbParams_, sizeof(camHzbParams_));

            auto h = renderer->GetRenderContextPool()->Acquire();
            RenderContext& rc = h.ref();
            rc.cbv[0] = cb0.gpu;
            rc.cbv[1] = cb1.gpu;
            rc.srvTable[0] = renderer->StageSrvUavTable({ BoundsReadSrv(f), casterGroup_.Srv(0), perGroupVgCam_.Srv(f), CasterLodSrv(f),
                                                          casterFade_.Srv(f), camHzbCurSrv_ }).gpu;
            rc.uavTable[0] = renderer->StageSrvUavTable({ cullUav_[10 * render::kFrameCount + f], cullUav_[11 * render::kFrameCount + f],
                                                          cullUav_[9 * render::kFrameCount + f], cullUav_[4 * render::kFrameCount + f] }).gpu;
            rc.samplerTable[0] = D3D12_GPU_DESCRIPTOR_HANDLE{};
            camCullPostMat_->Bind(cl, rc);
            // numthreads(8, 8, 1), x walks the deferred list: at most one entry per caster, and the
            // shader reads the list's length from the counters.
            const UINT groupsX = (numCasters + kComputeDispatchGroupSize - 1u) / kComputeDispatchGroupSize;
            if (groupsX > 0) { cl->Dispatch(groupsX, 1, 1); }
            renderer->UAVBarrier(cl, camArgsB_.buffer.Get());
            if (cmdMerge_[1]) { RecordGBufferCommandExpand(renderer, cl, /*passB=*/true); } // A6: pass B's merged commands
        }
    }
    // Tessy N5: the clusters pass A deferred, against this frame's pyramid -> each run's pass-B row (after pass A's).
    if (dec.tessy && tessyGbufRows_ > 0u && f < render::kFrameCount)
    {
        GPU_SCOPE(cl, ProfilerScopes::kTessyCameraPost);
        struct CameraPostCB
        {
            std::uint32_t numRuns, rowFirst, recordBase, recordCap;
            float wind0[4], wind1[4];
            std::uint32_t windPad, deferCap, pad[2];
        };
        CameraPostCB c{};
        c.numRuns = tessyGbufRows_;
        c.rowFirst = kTessyCameraRowSet * render::kShadowViewCeiling + tessyGbufRows_;
        c.recordBase = kTessyCameraRowSet * kTessyAtlasRowSetCap;
        c.recordCap = kTessyAtlasRowSetCap;
        for (int i = 0; i < 4; ++i) { c.wind0[i] = dec.tessyWind0[i]; c.wind1[i] = dec.tessyWind1[i]; }
        c.windPad = std::min(tessy::g_vsmWindPad, 2u);
        c.deferCap = kTessyCamDeferCap;
        auto cb0 = renderer->GetFrameResource()->AllocDynamic(static_cast<UINT>(sizeof(c)), render::kConstantBufferAlignment);
        auto cb1 = renderer->GetFrameResource()->AllocDynamic(static_cast<UINT>(sizeof(CameraHzbParams)), render::kConstantBufferAlignment);
        if (cb0.cpu && cb1.cpu)
        {
            std::memcpy(cb0.cpu, &c, sizeof(c));
            std::memcpy(cb1.cpu, &camHzbParams_, sizeof(camHzbParams_));
            auto h = renderer->GetRenderContextPool()->Acquire();
            RenderContext& rc = h.ref();
            rc.cbv[0] = cb0.gpu;
            rc.cbv[1] = cb1.gpu;
            rc.srvTable[0] = renderer->StageSrvUavTable({ InstanceReadSrv(f), TessyClusterSrv(), tessyVgRun_.Srv(f), camHzbCurSrv_,
                                                          TessyAtlasDescriptor(f, 7u) }).gpu;
            rc.uavTable[0] = renderer->StageSrvUavTable({ TessyAtlasDescriptor(f, 0u), TessyAtlasDescriptor(f, 1u),
                                                          TessyAtlasDescriptor(f, 2u), TessyAtlasDescriptor(f, 3u) }).gpu;
            rc.samplerTable[0] = D3D12_GPU_DESCRIPTOR_HANDLE{};
            tessyCameraPostMat_->Bind(cl, rc);
            cl->Dispatch(tessyGbufRows_, 1, 1); // one group a run
        }
    }
    renderer->EmitPoint(cl, dec.consume);
    if (dec.stats)
    {
        cl->CopyBufferRegion(hzbStatsReadback_.Get(), static_cast<UINT64>(f) * kHzbStatsBytes,
                             deferredCount_.buffer.Get(), static_cast<UINT64>(f) * deferredCount_.regionBytes, kHzbStatsBytes);
    }
    renderer->EmitPoint(cl, dec.restore);
}

// ---- S4: the camera's indirect G-buffer -------------------------------------------------------

bool ShadowGpuData::GBufferIndirectReady() const
{
    return eligibleCasterCount_ > 0 && camArgs_.Valid() && camVisibleList_.Valid() &&
           cullMat_ && cullClearMat_ && numVirtualGroups_ > 0 && IndirectDrawReady();
}

bool ShadowGpuData::RecordIndirectGBufferDraws(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                               D3D12_GPU_VIRTUAL_ADDRESS viewCB, bool wireframe,
                                               std::uint32_t vgBegin, std::uint32_t vgEnd, bool passB)
{
    if (!renderer || !cl || !gbufferIndirectFrame_ || !camArgs_.Valid() || !camVisibleList_.Valid()) { return false; }
    if (numVirtualGroups_ == 0 || camListCount_ == 0) { return false; }
    // S5: pass B draws the post cull's pair -- the same virtual groups, the same materials, only
    // the args + the instance stream differ. Mostly empty draws on a settled frame (measured; the
    // per-group ExecuteIndirect with InstanceCount 0 is the accepted cost of one list).
    if (passB && (!camHzbThisFrame_ || !camArgsB_.Valid() || !camVisibleListB_.Valid())) { return false; }
    const UavRing& args = passB ? camArgsB_ : camArgs_;
    const UavRing& list = passB ? camVisibleListB_ : camVisibleList_;
    vgEnd = std::min(vgEnd, numVirtualGroups_);
    if (vgBegin >= vgEnd) { return false; }
    const UINT f = renderer->GetCurrentFrameIndex();
    if (f >= render::kFrameCount) { return false; }
    ID3D12CommandSignature* sig = renderer->GetDrawIndexedCommandSignature();
    if (!sig) { return false; }

    // b0 per LOD value: the virtual group's LOD is the only per-draw constant, and there are four
    // of them -- four allocations for the whole pass, not one per draw.
    struct IndirectDrawCB { std::uint32_t groupLod; std::uint32_t pad[3]; };
    D3D12_GPU_VIRTUAL_ADDRESS lodCb[render::kMaxShadowLods] = {};
    for (std::uint32_t L = 0; L < render::kMaxShadowLods; ++L)
    {
        auto cb = renderer->GetFrameResource()->AllocDynamic(static_cast<UINT>(sizeof(IndirectDrawCB)), render::kConstantBufferAlignment);
        if (!cb.cpu) { return false; }
        const IndirectDrawCB c{ L, { 0u, 0u, 0u } };
        std::memcpy(cb.cpu, &c, sizeof(c));
        lodCb[L] = cb.gpu;
    }
    // t4..t6 for every draw: the records (the unified copy the shadow VS reads too), the fades,
    // the camera LODs (t3 is the material's emissive map, Part E). Staged once.
    const D3D12_GPU_DESCRIPTOR_HANDLE tail = renderer->StageSrvUavTable({ InstanceReadSrv(f), casterFade_.Srv(f), CasterLodSrv(f) }).gpu;

    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // Slot 1 = the camera visible list as a per-instance stream; each draw's StartInstanceLocation
    // (seeded by the cull-clear from the camera bases) offsets into it.
    D3D12_VERTEX_BUFFER_VIEW visVBV{};
    visVBV.BufferLocation = list.buffer->GetGPUVirtualAddress() + static_cast<UINT64>(f) * list.regionBytes;
    visVBV.SizeInBytes = static_cast<UINT>(list.regionBytes);
    visVBV.StrideInBytes = sizeof(std::uint32_t);
    cl->IASetVertexBuffers(1, 1, &visVBV);

    const UINT64 argRegionBase = static_cast<UINT64>(f) * args.regionBytes;
    const Mesh* boundMesh = nullptr;
    UINT boundLod = 0xFFFFFFFFu;
    bool drew = false;

    // A6, the merged path: every run whose PSO is the bindless permutation draws with ONE
    // ExecuteIndirect from the commands the expand kernel wrote this frame; the loop below then
    // only handles what is left (table PSOs, runs without a command signature). Only a full range
    // merges -- the fan-out shape of the callers is a whole pass on one list.
    const UavRing& cmds = passB ? camCmdsB_ : camCmds_;
    const bool merged = cmdFrameReady_ && cmdMerge_[passB ? 1u : 0u] && cmds.Valid() && !cmdRuns_.empty() &&
                        vgBegin == 0u && vgEnd >= numVirtualGroups_ && cmdOrder_.size() == cmdCountFrame_;
    // Per virtual group: 1 = a merged run drew it. Per THREAD and kept for its capacity (two passes can record at
    // once; as a local it was an allocation per pass per frame).
    thread_local std::vector<std::uint8_t> mergedVg;
    mergedVg.clear();
    std::uint32_t mergedCalls = 0, mergedCmds = 0, perGroupDraws = 0;
    if (merged)
    {
        mergedVg.assign(numVirtualGroups_, 0u);
        const UINT64 cmdRegionBase = static_cast<UINT64>(f) * cmds.regionBytes;
        for (std::uint32_t r = 0; r < cmdRuns_.size(); ++r)
        {
            const GBufferCmdRun& run = cmdRuns_[r];
            if (!run.mat || !run.md || run.count == 0 || !run.mat->IsBindless()) { continue; }
            ID3D12CommandSignature* runSig = renderer->GetBindlessIndirectCommandSignature(run.mat);
            if (!runSig) { continue; }
            auto h = renderer->GetRenderContextPool()->Acquire();
            RenderContext& ctx = h.ref();
            ctx.cbv[1] = viewCB;
            run.md->StageGBufferBindings(renderer, ctx, 0, 0, /*bindless=*/true); // the sampler; textures by index
            ctx.srvTable[4] = tail;
            if (!run.mat->Bind(cl, ctx, wireframe)) { continue; }
            // Tessy N5: a run the camera's clusters took whole has only empty commands left -- the kernel's count skips
            // them (0) or keeps them all (the run's exact count: ~0 drew nothing, tessy_atlas_cull_cs.hlsl says why); 824
            // empty commands cost the grove 0.045 ms.
            const bool tessyCount = !passB && tessyGbufFrame_ && r < tessyGbufRows_ && r < tessyGbufMats_.size() && tessyGbufMats_[r];
            renderer->ExecuteIndirect(cl, runSig, run.count, cmds.buffer.Get(),
                                      cmdRegionBase + static_cast<UINT64>(run.first) * Renderer::kBindlessIndirectCommandBytes,
                                      tessyCount ? tessyAtlasArgs_.Get() : nullptr,
                                      tessyCount ? static_cast<UINT64>(f) * kTessyAtlasArgsRegionBytes + kTessyCamClassicCountOffset + r * 4u : 0u);
            for (std::uint32_t k = run.first; k < run.first + run.count && k < cmdOrder_.size(); ++k)
            {
                if (cmdOrder_[k] < mergedVg.size()) { mergedVg[cmdOrder_[k]] = 1u; }
            }
            ++mergedCalls;
            mergedCmds += run.count;
            drew = true;
        }
        // The commands set root arguments and IA views behind the bind cache's back: the per-group
        // draws below start from a clean slate.
        render::g_clBindState.Reset();
    }
    for (std::uint32_t vg = vgBegin; vg < vgEnd; ++vg)
    {
        if (vg >= vgCamCount_.size() || vgCamCount_[vg] == 0u) { continue; }
        if (merged && vg < mergedVg.size() && mergedVg[vg]) { continue; } // A6: drawn by a merged run
        const std::uint32_t g = vg / render::kMaxShadowLods;
        const std::uint32_t vgLod = vg % render::kMaxShadowLods;
        if (g >= numStaticGroups_ || g >= groupGbuf_.size()) { continue; } // GI groups draw their own way
        const GroupGbuf& gg = groupGbuf_[g];
        const Mesh* mesh = (g < groupMesh_.size()) ? groupMesh_[g] : nullptr;
        if (!gg.mat || !mesh) { continue; }
        ID3D12Resource* vb = mesh->GetVertexBufferResource();
        const UINT groupLod = mesh->ClampExplicitLod(vgLod);
        ID3D12Resource* ib = mesh->GetLodIndexBufferResource(groupLod);
        if (!vb || !ib) { continue; }

        if (mesh != boundMesh || groupLod != boundLod)
        {
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
            boundMesh = mesh;
            boundLod = groupLod;
        }

        // The group's material exactly as the CPU per-submesh draw binds it: textures + sampler
        // (t0..t2, s0), surface params (b2); plus the view CB and the LOD constant.
        auto h = renderer->GetRenderContextPool()->Acquire();
        RenderContext& ctx = h.ref();
        // A6: the bindless permutation takes the LOD as root constants (what the merged command
        // signature writes per command); the table permutation keeps its b0 CBV.
        const bool bindless = gg.mat->IsBindless();
        if (bindless) { ctx.constants[0] = { vgLod, 0u, 0u, 0u }; }
        else { ctx.cbv[0] = lodCb[vgLod]; }
        ctx.cbv[1] = viewCB;
        if (gg.md)
        {
            gg.md->StageGBufferBindings(renderer, ctx, 0, 0, bindless);
            gg.md->StageGBufferSurfaceParams(renderer, ctx, 2);
        }
        else
        {
            MaterialData::StageNeutralGBufferSurfaceParams(renderer, ctx, 2);
        }
        ctx.srvTable[4] = tail;
        // GBufferBindingGuard.h: a root parameter left unbound (a group with no material) is not a
        // draw to issue -- the CPU path skips that submesh the same way.
        if (!gg.mat->Bind(cl, ctx, wireframe)) { continue; }

        const UINT64 argOffset = argRegionBase + static_cast<UINT64>(vg) * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        renderer->ExecuteIndirect(cl, sig, 1, args.buffer.Get(), argOffset, nullptr, 0);
        drew = true;
        ++perGroupDraws;
    }
    // Tessy N5: the clusters the camera row's kernel took from the rows above, one DispatchMesh per run. Pass A only
    // (pass B's casters stay classic), and on the call that holds the whole range (the pass records one list).
    if (!passB && tessyGbufFrame_ && vgBegin == 0u)
    {
        GPU_SCOPE(cl, ProfilerScopes::kGBufferTessy);
        RecordTessyGBufferDraws_(renderer, cl, viewCB, wireframe, 0u);
        drew = true;
    }
    // ...and pass B's: the clusters the post pass found visible after all, from the runs' pass-B rows.
    if (passB && tessyCamHzbFrame_ && vgBegin == 0u)
    {
        GPU_SCOPE(cl, ProfilerScopes::kGBufferTessy);
        RecordTessyGBufferDraws_(renderer, cl, viewCB, wireframe, tessyGbufRows_);
        drew = true;
    }
    // A6 readout: how many ExecuteIndirect the pass cost (the plan's acceptance number).
    RecordIndirectGBufferStats_(passB, mergedCalls, mergedCmds, perGroupDraws);
    return drew;
}

// Tessy N5: each merged run's mesh shader twin -- the run's own desc (RTs, ALPHA_TEST, cull mode, shading model, the
// layouts, EDITOR_OBJECT_ID) with TESSY_GBUFFER_MS. A table PSO's run has none, and neither has a run whose build failed:
// their groups keep the classic draw (VgRun = ~0 sends the kernel past them).
void ShadowGpuData::EnsureTessyGBufferMaterials_(Renderer* renderer)
{
    if (!tessyGbufMatsDirty_ || !renderer || !renderer->GetMaterialManager()) { return; }
    tessyGbufMatsDirty_ = false;
    tessyGbufMats_.assign(cmdRuns_.size(), nullptr);
    tessyGbufTwins_ = 0;
    tessyVgRunCpu_.assign(numVirtualGroups_, ~0u);
    // The runs' command order for the kernel (t13): {first, count} per run, then cmdOrder_.
    const std::uint32_t runCount = static_cast<std::uint32_t>(cmdRuns_.size());
    tessyRunCmdsCpu_.assign(2u * runCount, 0u);
    for (std::uint32_t r = 0; r < runCount; ++r)
    {
        tessyRunCmdsCpu_[2u * r] = 2u * runCount + cmdRuns_[r].first;
        tessyRunCmdsCpu_[2u * r + 1u] = cmdRuns_[r].count;
    }
    tessyRunCmdsCpu_.insert(tessyRunCmdsCpu_.end(), cmdOrder_.begin(), cmdOrder_.end());
    if (cmdRuns_.empty() || !renderer->SupportsMeshShaders()) { return; }
    auto* mm = renderer->GetMaterialManager();
    for (std::size_t r = 0; r < cmdRuns_.size(); ++r)
    {
        const GBufferCmdRun& run = cmdRuns_[r];
        if (!run.mat || !run.md || !run.mat->IsBindless()) { continue; }
        Material::GraphicsDesc gd = run.mat->GetCachedGraphicsDesc();
        gd.msEntry = "MSMain";
        gd.inputLayoutKey = "";
        gd.defines.emplace_back("TESSY_GBUFFER_MS", "1");
        auto m = mm->GetOrCreateGraphics(renderer, gd);
        if (!m || !m->GetPipelineState())
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "TESSY_GBUFFER_MS mesh shader PSO of G-buffer run {} FAILED (its groups stay classic)", r);
            continue;
        }
        tessyGbufMats_[r] = std::move(m);
        ++tessyGbufTwins_;
        for (std::uint32_t k = run.first; k < run.first + run.count && k < cmdOrder_.size(); ++k)
        {
            if (cmdOrder_[k] < tessyVgRunCpu_.size()) { tessyVgRunCpu_[cmdOrder_[k]] = static_cast<std::uint32_t>(r); }
        }
    }
    LOG_INFO(logging::LogCategory::RenderShadow, "tessy.gbuffer: mesh shader twins for {} of {} G-buffer PSO runs",
             tessyGbufTwins_, cmdRuns_.size());
}

// This frame's VgRun (the kernel's t5 on every row set -- a volatile table has no holes) and, with twins, every mesh
// group's SurfaceParams (the twins' t13). The rings grow only after a Rebuild (GPU idle).
bool ShadowGpuData::FillTessyGBufferForFrame_(Renderer* renderer)
{
    const UINT f = renderer ? renderer->GetCurrentFrameIndex() : render::kFrameCount;
    if (f >= render::kFrameCount || numVirtualGroups_ == 0) { return false; }
    if (tessyVgRunCpu_.size() != numVirtualGroups_) { tessyVgRunCpu_.assign(numVirtualGroups_, ~0u); }
    if (!EnsureRing(renderer, tessyVgRun_, numVirtualGroups_, sizeof(std::uint32_t), L"Tessy.VgRun")) { return false; }
    std::memcpy(tessyVgRun_.Region(f), tessyVgRunCpu_.data(), tessyVgRunCpu_.size() * sizeof(std::uint32_t));
    if (!tessyRunCmdsCpu_.empty())
    {
        if (!EnsureRing(renderer, tessyRunCmds_, tessyRunCmdsCpu_.size(), sizeof(std::uint32_t), L"Tessy.RunCmds")) { return false; }
        std::memcpy(tessyRunCmds_.Region(f), tessyRunCmdsCpu_.data(), tessyRunCmdsCpu_.size() * sizeof(std::uint32_t));
    }
    if (tessyGbufTwins_ == 0) { return true; }
    const std::size_t groups = std::max<std::size_t>(std::min<std::size_t>(numStaticGroups_, groupGbuf_.size()), 1u);
    if (!EnsureRing(renderer, tessyGroupSurface_, groups, sizeof(render::MaterialSurfaceParamsGpu), L"Tessy.GroupSurface"))
    {
        return false;
    }
    auto* dst = reinterpret_cast<render::MaterialSurfaceParamsGpu*>(tessyGroupSurface_.Region(f));
    for (std::size_t g = 0; g < groups; ++g)
    {
        if (g < groupGbuf_.size() && groupGbuf_[g].md) { groupGbuf_[g].md->FillGBufferSurfaceParams(dst[g]); }
        else { dst[g] = render::MaterialSurfaceParamsGpu{}; }
    }
    return true;
}

// Pass A's clusters: per run with a twin, one DispatchMesh of its camera row (the kernel wrote the args). The table is
// the classic draws' t4..t6 followed by the cluster inputs, the run's row and the groups' SurfaceParams.
void ShadowGpuData::RecordTessyGBufferDraws_(Renderer* renderer, ID3D12GraphicsCommandList* cl, D3D12_GPU_VIRTUAL_ADDRESS viewCB,
                                             bool wireframe, std::uint32_t rowFirst)
{
    const UINT f = renderer->GetCurrentFrameIndex();
    ID3D12CommandSignature* const meshSig = renderer->GetDispatchMeshCommandSignature();
    if (f >= render::kFrameCount || !meshSig || !tessyGroupSurface_.Valid()) { return; }
    const D3D12_GPU_DESCRIPTOR_HANDLE table = renderer->StageSrvUavTable({
        InstanceReadSrv(f), casterFade_.Srv(f), CasterLodSrv(f),
        TessyAtlasDescriptor(f, 4u), MegaVertexRawSrv(), TessyMeshletSrv(), TessyClusterVertsSrv(), TessyClusterTrisSrv(),
        TessyAtlasDescriptor(f, 5u), tessyGroupSurface_.Srv(f) }).gpu;
    const float viewport[2] = { static_cast<float>(renderer->GetRenderWidth()), static_cast<float>(renderer->GetRenderHeight()) };
    std::uint32_t viewportBits[2];
    std::memcpy(viewportBits, viewport, sizeof(viewportBits));
    const std::uint32_t flags = tessy::g_vsmMsCull & 1u; // kTessyMsCullSmall: the triangles that hold no pixel centre
    for (std::uint32_t r = 0; r < tessyGbufRows_ && r < tessyGbufMats_.size() && r < cmdRuns_.size(); ++r)
    {
        Material* const m = tessyGbufMats_[r].get();
        const GBufferCmdRun& run = cmdRuns_[r];
        if (!m || !run.md) { continue; }
        auto h = renderer->GetRenderContextPool()->Acquire();
        RenderContext& ctx = h.ref();
        const std::uint32_t row = kTessyCameraRowSet * render::kShadowViewCeiling + rowFirst + r;
        ctx.constants[0] = { row, flags, viewportBits[0], viewportBits[1] };
        ctx.cbv[1] = viewCB;
        run.md->StageGBufferBindings(renderer, ctx, 0, 0, /*bindless=*/true); // the sampler; the textures by index
        ctx.srvTable[4] = table;
        if (!m->Bind(cl, ctx, wireframe)) { continue; }
        renderer->ExecuteIndirect(cl, meshSig, 1, tessyAtlasArgs_.Get(),
            static_cast<UINT64>(f) * kTessyAtlasArgsRegionBytes + static_cast<UINT64>(row) * 12u, nullptr, 0);
    }
}

Material* ShadowGpuData::IndirectShadowTessyMaterial() const
{
    return MaskedShadowsActive() ? indirectShadowTessyMaskedMat_.get() : indirectShadowTessyMat_.get();
}

Material* ShadowGpuData::IndirectShadowTessyMeshMaterial() const
{
    return MaskedShadowsActive() ? indirectShadowTessyMeshMaskedMat_.get() : indirectShadowTessyMeshMat_.get();
}

Material* ShadowGpuData::IndirectShadowPageMaterial() const
{
    return MaskedShadowsActive() ? indirectShadowPageMaskedMat_.get() : indirectShadowPageMat_.get();
}

bool ShadowGpuData::MaskedShadowsActive() const
{
    if (!hasMaskedGroups_ || !indirectShadowMaskedMat_ || !indirectShadowMaskedMat_->GetPipelineState()) { return false; }
    // A6: the table PSO can only serve the first kMaxMaskedGroups masked groups; the bindless one
    // serves them all by heap index.
    return MaskedShadowsBindless() ? maskedGroupCount_ > 0 : maskedAlbedoCount_ > 0;
}

bool ShadowGpuData::MaskedShadowsBindless() const
{
    return indirectShadowMaskedMat_ && indirectShadowMaskedMat_->IsBindless();
}

void ShadowGpuData::RefreshGroupMaskForFrame(Renderer* renderer)
{
    groupMaskRegion_ = 0; // the static table-path copy, written at Rebuild
    if (!renderer || !MaskedShadowsBindless() || !groupMask_.Valid() || numMeshGroups_ == 0 ||
        groupMaskCpu_.size() < numMeshGroups_ || groupMaskMd_.size() < numMeshGroups_)
    {
        return;
    }
    const UINT f = renderer->GetCurrentFrameIndex();
    if (f >= render::kFrameCount) { return; }
    auto* dst = reinterpret_cast<DirectX::XMUINT2*>(groupMask_.Region(f));
    if (!dst) { return; }
    for (std::uint32_t g = 0; g < numMeshGroups_; ++g)
    {
        DirectX::XMUINT2 gm = groupMaskCpu_[g];
        if (const MaterialData* md = groupMaskMd_[g])
        {
            const std::uint32_t index = md->albedo.BindlessIndex();
            gm.x = index != render::BindlessHeap::kInvalidIndex ? index : 0xFFFFFFFFu; // no slot = casts solid
        }
        dst[g] = gm;
    }
    groupMaskRegion_ = f;
}

void ShadowGpuData::BuildGBufferCommandOrder_()
{
    tessyGbufMatsDirty_ = true; // Tessy N5: the runs' mesh shader twins follow the runs (next PrepareCullPass)
    cmdOrder_.clear();
    cmdGeom_.clear();
    cmdRuns_.clear();
    struct Entry { Material* mat; const Mesh* mesh; std::uint32_t lod; std::uint32_t vg; };
    std::vector<Entry> entries;
    entries.reserve(numVirtualGroups_);
    for (std::uint32_t g = 0; g < numStaticGroups_ && g < groupGbuf_.size() && g < groupMesh_.size(); ++g)
    {
        const GroupGbuf& gg = groupGbuf_[g];
        const Mesh* mesh = groupMesh_[g];
        // A textureless material merges like any other since 2026-10-23: its slots are the heap's
        // null texture and the sampler is staged regardless (MaterialData::StageGBufferBindings).
        if (!gg.mat || !gg.md || !mesh || !mesh->GetVertexBufferResource()) { continue; }
        for (std::uint32_t lod = 0; lod < render::kMaxShadowLods; ++lod)
        {
            if (!mesh->GetLodIndexBufferResource(mesh->ClampExplicitLod(lod))) { continue; }
            entries.push_back(Entry{ gg.mat, mesh, lod, g * render::kMaxShadowLods + lod });
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b)
    {
        if (a.mat != b.mat) { return a.mat < b.mat; }
        if (a.mesh != b.mesh) { return a.mesh < b.mesh; }
        return a.lod < b.lod;
    });
    for (const Entry& e : entries)
    {
        if (cmdRuns_.empty() || cmdRuns_.back().mat != e.mat)
        {
            GBufferCmdRun run;
            run.mat = e.mat;
            run.md = groupGbuf_[e.vg / render::kMaxShadowLods].md;
            run.first = static_cast<std::uint32_t>(cmdOrder_.size());
            run.count = 0;
            cmdRuns_.push_back(run);
        }
        ++cmdRuns_.back().count;
        cmdOrder_.push_back(e.vg);
        // The geometry views are immutable after load: written once here, copied per frame.
        ID3D12Resource* vb = e.mesh->GetVertexBufferResource();
        ID3D12Resource* ib = e.mesh->GetLodIndexBufferResource(e.mesh->ClampExplicitLod(e.lod));
        const D3D12_GPU_VIRTUAL_ADDRESS vbVA = vb->GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS ibVA = ib->GetGPUVirtualAddress();
        cmdGeom_.push_back(DirectX::XMUINT4(static_cast<std::uint32_t>(vbVA & 0xFFFFFFFFull), static_cast<std::uint32_t>(vbVA >> 32),
                                            static_cast<std::uint32_t>(vb->GetDesc().Width), e.mesh->GetVertexStride()));
        cmdGeom_.push_back(DirectX::XMUINT4(static_cast<std::uint32_t>(ibVA & 0xFFFFFFFFull), static_cast<std::uint32_t>(ibVA >> 32),
                                            static_cast<std::uint32_t>(ib->GetDesc().Width), static_cast<std::uint32_t>(e.mesh->GetIndexFormat())));
    }
    LOG_INFO(logging::LogCategory::RenderShadow, "[gbuffer.indirect] merged command order: {} commands in {} PSO runs over {} static groups",
             cmdOrder_.size(), cmdRuns_.size(), numStaticGroups_);
}

bool ShadowGpuData::FillGBufferCommandsForFrame(Renderer* renderer)
{
    cmdFrameReady_ = false;
    cmdCountFrame_ = 0;
    // UNDER GBV, NO MERGED COMMANDS: they change the VB, the IB and two root arguments per draw inside one
    // ExecuteIndirect, which GPU-based validation cannot follow -- it says so with D3D12 id=1044 on 'GBuffer.Indirect'
    // and from then on calls its own state tracking imprecise for the rest of the frame. The per-group path below in
    // RecordIndirectGBufferDraws draws the same groups with the same PSOs and a DRAW_INDEXED-only signature, every
    // binding set on the CPU where GBV sees it. So a GBV run validates these shaders exactly instead of half-blind.
    if (GraphicsDevice::GbvEnabled())
    {
        LOG_INFO_ONCE(logging::LogCategory::RenderShadow,
                      "[gbuffer.indirect] GBV on: one ExecuteIndirect per group (merged bindless commands change VB/IB/root "
                      "bindings per draw, which GBV cannot track -- D3D12 id=1044)");
        return false;
    }
    if (!renderer || !render::g_rasterBindless || !gbufferIndirectFrame_ || cmdOrder_.empty() ||
        !gbufferCmdExpandMat_ || !gbufferCmdExpandMat_->GetPipelineState() ||
        !cmdStatic_.Valid() || !camCmds_.Valid())
    {
        return false;
    }
    const UINT f = renderer->GetCurrentFrameIndex();
    if (f >= render::kFrameCount) { return false; }
    const std::size_t n = cmdOrder_.size();
    if (cmdGeom_.size() != n * 2 || cmdStatic_.capacity < n * 3 || camCmds_.regionBytes < n * Renderer::kBindlessIndirectCommandBytes) { return false; }
    auto* dst = reinterpret_cast<DirectX::XMUINT4*>(cmdStatic_.Region(f));
    if (!dst) { return false; }
    for (std::size_t k = 0; k < n; ++k)
    {
        const std::uint32_t vg = cmdOrder_[k];
        const std::uint32_t g = vg / render::kMaxShadowLods;
        const std::uint32_t vgLod = vg % render::kMaxShadowLods;
        RenderContext ctx{};
        groupGbuf_[g].md->StageGBufferSurfaceParams(renderer, ctx, 2); // per material per frame (cached), carries the texture indices
        dst[k * 3 + 0] = cmdGeom_[k * 2 + 0];
        dst[k * 3 + 1] = cmdGeom_[k * 2 + 1];
        dst[k * 3 + 2] = DirectX::XMUINT4(static_cast<std::uint32_t>(ctx.cbv[2] & 0xFFFFFFFFull),
                                          static_cast<std::uint32_t>(ctx.cbv[2] >> 32), vgLod, vg);
    }
    cmdCountFrame_ = static_cast<std::uint32_t>(n);
    cmdFrameReady_ = true;
    return true;
}

void ShadowGpuData::RecordIndirectGBufferStats_(bool passB, std::uint32_t mergedCalls, std::uint32_t mergedCmds,
                                                std::uint32_t perGroupDraws) const
{
    // Two call sites on purpose: the throttle is per site, and the two passes record on different lists.
    if (passB)
    {
        LOG_INFO_THROTTLED(std::chrono::seconds(5), logging::LogCategory::RenderShadow,
                           "[gbuffer.indirect] pass B: {} ExecuteIndirect ({} merged runs over {} commands + {} per-group), raster.bindless {}",
                           mergedCalls + perGroupDraws, mergedCalls, mergedCmds, perGroupDraws, render::g_rasterBindless ? 1 : 0);
    }
    else
    {
        LOG_INFO_THROTTLED(std::chrono::seconds(5), logging::LogCategory::RenderShadow,
                           "[gbuffer.indirect] pass A: {} ExecuteIndirect ({} merged runs over {} commands + {} per-group), raster.bindless {}",
                           mergedCalls + perGroupDraws, mergedCalls, mergedCmds, perGroupDraws, render::g_rasterBindless ? 1 : 0);
    }
}

void ShadowGpuData::RecordGBufferCommandExpand(Renderer* renderer, ID3D12GraphicsCommandList* cl, bool passB)
{
    if (!renderer || !cl || !cmdFrameReady_ || cmdCountFrame_ == 0 || !gbufferCmdExpandMat_) { return; }
    const UINT f = renderer->GetCurrentFrameIndex();
    if (f >= render::kFrameCount) { return; }
    const UavRing& cmds = passB ? camCmdsB_ : camCmds_;
    if (!cmds.Valid()) { return; }
    const std::size_t argSet = passB ? 10u : 7u;  // the pass's camera args (RAW UAV)
    const std::size_t cmdSet = passB ? 13u : 12u; // the pass's commands (RAW UAV)
    struct ExpandCB { std::uint32_t count; std::uint32_t pad[3]; };
    const std::uint32_t count = cmdCountFrame_;
    RecordComputeDispatch(renderer, cl, gbufferCmdExpandMat_.get(), static_cast<UINT>(sizeof(ExpandCB)),
        [count](std::uint8_t* dst) { const ExpandCB c{ count, { 0u, 0u, 0u } }; std::memcpy(dst, &c, sizeof(c)); },
        { cmdStatic_.Srv(f) },
        { cullUav_[argSet * render::kFrameCount + f], cullUav_[cmdSet * render::kFrameCount + f] },
        D3D12_GPU_DESCRIPTOR_HANDLE{},
        count, 1,
        cmds.buffer.Get());
}

void ShadowGpuData::RecordLocalArgsCompact(Renderer* renderer, ID3D12GraphicsCommandList* cl, std::uint32_t views,
                                           bool staticRows)
{
    if (!renderer || !cl || views == 0u || !localCompactMat_) { return; }
    const UINT f = renderer->GetCurrentFrameIndex();
    if (f >= render::kFrameCount) { return; }
    struct CompactCB { std::uint32_t numGroups, firstView, numViews, pad; };
    // The main rows are indexed by view slot; S7's static rows already by slot - kShadowViewCascades.
    const CompactCB c{ numVirtualGroups_, staticRows ? 0u : render::kShadowViewCascades, views, 0u };
    const std::size_t src = staticRows ? 16u : 0u;
    const std::size_t outArgs = staticRows ? 18u : 14u;
    const std::size_t outCounts = staticRows ? 19u : 15u;
    // One 64-thread group per view: the helper divides both extents by its 8x8 group.
    RecordComputeDispatch(renderer, cl, localCompactMat_.get(), static_cast<UINT>(sizeof(CompactCB)),
        [c](std::uint8_t* dst) { std::memcpy(dst, &c, sizeof(c)); },
        { GroupLodMegaSrv() },
        { cullUav_[src * render::kFrameCount + f], cullUav_[outArgs * render::kFrameCount + f],
          cullUav_[outCounts * render::kFrameCount + f] },
        D3D12_GPU_DESCRIPTOR_HANDLE{},
        views * kComputeDispatchGroupSize, kComputeDispatchGroupSize);
}

bool ShadowGpuData::IsGiIndirectActive() const
{
    return render::g_giIndirectShadowsEnabled && !giCasters_.empty() &&
           giScatterMat_ && giScatterMat_->GetPipelineState();
}
bool ShadowGpuData::IsGiFoldedActive(const RenderableObjectBase* obj) const
{
    if (!obj || !IsGiIndirectActive()) { return false; }
    for (const GiCaster& gc : giCasters_) { if (gc.obj == obj) { return true; } }
    return false;
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
    const std::vector<std::unique_ptr<RenderableObjectBase>>& objects, bool sameGeometry)
{
    if (!renderer) { return; }
    ++drawTablesGeneration_; // the shadow views' staged tables name what this replaces
    // Scoped because a rebuild is never just its own cost: it resizes casterLod_, which reads as a
    // LOD change, which sets VSM forceAll -- a full page-pool re-render. In a trace that lands in
    // Pass_VsmPageRender, nowhere near here, so without this scope the cause is invisible.
    CPU_SCOPE(ProfilerScopes::kShadowCastersRebuild);

    // Snapshot the shadow LOD curve this build baked into the per-view LOD tables
    // (viewLod_/perViewGroup_/groupLodMega_). Scene compares both knobs to the live globals and
    // re-Rebuilds on a change.
    builtShadowLod_ = render::g_shadowLodBias;
    builtShadowLodBiasNearTier_ = render::g_shadowLodBiasNearTier;
    builtShadowLodTierStride_ = render::ShadowLodTierStride();

    size_t casterCount = 0;
    for (const auto& obj : objects)
    {
        if (IsCaster(obj.get())) { casterCount += CasterSlots(obj.get()); }
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

    staticCount_ = static_cast<std::uint32_t>(casterCount);
    cpuInstances_.assign(casterCount, render::InstancePerObject{});
    cpuBounds_.assign(casterCount, render::CasterBounds{});
    pending_.assign(casterCount, 0);
    // The material-content version each caster's mirror was last filled from. Rebuild fills every
    // record below, so seeding these from the objects happens there; 0 here only sizes the array.
    cpuMaterialVersion_.assign(casterCount, 0u);
    // S4: eligibility is re-decided below for every object in the registry; everything else (a
    // non-caster, a GI object, an object that just stopped casting) must read as CPU-drawn.
    casterEligible_.assign(casterCount, 0u);
    std::vector<GroupGbuf> groupGbufCpu; // per static group, pushed as groups are allocated
    for (const auto& o : objects) { if (o) { o->SetGBufferIndirect(false); } }

    // (1) STATIC casters: fill per-caster data + assign mesh-groups (the cull groups draws by mesh,
    // so the indirect arg/count buffers are sized per (view, mesh-group)). B3: one caster SLOT per
    // (object, submesh) and one group per (mesh, submesh) — a mesh's groups are allocated
    // contiguously first-seen, so group(mesh, s) = meshToGroup[mesh] + s. All of an object's slots
    // share its instance data/bounds (per-submesh bounds are a future refinement).
    std::unordered_map<const Mesh*, std::uint32_t> meshToGroup; // mesh -> FIRST group id
    std::unordered_map<const Mesh*, std::string> meshPath;      // Tessy N2: mesh -> its geometry path
    std::vector<const Mesh*> casterMesh(casterCount, nullptr);
    std::vector<std::uint32_t> casterSub(casterCount, 0u);      // submesh ordinal within casterMesh
    std::vector<std::uint32_t> staticDynamic(casterCount, 0u); // per static caster: IsDynamicCaster (VSM page-cache invalidation)
    // Local-light plan S7: per caster, the object sways in the wind (its BASE strength -- the distance fade would flip a
    // caster at the fade radius between static and movable as the camera moves, and every flip empties the cache).
    casterWindy_.assign(casterCount, 0u);
    // C2: per-group shadow-mask table, filled when a mesh is FIRST seen — the first object using
    // a (mesh, slot) defines the group's mask (an object overriding a shared mesh's slot keeps
    // the first object's shadow mask; same shared-mesh semantics as the mega buffer / RT BLAS).
    std::vector<DirectX::XMUINT2>& groupMaskCpu = groupMaskCpu_; // per group: {albedo slot (~0 = opaque), asuint(cutoff)}
    groupMaskCpu.clear();
    groupMaskMd_.clear(); // A6: the masked groups' MaterialData, for the per-frame bindless refresh
    maskedGroupCount_ = 0;
    maskedAlbedoSrvs_.fill({});
    maskedAlbedoCount_ = 0;
    hasWindCasters_ = false; // W5: recomputed below over the static set + the folded GI objects
    bool maskedOverflow = false;
    std::uint32_t nextGroup = 0;
    std::uint32_t chunkedMeshes = 0; // meshes whose submeshes are spatial chunks (independent casters)
    size_t idx = 0;
    for (const auto& objPtr : objects)
    {
        RenderableObjectBase* obj = objPtr.get();
        if (!IsCaster(obj)) { continue; }
        const RenderableObject* ro = obj->AsRenderableObject();
        const Mesh* mesh = ro ? ro->GetMesh() : nullptr; // non-null: IsCaster requires it
        const size_t slots = CasterSlots(obj);
        if (mesh && meshToGroup.find(mesh) == meshToGroup.end())
        {
            meshToGroup.emplace(mesh, nextGroup);
            meshPath.emplace(mesh, ro ? ro->GetGeometryPath() : std::string()); // Tessy N2: named when it has no clusters
            if (mesh->IsChunkedSubmeshes()) { ++chunkedMeshes; }
            GBufferRenderable* gb = obj->AsGBufferRenderable();
            for (size_t s = 0; s < slots; ++s)
            {
                // S4: the group's G-buffer identity = this FIRST object's material slot for the
                // submesh (textures + surface params) and its indirect PSO.
                {
                    GroupGbuf gg{};
                    if (gb)
                    {
                        const std::uint32_t slot = MaterialSlotOf(mesh, s, gb->SlotCount());
                        gg.md = gb->GetMaterialDataForSlot(slot);
                        gg.mat = gb->IndirectGraphicsMaterialForSlot(slot); // null for a glass-only slot
                        // Glass with no opaque texel (GBufferRenderable::SlotIsGlassOnly): the group draws
                        // nothing anywhere -- its index count is zeroed below for the camera and every
                        // shadow view; the slot's glass is drawn by Main_Translucent.
                        gg.glassOnly = gb->SlotIsGlassOnly(slot);
                    }
                    groupGbufCpu.push_back(gg);
                }
                DirectX::XMUINT2 gm{ 0xFFFFFFFFu, 0u };
                const MaterialData* md = gb ? gb->GetMaterialDataForSlot(s) : nullptr;
                const MaterialData* maskMd = nullptr;
                if (md && md->alphaMask && md->hasAlbedo)
                {
                    // A6: the bindless masked PSO reads the albedo by heap index, refreshed per
                    // frame from maskMd (RefreshGroupMaskForFrame) -- no cap. The table PSO keeps
                    // the 16-slot cap below.
                    maskMd = md;
                    ++maskedGroupCount_;
                    const float cutoff = md->ShadowAlphaCutoff(); // a glass split's is raised: dithered glass texels
                    std::memcpy(&gm.y, &cutoff, sizeof(gm.y));
                    if (maskedAlbedoCount_ < kMaxMaskedGroups)
                    {
                        maskedAlbedoSrvs_[maskedAlbedoCount_] = md->albedo.GetSRVCPU();
                        gm.x = maskedAlbedoCount_++;
                    }
                    else
                    {
                        maskedOverflow = true; // over the t3 table cap -> this group casts solid
                    }
                }
                groupMaskCpu.push_back(gm);
                groupMaskMd_.push_back(maskMd);
            }
            nextGroup += static_cast<std::uint32_t>(slots);
        }
        render::InstancePerObject inst{};
        render::CasterBounds bnd{};
        const GBufferRenderable* gbFoliage = obj->AsGBufferRenderable();
        FillInstance(obj, inst);
        FillBounds(obj, bnd);
        // W5: a wind caster animates in the VERTEX shader — its transform never changes, so the
        // mover-based "nothing changed" tests would freeze its shadow. Remember that we have one.
        // Only a mesh that CASTS: grass with its shadow switched off must not keep VSM redrawing for the wind.
        if (inst.windStrength > 0.0f && obj->CastsShadow()) { hasWindCasters_ = true; }
        const std::uint32_t dyn = obj->IsDynamicCaster() ? 1u : 0u;
        const bool chunked = mesh && mesh->IsChunkedSubmeshes();
        // S4: the object draws through the indirect G-buffer when EVERY submesh's slot has the
        // indirect PSO and the same MaterialData as the group's first object -- otherwise a
        // second object overriding a shared mesh's material would draw with the first one's
        // textures. All-or-nothing per object, so the CPU tail and the indirect pass never split
        // one object between them.
        bool eligible = gbFoliage != nullptr && mesh != nullptr;
        if (eligible)
        {
            const std::uint32_t g0 = meshToGroup[mesh];
            for (size_t s = 0; s < slots && eligible; ++s)
            {
                const std::uint32_t slot = MaterialSlotOf(mesh, s, gbFoliage->SlotCount());
                const GroupGbuf& gg = groupGbufCpu[g0 + s];
                if (gbFoliage->SlotIsGlassOnly(slot))
                {
                    eligible = gg.glassOnly; // nothing to draw, as long as the group draws nothing too
                    continue;
                }
                Material* mat = gbFoliage->IndirectGraphicsMaterialForSlot(slot);
                eligible = mat != nullptr && mat == gg.mat && gbFoliage->GetMaterialDataForSlot(slot) == gg.md;
            }
        }
        obj->SetGBufferIndirect(eligible);
        for (size_t s = 0; s < slots; ++s)
        {
            // S4: each caster id is one submesh, and since S4 its record carries THAT submesh's
            // slot params (the shadow VS reads world/wind only, so the shadow path is unchanged;
            // the per-slot foliage weight the shadow VS does read is the slot's, as before).
            FillInstanceSlot(obj, s, cpuInstances_[idx]);
            // The rebuild primes EVERY ring region from this record, so a record carrying motion
            // (prev != world: the object moved on the last sync -- dragged, then Ctrl+D) would put
            // that motion into all of them for good. Give it the same settle window a mover gets
            // in UpdateForFrame, and the refills converge prev onto world in every region.
            if (std::memcmp(&cpuInstances_[idx].world, &cpuInstances_[idx].prevWorld,
                    sizeof(cpuInstances_[idx].world)) != 0)
            {
                pending_[idx] = static_cast<std::uint8_t>(render::kFrameCount + 1);
            }
            cpuBounds_[idx] = bnd;
            if (chunked) { FillChunkBounds(ro, s, bnd, cpuBounds_[idx]); }
            // Seed the version this record was filled from, or the first UpdateForFrame after any
            // rebuild would see a mismatch on EVERY caster and re-fill the whole scene once.
            if (idx < cpuMaterialVersion_.size())
            {
                const GBufferRenderable* gbVer = obj->AsGBufferRenderable();
                cpuMaterialVersion_[idx] = gbVer ? gbVer->MaterialContentVersion() : 0u;
            }
            casterMesh[idx] = mesh;
            casterSub[idx] = static_cast<std::uint32_t>(s);
            staticDynamic[idx] = dyn;
            casterWindy_[idx] = (gbFoliage && gbFoliage->GetWindStrength() > 0.0f) ? 1u : 0u; // S7
            casterEligible_[idx] = eligible ? 1u : 0u;
            ++idx;
        }
    }
    const std::uint32_t staticGroups = nextGroup;
    // Chunked-terrain LOD: keep the mesh -> first-group mapping so the per-frame override refresh
    // (RefreshChunkGroupLods) can address a chunk's group without re-deriving the layout.
    meshFirstGroup_ = meshToGroup;

    // (2) GI→VSM reservation (Step 3): fold each GPU-instanced caster's instances into the caster
    // set as an id sub-range [giBase, giBase+count) after the static casters, one mesh-group per GI
    // object (all its instances share one mesh). Still DORMANT: the cull count stays at Nstatic
    // (see RecordCull), so these ids/groups are allocated + wired but never visited or drawn yet —
    // Step 4 runs the scatter + bumps the cull count. Respect the 64-group cap (VSM_MAX_SETUP_GROUPS):
    // over-cap GI objects are left unfolded and keep drawing via the CPU RenderShadow tail (Step 5).
    constexpr std::uint32_t kMaxGroups = vsm::kMaxMeshGroups; // == VSM_MAX_SETUP_GROUPS in the shader
    giCasters_.clear();
    giFoldableInstances_ = 0;
    std::uint32_t totalCount = static_cast<std::uint32_t>(casterCount);
    std::uint32_t numGroups = staticGroups;
    std::vector<const Mesh*>   giGroupMesh;       // folded GI group (relative id staticGroups+j) -> mesh
    std::vector<std::uint32_t> giGroupIndexCount; // its base-mesh index count
    bool giCapped = false;
    for (const auto& objPtr : objects)
    {
        RenderableObjectBase* obj = objPtr.get();
        if (!IsGiFoldable(obj)) { continue; }
        const std::uint32_t giCount = obj->GetInstanceCasterCount();
        giFoldableInstances_ += giCount;
        if (numGroups >= kMaxGroups) { giCapped = true; continue; } // over cap -> CPU tail (Step 5)
        const RenderableObject* ro = obj->AsRenderableObject();
        const Mesh* mesh = ro ? ro->GetMesh() : nullptr; // non-null: IsGiFoldable requires it
        if (!mesh) { continue; }

        GiCaster gc{};
        gc.obj = obj;
        gc.giBase = totalCount;
        gc.count = giCount;
        const AABB& mb = mesh->GetBoundingBox();
        if (mb.IsValid())
        {
            const Math::float3 c = mb.GetCenter();
            const Math::float3 e = mb.GetHalfExtents();
            gc.aabbCenter = DirectX::XMFLOAT4(c.x, c.y, c.z, 0.0f);
            gc.aabbExtent = DirectX::XMFLOAT4(e.x, e.y, e.z, 0.0f);
        }
        if (const GBufferRenderable* giGb = obj->AsGBufferRenderable())
        {
            if (giGb->CurrentDrawParams().windStrength > 0.0f) { hasWindCasters_ = true; } // W5
        }
        giCasters_.push_back(gc);
        giGroupMesh.push_back(mesh);
        giGroupIndexCount.push_back(static_cast<std::uint32_t>(mesh->GetIndexCount()));
        totalCount += giCount;
        ++numGroups;
    }

    count_ = totalCount;
    numMeshGroups_ = numGroups;
    numVirtualGroups_ = numMeshGroups_ * render::kMaxShadowLods; // S3.6: one bucket per (group, receiver LOD)
    numStaticGroups_ = staticGroups; // groups below this are static (LOD-biased); the rest are GI (LOD0)
    // S4: per-group G-buffer identity (static groups; GI groups draw through their own path).
    groupGbuf_ = std::move(groupGbufCpu);
    groupGbuf_.resize(numMeshGroups_);
    eligibleCasterCount_ = 0;
    for (std::uint8_t e : casterEligible_) { eligibleCasterCount_ += e; }

    // (3) group id -> Mesh*: static groups from the first-seen map (B3: a mesh's submesh groups
    // are contiguous and all map to it), then one per folded GI object.
    groupMesh_.assign(numMeshGroups_, nullptr);
    for (const auto& kv : meshToGroup)
    {
        const size_t slots = std::max<size_t>(kv.first->GetSubmeshCount(), 1u);
        for (size_t s = 0; s < slots && kv.second + s < staticGroups; ++s)
        {
            groupMesh_[kv.second + s] = kv.first;
        }
    }
    for (size_t j = 0; j < giGroupMesh.size(); ++j)
    {
        groupMesh_[staticGroups + j] = giGroupMesh[j];
    }

    // (4) Per-group caster count + index range, per-caster group id, and per-caster dynamic flag
    // (all sized to the TOTAL count_). casterDynamicId feeds the VSM page-cache invalidation: 1 for
    // GI (always animating) + any static caster whose object reports IsDynamicCaster.
    // B3: static groups are (mesh, submesh) ranges — index count + START come from the LOD0
    // submesh table (the GPU shadow paths always draw LOD0). GI groups stay whole-buffer.
    std::vector<std::uint32_t> groupCount(numMeshGroups_, 0);
    std::vector<std::uint32_t> groupIndexCount(numMeshGroups_, 0);
    std::vector<std::uint32_t> groupStartIndex(numMeshGroups_, 0);
    std::vector<std::uint32_t> casterGroupId(std::max<std::uint32_t>(count_, 1u), 0);
    // Per-caster meta word: bit0 = dynamic (VSM page-cache invalidation); bits 1+ = the object's
    // slot count, stored ONLY on its FIRST slot (0 on continuation slots). The VSM per-page cull
    // tests bounds once per OBJECT and applies the result to all its slots — B3 multiplied the
    // caster count by submeshes, and without this the (pages x casters) plane tests scale with it.
    std::vector<std::uint32_t> casterMeta(std::max<std::uint32_t>(count_, 1u), 0);
    for (size_t i = 0; i < casterCount; ++i)
    {
        const Mesh* mesh = casterMesh[i];
        const std::uint32_t g = (mesh ? meshToGroup[mesh] : 0u) + casterSub[i];
        casterGroupId[i] = g;
        // Chunked meshes break the "one object, one box, N slots" rule on purpose: every chunk owns
        // its own bounds, so every chunk must LEAD ITSELF (slot count 1) or the cull would test the
        // first chunk's box and hand the verdict to all 35 others — the exact opposite of chunking.
        const std::uint32_t slots = (mesh && mesh->IsChunkedSubmeshes())
            ? 1u
            : ((casterSub[i] == 0u && mesh)
                ? static_cast<std::uint32_t>(std::max<size_t>(mesh->GetSubmeshCount(), 1u))
                : 0u);
        casterMeta[i] = (slots << 1) | (staticDynamic[i] & 1u);
        if (g < numMeshGroups_) { ++groupCount[g]; }
    }
    // perGroup_ carries LOD0 ranges + the visible-list base. Only its base (.x) is read at draw time
    // now (by shadow_cull_cs); the per-VIEW LOD ranges live in perViewGroup_ / groupLodMega_ below.
    for (const auto& kv : meshToGroup)
    {
        const auto& subs = kv.first->GetSubmeshes();
        const size_t slots = std::max<size_t>(subs.size(), 1u);
        for (size_t s = 0; s < slots && kv.second + s < staticGroups; ++s)
        {
            const std::uint32_t g = kv.second + static_cast<std::uint32_t>(s);
            if (s < subs.size())
            {
                groupIndexCount[g] = subs[s].indexCount;
                groupStartIndex[g] = subs[s].indexOffset;
            }
            else
            {
                groupIndexCount[g] = static_cast<std::uint32_t>(kv.first->GetIndexCount());
            }
            // A glass-only slot's group: no triangles in the Rung0 / Legacy args (shadow_cull_clear_cs).
            if (g < groupGbuf_.size() && groupGbuf_[g].glassOnly) { groupIndexCount[g] = 0u; }
        }
    }
    for (size_t j = 0; j < giCasters_.size(); ++j)
    {
        const std::uint32_t g = staticGroups + static_cast<std::uint32_t>(j);
        const GiCaster& gc = giCasters_[j];
        for (std::uint32_t c = 0; c < gc.count; ++c) { casterGroupId[gc.giBase + c] = g; casterMeta[gc.giBase + c] = (1u << 1) | 1u; } // each GI instance: own 1-slot object, dynamic
        groupCount[g] = gc.count;
        groupIndexCount[g] = giGroupIndexCount[j];
    }
    // Prefix-sum group caster counts -> each group's base slice offset within a view's region (sum
    // of counts == count_, so a group's visible run never overflows its slice). A GI group's base
    // equals its giBase (GI ids are contiguous immediately after all static casters).
    std::vector<std::uint32_t> groupBase(numMeshGroups_, 0);
    for (std::uint32_t g = 1; g < numMeshGroups_; ++g)
    {
        groupBase[g] = groupBase[g - 1] + groupCount[g - 1];
    }

    // --- Per-view shadow LOD (tunable tier stride + g_shadowLodBias) -----------------------------------
    // Cull-view layout: [cascades | spots | point-faces | clipmap | the atlas modes' extra spots and faces]
    // (render::ShadowView*Slot, local-light shadow plan S9). A view's tier picks a base LOD
    // (near = fine, far = coarse); the global bias shifts it. Locals use the near tier. The final LOD
    // is clamped per mesh by the tables below (a mesh may have fewer LODs than the view asks for).
    const int lodCap = static_cast<int>(render::kMaxShadowLods) - 1;
    const std::uint32_t maxViews = render::MaxShadowViews(); // S9e: the view capacity this rebuild is sized for
    viewLod_.assign(maxViews, 0u);
    {
        constexpr std::uint32_t kCasc = vsm::kNumCascades;                       // [0, 4)
        constexpr std::uint32_t kClipmapBegin = render::ShadowViewClipmapSlot(0u);
        for (std::uint32_t v = 0; v < maxViews; ++v)
        {
            std::uint32_t tier;
            if (v < kCasc)                       { tier = v; }                  // CSM cascade index (near->far)
            else if (v < kClipmapBegin)          { tier = 0u; }                 // local light -> near tier
            else if (v < render::kVsmShadowViews) { tier = v - kClipmapBegin; } // VSM clipmap level (near->far)
            else                                 { tier = 0u; }                 // S9: the atlas modes' extra locals
            // The bias is a shift of the TIER curve, and a local light has no tier -- it is pinned
            // at 0 and then biased anyway, which is how a spot ended up rasterizing its casters two
            // levels coarser than the camera drew them. On thin shells that is self-shadow blobs:
            // measured on demo.json's tent, bias 0 is clean, bias 1 smudges it, bias 2 covers it in
            // dark patches, and Legacy CSM is clean at every setting. Locals shadow exactly the
            // near-field geometry the camera is closest to, so there is no distance to hide a
            // coarser caster behind. Cascades and clipmaps keep the bias -- that is where the tier
            // curve is real and where the triangles it saves actually live.
            // Applies to the NEAREST tier of ANY view kind, not just locals: a local light is
            // pinned at tier 0, and so is clipmap level 0, and it was the clipmap that was
            // biasing the tent (toggling locals alone moved the canvas metric by 0 px across 5
            // interleaved samples -- 3076 vs 3076).
            const bool nearTier = (tier == 0u);
            int lod = render::ShadowTierBaseLod(tier)
                    + ((nearTier && !render::g_shadowLodBiasNearTier) ? 0 : render::g_shadowLodBias);
            lod = lod < 0 ? 0 : (lod > lodCap ? lodCap : lod);
            // S3.6: CSM CASCADES take no per-view LOD at all. UE has no per-cascade coarsening
            // either -- r.Shadow.LODDistanceFactor.CascadeScale defaults to 0 and the shadow depth
            // pass reuses CurrentView.PrimitivesLODMask, i.e. the LOD the CAMERA picked. Our caster
            // LOD now comes from the receiver (casterLod_ -> virtual groups), so a tier curve on top
            // could only push the caster COARSER than its receiver, which is the tent-smudge family.
            // viewLod_ still serves VSM, which reads only the local (+4) and clipmap entries.
            if (v < kCasc) { lod = 0; }
            viewLod_[v] = static_cast<std::uint32_t>(lod);
        }
    }

    // Chunked-terrain LOD note: the per-view tables below carry the PLAIN view LOD. Chunk groups
    // get their ABSOLUTE per-frame camera-tier override later (UpdateForFrame -> groupLodOverride_
    // -> the VSM setup CB), so the caster always matches what the camera drew — Rebuild bakes
    // nothing camera-dependent. The Rung0-args/Legacy-indirect fallback keeps the view LOD for
    // chunk groups (documented divergence: only the VSM page path and the Legacy per-submesh CPU
    // loop can be per-chunk).
    const auto biasedLod = [&](std::uint32_t v, std::uint32_t) -> std::uint32_t
    {
        return v < viewLod_.size() ? viewLod_[v] : 0u;
    };

    // --- Per (view, group) draw ranges at that view's LOD (seeds the cull-clear args -> Legacy + Rung0).
    // For a static submesh group, use the mesh's clamped view LOD; GI groups stay whole-buffer LOD0.
    // Layout mirrors the args: index = view * numMeshGroups_ + group. `base` (visible-list slice) is
    // view-independent but replicated per view so the cull-clear reads one struct.
    // S3.6: the per-(view, group) LOD range table is GONE. With the caster LOD coming from the
    // receiver, the range depends on (group, lod) only -- the view axis carried no information,
    // and the one place that read it indexed by group anyway (see shadow_cull_clear_cs).

    // --- Per (group, lod) draw ranges, mesh-IB-relative (the VSM setup CB's gGroupLodMega). Filled for
    // EVERY group/lod here so the non-mega fallback works; the mega block below adds the absolute mega
    // start (.x) + baseVertex (.w). Entry = {megaAbsStart, lodRelStart, indexCount, baseVertex}.
    groupLodMega_.assign(static_cast<size_t>(numMeshGroups_) * render::kMaxShadowLods * 4u, 0u);
    for (std::uint32_t g = 0; g < numMeshGroups_; ++g)
    {
        const Mesh* m = (g < groupMesh_.size()) ? groupMesh_[g] : nullptr;
        const bool isStatic = (m && g < staticGroups);
        std::uint32_t sOrd = 0;
        if (isStatic) { const auto it = meshToGroup.find(m); sOrd = (it != meshToGroup.end()) ? (g - it->second) : 0u; }
        for (std::uint32_t L = 0; L < render::kMaxShadowLods; ++L)
        {
            std::uint32_t lodRel = 0u, count = groupIndexCount[g]; // GI / no-LOD default = whole LOD0
            if (isStatic)
            {
                const UINT cl = m->ClampExplicitLod(L);
                // The EXPLICIT LOD's submesh table, as its index buffer below: SubmeshesForLod applies the runtime
                // overrides (lod.forced / lod.enabled 0), and a Rebuild taken under one put that LOD's submesh offsets
                // on every LOD's buffer -- the local views drew another part of the mesh (Tessy N4 found it).
                const auto& subs = *m->GetLodDrawInfo(cl).submeshes;
                if (sOrd < subs.size()) { lodRel = subs[sOrd].indexOffset; count = subs[sOrd].indexCount; }
                else                    { lodRel = 0u; count = m->GetLodIndexCount(cl); }
                // A glass-only slot's group: no triangles for the VSM pages or the camera's indirect G-buffer.
                if (g < groupGbuf_.size() && groupGbuf_[g].glassOnly) { count = 0u; }
            }
            const size_t o = (static_cast<size_t>(g) * render::kMaxShadowLods + L) * 4u;
            groupLodMega_[o + 1] = lodRel;
            groupLodMega_[o + 2] = count;
        }
    }

    // S3.6 keeps a CPU copy: the per-frame virtual-group bucketing needs caster -> group without
    // reading back the GPU buffer.
    casterGroupCpu_ = casterGroupId;

    // Upload the static cull inputs (region 0; never rewritten after Rebuild). CasterGroup is sized
    // to the TOTAL count_ so Step 4's cull (numCasters=count_) can read the GI ids' group.
    if (EnsureRing(renderer, casterGroup_, std::max<std::uint32_t>(count_, 1u), sizeof(std::uint32_t), L"ShadowGpuData.CasterGroup") &&
        count_ > 0)
    {
        std::memcpy(casterGroup_.Region(0), casterGroupId.data(), count_ * sizeof(std::uint32_t));
    }
    // Per-caster meta (dynamic flag + first-slot object slot count; region 0, static after Rebuild).
    if (EnsureRing(renderer, casterMeta_, std::max<std::uint32_t>(count_, 1u), sizeof(std::uint32_t), L"ShadowGpuData.CasterMeta") &&
        count_ > 0)
    {
        // Every region, not just 0: the SRV is per-frame now and a frame that has not been
        // republished yet must still read a valid table.
        for (UINT f = 0; f < render::kFrameCount; ++f)
        {
            if (auto* dst = casterMeta_.Region(f))
            {
                std::memcpy(dst, casterMeta.data(), count_ * sizeof(std::uint32_t));
            }
        }
        cpuCasterMeta_ = casterMeta;
    }
    // B3: uint4 per group — {visible-list base, index count, start index, caster count}. The clear
    // CS seeds the indirect args' StartIndexLocation from .z so submesh groups draw their range;
    // .w sizes the per-LOD instance buckets in the VSM scatter (each of a group's kMaxShadowLods
    // buckets must be able to hold the WHOLE group, since an instance can land in any one of them).
    if (EnsureRing(renderer, perGroup_, std::max<size_t>(numMeshGroups_, 1), 4 * sizeof(std::uint32_t), L"ShadowGpuData.PerGroup") &&
        numMeshGroups_ > 0)
    {
        auto* pg = reinterpret_cast<std::uint32_t*>(perGroup_.Region(0));
        for (std::uint32_t g = 0; g < numMeshGroups_; ++g)
        {
            pg[g * 4 + 0] = groupBase[g];
            pg[g * 4 + 1] = groupIndexCount[g];
            pg[g * 4 + 2] = groupStartIndex[g];
            pg[g * 4 + 3] = groupCount[g];
        }
    }

    // C2: per-group shadow-mask table (uint2; region 0, static like perGroup_). GI groups (and
    // any group past the static set) are opaque.
    groupMaskCpu.resize(numMeshGroups_, DirectX::XMUINT2{ 0xFFFFFFFFu, 0u });
    groupMaskMd_.resize(numMeshGroups_, nullptr);
    hasMaskedGroups_ = maskedGroupCount_ > 0;
    BuildGBufferCommandOrder_(); // A6: groupGbuf_ and groupMesh_ are final here
    if (maskedOverflow)
    {
        LOG_WARNING(logging::LogCategory::RenderShadow,
                    "masked shadow groups exceed the albedo table cap; excess groups cast solid shadows on the table path (the bindless path has no cap)");
    }
    groupMaskRegion_ = 0;
    if (EnsureRing(renderer, groupMask_, std::max<size_t>(numMeshGroups_, 1), sizeof(DirectX::XMUINT2), L"ShadowGpuData.GroupMask") &&
        numMeshGroups_ > 0)
    {
        std::memcpy(groupMask_.Region(0), groupMaskCpu.data(), numMeshGroups_ * sizeof(DirectX::XMUINT2));
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
    tessyReady_ = false; // Tessy N2: the tables follow the mega buffers (EnsureMegaBuffer uploads both)
    megaVBBytes_ = megaIBBytes_ = megaStride_ = 0;
    tessyDagBase_.clear();
    megaIndexFormat_ = DXGI_FORMAT_R32_UINT;
    baseVertex_.assign(numMeshGroups_, 0u);
    startIndex_.assign(numMeshGroups_, 0u);
    megaCopy_.clear();
    // NO group cap here any more. This used to bail above vsm::kMaxMeshGroups because the VSM setup
    // shader addressed the per-(group,lod) ranges through a CB array of that fixed size; they are an
    // SRV now (groupLodMegaBuf_ -> t9), sized by numMeshGroups_. The mega layout itself never had a
    // 64 dependency -- it iterates megaCopy_ per UNIQUE MESH, and baseVertex_/startIndex_ are sized
    // by the group count. Keeping the gate would have cost the >64 case its fast path for nothing.
    if (numMeshGroups_ > 0)
    {
        // Per-view shadow LOD: lay the mega buffer out per UNIQUE mesh — one VB copy + its LOD index
        // buffers concatenated ([LOD0|LOD1|...]) — so different shadow views draw different LODs of the
        // same mesh from one buffer. A static mesh contributes all its LODs; a GI-only mesh just LOD0.
        // The per-(group, lod) start/count/baseVertex land in groupLodMega_ for the VSM setup shader.
        bool uniform = true;
        UINT stride0 = 0;
        DXGI_FORMAT fmt0 = DXGI_FORMAT_UNKNOWN;
        std::unordered_map<const Mesh*, std::uint32_t> meshSlot; // mesh -> megaCopy_ ordinal
        for (std::uint32_t g = 0; g < numMeshGroups_; ++g)
        {
            const Mesh* m = groupMesh_[g];
            ID3D12Resource* vb = m ? m->GetVertexBufferResource() : nullptr;
            ID3D12Resource* ib = m ? m->GetIndexBufferResource() : nullptr; // LOD0 exists for any mesh
            if (!vb || !ib) { uniform = false; break; }
            if (meshSlot.empty()) { stride0 = m->GetVertexStride(); fmt0 = m->GetIndexFormat(); }
            else if (m->GetVertexStride() != stride0 || m->GetIndexFormat() != fmt0) { uniform = false; break; }
            if (meshSlot.find(m) == meshSlot.end())
            {
                // Static-used meshes carry all LODs; GI-only meshes just LOD0 (they draw whole-buffer).
                // Static groups precede GI groups, so first-see order classifies correctly.
                const UINT lodCount = (g < staticGroups) ? m->GetLodCount() : 1u;
                meshSlot.emplace(m, static_cast<std::uint32_t>(megaCopy_.size()));
                const bool dag = g < staticGroups && m->HasDag() && m->GetDagIndexBufferResource() != nullptr; // N7
                megaCopy_.push_back(MegaCopy{ m, static_cast<UINT>(vb->GetDesc().Width), lodCount, dag });
            }
        }
        if (uniform && stride0 > 0 && fmt0 == DXGI_FORMAT_R32_UINT) // consolidated path is R32-only
        {
            const UINT idxBytes = 4u;
            std::vector<std::uint32_t> meshBaseVertex(megaCopy_.size(), 0u); // in vertices, into megaVB_
            // meshLodIBBase[m][L] = index offset of mesh m's LOD-L IB within megaIB_ (kMaxShadowLods deep;
            // entries past lodCount reuse the coarsest present LOD's base, matching ClampExplicitLod).
            std::vector<std::array<std::uint32_t, render::kMaxShadowLods>> meshLodIBBase(megaCopy_.size());
            UINT voff = 0, ioff = 0;
            for (size_t mi = 0; mi < megaCopy_.size(); ++mi)
            {
                const MegaCopy& mc = megaCopy_[mi];
                meshBaseVertex[mi] = voff / stride0; // exact: width is stride*count
                UINT lodBase = ioff / idxBytes;
                std::uint32_t lastBase = lodBase;
                for (std::uint32_t L = 0; L < render::kMaxShadowLods; ++L)
                {
                    if (L < mc.lodCount)
                    {
                        meshLodIBBase[mi][L] = lodBase;
                        lastBase = lodBase;
                        lodBase += mc.mesh->GetLodIndexCount(L); // advance past this LOD's IB
                    }
                    else { meshLodIBBase[mi][L] = lastBase; } // clamp: coarsest present LOD
                }
                if (mc.dag) // N7: the cluster DAG's index buffer after the LODs
                {
                    tessyDagBase_[mc.mesh] = lodBase;
                    lodBase += mc.mesh->GetDagIndexCount();
                }
                voff += mc.vbBytes;
                ioff += (lodBase - (ioff / idxBytes)) * idxBytes; // total indices of all this mesh's LODs
            }
            // Add the mega-absolute start (.x) + baseVertex (.w) to each group's per-LOD entries (the
            // lod-relative start + count were filled mesh-relative in the pre-pass above). GI groups use
            // the mesh's LOD0 base at every LOD index (they draw the whole LOD0 buffer). megaStart =
            // this LOD's mega base + the lod-relative submesh offset already stored in .y.
            for (std::uint32_t g = 0; g < numMeshGroups_; ++g)
            {
                const Mesh* m = groupMesh_[g];
                const std::uint32_t mo = meshSlot[m];
                baseVertex_[g] = meshBaseVertex[mo];
                startIndex_[g] = meshLodIBBase[mo][0];
                const bool isStatic = (g < staticGroups);
                for (std::uint32_t L = 0; L < render::kMaxShadowLods; ++L)
                {
                    const UINT cl = m->ClampExplicitLod(L);
                    const size_t o = (static_cast<size_t>(g) * render::kMaxShadowLods + L) * 4u;
                    const std::uint32_t lodBase = isStatic ? meshLodIBBase[mo][cl] : meshLodIBBase[mo][0];
                    groupLodMega_[o + 0] = lodBase + groupLodMega_[o + 1]; // mega base + lod-relative start
                    groupLodMega_[o + 3] = meshBaseVertex[mo];
                }
            }
            megaVBBytes_ = voff; megaIBBytes_ = ioff;
            megaStride_ = stride0; megaIndexFormat_ = fmt0;
            megaWanted_ = (megaVBBytes_ > 0 && megaIBBytes_ > 0);
        }
        else
        {
            megaCopy_.clear();
        }
    }
    BuildTessyTables_(meshPath); // Tessy N2: needs the mega-absolute starts the block above wrote

    // Per-(group,lod) mega ranges -> their SRV. Written LAST because the mega block above patches
    // groupLodMega_ in place (absolute starts + base vertex). Static region 0: it only changes when
    // the caster set is rebuilt, which is when this runs.
    if (numMeshGroups_ > 0 &&
        EnsureRing(renderer, groupLodMegaBuf_, groupLodMega_.size() / 4u,
                   4 * sizeof(std::uint32_t), L"ShadowGpuData.GroupLodMega"))
    {
        if (std::uint8_t* dst = groupLodMegaBuf_.Region(0))
        {
            std::memcpy(dst, groupLodMega_.data(), groupLodMega_.size() * sizeof(std::uint32_t));
        }
    }
    // The LOD tables are per-frame, but they must be VALID before the first RefreshCasterLods of
    // the new caster set — a rebuild can land between two frames' refreshes. Neutral defaults:
    // no chunk override, every caster at LOD0 (never coarser than any receiver for one frame).
    groupLodOverride_.assign(std::max<size_t>(numMeshGroups_, 1), -1);
    casterLod_.assign(std::max<std::uint32_t>(count_, 1u), render::kCasterLodExactBit); // EXACT LOD0 (see RefreshCasterLods)
    UploadCasterLods(renderer);

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
    // Plan S9e: the view CAPACITY (render::MaxShadowViews), which Scene::ReconcileShadowCapacity grows -- by rebuilding
    // this whole set, so the rows below always match the layout the frame uploads.
    const size_t numViews = render::MaxShadowViews();
    builtViews_ = static_cast<std::uint32_t>(numViews);
    // S3.6: the indirect args are indexed per (view, VIRTUAL group).
    const size_t groups = std::max<size_t>(numVirtualGroups_, 1);
    const size_t casters = std::max<size_t>(count_, 1); // TOTAL (static + GI): visible-list + unified width
    // P12.1. This buffer USED to declare NON_PIXEL_SHADER_RESOURCE and drifted: --canonical-check
    // reported it off-canonical on some levels and not others, and flipping the label alone merely
    // reversed the report. The cause is that it had TWO last-touchers, not one. RecordCull closes
    // by leaving it in INDIRECT_ARGUMENT (below), while VirtualShadowMap::PrepareRenderPass borrows
    // it as an SRV -- so whichever ran last decided the resting state, and whether the VSM page
    // render pass exists at all depends on VsmActive() && IsAllocated() && no skip-when-still. Hence
    // one level rested INDIRECT_ARGUMENT and another NON_PIXEL_SHADER_RESOURCE.
    //
    // The fix is the rule its two siblings below already follow: DECLARE THE STATE THE OWNER LEAVES
    // IT IN, and make every borrower hand it back. This is an indirect-argument buffer, the cull
    // ends there, and VSM now restores it at its consume point (VirtualShadowMap.cpp). Compare
    // VSM.PageDrawArgs, which declares INDIRECT_ARGUMENT and never drifted.
    EnsureUavRing(renderer, indirectArgs_, numViews * groups * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, L"ShadowGpuData.IndirectArgs");
    EnsureUavRing(renderer, visibleList_, numViews * casters * sizeof(std::uint32_t), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, L"ShadowGpuData.VisibleList");
    EnsureUavRing(renderer, indirectCounts_, numViews * sizeof(std::uint32_t), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"ShadowGpuData.IndirectCounts");
    // Local-light plan S6a: the atlas local views' compacted rows (non-empty records, mega offsets) and their counts,
    // written by shadow_args_compact_cs after the cull, read by ExecuteIndirect -- they rest where the draws read them.
    // Both are RAW views per region, and a RAW view's first element must sit on 16 bytes: 138 views x 4 B = 552 put
    // region 1 of the counts at element 138, which the debug layer rejects (and Release crashed on), so round up.
    {
        const size_t localViews = numViews - render::kShadowViewCascades;
        const auto align16 = [](size_t bytes) { return (bytes + 15u) & ~static_cast<size_t>(15u); };
        EnsureUavRing(renderer, localArgs_, align16(localViews * groups * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, L"ShadowGpuData.LocalArgs");
        EnsureUavRing(renderer, localCounts_, align16(localViews * sizeof(std::uint32_t)), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, L"ShadowGpuData.LocalCounts");
        // S7: the same pair again for the local views' STATIC casters (the cull's split rows) and its compacted twin.
        EnsureUavRing(renderer, staticArgs_, align16(localViews * groups * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, L"ShadowGpuData.StaticArgs");
        EnsureUavRing(renderer, staticVisibleList_, align16(localViews * casters * sizeof(std::uint32_t)), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, L"ShadowGpuData.StaticVisibleList");
        EnsureUavRing(renderer, localStaticArgs_, align16(localViews * groups * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, L"ShadowGpuData.LocalStaticArgs");
        EnsureUavRing(renderer, localStaticCounts_, align16(localViews * sizeof(std::uint32_t)), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, L"ShadowGpuData.LocalStaticCounts");
    }
    // S5b: the cascade HZB cull's buffers -- cascade rows only, otherwise the layout of the pair
    // above. The counters rest in UAV (the cull-clear zeroes them every frame, tested or not).
    {
        const size_t hzbViews = render::CascadeHzb::kCascades;
        EnsureUavRing(renderer, deferredList_, hzbViews * casters * sizeof(std::uint32_t), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"ShadowGpuData.DeferredList");
        EnsureUavRing(renderer, deferredCount_, kHzbStatsBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"ShadowGpuData.DeferredCount");
        EnsureUavRing(renderer, indirectArgsB_, hzbViews * groups * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, L"ShadowGpuData.IndirectArgsB");
        EnsureUavRing(renderer, visibleListB_, hzbViews * casters * sizeof(std::uint32_t), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, L"ShadowGpuData.VisibleListB");
    }
    // S4: the camera's own args (one row of virtual groups) and visible list -- two slots per
    // caster, since a crossfading caster sits in two LOD buckets.
    EnsureUavRing(renderer, camArgs_, groups * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, L"ShadowGpuData.CamArgs");
    EnsureUavRing(renderer, camVisibleList_, 2u * casters * sizeof(std::uint32_t), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, L"ShadowGpuData.CamVisibleList");
    // S5: the camera's deferred list (one entry per caster at most) and pass B's own pair, the
    // layout of the S4 pair above.
    EnsureUavRing(renderer, camDeferred_, casters * sizeof(std::uint32_t), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"ShadowGpuData.CamDeferred");
    EnsureUavRing(renderer, camArgsB_, groups * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, L"ShadowGpuData.CamArgsB");
    EnsureUavRing(renderer, camVisibleListB_, 2u * casters * sizeof(std::uint32_t), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, L"ShadowGpuData.CamVisibleListB");
    // A6: the merged G-buffer's commands (64 B per virtual group at most) for both passes, and the
    // CPU-written static part the expand kernel reads (3 x uint4 per command).
    EnsureUavRing(renderer, camCmds_, groups * Renderer::kBindlessIndirectCommandBytes, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, L"ShadowGpuData.CamCmds");
    EnsureUavRing(renderer, camCmdsB_, groups * Renderer::kBindlessIndirectCommandBytes, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, L"ShadowGpuData.CamCmdsB");
    EnsureRing(renderer, cmdStatic_, std::max<size_t>(groups, 1) * 3, static_cast<UINT>(sizeof(DirectX::XMUINT4)), L"ShadowGpuData.GBufferCmdStatic");
    // Step 2 (GI→VSM): DEFAULT-heap mirrors of instances_/bounds_, sized to `casters` per region.
    // RecordCull copies the ring's region into these each frame (verbatim at this step; Step 4 also
    // scatters GI casters into them). Their per-region SRVs feed the cull (bounds) + indirect VS (t0).
    EnsureUavRing(renderer, instancesUnified_, casters * sizeof(render::InstancePerObject), kInstancesReadState, L"ShadowGpuData.InstancesUnified");
    EnsureUavRing(renderer, boundsUnified_, casters * sizeof(render::CasterBounds), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"ShadowGpuData.BoundsUnified");
    // Instantiate the shared DRAW_INDEXED indirect command signature so it exists post-load.
    renderer->GetDrawIndexedCommandSignature();
    // Step 4: per-region UAVs for the cull outputs (depend on the just-decided sizes).
    RebuildCullDescriptors(renderer);
    RebuildUnifiedDescriptors(renderer); // Step 2: per-region SRVs onto the unified buffers
    valState_ = 0; // re-validate after a caster-set change
    // S7: a new caster set -- every cached local-light picture is stale. Not after a capacity-only rebuild: the casters
    // draw what they drew, and the Lamps' capacity growth (two doublings while the camera turned) refilled every light.
    if (!sameGeometry) { ++localStaticVersion_; }
    // ...and every cached VSM page (ComputePageRenderDecisions: forceAll once). It used to follow from the caster-LOD
    // table changing size, but a Cast Shadow switch keeps the mesh in the table (IsCaster) and only flips its bounds'
    // verdict: the pages it should now darken -- or stop darkening -- would stay as cached.
    casterLodsChanged_ = true;

    char buf[352];
    std::snprintf(buf, sizeof(buf),
        "[ShadowGpuData] rebuilt: %u casters (%u static + %u GI in %zu objs%s), %u mesh-groups "
        "(%u chunked of %u meshes; %u cast from local lights); %.2f KB instance + %.2f KB bounds x%u regions.\n",
        count_, staticCount_, count_ - staticCount_, giCasters_.size(),
        giCapped ? ", CAPPED" : "", numMeshGroups_, chunkedMeshes, static_cast<std::uint32_t>(meshToGroup.size()),
        // NOT a cap on the group count any more (directional shadows have none) — it is how many
        // groups the local-light per-page cull can still address. Printing it as "cap N" read like a
        // hard limit the moment a level went past it.
        (numMeshGroups_ < kMaxGroups) ? numMeshGroups_ : kMaxGroups,
        (instances_.capacity * sizeof(render::InstancePerObject)) / 1024.0,
        (bounds_.capacity * sizeof(render::CasterBounds)) / 1024.0,
        render::kFrameCount);
    // The group count is the number this whole feature is capped by (VSM_MAX_SETUP_GROUPS), so a
    // re-bake's effect on the caster set can be READ (session log) rather than assumed.
    logging::WriteRaw(logging::LogLevel::Info, logging::LogCategory::RenderShadow, buf);
    logFramesRemaining_ = 5;
}

std::uint32_t ShadowGpuData::UpdateForFrame(Renderer* renderer,
    const std::vector<std::unique_ptr<RenderableObjectBase>>& objects)
{
    if (!renderer) { return 0; }

    size_t newStatic = 0; // caster SLOTS (B3: one per (object, submesh)) — must match Rebuild's count
    std::uint32_t newGiInstances = 0;
    for (const auto& obj : objects)
    {
        if (IsCaster(obj.get())) { newStatic += CasterSlots(obj.get()); }
        else if (IsGiFoldable(obj.get())) { newGiInstances += obj->GetInstanceCasterCount(); }
    }

    // First use, or the caster set changed (level switch / editor spawn-delete) → full rebuild.
    // Track BOTH the static count and the total foldable GI instance count: a GI spawn/delete leaves
    // the static count unchanged but must re-run Rebuild (else groupMesh_/giCasters_ dangle). The
    // instances_/bounds_ rings are sized to the static count only (GI lives in the unified buffers).
    if (!instances_.Valid() || !bounds_.Valid() || newStatic != staticCount_ ||
        newGiInstances != giFoldableInstances_ ||
        newStatic > instances_.capacity || newStatic > bounds_.capacity)
    {
        // Rebuild re-creates the rings it outgrows (EnsureRing / EnsureUavRing release the old buffer AT ONCE), and
        // all kFrameCount regions live in one buffer: the frames still in flight -- on either queue -- read the old
        // one. Wait for them first, as Scene::RebuildShadowCasters does. Without it, switching the 104 bulbs of
        // wind_test's "Lamps" group on mid-session freed ShadowGpuData.Bounds under the GPU: a DRED page fault on
        // the freed buffer, DEVICE_HUNG about 1 time in 3 early in a session with async compute (2026-10-27).
        // Mid-session caster-set changes are editor edits; the first build has nothing in flight to wait for.
        // The Rebuild drops the mega buffers too, and only an upload batch makes them: Scene takes the request right
        // after this call (TakeMegaRebuildRequest). Until 2026-10-30 nothing did -- after the owner's "Lamps" switch-on
        // the local views lost their compaction and Tessy's atlas path for 13 s, until some full rebuild came along.
        const bool midSession = instances_.Valid() || bounds_.Valid();
        if (midSession) { renderer->WaitForPreviousFrame(); }
        Rebuild(renderer, objects);
        megaRebuildRequest_ = midSession;
        forceContentRefresh_ = false; // this frame's mover signal already covers the rebuild
        lastMoverCount_ = count_; // full rebuild -> treat everything as changed (don't skip VSM)
        return count_;
    }
    // (Chunked-terrain LOD overrides are refreshed by Scene AFTER PrepareViews — this function
    // runs BEFORE the frame's SelectLod, and reading last frame's tiers here would let the caster
    // lag the camera by one frame exactly on LOD-transition frames.)

    const UINT region = renderer->GetCurrentFrameIndex();
    if (region >= render::kFrameCount) { return 0; }
    auto* instBase = reinterpret_cast<render::InstancePerObject*>(instances_.Region(region));
    auto* boundBase = reinterpret_cast<render::CasterBounds*>(bounds_.Region(region));

    std::uint32_t idx = 0;
    std::uint32_t uploaded = 0;
    // W8: the distance fade scales windStrength from the CAMERA, so a swaying caster changes without
    // ever moving — and Step 7 below deliberately skips anything that has not moved, which would
    // leave the shadow swaying at full amplitude while the tree faded out. Re-check wind casters too,
    // but ONLY while the fade is actually enabled: with it off (the default) this is a single bool
    // and the O(movers) property is untouched.
    const bool windFadeActive = vfx::g_windFadeEnd > vfx::g_windFadeStart;
    for (const auto& objPtr : objects)
    {
        const RenderableObjectBase* obj = objPtr.get();
        if (!IsCaster(obj)) { continue; }
        const size_t slots = CasterSlots(obj); // B3: an object owns `slots` consecutive caster ids

        // Step 7: only movers are recomputed + re-uploaded. A static caster is skipped entirely
        // (no fill, no compare) — the per-frame cost is O(movers), not O(casters). The move
        // signal tracks transform changes, which is all a depth-only shadow entry depends on.
        const RenderableObject* ro = obj->AsRenderableObject();
        bool windFadeChanged = false;
        if (windFadeActive)
        {
            const GBufferRenderable* gbW = obj->AsGBufferRenderable();
            if (gbW && gbW->GetWindStrength() > 0.0f)
            {
                // Quantised so a slowly panning camera does not re-upload every caster every frame
                // (uploaded > 0 also drives the VSM "content changed" signal).
                const float now = gbW->EffectiveWindStrength(gbW->GetWindStrength());
                windFadeChanged = std::abs(now - cpuInstances_[idx].windStrength) > (1.0f / 255.0f);
            }
        }
        // S4 fallout: the move signal is NOT the whole change signal any more. This mirror feeds
        // the indirect G-buffer too, and a camera record carries baseColor / metalRough / texFlags
        // / mrMultiply / emissive -- so an editor material edit on a STANDING mesh was never
        // re-uploaded and never appeared. One integer compare per caster; the O(movers) property
        // that Step 7 exists for is about the FILL and the UPLOAD, and both still only happen on a
        // real change.
        const GBufferRenderable* gbMat = obj->AsGBufferRenderable();
        const std::uint32_t matVersion = gbMat ? gbMat->MaterialContentVersion() : 0u;
        const bool materialChanged = idx < cpuMaterialVersion_.size() &&
                                     cpuMaterialVersion_[idx] != matVersion;
        const bool contentChanged = (ro && ro->MovedThisFrame()) || windFadeChanged || materialChanged;
        // ONE MORE FILL IS OWED AFTER A MOVER STOPS, and leaving it out is a permanent artefact
        // rather than a transient one. FillInstanceSlot writes prevWorld straight from the object,
        // and on the frame the move happens prev is still the OLD transform -- correct, that is the
        // motion vector. But the record is never filled again once `MovedThisFrame()` goes false,
        // so prevWorld stays at that old transform forever while world sits at the new one. The
        // G-buffer then reports the same jump every single frame, DLSS believes the object is flying
        // and keeps smearing it: an object that was nudged once loses its surface detail for the
        // rest of the session. (Reported 2026-09-15: palm trunks go soft after moving them.)
        //
        // `pending_` already tracks "changed recently" for kFrameCount + 1 frames to walk the change
        // through every ring region (see where it is armed below for the + 1), so it is exactly the
        // window in which prev has to converge -- refill during it, but do NOT re-arm it, or the
        // object would refill itself forever.
        const bool settling = !contentChanged && idx < pending_.size() && pending_[idx] > 0;
        if (contentChanged || settling)
        {
            render::CasterBounds objectBounds{};
            FillBounds(obj, objectBounds);
            // Terrain today is static, so this branch never runs for a chunked mesh — but leaving
            // the object box here would silently un-chunk a mesh the moment one ever moved, and the
            // bug would look like a pure performance regression with a correct image.
            const Mesh* movedMesh = ro ? ro->GetMesh() : nullptr;
            const bool chunkedMover = movedMesh && movedMesh->IsChunkedSubmeshes();
            for (size_t s = 0; s < slots; ++s) // S4: each submesh slot's own record (see Rebuild)
            {
                FillInstanceSlot(obj, s, cpuInstances_[idx + s]);
                cpuBounds_[idx + s] = objectBounds;
                if (chunkedMover) { FillChunkBounds(ro, s, objectBounds, cpuBounds_[idx + s]); }
                // Stamped per slot so every one of an object's caster ids agrees, the way the
                // bounds above do -- a version left on slot 0 alone would re-fill this object
                // every frame forever.
                if (idx + s < cpuMaterialVersion_.size()) { cpuMaterialVersion_[idx + s] = matVersion; }
            }
            if (contentChanged)
            {
                // kFrameCount + 1, NOT kFrameCount: the window has to outlast the region written on
                // THIS frame. That record is the only one filled while prev was still the old
                // transform; with kFrameCount refills the next two frames rewrite the other two
                // regions and this one is never touched again, so every third frame the G-buffer
                // reported the whole move as motion. DLSS then rejected the object's history one
                // frame in three -- a clone dragged into place flickered over its trunk and fronds
                // for the rest of the session, while the CPU path (prevWorld read from the object
                // every frame) and --set=gbuffer.indirect:0 stayed clean. Measured 2026-09-23 on
                // wind_test, wind frozen: 5.2 % of view pixels changing frame to frame vs 0.75 %.
                for (size_t s = 0; s < slots; ++s)
                {
                    pending_[idx + s] = static_cast<std::uint8_t>(render::kFrameCount + 1);
                }
            }
        }
        // pending>0 propagates a recent change across all kFrameCount ring regions (even after
        // the object stops moving), so every region converges to the latest transform.
        for (size_t s = 0; s < slots; ++s)
        {
            if (pending_[idx + s] > 0)
            {
                instBase[idx + s] = cpuInstances_[idx + s];
                boundBase[idx + s] = cpuBounds_[idx + s];
                --pending_[idx + s];
                ++uploaded;
            }
        }
        idx += static_cast<std::uint32_t>(slots);
    }

    if (logFramesRemaining_ > 0)
    {
        // The five frames after a rebuild only (the counter above), never a standing per-frame line.
        --logFramesRemaining_;
        LOG_DEBUG(logging::LogCategory::RenderShadow, "frame update: {}/{} casters re-uploaded", uploaded, count_);
    }

    // Step 4 (docs/vsm_page_caching_plan.md): republish the caster meta for THIS frame with the
    // movers' dynamic bit set. `pending_ > 0` is already exactly "this caster changed recently" --
    // it is what propagates a transform across every ring region -- so it is the mover signal, and
    // it stays set for kFrameCount + 1 frames: as long as the stale pages live, plus the one frame
    // the prevWorld settle needs (one extra re-render of a stopped object's pages, nothing more).
    //
    // The page setup CS already dirties a page when a DYNAMIC caster overlaps it (`dynamicOverlap`
    // from the scatter pass). Until now a moved STATIC caster could not reach that path, so the only
    // way to invalidate its pages was `forceAll` -- the whole resident pool, for one object. This is
    // what made dragging a mesh in the editor cost ~195 ms a frame against a 0.38 ms baseline.
    if (!cpuCasterMeta_.empty() && count_ > 0)
    {
        const UINT frameSlot = renderer->GetCurrentFrameIndex();
        if (auto* metaDst = reinterpret_cast<std::uint32_t*>(casterMeta_.Region(frameSlot)))
        {
            const std::uint32_t n = std::min<std::uint32_t>(count_,
                static_cast<std::uint32_t>(cpuCasterMeta_.size()));
            for (std::uint32_t i = 0; i < n; ++i)
            {
                // bit 0 = dynamic, bits 1.. = slot count (see Rebuild). OR, never overwrite: an
                // object that reports IsDynamicCaster must stay dynamic when it is standing still.
                const std::uint32_t moved = (i < pending_.size() && pending_[i] > 0) ? 1u : 0u;
                metaDst[i] = cpuCasterMeta_[i] | moved;
            }
        }
    }

    if (forceContentRefresh_)
    {
        // Editor live-apply can keep identical transforms while replacing masked texture SRVs.
        // Treat content as changed once so VSM runs and refreshes every resident cached page.
        lastMoverCount_ = std::max<std::uint32_t>(count_, 1u);
        forceContentRefresh_ = false;
    }
    else
    {
        lastMoverCount_ = uploaded;
    }
    return uploaded;
}

void ShadowGpuData::SetActiveViewCount(std::uint32_t views)
{
    // S9e: never past the rows the last rebuild made (a capacity growth rebuilds them before the frame that uses it).
    activeViews_ = std::clamp<std::uint32_t>(views, render::kVsmShadowViews, std::max(builtViews_, render::kVsmShadowViews));
}

void ShadowGpuData::UpdateViewFrustums(Renderer* renderer, const Frustum* const* frustums, size_t count,
                                       const render::LocalViewCull* const* local)
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
        if (f && f->IsValid() && render::IsLocalShadowView(static_cast<unsigned>(i)))
        {
            // Plan S9e: a spot / point face -- its 6 planes, the light's sphere and cone, UE's hull (the shader reads them
            // by these slots). A perspective light view has exactly 6 planes; more would be dropped, which only culls less.
            const DirectX::XMFLOAT4 acceptAll(0.0f, 0.0f, 0.0f, 1.0f);
            const Math::float4* p = f->Planes();
            const int n = std::min<int>(f->PlaneCount(), static_cast<int>(render::kLocalViewFrustumPlanes));
            for (int j = 0; j < static_cast<int>(render::kLocalViewFrustumPlanes); ++j)
            {
                vf.planes[j] = j < n ? DirectX::XMFLOAT4(p[j].x, p[j].y, p[j].z, p[j].w) : acceptAll;
            }
            const render::LocalViewCull none{};
            const render::LocalViewCull& lc = (local && local[i]) ? *local[i] : none;
            vf.planes[render::kLocalViewSphereSlot] = lc.sphere;
            vf.planes[render::kLocalViewConeSlot] = lc.cone;
            for (std::uint32_t j = 0; j < render::kLocalViewHullPlanes; ++j)
            {
                vf.planes[render::kLocalViewHullFirst + j] = j < lc.hullCount ? lc.hull[j] : acceptAll;
            }
        }
        else if (f && f->IsValid())
        {
            // S14: PlaneCount() real planes (6 for a box/perspective view, up to 12 for a cascade's
            // convex cull volume), the rest padded with the accept-all plane (0,0,0,+1): signedDist
            // = +1, projRadius = 0 -> passes, so the shader can loop the full fixed array.
            const Math::float4* p = f->Planes();
            const int n = std::min<int>(f->PlaneCount(), static_cast<int>(render::kShadowViewPlanes));
            for (int j = 0; j < n; ++j)
            {
                vf.planes[j] = DirectX::XMFLOAT4(p[j].x, p[j].y, p[j].z, p[j].w);
            }
            for (int j = n; j < static_cast<int>(render::kShadowViewPlanes); ++j)
            {
                vf.planes[j] = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
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
    // Tessy N7e: per view, the eye (the light) and its texels per unit at distance 1 over tessy.lodShadowTexels -- the
    // lamps' atlases cut the cluster DAG by it (tessy_atlas_cull_cs ViewDag); w 0 = no cut (not a local view, VSM).
    if (EnsureRing(renderer, viewDag_, count, sizeof(DirectX::XMFLOAT4), L"ShadowGpuData.ViewDag"))
    {
        cpuViewDag_.assign(count, DirectX::XMFLOAT4{ 0.0f, 0.0f, 0.0f, 0.0f });
        const float texels = std::max(tessy::g_lodShadowTexels, 0.01f);
        for (size_t i = 0; i < count; ++i)
        {
            if (!local || !local[i] || !render::IsLocalShadowView(static_cast<unsigned>(i))) { continue; }
            DirectX::XMFLOAT4 d = local[i]->dag;
            d.w /= texels;
            cpuViewDag_[i] = d;
        }
        if (region < render::kFrameCount)
        {
            std::memcpy(viewDag_.Region(region), cpuViewDag_.data(), count * sizeof(DirectX::XMFLOAT4));
        }
    }
    // Tessy: per view, the matrix the lamp's atlas draw projects with and its viewport -- the cluster cull drops a cluster
    // whose box holds no texel centre of it (tessy_atlas_cull_cs ViewRects; size 0 = no test: not a lamp's view).
    static_assert(sizeof(TessyViewRectGpu) == 80, "tessy_atlas_cull_cs.hlsl TessyViewRect");
    if (EnsureRing(renderer, viewRect_, count, sizeof(TessyViewRectGpu), L"ShadowGpuData.ViewRect"))
    {
        cpuViewRect_.assign(count, TessyViewRectGpu{});
        for (size_t i = 0; i < count; ++i)
        {
            if (!local || !local[i] || !render::IsLocalShadowView(static_cast<unsigned>(i))) { continue; }
            cpuViewRect_[i].viewProj = local[i]->viewProj;
            cpuViewRect_[i].size = DirectX::XMFLOAT4(local[i]->rectTexels, local[i]->rectTexels, 0.0f, 0.0f);
        }
        if (region < render::kFrameCount)
        {
            std::memcpy(viewRect_.Region(region), cpuViewRect_.data(), count * sizeof(TessyViewRectGpu));
        }
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
    // S5b: the cascade HZB post cull. Optional -- without it the main cull never defers.
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/shadow_cull_post_cs.hlsl";
        cd.csEntry = "CSMain";
        cullPostMat_ = mm->GetOrCreateCompute(renderer, cd);
        if (!cullPostMat_ || !cullPostMat_->GetPipelineState())
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "shadow_cull_post_cs.hlsl did not build a PSO; cascade HZB cull off");
            cullPostMat_.reset();
        }
    }
    // S5: the camera's HZB post cull. Optional the same way -- without it the main cull never
    // defers a camera candidate and the indirect G-buffer stays single-pass.
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/cam_cull_post_cs.hlsl";
        cd.csEntry = "CSMain";
        camCullPostMat_ = mm->GetOrCreateCompute(renderer, cd);
        if (!camCullPostMat_ || !camCullPostMat_->GetPipelineState())
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "cam_cull_post_cs.hlsl did not build a PSO; camera HZB cull off");
            camCullPostMat_.reset();
        }
    }
    // A6: the merged G-buffer's expand kernel. Failure is non-fatal: one ExecuteIndirect per group stays.
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/gbuffer_indirect_expand_cs.hlsl";
        cd.csEntry = "CSMain";
        gbufferCmdExpandMat_ = mm->GetOrCreateCompute(renderer, cd);
        if (!gbufferCmdExpandMat_ || !gbufferCmdExpandMat_->GetPipelineState())
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "gbuffer_indirect_expand_cs.hlsl did not build a PSO; the G-buffer keeps one ExecuteIndirect per group");
            gbufferCmdExpandMat_.reset();
        }
    }
    // Local-light plan S6a: the atlas local views' args compaction. Optional -- without it a view keeps one
    // ExecuteIndirect per mesh group.
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/shadow_args_compact_cs.hlsl";
        cd.csEntry = "CSMain";
        localCompactMat_ = mm->GetOrCreateCompute(renderer, cd);
        if (!localCompactMat_ || !localCompactMat_->GetPipelineState())
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "shadow_args_compact_cs.hlsl did not build a PSO; local-light views keep one ExecuteIndirect per group");
            localCompactMat_.reset();
        }
    }
    // Step 4 (GI→VSM): the GI-scatter compute that folds each GPU-instanced object's per-instance
    // transforms into the unified caster buffers. Optional — a failure just leaves GI on the CPU tail.
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/shadow_gi_scatter_cs.hlsl";
        cd.csEntry = "CSMain";
        giScatterMat_ = mm->GetOrCreateCompute(renderer, cd);
    }
    // Step 5: the indirect depth-only shadow PSO. Depth-only (numRT 0, D16, LESS_EQUAL) mirroring
    // ConfigureShadowPipeline; the PosOnly_InstCasterId layout binds the mesh vertex stream (slot
    // 0) + the visible-list caster-id stream (slot 1, per-instance). Compiled now; drawn in Step 6.
    {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/shadow_indirect_csm.hlsl";
        gd.vsEntry = "VSMain";
        gd.psEntry = "PSMain";
        // S6: the two ATLAS PSOs (Legacy CSM cascades, spot, point) carry the vertex normal and
        // the SHADOW_DEPTH_BIAS permutation. The four VSM PSOs below deliberately do not -- each
        // starts from a gd.defines.clear() and the position-only layout, so the VSM page render
        // fetches position alone and this step costs it nothing at all.
        gd.defines.emplace_back("SHADOW_DEPTH_BIAS", "1");
        gd.inputLayoutKey = "PosNrm_InstCasterId";
        gd.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gd.numRT = 0;
        gd.dsvFormat = DXGI_FORMAT_D16_UNORM;
        gd.depth.DepthEnable = TRUE;
        gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        gd.depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        gd.raster.CullMode = D3D12_CULL_MODE_BACK;
        gd.blend.RenderTarget[0].BlendEnable = FALSE;
        indirectShadowMat_ = mm->GetOrCreateGraphics(renderer, gd);
        // Plan S4: both atlas PSOs get the local views' linear-depth twin (the spot / point passes pick it per view).
        if (indirectShadowMat_ && indirectShadowMat_->GetPipelineState())
        {
            mm->LinkLinearDepthVariant(renderer, *indirectShadowMat_);
        }

        // C2: the SHADOW_MASKED variant — selected by IndirectShadowMaterial() whenever the
        // caster set contains alpha-masked groups (opaque groups early-out in its PS, so ONE
        // PSO serves the whole set and the single-ExecuteIndirect structure stays). CULL_NONE:
        // masked foliage is authored double-sided, and depth-only backface culling would drop
        // casters. Failure is non-fatal — masked groups just cast solid shadows.
        gd.inputLayoutKey = "PosNrmUV_InstCasterId";
        gd.defines.emplace_back("SHADOW_MASKED", "1");
        gd.raster.CullMode = D3D12_CULL_MODE_NONE;
        indirectShadowMaskedMat_ = mm->GetOrCreateGraphics(renderer, gd);
        if (!indirectShadowMaskedMat_ || !indirectShadowMaskedMat_->GetPipelineState())
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "masked indirect shadow PSO FAILED (masked casters cast solid shadows)");
            indirectShadowMaskedMat_.reset();
        }
        else
        {
            mm->LinkLinearDepthVariant(renderer, *indirectShadowMaskedMat_); // plan S4
        }

        // Pool-format twins of the two PSOs above, for the VSM per-page LOOP fallback: same shader,
        // same state, but the POOL's D32_FLOAT depth format (the Legacy atlases stay D16, and one
        // PSO cannot serve two DSV formats). Optional like the masked variant — a failure only
        // costs the loop fallback, which the single-draw path already covers.
        gd.dsvFormat = DXGI_FORMAT_D32_FLOAT;
        gd.defines.clear();
        gd.inputLayoutKey = "PosOnly_InstCasterId";
        gd.raster.CullMode = D3D12_CULL_MODE_BACK;
        indirectShadowPoolMat_ = mm->GetOrCreateGraphics(renderer, gd);
        if (!indirectShadowPoolMat_ || !indirectShadowPoolMat_->GetPipelineState())
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "pool-format indirect shadow PSO FAILED (per-page loop fallback off)");
            indirectShadowPoolMat_.reset();
        }
        gd.defines.emplace_back("SHADOW_MASKED", "1");
        gd.inputLayoutKey = "PosUV_InstCasterId";
        gd.raster.CullMode = D3D12_CULL_MODE_NONE;
        indirectShadowPoolMaskedMat_ = mm->GetOrCreateGraphics(renderer, gd);
        if (!indirectShadowPoolMaskedMat_ || !indirectShadowPoolMaskedMat_->GetPipelineState())
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "pool-format masked indirect shadow PSO FAILED (per-page loop fallback off)");
            indirectShadowPoolMaskedMat_.reset();
        }

        // Single-draw page render: the VSM_PAGE permutations, used only by the VSM per-page pass.
        // Identical pipeline state to the pool pair above (D32 — they also draw into the pool) — the
        // whole difference lives in the shader (page index unpacked from the caster id, projection +
        // wind read from an SRV instead of the b1 root CBV, page borders emitted as SV_ClipDistance
        // instead of a per-page scissor), so the input layouts are unchanged: the same uint arrives
        // in CASTERID, only reinterpreted. Optional — on failure IndirectShadowPageMaterial()
        // returns null and RecordPageRender stays on the per-page loop, so shadows remain correct
        // either way.
        gd.defines.clear();
        gd.defines.emplace_back("VSM_PAGE", "1");
        gd.inputLayoutKey = "PosOnly_InstCasterId";
        gd.raster.CullMode = D3D12_CULL_MODE_BACK;
        indirectShadowPageMat_ = mm->GetOrCreateGraphics(renderer, gd);
        if (!indirectShadowPageMat_ || !indirectShadowPageMat_->GetPipelineState())
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "VSM_PAGE indirect shadow PSO FAILED (single-draw page render off)");
            indirectShadowPageMat_.reset();
        }

        gd.defines.emplace_back("SHADOW_MASKED", "1");
        gd.inputLayoutKey = "PosUV_InstCasterId";
        gd.raster.CullMode = D3D12_CULL_MODE_NONE; // double-sided foliage, as above
        indirectShadowPageMaskedMat_ = mm->GetOrCreateGraphics(renderer, gd);
        if (!indirectShadowPageMaskedMat_ || !indirectShadowPageMaskedMat_->GetPipelineState())
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "VSM_PAGE masked indirect shadow PSO FAILED (single-draw page render off)");
            indirectShadowPageMaskedMat_.reset();
        }

        // Tessy N3: the cluster twins -- the page PSOs' state, no input layout (the VS reads the mega buffers by
        // SV_VertexID / SV_InstanceID). Optional: without them every caster stays on the classic page draw.
        gd.defines.clear();
        gd.defines.emplace_back("VSM_TESSY", "1");
        gd.inputLayoutKey = "";
        gd.raster.CullMode = D3D12_CULL_MODE_BACK;
        indirectShadowTessyMat_ = mm->GetOrCreateGraphics(renderer, gd);
        if (!indirectShadowTessyMat_ || !indirectShadowTessyMat_->GetPipelineState())
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "VSM_TESSY indirect shadow PSO FAILED (tessy.vsm off)");
            indirectShadowTessyMat_.reset();
        }
        gd.defines.emplace_back("SHADOW_MASKED", "1");
        gd.raster.CullMode = D3D12_CULL_MODE_NONE; // double-sided foliage, as above
        indirectShadowTessyMaskedMat_ = mm->GetOrCreateGraphics(renderer, gd);
        if (!indirectShadowTessyMaskedMat_ || !indirectShadowTessyMaskedMat_->GetPipelineState())
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "VSM_TESSY masked indirect shadow PSO FAILED (tessy.vsm off)");
            indirectShadowTessyMaskedMat_.reset();
        }

        // Tessy N6: the mesh shader twins of the two above (MSMain, the same state, the same table). Only with a
        // mesh shader tier; without them `tessy.raster` 1 draws through the VS path.
        if (renderer->SupportsMeshShaders())
        {
            gd.defines.clear();
            gd.defines.emplace_back("VSM_TESSY", "1");
            gd.defines.emplace_back("VSM_TESSY_MS", "1");
            gd.msEntry = "MSMain";
            gd.raster.CullMode = D3D12_CULL_MODE_BACK;
            indirectShadowTessyMeshMat_ = mm->GetOrCreateGraphics(renderer, gd);
            if (!indirectShadowTessyMeshMat_ || !indirectShadowTessyMeshMat_->GetPipelineState())
            {
                LOG_ERROR(logging::LogCategory::RenderShadow, "VSM_TESSY_MS mesh shader PSO FAILED (tessy.raster draws through the VS)");
                indirectShadowTessyMeshMat_.reset();
            }
            gd.defines.emplace_back("SHADOW_MASKED", "1");
            gd.raster.CullMode = D3D12_CULL_MODE_NONE;
            indirectShadowTessyMeshMaskedMat_ = mm->GetOrCreateGraphics(renderer, gd);
            if (!indirectShadowTessyMeshMaskedMat_ || !indirectShadowTessyMeshMaskedMat_->GetPipelineState())
            {
                LOG_ERROR(logging::LogCategory::RenderShadow, "VSM_TESSY_MS masked mesh shader PSO FAILED (tessy.raster draws through the VS)");
                indirectShadowTessyMeshMaskedMat_.reset();
            }
            gd.msEntry = nullptr;
        }

        // Tessy N4: the atlas twins -- the ATLAS PSOs' state (D16, LESS_EQUAL, the S6 bias, the S4 linear-depth twins),
        // drawn by a mesh shader. Only with a mesh shader tier: without them the atlas views stay classic.
        {
            Material::ComputeDesc cd{};
            cd.shaderFile = L"shaders/tessy_atlas_cull_cs.hlsl";
            cd.csEntry = "CSMain";
            tessyAtlasCullMat_ = mm->GetOrCreateCompute(renderer, cd);
            if (!tessyAtlasCullMat_ || !tessyAtlasCullMat_->GetPipelineState())
            {
                LOG_ERROR(logging::LogCategory::RenderShadow, "tessy_atlas_cull_cs.hlsl did not build a PSO (tessy.atlas off)");
                tessyAtlasCullMat_.reset();
            }
            // The camera row's twin: a handful of groups (the G-buffer's PSO runs), thousands of tests each.
            cd.defines.emplace_back("TESSY_CULL_THREADS", "1024");
            tessyCameraCullMat_ = mm->GetOrCreateCompute(renderer, cd);
            if (!tessyCameraCullMat_ || !tessyCameraCullMat_->GetPipelineState())
            {
                LOG_WARNING(logging::LogCategory::RenderShadow, "tessy_atlas_cull_cs.hlsl (1024 threads) did not build a PSO: the camera row takes the 256-thread kernel");
                tessyCameraCullMat_.reset();
            }
            // N5: the camera clusters' post pass (the two-pass HZB occlusion's retest).
            Material::ComputeDesc pd{};
            pd.shaderFile = L"shaders/tessy_camera_post_cs.hlsl";
            pd.csEntry = "CSMain";
            tessyCameraPostMat_ = mm->GetOrCreateCompute(renderer, pd);
            if (!tessyCameraPostMat_ || !tessyCameraPostMat_->GetPipelineState())
            {
                LOG_ERROR(logging::LogCategory::RenderShadow, "tessy_camera_post_cs.hlsl did not build a PSO (the camera clusters skip the HZB test)");
                tessyCameraPostMat_.reset();
            }
        }
        if (renderer->SupportsMeshShaders())
        {
            gd.defines.clear();
            gd.defines.emplace_back("TESSY_ATLAS_MS", "1");
            gd.defines.emplace_back("SHADOW_DEPTH_BIAS", "1");
            gd.msEntry = "MSMain";
            gd.inputLayoutKey = "";
            gd.dsvFormat = DXGI_FORMAT_D16_UNORM;
            gd.depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            gd.raster.CullMode = D3D12_CULL_MODE_BACK;
            tessyAtlasMeshMat_ = mm->GetOrCreateGraphics(renderer, gd);
            if (tessyAtlasMeshMat_ && tessyAtlasMeshMat_->GetPipelineState()) { mm->LinkLinearDepthVariant(renderer, *tessyAtlasMeshMat_); }
            else
            {
                LOG_ERROR(logging::LogCategory::RenderShadow, "TESSY_ATLAS_MS mesh shader PSO FAILED (tessy.atlas off)");
                tessyAtlasMeshMat_.reset();
            }
            gd.defines.emplace_back("SHADOW_MASKED", "1");
            gd.raster.CullMode = D3D12_CULL_MODE_NONE;
            tessyAtlasMeshMaskedMat_ = mm->GetOrCreateGraphics(renderer, gd);
            if (tessyAtlasMeshMaskedMat_ && tessyAtlasMeshMaskedMat_->GetPipelineState()) { mm->LinkLinearDepthVariant(renderer, *tessyAtlasMeshMaskedMat_); }
            else
            {
                LOG_ERROR(logging::LogCategory::RenderShadow, "TESSY_ATLAS_MS masked mesh shader PSO FAILED (tessy.atlas off)");
                tessyAtlasMeshMaskedMat_.reset();
            }
            // The cluster view (tessy::g_debugView): the same mesh shader in colour over the lit frame -- the scene colour
            // and the deferred depth (reversed-Z: GREATER_EQUAL, read only), nudged toward the camera by the slope so the
            // very triangles the G-buffer drew pass the test; no shadow bias, no linear depth.
            gd.defines.clear();
            gd.defines.emplace_back("TESSY_ATLAS_MS", "1");
            gd.defines.emplace_back("TESSY_VIEW", "1");
            gd.numRT = 1;
            gd.rtvFormats[0] = renderer->GetSceneColorFormat();
            gd.dsvFormat = render::kDeferredDepthFormat;
            gd.depth.DepthEnable = TRUE;
            gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            gd.depth.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            gd.raster.CullMode = D3D12_CULL_MODE_BACK;
            gd.raster.DepthBias = 16;
            gd.raster.SlopeScaledDepthBias = 1.0f;
            tessyViewMat_ = mm->GetOrCreateGraphics(renderer, gd);
            if (!tessyViewMat_ || !tessyViewMat_->GetPipelineState())
            {
                LOG_ERROR(logging::LogCategory::RenderShadow, "TESSY_VIEW mesh shader PSO FAILED (no cluster view)");
                tessyViewMat_.reset();
            }
            gd.defines.emplace_back("SHADOW_MASKED", "1");
            gd.raster.CullMode = D3D12_CULL_MODE_NONE;
            tessyViewMaskedMat_ = mm->GetOrCreateGraphics(renderer, gd);
            if (!tessyViewMaskedMat_ || !tessyViewMaskedMat_->GetPipelineState())
            {
                LOG_ERROR(logging::LogCategory::RenderShadow, "TESSY_VIEW masked mesh shader PSO FAILED (no cluster view)");
                tessyViewMaskedMat_.reset();
            }
            gd.msEntry = nullptr;
        }
    }

    const bool cullOk = cullClearMat_ && cullClearMat_->GetPipelineState() &&
                        cullMat_ && cullMat_->GetPipelineState();
    const bool drawOk = indirectShadowMat_ && indirectShadowMat_->GetPipelineState();
    if (!cullOk)
    {
        LOG_ERROR(logging::LogCategory::RenderShadow, "cull compute PSO creation FAILED (shader compile?)");
        cullClearMat_.reset();
        cullMat_.reset();
    }
    if (!drawOk)
    {
        LOG_ERROR(logging::LogCategory::RenderShadow, "indirect shadow PSO creation FAILED (shader compile?)");
        indirectShadowMat_.reset();
    }
    if (!giScatterMat_ || !giScatterMat_->GetPipelineState())
    {
        LOG_ERROR(logging::LogCategory::RenderShadow, "GI-scatter PSO creation FAILED (GI stays on the CPU tail)");
        giScatterMat_.reset();
    }
    if (cullOk && drawOk)
    {
        LOG_INFO(logging::LogCategory::RenderShadow, "shaders ready: cull (clear+cull) + indirect-shadow PSOs created");
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
    hd.NumDescriptors = static_cast<UINT>(kCullUavSets * render::kFrameCount);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(gpu::CreateDescriptorHeap(dev, &hd, MemTag::ShadowGpuData, L"ShadowGpuData.CullUavHeap", IID_PPV_ARGS(cullUavHeap_.GetAddressOf()))) || !cullUavHeap_)
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
        // S5b: the cascade HZB cull's four. A ring that failed to allocate leaves its slot at the
        // args UAV, so a table is never bound with a hole (dec.hzb is false then and the shader
        // does not touch u2/u3 beyond the counters, which alias the args harmlessly).
        const auto structuredUint = [&](const UavRing& ring, std::size_t set)
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE h = slotHandle(static_cast<UINT>(set * render::kFrameCount + f));
            if (ring.Valid())
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = DXGI_FORMAT_UNKNOWN;
                ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                ud.Buffer.FirstElement = static_cast<UINT64>(f) * (ring.regionBytes / 4);
                ud.Buffer.NumElements = static_cast<UINT>(ring.regionBytes / 4);
                ud.Buffer.StructureByteStride = sizeof(std::uint32_t);
                dev->CreateUnorderedAccessView(ring.buffer.Get(), nullptr, &ud, h);
                cullUav_[set * render::kFrameCount + f] = h;
            }
            else
            {
                cullUav_[set * render::kFrameCount + f] = cullUav_[f];
            }
        };
        structuredUint(deferredList_, 3);
        structuredUint(deferredCount_, 4);
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE h = slotHandle(5 * render::kFrameCount + f);
            if (indirectArgsB_.Valid())
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = DXGI_FORMAT_R32_TYPELESS;
                ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                ud.Buffer.FirstElement = static_cast<UINT64>(f) * (indirectArgsB_.regionBytes / 4);
                ud.Buffer.NumElements = static_cast<UINT>(indirectArgsB_.regionBytes / 4);
                ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                dev->CreateUnorderedAccessView(indirectArgsB_.buffer.Get(), nullptr, &ud, h);
                cullUav_[5 * render::kFrameCount + f] = h;
            }
            else
            {
                cullUav_[5 * render::kFrameCount + f] = cullUav_[f];
            }
        }
        structuredUint(visibleListB_, 6);
        // S4: the camera's args (RAW) + visible list.
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE h = slotHandle(7 * render::kFrameCount + f);
            if (camArgs_.Valid())
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = DXGI_FORMAT_R32_TYPELESS;
                ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                ud.Buffer.FirstElement = static_cast<UINT64>(f) * (camArgs_.regionBytes / 4);
                ud.Buffer.NumElements = static_cast<UINT>(camArgs_.regionBytes / 4);
                ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                dev->CreateUnorderedAccessView(camArgs_.buffer.Get(), nullptr, &ud, h);
                cullUav_[7 * render::kFrameCount + f] = h;
            }
            else
            {
                cullUav_[7 * render::kFrameCount + f] = cullUav_[f];
            }
        }
        structuredUint(camVisibleList_, 8);
        // S5: the camera's deferred list, pass B's args (RAW) + visible list.
        structuredUint(camDeferred_, 9);
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE h = slotHandle(10 * render::kFrameCount + f);
            if (camArgsB_.Valid())
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = DXGI_FORMAT_R32_TYPELESS;
                ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                ud.Buffer.FirstElement = static_cast<UINT64>(f) * (camArgsB_.regionBytes / 4);
                ud.Buffer.NumElements = static_cast<UINT>(camArgsB_.regionBytes / 4);
                ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                dev->CreateUnorderedAccessView(camArgsB_.buffer.Get(), nullptr, &ud, h);
                cullUav_[10 * render::kFrameCount + f] = h;
            }
            else
            {
                cullUav_[10 * render::kFrameCount + f] = cullUav_[f];
            }
        }
        structuredUint(camVisibleListB_, 11);
        // A6: the merged G-buffer's command buffers (RAW; the expand kernel writes them).
        const auto rawUav = [&](const UavRing& ring, std::size_t set)
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE h = slotHandle(static_cast<UINT>(set * render::kFrameCount + f));
            if (ring.Valid())
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = DXGI_FORMAT_R32_TYPELESS;
                ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                ud.Buffer.FirstElement = static_cast<UINT64>(f) * (ring.regionBytes / 4);
                ud.Buffer.NumElements = static_cast<UINT>(ring.regionBytes / 4);
                ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                dev->CreateUnorderedAccessView(ring.buffer.Get(), nullptr, &ud, h);
                cullUav_[set * render::kFrameCount + f] = h;
            }
            else
            {
                cullUav_[set * render::kFrameCount + f] = cullUav_[f];
            }
        };
        rawUav(camCmds_, 12);
        rawUav(camCmdsB_, 13);
        rawUav(localArgs_, 14);   // S6a
        rawUav(localCounts_, 15);
        rawUav(staticArgs_, 16);  // S7
        structuredUint(staticVisibleList_, 17);
        rawUav(localStaticArgs_, 18);
        rawUav(localStaticCounts_, 19);
    }
}

void ShadowGpuData::RebuildUnifiedDescriptors(Renderer* renderer)
{
    unifiedDescr_.fill({});
    unifiedSrvHeap_.Reset();
    if (!renderer || !renderer->GetDevice()) { return; }
    if (!instancesUnified_.Valid() || !boundsUnified_.Valid() || count_ == 0) { return; }

    ID3D12Device* dev = renderer->GetDevice();
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 4 * render::kFrameCount; // [0..k)=inst SRV, [k..2k)=bounds SRV, [2k..3k)=inst UAV, [3k..4k)=bounds UAV
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(gpu::CreateDescriptorHeap(dev, &hd, MemTag::ShadowGpuData, L"ShadowGpuData.UnifiedSrvHeap", IID_PPV_ARGS(unifiedSrvHeap_.GetAddressOf()))) || !unifiedSrvHeap_)
    {
        unifiedSrvHeap_.Reset();
        return;
    }
    const UINT incr = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE base = unifiedSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    auto slotHandle = [&](UINT slot) { D3D12_CPU_DESCRIPTOR_HANDLE h{ base.ptr + static_cast<SIZE_T>(slot) * incr }; return h; };

    // Region f is spaced by the buffer's physical regionBytes (>= count_*stride when the allocation
    // was reused from a larger prior level); NumElements is the live count_ (covers static + GI).
    const UINT instStride = static_cast<UINT>(sizeof(render::InstancePerObject));
    const UINT boundStride = static_cast<UINT>(sizeof(render::CasterBounds));
    const UINT64 instElemsPerRegion = instancesUnified_.regionBytes / instStride;
    const UINT64 boundElemsPerRegion = boundsUnified_.regionBytes / boundStride;

    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        // instance SRV (region f)
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
            unifiedDescr_[f] = h;
        }
        // bounds SRV (region f)
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
            unifiedDescr_[render::kFrameCount + f] = h;
        }
        // instance UAV (region f) — Step 4 scatter write target
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Buffer.FirstElement = static_cast<UINT64>(f) * instElemsPerRegion;
            ud.Buffer.NumElements = count_;
            ud.Buffer.StructureByteStride = instStride;
            ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
            const D3D12_CPU_DESCRIPTOR_HANDLE h = slotHandle(2 * render::kFrameCount + f);
            dev->CreateUnorderedAccessView(instancesUnified_.buffer.Get(), nullptr, &ud, h);
            unifiedDescr_[2 * render::kFrameCount + f] = h;
        }
        // bounds UAV (region f)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Buffer.FirstElement = static_cast<UINT64>(f) * boundElemsPerRegion;
            ud.Buffer.NumElements = count_;
            ud.Buffer.StructureByteStride = boundStride;
            ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
            const D3D12_CPU_DESCRIPTOR_HANDLE h = slotHandle(3 * render::kFrameCount + f);
            dev->CreateUnorderedAccessView(boundsUnified_.buffer.Get(), nullptr, &ud, h);
            unifiedDescr_[3 * render::kFrameCount + f] = h;
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
    gpu::CreateCommittedResource(renderer->GetDevice(),
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        MemTag::ShadowGpuData, L"ShadowGpuData.ValidationReadback",
        IID_PPV_ARGS(valReadback_.GetAddressOf()));
}

// pass-flow S7a: `WillUseUnifiedBuffers` and `WillRecordValidationReadback` are GONE. They existed
// only so a Prepare and a Record could not disagree about what this frame does; PrepareCullPass now
// decides both inline and hands the answers to RecordCull in CullDecisions, so there is nothing
// left for a second evaluation to get wrong.

void ShadowGpuData::RecordCull(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                               const CullDecisions& dec)
{
    // pass-flow S7a: no gates. `dec` came from PrepareCullPass, which made every one of these
    // decisions once and declared from them. EnsureShaderResources stays a no-op safety net — it
    // is one-shot and SceneRenderer::EnsureFrameResources already called it before the graph was
    // built, which is what makes the builder's view of the materials final.
    if (!renderer || !cl || !dec.active) { return; }
    EnsureShaderResources(renderer);

    const UINT f = renderer->GetCurrentFrameIndex();

    // Plan S9: only the views in use (VSM's prefix + the occupied extra local slots); the rows past it stay stale and
    // nothing draws from them. 142 rows cleared and tested every frame cost wind_test's cull +61 us for 3 lamps.
    const std::uint32_t numViews = activeViews_;
    const std::uint32_t numGroups = numVirtualGroups_; // S3.6: args are per (view, VIRTUAL group)
    // Step 4 (GI→VSM): when the GI folding path is active this frame, the cull covers ALL count_
    // casters (the scatter below fills the GI region first); otherwise only the static Nstatic (GI
    // dormant, drawn by the retained CPU tail). numCasters is also the per-view visible-list stride
    // the clear/cull/draw all agree on — Nstatic keeps the static-only layout byte-identical.
    const bool giOn = dec.giOn;
    const std::uint32_t numCasters = giOn ? count_ : staticCount_;

    // The cull outputs must be UNORDERED_ACCESS before the dispatches (created COMMON, or left
    // COPY_SOURCE by a prior validation frame).
    renderer->EmitPoint(cl, dec.base);

    // Step 2/3 (GI→VSM): before the cull, mirror this frame's per-STATIC-caster ring region into the
    // DEFAULT-heap unified buffers so the cull (bounds) + indirect VS (t0 instances) read a GPU-local
    // copy a compute shader can also write. Only the static region [0,Nstatic) is copied; the GI
    // region [Nstatic,count_) stays untouched here (Step 4's scatter fills it). The static copy is
    // byte-identical to reading the upload ring, so the static shadows are unchanged. The upload
    // rings stay in GENERIC_READ (which includes COPY_SOURCE) so no source barrier is needed. Left in
    // NON_PIXEL_SHADER_RESOURCE for the cull's compute SRV read and the parallel shadow passes' VS t0
    // read (both non-pixel shader stages).
    if (dec.useUnified)
    {
        renderer->EmitPoint(cl, dec.unifiedCopy);
        if (staticCount_ > 0)
        {
            const UINT64 instSrc = static_cast<UINT64>(f) * instances_.capacity * instances_.stride;
            const UINT64 boundSrc = static_cast<UINT64>(f) * bounds_.capacity * bounds_.stride;
            cl->CopyBufferRegion(instancesUnified_.buffer.Get(), static_cast<UINT64>(f) * instancesUnified_.regionBytes,
                                 instances_.buffer.Get(), instSrc,
                                 static_cast<UINT64>(staticCount_) * sizeof(render::InstancePerObject));
            cl->CopyBufferRegion(boundsUnified_.buffer.Get(), static_cast<UINT64>(f) * boundsUnified_.regionBytes,
                                 bounds_.buffer.Get(), boundSrc,
                                 static_cast<UINT64>(staticCount_) * sizeof(render::CasterBounds));
        }

        // Step 4: scatter each folded GI object's per-instance transforms into the unified GI region
        // [Nstatic,count_). Runs after the static copy (disjoint region) — transition the whole
        // resource COPY_DEST -> UAV, dispatch one scatter per GI object (source = the object's own
        // InstanceData buffer, read as NON_PIXEL; dst = the unified UAVs), UAV-barrier, then fall
        // through to NON_PIXEL for the cull/VS reads. Ordering vs the GI rotation compute is safe:
        // Main_ObjectCompute precedes Main_ShadowCull in the render graph.
        if (giOn)
        {
            renderer->EmitPoint(cl, dec.giWrite);
            const D3D12_CPU_DESCRIPTOR_HANDLE instUav = UnifiedInstanceUav(f);
            const D3D12_CPU_DESCRIPTOR_HANDLE boundUav = UnifiedBoundsUav(f);

            struct ScatterCB
            {
                std::uint32_t       giBase, count;
                float               windStrength, windFoliage; // W5 (mirrors gWindStrength/gWindFoliage)
                float               windTrunkStiff, windLeafScale, _pad0, _pad1;
                DirectX::XMFLOAT4   aabbCenter;
                DirectX::XMFLOAT4   aabbExtent;
                DirectX::XMFLOAT4X4 objectWorld;
            };
            const UINT scatterCbSize = static_cast<UINT>(sizeof(ScatterCB));
            // pass-flow S7a: the FILTERED list PrepareCullPass declared from — not a second walk
            // with a second copy of the skip conditions.
            bool giReadEmitted = false;
            for (std::uint16_t idx : dec.giCasterIdx)
            {
                const GiCaster& gc = giCasters_[idx];
                ID3D12Resource* giBuf = gc.obj->GetInstanceCasterResource();
                const D3D12_CPU_DESCRIPTOR_HANDLE giSrv = gc.obj->GetInstanceCasterSrv();
                // Fold in the object's model matrix so the stored world matches gbuffer_inst_csm's
                // mul(instanceWorld, objectWorld); read the object's transform fresh each frame.
                const RenderableObject* ro = gc.obj->AsRenderableObject();
                DirectX::XMFLOAT4X4 objWorld = ro ? ro->GetModelMatrix().m : DirectX::XMFLOAT4X4{};
                if (!ro) { DirectX::XMStoreFloat4x4(&objWorld, DirectX::XMMatrixIdentity()); }
                // W5: the same per-object windStrength gbuffer_inst.hlsl feeds its BaseVS, so a
                // flagged GPU-instanced object sways identically in the gbuffer and in shadow.
                const GBufferRenderable* giGb = gc.obj->AsGBufferRenderable();
                const float giWind = giGb
                    ? giGb->EffectiveWindStrength(giGb->CurrentDrawParams().windStrength) : 0.0f;
                const float giFoliage = giGb ? giGb->FoliageForSlot(0) : 0.0f;
                const float giStiff = giGb ? giGb->GetWindTrunkStiffness() : 1.0f;
                const float giLeafScale = giGb ? giGb->GetWindLeafScaleWorld() : 0.0f;

                // One marker for the whole list: the point carries every folded object's buffer,
                // and all of them are read (never written) by the dispatches below.
                if (!giReadEmitted) { renderer->EmitPoint(cl, dec.giRead); giReadEmitted = true; }
                RecordComputeDispatch(renderer, cl, giScatterMat_.get(), scatterCbSize,
                    [&](std::uint8_t* dst)
                    {
                        ScatterCB c{};
                        c.giBase = gc.giBase;
                        c.count = gc.count;
                        c.windStrength = giWind;
                        c.windFoliage = giFoliage;
                        c.windTrunkStiff = giStiff;
                        c.windLeafScale = giLeafScale;
                        c.aabbCenter = gc.aabbCenter;
                        c.aabbExtent = gc.aabbExtent;
                        c.objectWorld = objWorld;
                        std::memcpy(dst, &c, sizeof(c));
                    },
                    { giSrv },
                    { instUav, boundUav },
                    D3D12_GPU_DESCRIPTOR_HANDLE{},
                    gc.count, 1,
                    nullptr); // GI objects write disjoint id ranges -> no inter-dispatch barrier
            }
            renderer->UAVBarrier(cl, instancesUnified_.buffer.Get());
            renderer->UAVBarrier(cl, boundsUnified_.buffer.Get());

            // The hand-back the builder declared, for the same objects. The scatter is done with
            // these buffers here; leaving them in the state IT wanted made the resting state
            // depend on whether the owner's (conditional) draw ran afterwards.
            if (giReadEmitted) { renderer->EmitPoint(cl, dec.giRestore); }
        }

        renderer->EmitPoint(cl, dec.unifiedRead);
    }

    const std::uint32_t camView = dec.gbuffer ? render::MaxShadowViews() : 0xFFFFFFFFu; // S9e: past the capacity's views
    // The clears read only the first three words; `rows` is how many view rows they seed.
    const auto clearCB = [&](std::uint32_t rows, std::uint32_t cam)
    {
        ShadowCullCB c{};
        c.numCasters = numCasters;
        c.numViews = rows;
        c.numGroups = numGroups;
        c.camView = cam;
        return c;
    };
    ShadowCullCB cullCB = clearCB(numViews, camView);
    cullCB.localSplit = dec.localSplit ? 1u : 0u;
    cullCB.giFirst = staticCount_; // S9e: casters past it are GI (UploadCasterLods' isGi)
    cullCB.camPos[0] = dec.localCamPos.x;
    cullCB.camPos[1] = dec.localCamPos.y;
    cullCB.camPos[2] = dec.localCamPos.z;
    cullCB.radiusThresholdSq = dec.localRadiusSq;
    cullCB.windRigid = dec.localWindRigid;
    auto writeCB = [&](std::uint8_t* dst) { std::memcpy(dst, &cullCB, sizeof(cullCB)); };
    const UINT cbSize = static_cast<UINT>(sizeof(ShadowCullCB));
    const D3D12_GPU_DESCRIPTOR_HANDLE noSampler{};
    cullCastersThisFrame_ = numCasters;

    // S5b: the cull's UAV table gained the deferred list + counters (u2, u3) and its SRV table
    // the four cascade pyramids (t5..t8). On a frame without the test the shader never reads
    // them, but a VOLATILE table may not hold a hole: the atlas SRV stands in for a pyramid.
    const auto deferredListUav = [&]() { return cullUav_[3 * render::kFrameCount + f]; };
    const auto deferredCountUav = [&]() { return cullUav_[4 * render::kFrameCount + f]; };
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, render::CascadeHzb::kCascades> hzbSrv{};
    for (unsigned c = 0; c < render::CascadeHzb::kCascades; ++c)
    {
        hzbSrv[c] = (dec.hzb && cascadeHzb_.Srv(c).ptr != 0) ? cascadeHzb_.Srv(c) : renderer->GetDeferredForFrame().shadowSRV;
    }

    // Clear/init the per-(view, VIRTUAL group) indirect args + per-view draw counts. Reads perGroupVg_:
    // ranges are per (mesh group, receiver LOD) and therefore VIEW-INDEPENDENT (S3.6).
    RecordComputeDispatch(renderer, cl, cullClearMat_.get(), cbSize, writeCB,
        { perGroupVg_.Srv(f) },
        { cullUav_[f], cullUav_[2 * render::kFrameCount + f], deferredCountUav() },
        noSampler,
        numViews * numGroups, 1,
        indirectArgs_.buffer.Get()); // UAV barrier: args init visible to the cull's InterlockedAdd

    // S7: the static rows, seeded the same way (row = slot - kShadowViewCascades, StartInstance into their own list).
    // The counts and counters the clear also writes are the same buffers as above, rewritten with harmless values.
    if (dec.localSplit)
    {
        const std::uint32_t localRows = dec.localViews;
        auto writeCBS = [&](std::uint8_t* dst) { const ShadowCullCB c = clearCB(localRows, 0u); std::memcpy(dst, &c, sizeof(c)); };
        RecordComputeDispatch(renderer, cl, cullClearMat_.get(), cbSize, writeCBS,
            { perGroupVg_.Srv(f) },
            { cullUav_[16 * render::kFrameCount + f], cullUav_[2 * render::kFrameCount + f], deferredCountUav() },
            noSampler,
            localRows * numGroups, 1,
            staticArgs_.buffer.Get());
    }

    // S5b: pass B's args, seeded the same way for the cascade rows only (the counts and the
    // counters it also writes are the SAME buffers as above, rewritten with the same values).
    if (dec.hzb)
    {
        const std::uint32_t hzbViews = render::CascadeHzb::kCascades;
        auto writeCBB = [&](std::uint8_t* dst) { const ShadowCullCB c = clearCB(hzbViews, 0u); std::memcpy(dst, &c, sizeof(c)); };
        RecordComputeDispatch(renderer, cl, cullClearMat_.get(), cbSize, writeCBB,
            { perGroupVg_.Srv(f) },
            { cullUav_[5 * render::kFrameCount + f], cullUav_[2 * render::kFrameCount + f], deferredCountUav() },
            noSampler,
            hzbViews * numGroups, 1,
            indirectArgsB_.buffer.Get());
    }

    // S4: the camera's args, seeded from the camera's own bases (one view row).
    if (dec.gbuffer)
    {
        auto writeCBCam = [&](std::uint8_t* dst) { const ShadowCullCB c = clearCB(1u, camView); std::memcpy(dst, &c, sizeof(c)); };
        RecordComputeDispatch(renderer, cl, cullClearMat_.get(), cbSize, writeCBCam,
            { perGroupVgCam_.Srv(f) },
            { cullUav_[7 * render::kFrameCount + f], cullUav_[2 * render::kFrameCount + f], deferredCountUav() },
            noSampler,
            numGroups, 1,
            camArgs_.buffer.Get());
        // S5: pass B's args, the same seed (the post cull bumps their InstanceCount).
        if (dec.camHzb)
        {
            RecordComputeDispatch(renderer, cl, cullClearMat_.get(), cbSize, writeCBCam,
                { perGroupVgCam_.Srv(f) },
                { cullUav_[10 * render::kFrameCount + f], cullUav_[2 * render::kFrameCount + f], deferredCountUav() },
                noSampler,
                numGroups, 1,
                camArgsB_.buffer.Get());
        }
    }
    // S5: last frame's camera pyramid (t12) and the deferred list (u6); placeholders when off.
    // The CB says `on = 0` then, and the shader never reads either.
    const D3D12_CPU_DESCRIPTOR_HANDLE camHzbPrevSrv = (dec.camHzb && camHzbPrevSrv_.ptr != 0) ? camHzbPrevSrv_ : renderer->GetDeferredForFrame().shadowSRV;
    const D3D12_CPU_DESCRIPTOR_HANDLE camDeferredUav = cullUav_[9 * render::kFrameCount + f];
    CameraHzbParams camHzb = camHzbParams_;
    camHzb.on = dec.camHzb ? 1u : 0u;
    // S4: the camera tables of the cull (t9..t11, u4/u5); placeholders on a frame without the
    // camera row (the shader never reads them then, the table just may not have holes).
    const D3D12_CPU_DESCRIPTOR_HANDLE camGroupSrv = (dec.gbuffer && perGroupVgCam_.Valid()) ? perGroupVgCam_.Srv(f) : perGroupVg_.Srv(f);
    const D3D12_CPU_DESCRIPTOR_HANDLE camFlagSrv = (dec.gbuffer && casterCam_.Valid()) ? casterCam_.Srv(f) : casterGroup_.Srv(0);
    const D3D12_CPU_DESCRIPTOR_HANDLE camFadeSrv = (dec.gbuffer && casterFade_.Valid()) ? casterFade_.Srv(f) : casterGroup_.Srv(0);

    // Cull: frustum-test every caster into the visible list + InstanceCounts. Reads bounds from the
    // unified buffer (Step 2) when built, else the upload ring (fallback). S5b: two constant
    // buffers (b1 = the cascade HZB matrices), so the single-CBV helper does not fit -- the same
    // shape, spelled out.
    {
        auto cb0 = renderer->GetFrameResource()->AllocDynamic(cbSize, render::kConstantBufferAlignment);
        auto cb1 = renderer->GetFrameResource()->AllocDynamic(static_cast<UINT>(sizeof(render::CascadeHzb::GpuParams)), render::kConstantBufferAlignment);
        auto cb2 = renderer->GetFrameResource()->AllocDynamic(static_cast<UINT>(sizeof(CameraHzbParams)), render::kConstantBufferAlignment);
        if (cb0.cpu && cb1.cpu && cb2.cpu)
        {
            writeCB(static_cast<std::uint8_t*>(cb0.cpu));
            std::memcpy(cb1.cpu, &hzbParams_, sizeof(hzbParams_)); // the builder's snapshot (see PrepareCullPass)
            std::memcpy(cb2.cpu, &camHzb, sizeof(camHzb));         // S5: SceneRenderer's, this frame's decision applied

            auto h = renderer->GetRenderContextPool()->Acquire();
            RenderContext& rc = h.ref();
            rc.cbv[0] = cb0.gpu;
            rc.cbv[1] = cb1.gpu;
            rc.cbv[2] = cb2.gpu;
            rc.srvTable[0] = renderer->StageSrvUavTable({ BoundsReadSrv(f), viewFrustums_.Srv(f), casterGroup_.Srv(0), perGroupVg_.Srv(f),
                                                          CasterLodSrv(f), hzbSrv[0], hzbSrv[1], hzbSrv[2], hzbSrv[3],
                                                          camGroupSrv, camFlagSrv, camFadeSrv, camHzbPrevSrv }).gpu;
            rc.uavTable[0] = renderer->StageSrvUavTable({ cullUav_[f], cullUav_[render::kFrameCount + f],
                                                          deferredListUav(), deferredCountUav(),
                                                          cullUav_[7 * render::kFrameCount + f], cullUav_[8 * render::kFrameCount + f],
                                                          camDeferredUav,
                                                          // S7: the static rows (bound always: a volatile table has no holes)
                                                          cullUav_[16 * render::kFrameCount + f], cullUav_[17 * render::kFrameCount + f] }).gpu;
            rc.samplerTable[0] = noSampler;
            cullMat_->Bind(cl, rc);
            // Plan S9e: x the casters, y the views (shadow_cull_cs: one thread per pair; row 0 also the camera row).
            const UINT groupsX = (numCasters + kComputeDispatchGroupSize - 1u) / kComputeDispatchGroupSize;
            const UINT groupsY = (std::max(numViews, 1u) + kComputeDispatchGroupSize - 1u) / kComputeDispatchGroupSize;
            if (groupsX > 0) { cl->Dispatch(groupsX, groupsY, 1); }
            renderer->UAVBarrier(cl, indirectArgs_.buffer.Get());
        }
    }

    // Local-light plan S6a: the local views' non-empty records, once the cull's InterlockedAdds have landed (the UAV
    // barrier above) and before the validation copy takes the args to COPY_SOURCE.
    // Tessy N4: the records of groups with clusters go to the atlas cluster cull first, which zeroes them for the
    // compaction below (and the rows' counters start at zero). It also reads the caster ids the cull appended -- the
    // classic draw reads them through the input assembler, after a state change; this kernel needs their own barrier.
    if (dec.tessyAtlas || dec.tessyView || dec.tessyGBuffer) // the claim cursors and counters of every row set start at zero
    {
        const UINT zero[4] = { 0u, 0u, 0u, 0u };
        const D3D12_CPU_DESCRIPTOR_HANDLE counterUav = TessyAtlasDescriptor(f, 3u);
        cl->ClearUnorderedAccessViewUint(renderer->StageSrvUavTable({ counterUav }).gpu, counterUav,
                                         tessyAtlasCounter_.Get(), zero, 0, nullptr);
        renderer->UAVBarrier(cl, tessyAtlasCounter_.Get());
    }
    if (dec.tessyAtlas)
    {
        renderer->UAVBarrier(cl, visibleList_.buffer.Get());
        if (dec.localSplit && staticVisibleList_.Valid()) { renderer->UAVBarrier(cl, staticVisibleList_.buffer.Get()); }
        RecordTessyAtlasCull(renderer, cl, dec, 0u);
        renderer->UAVBarrier(cl, indirectArgs_.buffer.Get());
    }
    // The camera row: N5's G-buffer (culled by the camera frustum, taken from the classic G-buffer, by PSO run -- before
    // the merged commands' expand below reads the rows), or the cluster view alone (every cluster, nothing zeroed).
    if (dec.tessyGBuffer || dec.tessyView)
    {
        GPU_SCOPE(cl, ProfilerScopes::kTessyCameraCull);
        renderer->UAVBarrier(cl, camArgs_.buffer.Get());
        renderer->UAVBarrier(cl, camVisibleList_.buffer.Get());
        RecordTessyAtlasCull(renderer, cl, dec, kTessyCameraRowSet);
    }
    if (dec.localCompact)
    {
        RecordLocalArgsCompact(renderer, cl, dec.localViews, /*staticRows=*/false);
    }
    if (dec.localSplit) // S7: the static rows the same way, once the cull's adds on them have landed
    {
        renderer->UAVBarrier(cl, staticArgs_.buffer.Get());
        if (dec.tessyAtlas)
        {
            RecordTessyAtlasCull(renderer, cl, dec, 1u);
            renderer->UAVBarrier(cl, staticArgs_.buffer.Get());
        }
        RecordLocalArgsCompact(renderer, cl, dec.localViews, /*staticRows=*/true);
    }
    if (dec.tessyAtlas || dec.tessyView || dec.tessyGBuffer)
    {
        renderer->UAVBarrier(cl, tessyAtlasVisible_.Get());
        renderer->UAVBarrier(cl, tessyAtlasArgs_.Get());
    }

    // A6: pair the camera cull's counts with the PSO-sorted static part into the bindless commands
    // (pass A). The cull's InterlockedAdds on the camera args have to land before the kernel reads.
    if (dec.gbuffer && cmdMerge_[0])
    {
        renderer->UAVBarrier(cl, camArgs_.buffer.Get());
        RecordGBufferCommandExpand(renderer, cl, /*passB=*/false);
    }

    // Step 4 validation (temporary, one-shot after warmup): read back this region's args so a CPU
    // cull can be compared against them kFrameCount frames later. The decision, the snapshot and
    // the readback buffer are all the builder's (see PrepareCullPass) — what is left here is the
    // copy itself.
    if (dec.readback)
    {
        renderer->EmitPoint(cl, dec.valCopy);
        cl->CopyBufferRegion(valReadback_.Get(), 0, indirectArgs_.buffer.Get(),
                             static_cast<UINT64>(f) * indirectArgs_.regionBytes, indirectArgs_.regionBytes);
        if (dec.gbuffer)
        {
            cl->CopyBufferRegion(valReadback_.Get(), indirectArgs_.regionBytes, camArgs_.buffer.Get(),
                                 static_cast<UINT64>(f) * camArgs_.regionBytes, camArgs_.regionBytes);
        }
        // S5: the counters as the main cull left them -- [9] is what the camera row is short by.
        if (dec.camHzb)
        {
            cl->CopyBufferRegion(valReadback_.Get(), indirectArgs_.regionBytes + camArgs_.regionBytes, deferredCount_.buffer.Get(),
                                 static_cast<UINT64>(f) * deferredCount_.regionBytes, kHzbStatsBytes);
        }
    }

    // Step 6: leave the args in INDIRECT_ARGUMENT and the visible list as a vertex buffer so the
    // shadow passes (chained after this pass) can ExecuteIndirect + bind the per-instance stream
    // without touching state on their parallel CLs. Done every frame (harmless when the toggle is
    // off — nothing reads them); next frame's start transitions them back to UAV. Counts stays
    // UAV (Step 6 uses maxCount=1 per draw, so no count buffer is read).
    renderer->EmitPoint(cl, dec.consume);
    if (dec.tessyAtlasStats) // Tessy N4: the atlas cull's counters, read kFrameCount frames later (PollTessyAtlasStats)
    {
        cl->CopyBufferRegion(tessyAtlasStatsReadback_.Get(), static_cast<UINT64>(f) * kTessyAtlasCounterBytes, tessyAtlasCounter_.Get(),
                             static_cast<UINT64>(f) * kTessyAtlasCounterBytes, kTessyAtlasCounterBytes);
    }
}

// ---- S15: the SDSM directional re-cull -------------------------------------------------------

ShadowGpuData::DirectionalCullDecisions ShadowGpuData::PrepareDirectionalCullPass(
    RenderGraphPassContext& ctx, D3D12_CPU_DESCRIPTOR_HANDLE planeSrv, std::uint32_t views,
    D3D12_GPU_VIRTUAL_ADDRESS hzbCB, bool hzb)
{
    DirectionalCullDecisions dec{};
    tessySunFrame_ = false; // Main_SdsmShadow reads it whatever this pass decides
    if (!ctx.renderer || planeSrv.ptr == 0 || views == 0) { return dec; }
    if (ctx.renderer->GetCurrentFrameIndex() >= render::kFrameCount) { return dec; }
    if (!cullClearMat_ || !cullMat_ || numVirtualGroups_ == 0 || !IndirectDrawReady()) { return dec; }
    if (!indirectArgs_.Valid() || !visibleList_.Valid() || !indirectCounts_.Valid()) { return dec; }
    dec.active = true;
    dec.views = views;
    dec.planeSrv = planeSrv;
    dec.hzbCB = hzbCB;
    // Occlusion S5b for SDSM. Everything the Legacy chain needs is already here -- the deferred
    // list, the counters, the pass-B pair, the tile pyramids -- and only the MATRICES differed,
    // which the analyze now writes into `hzbCB`. The two-pass structure is what makes a poor
    // predictor safe: a caster last frame's tile hid is retested against THIS frame's tile after
    // pass A, so a partition that changed shape costs a pass-B draw, never a missing shadow.
    dec.hzb = hzb && hzbCB != 0 && cullPostMat_ && HzbRingsValid() && cascadeHzb_.Ready();
    hzbCullThisFrame_ = dec.hzb;
    if (dec.hzb)
    {
        for (unsigned c = 0; c < render::CascadeHzb::kCascades; ++c)
        {
            ctx.Use(cascadeHzb_.Pyramid(c), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }

    // The main cull left these in INDIRECT_ARGUMENT / vertex-buffer; they go back to UAV for the
    // re-seed and the re-cull, then forward again for the draws. Declared here rather than assumed
    // because this pass is the SECOND writer of the same rows in one frame, and a compiled barrier
    // set that does not know that would hand the draws a stale before-state.
    // Tessy (tessy.sdsm): the partitions' records of groups with clusters through the atlas kernel's row set 3, drawn by
    // the atlas mesh shader -- the cascade atlas has the local atlases' PSO state (D16, LESS_EQUAL, S6's bias). No
    // validation-frame gate: the readback compares the MAIN cull's rows, before this pass rewrites them.
    {
        Material* const sunMat = MaskedShadowsActive() ? tessyAtlasMeshMaskedMat_.get() : tessyAtlasMeshMat_.get();
        const bool pso = (tessyCameraCullMat_ || tessyAtlasCullMat_) && sunMat && sunMat->GetPipelineState();
        // tessy.lod turns the partitions' clusters on for itself, as it does the camera's: the cut reaches the shadow only
        // through them.
        const bool sunWanted = tessy::g_sdsm || tessyCamDagFrame_;
        dec.tessySun = tessy::g_enabled && sunWanted && TessyReady() && pso && tessyAtlasHeap_ &&
                       views <= render::kShadowViewCeiling && ctx.renderer->GetDispatchMeshCommandSignature() != nullptr;
        tessySunFrame_ = dec.tessySun;
        dec.tessySunClear = dec.tessySun && !tessyCountersCleared_;
        // tessy.lod: the camera's DAG cut, drawn by the partitions too (UE's rule for LODs: the shadow takes the LOD the
        // main view picked) -- with their discrete tier a far palm's shadow had LOD3's thinned fronds under a crown the
        // camera drew from the cut (wind_test aerial: 2.4 % of the frame over 8 levels, the floor 0-0.7 %). It costs the
        // SDSM cull + draw 0.015-0.06 ms (finer casters); tessy.sunCut:0 = the discrete tier, the A/B.
        dec.tessySunDag = dec.tessySun && tessyCamDagFrame_ && tessy::g_sunCameraCut;
        for (int i = 0; i < 4; ++i) { dec.tessyDagCam[i] = tessyDagCamFrame_[i]; }
        tessySunVerdict_ = dec.tessySun ? (dec.tessySunDag ? "on (the camera's DAG cut)" : "on")
            : !tessy::g_enabled ? "off (tessy.enabled 0)"
            : !sunWanted ? "off (tessy.sdsm 0)"
            : !TessyReady() ? "classic -- no caster mesh has clusters"
            : !pso ? "classic -- no atlas mesh shader PSO (no mesh shader tier, or it failed)"
            : "classic -- the buffers or the DispatchMesh signature are missing";
        if (tessySunVerdict_ != tessySunLogged_)
        {
            LOG_INFO(logging::LogCategory::RenderShadow, "tessy.sdsm: {} (frame {})", tessySunVerdict_,
                     ctx.renderer->GetTotalFrameNumber());
            tessySunLogged_ = tessySunVerdict_;
        }
    }

    dec.base = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(indirectArgs_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(visibleList_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(indirectCounts_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (dec.hzb)
    {
        ctx.Use(deferredList_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(deferredCount_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (dec.tessySun) // written by the kernel here, read by Main_SdsmShadow after the consume (as the main cull leaves them)
    {
        ctx.Use(tessyCamDeferred_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); // in the kernel's table, never written here
        ctx.Use(tessyAtlasVisible_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(tessyAtlasInfo_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(tessyAtlasArgs_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(tessyAtlasCounter_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    ctx.NextPoint();
    dec.consume = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(indirectArgs_.buffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    ctx.Use(visibleList_.buffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    if (dec.tessySun)
    {
        ctx.Use(tessyCamDeferred_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ctx.Use(tessyAtlasVisible_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ctx.Use(tessyAtlasInfo_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ctx.Use(tessyAtlasArgs_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        EnsureTessyAtlasStatsReadback(ctx.renderer);
        const UINT f = ctx.renderer->GetCurrentFrameIndex();
        dec.tessySunStats = tessyAtlasStatsReadback_ != nullptr;
        if (dec.tessySunStats)
        {
            ctx.Use(tessyAtlasCounter_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            tessyAtlasStatsFrame_[f] = ctx.renderer->GetTotalFrameNumber(); // the frame's slot holds the SDSM part too
        }
    }
    return dec;
}

ShadowGpuData::CullPostDecisions ShadowGpuData::PrepareSdsmCullPostPass(RenderGraphPassContext& ctx,
                                                                        D3D12_GPU_VIRTUAL_ADDRESS hzbCB)
{
    // The Legacy twin's gate, minus `cascadeHzb_.Active()` (which is CascadeHzb's own CPU matrix
    // history -- SDSM has none, its matrices live in `hzbCB`).
    CullPostDecisions dec{};
    if (!hzbCullThisFrame_ || hzbCB == 0 || !cullPostMat_ || !HzbRingsValid() || !cascadeHzb_.Ready()) { return dec; }
    if (ctx.renderer == nullptr || ctx.renderer->GetCurrentFrameIndex() >= render::kFrameCount) { return dec; }
    const UINT f = ctx.renderer->GetCurrentFrameIndex();
    dec.active = true;

    dec.base = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(indirectArgsB_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(visibleListB_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(deferredList_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(deferredCount_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (unsigned c = 0; c < render::CascadeHzb::kCascades; ++c)
    {
        ctx.Use(cascadeHzb_.Pyramid(c), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    ctx.NextPoint();
    dec.consume = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(indirectArgsB_.buffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    ctx.Use(visibleListB_.buffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    EnsureHzbStatsReadback(ctx.renderer);
    dec.stats = hzbStatsReadback_ != nullptr;
    if (dec.stats)
    {
        ctx.Use(deferredCount_.buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        hzbStatsFrame_[f] = ctx.renderer->GetTotalFrameNumber();
    }
    ctx.NextPoint();
    dec.restore = ctx.usePoint ? *ctx.usePoint : 0u;
    if (dec.stats) { ctx.Use(deferredCount_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
    return dec;
}

void ShadowGpuData::RecordSdsmCullPost(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                       const CullPostDecisions& dec, D3D12_GPU_VIRTUAL_ADDRESS hzbCB)
{
    if (!renderer || !cl || !dec.active || hzbCB == 0) { return; }
    const UINT f = renderer->GetCurrentFrameIndex();
    const std::uint32_t numCasters = cullCastersThisFrame_;
    const std::uint32_t numGroups = numVirtualGroups_;
    const std::uint32_t hzbViews = render::CascadeHzb::kCascades;

    renderer->EmitPoint(cl, dec.base);
    if (numCasters > 0 && numGroups > 0)
    {
        struct CullCB { std::uint32_t numCasters, numViews, numGroups, pad; };
        auto cb0 = renderer->GetFrameResource()->AllocDynamic(static_cast<UINT>(sizeof(CullCB)),
                                                              render::kConstantBufferAlignment);
        if (cb0.cpu)
        {
            const CullCB c{ numCasters, hzbViews, numGroups, 0u };
            std::memcpy(cb0.cpu, &c, sizeof(c));

            auto h = renderer->GetRenderContextPool()->Acquire();
            RenderContext& rc = h.ref();
            rc.cbv[0] = cb0.gpu;
            // b1 is the GPU-WRITTEN block: this frame's partition matrices, already z-flipped by
            // the analyze. shadow_cull_post_cs.hlsl cannot tell it from a CPU upload.
            rc.cbv[1] = hzbCB;
            rc.srvTable[0] = renderer->StageSrvUavTable({ BoundsReadSrv(f), casterGroup_.Srv(0), perGroupVg_.Srv(f), CasterLodSrv(f),
                                                          cascadeHzb_.Srv(0), cascadeHzb_.Srv(1), cascadeHzb_.Srv(2), cascadeHzb_.Srv(3) }).gpu;
            rc.uavTable[0] = renderer->StageSrvUavTable({ cullUav_[5 * render::kFrameCount + f], cullUav_[6 * render::kFrameCount + f],
                                                          cullUav_[3 * render::kFrameCount + f], cullUav_[4 * render::kFrameCount + f] }).gpu;
            rc.samplerTable[0] = D3D12_GPU_DESCRIPTOR_HANDLE{};
            cullPostMat_->Bind(cl, rc);
            const UINT groupsX = (numCasters + kComputeDispatchGroupSize - 1u) / kComputeDispatchGroupSize;
            if (groupsX > 0) { cl->Dispatch(groupsX, 1, 1); }
            renderer->UAVBarrier(cl, indirectArgsB_.buffer.Get());
        }
    }
    renderer->EmitPoint(cl, dec.consume);
    if (dec.stats)
    {
        cl->CopyBufferRegion(hzbStatsReadback_.Get(), static_cast<UINT64>(f) * kHzbStatsBytes,
                             deferredCount_.buffer.Get(), static_cast<UINT64>(f) * deferredCount_.regionBytes, kHzbStatsBytes);
    }
    renderer->EmitPoint(cl, dec.restore);
}

void ShadowGpuData::RecordDirectionalCull(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                          const DirectionalCullDecisions& dec)
{
    if (!renderer || !cl || !dec.active) { return; }
    const UINT f = renderer->GetCurrentFrameIndex();
    if (f >= render::kFrameCount) { return; }

    const std::uint32_t numGroups = numVirtualGroups_;
    // The SAME caster set the main cull walked this frame: the two must agree, or a row cleared
    // with one stride and filled with another puts a caster id in another group's slice.
    const std::uint32_t numCasters = cullCastersThisFrame_;

    // THE POINTS ARE EMITTED WHATEVER THE DATA SAYS. A body that early-returns past a point it
    // declared leaves the compiled barriers for that point unrecorded, and the compile's model is
    // then one transition ahead of the GPU -- silent corruption for every later user of those
    // buffers. The barrier comparator caught exactly this here ("pass=30 SKIPPED (body performed
    // nothing - registration unverified)") on the frames before the caster set exists. Same shape
    // as RecordCullPost: emit, guard only the work between.
    renderer->EmitPoint(cl, dec.base);
    // Every exit: tessy.sdsm's cull (the rows as re-culled, or none -- empty runs, so Main_SdsmShadow never draws a stale
    // frame's args), the consume, and the SDSM counters' copy behind the main cull's in the frame's readback slot.
    const auto finish = [&](bool recull)
    {
        if (dec.tessySun) { RecordTessySunCull_(renderer, cl, dec, /*empty=*/!recull); }
        renderer->EmitPoint(cl, dec.consume);
        if (dec.tessySun && dec.tessySunStats)
        {
            const UINT64 at = static_cast<UINT64>(f) * kTessyAtlasCounterBytes + kTessySunCounterOffset;
            cl->CopyBufferRegion(tessyAtlasStatsReadback_.Get(), at, tessyAtlasCounter_.Get(), at, kTessySunCounterBytes);
        }
    };
    if (numCasters == 0 || numGroups == 0)
    {
        finish(false);
        return;
    }

    // shadow_cull_cs.hlsl's CullParams: cascade rows only, so no split, no camera row, nothing local (S7, S9e).
    auto writeCB = [&](std::uint8_t* dst)
    {
        ShadowCullCB c{};
        c.numCasters = numCasters;
        c.numViews = dec.views;
        c.numGroups = numGroups;
        std::memcpy(dst, &c, sizeof(c));
    };
    const UINT cbSize = static_cast<UINT>(sizeof(ShadowCullCB));
    const D3D12_GPU_DESCRIPTOR_HANDLE noSampler{};
    const D3D12_CPU_DESCRIPTOR_HANDLE deferredCountUav = cullUav_[4 * render::kFrameCount + f];

    // Re-seed rows [0, views): the clear's thread mapping is (view * numGroups + group) over
    // `numViews * numGroups` threads, so a smaller numViews IS the sub-range -- no shader change.
    // It also re-zeroes DeferredCount[0..views), the CASCADE counters; the camera's ([8]..[10])
    // are past that and untouched, which is what keeps the camera HZB path intact.
    RecordComputeDispatch(renderer, cl, cullClearMat_.get(), cbSize, writeCB,
        { perGroupVg_.Srv(f) },
        { cullUav_[f], cullUav_[2 * render::kFrameCount + f], deferredCountUav },
        noSampler,
        dec.views * numGroups, 1,
        indirectArgs_.buffer.Get());

    // Re-cull them against the GPU-written partition boxes. The camera row is off (camView = ~0)
    // and the CAMERA HZB is off; the CASCADE HZB is on whenever `dec.hzb` says so, and then b1 is
    // the block the analyze wrote -- prev matrices from last frame's partitions, this frame's from
    // this frame's, both already z-flipped. shadow_cull_cs.hlsl is untouched either way.
    {
        auto cb0 = renderer->GetFrameResource()->AllocDynamic(cbSize, render::kConstantBufferAlignment);
        auto cb2 = renderer->GetFrameResource()->AllocDynamic(
            static_cast<UINT>(sizeof(CameraHzbParams)), render::kConstantBufferAlignment);
        // Same rule as above: an allocation failure skips the WORK, never the point.
        if (!cb0.cpu || !cb2.cpu) { finish(false); return; }
        writeCB(static_cast<std::uint8_t*>(cb0.cpu));
        CameraHzbParams camOff{};
        camOff.on = 0u;
        camOff.prevValid = 0u;
        std::memcpy(cb2.cpu, &camOff, sizeof(camOff));

        D3D12_GPU_VIRTUAL_ADDRESS cullHzbCB = dec.hzbCB;
        if (!dec.hzb)
        {
            // Not this frame: an all-zero block, whose `on` is 0. Allocated rather than reusing
            // the GPU one, because that one's `on` says otherwise.
            auto cb1 = renderer->GetFrameResource()->AllocDynamic(
                static_cast<UINT>(sizeof(render::CascadeHzb::GpuParams)), render::kConstantBufferAlignment);
            if (!cb1.cpu) { finish(false); return; }
            render::CascadeHzb::GpuParams hzbOff{};
            hzbOff.on = 0u;
            std::memcpy(cb1.cpu, &hzbOff, sizeof(hzbOff));
            cullHzbCB = cb1.gpu;
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE stand = renderer->GetDeferredForFrame().shadowSRV;
        const auto pyramid = [&](unsigned c)
        {
            return (dec.hzb && cascadeHzb_.Srv(c).ptr != 0) ? cascadeHzb_.Srv(c) : stand;
        };
        auto h = renderer->GetRenderContextPool()->Acquire();
        RenderContext& rc = h.ref();
        rc.cbv[0] = cb0.gpu;
        rc.cbv[1] = cullHzbCB;
        rc.cbv[2] = cb2.gpu;
        // t1 is THE difference from the main cull: the planes Main_SdsmAnalyze wrote, instead of
        // the CPU ring. The camera tables stand in with any valid descriptor (the shader reads
        // none of them here); t5..t8 are the real tile pyramids when the occlusion test is on.
        rc.srvTable[0] = renderer->StageSrvUavTable({ BoundsReadSrv(f), dec.planeSrv, casterGroup_.Srv(0),
                                                      perGroupVg_.Srv(f), CasterLodSrv(f),
                                                      pyramid(0), pyramid(1), pyramid(2), pyramid(3),
                                                      perGroupVg_.Srv(f), casterGroup_.Srv(0),
                                                      casterGroup_.Srv(0), stand }).gpu;
        rc.uavTable[0] = renderer->StageSrvUavTable({ cullUav_[f], cullUav_[render::kFrameCount + f],
                                                      cullUav_[3 * render::kFrameCount + f], deferredCountUav,
                                                      cullUav_[7 * render::kFrameCount + f],
                                                      cullUav_[8 * render::kFrameCount + f],
                                                      cullUav_[9 * render::kFrameCount + f],
                                                      cullUav_[16 * render::kFrameCount + f], // S7: unread (split 0)
                                                      cullUav_[17 * render::kFrameCount + f] }).gpu;
        rc.samplerTable[0] = noSampler;
        cullMat_->Bind(cl, rc);
        // Plan S9e: one thread per (caster, view), as the main cull (four cascade rows -> one group row).
        const UINT groupsX = (numCasters + kComputeDispatchGroupSize - 1u) / kComputeDispatchGroupSize;
        const UINT groupsY = (std::max(dec.views, 1u) + kComputeDispatchGroupSize - 1u) / kComputeDispatchGroupSize;
        if (groupsX > 0) { cl->Dispatch(groupsX, groupsY, 1); }
        renderer->UAVBarrier(cl, indirectArgs_.buffer.Get());
    }

    finish(true);
}

// tessy.sdsm: the partitions' records of groups with clusters, cluster by cluster -- the atlas kernel's row set 3 over
// rows 0..views against the analyze's 16 planes, kTessySunSplit 256-thread groups a partition (each a slice of its
// virtual groups and its own run). The SDSM counters are cleared first (the main cull and the camera's post pass used the
// rest of the region). `empty`: the re-cull did not run -- every run is written empty, so the draw reads nothing stale.
void ShadowGpuData::RecordTessySunCull_(Renderer* renderer, ID3D12GraphicsCommandList* cl, const DirectionalCullDecisions& dec,
                                        bool empty)
{
    const UINT f = renderer->GetCurrentFrameIndex();
    GPU_SCOPE(cl, ProfilerScopes::kTessySunCull);
    if (dec.tessySunClear) // the main cull zeroed nothing this frame (no Tessy row set there): its own range
    {
        const UINT zero[4] = { 0u, 0u, 0u, 0u };
        const D3D12_CPU_DESCRIPTOR_HANDLE counterUav = TessyAtlasDescriptor(f, 8u);
        cl->ClearUnorderedAccessViewUint(renderer->StageSrvUavTable({ counterUav }).gpu, counterUav, tessyAtlasCounter_.Get(), zero,
                                         0, nullptr);
        renderer->UAVBarrier(cl, tessyAtlasCounter_.Get());
    }
    renderer->UAVBarrier(cl, visibleList_.buffer.Get()); // the re-cull's appends, read by the kernel
    CullDecisions sun{};
    sun.localViews = dec.views;
    for (int i = 0; i < 4; ++i) { sun.tessyWind0[i] = dec.tessyWind0[i]; sun.tessyWind1[i] = dec.tessyWind1[i]; }
    sun.tessyAtlasDag = dec.tessySunDag; // the camera's cut (RecordTessyAtlasCull: kFlagCameraCut)
    for (int i = 0; i < 4; ++i) { sun.tessyDagCam[i] = dec.tessyDagCam[i]; }
    RecordTessyAtlasCull(renderer, cl, sun, kTessySunRowSet, dec.planeSrv, empty, dec.partitionSrv, dec.tileTexels);
    renderer->UAVBarrier(cl, indirectArgs_.buffer.Get());
    renderer->UAVBarrier(cl, tessyAtlasVisible_.Get());
    renderer->UAVBarrier(cl, tessyAtlasArgs_.Get());
}

void ShadowGpuData::RecordTessySunDraw(Renderer* renderer, ID3D12GraphicsCommandList* cl, std::uint32_t partition,
                                       D3D12_GPU_VIRTUAL_ADDRESS viewCB)
{
    if (!tessySunFrame_ || !renderer || !cl || viewCB == 0 || partition >= render::kShadowViewCeiling) { return; }
    const UINT f = renderer->GetCurrentFrameIndex();
    Material* const tm = MaskedShadowsActive() ? tessyAtlasMeshMaskedMat_.get() : tessyAtlasMeshMat_.get();
    ID3D12CommandSignature* const meshSig = renderer->GetDispatchMeshCommandSignature();
    if (f >= render::kFrameCount || !tm || !meshSig) { return; }
    GPU_SCOPE(cl, ProfilerScopes::kTessySunDraw);
    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    RenderContext& ctx = ctxHandle.ref();
    ctx.cbv[1] = viewCB;
    ctx.srvTable[0] = TessyAtlasDrawTable(renderer, f);
    // The partition's kTessySunSplit runs (one per cull group), one DispatchMesh each: b0 names the run, the triangle
    // cull and the tile's content rect (BindShadowTarget's viewport).
    const float texels = renderer->ShadowTileContentTexels();
    std::uint32_t texelBits;
    std::memcpy(&texelBits, &texels, sizeof(texelBits));
    for (std::uint32_t s = 0; s < kTessySunSplit; ++s)
    {
        const std::uint32_t row = kTessySunRowSet * render::kShadowViewCeiling + partition * kTessySunSplit + s;
        ctx.constants[0] = { row, tessy::g_vsmMsCull & 1u, texelBits, texelBits };
        tm->Bind(cl, ctx, false);
        renderer->ExecuteIndirect(cl, meshSig, 1, tessyAtlasArgs_.Get(),
            static_cast<UINT64>(f) * kTessyAtlasArgsRegionBytes + static_cast<UINT64>(row) * 12u, nullptr, 0);
    }
}

std::uint32_t ShadowGpuData::CountCpuTailCasters(
    const std::vector<std::unique_ptr<RenderableObjectBase>>& objects) const
{
    std::uint32_t n = 0;
    for (const auto& objPtr : objects)
    {
        const RenderableObjectBase* obj = objPtr.get();
        if (!obj || !obj->IsGpuInstancedCaster()) { continue; }
        if (IsGiFoldedActive(obj)) { continue; }
        if (!obj->IsVisible() || !obj->CastsShadow()) { continue; }
        n += std::max<std::uint32_t>(1u, obj->GetInstanceCasterCount());
    }
    return n;
}

// ---- S5b: the cascade HZB post cull ---------------------------------------------------------

ShadowGpuData::CullPostDecisions ShadowGpuData::PrepareCullPostPass(RenderGraphPassContext& ctx)
{
    CullPostDecisions dec{};
    if (!hzbCullThisFrame_ || !cullPostMat_ || !HzbRingsValid() || !cascadeHzb_.Ready()) { return dec; }
    if (ctx.renderer == nullptr || ctx.renderer->GetCurrentFrameIndex() >= render::kFrameCount) { return dec; }
    const UINT f = ctx.renderer->GetCurrentFrameIndex();
    dec.active = true;

    // Where the main cull left them; declared again so the point is complete on its own.
    dec.base = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(indirectArgsB_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(visibleListB_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(deferredList_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(deferredCount_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (unsigned c = 0; c < render::CascadeHzb::kCascades; ++c)
    {
        ctx.Use(cascadeHzb_.Pyramid(c), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // Pass B consumes the args + list exactly as the main pair; the counters go out to the
    // readout (a 32-byte copy) and back to their resting state.
    ctx.NextPoint();
    dec.consume = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(indirectArgsB_.buffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    ctx.Use(visibleListB_.buffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    EnsureHzbStatsReadback(ctx.renderer);
    dec.stats = hzbStatsReadback_ != nullptr;
    if (dec.stats)
    {
        ctx.Use(deferredCount_.buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        hzbStatsFrame_[f] = ctx.renderer->GetTotalFrameNumber(); // cross-frame state: the builder's
    }
    ctx.NextPoint();
    dec.restore = ctx.usePoint ? *ctx.usePoint : 0u;
    if (dec.stats) { ctx.Use(deferredCount_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
    return dec;
}

void ShadowGpuData::RecordCullPost(Renderer* renderer, ID3D12GraphicsCommandList* cl, const CullPostDecisions& dec)
{
    if (!renderer || !cl || !dec.active) { return; }
    const UINT f = renderer->GetCurrentFrameIndex();
    const std::uint32_t numCasters = cullCastersThisFrame_;
    const std::uint32_t numGroups = numVirtualGroups_;
    const std::uint32_t hzbViews = render::CascadeHzb::kCascades;

    renderer->EmitPoint(cl, dec.base);
    if (numCasters > 0 && numGroups > 0)
    {
        struct CullCB { std::uint32_t numCasters, numViews, numGroups, pad; };
        auto cb0 = renderer->GetFrameResource()->AllocDynamic(static_cast<UINT>(sizeof(CullCB)), render::kConstantBufferAlignment);
        auto cb1 = renderer->GetFrameResource()->AllocDynamic(static_cast<UINT>(sizeof(render::CascadeHzb::GpuParams)), render::kConstantBufferAlignment);
        if (cb0.cpu && cb1.cpu)
        {
            const CullCB c{ numCasters, hzbViews, numGroups, 0u };
            std::memcpy(cb0.cpu, &c, sizeof(c));
            // The same snapshot as the main cull's: the post cull reads only `viewProj` (this
            // frame's matrices) and the rect/size, which do not change between the two.
            std::memcpy(cb1.cpu, &hzbParams_, sizeof(hzbParams_));

            auto h = renderer->GetRenderContextPool()->Acquire();
            RenderContext& rc = h.ref();
            rc.cbv[0] = cb0.gpu;
            rc.cbv[1] = cb1.gpu;
            rc.srvTable[0] = renderer->StageSrvUavTable({ BoundsReadSrv(f), casterGroup_.Srv(0), perGroupVg_.Srv(f), CasterLodSrv(f),
                                                          cascadeHzb_.Srv(0), cascadeHzb_.Srv(1), cascadeHzb_.Srv(2), cascadeHzb_.Srv(3) }).gpu;
            rc.uavTable[0] = renderer->StageSrvUavTable({ cullUav_[5 * render::kFrameCount + f], cullUav_[6 * render::kFrameCount + f],
                                                          cullUav_[3 * render::kFrameCount + f], cullUav_[4 * render::kFrameCount + f] }).gpu;
            rc.samplerTable[0] = D3D12_GPU_DESCRIPTOR_HANDLE{};
            cullPostMat_->Bind(cl, rc);
            // numthreads(8, 8, 1): x walks the deferred list, y the four cascades -- one group row.
            const UINT groupsX = (numCasters + kComputeDispatchGroupSize - 1u) / kComputeDispatchGroupSize;
            if (groupsX > 0) { cl->Dispatch(groupsX, 1, 1); }
            renderer->UAVBarrier(cl, indirectArgsB_.buffer.Get());
        }
    }
    renderer->EmitPoint(cl, dec.consume);
    if (dec.stats)
    {
        cl->CopyBufferRegion(hzbStatsReadback_.Get(), static_cast<UINT64>(f) * kHzbStatsBytes,
                             deferredCount_.buffer.Get(), static_cast<UINT64>(f) * deferredCount_.regionBytes, kHzbStatsBytes);
    }
    renderer->EmitPoint(cl, dec.restore);
}

void ShadowGpuData::EnsureHzbStatsReadback(Renderer* renderer)
{
    if (hzbStatsReadback_ || !renderer || !renderer->GetDevice()) { return; }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(kHzbStatsBytes) * render::kFrameCount;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(gpu::CreateCommittedResource(renderer->GetDevice(), &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, MemTag::ShadowGpuData, L"ShadowGpuData.HzbStatsReadback",
                                            IID_PPV_ARGS(hzbStatsReadback_.GetAddressOf()))))
    {
        hzbStatsReadback_.Reset();
        return;
    }
    hzbStatsFrame_.fill(0);
}

void ShadowGpuData::EnsureTessyAtlasStatsReadback(Renderer* renderer)
{
    if (tessyAtlasStatsReadback_ || !renderer || !renderer->GetDevice()) { return; }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(kTessyAtlasCounterBytes) * render::kFrameCount;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(gpu::CreateCommittedResource(renderer->GetDevice(), &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, MemTag::ShadowTessy, L"Tessy.AtlasStatsReadback",
                                            IID_PPV_ARGS(tessyAtlasStatsReadback_.GetAddressOf()))))
    {
        tessyAtlasStatsReadback_.Reset();
        return;
    }
    tessyAtlasStatsFrame_.fill(0);
}

void ShadowGpuData::PollTessyAtlasStats(Renderer* renderer)
{
    // As PollHzbStats: the slot of frame N - kFrameCount, whose fence passed in this frame's BeginFrame.
    if (!tessyAtlasStatsReadback_ || !renderer) { return; }
    const std::uint64_t now = renderer->GetTotalFrameNumber();
    if (now < render::kFrameCount) { return; }
    const std::uint64_t want = now - render::kFrameCount;
    for (UINT s = 0; s < render::kFrameCount; ++s)
    {
        if (tessyAtlasStatsFrame_[s] != want || want == 0) { continue; }
        const D3D12_RANGE range{ s * kTessyAtlasCounterBytes, (s + 1u) * kTessyAtlasCounterBytes };
        void* mapped = nullptr;
        if (FAILED(tessyAtlasStatsReadback_->Map(0, &range, &mapped)) || !mapped) { return; }
        std::memcpy(tessyAtlasStats_.data(), static_cast<const std::uint8_t*>(mapped) + s * kTessyAtlasCounterBytes, kTessyAtlasCounterBytes);
        const D3D12_RANGE noWrite{ 0, 0 };
        tessyAtlasStatsReadback_->Unmap(0, &noWrite);
        tessyAtlasStatsFrame_[s] = 0;
        LOG_INFO_THROTTLED(std::chrono::seconds(5), logging::LogCategory::RenderShadow,
                           "tessy.atlas: {} + {} meshlet records, camera {} (cap {} a row set), {} views did not fit, "
                           "{} clusters tested, {} camera clusters deferred to pass B, camera pass A {} triangles ({} DAG clusters); "
                           "sdsm {} meshlet records, {} runs did not fit, {} clusters tested, {} triangles",
                           tessyAtlasStats_[0], tessyAtlasStats_[1], tessyAtlasStats_[2], kTessyAtlasRowSetCap, tessyAtlasStats_[3],
                           tessyAtlasStats_[4], tessyAtlasStats_[5], tessyAtlasStats_[6], tessyAtlasStats_[7],
                           tessyAtlasStats_[8], tessyAtlasStats_[9], tessyAtlasStats_[10], tessyAtlasStats_[11]);
        return;
    }
}

void ShadowGpuData::PollHzbStats(Renderer* renderer)
{
    // The slot of frame N - kFrameCount: its fence passed in this frame's BeginFrame.
    if (!hzbStatsReadback_ || !renderer) { return; }
    const std::uint64_t now = renderer->GetTotalFrameNumber();
    if (now < render::kFrameCount) { return; }
    const std::uint64_t want = now - render::kFrameCount;
    for (UINT s = 0; s < render::kFrameCount; ++s)
    {
        if (hzbStatsFrame_[s] != want || want == 0) { continue; }
        const UINT64 begin = static_cast<UINT64>(s) * kHzbStatsBytes;
        const D3D12_RANGE range{ begin, begin + kHzbStatsBytes };
        void* mapped = nullptr;
        if (FAILED(hzbStatsReadback_->Map(0, &range, &mapped)) || !mapped) { return; }
        std::memcpy(hzbStats_.data(), static_cast<const std::uint8_t*>(mapped) + begin, kHzbStatsBytes);
        const D3D12_RANGE noWrite{ 0, 0 };
        hzbStatsReadback_->Unmap(0, &noWrite);
        hzbStatsFrame_[s] = 0;
        return;
    }
}

void ShadowGpuData::SetCascadeHzbViews(Renderer* renderer, const Math::mat4* lightViewProj, std::uint64_t frameNumber,
                                       bool wantOn, UINT contentRes)
{
    const bool ready = wantOn && renderer && cascadeHzb_.EnsureResources(renderer, contentRes);
    cascadeHzb_.SetFrameViews(lightViewProj, frameNumber, ready && IndirectDrawReady() && HzbRingsValid());
}

ShadowGpuData::SdsmHzbState ShadowGpuData::SetSdsmHzbViews(Renderer* renderer, std::uint64_t frameNumber,
                                                            bool wantOn, UINT contentRes)
{
    SdsmHzbState s{};
    const bool ready = wantOn && renderer && cascadeHzb_.EnsureResources(renderer, contentRes);
    const bool on = ready && IndirectDrawReady() && HzbRingsValid();
    cascadeHzb_.SetFrameExternalViews(frameNumber, on);
    s.on = on && cascadeHzb_.Active();
    if (!s.on) { return s; }
    // The rows FillParams would have produced, minus the matrices (the analyze has those).
    // Half the content: hzb_cull.hlsli derives mip-0 texels as view pixels >> 1, and mip 0 is a
    // quarter of the tile -- CascadeHzb::EnsureResources' own arithmetic, not a second guess.
    for (unsigned c = 0; c < render::CascadeHzb::kCascades; ++c)
    {
        s.prevValid[c] = cascadeHzb_.PrevValid(c) ? 1u : 0u;
    }
    render::CascadeHzb::GpuParams p{};
    cascadeHzb_.FillParams(p, true);
    s.viewRect[0] = p.viewRect[0];
    s.viewRect[1] = p.viewRect[1];
    s.viewRect[2] = p.viewRect[2];
    s.viewRect[3] = p.viewRect[3];
    s.hzbSize[0] = p.hzbSize[0];
    s.hzbSize[1] = p.hzbSize[1];
    return s;
}

bool ShadowGpuData::IndirectDrawReady() const
{
    return indirectShadowMat_ && indirectShadowMat_->GetPipelineState() &&
           count_ > 0 && numMeshGroups_ > 0 && !groupMesh_.empty() &&
           indirectArgs_.Valid() && visibleList_.Valid() && instances_.Valid();
}

bool ShadowGpuData::RecordIndirectShadowDraws(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                              std::uint32_t viewSlot, D3D12_GPU_VIRTUAL_ADDRESS viewCB,
                                              bool passB, bool staticRows, bool linearDepth)
{
    if (!renderer || !cl || !IndirectDrawReady()) { return false; }
    // S7: the static rows exist only on a split frame, only for the compacted local views.
    if (staticRows && (passB || !localSplitFrame_ || !localCompactFrame_ || viewSlot < render::kShadowViewCascades ||
                       viewSlot - render::kShadowViewCascades >= localCompactViews_)) { return false; }
    const UINT f = renderer->GetCurrentFrameIndex();
    if (f >= render::kFrameCount) { return false; }
    ID3D12CommandSignature* sig = renderer->GetDrawIndexedCommandSignature();
    if (!sig) { return false; }
    // S5b: pass B's pair has cascade rows only, laid out exactly like the main pair's first rows.
    if (passB && (viewSlot >= render::CascadeHzb::kCascades || !indirectArgsB_.Valid() || !visibleListB_.Valid())) { return false; }
    const UavRing& args = passB ? indirectArgsB_ : indirectArgs_;
    const UavRing& list = passB ? visibleListB_ : visibleList_;

    // Bind the depth-only indirect PSO + root args: b1 = light viewProj, t0 = instance buffer
    // SRV for this frame's region. C2: with masked groups present, the masked PSO also reads
    // casterGroup (t1) + groupMask (t2) + the masked albedo table (t3..).
    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    RenderContext& ctx = ctxHandle.ref();
    ctx.cbv[1] = viewCB;
    {
        // Once a frame for every view (drawTablesMutex_ says why).
        std::lock_guard<std::mutex> lock(drawTablesMutex_);
        const std::uint64_t frame = renderer->GetTotalFrameNumber();
        if (drawTablesFrame_ != frame || drawTablesStagedGeneration_ != drawTablesGeneration_)
        {
            drawTable3_ = {};
            if (MaskedShadowsActive())
            {
                drawTable0_ = renderer->StageSrvUavTable({ InstanceReadSrv(f), CasterGroupSrv(), GroupMaskSrv() }).gpu;
                // A6: the bindless masked PSO has no albedo table -- GroupMask.x is the heap index.
                if (!MaskedShadowsBindless()) { drawTable3_ = renderer->StageSrvUavTable(maskedAlbedoSrvs_, maskedAlbedoCount_).gpu; }
            }
            else
            {
                drawTable0_ = renderer->StageSrvUavTable({ InstanceReadSrv(f) }).gpu; // unified copy (Step 2), else ring
            }
            drawTablesFrame_ = frame;
            drawTablesStagedGeneration_ = drawTablesGeneration_;
        }
        ctx.srvTable[0] = drawTable0_;
        if (drawTable3_.ptr != 0) { ctx.srvTable[3] = drawTable3_; }
    }
    Material* const mat = IndirectShadowMaterial();
    Material* const linear = linearDepth ? mat->LinearDepthVariant() : nullptr; // plan S4
    (linear ? linear : mat)->Bind(cl, ctx, false);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Slot 1 = this frame's visible-list region as a per-instance stream; each draw's
    // StartInstanceLocation (baked by the cull) offsets into it.
    const UavRing& visible = staticRows ? staticVisibleList_ : list; // S7: the static rows' own list
    D3D12_VERTEX_BUFFER_VIEW visVBV{};
    visVBV.BufferLocation = visible.buffer->GetGPUVirtualAddress() +
                            static_cast<UINT64>(f) * visible.regionBytes;
    visVBV.SizeInBytes = static_cast<UINT>(visible.regionBytes);
    visVBV.StrideInBytes = sizeof(std::uint32_t);
    cl->IASetVertexBuffers(1, 1, &visVBV);

    // Local-light plan S6a: an atlas local view whose rows were compacted this frame (PrepareCullPass decided it on
    // the main thread) is ONE ExecuteIndirect over its non-empty records, the mega VB/IB bound once -- not a bind and
    // a draw per mesh group that had a caster anywhere in the scene.
    if (!passB && localCompactFrame_ && viewSlot >= render::kShadowViewCascades &&
        viewSlot - render::kShadowViewCascades < localCompactViews_)
    {
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = megaVB_.Get()->GetGPUVirtualAddress();
        vbv.SizeInBytes = megaVBBytes_;
        vbv.StrideInBytes = megaStride_;
        cl->IASetVertexBuffers(0, 1, &vbv);
        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = megaIB_.Get()->GetGPUVirtualAddress();
        ibv.SizeInBytes = megaIBBytes_;
        ibv.Format = megaIndexFormat_;
        cl->IASetIndexBuffer(&ibv);
        const std::uint32_t local = viewSlot - render::kShadowViewCascades;
        const UavRing& cargs = staticRows ? localStaticArgs_ : localArgs_;     // S7
        const UavRing& ccounts = staticRows ? localStaticCounts_ : localCounts_;
        renderer->ExecuteIndirect(cl, sig, numVirtualGroups_, cargs.buffer.Get(),
            static_cast<UINT64>(f) * cargs.regionBytes +
                static_cast<UINT64>(local) * numVirtualGroups_ * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
            ccounts.buffer.Get(),
            static_cast<UINT64>(f) * ccounts.regionBytes + static_cast<UINT64>(local) * sizeof(std::uint32_t));
        // Tessy N4: the records tessy_atlas_cull_cs took out of these rows, as this view's surviving meshlets -- one
        // DispatchMesh, the same target, the view's own PerView; b0 names the view's run (row set x view).
        if (tessyAtlasFrame_)
        {
            Material* const tm = MaskedShadowsActive() ? tessyAtlasMeshMaskedMat_.get() : tessyAtlasMeshMat_.get();
            Material* const tml = (tm && linearDepth) ? tm->LinearDepthVariant() : nullptr;
            ID3D12CommandSignature* const meshSig = renderer->GetDispatchMeshCommandSignature();
            if (tm && meshSig && (!linearDepth || tml))
            {
                const D3D12_GPU_DESCRIPTOR_HANDLE table = TessyAtlasDrawTable(renderer, f);
                auto tctxHandle = renderer->GetRenderContextPool()->Acquire();
                RenderContext& tctx = tctxHandle.ref();
                tctx.cbv[1] = viewCB;
                tctx.srvTable[0] = table;
                const std::uint32_t row = (staticRows ? render::kShadowViewCeiling : 0u) + local;
                // The mesh shader's triangle cull on the view's viewport (the face / spot rectangle, UpdateViewFrustums).
                const float texels = viewSlot < cpuViewRect_.size() ? cpuViewRect_[viewSlot].size.x : 0.0f;
                std::uint32_t texelBits;
                std::memcpy(&texelBits, &texels, sizeof(texelBits));
                tctx.constants[0] = { row, texels > 0.0f ? (tessy::g_vsmMsCull & 1u) : 0u, texelBits, texelBits };
                (tml ? tml : tm)->Bind(cl, tctx, false);
                renderer->ExecuteIndirect(cl, meshSig, 1, tessyAtlasArgs_.Get(),
                    static_cast<UINT64>(f) * kTessyAtlasArgsRegionBytes + static_cast<UINT64>(row) * 12u, nullptr, 0);
            }
        }
        return true;
    }

    const UINT64 argRegionBase = static_cast<UINT64>(f) * args.regionBytes;
    const Mesh* boundMesh = nullptr; // B3: a mesh's submesh groups are contiguous — bind once
    UINT boundLod = 0xFFFFFFFFu;
    // S3.6: one draw per (view, VIRTUAL group) = (mesh group, receiver LOD). Buckets nobody landed
    // in this frame are skipped on the CPU, so the kMaxShadowLods-fold of the group axis does NOT
    // become a kMaxShadowLods-fold of ExecuteIndirect calls — only groups whose instances actually
    // straddle two LODs cost a second draw.
    for (std::uint32_t vg = 0; vg < numVirtualGroups_; ++vg)
    {
        if (vg < vgCasterCount_.size() && vgCasterCount_[vg] == 0u) { continue; }
        const std::uint32_t g = vg / render::kMaxShadowLods;
        const std::uint32_t vgLod = vg % render::kMaxShadowLods;
        const Mesh* mesh = (g < groupMesh_.size()) ? groupMesh_[g] : nullptr;
        if (!mesh) { continue; }
        ID3D12Resource* vb = mesh->GetVertexBufferResource();
        // The bucket's own LOD IS the receiver's LOD (UE's rule). GI groups have only LOD0 geometry
        // registered, so they stay there. No per-view term: viewLod_ is 0 for cascades by S3.6, and
        // the ranges the cull-clear seeded come from the same (group, lod) row, so IB and index
        // range can never disagree -- which is exactly how they used to.
        const UINT groupLod = (g < numStaticGroups_) ? mesh->ClampExplicitLod(vgLod) : 0u;
        ID3D12Resource* ib = mesh->GetLodIndexBufferResource(groupLod);
        if (!vb || !ib) { continue; }

        if (mesh != boundMesh || groupLod != boundLod)
        {
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
            boundMesh = mesh;
            boundLod = groupLod;
        }

        // One indirect draw per (view, mesh-group). InstanceCount (from the cull) may be 0 -> a
        // free no-op, so empty groups cost nothing beyond the binding. B3: the args carry the
        // group's submesh StartIndexLocation (seeded by the cull-clear from PerGroup).
        const UINT64 argOffset = argRegionBase +
            static_cast<UINT64>(viewSlot * numVirtualGroups_ + vg) * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        renderer->ExecuteIndirect(cl, sig, 1, args.buffer.Get(), argOffset, nullptr, 0);
    }
    return true;
}

void ShadowGpuData::EnsureMegaBuffer(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer || !renderer->GetDevice() || !cl || megaBuilt_ || !megaWanted_) { return; }
    megaBuilt_ = true; // one-shot: never re-attempt (a failed alloc falls back to per-group binding)

    auto makeBuf = [&](UINT bytes, GpuResource& out, const wchar_t* name) -> bool
    {
        // Reset now unregisters as well as frees — the previous ComPtr::Reset() dropped the
        // prior level's buffer while leaving its registry entry behind (measured: net 2 -> 5).
        out.Reset(); // safe: EnsureMegaBuffer runs GPU-idle at load
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
        Microsoft::WRL::ComPtr<ID3D12Resource> buf;
        HRESULT hr = gpu::CreateCommittedResource(renderer->GetDevice(),
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, MemTag::ShadowGpuData, name, IID_PPV_ARGS(buf.GetAddressOf()));
        if (FAILED(hr) || !buf) { return false; }
        // Geometry is never transitioned, so it rests where it is created.
        out.Attach(renderer->Declarations(), std::move(buf), D3D12_RESOURCE_STATE_COMMON, name);
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
    // B3: one copy per UNIQUE mesh (submesh groups share their mesh's slice).
    UINT voff = 0, ioff = 0;
    for (const MegaCopy& mc : megaCopy_)
    {
        ID3D12Resource* vb = mc.mesh ? mc.mesh->GetVertexBufferResource() : nullptr;
        if (vb) { cl->CopyBufferRegion(megaVB_.Get(), voff, vb, 0, mc.vbBytes); }
        voff += mc.vbBytes;
        // Concatenate this mesh's LOD index buffers ([LOD0|LOD1|...]); layout in Rebuild matched this
        // order (meshLodIBBase). Each LOD IB is COMMON (Mesh::AddLod) so the copy implicitly promotes.
        for (std::uint32_t L = 0; mc.mesh && L < mc.lodCount; ++L)
        {
            ID3D12Resource* ib = mc.mesh->GetLodIndexBufferResource(L);
            const UINT bytes = mc.mesh->GetLodIndexCount(L) * 4u; // R32 indices
            if (ib && bytes > 0) { cl->CopyBufferRegion(megaIB_.Get(), ioff, ib, 0, bytes); }
            ioff += bytes;
        }
        if (mc.mesh && mc.dag) // N7: the cluster DAG's indices (COMMON, as the LOD IBs)
        {
            const UINT bytes = mc.mesh->GetDagIndexCount() * 4u;
            if (bytes > 0) { cl->CopyBufferRegion(megaIB_.Get(), ioff, mc.mesh->GetDagIndexBufferResource(), 0, bytes); }
            ioff += bytes;
        }
    }
    megaReady_ = true;
    UploadTessyTables_(renderer, cl);
}

// Tessy N2: per virtual group (group x shadow LOD) the clusters of exactly the index range the classic
// args draw -- the group's submesh in that LOD, mega-absolute (the mega block's lodBase + the cluster's
// LOD-relative start). The submesh range comes from the EXPLICIT LOD's table (GetLodDrawInfo), as the
// clusters do: SubmeshesForLod applies the runtime LOD overrides, which the clusters know nothing of.
// GI groups draw their whole LOD0. Duplicated LODs (a mesh with fewer than kMaxShadowLods) share a range.
void ShadowGpuData::BuildTessyTables_(const std::unordered_map<const Mesh*, std::string>& meshPath)
{
    tessyClusterCpu_.clear();
    tessyMeshletsCpu_.clear();
    tessyVertsCpu_.clear();
    tessyTrisCpu_.clear();
    tessyVgCpu_.assign(static_cast<size_t>(numMeshGroups_) * render::kMaxShadowLods, DirectX::XMUINT2{ 0u, 0u });
    tessyGroupsWith_ = tessyGroupsTotal_ = 0;
    tessyClusterLodCpu_.clear();
    tessyDagGroupCpu_.assign(numMeshGroups_, DirectX::XMUINT4{ 0u, 0u, 0u, 0u });
    tessyDagSphereCpu_.assign(numMeshGroups_, DirectX::XMFLOAT4{ 0.0f, 0.0f, 0.0f, 0.0f });
    tessyDagLevelCpu_.clear();
    tessyDagGroups_ = tessyDagClusters_ = 0;
    if (!megaWanted_ || numMeshGroups_ == 0) { return; }
    std::vector<std::string> without; // asset paths whose groups stay classic
    for (std::uint32_t g = 0; g < numMeshGroups_; ++g)
    {
        const Mesh* m = (g < groupMesh_.size()) ? groupMesh_[g] : nullptr;
        if (!m) { continue; }
        const bool isStatic = g < numStaticGroups_;
        if (isStatic && g < groupGbuf_.size() && groupGbuf_[g].glassOnly) { continue; } // draws nothing anyway
        if (isStatic) { ++tessyGroupsTotal_; }
        if (!m->HasClusters())
        {
            if (isStatic)
            {
                const auto it = meshPath.find(m);
                const std::string name = (it != meshPath.end() && !it->second.empty()) ? it->second : std::string("(unnamed mesh)");
                if (std::find(without.begin(), without.end(), name) == without.end()) { without.push_back(name); }
            }
            continue;
        }
        // The submesh ordinal of a static group: a mesh's groups are consecutive from its first one.
        std::uint32_t sOrd = 0;
        if (isStatic) { while (sOrd < g && groupMesh_[g - sOrd - 1u] == m) { ++sOrd; } }
        UINT prevLod = ~0u;
        DirectX::XMUINT2 prevRange{ 0u, 0u };
        for (std::uint32_t L = 0; L < render::kMaxShadowLods; ++L)
        {
            const UINT lod = isStatic ? m->ClampExplicitLod(L) : 0u;
            const size_t vg = static_cast<size_t>(g) * render::kMaxShadowLods + L;
            if (lod == prevLod) { tessyVgCpu_[vg] = prevRange; continue; }
            const size_t o = vg * 4u;
            const std::uint32_t lodBase = groupLodMega_[o + 0] - groupLodMega_[o + 1]; // this LOD's IB in the mega IB
            const Mesh::LodDrawInfo info = m->GetLodDrawInfo(lod);
            std::uint32_t lo = 0u, hi = info.indexCount;
            if (isStatic && info.submeshes && sOrd < info.submeshes->size())
            {
                lo = (*info.submeshes)[sOrd].indexOffset;
                hi = lo + (*info.submeshes)[sOrd].indexCount;
            }
            const std::uint32_t first = static_cast<std::uint32_t>(tessyClusterCpu_.size());
            const std::vector<Mesh::Cluster>& clusters = m->GetClusters(lod);
            const Mesh::ClusterLocalTables& local = m->GetClusterLocal(lod); // N6: parallel to `clusters`
            const bool hasLocal = local.clusters.size() == clusters.size();
            for (size_t ci = 0; ci < clusters.size(); ++ci)
            {
                const Mesh::Cluster& c = clusters[ci];
                if (c.firstIndex < lo || c.firstIndex >= hi) { continue; }
                TessyCluster t{};
                t.firstIndex = lodBase + c.firstIndex;
                t.triCount = c.triCount;
                t.windAlongMax = c.windAlongMax;
                t.windLeafMax = c.windLeafMax;
                t.center = c.boxCenter;
                t.extent = c.boxExtent;
                t.radius = c.sphere.w;
                if (hasLocal) // N6: its meshlets, their vertex lists and triangles copied per range (shared LODs share them)
                {
                    const Mesh::ClusterLocal& cl = local.clusters[ci];
                    t.firstMeshlet = static_cast<std::uint32_t>(tessyMeshletsCpu_.size());
                    t.meshletCount = cl.meshletCount;
                    for (std::uint32_t s = 0; s < cl.meshletCount; ++s)
                    {
                        const Mesh::ClusterMeshlet& m = local.meshlets[cl.firstMeshlet + s];
                        tessyMeshletsCpu_.push_back(TessyMeshlet{ static_cast<std::uint32_t>(tessyVertsCpu_.size()), m.vertCount,
                                                                  static_cast<std::uint32_t>(tessyTrisCpu_.size()), m.triCount });
                        tessyVertsCpu_.insert(tessyVertsCpu_.end(), local.verts.begin() + m.vertOffset,
                                              local.verts.begin() + m.vertOffset + m.vertCount);
                        tessyTrisCpu_.insert(tessyTrisCpu_.end(), local.tris.begin() + m.triOffset,
                                             local.tris.begin() + m.triOffset + m.triCount);
                    }
                }
                tessyClusterCpu_.push_back(t);
                tessyClusterLodCpu_.push_back(TessyClusterLod{}); // N7: a discrete LOD's cluster has no DAG record
            }
            prevLod = lod;
            prevRange = DirectX::XMUINT2{ first, static_cast<std::uint32_t>(tessyClusterCpu_.size()) - first };
            tessyVgCpu_[vg] = prevRange;
        }
        // N7: the group's submesh in the mesh's cluster DAG -- its clusters (mega-absolute, after the mesh's LODs in the
        // mega IB), their meshlets and LOD records, and its levels.
        const auto dagBase = tessyDagBase_.find(m);
        if (isStatic && m->HasDag() && dagBase != tessyDagBase_.end() && sOrd < m->GetDag().submeshes.size())
        {
            const Mesh::Dag& dag = m->GetDag();
            const Mesh::ClusterLocalTables& local = m->GetDagLocal();
            const bool hasLocal = local.clusters.size() == dag.clusters.size();
            const Mesh::DagSubmesh& ds = dag.submeshes[sOrd];
            const std::uint32_t first = static_cast<std::uint32_t>(tessyClusterCpu_.size());
            const std::uint32_t firstLevel = static_cast<std::uint32_t>(tessyDagLevelCpu_.size());
            for (std::uint32_t k = 0; k < ds.levelCount; ++k)
            {
                const Mesh::DagLevel& l = dag.levels[ds.firstLevel + k];
                std::uint32_t lo, hi;
                std::memcpy(&lo, &l.minLodError, 4);
                std::memcpy(&hi, &l.maxParentError, 4);
                tessyDagLevelCpu_.push_back(DirectX::XMUINT4{ first + (l.firstCluster - ds.firstCluster), l.clusterCount, lo, hi });
            }
            for (std::uint32_t ci = ds.firstCluster; ci < ds.firstCluster + ds.clusterCount; ++ci)
            {
                const Mesh::Cluster& c = dag.clusters[ci];
                TessyCluster t{};
                t.firstIndex = dagBase->second + c.firstIndex;
                t.triCount = c.triCount;
                t.windAlongMax = c.windAlongMax;
                t.windLeafMax = c.windLeafMax;
                t.center = c.boxCenter;
                t.extent = c.boxExtent;
                t.radius = c.sphere.w;
                if (hasLocal)
                {
                    const Mesh::ClusterLocal& cl = local.clusters[ci];
                    t.firstMeshlet = static_cast<std::uint32_t>(tessyMeshletsCpu_.size());
                    t.meshletCount = cl.meshletCount;
                    for (std::uint32_t s = 0; s < cl.meshletCount; ++s)
                    {
                        const Mesh::ClusterMeshlet& ml = local.meshlets[cl.firstMeshlet + s];
                        tessyMeshletsCpu_.push_back(TessyMeshlet{ static_cast<std::uint32_t>(tessyVertsCpu_.size()), ml.vertCount,
                                                                  static_cast<std::uint32_t>(tessyTrisCpu_.size()), ml.triCount });
                        tessyVertsCpu_.insert(tessyVertsCpu_.end(), local.verts.begin() + ml.vertOffset,
                                              local.verts.begin() + ml.vertOffset + ml.vertCount);
                        tessyTrisCpu_.insert(tessyTrisCpu_.end(), local.tris.begin() + ml.triOffset,
                                             local.tris.begin() + ml.triOffset + ml.triCount);
                    }
                }
                tessyClusterCpu_.push_back(t);
                static_assert(sizeof(TessyClusterLod) == sizeof(Mesh::ClusterLod), "the mirror of Mesh::ClusterLod");
                TessyClusterLod lod{};
                std::memcpy(&lod, &dag.lod[ci], sizeof(lod));
                tessyClusterLodCpu_.push_back(lod);
            }
            tessyDagGroupCpu_[g] = DirectX::XMUINT4{ first, ds.clusterCount, firstLevel, ds.levelCount };
            // N7e: a sphere around every lod and parent sphere of the submesh's DAG (the camera's level band reads the
            // distance range to it); a little over, so the band's test never cuts a cluster its own test would draw.
            {
                float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
                const auto grow = [&](const DirectX::XMFLOAT4& s)
                {
                    const float c[3] = { s.x, s.y, s.z };
                    for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], c[k] - s.w); hi[k] = std::max(hi[k], c[k] + s.w); }
                };
                for (std::uint32_t ci = ds.firstCluster; ci < ds.firstCluster + ds.clusterCount; ++ci)
                {
                    grow(dag.lod[ci].lodSphere);
                    grow(dag.lod[ci].parentSphere);
                }
                const float cx = 0.5f * (lo[0] + hi[0]), cy = 0.5f * (lo[1] + hi[1]), cz = 0.5f * (lo[2] + hi[2]);
                float r = 0.0f;
                const auto reach = [&](const DirectX::XMFLOAT4& s)
                {
                    const float dx = s.x - cx, dy = s.y - cy, dz = s.z - cz;
                    r = std::max(r, std::sqrt(dx * dx + dy * dy + dz * dz) + s.w);
                };
                for (std::uint32_t ci = ds.firstCluster; ci < ds.firstCluster + ds.clusterCount; ++ci)
                {
                    reach(dag.lod[ci].lodSphere);
                    reach(dag.lod[ci].parentSphere);
                }
                tessyDagSphereCpu_[g] = DirectX::XMFLOAT4{ cx, cy, cz, r * 1.001f + 1e-4f };
            }
            ++tessyDagGroups_;
            tessyDagClusters_ += ds.clusterCount;
        }
        if (isStatic) { ++tessyGroupsWith_; }
    }
    tessyBytes_ = tessyClusterCpu_.size() * sizeof(TessyCluster) + tessyVgCpu_.size() * sizeof(DirectX::XMUINT2) +
                  tessyMeshletsCpu_.size() * sizeof(TessyMeshlet) +
                  (tessyVertsCpu_.size() + tessyTrisCpu_.size()) * sizeof(std::uint32_t) +
                  tessyClusterLodCpu_.size() * sizeof(TessyClusterLod) +
                  (tessyDagGroupCpu_.size() + tessyDagLevelCpu_.size() + tessyDagSphereCpu_.size()) * sizeof(DirectX::XMUINT4);
    std::string list;
    for (size_t i = 0; i < without.size() && i < 24; ++i) { list += (i ? ", " : "") + without[i]; }
    if (without.size() > 24) { list += ", ... (" + std::to_string(without.size() - 24) + " more)"; }
    LOG_INFO(logging::LogCategory::RenderShadow,
             "tessy: clusters for {} of {} caster groups ({} clusters, {} KB); classic path: {}; cluster DAG for {} groups "
             "({} clusters)", tessyGroupsWith_, tessyGroupsTotal_, tessyClusterCpu_.size(), (tessyBytes_ + 1023u) / 1024u,
             without.empty() ? std::string("none") : list, tessyDagGroups_, tessyDagClusters_);
}

// Tessy N2: the tables into DEFAULT-heap buffers on the GPU-idle load list, beside the mega copy. They
// rest in COMMON like the mega buffers, so a shader read promotes them implicitly (buffers do) and the
// frame never transitions them.
void ShadowGpuData::UploadTessyTables_(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    tessyReady_ = false;
    if (!renderer || !cl || !megaReady_ || tessyClusterCpu_.empty() || tessyVgCpu_.empty()) { return; }
    ID3D12Device* dev = renderer->GetDevice();
    if (tessyVertsCpu_.empty()) { tessyVertsCpu_.push_back(0u); } // N6: never a zero-sized buffer / view
    if (tessyTrisCpu_.empty()) { tessyTrisCpu_.push_back(0u); }
    if (tessyMeshletsCpu_.empty()) { tessyMeshletsCpu_.push_back(TessyMeshlet{}); }
    if (tessyDagLevelCpu_.empty()) { tessyDagLevelCpu_.push_back(DirectX::XMUINT4{ 0u, 0u, 0u, 0u }); } // N7: never zero-sized
    if (tessyDagGroupCpu_.empty()) { tessyDagGroupCpu_.push_back(DirectX::XMUINT4{ 0u, 0u, 0u, 0u }); }
    if (tessyDagSphereCpu_.empty()) { tessyDagSphereCpu_.push_back(DirectX::XMFLOAT4{ 0.0f, 0.0f, 0.0f, 0.0f }); }
    struct Table { const void* src; UINT64 bytes; GpuResource* dst; const wchar_t* name; };
    const Table tables[] = {
        { tessyClusterCpu_.data(), tessyClusterCpu_.size() * sizeof(TessyCluster), &tessyClusters_, L"Tessy.Clusters" },
        { tessyVgCpu_.data(), tessyVgCpu_.size() * sizeof(DirectX::XMUINT2), &tessyVg_, L"Tessy.VirtualGroups" },
        { tessyVertsCpu_.data(), tessyVertsCpu_.size() * sizeof(std::uint32_t), &tessyVerts_, L"Tessy.ClusterVerts" },
        { tessyTrisCpu_.data(), tessyTrisCpu_.size() * sizeof(std::uint32_t), &tessyTris_, L"Tessy.ClusterTris" },
        { tessyMeshletsCpu_.data(), tessyMeshletsCpu_.size() * sizeof(TessyMeshlet), &tessyMeshlets_, L"Tessy.Meshlets" },
        { tessyClusterLodCpu_.data(), tessyClusterLodCpu_.size() * sizeof(TessyClusterLod), &tessyClusterLod_, L"Tessy.ClusterLod" },
        { tessyDagGroupCpu_.data(), tessyDagGroupCpu_.size() * sizeof(DirectX::XMUINT4), &tessyDagGroup_, L"Tessy.DagGroups" },
        { tessyDagLevelCpu_.data(), tessyDagLevelCpu_.size() * sizeof(DirectX::XMUINT4), &tessyDagLevel_, L"Tessy.DagLevels" },
        { tessyDagSphereCpu_.data(), tessyDagSphereCpu_.size() * sizeof(DirectX::XMFLOAT4), &tessyDagSphere_, L"Tessy.DagSpheres" },
    };
    auto makeBuf = [&](UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, const wchar_t* name,
                       Microsoft::WRL::ComPtr<ID3D12Resource>& out) -> bool
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(gpu::CreateCommittedResource(dev, &heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                      MemTag::ShadowTessy, name, IID_PPV_ARGS(out.GetAddressOf()))) && out;
    };
    for (const Table& t : tables) { t.dst->Reset(); } // GPU-idle here (EnsureMegaBuffer's contract)
    tessyUpload_.Reset();
    UINT64 totalBytes = 0;
    for (const Table& t : tables) { totalBytes += t.bytes; }
    Microsoft::WRL::ComPtr<ID3D12Resource> made[std::size(tables)];
    bool ok = makeBuf(totalBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"Tessy.Upload", tessyUpload_);
    for (size_t i = 0; ok && i < std::size(tables); ++i)
    {
        ok = makeBuf(tables[i].bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, tables[i].name, made[i]);
    }
    if (!ok)
    {
        LOG_ERROR(logging::LogCategory::RenderShadow, "tessy: the cluster tables could not be allocated; every group stays classic");
        tessyUpload_.Reset();
        return;
    }
    std::uint8_t* dst = nullptr;
    D3D12_RANGE none{ 0, 0 };
    if (FAILED(tessyUpload_->Map(0, &none, reinterpret_cast<void**>(&dst))) || !dst) { tessyUpload_.Reset(); return; }
    UINT64 at = 0;
    for (size_t i = 0; i < std::size(tables); ++i)
    {
        std::memcpy(dst + at, tables[i].src, static_cast<size_t>(tables[i].bytes));
        // COMMON -> COPY_DEST by implicit promotion, as the mega copy does.
        cl->CopyBufferRegion(made[i].Get(), 0, tessyUpload_.Get(), at, tables[i].bytes);
        tables[i].dst->Attach(renderer->Declarations(), std::move(made[i]), D3D12_RESOURCE_STATE_COMMON, tables[i].name);
        at += tables[i].bytes;
    }
    tessyUpload_->Unmap(0, nullptr);

    if (!tessyHeap_)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 11; // N7: + ClusterLod, DAG groups, DAG levels; N7e: + DAG spheres
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(gpu::CreateDescriptorHeap(dev, &hd, MemTag::ShadowTessy, L"Tessy.SrvHeap", IID_PPV_ARGS(tessyHeap_.GetAddressOf()))))
        {
            tessyHeap_.Reset();
            return;
        }
    }
    const UINT incr = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    tessyHeapIncr_ = incr;
    const D3D12_CPU_DESCRIPTOR_HANDLE base = tessyHeap_->GetCPUDescriptorHandleForHeapStart();
    const auto structured = [&](UINT slot, ID3D12Resource* res, UINT elems, UINT stride)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = elems;
        sd.Buffer.StructureByteStride = stride;
        dev->CreateShaderResourceView(res, &sd, D3D12_CPU_DESCRIPTOR_HANDLE{ base.ptr + slot * incr });
    };
    structured(0, tessyClusters_.Get(), static_cast<UINT>(tessyClusterCpu_.size()), sizeof(TessyCluster));
    structured(1, tessyVg_.Get(), static_cast<UINT>(tessyVgCpu_.size()), sizeof(DirectX::XMUINT2));
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = megaVBBytes_ / 4u;
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        dev->CreateShaderResourceView(megaVB_.Get(), &sd, D3D12_CPU_DESCRIPTOR_HANDLE{ base.ptr + 2u * incr });
    }
    structured(3, megaIB_.Get(), megaIBBytes_ / 4u, sizeof(std::uint32_t));
    structured(4, tessyVerts_.Get(), static_cast<UINT>(tessyVertsCpu_.size()), sizeof(std::uint32_t));
    structured(5, tessyTris_.Get(), static_cast<UINT>(tessyTrisCpu_.size()), sizeof(std::uint32_t));
    structured(6, tessyMeshlets_.Get(), static_cast<UINT>(tessyMeshletsCpu_.size()), sizeof(TessyMeshlet));
    structured(7, tessyClusterLod_.Get(), static_cast<UINT>(tessyClusterLodCpu_.size()), sizeof(TessyClusterLod));
    structured(8, tessyDagGroup_.Get(), static_cast<UINT>(tessyDagGroupCpu_.size()), sizeof(DirectX::XMUINT4));
    structured(9, tessyDagLevel_.Get(), static_cast<UINT>(tessyDagLevelCpu_.size()), sizeof(DirectX::XMUINT4));
    structured(10, tessyDagSphere_.Get(), static_cast<UINT>(tessyDagSphereCpu_.size()), sizeof(DirectX::XMFLOAT4));
    tessyReady_ = true;
    EnsureTessyAtlasResources(renderer); // N4: once, fixed sizes
}

// Tessy N4: the atlas views' cluster buffers -- fixed sizes, one region a frame each, created once (GPU-idle: the
// cluster tables' upload) and kept. Descriptors per frame: [0] records UAV, [1] view info UAV, [2] args RAW UAV,
// [3] counters RAW UAV, [4] records SRV, [5] view info SRV.
void ShadowGpuData::EnsureTessyAtlasResources(Renderer* renderer)
{
    if (!renderer || tessyAtlasHeap_) { return; }
    ID3D12Device* dev = renderer->GetDevice();
    const UINT64 recordBytes = static_cast<UINT64>(kTessyAtlasRecordCap) * 16u;
    const UINT64 infoBytes = static_cast<UINT64>(kTessyAtlasRows) * 8u;
    const UINT64 argsBytes = kTessyAtlasArgsRegionBytes;
    const UINT64 counterBytes = kTessyAtlasCounterBytes;
    const auto make = [&](UINT64 regionBytes, const wchar_t* name, GpuResource& out) -> bool
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = regionBytes * render::kFrameCount;
        desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Microsoft::WRL::ComPtr<ID3D12Resource> res;
        if (FAILED(gpu::CreateCommittedResource(dev, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                MemTag::ShadowTessy, name, IID_PPV_ARGS(res.GetAddressOf()))) || !res)
        {
            return false;
        }
        out.Attach(renderer->Declarations(), std::move(res), D3D12_RESOURCE_STATE_COMMON, name);
        return true;
    };
    const UINT64 deferBytes = static_cast<UINT64>(kTessyCamDeferCap) * 16u;
    if (!make(recordBytes, L"Tessy.AtlasVisible", tessyAtlasVisible_) || !make(infoBytes, L"Tessy.AtlasViewInfo", tessyAtlasInfo_) ||
        !make(argsBytes, L"Tessy.AtlasArgs", tessyAtlasArgs_) || !make(counterBytes, L"Tessy.AtlasCounters", tessyAtlasCounter_) ||
        !make(deferBytes, L"Tessy.CamDeferred", tessyCamDeferred_))
    {
        LOG_ERROR(logging::LogCategory::RenderShadow, "tessy: the atlas cluster buffers could not be allocated (tessy.atlas off)");
        tessyAtlasVisible_.Reset(); tessyAtlasInfo_.Reset(); tessyAtlasArgs_.Reset(); tessyAtlasCounter_.Reset(); tessyCamDeferred_.Reset();
        return;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = kTessyAtlasDescriptorsPerFrame * render::kFrameCount;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    if (FAILED(gpu::CreateDescriptorHeap(dev, &hd, MemTag::ShadowTessy, L"Tessy.AtlasHeap", IID_PPV_ARGS(tessyAtlasHeap_.GetAddressOf()))))
    {
        tessyAtlasHeap_.Reset();
        return;
    }
    tessyAtlasIncr_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        const auto uav = [&](UINT slot, ID3D12Resource* res, UINT64 regionBytes, UINT stride)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            if (stride == 0u)
            {
                ud.Format = DXGI_FORMAT_R32_TYPELESS;
                ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                ud.Buffer.FirstElement = f * (regionBytes / 4u);
                ud.Buffer.NumElements = static_cast<UINT>(regionBytes / 4u);
            }
            else
            {
                ud.Format = DXGI_FORMAT_UNKNOWN;
                ud.Buffer.StructureByteStride = stride;
                ud.Buffer.FirstElement = f * (regionBytes / stride);
                ud.Buffer.NumElements = static_cast<UINT>(regionBytes / stride);
            }
            dev->CreateUnorderedAccessView(res, nullptr, &ud, TessyAtlasDescriptor(f, slot));
        };
        const auto srv = [&](UINT slot, ID3D12Resource* res, UINT64 regionBytes, UINT stride)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_UNKNOWN;
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Buffer.StructureByteStride = stride;
            sd.Buffer.FirstElement = f * (regionBytes / stride);
            sd.Buffer.NumElements = static_cast<UINT>(regionBytes / stride);
            dev->CreateShaderResourceView(res, &sd, TessyAtlasDescriptor(f, slot));
        };
        uav(0u, tessyAtlasVisible_.Get(), recordBytes, 16u);
        uav(1u, tessyAtlasInfo_.Get(), infoBytes, 8u);
        uav(2u, tessyAtlasArgs_.Get(), argsBytes, 0u);
        uav(3u, tessyAtlasCounter_.Get(), counterBytes, 0u);
        srv(4u, tessyAtlasVisible_.Get(), recordBytes, 16u);
        srv(5u, tessyAtlasInfo_.Get(), infoBytes, 8u);
        uav(6u, tessyCamDeferred_.Get(), deferBytes, 16u);
        srv(7u, tessyCamDeferred_.Get(), deferBytes, 16u);
        {
            // tessy.sdsm: the SDSM rows' counters alone (Main_SdsmCull clears them; a RAW view starts on 16 bytes).
            static_assert(kTessySunCounterOffset % 16u == 0u && kTessyAtlasCounterBytes % 16u == 0u &&
                          kTessySunCounterOffset + kTessySunCounterBytes <= kTessyRectCulledOffset, "RAW views start on 16 bytes");
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            ud.Buffer.FirstElement = (f * counterBytes + kTessySunCounterOffset) / 4u;
            ud.Buffer.NumElements = kTessySunCounterBytes / 4u;
            dev->CreateUnorderedAccessView(tessyAtlasCounter_.Get(), nullptr, &ud, TessyAtlasDescriptor(f, 8u));
        }
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::TessyAtlasDescriptor(UINT frame, UINT slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = tessyAtlasHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(frame * kTessyAtlasDescriptorsPerFrame + slot) * tessyAtlasIncr_;
    return h;
}

// The cluster draws' table (TESSY_ATLAS_MS, TESSY_VIEW), staged once a frame for every view under drawTablesMutex_ (the
// views record on workers in parallel): Instances (+ CasterGroup, GroupMask), the records, the mega VB, the meshlet
// tables, the runs (+ the masked albedos on the table path).
D3D12_GPU_DESCRIPTOR_HANDLE ShadowGpuData::TessyAtlasDrawTable(Renderer* renderer, UINT f)
{
    std::lock_guard<std::mutex> lock(drawTablesMutex_);
    const std::uint64_t frame = renderer->GetTotalFrameNumber();
    if (tessyAtlasTableFrame_ != frame)
    {
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 25> tbl{};
        size_t n = 0;
        tbl[n++] = InstanceReadSrv(f);
        if (MaskedShadowsActive()) { tbl[n++] = CasterGroupSrv(); tbl[n++] = GroupMaskSrv(); }
        tbl[n++] = TessyAtlasDescriptor(f, 4u);
        tbl[n++] = MegaVertexRawSrv();
        tbl[n++] = TessyMeshletSrv();
        tbl[n++] = TessyClusterVertsSrv();
        tbl[n++] = TessyClusterTrisSrv();
        tbl[n++] = TessyAtlasDescriptor(f, 5u);
        if (MaskedShadowsActive() && !MaskedShadowsBindless())
        {
            for (std::uint32_t a = 0; a < maskedAlbedoCount_ && n < tbl.size(); ++a) { tbl[n++] = maskedAlbedoSrvs_[a]; }
        }
        tessyAtlasTable_ = renderer->StageSrvUavTable(tbl, n).gpu;
        tessyAtlasTableFrame_ = frame;
    }
    return tessyAtlasTable_;
}

// Tessy's cluster view: the camera row's records (row set 2), one colour per cluster or meshlet, into the scene colour
// and depth Main_DebugDraw bound (reversed-Z, read only). b0's bit 31 picks meshlets.
bool ShadowGpuData::RecordTessyView(Renderer* renderer, ID3D12GraphicsCommandList* cl, D3D12_GPU_VIRTUAL_ADDRESS viewCB)
{
    if (!tessyViewFrame_ || !renderer || !cl || viewCB == 0) { return false; }
    const UINT f = renderer->GetCurrentFrameIndex();
    Material* const m = MaskedShadowsActive() ? tessyViewMaskedMat_.get() : tessyViewMat_.get();
    ID3D12CommandSignature* const meshSig = renderer->GetDispatchMeshCommandSignature();
    if (f >= render::kFrameCount || !m || !meshSig) { return false; }
    const D3D12_GPU_DESCRIPTOR_HANDLE table = TessyAtlasDrawTable(renderer, f);
    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    RenderContext& ctx = ctxHandle.ref();
    ctx.cbv[1] = viewCB;
    ctx.srvTable[0] = table;
    // Under N5 the camera row set holds a row per G-buffer PSO run (what the clusters drew); alone, the one row.
    const std::uint32_t rows = tessyGbufFrame_ ? tessyGbufRows_ : 1u;
    for (std::uint32_t r = 0; r < rows; ++r)
    {
        const std::uint32_t row = kTessyCameraRowSet * render::kShadowViewCeiling + r;
        ctx.constants[0] = { row | (tessyViewMode_ == 2u ? 0x80000000u : tessyViewMode_ == 3u ? 0x40000000u : 0u), 0u, 0u, 0u };
        m->Bind(cl, ctx, false);
        renderer->ExecuteIndirect(cl, meshSig, 1, tessyAtlasArgs_.Get(),
            static_cast<UINT64>(f) * kTessyAtlasArgsRegionBytes + static_cast<UINT64>(row) * 12u, nullptr, 0);
    }
    return true;
}

// Tessy N4: the cluster cull of one row set -- 0 the atlas views' main rows, 1 S7's static rows (both after the shadow
// cull's adds have landed and before the compaction reads the rows it zeroes), 2 the camera's row: for N5's G-buffer
// culled by the camera frustum and taken from the classic draw, one group per PSO run (kFlagByRun); for the cluster
// view alone no cull and nothing zeroed (the G-buffer still draws those rows). The camera's records carry their group.
void ShadowGpuData::RecordTessyAtlasCull(Renderer* renderer, ID3D12GraphicsCommandList* cl, const CullDecisions& dec,
                                         std::uint32_t rowSet, D3D12_CPU_DESCRIPTOR_HANDLE frustumSrv, bool empty,
                                         D3D12_CPU_DESCRIPTOR_HANDLE partitionSrv, float sunTexels)
{
    const UINT f = renderer->GetCurrentFrameIndex();
    const bool camera = rowSet == kTessyCameraRowSet;
    const bool sun = rowSet == kTessySunRowSet; // tessy.sdsm: the SDSM partitions, rows 0.. of the main pair
    const bool byRun = camera && dec.tessyGBuffer;
    const std::uint32_t views = camera ? (byRun ? dec.tessyCamRows : 1u) : dec.localViews;
    if (f >= render::kFrameCount || views == 0u || rowSet >= kTessyAtlasRowSets || (sun && frustumSrv.ptr == 0)) { return; }
    struct AtlasCullCB
    {
        std::uint32_t numGroups, firstView, numViews, rowSet, viewStride, frustumFirst, recordBase, recordCap;
        float wind0[4], wind1[4];
        std::uint32_t windPad, flags, classicCountOffset, camDeferCap;
        float dagCam[4];      // N7d: the camera, pixels per unit / lod.pixelError (the camera's runs)
        std::uint32_t dagOn;
        float dagNear;
        std::uint32_t dagBand; // N7e: tessy.lodBand
        std::uint32_t planes;  // a local view's frustum planes (6), an SDSM partition's (16)
        std::uint32_t counterAt[4]; // the row set's counters in TessyAtlasCounter: claim, not fit, tested, triangles (~0 none)
        std::uint32_t split;   // groups a view (the SDSM partitions: kTessySunSplit, each its own run), else 1
        float rectSize[2];     // kFlagRectCull on the SDSM partitions: their viewport (texels)
        std::uint32_t rectCountAt; // where in TessyAtlasCounter the clusters it dropped are counted (bytes)
        ViewMask staticViews; // S7's rows: the views that draw them (SetLocalStaticDrawn); the other row sets ignore it
    };
    static_assert(offsetof(AtlasCullCB, staticViews) == 144u, "tessy_atlas_cull_cs.hlsl AtlasCullCB: gStaticViews at 144");
    static constexpr std::uint32_t kFlagKeepRows = 1u, kFlagNoCull = 2u, kFlagClusterId = 4u, kFlagByRun = 8u, // the kernel's
                                   kFlagLevelId = 16u, kFlagOrdered = 32u, kFlagCameraCut = 64u, kFlagRectCull = 128u;
    AtlasCullCB c{};
    c.numGroups = empty ? 0u : numVirtualGroups_;
    c.firstView = rowSet == 0u ? render::kShadowViewCascades : 0u; // the main rows by view slot, S7's by slot - cascades
    c.numViews = views;
    c.rowSet = rowSet;
    c.viewStride = render::kShadowViewCeiling;
    // The camera's frustum sits past the capacity's views (shadow_cull_cs gCamView); the cluster view alone reads none;
    // the SDSM partitions' planes are their own buffer's 0..
    c.frustumFirst = camera ? (byRun ? render::MaxShadowViews() : 0u) : sun ? 0u : render::kShadowViewCascades;
    c.planes = sun ? 16u : 6u;
    c.split = sun ? kTessySunSplit : 1u;
    if (sun)
    {
        c.counterAt[0] = kTessySunCounterOffset; c.counterAt[1] = kTessySunCounterOffset + 4u;
        c.counterAt[2] = kTessySunCounterOffset + 8u; c.counterAt[3] = kTessySunCounterOffset + 12u;
    }
    else
    {
        c.counterAt[0] = rowSet * 4u; c.counterAt[1] = 12u; c.counterAt[2] = 16u; c.counterAt[3] = byRun ? 24u : 0xFFFFFFFFu;
    }
    c.recordBase = rowSet * kTessyAtlasRowSetCap;
    c.recordCap = kTessyAtlasRowSetCap;
    for (int i = 0; i < 4; ++i) { c.wind0[i] = dec.tessyWind0[i]; c.wind1[i] = dec.tessyWind1[i]; }
    c.windPad = std::min(tessy::g_vsmWindPad, 2u);
    c.classicCountOffset = static_cast<std::uint32_t>(kTessyCamClassicCountOffset); // by run: the classic runs' counts
    c.flags = !camera ? 0u : byRun ? (kFlagClusterId | kFlagByRun) : (kFlagKeepRows | kFlagNoCull | kFlagClusterId);
    if (camera && tessyViewFrame_ && tessyViewMode_ == 3u) { c.flags |= kFlagLevelId; } // the cluster view's "Levels"
    // The camera's records in the classic draw's order (tessy_atlas_cull_cs Sweep: coplanar surfaces of two meshes must
    // resolve their depth tie the same way every frame); a shadow row writes depth alone and keeps the atomics.
    if (camera && tessy::g_ordered) { c.flags |= kFlagOrdered; }
    // The SDSM partitions cut the DAG exactly as the camera did (dagOn from the camera's frame decision, gDagCam its eye).
    if (sun && dec.tessyAtlasDag) { c.flags |= kFlagCameraCut; }
    // tessy.clusterCull: a cluster that holds no sample of its view goes -- the camera's runs on the render target (b1's
    // jittered matrix, SceneRenderer fills it every frame), the lamps by ViewRects, the partitions by their matrices. Not
    // the cluster view (it culls nothing).
    const bool rectCull = tessy::g_clusterCull && (sun ? (partitionSrv.ptr != 0 && sunTexels > 0.0f)
                                                       : camera ? (byRun && camHzbParams_.viewRect[2] > 0)
                                                                : viewRect_.Valid());
    if (rectCull) { c.flags |= kFlagRectCull; }
    c.rectSize[0] = c.rectSize[1] = sun ? sunTexels : 0.0f;
    c.rectCountAt = sun ? kTessySunCounterOffset + 16u : kTessyRectCulledOffset;
    c.staticViews = dec.tessyStaticViews;
    c.dagOn = byRun ? (dec.tessyCamDag ? 1u : 0u) : (!camera && dec.tessyAtlasDag ? 1u : 0u);
    for (int i = 0; i < 4; ++i) { c.dagCam[i] = dec.tessyDagCam[i]; }
    c.dagNear = 0.01f; // 1 cm: a sphere around the camera takes its leaves
    c.dagBand = tessy::g_lodBand ? 1u : 0u;
    // The rows (RAW) and their caster ids: main, static, camera, the SDSM partitions (the main pair's rows 0..3).
    static constexpr std::size_t kArgsSet[kTessyAtlasRowSets] = { 0u, 16u, 7u, 0u };
    static constexpr std::size_t kListSet[kTessyAtlasRowSets] = { 1u, 17u, 8u, 1u };
    // N5: the camera's runs test their clusters against last frame's pyramid too, when the two-pass HZB is on.
    const bool camHzb = byRun && dec.tessyCamHzb;
    c.camDeferCap = camHzb ? kTessyCamDeferCap : 0u;
    CameraHzbParams hzb = camHzbParams_;
    hzb.on = camHzb ? 1u : 0u;
    // The camera's runs are a handful of groups with thousands of tests each: the 1024-thread twin. (The SDSM partitions
    // are split instead -- 16 groups a partition, each a slice of its virtual groups.)
    Material* const kernel = (camera && tessyCameraCullMat_) ? tessyCameraCullMat_.get() : tessyAtlasCullMat_.get();
    auto cb0 = renderer->GetFrameResource()->AllocDynamic(static_cast<UINT>(sizeof(AtlasCullCB)), render::kConstantBufferAlignment);
    auto cb1 = renderer->GetFrameResource()->AllocDynamic(static_cast<UINT>(sizeof(CameraHzbParams)), render::kConstantBufferAlignment);
    if (!cb0.cpu || !cb1.cpu || !kernel) { return; }
    std::memcpy(cb0.cpu, &c, sizeof(c));
    std::memcpy(cb1.cpu, &hzb, sizeof(hzb));
    auto h = renderer->GetRenderContextPool()->Acquire();
    RenderContext& rc = h.ref();
    rc.cbv[0] = cb0.gpu;
    rc.cbv[1] = cb1.gpu;
    // A volatile table has no holes: the placeholders the cull uses where a frame has no pyramid / no run table.
    const D3D12_CPU_DESCRIPTOR_HANDLE prevHzb = (camHzb && camHzbPrevSrv_.ptr != 0) ? camHzbPrevSrv_ : renderer->GetDeferredForFrame().shadowSRV;
    rc.srvTable[0] = renderer->StageSrvUavTable({ InstanceReadSrv(f), sun ? frustumSrv : viewFrustums_.Srv(f), TessyClusterSrv(), TessyVgSrv(), GroupLodMegaSrv(),
                                                  tessyVgRun_.Valid() ? tessyVgRun_.Srv(f) : TessyVgSrv(), // read by the camera's runs alone
                                                  prevHzb, TessyClusterLodSrv(), TessyDagGroupSrv(), CasterLodSrv(f),
                                                  TessyDagLevelSrv(), TessyDagSphereSrv(),
                                                  viewDag_.Valid() ? viewDag_.Srv(f) : TessyDagSphereSrv(), // N7e: t12
                                                  // t13: the runs' command order, read by the camera's runs alone
                                                  tessyRunCmds_.Valid() ? tessyRunCmds_.Srv(f) : TessyVgSrv(),
                                                  // t14 / t15: tessy.clusterCull's matrices -- the partitions (the sun's
                                                  // row set), the lamps' views (placeholders where unread)
                                                  partitionSrv.ptr != 0 ? partitionSrv : TessyVgSrv(),
                                                  viewRect_.Valid() ? viewRect_.Srv(f) : TessyVgSrv() }).gpu;
    rc.uavTable[0] = renderer->StageSrvUavTable({ cullUav_[kArgsSet[rowSet] * render::kFrameCount + f], cullUav_[kListSet[rowSet] * render::kFrameCount + f],
                                                  TessyAtlasDescriptor(f, 0u), TessyAtlasDescriptor(f, 1u), TessyAtlasDescriptor(f, 2u),
                                                  TessyAtlasDescriptor(f, 3u), TessyAtlasDescriptor(f, 6u) }).gpu;
    rc.samplerTable[0] = D3D12_GPU_DESCRIPTOR_HANDLE{};
    kernel->Bind(cl, rc);
    cl->Dispatch(views * c.split, 1, 1); // one group per view (the camera: per run; the sun: kTessySunSplit per partition)
}

D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::TessySrv(UINT slot) const
{
    if (!tessyReady_ || !tessyHeap_ || !megaReady_) { return {}; }
    D3D12_CPU_DESCRIPTOR_HANDLE h = tessyHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * tessyHeapIncr_;
    return h;
}

namespace
{
// The positive-vertex test over planes [first, first + count) -- shadow_cull_cs.hlsl's PlanesPass.
bool CpuPlanesPass(const render::ShadowViewFrustum& vf, std::uint32_t first, std::uint32_t count, const render::CasterBounds& b)
{
    for (std::uint32_t i = first; i < first + count; ++i)
    {
        const DirectX::XMFLOAT4& p = vf.planes[i];
        const float signedDist = p.x * b.center.x + p.y * b.center.y + p.z * b.center.z + p.w;
        const float projRadius = std::abs(p.x) * b.halfExtents.x + std::abs(p.y) * b.halfExtents.y + std::abs(p.z) * b.halfExtents.z;
        if (signedDist + projRadius < 0.0f) { return false; }
    }
    return true;
}

// Plan S9e: shadow_cull_cs.hlsl's LightBoundsPass -- the box against the light's sphere, its bounding sphere against a
// spot's cone (UE USpotLightComponent::AffectsBounds).
bool CpuLightBoundsPass(const DirectX::XMFLOAT4& sphere, const DirectX::XMFLOAT4& cone, const render::CasterBounds& b)
{
    if (sphere.w >= 0.0f)
    {
        const float dx = std::max(std::abs(b.center.x - sphere.x) - b.halfExtents.x, 0.0f);
        const float dy = std::max(std::abs(b.center.y - sphere.y) - b.halfExtents.y, 0.0f);
        const float dz = std::max(std::abs(b.center.z - sphere.z) - b.halfExtents.z, 0.0f);
        if (dx * dx + dy * dy + dz * dz > sphere.w * sphere.w) { return false; }
    }
    if (cone.w <= 1.0f)
    {
        const float r = b.center.w;
        const float cosA = cone.w;
        const float sinA = std::sqrt(std::clamp(1.0f - cosA * cosA, 0.0f, 1.0f));
        const float pull = r / std::max(sinA, 1e-4f);
        float dx = b.center.x - (sphere.x - pull * cone.x);
        float dy = b.center.y - (sphere.y - pull * cone.y);
        float dz = b.center.z - (sphere.z - pull * cone.z);
        float dsqr = dx * dx + dy * dy + dz * dz;
        float e = cone.x * dx + cone.y * dy + cone.z * dz;
        if (!(e > 0.0f && e * e >= dsqr * (cosA * cosA))) { return false; }
        dx = b.center.x - sphere.x;
        dy = b.center.y - sphere.y;
        dz = b.center.z - sphere.z;
        dsqr = dx * dx + dy * dy + dz * dz;
        e = -(cone.x * dx + cone.y * dy + cone.z * dz);
        if (e > 0.0f && e * e >= dsqr * (sinA * sinA)) { return dsqr <= r * r; }
    }
    return true;
}
} // namespace

void ShadowGpuData::PollValidation(Renderer* renderer)
{
    if (valState_ != 1 || !renderer || !valReadback_) { return; }
    // Wait until the fence for the readback frame has surely passed (BeginFrame waits on the
    // slot's fence kFrameCount frames later), so the copy has completed on the GPU.
    if (renderer->GetTotalFrameNumber() < valFrame_ + render::kFrameCount) { return; }

    const size_t argBytes = static_cast<size_t>(valViews_) * valGroups_ * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    const size_t camBytes = valCamOn_ ? static_cast<size_t>(valCamGroups_) * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : 0u;
    D3D12_RANGE readRange{ 0, indirectArgs_.regionBytes + (valCamHzbOn_ ? camArgs_.regionBytes + kHzbStatsBytes : camBytes) };
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
    // S4: the camera row sits behind the shadow rows (its own region, copied at that offset).
    std::uint32_t gpuCam = 0;
    if (valCamOn_)
    {
        const auto* camArgs = reinterpret_cast<const D3D12_DRAW_INDEXED_ARGUMENTS*>(
            static_cast<const std::uint8_t*>(mapped) + indirectArgs_.regionBytes);
        for (std::uint32_t g = 0; g < valCamGroups_; ++g) { gpuCam += camArgs[g].InstanceCount; }
    }
    // S5: the entries the main cull deferred instead of emitting, with pass A's weight -- the
    // camera row is EXACTLY those short of the frustum reference (the post pass's verdicts are
    // not part of the check: they depend on this frame's depth, which the CPU cannot mirror).
    std::uint32_t gpuCamDeferred = 0;
    if (valCamHzbOn_)
    {
        std::memcpy(valCounters_.data(), static_cast<const std::uint8_t*>(mapped) + indirectArgs_.regionBytes + camArgs_.regionBytes, kHzbStatsBytes);
        gpuCamDeferred = valCounters_[9];
    }
    const D3D12_RANGE noWrite{ 0, 0 };
    valReadback_->Unmap(0, &noWrite);
    (void)argBytes;

    // CPU per-view totals from the snapshot (same positive-vertex test as shadow_cull_cs.hlsl).
    std::uint32_t mismatchViews = 0, firstView = 0, firstCpu = 0, firstGpu = 0;
    std::uint64_t localStages[3] = {}, localDrawn = 0; // S9e: local caster draws after the frustum, bounds, hull; final
    std::string mismatchList;
    for (std::uint32_t v = 0; v < valViews_; ++v)
    {
        const render::ShadowViewFrustum& vf = valFrustums_[v];
        const bool localView = render::IsLocalShadowView(v);
        std::uint32_t cpu = 0;
        for (std::uint32_t c = 0; c < valCasters_; ++c)
        {
            const render::CasterBounds& b = valBounds_[c];
            if (b.halfExtents.w != 0.0f) { continue; } // Cast Shadow off: the camera's only (FillBounds)
            bool visible;
            if (localView)
            {
                // S9e: the validation frame never splits, so every caster takes the hull and the threshold too. The
                // stages are counted for the verdict line (what each test removed at this view).
                const float dx = b.center.x - valLocalCamPos_.x, dy = b.center.y - valLocalCamPos_.y, dz = b.center.z - valLocalCamPos_.z;
                const bool tiny = valLocalRadiusSq_ > 0.0f && c < valGiFirst_ &&
                                  b.center.w * b.center.w <= valLocalRadiusSq_ * (dx * dx + dy * dy + dz * dz);
                visible = CpuPlanesPass(vf, 0u, render::kLocalViewFrustumPlanes, b);
                if (visible) { ++localStages[0]; }
                visible = visible &&
                          CpuLightBoundsPass(vf.planes[render::kLocalViewSphereSlot], vf.planes[render::kLocalViewConeSlot], b);
                if (visible) { ++localStages[1]; }
                visible = visible && CpuPlanesPass(vf, render::kLocalViewHullFirst, render::kLocalViewHullPlanes, b);
                if (visible) { ++localStages[2]; }
                visible = visible && !tiny;
                if (visible) { ++localDrawn; }
            }
            else
            {
                visible = CpuPlanesPass(vf, 0u, render::kShadowViewPlanes, b);
            }
            if (visible) { ++cpu; }
        }
        if (cpu != gpuTotal[v])
        {
            if (mismatchViews == 0) { firstView = v; firstCpu = cpu; firstGpu = gpuTotal[v]; }
            // Which views, with both counts: cascades-only with gpu=0 reads as an ordering problem
            // (copy before dispatch); gpu>0 on a sentinel (reject-all) slot reads as the GPU seeing
            // different plane bytes than the CPU mirror. The verdict line alone cannot tell them apart.
            if (mismatchViews < 16)
            {
                char one[48];
                std::snprintf(one, sizeof(one), " v%u:cpu=%u/gpu=%u", v, cpu, gpuTotal[v]);
                mismatchList += one;
            }
            ++mismatchViews;
        }
    }

    // S4: the camera row -- the same frustum test with the camera's planes (the slot past the
    // shadow views), gated by this frame's eligibility + skip flags, a fading caster twice: the
    // CPU twin of the camera branch in shadow_cull_cs.hlsl.
    std::uint32_t cpuCam = 0;
    bool camMismatch = false;
    if (valCamOn_ && valFrustums_.size() > render::MaxShadowViews())
    {
        const render::ShadowViewFrustum& vf = valFrustums_[render::MaxShadowViews()];
        for (std::uint32_t c = 0; c < valCasters_ && c < valCam_.size(); ++c)
        {
            if ((valCam_[c] & 3u) != 1u) { continue; }
            const render::CasterBounds& b = valBounds_[c];
            bool visible = true;
            for (int i = 0; i < static_cast<int>(render::kShadowViewPlanes); ++i)
            {
                const DirectX::XMFLOAT4& p = vf.planes[i];
                const float signedDist = p.x * b.center.x + p.y * b.center.y + p.z * b.center.z + p.w;
                const float projRadius = std::abs(p.x) * b.halfExtents.x + std::abs(p.y) * b.halfExtents.y + std::abs(p.z) * b.halfExtents.z;
                if (signedDist + projRadius < 0.0f) { visible = false; break; }
            }
            if (!visible) { continue; }
            ++cpuCam;
            if (c < valFade_.size() && valFade_[c] > 0.0f) { ++cpuCam; }
        }
        camMismatch = cpuCam != gpuCam + gpuCamDeferred;
    }

    char buf[512];
    if (mismatchViews == 0 && !camMismatch)
    {
        if (valCamOn_)
        {
            std::snprintf(buf, sizeof(buf),
                "[ShadowGpuData] cull validation PASS: %u views + camera match CPU (%u casters, %u groups; camera %u + %u deferred).\n",
                valViews_, valCasters_, valGroups_, gpuCam, gpuCamDeferred);
        }
        else
        {
            std::snprintf(buf, sizeof(buf),
                "[ShadowGpuData] cull validation PASS: %u views match CPU (%u casters, %u groups).\n",
                valViews_, valCasters_, valGroups_);
        }
    }
    else
    {
        std::snprintf(buf, sizeof(buf),
            "[ShadowGpuData] cull validation MISMATCH: %u/%u views differ (first view %u: cpu=%u gpu=%u).%s%s cam cpu=%u/gpu=%u+%u deferred\n",
            mismatchViews, valViews_, firstView, firstCpu, firstGpu, mismatchList.c_str(),
            camMismatch ? " CAMERA row differs:" : "", cpuCam, gpuCam, gpuCamDeferred);
        mismatchViews = std::max(mismatchViews, camMismatch ? 1u : 0u);
    }
    // The verdict this validation exists to produce, readable (session log) by the gate runs that
    // trust it.
    logging::WriteRaw(mismatchViews == 0 ? logging::LogLevel::Info : logging::LogLevel::Warning,
                      logging::LogCategory::RenderShadow, buf);
    // S9e: what UE's caster culling removed from the local views at this view (the CPU reference's own stages).
    if (localStages[0] > 0)
    {
        LOG_INFO(logging::LogCategory::RenderShadow,
                 "local shadow caster cull: {} draws inside the views' frusta, {} touch their light, {} in the light + "
                 "camera hull, {} above the radius threshold",
                 localStages[0], localStages[1], localStages[2], localDrawn);
    }
    valState_ = 2;
}

void ShadowGpuData::Reset()
{
    ++drawTablesGeneration_;
    // Retain the GPU buffers + SRV descriptors across a level unload (a pass may reference an
    // SRV while frames are in flight). Only CPU-side state is dropped; the next Rebuild reuses
    // the allocations when their capacity still suffices.
    count_ = 0;
    staticCount_ = 0;
    viewFrustumCount_ = 0;
    numMeshGroups_ = 0;
    numVirtualGroups_ = 0;
    cpuInstances_.clear();
    cpuBounds_.clear();
    pending_.clear();
    cpuViewFrustums_.clear();
    groupMesh_.clear();
    giCasters_.clear();            // GI reservation dropped; next Rebuild re-enumerates + refreshes obj*
    giFoldableInstances_ = 0;
    megaCopy_.clear();             // mesh pointers dangle across a level unload
    tessyDagBase_.clear();         // N7: so do these
    groupMaskCpu_.clear();         // A6: same contract as the SRV handles below (MaterialData-owned)
    groupMaskMd_.clear();
    maskedGroupCount_ = 0;
    groupMaskRegion_ = 0;
    cmdOrder_.clear();             // A6: the merged commands point at materials the level owned
    cmdGeom_.clear();
    cmdRuns_.clear();
    cmdFrameReady_ = false;
    cmdMerge_ = { false, false };
    cmdCountFrame_ = 0;
    maskedAlbedoSrvs_.fill({});    // C2: MaterialData-owned SRV handles dangle across a level unload
    maskedAlbedoCount_ = 0;
    hasMaskedGroups_ = false;
    forceContentRefresh_ = false;
    megaWanted_ = megaReady_ = false; // groupMesh_ gone; next Rebuild frees + rebuilds the mega buffers
    tessyReady_ = false;              // Tessy N2: the tables go with them (the GPU copies stay until replaced)
    tessyClusterCpu_.clear();
    tessyVgCpu_.clear();
    tessyMeshletsCpu_.clear();
    tessyVertsCpu_.clear();
    tessyTrisCpu_.clear();
    tessyGroupsWith_ = tessyGroupsTotal_ = 0;
    valState_ = 0;
    logFramesRemaining_ = 5;
    // S5b: the pyramids hold the OLD level's tiles; the next frames' culls treat every caster as
    // visible until a fresh build lands (two-pass makes even a stale pyramid safe, but there is no
    // reason to test against it).
    cascadeHzb_.Invalidate();
    hzbCullThisFrame_ = false;
    hzbStats_.fill(0);
    // S4: the group materials point into the objects being dropped.
    groupGbuf_.clear();
    casterEligible_.clear();
    casterCamCpu_.clear();
    casterFadeCpu_.clear();
    vgCamCount_.clear();
    eligibleCasterCount_ = 0;
    camListCount_ = 0;
    gbufferIndirectFrame_ = false;
    valCamOn_ = false;
    // S5: the pyramids are the renderer's and survive; the decision does not.
    camHzbThisFrame_ = false;
    valCamHzbOn_ = false;
}
