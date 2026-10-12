#include "rendering/lighting/VolumetricCloud.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "core/logging/Log.h"
#include "core/profiling/ProfilerScopes.h"
#include "materials/Material.h"
#include "materials/Texture2D.h"
#include "rendering/core/BarrierTranslation.h"
#include "rendering/core/ComputeDispatch.h"
#include "rendering/core/MemoryReport.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/TextureCreate.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/descriptors/SamplerManager.h"

namespace render
{
std::wstring g_cloudProfilePath;
std::wstring g_cloudFormationPath;
std::wstring g_cloudCellsPath;
bool g_cloudFormationOn = true;
}

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

// OUR profile LUT, authored to the Cloud_Profile semantics (cloud_common.hlsli CloudSampleProfile):
// u = the cloud type, 0 low and flat .. 1 the whole layer; row 0 = the TOP of the layer. Unlike
// UDS's, its density channel sits at 1 through the body, so the extinction scale keeps the meaning
// it has in the gradient model.
//   R: the envelope -- 0 on the layer floor, 1 from 4 % up through the lower third of the cloud's
//      height, then down to 0 at its top (top = 20 % of the layer for type 0 .. 100 % for type 1);
//   G: 0.6 at the base rising to 1 by 15 % (a soft underside), thinning over the top half;
//   B: erosion 0.15 at the base rising to 1 by 30 % of the cloud's height -- a firm, flat underside
//      under a cauliflower top.
constexpr UINT kProfileWidth = 64, kProfileHeight = 256;
std::vector<std::uint8_t> BuiltInCloudProfile()
{
    std::vector<std::uint8_t> px(static_cast<size_t>(kProfileWidth) * kProfileHeight * 4u);
    const auto byte = [](float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    for (UINT y = 0; y < kProfileHeight; ++y)
    {
        const float h = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(kProfileHeight);
        for (UINT x = 0; x < kProfileWidth; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kProfileWidth);
            const float top = 0.2f + 0.8f * u;
            const float envelope = std::min(h / 0.04f, (top - h) / (0.65f * top));
            const float density = std::min(0.6f + 0.4f * h / 0.15f, 1.0f - 0.3f * std::max(0.0f, (h - 0.5f * top) / (0.5f * top)));
            const float erosion = 0.15f + 0.85f * std::min(1.0f, h / (0.3f * top));
            std::uint8_t* p = &px[(static_cast<size_t>(y) * kProfileWidth + x) * 4u];
            p[0] = byte(envelope); p[1] = byte(density); p[2] = byte(erosion); p[3] = 255u;
        }
    }
    return px;
}

