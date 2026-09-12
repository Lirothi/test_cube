#include "rendering/lighting/VolumetricCloud.h"

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
constexpr auto kRest = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr size_t kNone = static_cast<size_t>(-1);

void CopyMat(float dst[16], const Math::mat4& m) { std::memcpy(dst, &m.m, sizeof(float) * 16); }

// RecordComputeDispatch's body with a 3D group count: the noise kernels are numthreads(8,8,1) over a
// volume, so the third dimension is real (the helper always dispatches z = 1).
template <typename WriteCBFn>
void Dispatch3D(Renderer* renderer, ID3D12GraphicsCommandList* cl, Material* material, UINT cbBytes,
                WriteCBFn&& write, std::initializer_list<D3D12_CPU_DESCRIPTOR_HANDLE> srvs,
                std::initializer_list<D3D12_CPU_DESCRIPTOR_HANDLE> uavs, D3D12_GPU_DESCRIPTOR_HANDLE samplers,
                UINT gx, UINT gy, UINT gz)
{
    auto h = renderer->GetRenderContextPool()->Acquire();
    RenderContext& rc = h.ref();
    if (cbBytes > 0)
    {
        auto cb = renderer->GetFrameResource()->AllocDynamic(cbBytes, render::kConstantBufferAlignment);
        write(static_cast<uint8_t*>(cb.cpu));
        rc.cbv[0] = cb.gpu;
    }
    if (srvs.size() > 0) { rc.srvTable[0] = renderer->StageSrvUavTable(srvs).gpu; }
    if (uavs.size() > 0) { rc.uavTable[0] = renderer->StageSrvUavTable(uavs).gpu; }
    rc.samplerTable[0] = samplers;
    material->Bind(cl, rc);
    if (gx > 0 && gy > 0 && gz > 0) { cl->Dispatch(gx, gy, gz); }
}
} // namespace

VolumetricCloud::VolumetricCloud()
{
    render::RegisterMemoryProvider("cloud.textures", [](const void* self) -> std::uint64_t {
        return static_cast<const VolumetricCloud*>(self)->ownedBytes_;
    }, this);
}

VolumetricCloud::~VolumetricCloud()
{
    render::UnregisterMemoryProvider(this);
}

void VolumetricCloud::Reset()
{
    base_.Reset(); detail_.Reset(); weather_.Reset(); shadowRaw_.Reset(); shadowFiltered_.Reset();
    heap_.Reset();
    baseSrv_ = baseUav_ = detailSrv_ = detailUav_ = weatherSrv_ = weatherUav_ = {};
    shadowRawSrv_ = shadowRawUav_ = shadowFilteredSrv_ = shadowFilteredUav_ = {};
    noiseBase_.reset(); noiseDetail_.reset(); noiseWeather_.reset(); trace_.reset(); temporal_.reset();
    shadowTrace_.reset(); shadowFilter_.reset();
    cbBytes_ = 0;
    initialized_ = failed_ = false;
    noiseReady_ = false; noiseSeed_ = 0; noiseBuilds_ = 0;
    shadowBuilt_ = false; shadowViewProj_ = {}; shadowFarKm_ = 1.0f;
    ownedBytes_ = 0;
}

