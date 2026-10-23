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
#include "rendering/core/GpuAlloc.h"
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
std::wstring g_cloudDumpDir;
float g_cloudComposeJitterScale = 1.0f;
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

// A texture the noise passes write (2D or 3D, one mip, UAV-capable), created at rest in NON_PIXEL,
// with its SRV and UAV written into the two given descriptors.
bool CreateStorageTexture(Renderer* renderer, GpuResource& target, D3D12_CPU_DESCRIPTOR_HANDLE srv,
                          D3D12_CPU_DESCRIPTOR_HANDLE uav, UINT width, UINT height, UINT depth, DXGI_FORMAT format,
                          const wchar_t* name)
{
    auto* device = renderer->GetDevice();
    const bool volume = depth > 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = static_cast<UINT16>(depth); desc.MipLevels = 1;
    desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (FAILED(gpu::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, desc, kRest, nullptr, MemTag::Cloud, name, &resource))) { return false; }
    target.Attach(renderer->Declarations(), resource, kRest, name);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = format; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.ViewDimension = volume ? D3D12_SRV_DIMENSION_TEXTURE3D : D3D12_SRV_DIMENSION_TEXTURE2D;
    if (volume) { sd.Texture3D.MipLevels = 1; } else { sd.Texture2D.MipLevels = 1; }
    device->CreateShaderResourceView(resource.Get(), &sd, srv);
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = format;
    ud.ViewDimension = volume ? D3D12_UAV_DIMENSION_TEXTURE3D : D3D12_UAV_DIMENSION_TEXTURE2D;
    if (volume) { ud.Texture3D.WSize = depth; }
    device->CreateUnorderedAccessView(resource.Get(), nullptr, &ud, uav);
    return true;
}

// A read-only texture (2D or 3D, one mip, 4 bytes a texel -- 1 for R8 -- tightly packed source rows) created in
// COPY_DEST, filled through an upload batch with a full GPU wait, and left at rest in NON_PIXEL --
// where the async compute queue the shadow map runs on can read it (Texture2D's uploads end in
// PIXEL|NON_PIXEL). Load-time only.
bool UploadToRest(Renderer* renderer, const D3D12_RESOURCE_DESC& desc, const std::uint8_t* texels,
                  const wchar_t* name, Microsoft::WRL::ComPtr<ID3D12Resource>& out)
{
    auto* device = renderer->GetDevice();
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (FAILED(gpu::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, MemTag::Cloud, name, &resource)))
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
    if (FAILED(gpu::CreateCommittedResource(device, &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, MemTag::Cloud, L"VolumetricCloud.UploadStaging", IID_PPV_ARGS(&upload))))
    {
        return false;
    }
    std::uint8_t* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)))) { return false; }
    const size_t sourceRow = static_cast<size_t>(desc.Width) * (desc.Format == DXGI_FORMAT_R8_UNORM ? 1u : 4u);
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

// The inverse, for render::g_cloudDumpDir: an 8-bit RGBA texture at rest in NON_PIXEL copied out
// through a readback buffer (full GPU wait) into an uncompressed DX10 DDS -- the layout
// VolumetricCloud::LoadVolumeDds reads. Dev-time only.
bool ReadbackToDds(Renderer* renderer, ID3D12Resource* texture, const std::wstring& path)
{
    auto* device = renderer->GetDevice();
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if (FAILED(gpu::CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, MemTag::Cloud, L"VolumetricCloud.DumpReadback", IID_PPV_ARGS(&readback))))
    {
        return false;
    }
    renderer->WaitForPreviousFrame();
    UploadBatch batch;
    if (!batch.Begin(renderer)) { return false; }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = texture;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = kRest; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers::EmitOne(batch.CommandList(), b);
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = texture; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    batch.CommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    barriers::EmitOne(batch.CommandList(), b);
    batch.KeepAlive()->push_back(readback);
    batch.SubmitAndWait(renderer);

    const UINT width = static_cast<UINT>(desc.Width), height = desc.Height;
    const UINT depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? desc.DepthOrArraySize : 1u;
    std::vector<std::uint8_t> file(148 + static_cast<size_t>(width) * height * depth * 4u, 0u);
    const auto put = [&file](size_t at, std::uint32_t v) { std::memcpy(file.data() + at, &v, 4); };
    std::memcpy(file.data(), "DDS ", 4);
    put(4, 124u);
    put(8, 0x1u | 0x2u | 0x4u | 0x1000u | (depth > 1 ? 0x800000u : 0u)); // CAPS HEIGHT WIDTH PIXELFORMAT [DEPTH]
    put(12, height); put(16, width); put(20, width * 4u); put(24, depth); put(28, 1u);
    put(76, 32u); put(80, 0x4u); std::memcpy(file.data() + 84, "DX10", 4);
    put(108, 0x1000u); put(112, depth > 1 ? 0x200000u : 0u);
    put(128, 28u); put(132, depth > 1 ? 4u : 3u); put(140, 1u); // R8G8B8A8_UNORM, TEXTURE3D / 2D, one
    std::uint8_t* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(total)};
    if (FAILED(readback->Map(0, &readRange, reinterpret_cast<void**>(&mapped)))) { return false; }
    const size_t row = static_cast<size_t>(width) * 4u;
    for (UINT z = 0; z < depth; ++z)
    {
        for (UINT y = 0; y < height; ++y)
        {
            const size_t index = static_cast<size_t>(z) * height + y;
            std::memcpy(file.data() + 148 + index * row, mapped + fp.Offset + index * fp.Footprint.RowPitch, row);
        }
    }
    const D3D12_RANGE noWrite{0, 0};
    readback->Unmap(0, &noWrite);
    HANDLE out = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) { return false; }
    DWORD written = 0;
    const bool ok = WriteFile(out, file.data(), static_cast<DWORD>(file.size()), &written, nullptr) && written == file.size();
    CloseHandle(out);
    return ok;
}
} // namespace

