#include "rendering/shadows/ShadowGpuData.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/diagnostics/DiagPaths.h" // shadow_casters.log: the group count a headless run cannot print
#include "core/math/Frustum.h"
#include "materials/MaterialData.h" // C2: per-slot alphaMask/alphaCutoff/albedo for masked shadows
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderGraph.h" // PrepareCullPass takes a RenderGraphPassContext&
#include "rendering/core/ComputeDispatch.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/meshes/LodSelect.h" // render::g_shadowLodBias + ShadowTierBaseLod (per-view LOD)
#include "rendering/shadows/VirtualShadowMap.h" // vsm::kNumCascades / kNumClipmapLevels (view tiers)
#include "rendering/lighting/LightManager.h"    // kMaxShadowedSpotLights / kMaxShadowedPointLights
#include "rendering/renderables/GBufferRenderable.h" // C2: per-slot MaterialData accessor
#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/IInstanceable.h"
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
    HRESULT hr = renderer->GetDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
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
    // Match SceneRenderQueue's visibility and CastsShadow predicates. A visibility change
    // changes the count seen by UpdateForFrame, which rebuilds this cache before VSM or
    // indirect draws consume it. In no-editor builds ordinary level objects remain visible.
    // GPU-instanced casters are handled separately by IsGiFoldable below.
    if (!obj || !obj->IsVisible() || !obj->CastsShadow() || obj->IsGpuInstancedCaster()) { return false; }
    const RenderableObject* ro = obj->AsRenderableObject();
    return ro && ro->GetMesh() != nullptr;
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
    out.halfExtents = DirectX::XMFLOAT4(e.x, e.y, e.z, 0.0f);
}

// `[ShadowGpuData]` lines also go to a file: they are OutputDebugStringA-only otherwise, i.e.
// invisible to exactly the headless runs that gate caster-set work. Truncates on the first write of
// a process and appends after, so one run's rebuilds stay together without the file growing forever.
static void LogCasterLine(const char* line)
{
    static bool started = false;
    FILE* f = nullptr;
    if (fopen_s(&f, diag::LogPath("shadow_casters.log").c_str(), started ? "a" : "w") != 0 || !f)
    {
        return; // a diagnostic must never be the reason a run fails
    }
    started = true;
    std::fputs(line, f);
    std::fclose(f);
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
    out.halfExtents = DirectX::XMFLOAT4(e.x, e.y, e.z, 0.0f);
}

// ---- Public API ------------------------------------------------------------

