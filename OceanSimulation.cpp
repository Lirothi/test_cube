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
#include "OceanSpectrum.h"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr UINT kThreadGroupSize = 8;
    constexpr UINT kFftFlagPermute = 1u << 1;
}

OceanSimulation::OceanSimulation()
{
    InitializeDefaultAssets();
    RefreshDerivedSettings();
}

void OceanSimulation::InitializeDefaultAssets()
{
    defaultSettings_ = OceanSimulationSettings();
    defaultSettings_.SetResolution(OceanSimulationSettings::ResolutionValue::Eight);
    defaultSettings_.SetCascadeCount(OceanSimulationSettings::CascadesNumberValue::Four);
    defaultSettings_.SetAnisotropyLevel(6u);
    defaultSettings_.SetSimulateFoam(false);
    defaultSettings_.SetUpdateSpectrum(false);
    defaultSettings_.SetReadbackMode(OceanSimulationSettings::ReadbackCascadesMode::None);
    defaultSettings_.SetSamplingIterations(3u);
    defaultSettings_.SetDomainsMode(OceanSimulationSettings::CascadeDomainsMode::Auto);
    defaultSettings_.SetSimulationScale(600.0f);
    defaultSettings_.SetAllowOverlap(true);
    defaultSettings_.SetMinWavesInCascade(6.0f);
    defaultSettings_.SetManualLengthScales(Math::float4(65.72f, 12.0f, 12.84f, 5.62f));

    settings_ = defaultSettings_;

    defaultEqualizerPreset_ = std::make_shared<EqualizerPreset>();
    defaultEqualizerPreset_->SetScaleFilters({
        { EqualizerPreset::FilterType::Lowshelf, -1.5f, -0.55f, 2.38f }
    });
    defaultEqualizerPreset_->SetChopFilters({
        { EqualizerPreset::FilterType::Bell, 0.0f, 0.4f, 0.77f }
    });

    defaultSwellPreset_ = std::make_shared<SwellPreset>();
    SpectrumParams swellSpectrum = defaultSwellPreset_->GetSpectrum();
    swellSpectrum.energySpectrum = SpectrumParams::EnergySpectrumModel::PM;
    swellSpectrum.windSpeed = 6.3f;
    swellSpectrum.fetch = 100.0f;
    swellSpectrum.peaking = 3.0f;
    swellSpectrum.scale = 0.2f;
    swellSpectrum.cutoffWavelength = 0.01f;
    swellSpectrum.alignment = 0.8f;
    swellSpectrum.extraAlignment = 0.5f;
    defaultSwellPreset_->SetSpectrum(swellSpectrum);
    defaultSwellPreset_->SetReferenceWaveHeight(0.0f);

    defaultLocalPresets_.clear();
    defaultLocalPresets_.reserve(6);

    const auto makeSpectrum = [](float windSpeed,
                                 float fetch,
                                 float peaking,
                                 float scale,
                                 float cutoff,
                                 float alignment,
                                 float extraAlignment)
    {
        SpectrumParams spectrum = SpectrumParams::GetDefaultLocal();
        spectrum.energySpectrum = SpectrumParams::EnergySpectrumModel::PM;
        spectrum.windSpeed = windSpeed;
        spectrum.fetch = fetch;
        spectrum.peaking = peaking;
        spectrum.scale = scale;
        spectrum.cutoffWavelength = cutoff;
        spectrum.alignment = alignment;
        spectrum.extraAlignment = extraAlignment;
        return spectrum;
    };

    const auto makeFoam = [](float decayRate,
                             float coverage,
                             float density,
                             float sharpness,
                             float persistence,
                             float trail,
                             float trailStrength,
                             const Math::float2& trailSize,
                             float underwater,
                             const Math::float4& cascadesWeights)
    {
        FoamParams foam = FoamParams::GetDefault();
        foam.decayRate = decayRate;
        foam.coverage = coverage;
        foam.density = density;
        foam.sharpness = sharpness;
        foam.persistence = persistence;
        foam.trail = trail;
        foam.trailTextureStrength = trailStrength;
        foam.trailTextureSize = trailSize;
        foam.underwater = underwater;
        foam.cascadesWeights = cascadesWeights;
        return foam;
    };

    const auto addLocalPreset = [&](float windForce,
                                     const SpectrumParams& spectrum,
                                     float referenceWaveHeight,
                                     float chop,
                                     const FoamParams& foam)
    {
        auto preset = std::make_shared<LocalWavesPreset>();
        preset->SetSpectrum(spectrum);
        preset->SetReferenceWaveHeight(referenceWaveHeight);
        preset->SetChop(chop);
        preset->SetFoam(foam);
        preset->SetEqualizer(defaultEqualizerPreset_);
        preset->SetWindForce(windForce);
        defaultLocalPresets_.push_back(preset);
        return preset;
    };

    FoamParams calmFoam = FoamParams::GetDefault();
    calmFoam.cascadesWeights = Math::float4(1.0f, 1.0f, 1.0f, 1.0f);

    defaultLocalPreset_ = addLocalPreset(0.0f,
        makeSpectrum(4.0f, 100.0f, 3.0f, 1.0f, 0.01f, 1.0f, 0.0f),
        1.0f,
        1.0f,
        calmFoam);

    addLocalPreset(1.0f,
        makeSpectrum(1.5f, 100.0f, 3.3f, 0.248f, 0.01f, 1.0f, 0.0f),
        0.0f,
        1.0f,
        calmFoam);

    addLocalPreset(2.0f,
        makeSpectrum(2.5f, 100.0f, 3.3f, 1.0f, 0.01f, 1.0f, 0.0f),
        0.26f,
        1.25f,
        calmFoam);

    addLocalPreset(3.0f,
        makeSpectrum(4.5f, 100.0f, 3.3f, 1.0f, 0.01f, 1.0f, 0.0f),
        0.93f,
        1.41f,
        makeFoam(0.2f, 0.662f, 13.2f, 1.0f, 0.768f, 0.0f, 0.503f,
            Math::float2(50.0f, 25.0f), 0.407f, Math::float4(1.0f, 1.0f, 0.5f, 0.3f)));

    addLocalPreset(4.0f,
        makeSpectrum(7.0f, 100.0f, 3.3f, 1.0f, 0.01f, 1.0f, 0.0f),
        1.84f,
        1.44f,
        makeFoam(0.1f, 0.61f, 19.38f, 0.651f, 0.746f, 0.0f, 0.5f,
            Math::float2(100.0f, 50.0f), 0.46f, Math::float4(2.0f, 1.0f, 0.3f, 0.2f)));

    addLocalPreset(5.0f,
        makeSpectrum(9.2f, 100.0f, 3.3f, 1.0f, 0.01f, 1.0f, 0.0f),
        3.4f,
        1.49f,
        makeFoam(0.02f, 0.575f, 13.99f, 0.5f, 0.762f, 0.265f, 0.5f,
            Math::float2(100.0f, 50.0f), 0.476f, Math::float4(3.0f, 1.0f, 0.3f, 0.2f)));

    inputsProvider_ = OceanSimulationInputsProvider();
    inputsProvider_.SetMode(OceanSimulationInputsProvider::InputsProviderMode::Scale);
    inputsProvider_.SetTimeScale(1.0f);
    inputsProvider_.SetDepth(1000.0f);
    inputsProvider_.SetSwellPreset(defaultSwellPreset_);
    inputsProvider_.SetLocalWavesPreset(defaultLocalPreset_);
    inputsProvider_.SetLocalWavesArray(defaultLocalPresets_);
    inputsProvider_.SetDefaultEqualizer(defaultEqualizerPreset_);

    windForce01_ = 0.8f;
    inputsProvider_.SetDisplayWindForce(windForce01_);
}

