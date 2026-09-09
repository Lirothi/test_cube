#include "rendering/lighting/SkyAtmosphere.h"

#include "app/camera/Camera.h"
#include "rendering/lighting/Skybox.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "core/logging/Log.h"
#include "core/profiling/ProfilerScopes.h"
#include "materials/Material.h"
#include "rendering/core/ComputeDispatch.h"
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
}

SkyAtmosphere::SkyAtmosphere()
{
    render::RegisterMemoryProvider("sky.luts", [](const void* self) -> std::uint64_t {
        return static_cast<const SkyAtmosphere*>(self)->ownedBytes_;
    }, this);
}

SkyAtmosphere::~SkyAtmosphere()
{
    render::UnregisterMemoryProvider(this);
}

void SkyAtmosphere::Reset()
{
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
    builds_ = 0; ownedBytes_ = 0;
}

void SkyAtmosphere::Prepare(Renderer* renderer, const SkyAtmosphereSettings& settings)
{
    // BeginFrame already waited for THIS slot. Never map a different in-flight slot.
    ValidateReadback(renderer->GetCurrentFrameIndex());
    ValidateDistant(renderer->GetCurrentFrameIndex());
    if (settings.mode && settings.distantSkyLight && !failed_) PrepareDistant(renderer);
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
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) { fail("descriptor heap"); return; }
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
        if (FAILED(render::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, desc, kRest, nullptr, &resource)))
        { fail("texture"); return; }
        ownedBytes_ += device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
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
        if (FAILED(render::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, vd, kRest, nullptr, &resource)))
        { fail("SkyView texture"); return; }
        ownedBytes_ += device->GetResourceAllocationInfo(0, 1, &vd).SizeInBytes;
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
        if (FAILED(render::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, vd, kRest, nullptr, &resource)))
        { fail("AerialPerspective texture"); return; }
        ownedBytes_ += device->GetResourceAllocationInfo(0, 1, &vd).SizeInBytes;
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
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource)))) { fail("readback ring"); return; }
    ownedBytes_ += render::kFrameCount * device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
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
    const auto params = settings.parameters;
    return graph.AddPass2(RenderPass::Main_SkyView, deps, {}, {},
        [this, renderer, params, view](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto point = ctx.usePoint ? *ctx.usePoint : 0u;
            for (auto& resource : lut_) ctx.Use(resource.Get(), kRest);
            ctx.Use(skyView_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint();
            const auto restore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(skyView_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.Use(lut_[0].Get(), kRest | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            return [this, renderer, params, view, point, restore](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassSkyView);
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
    struct Constants
    {
        SkyAtmosphereParameters atmosphere;
        SkyViewFrameData sky;
        Math::mat4 invView, invProj;
        Math::float4 start;
    };
    static_assert(sizeof(Constants) == 320);
    const Constants data{settings.parameters, view, camera.GetInvViewMatrix(), camera.GetInvProjMatrixNoJitter(),
        Math::float4(std::max(0.0f, startDepthMetres) / kMetresPerKm, 0, 0, 0)};
    aerialBuilt_ = true;
    return graph.AddPass2(RenderPass::Main_SkyAerial, {after}, {}, {},
        [this, renderer, data](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto point = ctx.usePoint ? *ctx.usePoint : 0u;
            for (auto& resource : lut_) ctx.Use(resource.Get(), kRest);
            ctx.Use(aerial_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint();
            const auto restore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(aerial_.Get(), kRest);
            return [this, renderer, data, point, restore](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassSkyAerial);
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                {
                    GPU_SCOPE(t.cl, ProfilerScopes::kPassSkyAerial);
                    renderer->EmitPoint(t.cl, point);
                    const auto samplers = std::array{*SamplerManager::LinearClamp()};
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
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 15;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&environmentHeap_)))) { fail("heap"); return; }
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
        desc.DepthOrArraySize = view ? 1 : 6; desc.MipLevels = i == 2 ? 8 : 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        if (FAILED(render::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, desc, kRest, nullptr, &resource)))
        { fail("texture"); return; }
        ownedBytes_ += device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        const wchar_t* names[] = {L"SkyEnvironment.View", L"SkyEnvironment.Radiance", L"SkyEnvironment.Specular", L"SkyEnvironment.Irradiance"};
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
    const SkyAtmosphereSettings& settings, SkyViewFrameData view, Skybox* sky, size_t luts)
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
    view.exposure[0] = 1.0f / 1024.0f;
    const auto params = settings.parameters;
    const bool dirty = !environmentReady_ || std::memcmp(&params, &environmentParameters_, sizeof(params)) != 0
        || std::memcmp(&view, &environmentKey_, sizeof(view)) != 0;
    // Select descriptors before any serial pass builder/worker consumes the frame's environment.
    sky->SetEnvironment(environmentSrv_, {environment_[0].Get(), environment_[1].Get(), environment_[2].Get()});
    if (!dirty) return luts;
    RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>::DependencyList deps;
    if (luts != none) deps.push_back(luts);
    return graph.AddPass2(RenderPass::Main_SkyEnvironment, deps, {}, {},
        [this, renderer, params, view](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            std::array<std::uint32_t, 4> points{};
            points[0] = ctx.usePoint ? *ctx.usePoint : 0u;
            for (auto& r : lut_) ctx.Use(r.Get(), kRest);
            ctx.Use(environmentView_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint(); points[1] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(environmentView_.Get(), kRest);
            ctx.Use(environment_[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint(); points[2] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(environment_[0].Get(), kRest);
            ctx.Use(environment_[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.Use(environment_[2].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint(); points[3] = ctx.usePoint ? *ctx.usePoint : 0u;
            for (auto& r : environment_) ctx.Use(r.Get(), kRest);
            environmentReady_ = true; environmentParameters_ = params; environmentKey_ = view;
            LOG_DEBUG(logging::LogCategory::Render, "sky environment rebuild {}: 128 cube, 8 GGX mips, 32 E/PI", ++environmentBuilds_);
            return [this, renderer, params, view, points](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassSkyEnvironment);
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                {
                    GPU_SCOPE(t.cl, ProfilerScopes::kPassSkyEnvironment);
                    const auto sampler = renderer->GetSamplerManager()->GetTable(renderer, std::array{*SamplerManager::LinearClamp()});
                    renderer->EmitPoint(t.cl, points[0]);
                    RecordComputeDispatch(renderer, t.cl, viewMaterial_.get(), render::kConstantBufferAlignment,
                        [&params, &view](uint8_t* dst) { std::memcpy(dst, &params, sizeof(params)); std::memcpy(dst + sizeof(params), &view, sizeof(view)); },
                        {srv_[0], srv_[1]}, {environmentViewUav_}, sampler, 192, 104);
                    renderer->EmitPoint(t.cl, points[1]);
                    const std::array<float, 4> capture{view.planet[0], view.planet[1], 1024.0f, 128.0f};
                    RecordComputeDispatch(renderer, t.cl, captureMaterial_.get(), render::kConstantBufferAlignment,
                        [&capture](uint8_t* dst) { std::memcpy(dst, capture.data(), sizeof(capture)); },
                        {environmentViewSrv_}, {environmentUav_[0]}, sampler, 128, 128*6);
                    renderer->EmitPoint(t.cl, points[2]);
                    for (UINT mip = 0; mip < 9; ++mip)
                    {
                        const bool diffuse = mip == 8;
                        const UINT size = diffuse ? 32u : 128u >> mip;
                        const std::array<UINT, 4> filter{size, diffuse ? 0u : mip, 8u, diffuse ? 1u : 0u};
                        RecordComputeDispatch(renderer, t.cl, filterMaterial_.get(), render::kConstantBufferAlignment,
                            [&filter](uint8_t* dst) { std::memcpy(dst, filter.data(), sizeof(filter)); },
                            {environmentSrv_[0]}, {environmentUav_[1 + mip]}, sampler, size, size*6);
                    }
                    renderer->EmitPoint(t.cl, points[3]);
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
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&distantHeap_)))) { fail("heap"); return; }
    distantSrv_ = distantHeap_->GetCPUDescriptorHandleForHeapStart();
    distantUav_ = {distantSrv_.ptr + device->GetDescriptorHandleIncrementSize(hd.Type)};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (FAILED(render::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, desc, kRest, nullptr, &resource)))
    { fail("texture"); return; }
    distant_.Attach(renderer->Declarations(), resource, kRest, L"SkyAtmosphere.DistantSkyLight");
    ownedBytes_ += device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
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
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) { fail("readback ring"); return; }
        ownedBytes_ += device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    }
}

size_t SkyAtmosphere::BuildDistant(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
    const SkyAtmosphereSettings& settings, SkyViewFrameData view, size_t after)
{
    distantActive_ = settings.mode && settings.distantSkyLight && !failed_ && !distantFailed_ && distantMaterial_;
    if (!distantActive_) return after;
    view.planet[0] = settings.parameters.radii[0] + 6.0f;
    view.planet[3] = 1.0f; view.sunDirection[3] = 0.0f; view.exposure[0] = 1.0f;
    const auto params = settings.parameters;
    if (distantReady_ && std::memcmp(&params, &distantParameters_, sizeof(params)) == 0
        && std::memcmp(&view, &distantKey_, sizeof(view)) == 0) return after;
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