// Fixed-size resources, allocated once the level asks for clouds and kept until Reset.
void VolumetricCloud::Prepare(Renderer* renderer, const VolumetricCloudSettings& settings)
{
    if (!settings.enabled || failed_ || heap_) { return; }
    auto* device = renderer->GetDevice();
    const auto fail = [this](const char* what) {
        LOG_ERROR(logging::LogCategory::Render, "volumetric clouds: {} failed; clouds off", what);
        failed_ = true;
    };
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 10;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) { fail("descriptor heap"); return; }
    const auto base = heap_->GetCPUDescriptorHandleForHeapStart();
    const UINT step = device->GetDescriptorHandleIncrementSize(hd.Type);
    UINT next = 0;
    const auto handle = [&]() { return D3D12_CPU_DESCRIPTOR_HANDLE{base.ptr + (next++) * step}; };

    // The three noise textures (C1) and the two shadow maps (C3). UNORM8 for the noises (Schneider's
    // textures are 8-bit and the density model remaps them), FP16 for the shadow's km / (1/m) / unitless.
    struct Tex { GpuResource* res; D3D12_CPU_DESCRIPTOR_HANDLE* srv; D3D12_CPU_DESCRIPTOR_HANDLE* uav;
                 UINT width, height, depth; DXGI_FORMAT format; const wchar_t* name; };
    const Tex textures[] = {
        {&base_, &baseSrv_, &baseUav_, kBaseSize, kBaseSize, kBaseSize, DXGI_FORMAT_R8G8B8A8_UNORM, L"VolumetricCloud.BaseNoise"},
        {&detail_, &detailSrv_, &detailUav_, kDetailSize, kDetailSize, kDetailSize, DXGI_FORMAT_R8G8B8A8_UNORM, L"VolumetricCloud.DetailNoise"},
        {&weather_, &weatherSrv_, &weatherUav_, kWeatherSize, kWeatherSize, 1, DXGI_FORMAT_R8G8B8A8_UNORM, L"VolumetricCloud.Weather"},
        {&shadowRaw_, &shadowRawSrv_, &shadowRawUav_, kShadowSize, kShadowSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, L"VolumetricCloud.ShadowRaw"},
        {&shadowFiltered_, &shadowFilteredSrv_, &shadowFilteredUav_, kShadowSize, kShadowSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, L"VolumetricCloud.Shadow"},
    };
    for (const Tex& t : textures)
    {
        const bool volume = t.depth > 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = t.width; desc.Height = t.height;
        desc.DepthOrArraySize = static_cast<UINT16>(t.depth); desc.MipLevels = 1;
        desc.Format = t.format; desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        if (FAILED(render::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, desc, kRest, nullptr, &resource)))
        { fail("texture"); return; }
        ownedBytes_ += device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        t.res->Attach(renderer->Declarations(), resource, kRest, t.name);
        *t.srv = handle(); *t.uav = handle();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = desc.Format; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = volume ? D3D12_SRV_DIMENSION_TEXTURE3D : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (volume) { sd.Texture3D.MipLevels = 1; } else { sd.Texture2D.MipLevels = 1; }
        device->CreateShaderResourceView(resource.Get(), &sd, *t.srv);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = desc.Format;
        ud.ViewDimension = volume ? D3D12_UAV_DIMENSION_TEXTURE3D : D3D12_UAV_DIMENSION_TEXTURE2D;
        if (volume) { ud.Texture3D.WSize = t.depth; }
        device->CreateUnorderedAccessView(resource.Get(), nullptr, &ud, *t.uav);
    }

    auto* mm = renderer->GetMaterialManager();
    const auto compute = [&](const wchar_t* file, const char* entry, bool plane = false) {
        Material::ComputeDesc cd{}; cd.shaderFile = file; cd.csEntry = entry;
        if (plane) { cd.defines.emplace_back("CLOUD_NOISE_2D", "1"); }
        auto m = mm->GetOrCreateCompute(renderer, cd);
        return (m && m->GetPipelineState()) ? m : nullptr;
    };
    noiseBase_ = compute(L"shaders/cloud_noise_cs.hlsl", "CSBase");
    noiseDetail_ = compute(L"shaders/cloud_noise_cs.hlsl", "CSDetail");
    noiseWeather_ = compute(L"shaders/cloud_noise_cs.hlsl", "CSWeather", true);
    trace_ = compute(L"shaders/cloud_trace_cs.hlsl", "CSMain");
    temporal_ = compute(L"shaders/cloud_temporal_cs.hlsl", "CSMain");
    shadowTrace_ = compute(L"shaders/cloud_shadow_cs.hlsl", "CSTrace");
    shadowFilter_ = compute(L"shaders/cloud_shadow_cs.hlsl", "CSFilter");
    if (!noiseBase_ || !noiseDetail_ || !noiseWeather_ || !trace_ || !temporal_ || !shadowTrace_ || !shadowFilter_)
    { fail("compute PSO"); return; }
    // The constant buffer is uploaded as one blob: the shader's layout must be the mirror's.
    const UINT cbBytes = trace_->GetCBSizeBytes(0);
    if (cbBytes != sizeof(VolumetricCloudConstants))
    {
        LOG_ERROR(logging::LogCategory::Render, "volumetric clouds: CloudCB is {} bytes in the shader, {} in VolumetricCloudConstants",
                  cbBytes, static_cast<unsigned>(sizeof(VolumetricCloudConstants)));
        fail("constant buffer mirror"); return;
    }
    cbBytes_ = trace_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment);
    initialized_ = true;
    LOG_INFO(logging::LogCategory::Render, "volumetric clouds: resources ready (base {}^3, detail {}^3, weather {}^2, shadow {}^2)",
             kBaseSize, kDetailSize, kWeatherSize, kShadowSize);
}

