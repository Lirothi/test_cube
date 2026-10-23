#include "rendering/lighting/SkyAtmosphere.h"

#include "app/camera/Camera.h"
#include "rendering/lighting/Skybox.h"
#include <algorithm>
#include <iterator>
#include <cmath>
#include <cstring>
#include "core/logging/Log.h"
#include "core/profiling/ProfilerScopes.h"
#include "materials/Material.h"
#include "rendering/core/ComputeDispatch.h"
#include "rendering/core/GpuAlloc.h"
#include "rendering/core/MemoryReport.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/TextureCreate.h"
#include "rendering/descriptors/SamplerManager.h"

namespace
{
constexpr UINT kWidth[] = {256, 32}, kHeight[] = {64, 32};
constexpr auto kRest = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

float ReadHalf(const std::uint16_t h)
{
    const unsigned exponent = (h >> 10) & 31u, mantissa = h & 1023u;
    const float magnitude = exponent == 31u ? (mantissa ? NAN : INFINITY)
        : std::ldexp(exponent ? 1.0f + mantissa / 1024.0f : mantissa / 1024.0f,
                     exponent ? static_cast<int>(exponent) - 15 : -14);
    return h & 32768u ? -magnitude : magnitude;
}

// TIME-OF-DAY PLAN P1: A RUNNING CLOCK MOVES THE SKY A LITTLE EVERY FRAME -- the sun, the moon, the stars'
// sphere and, through the twilight, the lights' illuminance -- and the environment capture and the distant
// light, keyed on the exact view, rebuilt every frame of it: measured on wind_test (the owner's three views,
// a day in 30 minutes), +0.07..0.12 ms of the animated frame's +0.02..0.17, the capture alone 0.06-0.10 ms a
// frame. Their keys stay exact for everything else; the bodies within kKeyTurnDeg and their light within
// kKeyLightShare of the two lights' sum count as unchanged, so a day in 30 minutes rebuilds them every ~2.5 s.
// A quarter of a degree is ~1/40 of the sun's aureole and one percent of the light, neither visible in a
// probe or the fog's sky term.
constexpr float kKeyTurnCos = 0.99999048f; // cos(0.25 degrees)
constexpr float kKeyLightShare = 0.01f;

bool SameDirection(const float a[4], const float b[4])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] >= kKeyTurnCos;
}

bool NearlySameView(const SkyViewFrameData& now, const SkyViewFrameData& key)
{
    if (!render::g_skyKeyTolerance) return std::memcmp(&now, &key, sizeof(now)) == 0;
    for (int c = 0; c < 3; ++c)
    {
        const float total = std::max(std::fabs(now.illuminance[c]) + std::fabs(now.light1Illuminance[c]),
                                     std::fabs(key.illuminance[c]) + std::fabs(key.light1Illuminance[c]));
        const float change = std::fabs(now.illuminance[c] - key.illuminance[c]) +
                             std::fabs(now.light1Illuminance[c] - key.light1Illuminance[c]);
        if (change > kKeyLightShare * total) return false;
    }
    if (!SameDirection(now.sunDirection, key.sunDirection) || !SameDirection(now.light1Direction, key.light1Direction))
        return false;
    // Everything else exactly: the moving fields copied over from the key, then the bytes compared.
    SkyViewFrameData rest = now;
    for (int c = 0; c < 3; ++c)
    {
        rest.sunDirection[c] = key.sunDirection[c]; rest.light1Direction[c] = key.light1Direction[c];
        rest.illuminance[c] = key.illuminance[c]; rest.light1Illuminance[c] = key.light1Illuminance[c];
    }
    return std::memcmp(&rest, &key, sizeof(rest)) == 0;
}

// The night sky's picture the same way: its celestial sphere turns with the clock (the rows), within
// kKeyTurnDeg of the last capture's it is the same sky.
bool NearlySameNight(const NightSkyFrameData& now, const NightSkyFrameData& key)
{
    // Not drawn (all zero by day, row0.w = 0): exactly, or a zero row would read as a turn every frame.
    if (!render::g_skyKeyTolerance || now.row0[3] == 0.0f || key.row0[3] == 0.0f)
        return std::memcmp(&now, &key, sizeof(now)) == 0;
    if (!SameDirection(now.row0, key.row0) || !SameDirection(now.row1, key.row1) || !SameDirection(now.row2, key.row2))
        return false;
    NightSkyFrameData rest = now;
    for (int c = 0; c < 3; ++c) { rest.row0[c] = key.row0[c]; rest.row1[c] = key.row1[c]; rest.row2[c] = key.row2[c]; }
    return std::memcmp(&rest, &key, sizeof(rest)) == 0;
}
}

// The bytes these LUTs hold are the `sky` tag of the GPU census (rendering/core/GpuAlloc.h); the
// "sky.luts" column of the memory line it replaced matched it by name (memory profiling plan M1c).
SkyAtmosphere::SkyAtmosphere() = default;

SkyAtmosphere::~SkyAtmosphere() = default;

void SkyAtmosphere::Reset()
{
    moon_.Reset(); moonHeap_.Reset(); moonMaterial_.reset(); moonSrv_ = {}; moonUav_ = {};
    moonReady_ = moonFailed_ = false;
    night_.Reset(); nightHeap_.Reset(); nightMaterial_.reset(); nightSrv_ = {}; nightNullSrv_ = {}; nightUav_ = {};
    nightReady_ = nightFailed_ = nightWanted_ = false; environmentNightKey_ = {};
    distant_.Reset(); distantHeap_.Reset(); distantMaterial_.reset();
    for (auto& r : distantReadback_) r.Reset();
    distantSrv_ = {}; distantUav_ = {}; distantPending_ = {};
    distantActive_ = distantReady_ = distantFailed_ = false; distantBuilds_ = 0;
    for (auto& resource : environment_) resource.Reset();
    environmentView_.Reset(); environmentHeap_.Reset(); captureMaterial_.reset(); filterMaterial_.reset();
    environmentSrv_ = {}; environmentUav_ = {}; environmentViewSrv_ = {}; environmentViewUav_ = {};
    environmentReady_ = environmentFailed_ = false; environmentBuilds_ = 0;
    for (auto& resource : lut_) resource.Reset();
    aerial_.Reset(); aerialMaterial_.reset(); aerialSrv_ = {}; aerialUav_ = {}; aerialBuilt_ = false;
    skyView_.Reset(); viewMaterial_.reset(); viewSrv_ = {}; viewUav_ = {};
    for (auto& resource : readback_) resource.Reset();
    for (auto& material : material_) material.reset();
    heap_.Reset();
    debugMaterial_.reset();
    srv_ = {}; uav_ = {}; pending_ = {};
    initialized_ = failed_ = false;
    builds_ = 0;
}