// The bytes these textures hold are the `cloud` tag of the GPU census (rendering/core/GpuAlloc.h);
// the "cloud.textures" column of the memory line it replaced matched it by name (memory profiling
// plan M1c).
VolumetricCloud::VolumetricCloud() = default;

VolumetricCloud::~VolumetricCloud() = default;

void VolumetricCloud::Reset()
{
    weather_.Reset(); shadowRaw_.Reset(); shadowFiltered_.Reset(); profile_.Reset();
    formation_.Reset(); cells_.Reset(); profileGen_.Reset(); formationGen_.Reset(); cellsGen_.Reset();
    blueNoise_.Reset(); blueNoiseSrv_ = {};
    heap_.Reset();
    weatherSrv_ = weatherUav_ = profileSrv_ = formationSrv_ = cellsSrv_ = {};
    profileGenSrv_ = profileGenUav_ = formationGenSrv_ = formationGenUav_ = cellsGenSrv_ = cellsGenUav_ = {};
    profileInvSize_[0] = 1.0f / kProfileWidth; profileInvSize_[1] = 1.0f / kProfileHeight;
    shadowRawSrv_ = shadowRawUav_ = shadowFilteredSrv_ = shadowFilteredUav_ = {};
    noiseWeather_.reset(); trace_.reset(); temporal_.reset();
    noiseProfile_.reset(); noiseFormation_.reset(); noiseCells_.reset();
    shadowTrace_.reset(); shadowFilter_.reset(); capture_.reset();
    cbBytes_ = 0; captureCbBytes_ = 0; captureValid_ = false;
    initialized_ = failed_ = false;
    noiseReady_ = false; noiseSeed_ = 0; noiseBuilds_ = 0;
    profileGenReady_ = formationGenReady_ = false;
    dumpArmed_ = dumpDone_ = false; dumpArmedFrame_ = 0;
    std::memset(profileGenKey_, 0, sizeof(profileGenKey_)); formationGenSeed_ = 0; formationGenRoughness_ = 0.0f;
    shadowBuilt_ = false; shadowViewProj_ = {}; shadowFarKm_ = 1.0f;
    shadowHistoryValid_ = false; shadowSlicePhase_ = 0;
}

// Fixed-size resources, allocated once the level asks for clouds and kept until Reset.
void VolumetricCloud::Prepare(Renderer* renderer, const VolumetricCloudSettings& settings)
{
    if (!settings.enabled || failed_) { return; }
    if (!heap_) { CreateResources(renderer); }
    if (dumpArmed_ && !dumpDone_ && renderer->GetTotalFrameNumber() > dumpArmedFrame_) { DumpGenerated(renderer); }
}

// render::g_cloudDumpDir: the generated textures, read back and written (the formation and cells only
// when files did not replace them -- those are not ours to dump).
void VolumetricCloud::DumpGenerated(Renderer* renderer)
{
    dumpArmed_ = false;
    const std::wstring& dir = render::g_cloudDumpDir;
    CreateDirectoryW(dir.c_str(), nullptr);
    const auto dump = [&](bool ready, ID3D12Resource* texture, const wchar_t* file) {
        if (!ready || !texture) { return; }
        const std::wstring path = dir + L"/" + file;
        if (ReadbackToDds(renderer, texture, path)) { LOG_INFO(logging::LogCategory::Render, "volumetric clouds: dumped {}", path); }
        else { LOG_WARNING(logging::LogCategory::Render, "volumetric clouds: dump of {} failed", path); }
    };
    dump(profileGenReady_, profileGen_.Get(), L"profile.dds");
    dump(formationGenReady_, formationGen_.Get(), L"formation.dds");
    dump(formationGenReady_, cellsGen_.Get(), L"cells.dds");
    dumpDone_ = true;
}