size_t VolumetricCloud::BuildNoise(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                                   const VolumetricCloudSettings& settings)
{
    if (!Ready() || (noiseReady_ && noiseSeed_ == settings.seed)) { return kNone; }
    const std::uint32_t seed = settings.seed;
    // Committed at registration (the frame's serial build, before any worker records): the shadow
    // and trace passes registered right after this one in the SAME frame must know the noise is
    // on its way, and the graph orders them behind this pass.
    noiseReady_ = true; noiseSeed_ = seed;
    LOG_INFO(logging::LogCategory::Render, "volumetric clouds: noise rebuild {} (seed {})", ++noiseBuilds_, seed);
    return graph.AddPass2(RenderPass::Main_CloudNoise, {},
        [this, renderer, seed](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto write = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(base_.Get(), kUav); ctx.Use(detail_.Get(), kUav); ctx.Use(weather_.Get(), kUav);
            ctx.NextPoint();
            const auto restore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(base_.Get(), kRest); ctx.Use(detail_.Get(), kRest); ctx.Use(weather_.Get(), kRest);
            return [this, renderer, seed, write, restore](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassCloudNoise);
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                {
                    GPU_SCOPE(t.cl, ProfilerScopes::kPassCloudNoise);
                    renderer->EmitPoint(t.cl, write);
                    const auto sampler = renderer->GetSamplerManager()->GetTable(renderer, std::array{*SamplerManager::LinearWrap()});
                    struct NoiseCB { UINT size, seed, pad0, pad1; };
                    const auto dispatch = [&](Material* m, UINT size, UINT depth, D3D12_CPU_DESCRIPTOR_HANDLE uav) {
                        const NoiseCB cb{size, seed, 0u, 0u};
                        Dispatch3D(renderer, t.cl, m, render::kConstantBufferAlignment,
                            [&cb](uint8_t* dst) { std::memcpy(dst, &cb, sizeof(cb)); }, {}, {uav}, sampler,
                            (size + 7u) / 8u, (size + 7u) / 8u, depth);
                    };
                    dispatch(noiseBase_.get(), kBaseSize, kBaseSize, baseUav_);
                    dispatch(noiseDetail_.get(), kDetailSize, kDetailSize, detailUav_);
                    dispatch(noiseWeather_.get(), kWeatherSize, 1u, weatherUav_);
                    renderer->EmitPoint(t.cl, restore);
                }
                c.EndCL(t);
            };
        });
}

