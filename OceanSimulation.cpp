#include "OceanSimulation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#include "Helpers.h"
#include "Material.h"
#include "Renderer.h"
#include "RenderContextPool.h"
#include "UploadManager.h"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr float kGravity = 9.81f;
    constexpr float kChoppiness = 1.0f;
    constexpr UINT kThreadGroupSize = 8;

    inline float PhillipsSpectrum(float kLen, float kDotW, float windSpeed, float patchLength, float spectrumScale)
    {
        if (kLen < Math::EPS)
        {
            return 0.0f;
        }

        const float L = (windSpeed * windSpeed) / kGravity;
        const float damping = 0.001f;

        float phillips = spectrumScale * std::exp(-1.0f / (kLen * kLen * L * L));
        phillips /= (kLen * kLen * kLen * kLen);
        phillips *= (kDotW * kDotW);
        phillips *= std::exp(-kLen * kLen * damping * damping);
        return std::max(phillips, 0.0f);
    }
}

OceanSimulation::OceanSimulation() = default;

uint32_t OceanSimulation::FloatToBits(float value)
{
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(uint32_t));
    return bits;
}

void OceanSimulation::Initialize(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    if (initialized_ || !renderer)
    {
        return;
    }

    BuildSpectrum();
    CreateResources(renderer, uploadCmdList, uploadKeepAlive);
    CreateDescriptors(renderer->GetDevice());
    CreateMaterials(renderer);

    initialized_ = true;
}

void OceanSimulation::OnHotReload(Renderer* renderer)
{
    if (!renderer)
    {
        return;
    }
    CreateMaterials(renderer);
}

