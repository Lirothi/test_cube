#include "vfx/ParticleEmitterObject.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include <Windows.h>

#include "app/camera/Camera.h"
#include "core/Helpers.h"
#include "core/math/Math.h"
#include "materials/Material.h"
#include "rendering/core/CommandListBindState.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadManager.h"
#include "rendering/descriptors/SamplerManager.h"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr UINT kThreadsPerGroup = 64;
    constexpr UINT kReadbackSlots = 4; // >= frames in flight, so the oldest slot is settled
    constexpr uint32_t kSortMax = 1024; // == SORT_N in particle_sort_cs.hlsl (single group)

    uint32_t HashU32(uint32_t v)
    {
        uint32_t state = v * 747796405u + 2891336453u;
        uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        return (word >> 22u) ^ word;
    }
}

ParticleEmitterObject::ParticleEmitterObject(const vfx::EmitterDesc& desc)
    // No graphics shader yet (E2 adds the billboard draw); a null graphics material makes
    // Render()/RenderShadow() no-op safely.
    : RenderableObject("PosNormTanUV", L"")
    , desc_(desc)
{
    desc_.maxParticles = std::max(desc_.maxParticles, 1u);
    allowWireframe_ = false;
}

void ParticleEmitterObject::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    // Intentionally NOT calling RenderableObject::Init: there is no graphics/shadow material
    // in E1 (and an empty shader path must not reach the material compiler).
    if (!renderer || !uploadCmdList)
    {
        return;
    }

    updateCs_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, L"shaders/particle_update_cs.hlsl");
    spawnCs_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, L"shaders/particle_spawn_cs.hlsl");

    // E2c: alpha (smoke) emitters sort back-to-front. Single-group bitonic caps the count.
    sortEnabled_ = desc_.sortParticles && desc_.maxParticles <= kSortMax;
    if (desc_.sortParticles && !sortEnabled_)
    {
        OutputDebugStringA("[vfx] emitter maxParticles > sort cap (1024); back-to-front sort disabled\n");
    }
    if (sortEnabled_)
    {
        sortCs_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, L"shaders/particle_sort_cs.hlsl");
    }

    CreateBuffers(renderer, uploadCmdList, uploadKeepAlive);
    CreateDescriptors(renderer->GetDevice());

    // E2: billboard pipeline. Vertexless ("None" resolves to an empty input layout); draws into
    // the transparent pass targets — RT0 (scene color) blends, while velocity/objectId
    // targets are write-masked off (a soft quad must not stomp motion vectors or picking ids
    // under the fire). Reversed-Z depth test, no depth write, no culling (billboards).
    {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/particles.hlsl";
        gd.inputLayoutKey = "None";
        gd.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
#if WITH_EDITOR
        gd.numRT = 3;
#else
        gd.numRT = 2;
#endif
        gd.rtvFormats[0] = renderer->GetSceneColorFormat();
        gd.rtvFormats[1] = renderer->GetGBufferVelocityFormat();
#if WITH_EDITOR
        gd.rtvFormats[2] = renderer->GetObjectIdFormat();
#endif
        gd.dsvFormat = renderer->GetDsvFormat();
        // The ocean is submitted before particles and writes depth, so the regular depth test
        // resolves their overlap per pixel: foreground particles survive while particles genuinely
        // behind the water are culled. The PS separately uses the opaque depth copy for soft fades.
        gd.depth.DepthEnable = TRUE;
        gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        gd.depth.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        gd.raster.CullMode = D3D12_CULL_MODE_NONE;
        gd.blend.IndependentBlendEnable = TRUE;
        gd.blend.RenderTarget[0].BlendEnable = TRUE;
        gd.blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE; // PS outputs premultiplied rgb
        gd.blend.RenderTarget[0].DestBlend = desc_.additive ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
        gd.blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        gd.blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        gd.blend.RenderTarget[0].DestBlendAlpha = desc_.additive ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
        gd.blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        for (UINT rt = 1; rt < gd.numRT; ++rt)
        {
            gd.blend.RenderTarget[rt].BlendEnable = FALSE;
            gd.blend.RenderTarget[rt].RenderTargetWriteMask = 0; // untouched under the quad
        }
        if (sortEnabled_) { gd.defines.emplace_back("PARTICLE_SORTED", "1"); }
        drawMaterial_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
    }

    hasSprite_ = false;
    if (!desc_.texture.empty())
    {
        Texture2D::CreateDesc td{};
        td.path = std::wstring(desc_.texture.begin(), desc_.texture.end());
        td.usage = Texture2D::Usage::AlbedoSRGB;
        hasSprite_ = sprite_.CreateFromFile(renderer, uploadCmdList, td, uploadKeepAlive);
    }
}