void VolumetricCloud::CreateResources(Renderer* renderer)
{
    auto* device = renderer->GetDevice();
    const auto fail = [this](const char* what) {
        LOG_ERROR(logging::LogCategory::Render, "volumetric clouds: {} failed; clouds off", what);
        failed_ = true;
    };
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    // Six written textures x (SRV, UAV) + the three file overrides' SRVs + the blue-noise mask's.
    hd.NumDescriptors = 16;
    if (FAILED(gpu::CreateDescriptorHeap(device, &hd, MemTag::Cloud, L"VolumetricCloud.Heap", IID_PPV_ARGS(&heap_)))) { fail("descriptor heap"); return; }
    const auto base = heap_->GetCPUDescriptorHandleForHeapStart();
    const UINT step = device->GetDescriptorHandleIncrementSize(hd.Type);
    UINT next = 0;
    const auto handle = [&]() { return D3D12_CPU_DESCRIPTOR_HANDLE{base.ptr + (next++) * step}; };

    // The file overrides first (render::g_cloud*Path), for an A/B against another set: the formation
    // and cells only as a pair, and a file that does not load leaves the generated texture in its
    // place. A pair that loaded makes the generated volumes unnecessary (33 MB not allocated).
    profileSrv_ = handle(); formationSrv_ = handle(); cellsSrv_ = handle();
    CreateProfile(renderer, profileSrv_);
    LoadVolumeDds(renderer, render::g_cloudFormationPath, formation_, formationSrv_, L"VolumetricCloud.FormationFile");
    LoadVolumeDds(renderer, render::g_cloudCellsPath, cells_, cellsSrv_, L"VolumetricCloud.CellsFile");
    const bool generateVolumes = !FormationFromFiles();
    // UE's view-march start offset: a spatiotemporal blue-noise mask (tools/gen_stbn.py; cloud_trace_cs.hlsl
    // CloudStartNoise). Missing, the trace keeps its own interleaved-gradient + golden-ratio noise.
    blueNoiseSrv_ = handle();
    if (!LoadVolumeDds(renderer, kBlueNoisePath, blueNoise_, blueNoiseSrv_, L"VolumetricCloud.BlueNoise"))
    {
        LOG_WARNING(logging::LogCategory::Render, "volumetric clouds: no blue-noise mask; the march start offset falls back to "
                    "interleaved gradient noise (python tools/gen_stbn.py writes it)");
    }

    // The generated textures (C1), the two shadow maps (C3). UNORM8 for the generated ones (the
    // density model remaps them), FP16 for the shadow's km / (1/m) / unitless.
    struct Tex { GpuResource* res; D3D12_CPU_DESCRIPTOR_HANDLE* srv; D3D12_CPU_DESCRIPTOR_HANDLE* uav;
                 UINT width, height, depth; DXGI_FORMAT format; const wchar_t* name; bool wanted; };
    const Tex textures[] = {
        {&weather_, &weatherSrv_, &weatherUav_, kWeatherSize, kWeatherSize, 1, DXGI_FORMAT_R8G8B8A8_UNORM, L"VolumetricCloud.Weather", true},
        {&shadowRaw_, &shadowRawSrv_, &shadowRawUav_, kShadowSize, kShadowSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, L"VolumetricCloud.ShadowRaw", true},
        {&shadowFiltered_, &shadowFilteredSrv_, &shadowFilteredUav_, kShadowSize, kShadowSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, L"VolumetricCloud.Shadow", true},
        {&profileGen_, &profileGenSrv_, &profileGenUav_, kProfileWidth, kProfileHeight, 1, DXGI_FORMAT_R8G8B8A8_UNORM, L"VolumetricCloud.Profile", true},
        {&formationGen_, &formationGenSrv_, &formationGenUav_, kFormationSize, kFormationSize, kFormationDepth, DXGI_FORMAT_R8G8B8A8_UNORM, L"VolumetricCloud.Formation", generateVolumes},
        {&cellsGen_, &cellsGenSrv_, &cellsGenUav_, kCellsSize, kCellsSize, kCellsSize, DXGI_FORMAT_R8G8B8A8_UNORM, L"VolumetricCloud.Cells", generateVolumes},
    };
    for (const Tex& t : textures)
    {
        *t.srv = handle(); *t.uav = handle();
        if (t.wanted && !CreateStorageTexture(renderer, *t.res, *t.srv, *t.uav, t.width, t.height, t.depth, t.format, t.name))
        { fail("texture"); return; }
    }

    auto* mm = renderer->GetMaterialManager();
    const auto compute = [&](const wchar_t* file, const char* entry, bool plane = false) {
        Material::ComputeDesc cd{}; cd.shaderFile = file; cd.csEntry = entry;
        if (plane) { cd.defines.emplace_back("CLOUD_NOISE_2D", "1"); }
        auto m = mm->GetOrCreateCompute(renderer, cd);
        return (m && m->GetPipelineState()) ? m : nullptr;
    };
    noiseWeather_ = compute(L"shaders/cloud_noise_cs.hlsl", "CSWeather", true);
    noiseProfile_ = compute(L"shaders/cloud_noise_cs.hlsl", "CSProfile", true);
    noiseFormation_ = compute(L"shaders/cloud_noise_cs.hlsl", "CSFormation");
    noiseCells_ = compute(L"shaders/cloud_noise_cs.hlsl", "CSCells");
    trace_ = compute(L"shaders/cloud_trace_cs.hlsl", "CSMain");
    temporal_ = compute(L"shaders/cloud_temporal_cs.hlsl", "CSMain");
    shadowTrace_ = compute(L"shaders/cloud_shadow_cs.hlsl", "CSTrace");
    shadowFilter_ = compute(L"shaders/cloud_shadow_cs.hlsl", "CSFilter");
    capture_ = compute(L"shaders/cloud_capture_cs.hlsl", "CSMain");
    if (!noiseWeather_ || !noiseProfile_ || !noiseFormation_ || !noiseCells_
        || !trace_ || !temporal_ || !shadowTrace_ || !shadowFilter_ || !capture_)
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
    LOG_INFO(logging::LogCategory::Render, "volumetric clouds: resources ready (formation {}x{}x{}{}, cells {}^3, profile {}x{}, weather {}^2, shadow {}^2)",
             kFormationSize, kFormationSize, kFormationDepth, generateVolumes ? "" : " from files", kCellsSize,
             kProfileWidth, kProfileHeight, kWeatherSize, kShadowSize);
}