void OceanSimulation::SetSettings(const OceanSimulationSettings& settings)
{
    settings_ = settings;
    RefreshDerivedSettings();

    initialized_ = false;
    ReleaseCpuData();

    h0Buffer_.Reset();
    waveDataBuffer_.Reset();
    displacement_.Reset();
    descriptorHeap_.Reset();
    descriptorIncr_ = 0;
    h0Srv_ = {};
    waveDataSrv_ = {};
    displacementSrvs_.clear();
    displacementUavs_.clear();

    spectrumMaterial_.reset();
    fftMaterial_.reset();
    fftPostMaterial_.reset();
    mipMaterial_.reset();

    mipCount_ = 1u;
}

void OceanSimulation::SetInputsProvider(const OceanSimulationInputsProvider& provider)
{
    inputsProvider_ = provider;
    inputsProvider_.SetDisplayWindForce(windForce01_);
    initialized_ = false;
}

void OceanSimulation::SetSceneVariables(float localWindDirectionDegrees, float swellDirectionDegrees, float windForce01)
{
    localWindDirection_ = localWindDirectionDegrees;
    swellDirection_ = swellDirectionDegrees;
    windForce01_ = Math::Saturate(windForce01);
    inputsProvider_.SetDisplayWindForce(windForce01_);
    initialized_ = false;
}