// A read-only texture (2D or 3D, one mip, 4 bytes a texel, tightly packed source rows) created in
// COPY_DEST, filled through an upload batch with a full GPU wait, and left at rest in NON_PIXEL --
// where the async compute queue the shadow map runs on can read it (Texture2D's uploads end in
// PIXEL|NON_PIXEL). Load-time only.
bool UploadToRest(Renderer* renderer, const D3D12_RESOURCE_DESC& desc, const std::uint8_t* texels,
                  Microsoft::WRL::ComPtr<ID3D12Resource>& out)
{
    auto* device = renderer->GetDevice();
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (FAILED(render::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, &resource)))
    {
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
    {
        return false;
    }
    std::uint8_t* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)))) { return false; }
    const size_t sourceRow = static_cast<size_t>(desc.Width) * 4u;
    const UINT depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? desc.DepthOrArraySize : 1u;
    for (UINT z = 0; z < depth; ++z)
    {
        for (UINT y = 0; y < desc.Height; ++y)
        {
            const size_t row = static_cast<size_t>(z) * desc.Height + y;
            std::memcpy(mapped + fp.Offset + row * fp.Footprint.RowPitch, texels + row * sourceRow, sourceRow);
        }
    }
    upload->Unmap(0, nullptr);

    renderer->WaitForPreviousFrame();
    UploadBatch batch;
    if (!batch.Begin(renderer)) { return false; }
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = resource.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    batch.CommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = kRest;
    barriers::EmitOne(batch.CommandList(), b);
    batch.KeepAlive()->push_back(upload);
    batch.SubmitAndWait(renderer);
    out = resource;
    return true;
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
    base_.Reset(); detail_.Reset(); weather_.Reset(); shadowRaw_.Reset(); shadowFiltered_.Reset(); profile_.Reset();
    formation_.Reset(); cells_.Reset();
    heap_.Reset();
    baseSrv_ = baseUav_ = detailSrv_ = detailUav_ = weatherSrv_ = weatherUav_ = profileSrv_ = formationSrv_ = cellsSrv_ = {};
    shadowRawSrv_ = shadowRawUav_ = shadowFilteredSrv_ = shadowFilteredUav_ = {};
    noiseBase_.reset(); noiseDetail_.reset(); noiseWeather_.reset(); trace_.reset(); temporal_.reset();
    shadowTrace_.reset(); shadowFilter_.reset(); capture_.reset();
    cbBytes_ = 0; captureCbBytes_ = 0; captureValid_ = false;
    initialized_ = failed_ = false;
    noiseReady_ = false; noiseSeed_ = 0; noiseBuilds_ = 0;
    shadowBuilt_ = false; shadowViewProj_ = {}; shadowFarKm_ = 1.0f;
    shadowHistoryValid_ = false; shadowSlicePhase_ = 0;
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
    hd.NumDescriptors = 13; // five textures x (SRV, UAV) + the profile LUT's, formation's and cells' SRVs
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
    if (!CreateProfile(renderer, handle())) { fail("profile LUT"); return; }
    // Optional test volumes: without both files the formation branch never runs.
    formationSrv_ = handle(); cellsSrv_ = handle();
    LoadVolumeDds(renderer, render::g_cloudFormationPath, formation_, formationSrv_, L"VolumetricCloud.Formation");
    LoadVolumeDds(renderer, render::g_cloudCellsPath, cells_, cellsSrv_, L"VolumetricCloud.Cells");

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
    capture_ = compute(L"shaders/cloud_capture_cs.hlsl", "CSMain");
    if (!noiseBase_ || !noiseDetail_ || !noiseWeather_ || !trace_ || !temporal_ || !shadowTrace_ || !shadowFilter_ || !capture_)
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
    // C4: the capture's buffer is the cloud blob with the sky's parameters and view appended
    // (cloud_common.hlsli CLOUD_WITH_SKY_CB); the reflection must agree with the three structs.
    const UINT captureBytes = capture_->GetCBSizeBytes(0);
    const UINT captureExpected = static_cast<UINT>(sizeof(VolumetricCloudConstants) + sizeof(SkyAtmosphereParameters) + sizeof(SkyViewFrameData));
    if (captureBytes != captureExpected)
    {
        LOG_ERROR(logging::LogCategory::Render, "volumetric clouds: the capture's CB is {} bytes in the shader, {} expected (cloud + sky params + sky view)",
                  captureBytes, captureExpected);
        fail("capture constant buffer mirror"); return;
    }
    captureCbBytes_ = capture_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment);
    initialized_ = true;
    LOG_INFO(logging::LogCategory::Render, "volumetric clouds: resources ready (base {}^3, detail {}^3, weather {}^2, shadow {}^2)",
             kBaseSize, kDetailSize, kWeatherSize, kShadowSize);
}