void ParticleEmitterObject::CreateBuffers(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    const uint32_t n = desc_.maxParticles;

    // Initial contents make the whole pool dead with every slot on the stack — no init
    // dispatch needed (mirrors the InstanceBuffer creation idiom).
    std::vector<vfx::GpuParticle> initParticles(n);
    for (vfx::GpuParticle& p : initParticles)
    {
        std::memset(&p, 0, sizeof(p));
        p.age = -1.0f;
    }
    std::vector<uint32_t> initDead(n);
    for (uint32_t i = 0; i < n; ++i) { initDead[i] = i; }
    const uint32_t initCount[4] = { n, 0u, 0u, 0u };

    UploadManager up(renderer->GetDevice(), uploadCmdList);
    particles_ = up.CreateBufferWithData(initParticles.data(), sizeof(vfx::GpuParticle) * n,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    deadList_ = up.CreateBufferWithData(initDead.data(), sizeof(uint32_t) * n,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    deadCount_ = up.CreateBufferWithData(initCount, sizeof(initCount),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (sortEnabled_)
    {
        std::vector<uint32_t> initSorted(n);
        for (uint32_t i = 0; i < n; ++i) { initSorted[i] = i; }
        sorted_ = up.CreateBufferWithData(initSorted.data(), sizeof(uint32_t) * n,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    up.StealKeepAlive(uploadKeepAlive);

    renderer->SetResourceState(particles_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->SetResourceState(deadList_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->SetResourceState(deadCount_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    particles_->SetName(L"vfx.particles");
    deadList_->SetName(L"vfx.deadList");
    deadCount_->SetName(L"vfx.deadCount");
    if (sorted_)
    {
        renderer->SetResourceState(sorted_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        sorted_->SetName(L"vfx.sorted");
    }

    // Debug alive-count readback ring (tiny, persistently mapped).
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = sizeof(uint32_t) * kReadbackSlots;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc = { 1, 0 };
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ThrowIfFailed(renderer->GetDevice()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)));
    readback_->SetName(L"vfx.aliveReadback");
    void* mapped = nullptr;
    ThrowIfFailed(readback_->Map(0, nullptr, &mapped));
    readbackPtr_ = static_cast<const uint32_t*>(mapped);
}

void ParticleEmitterObject::CreateDescriptors(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 6;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-only staging
    ThrowIfFailed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&cpuHeap_)));
    cpuHeap_->SetName(L"vfx.emitterDescriptors");

    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHeap_->GetCPUDescriptorHandleForHeapStart();
    particlesUav_ = h; h.ptr += inc;
    deadListUav_ = h; h.ptr += inc;
    deadCountUav_ = h; h.ptr += inc;
    particlesSrv_ = h; h.ptr += inc;
    sortedUav_ = h; h.ptr += inc;
    sortedSrv_ = h;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.FirstElement = 0;
    uav.Buffer.NumElements = desc_.maxParticles;
    uav.Buffer.StructureByteStride = sizeof(vfx::GpuParticle);
    device->CreateUnorderedAccessView(particles_.Get(), nullptr, &uav, particlesUav_);

    uav.Buffer.StructureByteStride = sizeof(uint32_t);
    device->CreateUnorderedAccessView(deadList_.Get(), nullptr, &uav, deadListUav_);

    uav.Buffer.NumElements = 4;
    device->CreateUnorderedAccessView(deadCount_.Get(), nullptr, &uav, deadCountUav_);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = 0;
    srv.Buffer.NumElements = desc_.maxParticles;
    srv.Buffer.StructureByteStride = sizeof(vfx::GpuParticle);
    device->CreateShaderResourceView(particles_.Get(), &srv, particlesSrv_);

    if (sorted_)
    {
        uav.Buffer.NumElements = desc_.maxParticles;
        uav.Buffer.StructureByteStride = sizeof(uint32_t);
        device->CreateUnorderedAccessView(sorted_.Get(), nullptr, &uav, sortedUav_);
        srv.Buffer.NumElements = desc_.maxParticles;
        srv.Buffer.StructureByteStride = sizeof(uint32_t);
        device->CreateShaderResourceView(sorted_.Get(), &srv, sortedSrv_);
    }
}

void ParticleEmitterObject::Tick(float dt)
{
    // Conservative swept bounds for culling/sorting: max travel + sprite radius, gravity drift
    // included. Cheap enough to refresh every frame (position may be animated/gizmo-dragged).
    const Math::float3 pos = GetPosition();
    const float travel = desc_.speedMax * desc_.lifetimeMax +
        0.5f * std::abs(desc_.gravity) * desc_.lifetimeMax * desc_.lifetimeMax;
    const float r = travel + std::max(desc_.sizeStart, desc_.sizeEnd);
    worldBounds_ = AABB(Math::float3(pos.x - r, pos.y - r, pos.z - r),
                        Math::float3(pos.x + r, pos.y + r, pos.z + r));

    if (vfx::g_freeze)
    {
        dt_ = 0.0f;
        return;
    }
    dt_ = std::min(dt, 0.1f); // clamp hitch spikes so particles don't teleport
    spawnAccum_ = std::min(spawnAccum_ + desc_.spawnRate * dt_,
        static_cast<float>(desc_.maxParticles)); // cap bursts after long stalls
    logAccum_ += dt;
}

void ParticleEmitterObject::RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer || !cl || !updateCs_ || !spawnCs_ || !particles_) { return; }
    if (vfx::g_freeze || dt_ <= 0.0f) { return; }
    ++frameCounter_;

    uint32_t spawnCount = static_cast<uint32_t>(spawnAccum_);
    spawnAccum_ -= static_cast<float>(spawnCount);

    // Per-frame emitter CB (b1). Refill-from-desc each frame => editor tweaks are live.
    vfx::GpuEmitterParams prm{};
    const Math::float3 pos = GetPosition();
    prm.emitterPos[0] = pos.x; prm.emitterPos[1] = pos.y; prm.emitterPos[2] = pos.z;
    prm.dt = dt_;
    prm.coneDir[0] = desc_.coneDir.x; prm.coneDir[1] = desc_.coneDir.y; prm.coneDir[2] = desc_.coneDir.z;
    prm.coneHalfAngleRad = desc_.coneAngleDeg * 0.5f * Math::DEG2RAD;
    prm.lifeMin = desc_.lifetimeMin; prm.lifeMax = desc_.lifetimeMax;
    prm.speedMin = desc_.speedMin; prm.speedMax = desc_.speedMax;
    prm.gravity = desc_.gravity; prm.drag = desc_.drag;
    prm.rotMin = desc_.rotMin; prm.rotMax = desc_.rotMax;
    prm.spinMin = desc_.spinMin; prm.spinMax = desc_.spinMax;
    prm.maxParticles = desc_.maxParticles;
    prm.frameSeed = HashU32(frameCounter_ * 0x9E3779B9u ^ desc_.seed * 0x85EBCA6Bu);

    auto cb = renderer->GetFrameResource()->AllocDynamic(sizeof(prm), render::kConstantBufferAlignment);
    std::memcpy(cb.cpu, &prm, sizeof(prm));

    auto uavTbl = renderer->StageSrvUavTable({ particlesUav_, deadListUav_, deadCountUav_ });

    renderer->Transition(cl, particles_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, deadList_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, deadCount_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    auto h = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = h.ref();
    ctx.cbv[1] = cb.gpu;
    ctx.uavTable[0] = uavTbl.gpu;

    // Update first (age/integrate/kill), then spawn — new particles render at age 0.
    ctx.constants[0] = { 0u, 0u, 0u, 0u };
    updateCs_->Bind(cl, ctx);
    cl->Dispatch((desc_.maxParticles + kThreadsPerGroup - 1u) / kThreadsPerGroup, 1, 1);

    renderer->UAVBarrier(cl, particles_.Get());
    renderer->UAVBarrier(cl, deadList_.Get());
    renderer->UAVBarrier(cl, deadCount_.Get());

    if (spawnCount > 0u)
    {
        ctx.constants[0] = { spawnCount, prm.frameSeed, 0u, 0u };
        spawnCs_->Bind(cl, ctx);
        cl->Dispatch((spawnCount + kThreadsPerGroup - 1u) / kThreadsPerGroup, 1, 1);

        renderer->UAVBarrier(cl, particles_.Get());
        renderer->UAVBarrier(cl, deadList_.Get());
        renderer->UAVBarrier(cl, deadCount_.Get());
    }

    // E2c: bitonic sort into `sorted_` (far-to-near) using the previous frame's camera position.
    // Runs after the particle state is final; the VS reads `sorted_` in the transparent pass.
    if (sortEnabled_ && sortCs_ && sorted_)
    {
        renderer->Transition(cl, sorted_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto sortTbl = renderer->StageSrvUavTable({ particlesUav_, sortedUav_ });
        auto sh = renderer->GetRenderContextPool()->Acquire();
        auto& sctx = sh.ref();
        const uint32_t cx = Math::FloatToUint32(lastCamPos_.x);
        const uint32_t cy = Math::FloatToUint32(lastCamPos_.y);
        const uint32_t cz = Math::FloatToUint32(lastCamPos_.z);
        sctx.constants[0] = { cx, cy, cz, desc_.maxParticles };
        sctx.uavTable[0] = sortTbl.gpu;
        sortCs_->Bind(cl, sctx);
        cl->Dispatch(1, 1, 1); // single workgroup
        renderer->UAVBarrier(cl, sorted_.Get());
    }

    if (vfx::g_debugAliveLog && readbackPtr_)
    {
        renderer->Transition(cl, deadCount_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyBufferRegion(readback_.Get(), (frameCounter_ % kReadbackSlots) * sizeof(uint32_t),
            deadCount_.Get(), 0, sizeof(uint32_t));
        renderer->Transition(cl, deadCount_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (logAccum_ >= 1.0f && frameCounter_ > kReadbackSlots)
        {
            logAccum_ = 0.0f;
            // Oldest ring slot: written kReadbackSlots-1 frames ago — safely retired.
            const uint32_t dead = readbackPtr_[(frameCounter_ + 1u) % kReadbackSlots];
            const uint32_t alive = dead <= desc_.maxParticles ? desc_.maxParticles - dead : 0u;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "[vfx] emitter alive=%u/%u (rate=%.0f/s)\n",
                alive, desc_.maxParticles, desc_.spawnRate);
            OutputDebugStringA(buf);
        }
    }
}

void ParticleEmitterObject::Render(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const Camera& camera, D3D12_GPU_VIRTUAL_ADDRESS viewCB)
{
    if (!renderer || !cl || !drawMaterial_ || !drawMaterial_->GetPipelineState() || !particles_)
    {
        return;
    }

    // E2c: cache the camera for the NEXT frame's compute-pass sort (a one-frame lag in sort
    // order is imperceptible; keeps the sort in the compute pass, not interleaved into graphics).
    lastCamPos_ = camera.GetPosition();

    // The VS reads the sim buffer (and the sorted index buffer); the compute pass left both in UAV.
    renderer->Transition(cl, particles_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (sortEnabled_ && sorted_)
    {
        renderer->Transition(cl, sorted_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    vfx::GpuEmitterDrawParams dp{};
    dp.sizeStart = desc_.sizeStart;
    dp.sizeEnd = desc_.sizeEnd;
    dp.flipCols = std::max(desc_.flipbookCols, 1u);
    dp.flipRows = std::max(desc_.flipbookRows, 1u);
    dp.flipFps = desc_.flipbookFps;
    dp.flipRandomStart = desc_.flipbookRandomStart ? 1u : 0u;
    dp.frameBlend = desc_.frameBlend ? 1u : 0u;
    dp.hasTexture = hasSprite_ ? 1u : 0u;
    for (int k = 0; k < 4; ++k)
    {
        dp.colorKeys[k][0] = desc_.colorKeys[k].x;
        dp.colorKeys[k][1] = desc_.colorKeys[k].y;
        dp.colorKeys[k][2] = desc_.colorKeys[k].z;
        dp.colorKeys[k][3] = desc_.colorKeys[k].w;
    }
    dp.maxParticles = desc_.maxParticles;

    // E2b: the depthCopy (opaque depth snapshotted at the start of Pass_Transparent) is the
    // soft-particle source. It always exists; make it a pixel SRV (the tracker no-ops if already
    // there). Disable the fade if it is somehow unavailable.
    const auto& D = renderer->GetDeferredForFrame();
    const bool haveDepth = D.depthCopy.Get() != nullptr && D.depthCopySRV.ptr != 0;
    // depthOcclude drives PS occlusion against the opaque snapshot; the hardware depth test
    // handles the ocean separately. softFade is the fade width; 0 = a hard opaque cutoff.
    dp.depthOcclude = haveDepth ? 1.0f : 0.0f;
    dp.softFadeDist = desc_.softFade;

    auto cb = renderer->GetFrameResource()->AllocDynamic(sizeof(dp), render::kConstantBufferAlignment);
    std::memcpy(cb.cpu, &dp, sizeof(dp));

    if (haveDepth)
    {
        renderer->Transition(cl, D.depthCopy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    auto h = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = h.ref();
    ctx.cbv[1] = viewCB;   // Pass_Transparent's shared per-view CB (GlassView layout)
    ctx.cbv[2] = cb.gpu;   // DrawParams
    // t0 = particle buffer, t1 = sprite atlas (self-aliased when absent; PS won't sample it),
    // t2 = scene depth copy for the soft fade, t3 = sorted order (self-aliased + unread when the
    // PSO has no PARTICLE_SORTED define).
    ctx.srvTable[0] = renderer->StageSrvUavTable(
        { particlesSrv_, hasSprite_ ? sprite_.GetSRVCPU() : particlesSrv_,
          haveDepth ? D.depthCopySRV : particlesSrv_,
          (sortEnabled_ && sorted_) ? sortedSrv_ : particlesSrv_ }).gpu;
    ctx.samplerTable[0] = renderer->GetSamplerManager()->Get(renderer, *SamplerManager::LinearClamp());

    drawMaterial_->Bind(cl, ctx, false);

    // Vertexless draw: 6 verts per slot, dead slots emit zero-w degenerate quads. Keep the
    // IA bind cache coherent with the direct topology set.
    render::CommandListBindState& cache = render::g_clBindState;
    if (!render::g_bindBatchingEnabled || cache.topology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
    {
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (render::g_bindBatchingEnabled) { cache.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; }
    }
    cl->DrawInstanced(6u * desc_.maxParticles, 1, 0, 0);
}
