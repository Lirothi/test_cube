#include "OceanFFT.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cfloat>
#include <limits>
#include <random>
#include <vector>
#include <cstring>

#include "Helpers.h"
#include "Material.h"
#include "MaterialManager.h"
#include "RenderContext.h"
#include "RenderContextPool.h"
#include "Renderer.h"
#include "UploadManager.h"

using Microsoft::WRL::ComPtr;

namespace {
constexpr float kPi = 3.14159265358979323846f;

inline uint32_t FloatAsUint(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

float DegreesToRadians(float deg) {
    return deg * (kPi / 180.0f);
}

float Clamp(float v, float minV, float maxV) {
    return std::max(minV, std::min(v, maxV));
}

} // namespace

struct SpectrumParameters
{
    float scale;
    float angle;
    float spreadBlend;
    float swell;
    float alpha;
    float peakOmega;
    float gamma;
    float shortWavesFade;
};

struct OceanFFTSystem::SpectrumBuffer {
    bool Initialize(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        const void* data,
        size_t byteSize,
        std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
    {
        if (!renderer || !uploadCmdList || data == nullptr) {
            return false;
        }

        UploadManager uploader(renderer->GetDevice(), uploadCmdList);
        buffer_ = uploader.CreateBufferWithData(data, byteSize,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        uploader.StealKeepAlive(uploadKeepAlive);
        renderer->SetResourceState(buffer_.Get(), D3D12_RESOURCE_STATE_GENERIC_READ);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(renderer->GetDevice()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&cpuHeap_)));

        srv_ = cpuHeap_->GetCPUDescriptorHandleForHeapStart();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = static_cast<UINT>(byteSize / sizeof(SpectrumParameters));
        srvDesc.Buffer.StructureByteStride = sizeof(SpectrumParameters);
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        renderer->GetDevice()->CreateShaderResourceView(buffer_.Get(), &srvDesc, srv_);
        return true;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetSRV() const { return srv_; }
    ID3D12Resource* GetResource() const { return buffer_.Get(); }

private:
    ComPtr<ID3D12Resource> buffer_;
    ComPtr<ID3D12DescriptorHeap> cpuHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_{};
};

class OceanFFTSystem::FastFourierTransform {
public:
    bool Initialize(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        UINT size,
        std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive);

    void IFFT2D(Renderer* renderer,
        ID3D12GraphicsCommandList* cl,
        GpuTexture2D& input,
        GpuTexture2D& buffer,
        bool outputToInput,
        bool scale,
        bool permute);

    D3D12_CPU_DESCRIPTOR_HANDLE GetPrecomputedSRV(Renderer* renderer);

private:
    void DispatchPrecompute(Renderer* renderer, ID3D12GraphicsCommandList* cl);

    UINT size_ = 0;
    UINT logSize_ = 0;

    GpuTexture2D precomputedData_;

    std::shared_ptr<Material> matPrecompute_;
    std::shared_ptr<Material> matHorizontalIFFT_;
    std::shared_ptr<Material> matVerticalIFFT_;
    std::shared_ptr<Material> matScale_;
    std::shared_ptr<Material> matPermute_;
};

class OceanFFTSystem::Cascade {
public:
    bool Initialize(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        UINT size,
        const OceanCascadeConfig& config,
        const OceanWavesSettings& settings,
        SpectrumBuffer& spectrumBuffer,
        const GpuTexture2D& gaussianNoise,
        std::shared_ptr<Material> initialSpectrum,
        std::shared_ptr<Material> conjugateSpectrum,
        std::shared_ptr<Material> timeSpectrum,
        std::shared_ptr<Material> mergeMaterial,
        FastFourierTransform& fft,
        std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive);

    void Update(Renderer* renderer,
        ID3D12GraphicsCommandList* cl,
        FastFourierTransform& fft,
        float time,
        float deltaTime);

    const GpuTexture2D& Displacement() const { return displacement_; }
    const GpuTexture2D& Derivatives() const { return derivatives_; }
    const GpuTexture2D& Turbulence() const { return turbulence_; }

private:
    void CalculateInitialSpectrum(Renderer* renderer,
        ID3D12GraphicsCommandList* cl,
        const OceanWavesSettings& settings,
        SpectrumBuffer& spectrumBuffer,
        const GpuTexture2D& gaussianNoise);

    void CalculateTimeDependentSpectrum(Renderer* renderer,
        ID3D12GraphicsCommandList* cl,
        float time);