// The profile LUT: the file `--cloud-profile=` names (render::g_cloudProfilePath), else the built-in
// one. Uploaded here, once, with a full GPU wait (Prepare runs when a level first asks for clouds)
// and left at rest in NON_PIXEL: Texture2D's own upload ends in PIXEL|NON_PIXEL, which the async
// compute queue the shadow map runs on may not read from.
bool VolumetricCloud::CreateProfile(Renderer* renderer, D3D12_CPU_DESCRIPTOR_HANDLE srv)
{
    std::vector<std::uint8_t> pixels;
    UINT width = 0, height = 0;
    std::wstring source = L"built-in";
    if (!render::g_cloudProfilePath.empty())
    {
        if (Texture2D::DecodeFileRGBA8(render::g_cloudProfilePath, pixels, width, height) && width > 1 && height > 1)
        {
            source = render::g_cloudProfilePath;
        }
        else
        {
            LOG_WARNING(logging::LogCategory::Render, "volumetric clouds: profile LUT '{}' did not decode; the built-in LUT instead",
                        render::g_cloudProfilePath);
            pixels.clear();
        }
    }
    if (pixels.empty())
    {
        pixels = BuiltInCloudProfile();
        width = kProfileWidth; height = kProfileHeight;
    }

    auto* device = renderer->GetDevice();
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (!UploadToRest(renderer, desc, pixels.data(), resource)) { return false; }
    ownedBytes_ += device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    profile_.Attach(renderer->Declarations(), resource, kRest, L"VolumetricCloud.Profile");
    profileSrv_ = srv;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = desc.Format; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(resource.Get(), &sd, profileSrv_);
    profileInvSize_[0] = 1.0f / static_cast<float>(width);
    profileInvSize_[1] = 1.0f / static_cast<float>(height);
    LOG_INFO(logging::LogCategory::Render, "volumetric clouds: profile LUT {}x{} ({})", width, height, source);
    return true;
}

// The formation / cells volumes (render::g_cloudFormationPath, g_cloudCellsPath): an uncompressed
// 8-bit RGBA/BGRA volume DDS with the DX10 header -- what UE's exporter writes for a volume
// texture's source -- read whole and uploaded once. Anything else is refused with one warning and the
// clouds keep their own model.
bool VolumetricCloud::LoadVolumeDds(Renderer* renderer, const std::wstring& path, GpuResource& target,
                                    D3D12_CPU_DESCRIPTOR_HANDLE srv, const wchar_t* name)
{
    if (path.empty()) { return false; }
    const auto refuse = [&path](const char* why) {
        LOG_WARNING(logging::LogCategory::Render, "volumetric clouds: volume '{}' not used: {}", path, why);
        return false;
    };
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) { return refuse("cannot open"); }
    LARGE_INTEGER size{};
    std::vector<std::uint8_t> bytes;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 148 && size.QuadPart < (1ll << 31))
    {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) { bytes.clear(); }
    }
    CloseHandle(file);
    const auto u32 = [&bytes](size_t at) { std::uint32_t v = 0; std::memcpy(&v, bytes.data() + at, 4); return v; };
    if (bytes.empty() || std::memcmp(bytes.data(), "DDS ", 4) != 0 || std::memcmp(bytes.data() + 84, "DX10", 4) != 0)
    { return refuse("not a DX10 DDS"); }
    const UINT height = u32(12), width = u32(16), depth = u32(24);
    const std::uint32_t format = u32(128), dimension = u32(132);
    const DXGI_FORMAT dxgi = format == 87u ? DXGI_FORMAT_B8G8R8A8_UNORM : (format == 28u ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN);
    if (dimension != 4u || dxgi == DXGI_FORMAT_UNKNOWN || width < 2 || height < 2 || depth < 2 || depth > 2048)
    { return refuse("not an 8-bit RGBA volume"); }
    if (bytes.size() < 148ull + static_cast<std::uint64_t>(width) * height * depth * 4u) { return refuse("truncated"); }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = static_cast<UINT16>(depth); desc.MipLevels = 1;
    desc.Format = dxgi; desc.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (!UploadToRest(renderer, desc, bytes.data() + 148, resource)) { return refuse("upload failed"); }
    auto* device = renderer->GetDevice();
    ownedBytes_ += device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    target.Attach(renderer->Declarations(), resource, kRest, name);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = dxgi; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; sd.Texture3D.MipLevels = 1;
    device->CreateShaderResourceView(resource.Get(), &sd, srv);
    LOG_INFO(logging::LogCategory::Render, "volumetric clouds: volume {}x{}x{} ({})", width, height, depth, path);
    return true;
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
VolumetricCloudConstants VolumetricCloud::MakeConstants(const FrameInputs& in) const
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
    c.distortion[1] = 1.0f / (std::isfinite(s.shapeDistortionTileKm) ? std::clamp(s.shapeDistortionTileKm, 0.1f, 1000.0f) : 6.0f);
    c.distortion[0] = std::isfinite(s.weatherDistortionStrength) ? std::clamp(s.weatherDistortionStrength, 0.0f, 1.0f) : 0.0f;
    // The weather warp moves relative to the wind-carried base and detail fields.
    // Speed 0 keeps a static warp. All cloud passes use the same freezable clock.
    // Wrap each coordinate in double before upload to keep the periodic animation continuous.
    if (c.distortion[0] > 0.0f && s.shapeDistortionSpeed > 0.0f
        && std::isfinite(s.shapeDistortionSpeed) && std::isfinite(in.windTime))
    {
        const double travel = static_cast<double>(in.windTime) * std::min(s.shapeDistortionSpeed, 10.0f) / 60.0;
        constexpr double direction[3] = {0.80, 0.48, -0.36}; // unit length
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            const double offset = travel * direction[axis];
            c.distortionOffset[axis] = static_cast<float>(offset - std::floor(offset));
        }
    }
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
    c.shadowMap2[3] = std::clamp(s.overcast, 0.0f, 1.0f);
    c.profile[0] = (s.profile && profile_.Get() != nullptr) ? 1.0f : 0.0f;
    c.profile[1] = profileInvSize_[0]; c.profile[2] = profileInvSize_[1];
    // The formation's time axis advances with the DRIFT, as UDS's does with their cloud movement:
    // formationChangeSpeed full cycles per 16 formation tiles travelled (VolumetricCloudSettings.h).
    // In double, wrapped before upload: the drift grows without bound on a long session.
    constexpr double kTilesPerCycle = 16.0;
    const double driftKm = static_cast<double>(s.windKmH) * static_cast<double>(in.windTime) / 3600.0;
    const double cycles = static_cast<double>(std::max(s.formationChangeSpeed, 0.0f)) * driftKm
        / (static_cast<double>(std::max(s.weatherTileKm, 0.001f)) * kTilesPerCycle);
    c.formation[0] = FormationActive(s) ? 1.0f : 0.0f;
    c.formation[1] = std::isfinite(cycles) ? static_cast<float>(cycles - std::floor(cycles)) : 0.0f;
    c.formation[2] = std::clamp(s.macroVariation, 0.0f, 1.0f);
    c.formation[3] = 1.0f / std::max(s.macroTileKm, 0.1f);
    c.lighting[0] = static_cast<float>(std::min(s.msOctaves, 2u));
    return c;
}