void OceanSimulation::RefreshDerivedSettings()
{
    resolution_ = settings_.GetResolution();
    cascadeCount_ = settings_.GetCascadeCount();
    arraySliceCount_ = cascadeCount_ * 2u;

    lengthScales_ = settings_.ComputeLengthScales();
    invLengthScales_ = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);

    float* invData = &invLengthScales_.x;
    float* lengthData = &lengthScales_.x;
    for (UINT i = 0; i < kClipLevels; ++i)
    {
        if (i < cascadeCount_ && lengthData[i] > Math::EPS)
        {
            invData[i] = 1.0f / lengthData[i];
        }
        else
        {
            invData[i] = 0.0f;
            if (i >= cascadeCount_)
            {
                lengthData[i] = 0.0f;
            }
        }
    }

    basePatchLength_ = (cascadeCount_ > 0 && lengthData[0] > Math::EPS) ? lengthData[0] : 0.0f;

    settings_.CalculateCascadeDomains(cutoffsLow_, cutoffsHigh_);
}

float OceanSimulation::ComputeCascadeContribution(float kLength, UINT cascade) const
{
    if (cascade >= cascadeCount_)
    {
        return 0.0f;
    }

    const float* low = &cutoffsLow_.x;
    const float* high = &cutoffsHigh_.x;

    const float cascadeHigh = high[cascade];
    const float cascadeLow = low[cascade];
    if (cascadeHigh <= Math::EPS || kLength > cascadeHigh || kLength < cascadeLow)
    {
        return 0.0f;
    }

    float total = 0.0f;
    for (UINT i = 0; i < cascadeCount_; ++i)
    {
        const float highValue = high[i];
        if (highValue <= Math::EPS)
        {
            continue;
        }

        if (kLength <= highValue && kLength >= low[i])
        {
            total += 1.0f;
        }
    }

    if (total <= Math::EPS)
    {
        return 0.0f;
    }

    return 1.0f / total;
}

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

    ReleaseCpuData();

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
    if (!renderer || resolution_ == 0 || cascadeCount_ == 0)
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
    UINT size = std::max<UINT>(1u, resolution_);
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
    texDesc.Width = resolution_;
    texDesc.Height = resolution_;
    texDesc.DepthOrArraySize = static_cast<UINT16>(arraySliceCount_);
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
    if (!device || !displacement_ || arraySliceCount_ == 0)
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
    texSrv.Texture2DArray.ArraySize = arraySliceCount_;
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
    texUav.Texture2DArray.ArraySize = arraySliceCount_;
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
    if (!renderer || resolution_ == 0)
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
    fftDesc.defines.emplace_back("FFT_SIZE", std::to_string(resolution_));
    const float logSizeFloat = std::log2(static_cast<float>(std::max<UINT>(1u, resolution_)));
    const uint32_t logSize = static_cast<uint32_t>(std::round(logSizeFloat));
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
    RefreshDerivedSettings();

    inputsProvider_.SetDisplayWindForce(windForce01_);
    inputsProvider_.PopulateInputs(inputs_, windForce01_);

    timeScale_ = inputs_.timeScale;
    waterDepth_ = inputs_.depth;
    chopValue_ = inputs_.chop;
    displacementAmplitude_ = inputs_.referenceWaveHeight;

    auto equalizer0 = inputs_.equalizerRamp0 ? inputs_.equalizerRamp0 : EqualizerPreset::CreateDefault();
    auto equalizer1 = inputs_.equalizerRamp1 ? inputs_.equalizerRamp1 : EqualizerPreset::CreateDefault();

    if (resolution_ == 0 || cascadeCount_ == 0)
    {
        ReleaseCpuData();
        return;
    }

    const size_t perCascade = size_t(resolution_) * size_t(resolution_);
    const size_t total = perCascade * size_t(cascadeCount_);

    h0Data_.assign(total, Math::float4(0.0f, 0.0f, 0.0f, 0.0f));
    waveData_.assign(total, Math::float4(0.0f, 0.0f, 0.0f, 0.0f));

    std::vector<Math::float2> h0Seed(total, Math::float2(0.0f, 0.0f));

    std::mt19937 rng(1337u);
    std::normal_distribution<float> gauss(0.0f, 1.0f);

    const float* lengthData = &lengthScales_.x;
    const float localWindRad = localWindDirection_ * Math::DEG2RAD;
    const float swellWindRad = swellDirection_ * Math::DEG2RAD;
    const SpectrumParams localSpectrum = inputs_.local;
    const SpectrumParams swellSpectrum = inputs_.swell;
    const bool hasSwell = swellSpectrum.scale > Math::EPS;
    const float equalizerRange = std::max(EqualizerPreset::kXMax - EqualizerPreset::kXMin, Math::EPS);

    for (UINT cascade = 0; cascade < cascadeCount_; ++cascade)
    {
        const float patchLength = lengthData[cascade];
        if (patchLength <= Math::EPS)
        {
            continue;
        }

        const float deltaK = Math::TWO_PI / patchLength;

        for (UINT y = 0; y < resolution_; ++y)
        {
            const int iy = static_cast<int>(y) - static_cast<int>(resolution_ / 2);
            for (UINT x = 0; x < resolution_; ++x)
            {
                const int ix = static_cast<int>(x) - static_cast<int>(resolution_ / 2);
                const size_t idx = size_t(cascade) * perCascade + size_t(y) * size_t(resolution_) + size_t(x);

                Math::float2 kVec(float(ix) * deltaK, float(iy) * deltaK);
                const float kLen = std::sqrt(kVec.x * kVec.x + kVec.y * kVec.y);

                if (kLen < Math::EPS)
                {
                    waveData_[idx] = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
                    continue;
                }

                const float contribution = ComputeCascadeContribution(kLen, cascade);
                if (contribution <= 0.0f)
                {
                    waveData_[idx] = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
                    continue;
                }

                const float theta = std::atan2(kVec.y, kVec.x);
                const float omega = OceanSpectrum::Frequency(kLen, waterDepth_);
                if (omega <= Math::EPS)
                {
                    waveData_[idx] = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
                    continue;
                }

                float spectrum = OceanSpectrum::FullSpectrum(omega, theta - localWindRad, localSpectrum, waterDepth_);
                spectrum *= localSpectrum.scale;
                spectrum *= OceanSpectrum::ShortWavesFade(kLen, localSpectrum.cutoffWavelength);

                if (hasSwell)
                {
                    float swellValue = OceanSpectrum::FullSpectrum(omega, theta - swellWindRad, swellSpectrum, waterDepth_);
                    swellValue *= swellSpectrum.scale;
                    swellValue *= OceanSpectrum::ShortWavesFade(kLen, swellSpectrum.cutoffWavelength);
                    spectrum += swellValue;
                }

                spectrum = std::max(spectrum, 0.0f);

                const float dOmegadk = OceanSpectrum::FrequencyDerivative(kLen, waterDepth_);
                const float spectralFactor = std::max(0.0f, 2.0f * spectrum * std::abs(dOmegadk) / kLen);

                const float logTerm = std::log10(Math::TWO_PI / kLen);
                float rampU = (logTerm - EqualizerPreset::kXMin) / equalizerRange;
                rampU = Math::Saturate(rampU);
                const Math::float2 eq0Sample = equalizer0->Sample(rampU);
                const Math::float2 eq1Sample = equalizer1->Sample(rampU);
                const Math::float2 eqSample = Math::float2::Lerp(eq0Sample, eq1Sample, inputs_.equalizerLerpValue);

                const float scaleRamp = eqSample.x;
                const float lambda = chopValue_ * eqSample.y;

                const float amplitude = contribution * scaleRamp * std::sqrt(spectralFactor) * deltaK;
                waveData_[idx] = Math::float4(kVec.x, lambda, kVec.y, omega);

                if (amplitude <= Math::EPS)
                {
                    h0Seed[idx] = Math::float2(0.0f, 0.0f);
                    continue;
                }

                h0Seed[idx] = Math::float2(gauss(rng) * amplitude, gauss(rng) * amplitude);
            }
        }
    }

    for (UINT cascade = 0; cascade < cascadeCount_; ++cascade)
    {
        for (UINT y = 0; y < resolution_; ++y)
        {
            for (UINT x = 0; x < resolution_; ++x)
            {
                const size_t idx = size_t(cascade) * perCascade + size_t(y) * size_t(resolution_) + size_t(x);

                const UINT negX = (resolution_ - x) % resolution_;
                const UINT negY = (resolution_ - y) % resolution_;
                const size_t negIdx = size_t(cascade) * perCascade + size_t(negY) * size_t(resolution_) + size_t(negX);

                const Math::float2 h0 = h0Seed[idx];
                const Math::float2 h0Neg = h0Seed[negIdx];
                h0Data_[idx] = Math::float4(h0.x, h0.y, h0Neg.x, -h0Neg.y);
            }
        }
    }
}