void SkyAtmosphere::Prepare(Renderer* renderer, const SkyAtmosphereSettings& settings)
{
    // BeginFrame already waited for THIS slot. Never map a different in-flight slot.
    ValidateReadback(renderer->GetCurrentFrameIndex());
    ValidateDistant(renderer->GetCurrentFrameIndex());
    if (settings.mode && !failed_) PrepareDistant(renderer);
    if (settings.mode && !failed_) PrepareMoon(renderer);
    if (settings.mode && !failed_ && nightWanted_) PrepareNight(renderer);
    if (settings.mode && settings.environmentLighting && !failed_) PrepareEnvironment(renderer);
    if ((!settings.mode && !settings.lutEnabled && !settings.lutDebugView) || failed_ || heap_) return;
    auto* device = renderer->GetDevice();
    const auto fail = [this](const char* what) {
        LOG_ERROR(logging::LogCategory::Render, "sky atmosphere LUT: {} failed", what);
        failed_ = true;
    };
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 8;
    if (FAILED(gpu::CreateDescriptorHeap(device, &hd, MemTag::Sky, L"SkyAtmosphere.LutHeap", IID_PPV_ARGS(&heap_)))) { fail("descriptor heap"); return; }
    const auto base = heap_->GetCPUDescriptorHandleForHeapStart();
    const UINT step = device->GetDescriptorHandleIncrementSize(hd.Type);
    const wchar_t* names[] = {L"SkyAtmosphere.Transmittance", L"SkyAtmosphere.MultiScatter"};
    const wchar_t* shaders[] = {L"shaders/sky_lut_transmittance_cs.hlsl", L"shaders/sky_lut_multiscatter_cs.hlsl"};
    UINT64 offset = 0;
    for (UINT i = 0; i < 2; ++i)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kWidth[i]; desc.Height = kHeight[i];
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        if (FAILED(gpu::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, desc, kRest, nullptr, MemTag::Sky, names[i], &resource)))
        { fail("texture"); return; }
        lut_[i].Attach(renderer->Declarations(), resource, kRest, names[i]);
        srv_[i] = {base.ptr + (i * 2u) * step};
        uav_[i] = {base.ptr + (i * 2u + 1u) * step};
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = desc.Format; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource.Get(), &sd, srv_[i]);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = desc.Format; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource.Get(), nullptr, &ud, uav_[i]);
        device->GetCopyableFootprints(&desc, 0, 1, offset, &footprint_[i], nullptr, nullptr, nullptr);
        offset = footprint_[i].Offset + UINT64(footprint_[i].Footprint.RowPitch) * kHeight[i];
        Material::ComputeDesc cd{}; cd.shaderFile = shaders[i]; cd.csEntry = "CSMain";
        material_[i] = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
        if (!material_[i] || !material_[i]->GetPipelineState()) { fail("compute PSO"); return; }
    }
    {
        D3D12_RESOURCE_DESC vd{};
        vd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        vd.Width = 192; vd.Height = 104; vd.DepthOrArraySize = 1; vd.MipLevels = 1;
        vd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; vd.SampleDesc.Count = 1;
        vd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        if (FAILED(gpu::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, vd, kRest, nullptr, MemTag::Sky, L"SkyAtmosphere.SkyView", &resource)))
        { fail("SkyView texture"); return; }
        skyView_.Attach(renderer->Declarations(), resource, kRest, L"SkyAtmosphere.SkyView");
        viewSrv_ = {base.ptr + 4u * step}; viewUav_ = {base.ptr + 5u * step};
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = vd.Format; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource.Get(), &sd, viewSrv_);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = vd.Format; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource.Get(), nullptr, &ud, viewUav_);
        Material::ComputeDesc cd{}; cd.shaderFile = L"shaders/sky_lut_view_cs.hlsl"; cd.csEntry = "CSMain";
        viewMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
        if (!viewMaterial_ || !viewMaterial_->GetPipelineState()) { fail("SkyView PSO"); return; }
    }
    {
        D3D12_RESOURCE_DESC vd{};
        vd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        vd.Width = 32; vd.Height = 32; vd.DepthOrArraySize = 16; vd.MipLevels = 1;
        vd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; vd.SampleDesc.Count = 1;
        vd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        if (FAILED(gpu::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, vd, kRest, nullptr, MemTag::Sky, L"SkyAtmosphere.AerialPerspective", &resource)))
        { fail("AerialPerspective texture"); return; }
        aerial_.Attach(renderer->Declarations(), resource, kRest, L"SkyAtmosphere.AerialPerspective");
        aerialSrv_ = {base.ptr + 6u * step}; aerialUav_ = {base.ptr + 7u * step};
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = vd.Format; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture3D.MipLevels = 1;
        device->CreateShaderResourceView(resource.Get(), &sd, aerialSrv_);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = vd.Format; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D; ud.Texture3D.WSize = 16;
        device->CreateUnorderedAccessView(resource.Get(), nullptr, &ud, aerialUav_);
        Material::ComputeDesc cd{}; cd.shaderFile = L"shaders/sky_lut_aerial_cs.hlsl"; cd.csEntry = "CSMain";
        aerialMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
        if (!aerialMaterial_ || !aerialMaterial_->GetPipelineState()) { fail("AerialPerspective PSO"); return; }
    }
    Material::ComputeDesc debugDesc{};
    debugDesc.shaderFile = L"shaders/sky_lut_debug_cs.hlsl"; debugDesc.csEntry = "CSMain";
    debugMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, debugDesc);
    if (!debugMaterial_ || !debugMaterial_->GetPipelineState()) { fail("LUT debug PSO"); return; }
    readbackBytes_ = offset;
    // Readback is used once per rebuild, no GPU work or CPU mapping on unchanged frames.
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = readbackBytes_;
    desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    for (auto& resource : readback_)
        if (FAILED(gpu::CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, MemTag::Sky, L"SkyAtmosphere.LutReadback", IID_PPV_ARGS(&resource)))) { fail("readback ring"); return; }
}