void OceanSimulation::CreateResources(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    if (!renderer)
    {
        return;
    }

    UploadManager uploader(renderer->GetDevice(), uploadCmdList);

    const size_t h0Bytes = h0Data_.size() * sizeof(Math::float4);
    h0Buffer_ = uploader.CreateBufferWithData(h0Data_.data(), h0Bytes,
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const size_t waveBytes = waveData_.size() * sizeof(Math::float4);
    waveDataBuffer_ = uploader.CreateBufferWithData(waveData_.data(), waveBytes,
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    uploader.StealKeepAlive(uploadKeepAlive);

    mipCount_ = 1u;
    UINT size = kResolution;
    while (size > 1u)
    {
        size = std::max<UINT>(1u, size / 2u);
        ++mipCount_;
    }

    auto* device = renderer->GetDevice();

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = kResolution;
    texDesc.Height = kResolution;
    texDesc.DepthOrArraySize = kArraySlices;
    texDesc.MipLevels = static_cast<UINT16>(mipCount_);
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ThrowIfFailed(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&displacement_)));

    renderer->SetResourceState(displacement_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void OceanSimulation::CreateDescriptors(ID3D12Device* device)
{
    if (!device)
    {
        return;
    }

    const UINT descriptorCount = 2u + mipCount_ * 2u;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = descriptorCount;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap_)));

    descriptorIncr_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE base = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();

    auto Offset = [this](D3D12_CPU_DESCRIPTOR_HANDLE start, UINT index)
    {
        start.ptr += SIZE_T(index) * SIZE_T(descriptorIncr_);
        return start;
    };

    h0Srv_ = base;
    waveDataSrv_ = Offset(base, 1);

    displacementSrvs_.resize(mipCount_);
    displacementUavs_.resize(mipCount_);

    for (UINT mip = 0; mip < mipCount_; ++mip)
    {
        displacementSrvs_[mip] = Offset(base, 2 + mip);
    }
    for (UINT mip = 0; mip < mipCount_; ++mip)
    {
        displacementUavs_[mip] = Offset(base, 2 + mipCount_ + mip);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC bufferSrv{};
    bufferSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    bufferSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    bufferSrv.Format = DXGI_FORMAT_UNKNOWN;

    bufferSrv.Buffer.FirstElement = 0;
    bufferSrv.Buffer.NumElements = static_cast<UINT>(h0Data_.size());
    bufferSrv.Buffer.StructureByteStride = sizeof(Math::float4);
    bufferSrv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    device->CreateShaderResourceView(h0Buffer_.Get(), &bufferSrv, h0Srv_);

    bufferSrv.Buffer.NumElements = static_cast<UINT>(waveData_.size());
    device->CreateShaderResourceView(waveDataBuffer_.Get(), &bufferSrv, waveDataSrv_);

    D3D12_SHADER_RESOURCE_VIEW_DESC texSrv{};
    texSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    texSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    texSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texSrv.Texture2DArray.ArraySize = kArraySlices;
    texSrv.Texture2DArray.FirstArraySlice = 0;
    texSrv.Texture2DArray.MostDetailedMip = 0;
    texSrv.Texture2DArray.MipLevels = 1;
    texSrv.Texture2DArray.PlaneSlice = 0;
    texSrv.Texture2DArray.ResourceMinLODClamp = 0.0f;

    for (UINT mip = 0; mip < mipCount_; ++mip)
    {
        texSrv.Texture2DArray.MostDetailedMip = mip;
        device->CreateShaderResourceView(displacement_.Get(), &texSrv, displacementSrvs_[mip]);
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC texUav{};
    texUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    texUav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texUav.Texture2DArray.ArraySize = kArraySlices;
    texUav.Texture2DArray.FirstArraySlice = 0;
    texUav.Texture2DArray.PlaneSlice = 0;

    for (UINT mip = 0; mip < mipCount_; ++mip)
    {
        texUav.Texture2DArray.MipSlice = mip;
        device->CreateUnorderedAccessView(displacement_.Get(), nullptr, &texUav, displacementUavs_[mip]);
    }
}

void OceanSimulation::CreateMaterials(Renderer* renderer)
{
    if (!renderer)
    {
        return;
    }

    auto* materialMgr = renderer->GetMaterialManager();

    Material::ComputeDesc spectrumDesc{};
    spectrumDesc.shaderFile = L"shaders/ocean_time_spectrum.hlsl";
    spectrumDesc.csEntry = "CalculateAmplitudes";
    spectrumMaterial_ = materialMgr->GetOrCreateCompute(renderer, spectrumDesc);

    Material::ComputeDesc fftDesc{};
    fftDesc.shaderFile = L"shaders/ocean_fft.hlsl";
    fftDesc.csEntry = "Fft";
    fftDesc.defines.emplace_back("FFT_SIZE", std::to_string(kResolution));
    const uint32_t logSize = static_cast<uint32_t>(std::log2(static_cast<float>(kResolution)));
    fftDesc.defines.emplace_back("FFT_LOG_SIZE", std::to_string(logSize));
    fftMaterial_ = materialMgr->GetOrCreateCompute(renderer, fftDesc);

    Material::ComputeDesc fftPostDesc = fftDesc;
    fftPostDesc.csEntry = "PostProcess";
    fftPostMaterial_ = materialMgr->GetOrCreateCompute(renderer, fftPostDesc);

    Material::ComputeDesc mipDesc{};
    mipDesc.shaderFile = L"shaders/ocean_generate_mips.hlsl";
    mipDesc.csEntry = "GenerateMip";
    mipMaterial_ = materialMgr->GetOrCreateCompute(renderer, mipDesc);
}

void OceanSimulation::BuildSpectrum()
{
    const size_t perCascade = size_t(kResolution) * size_t(kResolution);
    const size_t total = perCascade * size_t(kCascadeCount);

    h0Data_.resize(total);
    waveData_.resize(total);

    std::vector<Math::float2> h0Seed(total);

    std::mt19937 rng(1337u);
    std::normal_distribution<float> gauss(0.0f, 1.0f);

    const Math::float2 windDirNorm = windDir_.Normalized();

    auto SetComponent = [](Math::float4& v, UINT index, float value)
    {
        float* data = &v.x;
        if (index < 4)
        {
            data[index] = value;
        }
    };

    for (UINT cascade = 0; cascade < kCascadeCount; ++cascade)
    {
        const float patchLength = basePatchLength_ * std::pow(2.0f, static_cast<float>(cascade));
        SetComponent(lengthScales_, cascade, patchLength);
        SetComponent(invLengthScales_, cascade, patchLength > Math::EPS ? (1.0f / patchLength) : 0.0f);

        const float twoPiOverL = Math::TWO_PI / patchLength;
        const float lambda = kChoppiness;

        for (UINT y = 0; y < kResolution; ++y)
        {
            const int iy = static_cast<int>(y) - static_cast<int>(kResolution / 2);
            for (UINT x = 0; x < kResolution; ++x)
            {
                const int ix = static_cast<int>(x) - static_cast<int>(kResolution / 2);
                const size_t idx = size_t(cascade) * perCascade + size_t(y) * size_t(kResolution) + size_t(x);

                Math::float2 kVec(float(ix) * twoPiOverL, float(iy) * twoPiOverL);
                const float kLen = std::sqrt(kVec.x * kVec.x + kVec.y * kVec.y);

                if (kLen < Math::EPS)
                {
                    h0Seed[idx] = Math::float2(0.0f, 0.0f);
                    waveData_[idx] = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
                    continue;
                }

                Math::float2 kNorm = kVec / kLen;
                float kDotW = Math::Clamp(kNorm.Dot(windDirNorm), -1.0f, 1.0f);
                const float spectrum = PhillipsSpectrum(kLen, kDotW, windSpeed_, patchLength, spectrumScale_);
                const float sigma = std::sqrt(spectrum) * 0.70710678f;

                h0Seed[idx] = Math::float2(gauss(rng) * sigma, gauss(rng) * sigma);

                const float omega = std::sqrt(kGravity * kLen);
                waveData_[idx] = Math::float4(kVec.x, lambda, kVec.y, omega);
            }
        }
    }

    for (UINT cascade = 0; cascade < kCascadeCount; ++cascade)
    {
        for (UINT y = 0; y < kResolution; ++y)
        {
            for (UINT x = 0; x < kResolution; ++x)
            {
                const size_t idx = size_t(cascade) * perCascade + size_t(y) * size_t(kResolution) + size_t(x);

                const UINT negX = (kResolution - x) % kResolution;
                const UINT negY = (kResolution - y) % kResolution;
                const size_t negIdx = size_t(cascade) * perCascade + size_t(negY) * size_t(kResolution) + size_t(negX);

                const Math::float2 h0 = h0Seed[idx];
                const Math::float2 h0Neg = h0Seed[negIdx];
                h0Data_[idx] = Math::float4(h0.x, h0.y, h0Neg.x, -h0Neg.y);
            }
        }
    }
}

void OceanSimulation::Update(Renderer* renderer, ID3D12GraphicsCommandList* cl, float timeSeconds)
{
    if (!renderer || !cl)
    {
        return;
    }

    if (!initialized_)
    {
        Initialize(renderer, cl, nullptr);
    }

    const float simTime = timeSeconds * timeScale_;

    DispatchSpectrum(renderer, cl, simTime);
    DispatchFFT(renderer, cl);
    DispatchFFTPost(renderer, cl);
    GenerateMips(renderer, cl);

    const D3D12_RESOURCE_STATES srvState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    renderer->Transition(cl, displacement_.Get(), srvState);
}

void OceanSimulation::DispatchSpectrum(Renderer* renderer, ID3D12GraphicsCommandList* cl, float timeSeconds)
{
    if (!spectrumMaterial_)
    {
        return;
    }

    renderer->Transition(cl, displacement_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = ctxHandle.ref();

    ctx.constants[0] = {
        kResolution,
        kCascadeCount,
        FloatToBits(timeSeconds),
        kResolution * kResolution
    };

    auto srvTable = renderer->StageSrvUavTable({ h0Srv_, waveDataSrv_ });
    auto uavTable = renderer->StageSrvUavTable({ displacementUavs_.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{} : displacementUavs_[0] });

    ctx.table[0] = srvTable.gpu;
    ctx.table[1] = uavTable.gpu;

    spectrumMaterial_->Bind(cl, ctx);

    const UINT groups = (kResolution + kThreadGroupSize - 1u) / kThreadGroupSize;
    cl->Dispatch(groups, groups, kCascadeCount);

    renderer->UAVBarrier(cl, displacement_.Get());
}

void OceanSimulation::DispatchFFT(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!fftMaterial_)
    {
        return;
    }

    renderer->Transition(cl, displacement_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    auto uavTable = renderer->StageSrvUavTable({ displacementUavs_.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{} : displacementUavs_[0] });

    {
        auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
        auto& ctx = ctxHandle.ref();
        ctx.constants[0] = {
            kArraySlices,
            0u,
            1u,
            0u,
        };
        ctx.table[0] = uavTable.gpu;
        fftMaterial_->Bind(cl, ctx);
        cl->Dispatch(1, kResolution, 1);
    }

    renderer->UAVBarrier(cl, displacement_.Get());

    {
        auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
        auto& ctx = ctxHandle.ref();
        ctx.constants[0] = {
            kArraySlices,
            1u,
            1u,
            0u,
        };
        ctx.table[0] = uavTable.gpu;
        fftMaterial_->Bind(cl, ctx);
        cl->Dispatch(1, kResolution, 1);
    }

    renderer->UAVBarrier(cl, displacement_.Get());
}

void OceanSimulation::DispatchFFTPost(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!fftPostMaterial_)
    {
        return;
    }

    renderer->Transition(cl, displacement_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    auto uavTable = renderer->StageSrvUavTable({ displacementUavs_.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{} : displacementUavs_[0] });

    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = ctxHandle.ref();
    ctx.constants[0] = {
        kArraySlices,
        1u,
        1u,
        0u,
    };
    ctx.table[0] = uavTable.gpu;

    fftPostMaterial_->Bind(cl, ctx);

    const UINT groups = (kResolution + kThreadGroupSize - 1u) / kThreadGroupSize;
    cl->Dispatch(groups, groups, 1);

    renderer->UAVBarrier(cl, displacement_.Get());
}

void OceanSimulation::GenerateMips(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!mipMaterial_ || mipCount_ <= 1)
    {
        return;
    }

    renderer->Transition(cl, displacement_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    UINT srcWidth = kResolution;
    UINT srcHeight = kResolution;

    for (UINT mip = 1; mip < mipCount_; ++mip)
    {
        const UINT dstWidth = std::max<UINT>(1u, srcWidth / 2u);
        const UINT dstHeight = std::max<UINT>(1u, srcHeight / 2u);

        auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
        auto& ctx = ctxHandle.ref();

        ctx.constants[0] = {
            srcWidth,
            srcHeight,
            dstWidth,
            dstHeight,
            kArraySlices,
            mip,
            0u,
            0u
        };

        auto srvTable = renderer->StageSrvUavTable({ displacementSrvs_[mip - 1] });
        auto uavTable = renderer->StageSrvUavTable({ displacementUavs_[mip] });

        ctx.table[0] = srvTable.gpu;
        ctx.table[1] = uavTable.gpu;

        mipMaterial_->Bind(cl, ctx);

        const UINT groupsX = (dstWidth + kThreadGroupSize - 1u) / kThreadGroupSize;
        const UINT groupsY = (dstHeight + kThreadGroupSize - 1u) / kThreadGroupSize;
        cl->Dispatch(groupsX, groupsY, kArraySlices);

        renderer->UAVBarrier(cl, displacement_.Get());

        srcWidth = dstWidth;
        srcHeight = dstHeight;
    }
}