    void MergeDisplacement(Renderer* renderer,
        ID3D12GraphicsCommandList* cl,
        float deltaTime);

    UINT size_ = 0;
    float lengthScale_ = 0.0f;
    float cutoffLow_ = 0.0f;
    float cutoffHigh_ = 0.0f;
    float lambda_ = 1.0f;

    GpuTexture2D initialSpectrum_;
    GpuTexture2D waveData_;
    GpuTexture2D h0kBuffer_;

    GpuTexture2D dx_dz_;
    GpuTexture2D dy_dxz_;
    GpuTexture2D dyx_dyz_;
    GpuTexture2D dxx_dzz_;
    GpuTexture2D fftBuffer_;

    GpuTexture2D displacement_;
    GpuTexture2D derivatives_;
    GpuTexture2D turbulence_;

    std::shared_ptr<Material> matInitial_;
    std::shared_ptr<Material> matConjugate_;
    std::shared_ptr<Material> matTimeDependent_;
    std::shared_ptr<Material> matMerge_;
};

// -----------------------------------------------------------------------------
// FastFourierTransform implementation
// -----------------------------------------------------------------------------

bool OceanFFTSystem::FastFourierTransform::Initialize(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    UINT size,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    size_ = size;
    logSize_ = 0;
    UINT n = size;
    while (n > 1) {
        n >>= 1u;
        ++logSize_;
    }

    Material::ComputeDesc desc{};
    desc.shaderFile = L"shaders/ocean_fft.hlsl";

    desc.csEntry = "PrecomputeTwiddleFactorsAndInputIndices";
    matPrecompute_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, desc);

    desc.csEntry = "HorizontalStepInverseFFT";
    matHorizontalIFFT_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, desc);