size_t SkyAtmosphere::Build(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph, const SkyAtmosphereSettings& settings)
{
    constexpr size_t none = static_cast<size_t>(-1);
    if ((!settings.mode && !settings.lutEnabled && !settings.lutDebugView) || failed_ || !heap_) return none;
    const auto params = settings.parameters;
    const bool validate = settings.lutValidate;
    if (initialized_ && std::memcmp(&cached_, &params, sizeof(params)) == 0) return none;
    return graph.AddPass2(RenderPass::Main_SkyAtmosphereLuts, {},
        [this, renderer, params, validate](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            std::array<std::uint32_t, 4> points{};
            points[0] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(lut_[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint(); points[1] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(lut_[0].Get(), kRest);
            ctx.Use(lut_[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint(); points[2] = ctx.usePoint ? *ctx.usePoint : 0u;
            if (validate) for (auto& resource : lut_) ctx.Use(resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            ctx.NextPoint(); points[3] = ctx.usePoint ? *ctx.usePoint : 0u;
            for (auto& resource : lut_) ctx.Use(resource.Get(), kRest);
            const UINT slot = renderer->GetCurrentFrameIndex();
            // Cross-frame state committed ONLY in the serial builder, never worker recording.
            cached_ = params; initialized_ = true;
            pending_[slot] = validate; pendingParameters_[slot] = params;
            LOG_INFO(logging::LogCategory::Render, "sky atmosphere LUT rebuild {}: 256x64/10 + 32x32/15 (UE two directions)", ++builds_);
            return [this, renderer, params, slot, points, validate](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassSkyAtmosphereLuts);
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                {
                    GPU_SCOPE(t.cl, ProfilerScopes::kPassSkyAtmosphereLuts);
                    renderer->EmitPoint(t.cl, points[0]);
                    const auto write = [&params](uint8_t* dst) { std::memcpy(dst, &params, sizeof(params)); };
                    RecordComputeDispatch(renderer, t.cl, material_[0].get(), render::kConstantBufferAlignment,
                        write, {}, {uav_[0]}, {}, kWidth[0], kHeight[0]);
                    renderer->EmitPoint(t.cl, points[1]);
                    const auto samplers = std::array{*SamplerManager::LinearClamp()};
                    RecordComputeDispatch(renderer, t.cl, material_[1].get(), render::kConstantBufferAlignment,
                        write, {srv_[0]}, {uav_[1]}, renderer->GetSamplerManager()->GetTable(renderer, samplers), kWidth[1], kHeight[1]);
                }
                renderer->EmitPoint(t.cl, points[2]);
                if (validate) for (UINT i = 0; i < 2; ++i)
                {
                    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                    dst.pResource = readback_[slot].Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint = footprint_[i];
                    src.pResource = lut_[i].Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    t.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                }
                renderer->EmitPoint(t.cl, points[3]);
                c.EndCL(t);
            };
        });
}

size_t SkyAtmosphere::BuildView(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
    const SkyAtmosphereSettings& settings, const SkyViewFrameData& view, size_t after, size_t luts)
{
    if (!settings.mode || failed_ || !viewMaterial_) return after;
    RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>::DependencyList deps{after};
    if (luts != static_cast<size_t>(-1)) deps.push_back(luts);
    viewPass_.params = settings.parameters; // read by the body (see the member)
    viewPass_.view = view;
    return graph.AddPass2(RenderPass::Main_SkyView, deps, {}, {},
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto point = ctx.usePoint ? *ctx.usePoint : 0u;
            for (auto& resource : lut_) ctx.Use(resource.Get(), kRest);
            ctx.Use(skyView_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint();
            const auto restore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(skyView_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.Use(lut_[0].Get(), kRest | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            return [this, renderer, point, restore](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassSkyView);
                const SkyAtmosphereParameters& params = viewPass_.params;
                const SkyViewFrameData& view = viewPass_.view;
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                {
                    GPU_SCOPE(t.cl, ProfilerScopes::kPassSkyView);
                    renderer->EmitPoint(t.cl, point);
                    const auto samplers = std::array{*SamplerManager::LinearClamp()};
                    RecordComputeDispatch(renderer, t.cl, viewMaterial_.get(), render::kConstantBufferAlignment,
                        [&params, &view](uint8_t* dst) {
                            std::memcpy(dst, &params, sizeof(params));
                            std::memcpy(dst + sizeof(params), &view, sizeof(view));
                        }, {srv_[0], srv_[1]}, {viewUav_}, renderer->GetSamplerManager()->GetTable(renderer, samplers), 192, 104);
                    renderer->EmitPoint(t.cl, restore);
                }
                c.EndCL(t);
            };
        });
}

size_t SkyAtmosphere::BuildAerial(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
    const SkyAtmosphereSettings& settings, const SkyViewFrameData& view, const Camera& camera,
    float startDepthMetres, size_t after)
{
    aerialBuilt_ = false;
    if (!settings.mode || !settings.aerialPerspective || failed_ || !aerialMaterial_) return after;
    static_assert(sizeof(AerialConstants) == 400);
    aerialConstants_ = AerialConstants{settings.parameters, view, camera.GetInvViewMatrix(), camera.GetInvProjMatrixNoJitter(),
        Math::float4(std::max(0.0f, startDepthMetres) / kMetresPerKm,
                     std::clamp(settings.aerialViewDistanceScale, 0.01f, 100.0f), 0, 0)};
    aerialBuilt_ = true;
    return graph.AddPass2(RenderPass::Main_SkyAerial, {after}, {}, {},
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto point = ctx.usePoint ? *ctx.usePoint : 0u;
            for (auto& resource : lut_) ctx.Use(resource.Get(), kRest);
            ctx.Use(aerial_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint();
            const auto restore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(aerial_.Get(), kRest);
            return [this, renderer, point, restore](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassSkyAerial);
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                {
                    GPU_SCOPE(t.cl, ProfilerScopes::kPassSkyAerial);
                    renderer->EmitPoint(t.cl, point);
                    const auto samplers = std::array{*SamplerManager::LinearClamp()};
                    const AerialConstants& data = aerialConstants_;
                    RecordComputeDispatch(renderer, t.cl, aerialMaterial_.get(), 2u * render::kConstantBufferAlignment,
                        [&data](uint8_t* dst) { std::memcpy(dst, &data, sizeof(data)); },
                        {srv_[0], srv_[1]}, {aerialUav_}, renderer->GetSamplerManager()->GetTable(renderer, samplers), 32, 32 * 16);
                    renderer->EmitPoint(t.cl, restore);
                }
                c.EndCL(t);
            };
        });
}

size_t SkyAtmosphere::BuildDebug(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                                const SkyAtmosphereSettings& settings, const Camera& camera, float startDepthMetres, size_t after, size_t luts)
{
    const bool showAerial = aerialBuilt_ && settings.aerialDebugView != 0 && settings.lutDebugView == 0;
    if ((!settings.lutDebugView && !showAerial) || failed_ || !heap_) return after;
    RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>::DependencyList deps;
    deps.push_back(after);
    if (luts != static_cast<size_t>(-1)) deps.push_back(luts);
    struct Constants { UINT width, height, view; float start; Math::mat4 invProj, projNoJitter; };
    static_assert(sizeof(Constants) == 144);
    const Constants data{renderer->GetRenderWidth(), renderer->GetRenderHeight(),
        showAerial ? settings.aerialDebugView + 2u : settings.lutDebugView, std::max(0.0f, startDepthMetres),
        camera.GetInvProjMatrix(), camera.GetProjMatrixNoJitter()};
    return graph.AddPass2(RenderPass::Main_SkyAtmosphereDebug, deps, {}, {},
        [this, renderer, data, showAerial](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto& D = renderer->GetDeferredForFrame();
            const auto point = ctx.usePoint ? *ctx.usePoint : 0u;
            if (showAerial)
            {
                ctx.Use(aerial_.Get(), kRest);
                // Opaque depth snapshot: forward ocean writes the live depth after compose.
                ctx.Use(D.depthCopy.Get(), kRest);
            }
            else for (const auto& resource : lut_) ctx.Use(resource.Get(), kRest);
            ctx.Use(D.scene.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint();
            const auto restore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            return [this, renderer, data, point, restore](RenderGraphPassContext c) {
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                renderer->EmitPoint(t.cl, point);
                const auto samplers = std::array{*SamplerManager::LinearClamp()};
                RecordComputeDispatch(renderer, t.cl, debugMaterial_.get(), render::kConstantBufferAlignment,
                    [&data](uint8_t* dst) { std::memcpy(dst, &data, sizeof(data)); },
                    {srv_[0], srv_[1], aerialSrv_, renderer->GetDeferredForFrame().depthCopySRV}, {renderer->GetDeferredForFrame().sceneUAV},
                    renderer->GetSamplerManager()->GetTable(renderer, samplers), data.width, data.height);
                renderer->EmitPoint(t.cl, restore);
                c.EndCL(t);
            };
        });
}

void SkyAtmosphere::ValidateReadback(UINT slot)
{
    if (!pending_[slot]) return;
    pending_[slot] = false;
    void* data = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(readbackBytes_)};
    if (FAILED(readback_[slot]->Map(0, &range, &data)) || !data)
    { LOG_ERROR(logging::LogCategory::Render, "sky atmosphere LUT validation: readback map failed"); return; }
    const auto read = [&](UINT lut, UINT x, UINT y, UINT channel) {
        const auto* row = static_cast<const uint8_t*>(data) + footprint_[lut].Offset + y * footprint_[lut].Footprint.RowPitch;
        return ReadHalf(reinterpret_cast<const std::uint16_t*>(row)[x * 4u + channel]);
    };
    unsigned bad = 0;
    float maxima[2]{};
    for (UINT lut = 0; lut < 2; ++lut)
        for (UINT y = 0; y < kHeight[lut]; ++y)
            for (UINT x = 0; x < kWidth[lut]; ++x)
                for (UINT channel = 0; channel < 3; ++channel)
                {
                    const float v = read(lut, x, y, channel);
                    if (!std::isfinite(v) || v < 0 || (lut == 0 && v > 1)) ++bad;
                    maxima[lut] = std::max(maxima[lut], v);
                }
    // Scalar double-precision UE reference, independent of GPU evaluation/half storage.
    // Transmittance: Common.ush:169-212 + usf:595-604, 10 samples at offset .3.
    // Multi: usf:1156-1268 default two vertical rays, 15 samples, five orders.
    // Use the GPU T LUT for the second reference, so a failure names the wrong stage.
    const auto& a = pendingParameters_[slot];
    const double bottom = a.radii[0], top = a.radii[1];
    const double H = std::sqrt(top * top - bottom * bottom);
    const auto medium = [&](double height, UINT ch, double& scattering, double& extinction) {
        const double h = std::max(0.0, height - bottom);
        const double ray = std::exp(a.radii[2] * h), mie = std::exp(a.radii[3] * h);
        const double ozone = std::clamp(h < a.ozone[3] ? a.ozoneDensity[0]*h + a.ozoneDensity[1]
            : a.ozoneDensity[2]*h + a.ozoneDensity[3], 0.0, 1.0);
        scattering = ray*a.rayleigh[ch] + mie*a.mieScattering[ch];
        extinction = scattering + mie*a.mieAbsorption[ch] + ozone*a.ozone[ch];
    };
    const auto sampleT = [&](double height, double mu, UINT ch) {
        const double rho = std::sqrt(std::max(0.0, height*height - bottom*bottom));
        const double d = std::max(0.0, -height*mu + std::sqrt(height*height*(mu*mu-1) + top*top));
        const double u = (d-(top-height))/(rho+H-(top-height)), v = rho/H;
        const double px = u*256-.5, py = v*64-.5;
        const int ix = static_cast<int>(std::floor(px)), iy = static_cast<int>(std::floor(py));
        const double fx = px-ix, fy = py-iy;
        const auto tap = [&](int x, int y) { return double(read(0, std::clamp(x,0,255), std::clamp(y,0,63), ch)); };
        return (tap(ix,iy)*(1-fx)+tap(ix+1,iy)*fx)*(1-fy)
             + (tap(ix,iy+1)*(1-fx)+tap(ix+1,iy+1)*fx)*fy;
    };
    double maxErrorT = 0, maxErrorMS = 0;
    for (UINT y=0; y<64; ++y)
        for (UINT x=0; x<256; ++x)
        {
            const double rho = H*(y+.5)/64, height = std::sqrt(rho*rho+bottom*bottom);
            const double d = top-height + (x+.5)/256*(rho+H-(top-height));
            const double mu = std::clamp((H*H-rho*rho-d*d)/(2*height*d), -1.0, 1.0);
            for (UINT ch=0; ch<3; ++ch)
            {
                double optical = 0;
                for (UINT i=0; i<10; ++i)
                {
                    const double t = d*(i+.3)/10;
                    const double sampleHeight = std::sqrt(height*height+2*height*mu*t+t*t);
                    double scattering, extinction; medium(sampleHeight,ch,scattering,extinction);
                    optical += extinction*d/10;
                }
                maxErrorT = std::max(maxErrorT, std::abs(read(0,x,y,ch)-std::exp(-optical)));
            }
        }
    for (UINT y=0; y<32; ++y)
        for (UINT x=0; x<32; ++x)
        {
            const double mu = (x+.5)/32*2-1, height = bottom+(y+.5)/32*(top-bottom);
            for (UINT ch=0; ch<3; ++ch)
            {
                double L[2]{}, r[2]{};
                for (int ray=0; ray<2; ++ray)
                {
                    const double sign = ray == 0 ? 1.0 : -1.0;
                    const double distance = ray == 0 ? top-height : height-bottom;
                    const double dt = distance/15;
                    double throughput = 1;
                    for (UINT i=0; i<15; ++i)
                    {
                        const double h = height+sign*distance*(i+.3)/15;
                        double scattering, extinction; medium(h,ch,scattering,extinction);
                        const double tr = std::exp(-extinction*dt);
                        const double shadowHeight = h-.001;
                        const bool shadow = mu < 0 && shadowHeight*shadowHeight*(mu*mu-1)+bottom*bottom >= 0;
                        const double S = shadow ? 0 : sampleT(h,mu,ch)*scattering/(4*3.14159265358979323846);
                        r[ray] += throughput*scattering*dt;
                        L[ray] += throughput*(S-S*tr)/std::max(extinction,1.e-9);
                        throughput *= tr;
                    }
                    if (ray == 1) L[ray] += sampleT(bottom,mu,ch)*throughput*std::max(0.0,mu)*a.groundAlbedo[ch]/3.14159265358979323846;
                }
                const double R = .5*(r[0]+r[1]), R2 = R*R;
                const double reference = .5*(L[0]+L[1])*(1+R+R2+R*R2+R2*R2)*a.groundAlbedo[3];
                maxErrorMS = std::max(maxErrorMS, std::abs(read(1,x,y,ch)-reference));
            }
        }
    // Float planet-radius cancellation + FP16 output. Absolute linear RGB errors, not screenshot percentages.
    if (maxErrorT > 0.002 || maxErrorMS > 0.002) ++bad;
    LOG_INFO(logging::LogCategory::Render, "sky atmosphere LUT CPU/GPU max abs error: T={:.7f} MS={:.7f}", maxErrorT, maxErrorMS);
    LOG_INFO(logging::LogCategory::Render,
        "sky atmosphere LUT GPU: ground zenith=({:.6f},{:.6f},{:.6f}) horizon=({:.6f},{:.6f},{:.6f}); max T={:.6f} MS={:.6f}",
        read(0,0,0,0), read(0,0,0,1), read(0,0,0,2), read(0,255,0,0), read(0,255,0,1), read(0,255,0,2), maxima[0], maxima[1]);
    if (bad) LOG_ERROR(logging::LogCategory::Render, "sky atmosphere LUT validation: FAIL ({} invalid components/reference checks)", bad);
    else LOG_INFO(logging::LogCategory::Render, "sky atmosphere LUT validation: PASS ({} finite nonnegative RGB texels)", 256*64+32*32);
    const D3D12_RANGE noWrite{0, 0}; readback_[slot]->Unmap(0, &noWrite);
}

// The skylight probes' lower hemisphere. UE's SkyLightComponent defaults (SkyLightComponent.cpp
// :304,312): bLowerHemisphereIsBlack = true, LowerHemisphereColor = Black, and the shader form is
// lerp(sky, rgb, a) -- so an opaque black replacement. RGBA, and the alpha IS the coverage.
// Applies to the diffuse and specular probes only; the captured radiance keeps the whole sphere.
// ...and WE DEPART FROM IT for the specular probe, filling the lower hemisphere with the ground's
// own outgoing radiance instead of black: `albedo * (sunOnGroundOverPi + skyOnGroundOverPi)`, the
// same expression `lighting_cs.hlsl`'s GroundBounceOverPi already uses for surfaces. UE's black is
// safe for them because the ground comes back through reflection captures and Lumen; here a
// near-mirror simply saw black below the horizon and went blotchy. The DIFFUSE probe keeps the
// black (the CPU hands it albedo 0), because that path adds GroundBounceOverPi separately and would
// otherwise count the ground twice. See SkyProbeSample in sky_ibl_filter_cs.hlsl for the measurement.
//
// `kLowerHemisphereColor` is gone with it: the replacement colour is no longer a constant, it is
// computed per rebuild from the sun, the sky and `groundAlbedo`.

// B4: fixed-size persistent resources, allocated before graph registration. No per-frame uploads.
void SkyAtmosphere::PrepareEnvironment(Renderer* renderer)
{
    if (environmentHeap_ || environmentFailed_) return;
    auto* device = renderer->GetDevice();
    const auto fail = [this](const char* what) {
        environmentFailed_ = true;
        LOG_ERROR(logging::LogCategory::Render, "sky environment: {} failed; retaining HDRI", what);
    };
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    // 1+1 view, 1+8 radiance (the sky picture, mipped for the fog's distance fade),
    // 1+8 specular, 1+1 irradiance.
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 22;
    if (FAILED(gpu::CreateDescriptorHeap(device, &hd, MemTag::Sky, L"SkyEnvironment.Heap", IID_PPV_ARGS(&environmentHeap_)))) { fail("heap"); return; }
    const auto base = environmentHeap_->GetCPUDescriptorHandleForHeapStart();
    const UINT step = device->GetDescriptorHandleIncrementSize(hd.Type);
    UINT descriptor = 0, output = 0;
    for (UINT i = 0; i < 4; ++i)
    {
        const bool view = i == 0;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = view ? 192 : (i == 3 ? 32 : 128);
        desc.Height = view ? 104 : static_cast<UINT>(desc.Width);
        desc.DepthOrArraySize = view ? 1 : 6; desc.MipLevels = (i == 1 || i == 2) ? 8 : 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        const wchar_t* names[] = {L"SkyEnvironment.View", L"SkyEnvironment.Radiance", L"SkyEnvironment.Specular", L"SkyEnvironment.Irradiance"};
        if (FAILED(gpu::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, desc, kRest, nullptr, MemTag::Sky, names[i], &resource)))
        { fail("texture"); return; }
        (view ? environmentView_ : environment_[i-1]).Attach(renderer->Declarations(), resource, kRest, names[i]);
        D3D12_CPU_DESCRIPTOR_HANDLE srv{base.ptr + descriptor++ * step};
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = desc.Format; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = view ? D3D12_SRV_DIMENSION_TEXTURE2D : D3D12_SRV_DIMENSION_TEXTURECUBE;
        if (view) sd.Texture2D.MipLevels = 1; else sd.TextureCube.MipLevels = desc.MipLevels;
        device->CreateShaderResourceView(resource.Get(), &sd, srv);
        if (view) environmentViewSrv_ = srv; else environmentSrv_[i-1] = srv;
        for (UINT mip = 0; mip < desc.MipLevels; ++mip)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE uav{base.ptr + descriptor++ * step};
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = desc.Format;
            ud.ViewDimension = view ? D3D12_UAV_DIMENSION_TEXTURE2D : D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            if (!view) { ud.Texture2DArray.MipSlice = mip; ud.Texture2DArray.ArraySize = 6; }
            device->CreateUnorderedAccessView(resource.Get(), nullptr, &ud, uav);
            if (view) environmentViewUav_ = uav; else environmentUav_[output++] = uav;
        }
    }
    Material::ComputeDesc cd{}; cd.csEntry = "CSMain";
    cd.shaderFile = L"shaders/sky_ibl_capture_cs.hlsl";
    captureMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
    cd.shaderFile = L"shaders/sky_ibl_filter_cs.hlsl";
    filterMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
    if (!captureMaterial_ || !captureMaterial_->GetPipelineState() || !filterMaterial_ || !filterMaterial_->GetPipelineState())
        fail("compute PSO");
}

size_t SkyAtmosphere::BuildEnvironment(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
    const SkyAtmosphereSettings& settings, SkyViewFrameData view, Skybox* sky, size_t luts,
    const SkyEnvironmentOverlay* overlay, const NightSkyFrameData& nightIn, size_t nightPass)
{
    constexpr size_t none = static_cast<size_t>(-1);
    if (sky) sky->SetEnvironment({}, {});
    if (!settings.mode || !settings.environmentLighting || failed_ || environmentFailed_ || !environmentHeap_ || !sky)
        return luts;
    // One global probe at sea level, independent of viewer position/yaw and auto exposure.
    // SkyView storage uses a fixed exposure; the capture decodes it into absolute radiance.
    view.planet[0] = settings.parameters.radii[0] + 0.001f;
    view.planet[3] = 1;
    view.sunDirection[3] = 0; // no solar disk in reflection captures (UE usf:314-317)
    // ...nor a lunar one (N1): the disc's fields go too, so the moon's phase alone cannot dirty the cube.
    view.light1Direction[3] = 0;
    view.light1Illuminance[3] = 0;
    std::fill(std::begin(view.moonDisc), std::end(view.moonDisc), 0.0f);
    std::fill(std::begin(view.moonDisc2), std::end(view.moonDisc2), 0.0f);
    view.exposure[0] = 1.0f / 1024.0f;
    view.exposure[1] = 1.0f; // the sky-only PICTURE factor never reaches the probes (and must not rebuild them)
    const auto params = settings.parameters;
    // Plan C4: the overlay (the clouds) rebuilds the environment on its own schedule -- and once
    // more when it appears or disappears, so no cube keeps clouds that are gone or lacks new ones.
    const bool overlayActive = overlay && overlay->active && overlay->material && overlay->writeConstants;
    // N2: the night sky as the picture holds it -- no twinkle (its clock and amount zeroed, so a still
    // sky does not rebuild every frame), the Milky Way only once its map exists.
    NightSkyFrameData night = nightIn;
    night.layers[2] = night.layers[3] = 0.0f;
    if (!nightReady_ || !night_.Get()) night.layers[1] = 0.0f;
    // P1: the moving bodies and their light within a tolerance of the last capture's (NearlySameView).
    const bool dirty = !environmentReady_ || std::memcmp(&params, &environmentParameters_, sizeof(params)) != 0
        || !NearlySameView(view, environmentKey_)
        || !NearlySameNight(night, environmentNightKey_)
        || (overlayActive && overlay->dirty) || overlayActive != environmentOverlayActive_;
    // Select descriptors before any serial pass builder/worker consumes the frame's environment.
    sky->SetEnvironment(environmentSrv_, {environment_[0].Get(), environment_[1].Get(), environment_[2].Get()});
    if (!dirty) return luts;
    RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>::DependencyList deps;
    if (luts != none) deps.push_back(luts);
    if (nightPass != none) deps.push_back(nightPass);
    if (overlayActive && overlay->dependency != none) deps.push_back(overlay->dependency);
    // Everything the pass reads, in environmentPass_ (see the member: closures copying it allocated every rebuild).
    EnvironmentPassFrame& pass = environmentPass_;
    pass.ov = overlayActive ? *overlay : SkyEnvironmentOverlay{};
    // The ground's outgoing radiance for the probes' lower hemisphere, solved HERE rather than in
    // the record body: it is a pure function of `params` + `view`, which are exactly the two things
    // the `dirty` test above keys on, so it changes once per rebuild and never per dispatch.
    // (It also cannot be solved in there -- the record lambda does not capture `settings`.)
    const float sinElevation = std::clamp(view.sunDirection[2], -1.0f, 1.0f);
    float sunTransmittance[3] = {1.0f, 1.0f, 1.0f};
    SkyTransmittanceTowardSun(params, std::asin(sinElevation), sunTransmittance);
    // The overlay's clouds can take the sun off the ground (an overcast deck, SkyEnvironmentOverlay).
    const float groundCosine = std::max(0.0f, sinElevation) * (overlayActive ? std::clamp(overlay->groundSunScale, 0.0f, 1.0f) : 1.0f);
    constexpr float kInvPi = 0.3183098861837907f;
    // w = the radiance cube's coarsest mip (1x1 per face), the sky's sphere average, which the
    // shader adds as the sky's half of the same ground irradiance.
    // N1: the moon lights the same ground, through its own air and the same deck.
    const float moonSinElevation = std::clamp(view.light1Direction[2], -1.0f, 1.0f);
    float moonTransmittance[3] = {0.0f, 0.0f, 0.0f};
    if (view.light1Illuminance[0] > 0.0f || view.light1Illuminance[1] > 0.0f || view.light1Illuminance[2] > 0.0f)
    {
        SkyTransmittanceTowardSun(params, std::asin(moonSinElevation), moonTransmittance);
    }
    const float moonCosine = std::max(0.0f, moonSinElevation) * (overlayActive ? std::clamp(overlay->groundSunScale, 0.0f, 1.0f) : 1.0f);
    pass.groundSun = {
        (view.illuminance[0] * sunTransmittance[0] * groundCosine + view.light1Illuminance[0] * moonTransmittance[0] * moonCosine) * kInvPi,
        (view.illuminance[1] * sunTransmittance[1] * groundCosine + view.light1Illuminance[1] * moonTransmittance[1] * moonCosine) * kInvPi,
        (view.illuminance[2] * sunTransmittance[2] * groundCosine + view.light1Illuminance[2] * moonTransmittance[2] * moonCosine) * kInvPi,
        7.0f};
    pass.groundAlbedo = {params.groundAlbedo[0], params.groundAlbedo[1], params.groundAlbedo[2]};
    pass.params = params;
    pass.view = view;
    pass.night = night;
    return graph.AddPass2(RenderPass::Main_SkyEnvironment, deps, {}, {},
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const EnvironmentPassFrame& pass = environmentPass_;
            const SkyEnvironmentOverlay& ov = pass.ov;
            const NightSkyFrameData& night = pass.night;
            std::array<std::uint32_t, 4> points{};
            points[0] = ctx.usePoint ? *ctx.usePoint : 0u;
            for (auto& r : lut_) ctx.Use(r.Get(), kRest);
            ctx.Use(environmentView_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint(); points[1] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(environmentView_.Get(), kRest);
            ctx.Use(environment_[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const bool nightMap = night.layers[1] > 0.0f;
            if (nightMap) ctx.Use(night_.Get(), kRest);
            for (auto* r : ov.reads) { if (r) ctx.Use(r, kRest); } // the overlay's inputs, read beside the capture
            ctx.NextPoint(); points[2] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(environment_[0].Get(), kRest);
            ctx.Use(environment_[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.Use(environment_[2].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint(); points[3] = ctx.usePoint ? *ctx.usePoint : 0u;
            for (auto& r : environment_) ctx.Use(r.Get(), kRest);
            environmentReady_ = true; environmentParameters_ = pass.params; environmentKey_ = pass.view; environmentNightKey_ = night;
            environmentOverlayActive_ = ov.material != nullptr;
            //LOG_DEBUG(logging::LogCategory::Render, "sky environment rebuild {}: 128 cube 8 mips, 8 GGX mips, 32 E/PI{}",
            //          ++environmentBuilds_, ov.material ? ", cloud overlay" : "");
            return [this, renderer, points, nightMap](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassSkyEnvironment);
                const EnvironmentPassFrame& pass = environmentPass_;
                const SkyAtmosphereParameters& params = pass.params;
                const SkyViewFrameData& view = pass.view;
                const SkyEnvironmentOverlay& ov = pass.ov;
                const NightSkyFrameData& night = pass.night;
                const std::array<float, 4>& groundSun = pass.groundSun;
                const std::array<float, 3>& groundAlbedo = pass.groundAlbedo;
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                {
                    GPU_SCOPE(t.cl, ProfilerScopes::kPassSkyEnvironment);
                    const auto sampler = renderer->GetSamplerManager()->GetTable(renderer, std::array{*SamplerManager::LinearClamp()});
                    renderer->EmitPoint(t.cl, points[0]);
                    RecordComputeDispatch(renderer, t.cl, viewMaterial_.get(), render::kConstantBufferAlignment,
                        [&params, &view](uint8_t* dst) { std::memcpy(dst, &params, sizeof(params)); std::memcpy(dst + sizeof(params), &view, sizeof(view)); },
                        {srv_[0], srv_[1]}, {environmentViewUav_}, sampler, 192, 104);
                    renderer->EmitPoint(t.cl, points[1]);
                    // The sky PICTURE: whole sphere, mips 0..7. Every mip is captured straight from
                    // the SkyView LUT with (128 / size) supersamples per axis, capped at 8 -- no mip
                    // reads another, so the chain is one barrier-free run. The top mip is the
                    // sphere average the fog fades to at short range (UE's NonDirectionalColor).
                    for (UINT mip = 0; mip < 8; ++mip)
                    {
                        const UINT size = 128u >> mip;
                        const UINT taps = (128u / size) < 8u ? (128u / size) : 8u;
                        // N2: the Milky Way's mip for this capture's sample spacing (90 degrees / (size x
                        // taps) against the map's 360 / 2048): log2(512 / (size x taps)), within its chain.
                        const float nightMip = std::clamp(std::log2(512.0f / static_cast<float>(size * taps)), 0.0f,
                                                          static_cast<float>(kNightMips - 1u));
                        const std::array<float, 28> capture{view.planet[0], view.planet[1], 1024.0f,
                            static_cast<float>(size), static_cast<float>(taps), view.planet[2], nightMip, 0.0f,
                            night.glow[0], night.glow[1], night.glow[2], night.glow[3],
                            night.layers[0], night.layers[1], night.layers[2], night.layers[3],
                            night.row0[0], night.row0[1], night.row0[2], night.row0[3],
                            night.row1[0], night.row1[1], night.row1[2], night.row1[3],
                            night.row2[0], night.row2[1], night.row2[2], night.row2[3]};
                        // The overlay below reads and rewrites the same mips: one UAV barrier after
                        // the last capture orders the two.
                        RecordComputeDispatch(renderer, t.cl, captureMaterial_.get(), render::kConstantBufferAlignment,
                            [&capture](uint8_t* dst) { std::memcpy(dst, capture.data(), sizeof(capture)); },
                            {environmentViewSrv_, srv_[0], nightMap ? nightSrv_ : (nightNullSrv_.ptr ? nightNullSrv_ : environmentViewSrv_)},
                            {environmentUav_[mip]}, sampler, size, size*6,
                            (ov.material && mip == 7u) ? environment_[0].Get() : nullptr);
                    }
                    // Plan C4: the overlay -- the clouds -- composited into every radiance mip in
                    // place, so the probes filtered next and every reader of the picture see them.
                    if (ov.material)
                    {
                        GPU_SCOPE(t.cl, ProfilerScopes::kCloudCapture);
                        const auto overlaySamplers = renderer->GetSamplerManager()->GetTable(renderer,
                            std::array{*SamplerManager::LinearWrap(), *SamplerManager::LinearClamp()});
                        for (UINT mip = 0; mip < 8; ++mip)
                        {
                            const UINT size = 128u >> mip;
                            RecordComputeDispatch(renderer, t.cl, ov.material, ov.cbBytes,
                                [&ov, &params, &view, size](uint8_t* dst) {
                                    ov.writeConstants(dst, size);
                                    std::memcpy(dst + ov.ownBytes, &params, sizeof(params));
                                    std::memcpy(dst + ov.ownBytes + sizeof(params), &view, sizeof(view));
                                },
                                {ov.srvs[0], ov.srvs[1], ov.srvs[2], ov.srvs[3], ov.srvs[4], srv_[0], srv_[1]},
                                {environmentUav_[mip]}, overlaySamplers, size, size*6);
                        }
                    }
                    renderer->EmitPoint(t.cl, points[2]);
                    // The LIGHTING PROBES, built from that picture with the lower-hemisphere policy
                    // applied per sample of the convolution (sky_ibl_filter_cs.hlsl). UE's default
                    // is an opaque black replacement (SkyLightComponent.cpp:304,312).
                    for (UINT mip = 0; mip < 9; ++mip)
                    {
                        const bool diffuse = mip == 8;
                        const UINT size = diffuse ? 32u : 128u >> mip;
                        struct FilterCB { UINT size, mip, mipCount, diffuse; float albedo[4]; float sun[4]; float source[4]; };
                        // ALBEDO 0 FOR THE DIFFUSE PROBE, which collapses the shader's expression to
                        // UE's opaque black: `lighting_cs.hlsl:336` adds `GroundBounceOverPi(N)` to
                        // the irradiance itself, so a ground term in that cube would be counted
                        // twice. The specular probe has no such second source -- that is the whole
                        // asymmetry this fixes.
                        const FilterCB filter{size, diffuse ? 0u : mip, 8u, diffuse ? 1u : 0u,
                            {diffuse ? 0.0f : groundAlbedo[0],
                             diffuse ? 0.0f : groundAlbedo[1],
                             diffuse ? 0.0f : groundAlbedo[2],
                             1.0f},
                            {groundSun[0], groundSun[1], groundSun[2], groundSun[3]},
                            // The SOURCE is environment_[0], the radiance capture: 128 with 8 mips
                            // (PrepareEnvironment). The filter picks a mip per sample from these.
                            {128.0f, 8.0f, 0.0f, 0.0f}};
                        RecordComputeDispatch(renderer, t.cl, filterMaterial_.get(), render::kConstantBufferAlignment,
                            [&filter](uint8_t* dst) { std::memcpy(dst, &filter, sizeof(filter)); },
                            {environmentSrv_[0]}, {environmentUav_[8 + mip]}, sampler, size, size*6);
                    }
                    renderer->EmitPoint(t.cl, points[3]);
                }
                c.EndCL(t);
            };
        });
}

void SkyAtmosphere::PrepareMoon(Renderer* renderer)
{
    if (moonHeap_ || moonFailed_) return;
    auto* device = renderer->GetDevice();
    const auto fail = [this](const char* what) {
        moonFailed_ = true;
        LOG_ERROR(logging::LogCategory::Render, "moon disc texture: {} failed; the disc is drawn as a smooth sphere", what);
    };
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1 + kMoonMips;
    if (FAILED(gpu::CreateDescriptorHeap(device, &hd, MemTag::Sky, L"SkyAtmosphere.MoonHeap", IID_PPV_ARGS(&moonHeap_)))) { fail("heap"); return; }
    const auto base = moonHeap_->GetCPUDescriptorHandleForHeapStart();
    const UINT step = device->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = kMoonSize; desc.DepthOrArraySize = 1; desc.MipLevels = kMoonMips;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (FAILED(gpu::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, desc, kRest, nullptr, MemTag::Sky, L"SkyAtmosphere.Moon", &resource)))
    { fail("texture"); return; }
    moon_.Attach(renderer->Declarations(), resource, kRest, L"SkyAtmosphere.Moon");
    moonSrv_ = base;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = desc.Format; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = kMoonMips;
    device->CreateShaderResourceView(resource.Get(), &sd, moonSrv_);
    for (UINT mip = 0; mip < kMoonMips; ++mip)
    {
        moonUav_[mip] = {base.ptr + (1u + mip) * step};
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = desc.Format; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; ud.Texture2D.MipSlice = mip;
        device->CreateUnorderedAccessView(resource.Get(), nullptr, &ud, moonUav_[mip]);
    }
    Material::ComputeDesc cd{}; cd.shaderFile = L"shaders/sky_moon_cs.hlsl"; cd.csEntry = "CSMain";
    moonMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
    if (!moonMaterial_ || !moonMaterial_->GetPipelineState()) { fail("PSO"); return; }
}

size_t SkyAtmosphere::BuildMoon(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph)
{
    constexpr size_t none = static_cast<size_t>(-1);
    if (moonReady_ || moonFailed_ || !moonMaterial_ || !moon_.Get()) return none;
    // Committed at registration, as the cloud noise is: the skybox registered later in THIS frame
    // must already know the texture is on its way (the graph orders it behind this pass).
    moonReady_ = true;
    LOG_INFO(logging::LogCategory::Render, "moon disc texture: generating {}^2, {} mips (procedural maria + craters)",
             kMoonSize, kMoonMips);
    return graph.AddPass2(RenderPass::Main_SkyMoon, {}, {}, {},
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto write = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(moon_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint();
            const auto restore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(moon_.Get(), kRest);
            return [this, renderer, write, restore](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassSkyMoon);
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                {
                    GPU_SCOPE(t.cl, ProfilerScopes::kPassSkyMoon);
                    renderer->EmitPoint(t.cl, write);
                    for (UINT mip = 0; mip < kMoonMips; ++mip)
                    {
                        const UINT size = kMoonSize >> mip;
                        const std::array<UINT, 4> cb{size, mip, 0x5EA7u, 0u};
                        // numthreads(8, 8, 1): the helper takes the thread counts.
                        RecordComputeDispatch(renderer, t.cl, moonMaterial_.get(), render::kConstantBufferAlignment,
                            [&cb](uint8_t* dst) { std::memcpy(dst, cb.data(), sizeof(cb)); },
                            {}, {moonUav_[mip]}, D3D12_GPU_DESCRIPTOR_HANDLE{}, std::max(size, 8u), std::max(size, 8u));
                    }
                    renderer->EmitPoint(t.cl, restore);
                }
                c.EndCL(t);
            };
        });
}

void SkyAtmosphere::PrepareNight(Renderer* renderer)
{
    if (nightHeap_ || nightFailed_) return;
    auto* device = renderer->GetDevice();
    const auto fail = [this](const char* what) {
        nightFailed_ = true;
        LOG_ERROR(logging::LogCategory::Render, "night sky: the Milky Way's map: {} failed; the night sky draws without it", what);
    };
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2 + kNightMips;
    if (FAILED(gpu::CreateDescriptorHeap(device, &hd, MemTag::Sky, L"SkyAtmosphere.NightSkyHeap", IID_PPV_ARGS(&nightHeap_)))) { fail("heap"); return; }
    const auto base = nightHeap_->GetCPUDescriptorHandleForHeapStart();
    const UINT step = device->GetDescriptorHandleIncrementSize(hd.Type);
    // R11G11B10: radiance only, 4 bytes a texel -- 8 MB at 2048 x 1024 where RGBA16F would be 16. Its
    // values are in kNightMapUnit (1e-4 cd/m2), which keeps them clear of its denormals.
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kNightWidth; desc.Height = kNightHeight; desc.DepthOrArraySize = 1; desc.MipLevels = kNightMips;
    desc.Format = DXGI_FORMAT_R11G11B10_FLOAT; desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (FAILED(gpu::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, desc, kRest, nullptr, MemTag::Sky, L"SkyAtmosphere.NightSky", &resource)))
    { fail("texture"); return; }
    night_.Attach(renderer->Declarations(), resource, kRest, L"SkyAtmosphere.NightSky");
    nightSrv_ = base;
    nightNullSrv_ = {base.ptr + step};
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = desc.Format; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = kNightMips;
    device->CreateShaderResourceView(resource.Get(), &sd, nightSrv_);
    sd.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(nullptr, &sd, nightNullSrv_);
    for (UINT mip = 0; mip < kNightMips; ++mip)
    {
        nightUav_[mip] = {base.ptr + (2u + mip) * step};
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = desc.Format; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; ud.Texture2D.MipSlice = mip;
        device->CreateUnorderedAccessView(resource.Get(), nullptr, &ud, nightUav_[mip]);
    }
    Material::ComputeDesc cd{}; cd.shaderFile = L"shaders/sky_night_cs.hlsl"; cd.csEntry = "CSMain";
    nightMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
    if (!nightMaterial_ || !nightMaterial_->GetPipelineState()) { fail("PSO"); return; }
}

size_t SkyAtmosphere::BuildNight(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph)
{
    constexpr size_t none = static_cast<size_t>(-1);
    if (nightReady_ || nightFailed_) return none;
    // Allocated by the next Prepare, not here: 8 MB that a level without a night never needs.
    if (!nightMaterial_ || !night_.Get()) { nightWanted_ = true; return none; }
    // Committed at registration, as the moon is: the readers registered later this frame are ordered
    // behind this pass.
    nightReady_ = true;
    LOG_INFO(logging::LogCategory::Render, "night sky: generating the Milky Way's map, {}x{}, {} mips",
             kNightWidth, kNightHeight, kNightMips);
    return graph.AddPass2(RenderPass::Main_SkyNight, {}, {}, {},
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto write = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(night_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint();
            const auto restore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(night_.Get(), kRest);
            return [this, renderer, write, restore](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassSkyNight);
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                {
                    GPU_SCOPE(t.cl, ProfilerScopes::kPassSkyNight);
                    renderer->EmitPoint(t.cl, write);
                    for (UINT mip = 0; mip < kNightMips; ++mip)
                    {
                        const UINT width = kNightWidth >> mip, height = kNightHeight >> mip;
                        // fBm octaves this mip can hold: the finest of six is ~1 texel at mip 0.
                        const std::array<UINT, 4> cb{width, height, mip < 6u ? 6u - mip : 1u, 0x9A1Au};
                        RecordComputeDispatch(renderer, t.cl, nightMaterial_.get(), render::kConstantBufferAlignment,
                            [&cb](uint8_t* dst) { std::memcpy(dst, cb.data(), sizeof(cb)); },
                            {}, {nightUav_[mip]}, D3D12_GPU_DESCRIPTOR_HANDLE{}, std::max(width, 8u), std::max(height, 8u));
                    }
                    renderer->EmitPoint(t.cl, restore);
                }
                c.EndCL(t);
            };
        });
}

void SkyAtmosphere::PrepareDistant(Renderer* renderer)
{
    if (distantHeap_ || distantFailed_) return;
    auto* device = renderer->GetDevice();
    const auto fail = [this](const char* what) {
        distantFailed_ = true;
        LOG_ERROR(logging::LogCategory::Render, "distant sky light: {} failed; retaining IBL fog ambient", what);
    };
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2;
    if (FAILED(gpu::CreateDescriptorHeap(device, &hd, MemTag::Sky, L"SkyAtmosphere.DistantSkyLightHeap", IID_PPV_ARGS(&distantHeap_)))) { fail("heap"); return; }
    distantSrv_ = distantHeap_->GetCPUDescriptorHandleForHeapStart();
    distantUav_ = {distantSrv_.ptr + device->GetDescriptorHandleIncrementSize(hd.Type)};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (FAILED(gpu::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, desc, kRest, nullptr, MemTag::Sky, L"SkyAtmosphere.DistantSkyLight", &resource)))
    { fail("texture"); return; }
    distant_.Attach(renderer->Declarations(), resource, kRest, L"SkyAtmosphere.DistantSkyLight");
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = desc.Format; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(resource.Get(), &sd, distantSrv_);
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = desc.Format; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(resource.Get(), nullptr, &ud, distantUav_);
    Material::ComputeDesc cd{}; cd.shaderFile = L"shaders/sky_lut_distant_cs.hlsl"; cd.csEntry = "CSMain";
    distantMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
    if (!distantMaterial_ || !distantMaterial_->GetPipelineState()) { fail("PSO"); return; }
    desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; hp.Type = D3D12_HEAP_TYPE_READBACK;
    for (auto& rb : distantReadback_)
    {
        if (FAILED(gpu::CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, MemTag::Sky, L"SkyAtmosphere.DistantSkyLightReadback", IID_PPV_ARGS(&rb)))) { fail("readback ring"); return; }
    }
}