// UE VolumetricCloudRendering.cpp:1725-1810 and the cvars beside it, in metres and our matrix
// convention (row vectors, mul(v, M); the engine's forward ortho, clip z 0 at the near plane).
VolumetricCloudConstants VolumetricCloud::MakeConstants(const FrameInputs& in)
{
    const VolumetricCloudSettings& s = *in.settings;
    VolumetricCloudConstants c{};
    CopyMat(c.invViewProjNoJitter, in.invViewProjNoJitter);
    CopyMat(c.viewProjNoJitter, in.viewProjNoJitter);
    CopyMat(c.prevViewProjNoJitter, in.prevViewProjNoJitter);

    // --- The shadow map's view (C3). The look-at is the ground under the camera (sea level, Y 0),
    // snapped to a world grid; the light sits 2 extents up the light direction and the far plane is
    // twice that (their LightDistance / FarPlane); the world origin is snapped to the texel grid.
    const float extentM = std::max(s.shadowExtentKm, 0.001f) * 1000.0f;
    const float lightDistance = extentM * 2.0f;
    const float farPlane = lightDistance * 2.0f;
    const Math::float3 lightDir = Math::Normalize(in.sunDirection);
    const float snap = std::max(s.shadowSnapKm, 0.001f) * 1000.0f;
    Math::float3 lookAt(in.cameraPos.x, 0.0f, in.cameraPos.z);
    lookAt.x = std::floor((lookAt.x + 0.5f * snap) / snap) * snap;
    lookAt.z = std::floor((lookAt.z + 0.5f * snap) / snap) * snap;
    const Math::float3 up = std::fabs(lightDir.y) > 0.99f ? Math::float3(1.0f, 0.0f, 0.0f) : Math::float3(0.0f, 1.0f, 0.0f);
    const Math::float3 lightPos = lookAt - lightDir * lightDistance;
    Math::mat4 shadowViewProj = Math::mat4::LookAtLH(lightPos, lookAt, up) * Math::mat4::OrthoLH(extentM * 2.0f, extentM * 2.0f, 0.0f, farPlane);
    {
        // SnapToPixelGrid (:1769-1793): the world origin lands on a texel, whatever the camera does.
        const Math::float4 originClip = shadowViewProj.Transform(Math::float4(0.0f, 0.0f, 0.0f, 1.0f));
        const float halfRes = static_cast<float>(kShadowSize) * 0.5f * 0.5f;
        const auto snapComponent = [halfRes](float v) { return std::round(v * halfRes) / halfRes; };
        const Math::float3 offset(snapComponent(originClip.x) - originClip.x, snapComponent(originClip.y) - originClip.y, 0.0f);
        shadowViewProj = shadowViewProj * Math::mat4::Translation(offset);
    }
    CopyMat(c.shadowViewProj, shadowViewProj);
    CopyMat(c.shadowInvViewProj, Math::mat4::Inverse(shadowViewProj));
    // :1805-1810: more samples towards the horizon, where each ray crosses far more layer.
    const float horizonFactor = std::clamp(0.2f / std::max(std::fabs(-lightDir.y), 1.0e-4f), 0.0f, 1.0f);
    const float baseCount = std::clamp(static_cast<float>(s.shadowMapSampleCount), 4.0f, 128.0f);
    const float shadowSamples = baseCount + (2.0f - 1.0f) * baseCount * horizonFactor;

    const float bottomRadius = in.planetRadiusKm + s.layerBottomKm;
    const float topRadius = bottomRadius + std::max(s.layerHeightKm, 0.001f);
    c.camera[0] = in.cameraPos.x; c.camera[1] = in.cameraPos.y; c.camera[2] = in.cameraPos.z; c.camera[3] = in.planetRadiusKm;
    c.layer[0] = bottomRadius; c.layer[1] = topRadius; c.layer[2] = 1.0f / (topRadius - bottomRadius); c.layer[3] = s.layerBottomKm;
    c.sun[0] = -lightDir.x; c.sun[1] = -lightDir.y; c.sun[2] = -lightDir.z; c.sun[3] = s.phaseG;
    c.sunColor[0] = in.sunIlluminance.x; c.sunColor[1] = in.sunIlluminance.y; c.sunColor[2] = in.sunIlluminance.z; c.sunColor[3] = s.phaseG2;
    c.phase[0] = s.phaseBlend; c.phase[1] = s.msContribution; c.phase[2] = s.msOcclusion; c.phase[3] = s.msEccentricity;
    c.trace[0] = static_cast<float>(std::max(s.viewSampleCountMax, 1u));
    c.trace[1] = static_cast<float>(std::max(s.viewSampleCountMin, 1u));
    c.trace[2] = 1.0f / std::max(s.distanceToSampleCountMaxKm, 0.001f);
    c.trace[3] = s.stopTracingTransmittance;
    c.shadowTrace[0] = static_cast<float>(std::max(s.shadowSampleCount, 1u));
    c.shadowTrace[1] = s.shadowTracingDistanceKm;
    c.shadowTrace[2] = 1.0f - std::clamp(s.skyLightBottomOcclusion, 0.0f, 1.0f); // UE SkyLightCloudBottomVisibility
    c.shadowTrace[3] = s.tracingMaxDistanceKm;
    c.medium[0] = s.extinctionScale; c.medium[1] = s.albedo; c.medium[2] = s.coverage; c.medium[3] = s.cloudType;
    c.shape[0] = 1.0f / std::max(s.baseTileKm, 0.001f); c.shape[1] = 1.0f / std::max(s.detailTileKm, 0.001f);
    c.shape[2] = 1.0f / std::max(s.weatherTileKm, 0.001f); c.shape[3] = s.detailStrength;
    // The drift: the level's wind heading at the cloud's own speed, on the shared (freezable) clock.
    // The field moves WITH the wind, so the sample point moves against it.
    const float driftM = s.windKmH / 3.6f * in.windTime;
    c.wind[0] = -in.windDirXZ.x * driftM; c.wind[1] = 0.0f; c.wind[2] = -in.windDirXZ.y * driftM; c.wind[3] = s.tracingStartMaxDistanceKm;
    c.output[0] = static_cast<float>(in.outputWidth); c.output[1] = static_cast<float>(in.outputHeight);
    c.output[2] = in.preExposure; c.output[3] = 1.0f / std::max(in.preExposure, 1.0e-8f);
    c.depth[0] = static_cast<float>(in.depthWidth); c.depth[1] = static_cast<float>(in.depthHeight);
    c.depth[2] = in.projZ33; c.depth[3] = in.projZ43;
    c.aerial[0] = in.aerialBuilt ? 1.0f : 0.0f; c.aerial[1] = in.aerialStartKm;
    c.aerial[2] = in.distantActive ? 1.0f : 0.0f; c.aerial[3] = static_cast<float>(in.debugView);
    c.temporal[0] = in.historyValid ? 1.0f : 0.0f; c.temporal[1] = s.temporal ? std::clamp(s.historyWeight, 0.0f, 0.99f) : 0.0f;
    c.temporal[2] = in.prevPreExposure > 0.0f ? in.preExposure / in.prevPreExposure : 1.0f;
    c.temporal[3] = static_cast<float>(in.frameIndex & 1023u);
    c.shadowMap[0] = static_cast<float>(kShadowSize); c.shadowMap[1] = 1.0f / static_cast<float>(kShadowSize);
    c.shadowMap[2] = farPlane / 1000.0f; c.shadowMap[3] = s.shadowStrength;
    c.shadowMap2[0] = shadowSamples; c.shadowMap2[1] = s.shadowDepthBiasKm;
    // The base noise's vertical compression: a tile may span at most `baseVerticalTiles` layer
    // heights (never stretched the other way -- a thick layer keeps the isotropic noise).
    c.shadowMap2[2] = std::max(1.0f, s.baseTileKm / std::max(s.layerHeightKm * s.baseVerticalTiles, 1.0e-3f));
    c.shadowMap2[3] = 0.0f;
    return c;
}