D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::InstanceSrv(UINT frameIndex) const { return instances_.Srv(frameIndex); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::BoundsSrv(UINT frameIndex) const { return bounds_.Srv(frameIndex); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::ViewFrustumSrv(UINT frameIndex) const { return viewFrustums_.Srv(frameIndex); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::CasterGroupSrv() const { return casterGroup_.Srv(0); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::CasterMetaSrv() const { return casterMeta_.Srv(0); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::PerGroupSrv() const { return perGroup_.Srv(0); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::GroupLodMegaSrv() const { return groupLodMegaBuf_.Srv(0); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::GroupLodOverrideSrv(UINT f) const { return groupLodOverrideBuf_.Srv(f); }
D3D12_CPU_DESCRIPTOR_HANDLE ShadowGpuData::PerViewGroupSrv() const { return perViewGroup_.Srv(0); }

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
    const Math::float3& cameraPos)
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
    // Change detection for the page cache: any LOD move invalidates cached pages (they hold
    // the old geometry). Rebuild resizes the table, which reads as a change — correct, a new
    // caster set must flush too.
    if (casterLodPrev_ != casterLod_)
    {
        casterLodsChanged_ = true;
        casterLodPrev_ = casterLod_;
    }
    UploadCasterLods(renderer);
}

// The override table's home is a PER-FRAME ring region: it is rewritten every frame, so writing it
// into region f is the same WAR discipline the instance/bounds rings use. Sized by numMeshGroups_,
// which is what took the group cap off this table.
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
        std::memcpy(dst, casterLod_.data(), casterLod_.size() * sizeof(std::uint32_t));
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
    if (!cullClearMat_ || !cullMat_) { return dec; }
    if (count_ == 0 || numMeshGroups_ == 0) { return dec; }
    if (!indirectArgs_.Valid() || !visibleList_.Valid() || !indirectCounts_.Valid()) { return dec; }
    if (!bounds_.Valid() || !viewFrustums_.Valid() || !casterGroup_.Valid() || !perGroup_.Valid() ||
        !perViewGroup_.Valid()) { return dec; }
    if (!cullUavHeap_) { return dec; }
    if (ctx.renderer == nullptr || ctx.renderer->GetCurrentFrameIndex() >= render::kFrameCount) { return dec; }
    dec.active = true;

    dec.base = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(indirectArgs_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(visibleList_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(indirectCounts_.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

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
        ctx.Use(instancesUnified_.buffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
        EnsureReadback(ctx.renderer, indirectArgs_.regionBytes);
        dec.readback = valReadback_ != nullptr;
    }
    if (dec.readback)
    {
        valBounds_ = cpuBounds_;
        valFrustums_ = cpuViewFrustums_;
        valCasters_ = dec.giOn ? count_ : staticCount_;
        valViews_ = render::kMaxShadowViews;
        valGroups_ = numMeshGroups_;
        valFrame_ = ctx.renderer->GetTotalFrameNumber();
        valState_ = 1;
        ctx.NextPoint();
        dec.valCopy = ctx.usePoint ? *ctx.usePoint : 0u;
        ctx.Use(indirectArgs_.buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    ctx.NextPoint();
    dec.consume = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(indirectArgs_.buffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    ctx.Use(visibleList_.buffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    return dec;
}

Material* ShadowGpuData::IndirectShadowPageMaterial() const
{
    return MaskedShadowsActive() ? indirectShadowPageMaskedMat_.get() : indirectShadowPageMat_.get();
}

bool ShadowGpuData::MaskedShadowsActive() const
{
    return hasMaskedGroups_ && indirectShadowMaskedMat_ && indirectShadowMaskedMat_->GetPipelineState();
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
    const std::vector<std::unique_ptr<RenderableObjectBase>>& objects)
{
    if (!renderer) { return; }

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

    // (1) STATIC casters: fill per-caster data + assign mesh-groups (the cull groups draws by mesh,
    // so the indirect arg/count buffers are sized per (view, mesh-group)). B3: one caster SLOT per
    // (object, submesh) and one group per (mesh, submesh) — a mesh's groups are allocated
    // contiguously first-seen, so group(mesh, s) = meshToGroup[mesh] + s. All of an object's slots
    // share its instance data/bounds (per-submesh bounds are a future refinement).
    std::unordered_map<const Mesh*, std::uint32_t> meshToGroup; // mesh -> FIRST group id
    std::vector<const Mesh*> casterMesh(casterCount, nullptr);
    std::vector<std::uint32_t> casterSub(casterCount, 0u);      // submesh ordinal within casterMesh
    std::vector<std::uint32_t> staticDynamic(casterCount, 0u); // per static caster: IsDynamicCaster (VSM page-cache invalidation)
    // C2: per-group shadow-mask table, filled when a mesh is FIRST seen — the first object using
    // a (mesh, slot) defines the group's mask (an object overriding a shared mesh's slot keeps
    // the first object's shadow mask; same shared-mesh semantics as the mega buffer / RT BLAS).
    std::vector<DirectX::XMUINT2> groupMaskCpu; // per group: {albedo slot (~0 = opaque), asuint(cutoff)}
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
            if (mesh->IsChunkedSubmeshes()) { ++chunkedMeshes; }
            GBufferRenderable* gb = obj->AsGBufferRenderable();
            for (size_t s = 0; s < slots; ++s)
            {
                DirectX::XMUINT2 gm{ 0xFFFFFFFFu, 0u };
                const MaterialData* md = gb ? gb->GetMaterialDataForSlot(s) : nullptr;
                if (md && md->alphaMask && md->hasAlbedo)
                {
                    if (maskedAlbedoCount_ < kMaxMaskedGroups)
                    {
                        maskedAlbedoSrvs_[maskedAlbedoCount_] = md->albedo.GetSRVCPU();
                        gm.x = maskedAlbedoCount_++;
                        const float cutoff = md->alphaCutoff;
                        std::memcpy(&gm.y, &cutoff, sizeof(gm.y));
                    }
                    else
                    {
                        maskedOverflow = true; // over the t3 table cap -> this group casts solid
                    }
                }
                groupMaskCpu.push_back(gm);
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
        if (inst.windStrength > 0.0f) { hasWindCasters_ = true; }
        const std::uint32_t dyn = obj->IsDynamicCaster() ? 1u : 0u;
        const bool chunked = mesh && mesh->IsChunkedSubmeshes();
        for (size_t s = 0; s < slots; ++s)
        {
            cpuInstances_[idx] = inst;
            // Caster ids are already one per slot, so the PER-SLOT foliage weight rides here for
            // free — this is what lets the shadow VS treat fronds and trunk differently without any
            // extra buffer. FillInstance only knows slot 0, so patch each slot's own value in.
            if (gbFoliage) { cpuInstances_[idx].windFoliage = gbFoliage->FoliageForSlot(s); }
            cpuBounds_[idx] = bnd;
            if (chunked) { FillChunkBounds(ro, s, bnd, cpuBounds_[idx]); }
            casterMesh[idx] = mesh;
            casterSub[idx] = static_cast<std::uint32_t>(s);
            staticDynamic[idx] = dyn;
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
    numStaticGroups_ = staticGroups; // groups below this are static (LOD-biased); the rest are GI (LOD0)

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
    // Cull-view layout: [cascades | spots | point-faces | clipmap]. A view's tier picks a base LOD
    // (near = fine, far = coarse); the global bias shifts it. Locals use the near tier. The final LOD
    // is clamped per mesh by the tables below (a mesh may have fewer LODs than the view asks for).
    const int lodCap = static_cast<int>(render::kMaxShadowLods) - 1;
    viewLod_.assign(render::kMaxShadowViews, 0u);
    {
        constexpr std::uint32_t kCasc = vsm::kNumCascades;                       // [0, 4)
        constexpr std::uint32_t kLocalEnd = kCasc + LightManager::kMaxShadowedSpotLights
                                          + LightManager::kMaxShadowedPointLights * 6u; // spots + point faces
        for (std::uint32_t v = 0; v < render::kMaxShadowViews; ++v)
        {
            std::uint32_t tier;
            if (v < kCasc)              { tier = v; }                  // CSM cascade index (near->far)
            else if (v < kLocalEnd)     { tier = 0u; }                 // local light -> near tier
            else                        { tier = v - kLocalEnd; }      // VSM clipmap level (near->far)
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
    std::vector<std::uint32_t> perViewGroup(
        static_cast<size_t>(render::kMaxShadowViews) * std::max<std::uint32_t>(numMeshGroups_, 1u) * 4u, 0u);
    for (std::uint32_t v = 0; v < render::kMaxShadowViews; ++v)
    {
        for (std::uint32_t g = 0; g < numMeshGroups_; ++g)
        {
            std::uint32_t count = groupIndexCount[g]; // LOD0 default (GI groups + no-LOD meshes)
            std::uint32_t start = groupStartIndex[g];
            const Mesh* m = (g < groupMesh_.size()) ? groupMesh_[g] : nullptr;
            if (m && g < staticGroups) // static submesh group -> per-view LOD
            {
                const auto it = meshToGroup.find(m);
                const std::uint32_t s = (it != meshToGroup.end()) ? (g - it->second) : 0u;
                const UINT lod = m->ClampExplicitLod(biasedLod(v, g));
                const auto& subs = m->SubmeshesForLod(lod);
                if (s < subs.size()) { count = subs[s].indexCount; start = subs[s].indexOffset; }
                else                 { count = m->GetLodIndexCount(lod); start = 0u; }
            }
            const size_t o = (static_cast<size_t>(v) * numMeshGroups_ + g) * 4u;
            perViewGroup[o + 0] = groupBase[g];
            perViewGroup[o + 1] = count;
            perViewGroup[o + 2] = start;
            perViewGroup[o + 3] = 0u;
        }
    }
    if (EnsureRing(renderer, perViewGroup_,
            std::max<size_t>(static_cast<size_t>(render::kMaxShadowViews) * numMeshGroups_, 1),
            4 * sizeof(std::uint32_t), L"ShadowGpuData.PerViewGroup") &&
        numMeshGroups_ > 0)
    {
        std::memcpy(perViewGroup_.Region(0), perViewGroup.data(),
                    static_cast<size_t>(render::kMaxShadowViews) * numMeshGroups_ * 4u * sizeof(std::uint32_t));
    }

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
                const auto& subs = m->SubmeshesForLod(cl);
                if (sOrd < subs.size()) { lodRel = subs[sOrd].indexOffset; count = subs[sOrd].indexCount; }
                else                    { lodRel = 0u; count = m->GetLodIndexCount(cl); }
            }
            const size_t o = (static_cast<size_t>(g) * render::kMaxShadowLods + L) * 4u;
            groupLodMega_[o + 1] = lodRel;
            groupLodMega_[o + 2] = count;
        }
    }

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
        std::memcpy(casterMeta_.Region(0), casterMeta.data(), count_ * sizeof(std::uint32_t));
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
    hasMaskedGroups_ = maskedAlbedoCount_ > 0;
    if (maskedOverflow)
    {
        OutputDebugStringA("[ShadowGpuData] WARNING: masked shadow groups exceed the albedo table cap; excess groups cast solid shadows.\n");
    }
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
    megaVBBytes_ = megaIBBytes_ = megaStride_ = 0;
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
                megaCopy_.push_back(MegaCopy{ m, static_cast<UINT>(vb->GetDesc().Width), lodCount });
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
    const size_t numViews = render::kMaxShadowViews;
    const size_t groups = std::max<size_t>(numMeshGroups_, 1);
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
    // Step 2 (GI→VSM): DEFAULT-heap mirrors of instances_/bounds_, sized to `casters` per region.
    // RecordCull copies the ring's region into these each frame (verbatim at this step; Step 4 also
    // scatters GI casters into them). Their per-region SRVs feed the cull (bounds) + indirect VS (t0).
    EnsureUavRing(renderer, instancesUnified_, casters * sizeof(render::InstancePerObject), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"ShadowGpuData.InstancesUnified");
    EnsureUavRing(renderer, boundsUnified_, casters * sizeof(render::CasterBounds), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"ShadowGpuData.BoundsUnified");
    // Instantiate the shared DRAW_INDEXED indirect command signature so it exists post-load.
    renderer->GetDrawIndexedCommandSignature();
    // Step 4: per-region UAVs for the cull outputs (depend on the just-decided sizes).
    RebuildCullDescriptors(renderer);
    RebuildUnifiedDescriptors(renderer); // Step 2: per-region SRVs onto the unified buffers
    valState_ = 0; // re-validate after a caster-set change

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
    OutputDebugStringA(buf);
    // The group count is the number this whole feature is capped by (VSM_MAX_SETUP_GROUPS), so a
    // re-bake's effect on the caster set can be READ rather than assumed.
    LogCasterLine(buf);
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
        Rebuild(renderer, objects);
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
        if ((ro && ro->MovedThisFrame()) || windFadeChanged)
        {
            FillInstance(obj, cpuInstances_[idx]);
            render::CasterBounds objectBounds{};
            FillBounds(obj, objectBounds);
            cpuBounds_[idx] = objectBounds;
            // Terrain today is static, so this branch never runs for a chunked mesh — but leaving
            // the object box here would silently un-chunk a mesh the moment one ever moved, and the
            // bug would look like a pure performance regression with a correct image.
            const Mesh* movedMesh = ro ? ro->GetMesh() : nullptr;
            const bool chunkedMover = movedMesh && movedMesh->IsChunkedSubmeshes();
            if (chunkedMover) { FillChunkBounds(ro, 0, objectBounds, cpuBounds_[idx]); }
            const GBufferRenderable* gbF = obj->AsGBufferRenderable();
            if (gbF) { cpuInstances_[idx].windFoliage = gbF->FoliageForSlot(0); }
            for (size_t s = 1; s < slots; ++s) // duplicate across the object's submesh slots
            {
                cpuInstances_[idx + s] = cpuInstances_[idx];
                if (gbF) { cpuInstances_[idx + s].windFoliage = gbF->FoliageForSlot(s); }
                cpuBounds_[idx + s] = cpuBounds_[idx];
                if (chunkedMover) { FillChunkBounds(ro, s, objectBounds, cpuBounds_[idx + s]); }
            }
            for (size_t s = 0; s < slots; ++s)
            {
                pending_[idx + s] = static_cast<std::uint8_t>(render::kFrameCount);
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
        --logFramesRemaining_;
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "[ShadowGpuData] frame update: %u/%u casters re-uploaded.\n", uploaded, count_);
        OutputDebugStringA(buf);
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

        // C2: the SHADOW_MASKED variant — selected by IndirectShadowMaterial() whenever the
        // caster set contains alpha-masked groups (opaque groups early-out in its PS, so ONE
        // PSO serves the whole set and the single-ExecuteIndirect structure stays). CULL_NONE:
        // masked foliage is authored double-sided, and depth-only backface culling would drop
        // casters. Failure is non-fatal — masked groups just cast solid shadows.
        gd.inputLayoutKey = "PosUV_InstCasterId";
        gd.defines.emplace_back("SHADOW_MASKED", "1");
        gd.raster.CullMode = D3D12_CULL_MODE_NONE;
        indirectShadowMaskedMat_ = mm->GetOrCreateGraphics(renderer, gd);
        if (!indirectShadowMaskedMat_ || !indirectShadowMaskedMat_->GetPipelineState())
        {
            OutputDebugStringA("[ShadowGpuData] masked indirect shadow PSO FAILED (masked casters cast solid shadows).\n");
            indirectShadowMaskedMat_.reset();
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
            OutputDebugStringA("[ShadowGpuData] pool-format indirect shadow PSO FAILED (per-page loop fallback off).\n");
            indirectShadowPoolMat_.reset();
        }
        gd.defines.emplace_back("SHADOW_MASKED", "1");
        gd.inputLayoutKey = "PosUV_InstCasterId";
        gd.raster.CullMode = D3D12_CULL_MODE_NONE;
        indirectShadowPoolMaskedMat_ = mm->GetOrCreateGraphics(renderer, gd);
        if (!indirectShadowPoolMaskedMat_ || !indirectShadowPoolMaskedMat_->GetPipelineState())
        {
            OutputDebugStringA("[ShadowGpuData] pool-format masked indirect shadow PSO FAILED (per-page loop fallback off).\n");
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
            OutputDebugStringA("[ShadowGpuData] VSM_PAGE indirect shadow PSO FAILED (single-draw page render off).\n");
            indirectShadowPageMat_.reset();
        }

        gd.defines.emplace_back("SHADOW_MASKED", "1");
        gd.inputLayoutKey = "PosUV_InstCasterId";
        gd.raster.CullMode = D3D12_CULL_MODE_NONE; // double-sided foliage, as above
        indirectShadowPageMaskedMat_ = mm->GetOrCreateGraphics(renderer, gd);
        if (!indirectShadowPageMaskedMat_ || !indirectShadowPageMaskedMat_->GetPipelineState())
        {
            OutputDebugStringA("[ShadowGpuData] VSM_PAGE masked indirect shadow PSO FAILED (single-draw page render off).\n");
            indirectShadowPageMaskedMat_.reset();
        }
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
    if (!giScatterMat_ || !giScatterMat_->GetPipelineState())
    {
        OutputDebugStringA("[ShadowGpuData] GI-scatter PSO creation FAILED (GI stays on the CPU tail).\n");
        giScatterMat_.reset();
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
    unifiedDescr_.fill({});
    unifiedSrvHeap_.Reset();
    if (!renderer || !renderer->GetDevice()) { return; }
    if (!instancesUnified_.Valid() || !boundsUnified_.Valid() || count_ == 0) { return; }

    ID3D12Device* dev = renderer->GetDevice();
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 4 * render::kFrameCount; // [0..k)=inst SRV, [k..2k)=bounds SRV, [2k..3k)=inst UAV, [3k..4k)=bounds UAV
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
    renderer->GetDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
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

    const std::uint32_t numViews = render::kMaxShadowViews;
    const std::uint32_t numGroups = numMeshGroups_;
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

    struct CullCB { std::uint32_t numCasters, numViews, numGroups, pad; };
    auto writeCB = [&](std::uint8_t* dst) { CullCB c{ numCasters, numViews, numGroups, 0u }; std::memcpy(dst, &c, sizeof(c)); };
    const UINT cbSize = static_cast<UINT>(sizeof(CullCB));
    const D3D12_GPU_DESCRIPTOR_HANDLE noSampler{};

    // Clear/init the per-(view, mesh-group) indirect args + per-view draw counts. Reads perViewGroup_
    // so each view's args carry THAT view's shadow-LOD index count + start (per-view LOD, Legacy + Rung0).
    RecordComputeDispatch(renderer, cl, cullClearMat_.get(), cbSize, writeCB,
        { perViewGroup_.Srv(0) },
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

    // Step 4 validation (temporary, one-shot after warmup): read back this region's args so a CPU
    // cull can be compared against them kFrameCount frames later. The decision, the snapshot and
    // the readback buffer are all the builder's (see PrepareCullPass) — what is left here is the
    // copy itself.
    if (dec.readback)
    {
        renderer->EmitPoint(cl, dec.valCopy);
        cl->CopyBufferRegion(valReadback_.Get(), 0, indirectArgs_.buffer.Get(),
                             static_cast<UINT64>(f) * indirectArgs_.regionBytes, indirectArgs_.regionBytes);
    }

    // Step 6: leave the args in INDIRECT_ARGUMENT and the visible list as a vertex buffer so the
    // shadow passes (chained after this pass) can ExecuteIndirect + bind the per-instance stream
    // without touching state on their parallel CLs. Done every frame (harmless when the toggle is
    // off — nothing reads them); next frame's start transitions them back to UAV. Counts stays
    // UAV (Step 6 uses maxCount=1 per draw, so no count buffer is read).
    renderer->EmitPoint(cl, dec.consume);
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
    // SRV for this frame's region. C2: with masked groups present, the masked PSO also reads
    // casterGroup (t1) + groupMask (t2) + the masked albedo table (t3..).
    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    RenderContext& ctx = ctxHandle.ref();
    ctx.cbv[1] = viewCB;
    if (MaskedShadowsActive())
    {
        ctx.srvTable[0] = renderer->StageSrvUavTable({ InstanceReadSrv(f), CasterGroupSrv(), GroupMaskSrv() }).gpu;
        ctx.srvTable[3] = renderer->StageSrvUavTable(maskedAlbedoSrvs_, maskedAlbedoCount_).gpu;
    }
    else
    {
        ctx.srvTable[0] = renderer->StageSrvUavTable({ InstanceReadSrv(f) }).gpu; // unified copy (Step 2), else ring
    }
    IndirectShadowMaterial()->Bind(cl, ctx, false);
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
    const Mesh* boundMesh = nullptr; // B3: a mesh's submesh groups are contiguous — bind once
    for (std::uint32_t g = 0; g < numMeshGroups_; ++g)
    {
        const Mesh* mesh = (g < groupMesh_.size()) ? groupMesh_[g] : nullptr;
        if (!mesh) { continue; }
        ID3D12Resource* vb = mesh->GetVertexBufferResource();
        // Per-view shadow LOD: bind THIS view's shadow-LOD index buffer for static groups (the args'
        // IndexCount/StartIndex were seeded from perViewGroup_ = this view+LOD's submesh ranges).
        // GI groups (g >= numStaticGroups_) draw the whole LOD0 buffer, so keep LOD0. VB shared.
        const UINT groupLod = (g < numStaticGroups_) ? mesh->ClampExplicitLod(ViewLodAt(viewSlot)) : 0u;
        ID3D12Resource* ib = mesh->GetLodIndexBufferResource(groupLod);
        if (!vb || !ib) { continue; }

        if (mesh != boundMesh)
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
        }

        // One indirect draw per (view, mesh-group). InstanceCount (from the cull) may be 0 -> a
        // free no-op, so empty groups cost nothing beyond the binding. B3: the args carry the
        // group's submesh StartIndexLocation (seeded by the cull-clear from PerGroup).
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
        HRESULT hr = renderer->GetDevice()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(buf.GetAddressOf()));
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
    // The verdict this validation exists to produce, readable by the gate runs that trust it.
    LogCasterLine(buf);
    valState_ = 2;
}

void ShadowGpuData::Reset()
{
    // Retain the GPU buffers + SRV descriptors across a level unload (a pass may reference an
    // SRV while frames are in flight). Only CPU-side state is dropped; the next Rebuild reuses
    // the allocations when their capacity still suffices.
    count_ = 0;
    staticCount_ = 0;
    viewFrustumCount_ = 0;
    numMeshGroups_ = 0;
    cpuInstances_.clear();
    cpuBounds_.clear();
    pending_.clear();
    cpuViewFrustums_.clear();
    groupMesh_.clear();
    giCasters_.clear();            // GI reservation dropped; next Rebuild re-enumerates + refreshes obj*
    giFoldableInstances_ = 0;
    megaCopy_.clear();             // mesh pointers dangle across a level unload
    maskedAlbedoSrvs_.fill({});    // C2: MaterialData-owned SRV handles dangle across a level unload
    maskedAlbedoCount_ = 0;
    hasMaskedGroups_ = false;
    forceContentRefresh_ = false;
    megaWanted_ = megaReady_ = false; // groupMesh_ gone; next Rebuild frees + rebuilds the mega buffers
    valState_ = 0;
    logFramesRemaining_ = 5;
}