size_t SkyAtmosphere::BuildDistant(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
    const SkyAtmosphereSettings& settings, SkyViewFrameData view, size_t after)
{
    // Always on in procedural mode: it is the volumetric fog's sky term (UE's DistantSkyLightLut), and the
    // checkbox that used to gate it switched between two sources of the SAME light (this LUT at 6 km vs
    // the probe of the same sky at sea level) -- measured 2026-09-11 as 0.5 % of pixels, a control that lied.
    distantActive_ = settings.mode && !failed_ && !distantFailed_ && distantMaterial_;
    if (!distantActive_) return after;
    view.planet[0] = settings.parameters.radii[0] + 6.0f;
    view.planet[3] = 1.0f; view.sunDirection[3] = 0.0f; view.exposure[0] = 1.0f;
    view.exposure[1] = 1.0f; // picture-only factor: not the fog's light, not a rebuild key
    // N1: the moon's disc is not light either -- only its direction and illuminance key a rebuild.
    view.light1Direction[3] = 0.0f; view.light1Illuminance[3] = 0.0f;
    std::fill(std::begin(view.moonDisc), std::end(view.moonDisc), 0.0f);
    std::fill(std::begin(view.moonDisc2), std::end(view.moonDisc2), 0.0f);
    const auto params = settings.parameters;
    // P1: the moving bodies and their light within a tolerance of the last build's (NearlySameView).
    if (distantReady_ && std::memcmp(&params, &distantParameters_, sizeof(params)) == 0
        && NearlySameView(view, distantKey_)) return after;
    RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>::DependencyList deps;
    if (after != static_cast<size_t>(-1)) deps.push_back(after);
    const bool validate = settings.lutValidate;
    return graph.AddPass2(RenderPass::Main_SkyDistant, deps, {}, {},
        [this, renderer, params, view, validate](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            std::array<std::uint32_t, 3> points{};
            points[0] = ctx.usePoint ? *ctx.usePoint : 0u;
            for (auto& r : lut_) ctx.Use(r.Get(), kRest);
            ctx.Use(distant_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint(); points[1] = ctx.usePoint ? *ctx.usePoint : 0u;
            if (validate) ctx.Use(distant_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            ctx.NextPoint(); points[2] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(distant_.Get(), kRest);
            const UINT slot = renderer->GetCurrentFrameIndex();
            distantParameters_ = params; distantKey_ = view; distantReady_ = true;
            distantPending_[slot] = validate;
            LOG_DEBUG(logging::LogCategory::Render, "distant sky light rebuild {}: 64 rays x 10 samples, 6 km", ++distantBuilds_);
            // The MAJOR revision (DistantMajorRevision): far from the last major key, or the first.
            {
                const auto& k = distantMajorKey_;
                const float cosTurn = view.sunDirection[0] * k.sunDirection[0] + view.sunDirection[1] * k.sunDirection[1] +
                                      view.sunDirection[2] * k.sunDirection[2];
                // N1: the moon counts by its SHARE of the light. The test is on the two lights' sum, so
                // a daytime moon (a millionth of the sun) cannot reset the fog's history by waxing; its
                // turn counts only while it carries a tenth of the light or more -- the night.
                const float cosMoonTurn = view.light1Direction[0] * k.light1Direction[0] +
                                          view.light1Direction[1] * k.light1Direction[1] +
                                          view.light1Direction[2] * k.light1Direction[2];
                bool lightChanged = false;
                bool moonMatters = false;
                for (int c = 0; c < 3; ++c)
                {
                    const float total = std::fabs(k.illuminance[c]) + std::fabs(k.light1Illuminance[c]);
                    const float change = std::fabs(view.illuminance[c] - k.illuminance[c]) +
                                         std::fabs(view.light1Illuminance[c] - k.light1Illuminance[c]);
                    lightChanged = lightChanged || change > 0.1f * std::max(total, 1.0e-6f);
                    moonMatters = moonMatters || std::fabs(view.light1Illuminance[c]) > 0.1f * std::max(
                        std::fabs(view.illuminance[c]) + std::fabs(view.light1Illuminance[c]), 1.0e-12f);
                }
                lightChanged = lightChanged || (moonMatters && cosMoonTurn < 0.99939083f);
                const bool major = !distantMajorValid_ ||
                    std::memcmp(&params, &distantMajorParameters_, sizeof(params)) != 0 ||
                    cosTurn < 0.99939083f /* cos 2 degrees */ || lightChanged ||
                    view.illuminance[3] != k.illuminance[3] ||
                    std::memcmp(view.planet, k.planet, sizeof(view.planet)) != 0;
                if (major)
                {
                    distantMajorParameters_ = params; distantMajorKey_ = view; distantMajorValid_ = true;
                    ++distantMajorBuilds_;
                }
            }
            return [this, renderer, params, view, validate, slot, points](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassSkyDistant);
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                {
                    GPU_SCOPE(t.cl, ProfilerScopes::kPassSkyDistant);
                    renderer->EmitPoint(t.cl, points[0]);
                    const auto sampler = renderer->GetSamplerManager()->GetTable(renderer, std::array{*SamplerManager::LinearClamp()});
                    RecordComputeDispatch(renderer, t.cl, distantMaterial_.get(), render::kConstantBufferAlignment,
                        [&params, &view](uint8_t* dst) { std::memcpy(dst, &params, sizeof(params)); std::memcpy(dst + sizeof(params), &view, sizeof(view)); },
                        {srv_[0], srv_[1]}, {distantUav_}, sampler, 8, 8);
                }
                renderer->EmitPoint(t.cl, points[1]);
                if (validate)
                {
                    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                    dst.pResource = distantReadback_[slot].Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
                    src.pResource = distant_.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    t.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                }
                renderer->EmitPoint(t.cl, points[2]); c.EndCL(t);
            };
        });
}

void SkyAtmosphere::ValidateDistant(UINT slot)
{
    if (!distantPending_[slot]) return;
    distantPending_[slot] = false;
    void* data = nullptr;
    const D3D12_RANGE read{0, sizeof(float)*4};
    if (FAILED(distantReadback_[slot]->Map(0, &read, &data)) || !data)
    { LOG_ERROR(logging::LogCategory::Render, "distant sky light validation: map failed"); return; }
    float value[4]; std::memcpy(value, data, sizeof(value));
    const D3D12_RANGE written{0, 0}; distantReadback_[slot]->Unmap(0, &written);
    const bool ok = std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2])
        && value[0] >= 0 && value[1] >= 0 && value[2] >= 0 && value[3] == 0;
    if (!ok) LOG_ERROR(logging::LogCategory::Render, "distant sky light validation FAIL: [{}, {}, {}, {}]", value[0], value[1], value[2], value[3]);
    else LOG_INFO(logging::LogCategory::Render, "distant sky light validation PASS: raw RGB [{:.6f}, {:.6f}, {:.6f}] at 6 km", value[0], value[1], value[2]);
}