// The profile LUT file `--cloud-profile=` names (render::g_cloudProfilePath), if any; without one
// (or one that does not decode) the generated LUT stays in use. Uploaded here, once, with a full GPU
// wait (Prepare runs when a level first asks for clouds) and left at rest in NON_PIXEL: Texture2D's
// own upload ends in PIXEL|NON_PIXEL, which the async compute queue the shadow map runs on may not
// read from.
bool VolumetricCloud::CreateProfile(Renderer* renderer, D3D12_CPU_DESCRIPTOR_HANDLE srv)
{
    if (render::g_cloudProfilePath.empty()) { return false; }
    std::vector<std::uint8_t> pixels;
    UINT width = 0, height = 0;
    if (!Texture2D::DecodeFileRGBA8(render::g_cloudProfilePath, pixels, width, height) || width < 2 || height < 2)
    {
        LOG_WARNING(logging::LogCategory::Render, "volumetric clouds: profile LUT '{}' did not decode; the generated LUT instead",
                    render::g_cloudProfilePath);
        return false;
    }
    const std::wstring& source = render::g_cloudProfilePath;

    auto* device = renderer->GetDevice();
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (!UploadToRest(renderer, desc, pixels.data(), L"VolumetricCloud.ProfileFile", resource)) { return false; }
    profile_.Attach(renderer->Declarations(), resource, kRest, L"VolumetricCloud.ProfileFile");
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
    // 8-bit RGBA / BGRA (the density volumes UE's exporter writes) or 8-bit R (tools/gen_stbn.py's mask).
    const DXGI_FORMAT dxgi = format == 87u ? DXGI_FORMAT_B8G8R8A8_UNORM
                           : (format == 28u ? DXGI_FORMAT_R8G8B8A8_UNORM : (format == 61u ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_UNKNOWN));
    const std::uint32_t texelBytes = dxgi == DXGI_FORMAT_R8_UNORM ? 1u : 4u;
    if (dimension != 4u || dxgi == DXGI_FORMAT_UNKNOWN || width < 2 || height < 2 || depth < 2 || depth > 2048)
    { return refuse("not an 8-bit RGBA or R volume"); }
    if (bytes.size() < 148ull + static_cast<std::uint64_t>(width) * height * depth * texelBytes) { return refuse("truncated"); }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = static_cast<UINT16>(depth); desc.MipLevels = 1;
    desc.Format = dxgi; desc.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (!UploadToRest(renderer, desc, bytes.data() + 148, name, resource)) { return refuse("upload failed"); }
    auto* device = renderer->GetDevice();
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
    if (!Ready()) { return kNone; }
    const VolumetricCloudSettings& s = settings;
    const std::uint32_t seed = s.seed;
    // Three independent keys: the seed for the weather map; the seed and the roughness for the
    // formation and cells volumes (unless files replaced them); the profile knobs for the LUT (64 x
    // 256, cheap enough to follow a slider frame by frame). A file override is never regenerated.
    const float profileKey[10] = {s.profileHeight, s.profileDome, s.profileTopSoftness, s.profileBaseSoftness,
                                  s.profileSmallDensity, s.profileBodyDensity, s.profileBaseErosion, s.profileErosionRise,
                                  s.profileHeavyErosionCut, s.profileHeavyThinning};
    const bool noise = !noiseReady_ || noiseSeed_ != seed;
    const bool profile = !ProfileFromFile() && (!profileGenReady_ || std::memcmp(profileKey, profileGenKey_, sizeof(profileKey)) != 0);
    const bool formation = !FormationFromFiles()
        && (!formationGenReady_ || formationGenSeed_ != seed || formationGenRoughness_ != s.formationRoughness);
    if (!noise && !profile && !formation) { return kNone; }
    // Committed at registration (the frame's serial build, before any worker records): the shadow
    // and trace passes registered right after this one in the SAME frame must know the textures are
    // on their way, and the graph orders them behind this pass.
    const bool firstFormation = formation && !formationGenReady_;
    noiseReady_ = true; noiseSeed_ = seed;
    if (profile) { profileGenReady_ = true; std::memcpy(profileGenKey_, profileKey, sizeof(profileKey)); }
    if (formation) { formationGenReady_ = true; formationGenSeed_ = seed; formationGenRoughness_ = s.formationRoughness; }
    ++noiseBuilds_;
    if (!render::g_cloudDumpDir.empty() && !dumpDone_)
    {
        dumpArmed_ = true; dumpArmedFrame_ = renderer->GetTotalFrameNumber();
    }
    // A slider drag regenerates every frame: those rebuilds are Debug, the ones a level load makes Info.
    if (noise || firstFormation)
    {
        LOG_INFO(logging::LogCategory::Render, "volumetric clouds: noise rebuild {} (seed {}):{}{}{}", noiseBuilds_, seed,
                 noise ? " weather" : "", formation ? " formation+cells" : "", profile ? " profile" : "");
    }
    else
    {
        LOG_DEBUG(logging::LogCategory::Render, "volumetric clouds: noise rebuild {} (seed {}):{}{}", noiseBuilds_, seed,
                  formation ? " formation+cells" : "", profile ? " profile" : "");
    }

    struct NoiseCB
    {
        UINT size, seed, depth, pad;
        float formationParams[4];
        float profileA[4], profileB[4], profileC[4];
    };
    NoiseCB params{};
    params.seed = seed;
    params.formationParams[0] = std::clamp(s.formationRoughness, 0.3f, 0.9f);
    params.profileA[0] = s.profileHeight; params.profileA[1] = s.profileDome;
    params.profileA[2] = s.profileTopSoftness; params.profileA[3] = s.profileBaseSoftness;
    params.profileB[0] = s.profileSmallDensity; params.profileB[1] = s.profileBodyDensity;
    params.profileB[2] = s.profileBaseErosion; params.profileB[3] = s.profileErosionRise;
    params.profileC[0] = s.profileHeavyErosionCut; params.profileC[1] = s.profileHeavyThinning;
    return graph.AddPass2(RenderPass::Main_CloudNoise, {},
        [this, renderer, params, noise, profile, formation](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            // Only what this rebuild writes is declared: the rest stays at rest, possibly being read.
            const auto declare = [&](D3D12_RESOURCE_STATES state) {
                if (noise) { ctx.Use(weather_.Get(), state); }
                if (profile) { ctx.Use(profileGen_.Get(), state); }
                if (formation) { ctx.Use(formationGen_.Get(), state); ctx.Use(cellsGen_.Get(), state); }
            };
            const auto write = ctx.usePoint ? *ctx.usePoint : 0u;
            declare(kUav);
            ctx.NextPoint();
            const auto restore = ctx.usePoint ? *ctx.usePoint : 0u;
            declare(kRest);
            return [this, renderer, params, noise, profile, formation, write, restore](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassCloudNoise);
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                {
                    GPU_SCOPE(t.cl, ProfilerScopes::kPassCloudNoise);
                    renderer->EmitPoint(t.cl, write);
                    const auto sampler = renderer->GetSamplerManager()->GetTable(renderer, std::array{*SamplerManager::LinearWrap()});
                    // numthreads(8, 8, 1): x and y in groups of 8, one group per slice.
                    const auto dispatch = [&](Material* m, UINT width, UINT height, UINT depth, D3D12_CPU_DESCRIPTOR_HANDLE uav) {
                        NoiseCB cb = params;
                        cb.size = width; cb.depth = depth > 1u ? depth : height; // a volume's depth, a plane's height
                        Dispatch3D(renderer, t.cl, m, render::kConstantBufferAlignment,
                            [&cb](uint8_t* dst) { std::memcpy(dst, &cb, sizeof(cb)); }, {}, {uav}, sampler,
                            (width + 7u) / 8u, (height + 7u) / 8u, depth);
                    };
                    if (noise) { dispatch(noiseWeather_.get(), kWeatherSize, kWeatherSize, 1u, weatherUav_); }
                    if (profile) { dispatch(noiseProfile_.get(), kProfileWidth, kProfileHeight, 1u, profileGenUav_); }
                    if (formation)
                    {
                        dispatch(noiseFormation_.get(), kFormationSize, kFormationSize, kFormationDepth, formationGenUav_);
                        dispatch(noiseCells_.get(), kCellsSize, kCellsSize, kCellsSize, cellsGenUav_);
                    }
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
    // N3: the map is the GROUND's light's (the clouds' own light can be another body after sunset).
    const Math::float3 cloudLightDir = Math::Normalize(in.sunDirection);
    const Math::float3 lightDir = (in.shadowDirection.x != 0.0f || in.shadowDirection.y != 0.0f || in.shadowDirection.z != 0.0f)
        ? Math::Normalize(in.shadowDirection) : cloudLightDir;
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
    c.sun[0] = -cloudLightDir.x; c.sun[1] = -cloudLightDir.y; c.sun[2] = -cloudLightDir.z; c.sun[3] = s.phaseG;
    c.shadowSun[0] = -lightDir.x; c.shadowSun[1] = -lightDir.y; c.shadowSun[2] = -lightDir.z;
    const bool perSample = in.perSampleTransmittance && in.atmosphereTopKm > in.planetRadiusKm;
    c.light[0] = perSample ? in.atmosphereTopKm : 0.0f; c.light[1] = perSample ? 1.0f : 0.0f;
    c.light[2] = static_cast<float>(in.lightBody);
    c.light[3] = std::sin(std::clamp(std::isfinite(s.reflectionHorizonFadeDeg) ? s.reflectionHorizonFadeDeg : 5.0f, 0.0f, 45.0f) * 0.0174532925f);
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
    // The drift: the level's wind heading at the cloud's own speed, on the shared (freezable) clock.
    // The field moves WITH the wind, so the sample point moves against it.
    const float driftM = s.windKmH / 3.6f * in.windTime;
    c.wind[0] = -in.windDirXZ.x * driftM; c.wind[1] = 0.0f; c.wind[2] = -in.windDirXZ.y * driftM; c.wind[3] = s.tracingStartMaxDistanceKm;
    c.output[0] = static_cast<float>(in.outputWidth); c.output[1] = static_cast<float>(in.outputHeight);
    c.output[2] = in.preExposure; c.output[3] = 1.0f / std::max(in.preExposure, 1.0e-8f);
    c.depth[0] = static_cast<float>(in.depthWidth); c.depth[1] = static_cast<float>(in.depthHeight);
    c.depth[2] = in.projZ33; c.depth[3] = in.projZ43;
    c.resolve[0] = static_cast<float>(std::max(in.resolveWidth, 1u)); c.resolve[1] = static_cast<float>(std::max(in.resolveHeight, 1u));
    c.resolve[2] = static_cast<float>(std::max(in.resolveScale, 1u));
    c.resolve[3] = (blueNoise_.Get() != nullptr && render::g_cloudBlueNoise) ? 1.0f : 0.0f;
    c.aerial[0] = in.aerialBuilt ? 1.0f : 0.0f; c.aerial[1] = in.aerialStartKm;
    c.aerial[2] = in.distantActive ? 1.0f : 0.0f; c.aerial[3] = static_cast<float>(in.debugView);
    c.temporal[0] = in.historyValid ? 1.0f : 0.0f; c.temporal[1] = s.temporal ? std::clamp(s.historyWeight, 0.0f, 0.99f) : 0.0f;
    c.temporal[2] = in.prevPreExposure > 0.0f ? in.preExposure / in.prevPreExposure : 1.0f;
    c.temporal[3] = static_cast<float>(in.frameIndex & 1023u);
    c.shadowMap[0] = static_cast<float>(kShadowSize); c.shadowMap[1] = 1.0f / static_cast<float>(kShadowSize);
    c.shadowMap[2] = farPlane / 1000.0f; c.shadowMap[3] = s.shadowStrength;
    c.shadowMap2[0] = shadowSamples; c.shadowMap2[1] = s.shadowDepthBiasKm;
    // Read by the environment capture only: the screen trace's aerial volume is scaled on the CPU.
    c.shadowMap2[2] = CloudAirSunScale(CloudSunGroundVisibility(s));
    c.profile[1] = profileInvSize_[0]; c.profile[2] = profileInvSize_[1];
    // The formation's time axis advances with the DRIFT, as UDS's does with their cloud movement:
    // formationChangeSpeed full cycles per 16 formation tiles travelled (VolumetricCloudSettings.h).
    // In double, wrapped before upload: the drift grows without bound on a long session.
    constexpr double kTilesPerCycle = 16.0;
    const double driftKm = static_cast<double>(s.windKmH) * static_cast<double>(in.windTime) / 3600.0;
    const double cycles = static_cast<double>(std::max(s.formationChangeSpeed, 0.0f)) * driftKm
        / (static_cast<double>(std::max(s.weatherTileKm, 0.001f)) * kTilesPerCycle);
    c.formation[0] = 1.0f / std::max(s.weatherTileKm, 0.001f);
    c.formation[1] = std::isfinite(cycles) ? static_cast<float>(cycles - std::floor(cycles)) : 0.0f;
    // An overcast deck is one sheet: the macro variation fades with it (UDS scale theirs by 1 - lerp to simplified).
    const float overcast = CloudOvercastAmount(s);
    c.formation[2] = std::clamp(s.macroVariation, 0.0f, 1.0f) * (1.0f - overcast);
    c.formation[3] = 1.0f / std::max(s.macroTileKm, 0.1f);
    c.lighting[0] = static_cast<float>(std::min(s.msOctaves, 2u));
    c.lighting[1] = std::isfinite(s.skyLightIntensity) ? std::clamp(s.skyLightIntensity, 0.0f, 10.0f) : 1.0f;
    c.lighting[2] = std::isfinite(s.diffuseLight) ? std::clamp(s.diffuseLight, 0.0f, 10.0f) : 0.25f;
    c.lighting[3] = overcast;
    c.floorShape[3] = kCloudOvercastDensityLift * overcast;
    c.profile[0] = overcast * (std::isfinite(s.overcastEvenness) ? std::clamp(s.overcastEvenness, 0.0f, 1.0f) : 0.7f);
    const auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    c.erosion[0] = std::clamp(finite(s.erosionStrength, 1.2f), 0.0f, 4.0f);
    c.erosion[1] = std::clamp(finite(s.erosionPower, 3.0f), 0.25f, 8.0f);
    c.erosion[2] = 1.0f / std::clamp(finite(s.erosionTileKm, 1.2f), 0.05f, 100.0f);
    c.erosion[3] = std::clamp(finite(s.highFrequencyStrength, 0.24f), 0.0f, 2.0f);
    c.floorShape[0] = std::clamp(finite(s.floorVariationClear, 0.08f), 0.0f, 2.0f);
    c.floorShape[1] = std::clamp(finite(s.floorVariationCloudy, 0.64f), 0.0f, 2.0f);
    c.floorShape[2] = 1.0f / std::clamp(finite(s.highFrequencyDistanceKm, 1.5f), 0.01f, 100.0f);
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
    bool dirty = !captureValid_ || std::memcmp(&s, &captureSettings_, sizeof(s)) != 0
        || noiseBuilds_ != captureNoiseRevision_ || distantRevision != captureDistantRevision_;
    if (!dirty && frame >= captureFrame_ + kCaptureIntervalFrames)
    {
        const float dx = probeXZ.x - captureProbeXZ_.x, dz = probeXZ.y - captureProbeXZ_.y;
        dirty = (dx * dx + dz * dz) > kCaptureMoveMetres * kCaptureMoveMetres || in.windTime != captureWindTime_;
    }
    if (dirty)
    {
        captureValid_ = true; captureSettings_ = s; captureNoiseRevision_ = noiseBuilds_;
        captureDistantRevision_ = distantRevision; captureFrame_ = frame;
        captureProbeXZ_ = probeXZ; captureWindTime_ = in.windTime;
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
    overlayConstants_ = constants; // read by the closure, which stays inside the small buffer (see the member)
    overlay.writeConstants = [this](uint8_t* dst, unsigned mipSize) {
        VolumetricCloudConstants mip = overlayConstants_;
        mip.output[0] = mip.output[1] = static_cast<float>(mipSize);
        std::memcpy(dst, &mip, sizeof(mip));
    };
    const DensityBinding bind = Binding();
    overlay.reads = {bind.formation, bind.cells, weather_.Get(), distant ? in.distantResource : nullptr, bind.profile};
    overlay.srvs = {bind.formationSrv, bind.cellsSrv, weatherSrv_, distant ? in.distantSrv : renderer->VsmDummyTexSrv(), bind.profileSrv};
    overlay.dependency = noisePass;
    overlay.groundSunScale = CloudSunGroundVisibility(s);
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
        shadowSurfaceStrength_ = 1.0f;
        return kNone;
    }
    VolumetricCloudConstants constants = MakeConstants(in);
    // The surfaces' share of the shadow by its light's height (VolumetricCloudSettings::shadowHorizonFadeDeg):
    // none below 1 degree, smoothstep to full at the fade height. shadowSun is the unit direction TO the light.
    {
        const float fadeDeg = in.settings->shadowHorizonFadeDeg;
        const float elevationDeg = std::asin(std::clamp(constants.shadowSun[1], -1.0f, 1.0f)) * 57.2957795f;
        float strength = 1.0f;
        if (fadeDeg > 1.0f)
        {
            const float t = std::clamp((elevationDeg - 1.0f) / (fadeDeg - 1.0f), 0.0f, 1.0f);
            strength = t * t * (3.0f - 2.0f * t);
        }
        shadowSurfaceStrength_ = strength;
    }

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
            // Which key forced a FULL re-trace while slicing -- one line per reason as it changes. A key
            // that moves every frame turns the slicing off without a word (the dev window's counter only).
            LOG_INFO_ONCE_PER_MESSAGE(logging::LogCategory::Render, "cloud shadow map: full re-trace forced: {}",
                                      forced ? forced : "?");
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
    // The density model's textures: generated, or the files that replaced them. The pass reads all of it from
    // shadowPass_ (see the member: closures that copied it were heap allocations every frame).
    shadowPass_.constants = constants;
    shadowPass_.bind = Binding();
    shadowPass_.traceSize = traceSize;
    return graph.AddPass2(RenderPass::Main_CloudShadow, RenderQueue::AsyncCompute, deps, {}, {},
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const DensityBinding& bind = shadowPass_.bind;
            std::array<std::uint32_t, 3> points{};
            points[0] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(bind.formation, kRest); ctx.Use(bind.cells, kRest); ctx.Use(weather_.Get(), kRest);
            ctx.Use(bind.profile, kRest);
            ctx.Use(shadowRaw_.Get(), kUav);
            ctx.NextPoint(); points[1] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(shadowRaw_.Get(), kRest);
            ctx.Use(shadowFiltered_.Get(), kUav);
            ctx.NextPoint(); points[2] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(shadowFiltered_.Get(), kRest);
            // Serial builder: what the consumers registered after this read.
            shadowBuilt_ = true;
            std::memcpy(&shadowViewProj_.m, shadowPass_.constants.shadowViewProj, sizeof(float) * 16);
            shadowFarKm_ = shadowPass_.constants.shadowMap[2];
            return [this, renderer, points](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassCloudShadow);
                const VolumetricCloudConstants& constants = shadowPass_.constants;
                const DensityBinding& bind = shadowPass_.bind;
                const UINT traceSize = shadowPass_.traceSize;
                auto t = c.BeginCL(); SetCommandListName(t.cl, c.pass);
                {
                    GPU_SCOPE(t.cl, ProfilerScopes::kPassCloudShadow);
                    const auto sampler = renderer->GetSamplerManager()->GetTable(renderer, std::array{*SamplerManager::LinearWrap()});
                    const auto write = [&constants](uint8_t* dst) { std::memcpy(dst, &constants, sizeof(constants)); };
                    renderer->EmitPoint(t.cl, points[0]);
                    RecordComputeDispatch(renderer, t.cl, shadowTrace_.get(), cbBytes_, write,
                        {bind.formationSrv, bind.cellsSrv, weatherSrv_, renderer->VsmDummyTexSrv(), bind.profileSrv}, {shadowRawUav_}, sampler, traceSize, traceSize);
                    renderer->EmitPoint(t.cl, points[1]);
                    RecordComputeDispatch(renderer, t.cl, shadowFilter_.get(), cbBytes_, write,
                        {bind.formationSrv, bind.cellsSrv, weatherSrv_, shadowRawSrv_, bind.profileSrv}, {shadowFilteredUav_}, sampler, kShadowSize, kShadowSize);
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
    // Everything the pass reads, in tracePass_ (see the member: closures that copied it were heap allocations every
    // frame).
    TracePassFrame& f = tracePass_;
    f.constants = constants;
    const bool aerial = in.aerialBuilt && in.aerialResource && in.aerialSrv.ptr != 0;
    const bool distant = in.distantActive && in.distantResource && in.distantSrv.ptr != 0;
    f.aerialResource = aerial ? in.aerialResource : nullptr;
    f.distantResource = distant ? in.distantResource : nullptr;
    f.aerialSrv = aerial ? in.aerialSrv : renderer->VsmDummyTexSrv();
    f.distantSrv = distant ? in.distantSrv : renderer->VsmDummyTexSrv();
    // N3: the sky's transmittance LUT (t7), for the light at each sample; read only when constants say so.
    const bool transmittance = constants.light[1] > 0.0f && in.transmittanceResource && in.transmittanceSrv.ptr != 0;
    f.transmittanceResource = transmittance ? in.transmittanceResource : nullptr;
    f.transmittanceSrv = transmittance ? in.transmittanceSrv : renderer->VsmDummyTexSrv();
    // The density model's textures: generated, or the files that replaced them.
    f.bind = Binding();
    // t8: the blue-noise mask (read only when the constants say so; a placeholder otherwise).
    f.blueNoiseResource = blueNoise_.Get();
    f.blueNoiseSrv = f.blueNoiseResource ? blueNoiseSrv_ : renderer->VsmDummyTexSrv();
    // ON THE ASYNC COMPUTE QUEUE (`--async-pass` name CloudTrace). The march is latency-bound -- a
    // long serial loop per ray, and in trace mode 0 too few rays to fill the GPU -- which is the
    // character that overlaps (docs/async_compute_plan.md: BuildAS won, GTAO lost to a saturating
    // partner). SceneRenderer::BuildLighting registers it before the fog and the lights, so it runs
    // beside them; compose's wait is the one fence back. Everything it reads is left compute-legal by
    // its last graphics user: the depth NON_PIXEL by Main_Hzb, the transmittance LUT at rest by
    // Main_SkyAerial (which drops the PIXEL bit Main_SkyView adds for the sky's disc). Measured
    // 2026-10-14 on the owner's view (wind_test 15:00, DLSS Balanced, --profdump, one binary, the A arm
    // `--async-pass=BuildAS,ObjectCompute,RTTrace`, interleaved x3, no overlap between the arms):
    // GPU.Frame trace mode 1 2.994 -> 2.795 ms, mode 0 2.803 -> 2.656; pixels within the run-to-run floor.
    // THOSE RUNS HAD NO WAIT: compose's edge to this pass was dropped (a full dependency list, see
    // RenderGraph::kPassDependencyCapacity), so compose sometimes read the pair kFrameCount frames old --
    // the clouds jumped back three frames of camera motion. With the wait (2026-10-23, the level's own
    // camera, 15:00, DLSS Balanced, A/B/A/B): 3.57 (and a noisy 3.93) on graphics -> 3.44 / 3.46 async.
    return graph.AddPass2(RenderPass::Main_CloudTrace, RenderQueue::AsyncCompute, list, {}, {},
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const TracePassFrame& f = tracePass_;
            const DensityBinding& bind = f.bind;
            const auto& D = renderer->GetDeferredForFrame();
            const auto& P = renderer->GetDeferredForPrevFrame();
            std::array<std::uint32_t, 3> points{};
            points[0] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(bind.formation, kRest); ctx.Use(bind.cells, kRest); ctx.Use(weather_.Get(), kRest);
            ctx.Use(bind.profile, kRest);
            if (f.blueNoiseResource) { ctx.Use(f.blueNoiseResource, kRest); }
            // The sky's volume and distant light, at their rest; declared so the reads are named.
            if (f.aerialResource) { ctx.Use(f.aerialResource, kRest); }
            if (f.distantResource) { ctx.Use(f.distantResource, kRest); }
            if (f.transmittanceResource) { ctx.Use(f.transmittanceResource, kRest); }
            // The depth NON_PIXEL only: the PIXEL bit is illegal on the compute queue.
            ctx.Use(D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ctx.Use(D.cloudTrace.Get(), kUav); ctx.Use(D.cloudTraceDepth.Get(), kUav);
            ctx.NextPoint(); points[1] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(D.cloudTrace.Get(), kRest); ctx.Use(D.cloudTraceDepth.Get(), kRest);
            ctx.Use(P.cloudResolved.Get(), kRest); ctx.Use(P.cloudResolvedDepth.Get(), kRest); // the history, at rest
            ctx.Use(D.cloudResolved.Get(), kUav); ctx.Use(D.cloudResolvedDepth.Get(), kUav);
            ctx.NextPoint(); points[2] = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(D.cloudResolved.Get(), kRest); ctx.Use(D.cloudResolvedDepth.Get(), kRest);
            return [this, renderer, points](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassCloudTrace);
                const TracePassFrame& f = tracePass_;
                const VolumetricCloudConstants& constants = f.constants;
                const DensityBinding& bind = f.bind;
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
                            {bind.formationSrv, bind.cellsSrv, weatherSrv_, DF.depthSRV, f.aerialSrv, f.distantSrv, bind.profileSrv, f.transmittanceSrv,
                             f.blueNoiseSrv},
                            {DF.cloudTraceUAV, DF.cloudTraceDepthUAV}, samplers, DF.cloudWidth, DF.cloudHeight);
                    }
                    renderer->EmitPoint(t.cl, points[1]);
                    {
                        GPU_SCOPE(t.cl, ProfilerScopes::kCloudTraceTemporal);
                        const auto samplers = renderer->GetSamplerManager()->GetTable(renderer,
                            std::array{*SamplerManager::LinearClamp(), *SamplerManager::PointClamp()});
                        RecordComputeDispatch(renderer, t.cl, temporal_.get(), cbBytes_, write,
                            {DF.cloudTraceSRV, DF.cloudTraceDepthSRV, PF.cloudResolvedSRV, PF.cloudResolvedDepthSRV},
                            {DF.cloudResolvedUAV, DF.cloudResolvedDepthUAV}, samplers, DF.cloudResolvedWidth, DF.cloudResolvedHeight);
                    }
                    renderer->EmitPoint(t.cl, points[2]);
                }
                c.EndCL(t);
            };
        });
}