    desc.csEntry = "VerticalStepInverseFFT";
    matVerticalIFFT_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, desc);

    desc.csEntry = "Scale";
    matScale_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, desc);

    desc.csEntry = "Permute";
    matPermute_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, desc);

    if (!precomputedData_.Create(renderer,
        logSize_,
        size_,
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        1,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }

    precomputedData_.CreateViews(renderer,
        /*srv*/true,
        /*uav*/true,
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        DXGI_FORMAT_R32G32B32A32_FLOAT);

    DispatchPrecompute(renderer, uploadCmdList);

    renderer->Transition(uploadCmdList, precomputedData_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (uploadKeepAlive) {
        // No additional uploads created here, but keep interface consistent
        (void)uploadKeepAlive;
    }

    return true;
}

void OceanFFTSystem::FastFourierTransform::DispatchPrecompute(Renderer* renderer,
    ID3D12GraphicsCommandList* cl)
{
    if (!renderer || !cl) {
        return;
    }

    renderer->Transition(cl, precomputedData_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = ctxHandle.ref();
    ctx.ClearFast();
    ctx.constants[0] = { size_, 0u, 0u };
    auto uav = renderer->StageSrvUavTable({ precomputedData_.GetUavCPU(), precomputedData_.GetUavCPU(), precomputedData_.GetUavCPU() });
    ctx.table[4] = uav.gpu;

    matPrecompute_->Bind(cl, ctx);
    const UINT groupsX = logSize_;
    const UINT groupsY = std::max<UINT>(1u, (size_ / 2u + 7u) / 8u);
    cl->Dispatch(groupsX, groupsY, 1);
    renderer->UAVBarrier(cl, precomputedData_.GetResource());
}

D3D12_CPU_DESCRIPTOR_HANDLE OceanFFTSystem::FastFourierTransform::GetPrecomputedSRV(Renderer* renderer)
{
    (void)renderer;
    return precomputedData_.GetSrvCPU();
}

void OceanFFTSystem::FastFourierTransform::IFFT2D(Renderer* renderer,
    ID3D12GraphicsCommandList* cl,
    GpuTexture2D& input,
    GpuTexture2D& buffer,
    bool outputToInput,
    bool scale,
    bool permute)
{
    if (!renderer || !cl) {
        return;
    }

    renderer->Transition(cl, input.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, buffer.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    bool pingPong = false;

    const UINT groupCount = std::max<UINT>(1u, size_ / 8u);

    for (UINT step = 0; step < logSize_; ++step) {
        pingPong = !pingPong;

        auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
        auto& ctx = ctxHandle.ref();
        ctx.ClearFast();
        ctx.constants[0] = { size_, step, pingPong ? 1u : 0u };
        auto srv = renderer->StageSrvUavTable({ precomputedData_.GetSrvCPU() });
        ctx.table[0] = srv.gpu;
        auto uav = renderer->StageSrvUavTable({ precomputedData_.GetUavCPU(), input.GetUavCPU(), buffer.GetUavCPU() });
        ctx.table[4] = uav.gpu;

        matHorizontalIFFT_->Bind(cl, ctx);
        cl->Dispatch(groupCount, groupCount, 1);
        renderer->UAVBarrier(cl, pingPong ? buffer.GetResource() : input.GetResource());
    }

    for (UINT step = 0; step < logSize_; ++step) {
        pingPong = !pingPong;

        auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
        auto& ctx = ctxHandle.ref();
        ctx.ClearFast();
        ctx.constants[0] = { size_, step, pingPong ? 1u : 0u };
        auto srv = renderer->StageSrvUavTable({ precomputedData_.GetSrvCPU() });
        ctx.table[0] = srv.gpu;
        auto uav = renderer->StageSrvUavTable({ precomputedData_.GetUavCPU(), input.GetUavCPU(), buffer.GetUavCPU() });
        ctx.table[4] = uav.gpu;

        matVerticalIFFT_->Bind(cl, ctx);
        cl->Dispatch(groupCount, groupCount, 1);
        renderer->UAVBarrier(cl, pingPong ? buffer.GetResource() : input.GetResource());
    }

    if (pingPong && outputToInput) {
        renderer->Transition(cl, buffer.GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        renderer->Transition(cl, input.GetResource(), D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(input.GetResource(), buffer.GetResource());
        renderer->Transition(cl, buffer.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        renderer->Transition(cl, input.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pingPong = false;
    }

    GpuTexture2D& resultTexture = (outputToInput || !pingPong) ? input : buffer;

    if (permute) {
        auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
        auto& ctx = ctxHandle.ref();
        ctx.ClearFast();
        ctx.constants[0] = { size_, 0u, 0u };
        auto srv = renderer->StageSrvUavTable({ precomputedData_.GetSrvCPU() });
        ctx.table[0] = srv.gpu;
        auto uav = renderer->StageSrvUavTable({ resultTexture.GetUavCPU(), resultTexture.GetUavCPU(), resultTexture.GetUavCPU() });
        ctx.table[4] = uav.gpu;

        matPermute_->Bind(cl, ctx);
        cl->Dispatch(groupCount, groupCount, 1);
        renderer->UAVBarrier(cl, resultTexture.GetResource());
    }

    if (scale) {
        auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
        auto& ctx = ctxHandle.ref();
        ctx.ClearFast();
        ctx.constants[0] = { size_, 0u, 0u };
        auto srv = renderer->StageSrvUavTable({ precomputedData_.GetSrvCPU() });
        ctx.table[0] = srv.gpu;
        auto uav = renderer->StageSrvUavTable({ resultTexture.GetUavCPU(), resultTexture.GetUavCPU(), resultTexture.GetUavCPU() });
        ctx.table[4] = uav.gpu;

        matScale_->Bind(cl, ctx);
        cl->Dispatch(groupCount, groupCount, 1);
        renderer->UAVBarrier(cl, resultTexture.GetResource());
    }
}

// -----------------------------------------------------------------------------
// Cascade implementation
// -----------------------------------------------------------------------------

bool OceanFFTSystem::Cascade::Initialize(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    UINT size,
    const OceanCascadeConfig& config,
    const OceanWavesSettings& settings,
    SpectrumBuffer& spectrumBuffer,
    const GpuTexture2D& gaussianNoise,
    std::shared_ptr<Material> initialSpectrum,
    std::shared_ptr<Material> conjugateSpectrum,
    std::shared_ptr<Material> timeSpectrum,
    std::shared_ptr<Material> mergeMaterial,
    FastFourierTransform& fft,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    (void)uploadKeepAlive;
    size_ = size;
    lengthScale_ = config.lengthScale;
    cutoffLow_ = config.cutoffLow;
    cutoffHigh_ = config.cutoffHigh;
    lambda_ = settings.lambda;

    matInitial_ = std::move(initialSpectrum);
    matConjugate_ = std::move(conjugateSpectrum);
    matTimeDependent_ = std::move(timeSpectrum);
    matMerge_ = std::move(mergeMaterial);

    if (!initialSpectrum_.Create(renderer, size_, size_, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }
    initialSpectrum_.CreateViews(renderer, true, true, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT);

    if (!waveData_.Create(renderer, size_, size_, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }
    waveData_.CreateViews(renderer, true, true, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT);

    if (!h0kBuffer_.Create(renderer, size_, size_, DXGI_FORMAT_R32G32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }
    h0kBuffer_.CreateViews(renderer, false, true, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT);

    if (!dx_dz_.Create(renderer, size_, size_, DXGI_FORMAT_R32G32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }
    dx_dz_.CreateViews(renderer, true, true, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT);

    if (!dy_dxz_.Create(renderer, size_, size_, DXGI_FORMAT_R32G32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }
    dy_dxz_.CreateViews(renderer, true, true, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT);

    if (!dyx_dyz_.Create(renderer, size_, size_, DXGI_FORMAT_R32G32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }
    dyx_dyz_.CreateViews(renderer, true, true, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT);

    if (!dxx_dzz_.Create(renderer, size_, size_, DXGI_FORMAT_R32G32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }
    dxx_dzz_.CreateViews(renderer, true, true, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT);

    if (!fftBuffer_.Create(renderer, size_, size_, DXGI_FORMAT_R32G32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }
    fftBuffer_.CreateViews(renderer, false, true, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT);

    if (!displacement_.Create(renderer, size_, size_, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }
    displacement_.CreateViews(renderer, true, true, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT);

    if (!derivatives_.Create(renderer, size_, size_, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }
    derivatives_.CreateViews(renderer, true, true, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT);

    if (!turbulence_.Create(renderer, size_, size_, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }
    turbulence_.CreateViews(renderer, true, true, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT);

    CalculateInitialSpectrum(renderer, uploadCmdList, settings, spectrumBuffer, gaussianNoise);

    renderer->Transition(uploadCmdList, initialSpectrum_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(uploadCmdList, waveData_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    return true;
}

void OceanFFTSystem::Cascade::CalculateInitialSpectrum(Renderer* renderer,
    ID3D12GraphicsCommandList* cl,
    const OceanWavesSettings& settings,
    SpectrumBuffer& spectrumBuffer,
    const GpuTexture2D& gaussianNoise)
{
    renderer->Transition(cl, initialSpectrum_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, waveData_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, h0kBuffer_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = ctxHandle.ref();
    ctx.ClearFast();
    ctx.constants[0] = {
        size_,
        FloatAsUint(lengthScale_),
        FloatAsUint(cutoffHigh_),
        FloatAsUint(cutoffLow_),
        FloatAsUint(settings.gravity),
        FloatAsUint(settings.depth),
        0u,
        0u
    };

    auto srvs = renderer->StageSrvUavTable({ gaussianNoise.GetSrvCPU(), spectrumBuffer.GetSRV() });
    ctx.table[0] = srvs.gpu;
    auto uavs = renderer->StageSrvUavTable({ initialSpectrum_.GetUavCPU(), waveData_.GetUavCPU(), h0kBuffer_.GetUavCPU() });
    ctx.table[4] = uavs.gpu;

    matInitial_->Bind(cl, ctx);
    const UINT groupCount = std::max<UINT>(1u, size_ / 8u);
    cl->Dispatch(groupCount, groupCount, 1);
    renderer->UAVBarrier(cl, waveData_.GetResource());
    renderer->UAVBarrier(cl, h0kBuffer_.GetResource());

    ctx.ClearFast();
    auto uavsConjugate = renderer->StageSrvUavTable({ initialSpectrum_.GetUavCPU(), waveData_.GetUavCPU(), h0kBuffer_.GetUavCPU() });
    ctx.table[4] = uavsConjugate.gpu;
    matConjugate_->Bind(cl, ctx);
    cl->Dispatch(groupCount, groupCount, 1);
    renderer->UAVBarrier(cl, initialSpectrum_.GetResource());
}

void OceanFFTSystem::Cascade::CalculateTimeDependentSpectrum(Renderer* renderer,
    ID3D12GraphicsCommandList* cl,
    float time)
{
    renderer->Transition(cl, dx_dz_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, dy_dxz_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, dyx_dyz_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, dxx_dzz_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    renderer->Transition(cl, initialSpectrum_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, waveData_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = ctxHandle.ref();
    ctx.ClearFast();
    ctx.constants[0] = { FloatAsUint(time) };
    auto srvs = renderer->StageSrvUavTable({ initialSpectrum_.GetSrvCPU(), waveData_.GetSrvCPU() });
    ctx.table[0] = srvs.gpu;
    auto uavs = renderer->StageSrvUavTable({ dx_dz_.GetUavCPU(), dy_dxz_.GetUavCPU(), dyx_dyz_.GetUavCPU(), dxx_dzz_.GetUavCPU() });
    ctx.table[4] = uavs.gpu;

    const UINT groupCount = std::max<UINT>(1u, size_ / 8u);
    matTimeDependent_->Bind(cl, ctx);
    cl->Dispatch(groupCount, groupCount, 1);

    renderer->UAVBarrier(cl, dx_dz_.GetResource());
    renderer->UAVBarrier(cl, dy_dxz_.GetResource());
    renderer->UAVBarrier(cl, dyx_dyz_.GetResource());
    renderer->UAVBarrier(cl, dxx_dzz_.GetResource());
}

void OceanFFTSystem::Cascade::MergeDisplacement(Renderer* renderer,
    ID3D12GraphicsCommandList* cl,
    float deltaTime)
{
    renderer->Transition(cl, dx_dz_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, dy_dxz_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, dyx_dyz_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, dxx_dzz_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    renderer->Transition(cl, displacement_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, derivatives_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, turbulence_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = ctxHandle.ref();
    ctx.ClearFast();
    ctx.constants[0] = { FloatAsUint(lambda_), FloatAsUint(deltaTime) };
    auto srvs = renderer->StageSrvUavTable({ dx_dz_.GetSrvCPU(), dy_dxz_.GetSrvCPU(), dyx_dyz_.GetSrvCPU(), dxx_dzz_.GetSrvCPU() });
    ctx.table[0] = srvs.gpu;
    auto uavs = renderer->StageSrvUavTable({ displacement_.GetUavCPU(), derivatives_.GetUavCPU(), turbulence_.GetUavCPU() });
    ctx.table[4] = uavs.gpu;

    const UINT groupCount = std::max<UINT>(1u, size_ / 8u);
    matMerge_->Bind(cl, ctx);
    cl->Dispatch(groupCount, groupCount, 1);

    renderer->UAVBarrier(cl, displacement_.GetResource());
    renderer->UAVBarrier(cl, derivatives_.GetResource());
    renderer->UAVBarrier(cl, turbulence_.GetResource());

    renderer->Transition(cl, displacement_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, derivatives_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, turbulence_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void OceanFFTSystem::Cascade::Update(Renderer* renderer,
    ID3D12GraphicsCommandList* cl,
    FastFourierTransform& fft,
    float time,
    float deltaTime)
{
    CalculateTimeDependentSpectrum(renderer, cl, time);

    fft.IFFT2D(renderer, cl, dx_dz_, fftBuffer_, true, false, true);
    fft.IFFT2D(renderer, cl, dy_dxz_, fftBuffer_, true, false, true);
    fft.IFFT2D(renderer, cl, dyx_dyz_, fftBuffer_, true, false, true);
    fft.IFFT2D(renderer, cl, dxx_dzz_, fftBuffer_, true, false, true);

    renderer->Transition(cl, dx_dz_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, dy_dxz_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, dyx_dyz_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, dxx_dzz_.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    MergeDisplacement(renderer, cl, deltaTime);
}

// -----------------------------------------------------------------------------
// OceanFFTSystem implementation
// -----------------------------------------------------------------------------

OceanFFTSystem::OceanFFTSystem()
{
    settings_.gravity = 9.81f;
    settings_.depth = 500.0f;
    settings_.lambda = 1.0f;
    settings_.local = { 1.0f, 0.5f, -29.81f, 100000.0f, 1.0f, 0.198f, 3.3f, 0.01f };
    settings_.swell = { 0.0f, 1.0f, 0.0f, 300000.0f, 1.0f, 1.0f, 3.3f, 0.01f };
}

OceanFFTSystem::~OceanFFTSystem() = default;

namespace {

SpectrumParameters BuildSpectrumParameters(const OceanWavesSettings& settings,
    const OceanDisplaySpectrumSettings& display)
{
    SpectrumParameters p{};
    p.scale = display.scale;
    p.angle = DegreesToRadians(display.windDirection);
    p.spreadBlend = display.spreadBlend;
    p.swell = Clamp(display.swell, 0.01f, 1.0f);
    if (display.windSpeed <= 0.0f) {
        p.alpha = 0.0f;
        p.peakOmega = 0.0f;
    } else {
        p.alpha = 0.076f * std::pow(settings.gravity * display.fetch / (display.windSpeed * display.windSpeed), -0.22f);
        p.peakOmega = 22.0f * std::pow(display.windSpeed * display.fetch / (settings.gravity * settings.gravity), -0.33f);
    }
    p.gamma = display.peakEnhancement;
    p.shortWavesFade = display.shortWavesFade;
    return p;
}

} // namespace

bool OceanFFTSystem::Initialize(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    if (!renderer || !uploadCmdList) {
        return false;
    }

    size_ = 256;

    fft_ = std::make_unique<FastFourierTransform>();
    if (!fft_->Initialize(renderer, uploadCmdList, size_, uploadKeepAlive)) {
        return false;
    }

    const size_t noiseCount = size_t(size_) * size_t(size_);
    std::vector<float> gaussian(noiseCount * 2ull);
    std::mt19937 rng(1337);
    std::normal_distribution<float> normal(0.0f, 1.0f);
    for (size_t i = 0; i < gaussian.size(); ++i) {
        gaussian[i] = normal(rng);
    }

    if (!gaussianNoise_.CreateFromData(renderer, uploadCmdList, DXGI_FORMAT_R32G32_FLOAT,
        size_, size_, 1,
        D3D12_RESOURCE_FLAG_NONE,
        gaussian.data(),
        size_t(size_) * sizeof(float) * 2u,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        uploadKeepAlive))
    {
        return false;
    }
    gaussianNoise_.CreateViews(renderer, true, false, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT);

    SpectrumParameters params[2];
    params[0] = BuildSpectrumParameters(settings_, settings_.local);
    params[1] = BuildSpectrumParameters(settings_, settings_.swell);

    spectrumBuffer_ = std::make_unique<SpectrumBuffer>();
    if (!spectrumBuffer_->Initialize(renderer, uploadCmdList, params, sizeof(params), uploadKeepAlive)) {
        return false;
    }

    Material::ComputeDesc desc{};
    desc.shaderFile = L"shaders/ocean_initial_spectrum.hlsl";
    desc.csEntry = "CalculateInitialSpectrum";
    auto matInitial = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, desc);
    desc.csEntry = "CalculateConjugatedSpectrum";
    auto matConjugate = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, desc);

    desc.shaderFile = L"shaders/ocean_time_dependent.hlsl";
    desc.csEntry = "CalculateAmplitudes";
    auto matTime = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, desc);

    desc.shaderFile = L"shaders/ocean_merge.hlsl";
    desc.csEntry = "FillResultTextures";
    auto matMerge = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, desc);

    const float lengthScale0 = 250.0f;
    const float lengthScale1 = 17.0f;
    const float lengthScale2 = 5.0f;

    const float boundary1 = 2.0f * kPi / lengthScale1 * 6.0f;
    const float boundary2 = 2.0f * kPi / lengthScale2 * 6.0f;

    std::array<OceanCascadeConfig, 3> configs{};
    configs[0] = { lengthScale0, 0.0001f, boundary1 };
    configs[1] = { lengthScale1, boundary1, boundary2 };
    configs[2] = { lengthScale2, boundary2, std::numeric_limits<float>::max() };

    for (size_t i = 0; i < cascades_.size(); ++i) {
        cascades_[i] = std::make_unique<Cascade>();
        if (!cascades_[i]->Initialize(renderer, uploadCmdList, size_, configs[i], settings_, *spectrumBuffer_,
            gaussianNoise_, matInitial, matConjugate, matTime, matMerge, *fft_, uploadKeepAlive))
        {
            return false;
        }
    }

    time_ = 0.0f;
    deltaTime_ = 0.0f;

    return true;
}

void OceanFFTSystem::Tick(float deltaTime)
{
    deltaTime_ = deltaTime;
    time_ += deltaTime;
}

void OceanFFTSystem::Dispatch(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer || !cl || !fft_) {
        return;
    }

    for (auto& cascade : cascades_) {
        if (cascade) {
            cascade->Update(renderer, cl, *fft_, time_, deltaTime_);
        }
    }
}

void OceanFFTSystem::Reset()
{
    cascades_ = {};
    spectrumBuffer_.reset();
    fft_.reset();
    gaussianNoise_ = GpuTexture2D();
    time_ = 0.0f;
    deltaTime_ = 0.0f;
}

const GpuTexture2D& OceanFFTSystem::GetDisplacement(size_t cascadeIndex) const
{
    return cascades_.at(cascadeIndex)->Displacement();
}

const GpuTexture2D& OceanFFTSystem::GetDerivatives(size_t cascadeIndex) const
{
    return cascades_.at(cascadeIndex)->Derivatives();
}

const GpuTexture2D& OceanFFTSystem::GetTurbulence(size_t cascadeIndex) const
{
    return cascades_.at(cascadeIndex)->Turbulence();
}