void OceanSimulation::ReleaseCpuData()
{
    std::vector<Math::float4>().swap(h0Data_);
    std::vector<Math::float4>().swap(waveData_);
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
    if (!spectrumMaterial_ || resolution_ == 0 || cascadeCount_ == 0)
    {
        return;
    }

    renderer->Transition(cl, displacement_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = ctxHandle.ref();

    ctx.constants[0] = {
        resolution_,
        cascadeCount_,
        FloatToBits(timeSeconds),
        resolution_ * resolution_
    };

    auto srvTable = renderer->StageSrvUavTable({ h0Srv_, waveDataSrv_ });
    auto uavTable = renderer->StageSrvUavTable({ displacementUavs_.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{} : displacementUavs_[0] });

    ctx.table[0] = srvTable.gpu;
    ctx.table[1] = uavTable.gpu;

    spectrumMaterial_->Bind(cl, ctx);

    const UINT groups = (resolution_ + kThreadGroupSize - 1u) / kThreadGroupSize;
    cl->Dispatch(groups, groups, cascadeCount_);

    renderer->UAVBarrier(cl, displacement_.Get());
}

void OceanSimulation::DispatchFFT(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!fftMaterial_ || arraySliceCount_ == 0 || resolution_ == 0)
    {
        return;
    }

    renderer->Transition(cl, displacement_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    auto uavTable = renderer->StageSrvUavTable({ displacementUavs_.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{} : displacementUavs_[0] });

    {
        auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
        auto& ctx = ctxHandle.ref();
        ctx.constants[0] = {
            arraySliceCount_,
            0u,
            1u,
            0u,
        };
        ctx.table[1] = uavTable.gpu;
        fftMaterial_->Bind(cl, ctx);
        cl->Dispatch(1, resolution_, 1);
    }

    renderer->UAVBarrier(cl, displacement_.Get());

    {
        auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
        auto& ctx = ctxHandle.ref();
        ctx.constants[0] = {
            arraySliceCount_,
            1u,
            1u,
            0u,
        };
        ctx.table[1] = uavTable.gpu;
        fftMaterial_->Bind(cl, ctx);
        cl->Dispatch(1, resolution_, 1);
    }

    renderer->UAVBarrier(cl, displacement_.Get());
}

void OceanSimulation::DispatchFFTPost(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!fftPostMaterial_ || arraySliceCount_ == 0 || resolution_ == 0)
    {
        return;
    }

    renderer->Transition(cl, displacement_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    auto uavTable = renderer->StageSrvUavTable({ displacementUavs_.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{} : displacementUavs_[0] });

    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = ctxHandle.ref();
    ctx.constants[0] = {
        arraySliceCount_,
        1u,
        1u,
        kFftFlagPermute,
    };
    ctx.table[1] = uavTable.gpu;

    fftPostMaterial_->Bind(cl, ctx);

    const UINT groups = (resolution_ + kThreadGroupSize - 1u) / kThreadGroupSize;
    cl->Dispatch(groups, groups, 1);

    renderer->UAVBarrier(cl, displacement_.Get());
}

void OceanSimulation::GenerateMips(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!mipMaterial_ || mipCount_ <= 1 || arraySliceCount_ == 0 || resolution_ == 0)
    {
        return;
    }

    renderer->Transition(cl, displacement_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    UINT srcWidth = resolution_;
    UINT srcHeight = resolution_;

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
            arraySliceCount_,
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
        cl->Dispatch(groupsX, groupsY, arraySliceCount_);

        renderer->UAVBarrier(cl, displacement_.Get());

        srcWidth = dstWidth;
        srcHeight = dstHeight;
    }
}