size_t VolumetricCloud::BuildShadow(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                                    const FrameInputs& in, size_t after)
{
    shadowBuilt_ = false;
    if (!Ready() || !in.settings || !in.settings->shadowMap || !noiseReady_) { return kNone; }
    const VolumetricCloudConstants constants = MakeConstants(in);
    RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>::DependencyList deps;
    if (after != kNone) { deps.push_back(after); }
    return graph.AddPass2(RenderPass::Main_CloudShadow, deps, {}, {},
        [this, renderer, constants](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            std::array<std::uint32_t, 3> points{};
            points[0] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(base_.Get(), kRest); ctx.Use(detail_.Get(), kRest); ctx.Use(weather_.Get(), kRest);
            ctx.Use(shadowRaw_.Get(), kUav);
            ctx.NextPoint(); points[1] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(shadowRaw_.Get(), kRest);
            ctx.Use(shadowFiltered_.Get(), kUav);
            ctx.NextPoint(); points[2] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(shadowFiltered_.Get(), kRest);
            // Serial builder: what the consumers registered after this read.
            shadowBuilt_ = true;
            std::memcpy(&shadowViewProj_.m, constants.shadowViewProj, sizeof(float) * 16);
            shadowFarKm_ = constants.shadowMap[2];
            return [this, renderer, constants, points](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassCloudShadow);
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                {
                    GPU_SCOPE(t.cl, ProfilerScopes::kPassCloudShadow);
                    const auto sampler = renderer->GetSamplerManager()->GetTable(renderer, std::array{*SamplerManager::LinearWrap()});
                    const auto write = [&constants](uint8_t* dst) { std::memcpy(dst, &constants, sizeof(constants)); };
                    renderer->EmitPoint(t.cl, points[0]);
                    RecordComputeDispatch(renderer, t.cl, shadowTrace_.get(), cbBytes_, write,
                        {baseSrv_, detailSrv_, weatherSrv_, renderer->VsmDummyTexSrv()}, {shadowRawUav_}, sampler, kShadowSize, kShadowSize);
                    renderer->EmitPoint(t.cl, points[1]);
                    RecordComputeDispatch(renderer, t.cl, shadowFilter_.get(), cbBytes_, write,
                        {baseSrv_, detailSrv_, weatherSrv_, shadowRawSrv_}, {shadowFilteredUav_}, sampler, kShadowSize, kShadowSize);
                    renderer->EmitPoint(t.cl, points[2]);
                }
                c.EndCL(t);
            };
        });
}