// C4: UE's real-time sky light capture with clouds (VolumetricCloudRendering.cpp:2257,
// :2266-2280), in the shape of an overlay on the B4 environment (SkyAtmosphere::BuildEnvironment).
// The probe is sea level under the camera; the blob is the frame's, re-based on that probe with
// the exposure at 1 (the cube stores raw radiance) and the aerial perspective forced on (the
// capture integrates the LUTs itself, it has no camera volume to wait for).
SkyEnvironmentOverlay VolumetricCloud::MakeEnvironmentOverlay(Renderer* renderer, const FrameInputs& in,
                                                              size_t noisePass, unsigned distantRevision)
{
    SkyEnvironmentOverlay overlay{};
    if (!Ready() || !in.settings || !noiseReady_ || !capture_) { return overlay; }
    const VolumetricCloudSettings& s = *in.settings;
    const Math::float2 probeXZ(in.cameraPos.x, in.cameraPos.z);
    const unsigned long long frame = renderer->GetTotalFrameNumber();
    const bool formation = FormationActive(s);
    bool dirty = !captureValid_ || std::memcmp(&s, &captureSettings_, sizeof(s)) != 0
        || noiseBuilds_ != captureNoiseRevision_ || distantRevision != captureDistantRevision_
        || formation != captureFormation_;
    if (!dirty && frame >= captureFrame_ + kCaptureIntervalFrames)
    {
        const float dx = probeXZ.x - captureProbeXZ_.x, dz = probeXZ.y - captureProbeXZ_.y;
        dirty = (dx * dx + dz * dz) > kCaptureMoveMetres * kCaptureMoveMetres || in.windTime != captureWindTime_;
    }
    if (dirty)
    {
        captureValid_ = true; captureSettings_ = s; captureNoiseRevision_ = noiseBuilds_;
        captureDistantRevision_ = distantRevision; captureFrame_ = frame;
        captureProbeXZ_ = probeXZ; captureWindTime_ = in.windTime; captureFormation_ = formation;
    }

    FrameInputs probe = in;
    probe.cameraPos = Math::float3(in.cameraPos.x, 1.0f, in.cameraPos.z);
    probe.preExposure = 1.0f; probe.prevPreExposure = 1.0f;
    VolumetricCloudConstants constants = MakeConstants(probe);
    constants.aerial[0] = 1.0f;
    const bool distant = in.distantActive && in.distantResource && in.distantSrv.ptr != 0;

    overlay.active = true;
    overlay.dirty = dirty;
    overlay.material = capture_.get();
    overlay.cbBytes = captureCbBytes_;
    overlay.ownBytes = static_cast<UINT>(sizeof(VolumetricCloudConstants));
    overlay.writeConstants = [constants](uint8_t* dst, unsigned mipSize) mutable {
        constants.output[0] = constants.output[1] = static_cast<float>(mipSize);
        std::memcpy(dst, &constants, sizeof(constants));
    };
    overlay.reads = {formation ? formation_.Get() : base_.Get(), formation ? cells_.Get() : detail_.Get(), weather_.Get(),
                     distant ? in.distantResource : nullptr, profile_.Get()};
    overlay.srvs = {formation ? formationSrv_ : baseSrv_, formation ? cellsSrv_ : detailSrv_, weatherSrv_,
                    distant ? in.distantSrv : renderer->VsmDummyTexSrv(), profileSrv_};
    overlay.dependency = noisePass;
    return overlay;
}

size_t VolumetricCloud::BuildShadow(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                                    const FrameInputs& in, size_t after)
{
    shadowBuilt_ = false;
    render::g_cloudShadowStats.tracedTexels = 0;
    render::g_cloudShadowStats.totalTexels = kShadowSize * kShadowSize;
    if (!Ready() || !in.settings || !in.settings->shadowMap || !noiseReady_)
    {
        shadowHistoryValid_ = false;
        return kNone;
    }
    VolumetricCloudConstants constants = MakeConstants(in);

    // TIME SLICING (render::g_cloudShadowUpdateFrames; the rationale is at the switch). The raw map
    // persists, so a partial trace is only valid while everything the whole map was traced under is
    // unchanged. The key, in the order it is most likely to move:
    //   - the map's own matrix: the sun's direction and the snapped anchor under the camera -- every
    //     texel's ray is a different ray (a sun animated by the dev window re-traces every frame,
    //     which is simply the old cost, never a stale map);
    //   - the cloud settings (a byte copy: this struct is copied whole every frame), the noise, the
    //     planet;
    //   - the wind: the drift is the one thing slicing is ALLOWED to lag, and only while it moves
    //     less than a quarter texel a frame -- a jump (a level load resetting the clock, a shot
    //     series stepping it, a hitch at a gale) re-traces instead of smearing.
    // The filter pass still runs over the whole map every frame; it reads texels of mixed age.
    const std::uint32_t updateFrames = render::SanitizeCloudShadowUpdateFrames(render::g_cloudShadowUpdateFrames);
    const float texelMetres = 2000.0f * std::max(in.settings->shadowExtentKm, 0.001f) / static_cast<float>(kShadowSize);
    const float windDx = constants.wind[0] - shadowKeyWind_.x, windDz = constants.wind[2] - shadowKeyWind_.y;
    // The map march's cheap-density bits (render::CloudShadowCheapBits): part of the key, because a
    // slice traced with them beside texels traced without them would be two different maps.
    const std::uint32_t cheapBits = render::CloudShadowCheapBits();
    if (updateFrames != shadowKeyFrames_ || cheapBits != shadowKeyCheap_)
    {
        LOG_INFO(logging::LogCategory::Render, "cloud shadow map: {} of the texels re-traced per frame{}; weather {}, detail {}",
                 updateFrames == 1u ? "all" : (updateFrames == 4u ? "1/4" : "1/16"),
                 updateFrames == 1u ? " (UE)" : (updateFrames == 4u ? " (2x2 interleave)" : " (4x4 interleave)"),
                 (cheapBits & 1u) != 0u ? "bilinear" : "bicubic (as the view)",
                 (cheapBits & 2u) != 0u ? "mean" : "full (as the view)");
    }
    const char* forced = nullptr;
    if (!shadowHistoryValid_) { forced = "no history"; }
    else if (updateFrames != shadowKeyFrames_) { forced = "update mode changed"; }
    else if (cheapBits != shadowKeyCheap_) { forced = "density mode changed"; }
    else if (std::memcmp(constants.shadowViewProj, shadowKeyViewProj_, sizeof(shadowKeyViewProj_)) != 0) { forced = "map moved (sun or anchor)"; }
    else if (std::memcmp(in.settings, &shadowKeySettings_, sizeof(VolumetricCloudSettings)) != 0) { forced = "cloud settings"; }
    else if (noiseBuilds_ != shadowKeyNoise_) { forced = "noise rebuilt"; }
    else if (FormationActive(*in.settings) != shadowKeyFormation_) { forced = "formation volume switched"; }
    else if (in.planetRadiusKm != shadowKeyPlanetKm_) { forced = "planet radius"; }
    else if (windDx * windDx + windDz * windDz > 0.0625f * texelMetres * texelMetres) { forced = "wind jumped"; }

    // Interleave orders (ordered-dither ranks): consecutive frames land far apart in the cell, so a
    // half-finished cycle never leaves a contiguous stale block.
    static constexpr std::uint8_t kOrder2[4][2] = { {0, 0}, {1, 1}, {1, 0}, {0, 1} };
    static constexpr std::uint8_t kOrder4[16][2] = {
        {0, 0}, {2, 2}, {2, 0}, {0, 2}, {1, 1}, {3, 3}, {3, 1}, {1, 3},
        {1, 0}, {3, 2}, {3, 0}, {1, 2}, {0, 1}, {2, 3}, {2, 1}, {0, 3} };
    std::uint32_t stride = 1u, offsetX = 0u, offsetY = 0u;
    if (updateFrames > 1u && forced == nullptr)
    {
        const std::uint32_t phase = shadowSlicePhase_ % updateFrames;
        shadowSlicePhase_ = phase + 1u;
        stride = updateFrames == 16u ? 4u : 2u;
        offsetX = updateFrames == 16u ? kOrder4[phase][0] : kOrder2[phase][0];
        offsetY = updateFrames == 16u ? kOrder4[phase][1] : kOrder2[phase][1];
    }
    else
    {
        shadowSlicePhase_ = 0u;
        if (updateFrames > 1u)
        {
            ++render::g_cloudShadowStats.forcedFullTraces;
            render::g_cloudShadowStats.lastForcedReason = forced;
        }
    }
    constants.shadowMap3[0] = static_cast<float>(stride);
    constants.shadowMap3[1] = static_cast<float>(offsetX);
    constants.shadowMap3[2] = static_cast<float>(offsetY);
    constants.shadowMap3[3] = static_cast<float>(cheapBits);
    const UINT traceSize = kShadowSize / stride;
    render::g_cloudShadowStats.tracedTexels = traceSize * traceSize;

    shadowHistoryValid_ = true;
    std::memcpy(shadowKeyViewProj_, constants.shadowViewProj, sizeof(shadowKeyViewProj_));
    std::memcpy(&shadowKeySettings_, in.settings, sizeof(VolumetricCloudSettings));
    shadowKeyNoise_ = noiseBuilds_;
    shadowKeyFormation_ = FormationActive(*in.settings);
    shadowKeyPlanetKm_ = in.planetRadiusKm;
    shadowKeyFrames_ = updateFrames;
    shadowKeyCheap_ = cheapBits;
    shadowKeyWind_ = Math::float2(constants.wind[0], constants.wind[2]);

    RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>::DependencyList deps;
    if (after != kNone) { deps.push_back(after); }
    // ASYNC COMPUTE. Everything this pass registers is compute-legal (`kRest` here is NON_PIXEL)
    // and its consumers -- `Main_VolumetricFog` and `Main_Lighting` -- already name `pCloudShadow`
    // as an explicit prereq (SceneRenderer_Graph.cpp:1087,1133), so the cross-queue fence has a
    // graph edge to compile from. What sits between is the whole G-buffer and shadow block.
    //
    // THE HAND-OVER IS CROSS-FRAME, and that is what an ordinary audit misses: the first attempt
    // failed with the shadow map in 0x80 (PIXEL_SHADER_RESOURCE), left there by the OCEAN, which
    // samples it in a pixel shader (`Main_Transparent`, SceneRenderer_Graph.cpp:1785) and hands it
    // to the NEXT frame's write in that state -- and no compute acquire may begin from it.
    // `Main_TransparentFog` hands it back; see the note there.
    // The formation and cells volumes, when active, sit in the base and detail noises' slots.
    const bool formation = FormationActive(*in.settings);
    ID3D12Resource* const baseResource = formation ? formation_.Get() : base_.Get();
    ID3D12Resource* const detailResource = formation ? cells_.Get() : detail_.Get();
    const D3D12_CPU_DESCRIPTOR_HANDLE baseSrv = formation ? formationSrv_ : baseSrv_;
    const D3D12_CPU_DESCRIPTOR_HANDLE detailSrv = formation ? cellsSrv_ : detailSrv_;
    return graph.AddPass2(RenderPass::Main_CloudShadow, RenderQueue::AsyncCompute, deps, {}, {},
        [this, renderer, constants, traceSize, baseResource, detailResource, baseSrv, detailSrv](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            std::array<std::uint32_t, 3> points{};
            points[0] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(baseResource, kRest); ctx.Use(detailResource, kRest); ctx.Use(weather_.Get(), kRest);
            ctx.Use(profile_.Get(), kRest);
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
            return [this, renderer, constants, points, traceSize, baseSrv, detailSrv](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassCloudShadow);
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                {
                    GPU_SCOPE(t.cl, ProfilerScopes::kPassCloudShadow);
                    const auto sampler = renderer->GetSamplerManager()->GetTable(renderer, std::array{*SamplerManager::LinearWrap()});
                    const auto write = [&constants](uint8_t* dst) { std::memcpy(dst, &constants, sizeof(constants)); };
                    renderer->EmitPoint(t.cl, points[0]);
                    RecordComputeDispatch(renderer, t.cl, shadowTrace_.get(), cbBytes_, write,
                        {baseSrv, detailSrv, weatherSrv_, renderer->VsmDummyTexSrv(), profileSrv_}, {shadowRawUav_}, sampler, traceSize, traceSize);
                    renderer->EmitPoint(t.cl, points[1]);
                    RecordComputeDispatch(renderer, t.cl, shadowFilter_.get(), cbBytes_, write,
                        {baseSrv, detailSrv, weatherSrv_, shadowRawSrv_, profileSrv_}, {shadowFilteredUav_}, sampler, kShadowSize, kShadowSize);
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
    // The formation and cells volumes, when active, sit in the base and detail noises' slots.
    const bool formation = FormationActive(*in.settings);
    ID3D12Resource* const baseResource = formation ? formation_.Get() : base_.Get();
    ID3D12Resource* const detailResource = formation ? cells_.Get() : detail_.Get();
    const D3D12_CPU_DESCRIPTOR_HANDLE baseSrv = formation ? formationSrv_ : baseSrv_;
    const D3D12_CPU_DESCRIPTOR_HANDLE detailSrv = formation ? cellsSrv_ : detailSrv_;
    return graph.AddPass2(RenderPass::Main_CloudTrace, list, {}, {},
        [this, renderer, constants, aerialResource, distantResource, aerialSrv, distantSrv, baseResource, detailResource, baseSrv, detailSrv](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto& D = renderer->GetDeferredForFrame();
            const auto& P = renderer->GetDeferredForPrevFrame();
            std::array<std::uint32_t, 3> points{};
            points[0] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(baseResource, kRest); ctx.Use(detailResource, kRest); ctx.Use(weather_.Get(), kRest);
            ctx.Use(profile_.Get(), kRest);
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
            return [this, renderer, constants, aerialSrv, distantSrv, points, baseSrv, detailSrv](RenderGraphPassContext c) {
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
                            {baseSrv, detailSrv, weatherSrv_, DF.depthSRV, aerialSrv, distantSrv, profileSrv_},
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