size_t VolumetricCloud::BuildTrace(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                                   const FrameInputs& in, const std::array<size_t, 4>& deps, size_t depCount)
{
    if (!Ready() || !in.settings || !noiseReady_) { return kNone; }
    const VolumetricCloudConstants constants = MakeConstants(in);
    RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>::DependencyList list;
    for (size_t i = 0; i < depCount && i < deps.size(); ++i) { if (deps[i] != kNone) { list.push_back(deps[i]); } }
    const bool aerial = in.aerialBuilt && in.aerialResource && in.aerialSrv.ptr != 0;
    const bool distant = in.distantActive && in.distantResource && in.distantSrv.ptr != 0;
    ID3D12Resource* aerialResource = aerial ? in.aerialResource : nullptr;
    ID3D12Resource* distantResource = distant ? in.distantResource : nullptr;
    const D3D12_CPU_DESCRIPTOR_HANDLE aerialSrv = aerial ? in.aerialSrv : renderer->VsmDummyTexSrv();
    const D3D12_CPU_DESCRIPTOR_HANDLE distantSrv = distant ? in.distantSrv : renderer->VsmDummyTexSrv();
    return graph.AddPass2(RenderPass::Main_CloudTrace, list, {}, {},
        [this, renderer, constants, aerialResource, distantResource, aerialSrv, distantSrv](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto& D = renderer->GetDeferredForFrame();
            const auto& P = renderer->GetDeferredForPrevFrame();
            std::array<std::uint32_t, 3> points{};
            points[0] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(base_.Get(), kRest); ctx.Use(detail_.Get(), kRest); ctx.Use(weather_.Get(), kRest);
            // The sky's volume and distant light, at their rest; declared so the reads are named.
            if (aerialResource) { ctx.Use(aerialResource, kRest); }
            if (distantResource) { ctx.Use(distantResource, kRest); }
            // The depth as every other reader takes it (both shader-visible states, one barrier).
            ctx.Use(D.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ctx.Use(D.cloudTrace.Get(), kUav); ctx.Use(D.cloudTraceDepth.Get(), kUav);
            ctx.NextPoint(); points[1] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(D.cloudTrace.Get(), kRest); ctx.Use(D.cloudTraceDepth.Get(), kRest);
            ctx.Use(P.cloudResolved.Get(), kRest); ctx.Use(P.cloudResolvedDepth.Get(), kRest); // the history, at rest
            ctx.Use(D.cloudResolved.Get(), kUav); ctx.Use(D.cloudResolvedDepth.Get(), kUav);
            ctx.NextPoint(); points[2] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(D.cloudResolved.Get(), kRest); ctx.Use(D.cloudResolvedDepth.Get(), kRest);
            return [this, renderer, constants, aerialSrv, distantSrv, points](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassCloudTrace);
                const auto& DF = renderer->GetDeferredForFrame();
                const auto& PF = renderer->GetDeferredForPrevFrame();
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                {
                    GPU_SCOPE(t.cl, ProfilerScopes::kPassCloudTrace);
                    const auto write = [&constants](uint8_t* dst) { std::memcpy(dst, &constants, sizeof(constants)); };
                    renderer->EmitPoint(t.cl, points[0]);
                    {
                        GPU_SCOPE(t.cl, ProfilerScopes::kCloudTraceMarch);
                        const auto samplers = renderer->GetSamplerManager()->GetTable(renderer,
                            std::array{*SamplerManager::LinearWrap(), *SamplerManager::LinearClamp()});
                        RecordComputeDispatch(renderer, t.cl, trace_.get(), cbBytes_, write,
                            {baseSrv_, detailSrv_, weatherSrv_, DF.depthSRV, aerialSrv, distantSrv},
                            {DF.cloudTraceUAV, DF.cloudTraceDepthUAV}, samplers, DF.cloudWidth, DF.cloudHeight);
                    }
                    renderer->EmitPoint(t.cl, points[1]);
                    {
                        GPU_SCOPE(t.cl, ProfilerScopes::kCloudTraceTemporal);
                        const auto samplers = renderer->GetSamplerManager()->GetTable(renderer,
                            std::array{*SamplerManager::LinearClamp(), *SamplerManager::PointClamp()});
                        RecordComputeDispatch(renderer, t.cl, temporal_.get(), cbBytes_, write,
                            {DF.cloudTraceSRV, DF.cloudTraceDepthSRV, PF.cloudResolvedSRV, PF.cloudResolvedDepthSRV},
                            {DF.cloudResolvedUAV, DF.cloudResolvedDepthUAV}, samplers, DF.cloudWidth, DF.cloudHeight);
                    }
                    renderer->EmitPoint(t.cl, points[2]);
                }
                c.EndCL(t);
            };
        });
}
