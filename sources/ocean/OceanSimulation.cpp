#include "ocean/OceanSimulation.h"
#include "rendering/core/TextureCreate.h"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "core/Helpers.h"
#include "core/diagnostics/DiagPaths.h"
#include "materials/Material.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/RenderContextPool.h"
#include "rendering/core/UploadManager.h"
#include "ocean/OceanSimulationConfig.h"
#include "ocean/OceanSpectrum.h"
#include "app/camera/Camera.h"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr UINT kThreadGroupSize = 8;
    constexpr UINT kFftFlagPermute = 1u << 1;
    constexpr UINT kMipsPerDispatch = 4u;

    bool Float4Equal(const Math::float4& lhs, const Math::float4& rhs)
    {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
    }

    bool SettingsEqual(const OceanSimulationSettings& lhs, const OceanSimulationSettings& rhs)
    {
        return lhs.GetResolution() == rhs.GetResolution() &&
            lhs.GetCascadeCount() == rhs.GetCascadeCount() &&
            lhs.GetAnisotropyLevel() == rhs.GetAnisotropyLevel() &&
            lhs.ShouldUpdateSpectrum() == rhs.ShouldUpdateSpectrum() &&
            lhs.ShouldSimulateFoam() == rhs.ShouldSimulateFoam() &&
            lhs.GetReadbackMode() == rhs.GetReadbackMode() &&
            lhs.GetSamplingIterations() == rhs.GetSamplingIterations() &&
            lhs.GetDomainsMode() == rhs.GetDomainsMode() &&
            lhs.GetSimulationScale() == rhs.GetSimulationScale() &&
            lhs.AllowOverlap() == rhs.AllowOverlap() &&
            lhs.GetMinWavesInCascade() == rhs.GetMinWavesInCascade() &&
            Float4Equal(lhs.GetManualLengthScales(), rhs.GetManualLengthScales());
    }
}

OceanSimulation::OceanSimulation()
{
    InitializeFromConfig(L"data/ocean/default.json");
}

OceanSimulation::OceanSimulation(const std::wstring& configPath)
{
    InitializeFromConfig(configPath);
}

void OceanSimulation::InitializeFromConfig(const std::wstring& configPath)
{
    OceanSimulationConfig config;
    configPath_ = configPath.empty() ? L"data/ocean/default.json" : configPath;
    const bool loaded = LoadOceanSimulationConfigFromFile(configPath_, config);
    assert(loaded && "Ocean config file not found or invalid JSON");

    ApplyConfigInternal(nullptr, loaded ? config : OceanSimulationConfig(), false);
    RefreshDerivedSettings();
}

void OceanSimulation::SetSettings(Renderer* renderer, const OceanSimulationSettings& settings)
{
    settings_ = settings;
    config_.settings = settings_;
    RefreshDerivedSettings();

    ResetGpuResources(renderer, true);
}

void OceanSimulation::ResetInitialSpectrum(Renderer* renderer)
{
    RefreshDerivedSettings();
    ResetGpuResources(renderer, false);
}

void OceanSimulation::ResetGpuResources(Renderer* renderer, bool resetMaterials)
{
    CollectRetiredResources(renderer);
    RetireGpuResources(renderer);

    initialized_ = false;
    ReleaseCpuData();

    descriptorHeap_.Reset();
    descriptorIncr_ = 0;
    h0Srv_ = {};
    waveDataSrv_ = {};
    displacementFullSrv_ = {};
    displacementSrvs_.clear();
    prevDisplacementSrv_ = {};
    shoreDepthSrv_ = {};
    displacementUavs_.clear();
    foamSrvs_.clear();
    foamUavs_.clear();
    foamSrv_ = {};
    foamUav_ = {};

    if (resetMaterials)
    {
        spectrumMaterial_.reset();
        fftMaterial_.reset();
        fftPostMaterial_.reset();
        mipMaterial_.reset();
        foamSimMaterial_.reset();
        foamInitMaterial_.reset();
    }

    mipCount_ = 1u;
    mipExtents_.clear();
    foamNeedsInit_ = true;
    hasDisplacementHistory_ = false;
    prevDisplacementValid_ = false;
    lastFoamSimTime_ = 0.0f;
}

void OceanSimulation::RetireGpuResources(Renderer* renderer)
{
    RetiredGpuResources retired{};
    retired.retireFrame = renderer ? renderer->GetTotalFrameNumber() : 0u;

    auto clearState = [renderer](const ComPtr<ID3D12Resource>& resource)
    {
        if (renderer && resource)
        {
            renderer->ClearResourceState(resource.Get());
        }
    };

    // displacement_/prevDisplacement_/foamTurbulence_ are GpuResource now: their registration
    // dies with the retired copy, which is LATER and more correct than clearing at retire time.
    // This hand-written list is exactly what used to forget shoreDepth_.
    clearState(h0Buffer_);
    clearState(waveDataBuffer_);

    retired.h0Buffer = std::move(h0Buffer_);
    retired.waveDataBuffer = std::move(waveDataBuffer_);
    retired.displacement = std::move(displacement_);
    retired.prevDisplacement = std::move(prevDisplacement_);
    retired.foamTurbulence = std::move(foamTurbulence_);

    if (retired.h0Buffer || retired.waveDataBuffer || retired.displacement ||
        retired.prevDisplacement || retired.foamTurbulence)
    {
        retiredGpuResources_.push_back(std::move(retired));
    }
}

void OceanSimulation::RetireUploadResources(Renderer* renderer, std::vector<ComPtr<ID3D12Resource>> resources)
{
    if (resources.empty())
    {
        return;
    }

    RetiredUploadResources retired{};
    retired.retireFrame = renderer ? renderer->GetTotalFrameNumber() : 0u;
    retired.resources = std::move(resources);
    retiredUploadResources_.push_back(std::move(retired));
}

void OceanSimulation::CollectRetiredResources(Renderer* renderer)
{
    if (!renderer)
    {
        return;
    }

    constexpr uint64_t kKeepAliveFrames = render::kFrameCount + 1u;
    const uint64_t frameNumber = renderer->GetTotalFrameNumber();
    const auto canRelease = [frameNumber](uint64_t retireFrame)
    {
        return frameNumber > retireFrame + kKeepAliveFrames;
    };

    // Upload batches from EnsureFrameResources: the intermediates and the command allocator must
    // outlive the GPU work, and this is the one place that already knows the retirement rule.
    auto batchIt = pendingInitBatches_.begin();
    while (batchIt != pendingInitBatches_.end())
    {
        batchIt = canRelease(batchIt->submitFrame) ? pendingInitBatches_.erase(batchIt)
                                                   : std::next(batchIt);
    }

    auto gpuIt = retiredGpuResources_.begin();
    while (gpuIt != retiredGpuResources_.end())
    {
        if (canRelease(gpuIt->retireFrame))
        {
            gpuIt = retiredGpuResources_.erase(gpuIt);
        }
        else
        {
            ++gpuIt;
        }
    }

    auto uploadIt = retiredUploadResources_.begin();
    while (uploadIt != retiredUploadResources_.end())
    {
        if (canRelease(uploadIt->retireFrame))
        {
            uploadIt = retiredUploadResources_.erase(uploadIt);
        }
        else
        {
            ++uploadIt;
        }
    }
}

void OceanSimulation::SetInputsProvider(Renderer* renderer, const OceanSimulationInputsProvider& provider)
{
    inputsProvider_ = provider;
    inputsProvider_.SetDisplayWindForce(windForce01_);
    config_.inputMode = inputsProvider_.GetMode();
    config_.timeScale = inputsProvider_.GetTimeScale();
    config_.depth = inputsProvider_.GetDepth();
    config_.swellPreset = inputsProvider_.GetSwellPreset();
    config_.localPreset = inputsProvider_.GetLocalWavesPreset();
    config_.localPresets = inputsProvider_.GetLocalWavesPresets();
    config_.defaultEqualizer = inputsProvider_.GetDefaultEqualizer();
    config_.localPresetIndex = 0;
    for (size_t i = 0; i < config_.localPresets.size(); ++i)
    {
        if (config_.localPresets[i] == config_.localPreset)
        {
            config_.localPresetIndex = i;
            break;
        }
    }
    ResetInitialSpectrum(renderer);
}

void OceanSimulation::SetSceneVariables(Renderer* renderer, float localWindDirectionDegrees, float swellDirectionDegrees, float windForce01)
{
    localWindDirection_ = localWindDirectionDegrees;
    swellDirection_ = swellDirectionDegrees;
    windForce01_ = Math::Saturate(windForce01);
    config_.localWindDirectionDegrees = localWindDirection_;
    config_.swellDirectionDegrees = swellDirection_;
    config_.windForce01 = windForce01_;
    inputsProvider_.SetDisplayWindForce(windForce01_);
    ResetInitialSpectrum(renderer);
}

bool OceanSimulation::LoadConfig(Renderer* renderer, const std::wstring& path)
{
    OceanSimulationConfig config;
    if (!LoadOceanSimulationConfigFromFile(path, config))
    {
        return false;
    }

    configPath_ = path;
    ApplyConfig(renderer, config);
    return true;
}

bool OceanSimulation::SaveConfig(const std::wstring& path) const
{
    if (path.empty())
    {
        return false;
    }
    return SaveOceanSimulationConfigToFile(path, config_);
}

void OceanSimulation::ApplyConfig(Renderer* renderer, const OceanSimulationConfig& config)
{
    ApplyConfigInternal(renderer, config, true);
}

OceanSimulationConfig OceanSimulation::GetConfigCopy() const
{
    return CloneOceanSimulationConfig(config_);
}

void OceanSimulation::ApplyConfigInternal(Renderer* renderer, const OceanSimulationConfig& config, bool resetResources)
{
    const bool settingsChanged = !SettingsEqual(settings_, config.settings);
    config_ = CloneOceanSimulationConfig(config);

    defaultSettings_ = config_.settings;
    settings_ = config_.settings;

    defaultEqualizerPreset_ = config_.defaultEqualizer ? config_.defaultEqualizer : EqualizerPreset::CreateDefault();
    defaultSwellPreset_ = config_.swellPreset ? config_.swellPreset : std::make_shared<SwellPreset>();
    defaultLocalPreset_ = config_.localPreset ? config_.localPreset : std::make_shared<LocalWavesPreset>();
    defaultLocalPresets_ = config_.localPresets;
    if (defaultLocalPresets_.empty())
    {
        defaultLocalPresets_.push_back(defaultLocalPreset_);
    }

    inputsProvider_ = OceanSimulationInputsProvider();
    inputsProvider_.SetMode(config_.inputMode);
    inputsProvider_.SetTimeScale(config_.timeScale);
    inputsProvider_.SetDepth(config_.depth);
    inputsProvider_.SetSwellPreset(defaultSwellPreset_);
    inputsProvider_.SetLocalWavesPreset(defaultLocalPreset_);
    inputsProvider_.SetLocalWavesArray(defaultLocalPresets_);
    inputsProvider_.SetDefaultEqualizer(defaultEqualizerPreset_);

    localWindDirection_ = config_.localWindDirectionDegrees;
    swellDirection_ = config_.swellDirectionDegrees;
    windForce01_ = Math::Saturate(config_.windForce01);
    config_.windForce01 = windForce01_;
    inputsProvider_.SetDisplayWindForce(windForce01_);

    RefreshDerivedSettings();

    if (resetResources)
    {
        ResetGpuResources(renderer, settingsChanged);
    }

    prevShoreDepthPos_ = { FLT_MAX, FLT_MAX };
}

OceanSimulationInputs OceanSimulation::EvaluateInputs() const
{
    OceanSimulationInputs result;
    inputsProvider_.PopulateInputs(result, windForce01_);
    return result;
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
    for (UINT i = 0; i < cascadeCount_; ++i)
    {
        if (lengthData[i] > Math::EPS)
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

void OceanSimulation::EnsureFrameResources(Renderer* renderer)
{
    if (initialized_ || !renderer)
    {
        return;
    }

    // Creation must happen BEFORE the render graph runs its Prepares, never inside a record body.
    // It used to be lazy inside Update() — i.e. inside Main_ObjectCompute's body — so changing a
    // setting (wind force) released the sim's resources, Prepare then registered NULL pointers,
    // the body recreated them, and the transparent pass handed Renderer::Transition a resource the
    // barrier compile had never seen: "no compiled barrier for res=...". Same class as the
    // ShadowGpuData::RecordCull miss the comparator caught, and it only fires when a setting
    // changes, which is why the stress harness never reached it.
    //
    // Its own upload batch, because there is no pass command list this early.
    //
    // Submitted WITHOUT waiting. It used to be SubmitAndWait, on the reasoning that re-init only
    // happens on a user action — but dragging a slider IS a user action, once per frame, so that
    // stalled the GPU every frame of the drag and made the inspector unusable. The batch is instead
    // retained until its work has certainly completed, which is the same rule the retired-resource
    // list here already uses.
    auto batch = std::make_unique<UploadBatch>();
    if (!batch->Begin(renderer))
    {
        return;
    }
    Initialize(renderer, batch->CommandList(), nullptr);
    if (batch->Submit(renderer))
    {
        pendingInitBatches_.push_back({ std::move(batch), renderer->GetTotalFrameNumber() });
    }
}

void OceanSimulation::OnHotReload(Renderer* renderer)
{
    if (!renderer)
    {
        return;
    }
    CreateMaterials(renderer);
}

float2 OceanSimulation::GetShoreViewCenter() const
{
    return float2(shoreDepthView_.position.x, shoreDepthView_.position.z);
}

float OceanSimulation::GetShoreViewHeight() const
{
    return shoreDepthView_.position.y;
}

float2 OceanSimulation::GetShoreDepthRange() const
{
    return float2(shoreDepthView_.zNear, shoreDepthView_.zFar);
}

void OceanSimulation::UpdateShoreView(const Camera& camera)
{
    constexpr float kShoreNearPlane = 0.1f;
    constexpr float kShoreFarPlane = 50.0f;
    constexpr float kShoreHeight = 20.0f;

    // Camera-following window, as before: this map is the DETAIL one (foam, run-up, sink, water
    // colour all read its depth), so its texel budget goes where the camera is. Whole-level
    // coverage is the SDF's job, not this one's.
    const float halfExtent = shoreDepthHalfExtent_;
    const float shoreDepthResolution = static_cast<float>(std::max(shoreDepthWidth_, 1u));
    const float shoreTexelSize = (halfExtent * 2.0f) / shoreDepthResolution;
    const float shoreSnapStep = shoreTexelSize * shoreViewSnapMultiplier_;

    float3 cameraPosition = camera.GetPosition();
    cameraPosition.x = std::floor(cameraPosition.x / shoreSnapStep) * shoreSnapStep + shoreSnapStep * 0.5f;
    cameraPosition.z = std::floor(cameraPosition.z / shoreSnapStep) * shoreSnapStep + shoreSnapStep * 0.5f;

    SceneView& shoreView = shoreDepthView_;
    shoreView.type = SceneView::Type::ShoreDepth;
    shoreView.renderLayerMask = RenderLayerMask(RenderLayer::Terrain);
    shoreView.position = float3(cameraPosition.x, kShoreHeight, cameraPosition.z);
    shoreView.view = mat4::LookAtLH(shoreView.position, shoreView.position + float3(0.0f, -1.0f, 0.0f), float3(0.0f, 0.0f, 1.0f));
    shoreView.proj = mat4::OrthoOffCenterLH(-halfExtent, halfExtent, -halfExtent, halfExtent, kShoreNearPlane, kShoreFarPlane);
    shoreView.invView = mat4::Inverse(shoreView.view);
    shoreView.invProj = mat4::Inverse(shoreView.proj);
    shoreView.frustum = Frustum::FromOrthoBounds(shoreView.invView, halfExtent, halfExtent, kShoreFarPlane - kShoreNearPlane,
        float3(shoreView.position.x, shoreView.position.y - (kShoreFarPlane - kShoreNearPlane) * 0.5f, shoreView.position.z) );
    shoreView.zNear = kShoreNearPlane;
    shoreView.zFar = kShoreFarPlane;
    shoreView.hfov = 0.0f;
    shoreView.requiresDepthCheck = false;

    shouldRenderShoreDepth_ = std::abs(cameraPosition.x - prevShoreDepthPos_.x) > 0.001f ||
                              std::abs(cameraPosition.z - prevShoreDepthPos_.y) > 0.001f;
    prevShoreDepthPos_ = float2(cameraPosition.x, cameraPosition.z);

    // The SDF's view never moves; it is rebuilt only when the level says where the terrain is.
    SceneView& sdfView = shoreSdfView_;
    const float sdfHalf = shoreSdfHalfExtent_;
    sdfView.type = SceneView::Type::ShoreDepth;
    sdfView.renderLayerMask = RenderLayerMask(RenderLayer::Terrain);
    sdfView.position = float3(shoreSdfCenter_.x, kShoreHeight, shoreSdfCenter_.y);
    sdfView.view = mat4::LookAtLH(sdfView.position, sdfView.position + float3(0.0f, -1.0f, 0.0f), float3(0.0f, 0.0f, 1.0f));
    sdfView.proj = mat4::OrthoOffCenterLH(-sdfHalf, sdfHalf, -sdfHalf, sdfHalf, kShoreNearPlane, kShoreFarPlane);
    sdfView.invView = mat4::Inverse(sdfView.view);
    sdfView.invProj = mat4::Inverse(sdfView.proj);
    sdfView.frustum = Frustum::FromOrthoBounds(sdfView.invView, sdfHalf, sdfHalf, kShoreFarPlane - kShoreNearPlane,
        float3(sdfView.position.x, sdfView.position.y - (kShoreFarPlane - kShoreNearPlane) * 0.5f, sdfView.position.z));
    sdfView.zNear = kShoreNearPlane;
    sdfView.zFar = kShoreFarPlane;
    sdfView.hfov = 0.0f;
    sdfView.requiresDepthCheck = false;
}

void OceanSimulation::BuildShoreSdf(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer || !cl || !shoreSdfSeedMaterial_ || !shoreSdfJumpMaterial_ ||
        !shoreSdfResolveMaterial_ || !shoreSdfJump_[0] || !shoreSdfJump_[1])
    {
        return;
    }

    // Must match UpdateShoreView's ortho setup — the shader decodes depth with these.
    constexpr float kShoreNearPlane = 0.1f;
    constexpr float kShoreFarPlane = 50.0f;
    constexpr float kShoreHeight = 20.0f;
    const float texelWorld = (shoreSdfHalfExtent_ * 2.0f) / static_cast<float>(kShoreSdfSize);
    const UINT groups = (kShoreSdfSize + 7u) / 8u;

    auto asUint = [](float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    };

    renderer->Transition(cl, shoreSdfJump_[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, shoreSdfJump_[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    auto srvTable = renderer->StageSrvUavTable({ shoreSdfSourceSrv_ });

    // The constants are the same every dispatch bar the jump step, so they are filled once and the
    // step is patched per pass.
    auto dispatch = [&](const std::shared_ptr<Material>& material, UINT step, UINT readIndex, UINT writeIndex)
    {
        auto uavTable = renderer->StageSrvUavTable(
            { shoreSdfJumpUav_[readIndex], shoreSdfJumpUav_[writeIndex] });

        auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
        auto& ctx = ctxHandle.ref();
        ctx.constants[0] = {
            kShoreSdfSize,
            step,
            asUint(texelWorld),
            asUint(kShoreHeight),
            asUint(kShoreNearPlane),
            asUint(kShoreFarPlane),
            asUint(0.0f), // sea level
            0u,
        };
        ctx.srvTable[0] = srvTable.gpu;
        ctx.uavTable[0] = uavTable.gpu;
        material->Bind(cl, ctx);
        cl->Dispatch(groups, groups, 1);
        renderer->UAVBarrier(cl, shoreSdfJump_[readIndex].Get());
        renderer->UAVBarrier(cl, shoreSdfJump_[writeIndex].Get());
    };

    // Seed writes through the READ slot, so the first jump reads what it wrote.
    dispatch(shoreSdfSeedMaterial_, 0u, 0u, 1u);

    // log2(N) passes, halving the step. With N = 1024 that is ten of them, an even count, so the
    // result lands back in buffer 0 and Resolve can read 0 and write 1 without aliasing.
    uint32_t readIndex = 0u;
    for (UINT step = kShoreSdfSize / 2u; step >= 1u; step >>= 1)
    {
        dispatch(shoreSdfJumpMaterial_, step, readIndex, 1u - readIndex);
        readIndex = 1u - readIndex;
    }

    dispatch(shoreSdfResolveMaterial_, 0u, readIndex, 1u);

    renderer->Transition(cl, shoreSdfJump_[1].Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void OceanSimulation::SetShoreArea(float2 centerXZ)
{
    if (std::abs(centerXZ.x - shoreSdfCenter_.x) < 0.01f &&
        std::abs(centerXZ.y - shoreSdfCenter_.y) < 0.01f)
    {
        return;
    }
    shoreSdfCenter_ = centerXZ;
    shoreSdfDirty_ = true;
}

void OceanSimulation::CreateResources(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    mipExtents_.clear();

    if (!renderer || resolution_ == 0 || cascadeCount_ == 0)
    {
        return;
    }

    if (!shoreDepth_)
    {
        CreateShoreDepth(renderer);
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

    if (uploadKeepAlive)
    {
        uploader.StealKeepAlive(uploadKeepAlive);
    }
    else
    {
        std::vector<ComPtr<ID3D12Resource>> runtimeUploadKeepAlive;
        uploader.StealKeepAlive(&runtimeUploadKeepAlive);
        RetireUploadResources(renderer, std::move(runtimeUploadKeepAlive));
    }

    mipCount_ = 1u;
    UINT size = std::max<UINT>(1u, resolution_);
    while (size > 1u)
    {
        size = std::max<UINT>(1u, size / 2u);
        ++mipCount_;
    }

    mipExtents_.resize(mipCount_);
    UINT mipSize = std::max<UINT>(1u, resolution_);
    for (UINT level = 0; level < mipCount_; ++level)
    {
        mipExtents_[level] = mipSize;
        mipSize = std::max<UINT>(1u, mipSize / 2u);
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

    ComPtr<ID3D12Resource> dispRes;
    ThrowIfFailed(render::CreateCommittedTexture(device, heapProps, D3D12_HEAP_FLAG_NONE, texDesc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, &dispRes));

    // Created as the FFT's UAV, but the frame leaves it shader-readable for the surface draw
    // (see PrepareUpdate). Measured with --canonical-check.
    displacement_.Attach(renderer->Declarations(), std::move(dispRes), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        L"Ocean.Displacement"); // created in its resting state

    D3D12_RESOURCE_DESC prevDesc = texDesc;
    prevDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    const D3D12_RESOURCE_STATES prevSrvState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ComPtr<ID3D12Resource> prevRes;
    // Step 15: created in its RESTING state like its two neighbours, not in COMMON. It was the one
    // texture step 7's create-in-canonical sweep missed, and under LEGACY barriers that was
    // invisible — COMMON implicitly promotes to a shader-read state, so the compile's canonical
    // seed happened to match reality. Enhanced barriers have no implicit promotion, so the first
    // barrier declared LayoutBefore=SHADER_RESOURCE against a resource genuinely in LAYOUT_COMMON:
    // debug-layer id=1334, and the only enhanced-only error that was actually OUR bug.
    ThrowIfFailed(render::CreateCommittedTexture(device, heapProps, D3D12_HEAP_FLAG_NONE, prevDesc,
            prevSrvState, nullptr, &prevRes));

    prevDisplacement_.Attach(renderer->Declarations(), std::move(prevRes),
        prevSrvState, prevSrvState, L"Ocean.PrevDisplacement");
    hasDisplacementHistory_ = false;
    prevDisplacementValid_ = false;

    if (cascadeCount_ > 0)
    {
        D3D12_RESOURCE_DESC foamDesc = texDesc;
        foamDesc.DepthOrArraySize = static_cast<UINT16>(cascadeCount_);
        foamDesc.MipLevels = static_cast<UINT16>(mipCount_);
        ComPtr<ID3D12Resource> foamRes;
        ThrowIfFailed(render::CreateCommittedTexture(device, heapProps, D3D12_HEAP_FLAG_NONE, foamDesc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, &foamRes));
        foamTurbulence_.Attach(renderer->Declarations(), std::move(foamRes), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            L"Ocean.FoamTurbulence");
        foamNeedsInit_ = true;
        lastFoamSimTime_ = 0.0f;
    }
}

void OceanSimulation::CreateShoreDepth(Renderer* renderer)
{
    shoreDepth_.Reset();
    shoreDepthDsvHeap_.Reset();
    shoreDepthDsv_ = {};
    shoreDepthSrv_ = {};
    shoreDepthWidth_ = 0u;
    shoreDepthHeight_ = 0u;

    if (!renderer)
    {
        return;
    }

    ID3D12Device* device = renderer->GetDevice();
    if (!device)
    {
        return;
    }

    constexpr UINT kShoreDepthSize = 512u;
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = kShoreDepthSize;
    depthDesc.Height = kShoreDepthSize;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_R16_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D16_UNORM;
    clear.DepthStencil.Depth = 1.0f;
    clear.DepthStencil.Stencil = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> shoreDepthRes;
    ThrowIfFailed(render::CreateCommittedTexture(device, heapProps, D3D12_HEAP_FLAG_NONE, depthDesc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, &shoreDepthRes));
    // Naming, declaring and unregistering are now one thing the wrapper owns; rasterized as
    // depth, then sampled, so the frame leaves it shader-readable.
    // Created in its RESTING state (rasterized as depth, then sampled) — step 7 prereq #1.
    shoreDepth_.Attach(renderer->Declarations(), std::move(shoreDepthRes), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        L"Ocean.ShoreDepth");

    shoreDepthWidth_ = static_cast<UINT>(depthDesc.Width);
    shoreDepthHeight_ = static_cast<UINT>(depthDesc.Height);

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.NumDescriptors = 1;
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&shoreDepthDsvHeap_)));

    shoreDepthDsv_ = shoreDepthDsvHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvView{};
    dsvView.Format = DXGI_FORMAT_D16_UNORM;
    dsvView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(shoreDepth_.Get(), &dsvView, shoreDepthDsv_);

    // ---- Shore SDF -------------------------------------------------------------------------
    // Source: the same top-down terrain depth, covering the whole level at a coarser texel. The two
    // jump buffers carry seed coordinates during the flood, and [1] ends up holding the distance.
    D3D12_RESOURCE_DESC sdfSourceDesc = depthDesc;
    sdfSourceDesc.Width = kShoreSdfSize;
    sdfSourceDesc.Height = kShoreSdfSize;

    Microsoft::WRL::ComPtr<ID3D12Resource> sdfSourceRes;
    ThrowIfFailed(render::CreateCommittedTexture(device, heapProps, D3D12_HEAP_FLAG_NONE, sdfSourceDesc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, &sdfSourceRes));
    shoreSdfSource_.Attach(renderer->Declarations(), std::move(sdfSourceRes),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        L"Ocean.ShoreSdfSource");

    ThrowIfFailed(device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&shoreSdfDsvHeap_)));
    shoreSdfSourceDsv_ = shoreSdfDsvHeap_->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(shoreSdfSource_.Get(), &dsvView, shoreSdfSourceDsv_);

    D3D12_RESOURCE_DESC jumpDesc{};
    jumpDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    jumpDesc.Width = kShoreSdfSize;
    jumpDesc.Height = kShoreSdfSize;
    jumpDesc.DepthOrArraySize = 1;
    jumpDesc.MipLevels = 1;
    jumpDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    jumpDesc.SampleDesc.Count = 1;
    jumpDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    jumpDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (int i = 0; i < 2; ++i)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> jumpRes;
        ThrowIfFailed(render::CreateCommittedTexture(device, heapProps, D3D12_HEAP_FLAG_NONE, jumpDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, &jumpRes));
        shoreSdfJump_[i].Attach(renderer->Declarations(), std::move(jumpRes),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            i == 0 ? L"Ocean.ShoreSdfJumpA" : L"Ocean.ShoreSdfJumpB");
    }
}

void OceanSimulation::CreateDescriptors(ID3D12Device* device)
{
    if (!device || !displacement_ || arraySliceCount_ == 0)
    {
        return;
    }

    UINT descriptorCount = 3u; // h0 SRV, wave SRV, full displacement SRV
    descriptorCount += 1u;     // base displacement SRV
    descriptorCount += 1u;     // previous displacement SRV
    descriptorCount += mipCount_; // displacement UAVs
    if (cascadeCount_ > 0)
    {
        descriptorCount += 1u;        // foam aggregate SRV
        descriptorCount += mipCount_; // foam UAVs
    }
    if (shoreDepth_)
    {
        descriptorCount += 1u;        // shore depth SRV
    }
    if (shoreSdfSource_)
    {
        descriptorCount += 4u;        // sdf source SRV + 2 jump UAVs + sdf SRV
    }

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
    displacementFullSrv_ = Offset(base, 2);

    displacementSrvs_.resize(1u);
    displacementUavs_.resize(mipCount_);
    foamSrvs_.clear();
    if (cascadeCount_ > 0)
    {
        foamUavs_.resize(mipCount_);
    }
    else
    {
        foamUavs_.clear();
    }

    UINT descriptorIndex = 3;
    displacementSrvs_[0] = Offset(base, descriptorIndex++);
    prevDisplacementSrv_ = Offset(base, descriptorIndex++);

    if (cascadeCount_ > 0)
    {
        foamSrv_ = Offset(base, descriptorIndex++);
    }
    else
    {
        foamSrv_ = {};
    }

    if (shoreDepth_)
    {
        shoreDepthSrv_ = Offset(base, descriptorIndex++);
    }
    else
    {
        shoreDepthSrv_ = {};
    }

    if (shoreSdfSource_)
    {
        shoreSdfSourceSrv_ = Offset(base, descriptorIndex++);
        shoreSdfJumpUav_[0] = Offset(base, descriptorIndex++);
        shoreSdfJumpUav_[1] = Offset(base, descriptorIndex++);
        shoreSdfSrv_ = Offset(base, descriptorIndex++);
    }
    else
    {
        shoreSdfSourceSrv_ = {};
        shoreSdfJumpUav_[0] = {};
        shoreSdfJumpUav_[1] = {};
        shoreSdfSrv_ = {};
    }

    for (UINT mip = 0; mip < mipCount_; ++mip)
    {
        displacementUavs_[mip] = Offset(base, descriptorIndex++);
    }

    if (cascadeCount_ > 0)
    {
        for (UINT mip = 0; mip < mipCount_; ++mip)
        {
            foamUavs_[mip] = Offset(base, descriptorIndex++);
        }
        foamUav_ = foamUavs_.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{} : foamUavs_[0];
    }
    else
    {
        foamUav_ = {};
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

    if (displacementFullSrv_.ptr != 0)
    {
        auto fullSrvDesc = texSrv;
        fullSrvDesc.Texture2DArray.MipLevels = mipCount_;
        device->CreateShaderResourceView(displacement_.Get(), &fullSrvDesc, displacementFullSrv_);
    }

    texSrv.Texture2DArray.MipLevels = 1;
    texSrv.Texture2DArray.MostDetailedMip = 0;
    device->CreateShaderResourceView(displacement_.Get(), &texSrv, displacementSrvs_[0]);

    if (prevDisplacementSrv_.ptr != 0 && prevDisplacement_)
    {
        device->CreateShaderResourceView(prevDisplacement_.Get(), &texSrv, prevDisplacementSrv_);
    }

    if (foamTurbulence_ && cascadeCount_ > 0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC foamSrvDesc = texSrv;
        foamSrvDesc.Texture2DArray.ArraySize = cascadeCount_;
        foamSrvDesc.Texture2DArray.MostDetailedMip = 0;
        foamSrvDesc.Texture2DArray.MipLevels = mipCount_;
        device->CreateShaderResourceView(foamTurbulence_.Get(), &foamSrvDesc, foamSrv_);

    }

    if (shoreDepthSrv_.ptr != 0 && shoreDepth_)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC shoreSrv{};
        shoreSrv.Format = DXGI_FORMAT_R16_UNORM;
        shoreSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        shoreSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shoreSrv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(shoreDepth_.Get(), &shoreSrv, shoreDepthSrv_);

        if (shoreSdfSourceSrv_.ptr != 0 && shoreSdfSource_)
        {
            device->CreateShaderResourceView(shoreSdfSource_.Get(), &shoreSrv, shoreSdfSourceSrv_);
        }
    }

    if (shoreSdfSrv_.ptr != 0 && shoreSdfJump_[0] && shoreSdfJump_[1])
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC jumpUav{};
        jumpUav.Format = DXGI_FORMAT_R16G16_FLOAT;
        jumpUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(shoreSdfJump_[0].Get(), nullptr, &jumpUav, shoreSdfJumpUav_[0]);
        device->CreateUnorderedAccessView(shoreSdfJump_[1].Get(), nullptr, &jumpUav, shoreSdfJumpUav_[1]);

        D3D12_SHADER_RESOURCE_VIEW_DESC sdfSrv{};
        sdfSrv.Format = DXGI_FORMAT_R16G16_FLOAT;
        sdfSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sdfSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sdfSrv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(shoreSdfJump_[1].Get(), &sdfSrv, shoreSdfSrv_);
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

    if (foamTurbulence_ && cascadeCount_ > 0)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC foamUavDesc = texUav;
        foamUavDesc.Texture2DArray.ArraySize = cascadeCount_;
        for (UINT mip = 0; mip < mipCount_; ++mip)
        {
            foamUavDesc.Texture2DArray.MipSlice = mip;
            device->CreateUnorderedAccessView(foamTurbulence_.Get(), nullptr, &foamUavDesc, foamUavs_[mip]);
        }
        foamUav_ = foamUavs_.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{} : foamUavs_[0];
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

    Material::ComputeDesc sdfDesc{};
    sdfDesc.shaderFile = L"shaders/ocean_shore_sdf.hlsl";
    sdfDesc.csEntry = "Seed";
    shoreSdfSeedMaterial_ = materialMgr->GetOrCreateCompute(renderer, sdfDesc);
    sdfDesc.csEntry = "Jump";
    shoreSdfJumpMaterial_ = materialMgr->GetOrCreateCompute(renderer, sdfDesc);
    sdfDesc.csEntry = "Resolve";
    shoreSdfResolveMaterial_ = materialMgr->GetOrCreateCompute(renderer, sdfDesc);

    Material::ComputeDesc foamDesc{};
    foamDesc.shaderFile = L"shaders/ocean_foam_simulation.hlsl";
    foamDesc.csEntry = "SimulateFoam";
    foamSimMaterial_ = materialMgr->GetOrCreateCompute(renderer, foamDesc);

    Material::ComputeDesc foamInitDesc = foamDesc;
    foamInitDesc.csEntry = "InitializeFoam";
    foamInitMaterial_ = materialMgr->GetOrCreateCompute(renderer, foamInitDesc);
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

    CollectRetiredResources(renderer);

    if (!initialized_)
    {
        Initialize(renderer, cl, nullptr);
    }

    const D3D12_RESOURCE_STATES srvState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    if (displacement_ && prevDisplacement_)
    {
        if (WillCopyDisplacementHistory()) // shared with PrepareUpdate — see the note there
        {
            renderer->Transition(cl, displacement_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            renderer->Transition(cl, prevDisplacement_.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyResource(prevDisplacement_.Get(), displacement_.Get());
            renderer->Transition(cl, prevDisplacement_.Get(), srvState);
            prevDisplacementValid_ = true;
        }
        else
        {
            renderer->Transition(cl, prevDisplacement_.Get(), srvState);
            prevDisplacementValid_ = false;
        }
    }

    const float simTime = timeSeconds * timeScale_;

    DispatchSpectrum(renderer, cl, simTime);
    DispatchFFT(renderer, cl);
    DispatchFFTPost(renderer, cl);
    GenerateMips(renderer, cl);
    DispatchFoam(renderer, cl, simTime);

    renderer->Transition(cl, displacement_.Get(), srvState);
    if (foamTurbulence_)
    {
        renderer->Transition(cl, foamTurbulence_.Get(), srvState);
    }

    hasDisplacementHistory_ = true;
}

// Barrier plan D1.1: does THIS frame's Update copy displacement into the history texture?
//
// `initialized_` matters as much as the history flag: Update calls Initialize() when it is false,
// and CreateResources resets hasDisplacementHistory_ — so a Prepare that consulted the flag alone
// saw `true`, registered the copy, and then the copy did not happen. That mismatch was benign
// under the tracker and is a wrong before-state once barriers are compiled.
//
// Hoisting Initialize out of the record body entirely is the cleaner end state (it is the Step-4
// rule, and this call escaped that sweep only because it is not named Ensure*). This gate makes
// Prepare and Record agree in the meantime.
bool OceanSimulation::WillCopyDisplacementHistory() const
{
    return initialized_ && hasDisplacementHistory_;
}

void OceanSimulation::PrepareUpdate(RenderGraphPassContext& ctx)
{
    const D3D12_RESOURCE_STATES srvState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    if (displacement_ && prevDisplacement_)
    {
        // Step 7: gate on hasDisplacementHistory_ after all. It is written at the END of Update,
        // so at Prepare time it still holds exactly the value Update is about to read — the
        // registration is therefore EXACT, not one frame stale. Over-registering was benign under
        // the tracker and is fatal under the flip: the compile would advance its model past a copy
        // that never happens, and every later use of these two gets a wrong before-state.
        if (WillCopyDisplacementHistory())
        {
            ctx.Use(displacement_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            ctx.Use(prevDisplacement_.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        }
        ctx.NextPoint();
        ctx.Use(prevDisplacement_.Get(), srvState);
    }

    // Spectrum -> FFT -> FFT post -> mip chain all write displacement as a UAV.
    ctx.NextPoint();
    ctx.Use(displacement_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Foam sim reads the finished displacement and writes the turbulence map.
    ctx.NextPoint();
    if (foamTurbulence_) { ctx.Use(foamTurbulence_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
    ctx.Use(displacement_.Get(), srvState);

    // Both maps end the pass readable by the ocean surface draw.
    ctx.NextPoint();
    if (foamTurbulence_) { ctx.Use(foamTurbulence_.Get(), srvState); }
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
        Math::FloatToUint32(timeSeconds),
        resolution_ * resolution_
    };

    auto srvTable = renderer->StageSrvUavTable({ h0Srv_, waveDataSrv_ });
    auto uavTable = renderer->StageSrvUavTable({ displacementUavs_.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{} : displacementUavs_[0] });

    ctx.srvTable[0] = srvTable.gpu;
    ctx.uavTable[0] = uavTable.gpu;

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
        ctx.uavTable[0] = uavTable.gpu;
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
        ctx.uavTable[0] = uavTable.gpu;
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
    ctx.uavTable[0] = uavTable.gpu;

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

    // The source mip is bound as a UAV, not an SRV — see the header comment in
    // shaders/ocean_generate_mips.hlsl. The whole resource is in UNORDERED_ACCESS for this pass, so
    // an SRV over it was undefined and GPU-based validation flagged it every frame (id=1358).
    if (mipExtents_.size() < mipCount_ || displacementUavs_.size() < mipCount_)
    {
        return;
    }

    for (UINT dstMip = 1; dstMip < mipCount_;)
    {
        const UINT srcMip = dstMip - 1u;
        const UINT batchCount = std::min<UINT>(kMipsPerDispatch, mipCount_ - dstMip);
        if (batchCount == 0)
        {
            break;
        }
        const UINT srcWidth = mipExtents_[srcMip];
        const UINT srcHeight = mipExtents_[srcMip];
        const UINT dstWidth = mipExtents_[dstMip];
        const UINT dstHeight = mipExtents_[dstMip];

        auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
        auto& ctx = ctxHandle.ref();

        ctx.constants[0] = {
            srcWidth,
            srcHeight,
            arraySliceCount_,
            srcMip,
            dstMip,
            batchCount,
            dstWidth,
            dstHeight
        };

        auto srcTable = renderer->StageSrvUavTable({ displacementUavs_[srcMip] });

        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMipsPerDispatch> uavs{};
        for (UINT i = 0; i < batchCount; ++i)
        {
            uavs[i] = displacementUavs_[dstMip + i];
        }
        if (batchCount < kMipsPerDispatch)
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE pad = uavs[batchCount - 1];
            for (UINT i = batchCount; i < kMipsPerDispatch; ++i)
            {
                uavs[i] = pad;
            }
        }
        auto uavTable = renderer->StageSrvUavTable(uavs);

        // Both tables are UAVs now; the slot index is the table's BASE REGISTER (u0 / u1).
        ctx.uavTable[0] = srcTable.gpu;
        ctx.uavTable[1] = uavTable.gpu;

        mipMaterial_->Bind(cl, ctx);

        const UINT groupsX = (dstWidth + kThreadGroupSize - 1u) / kThreadGroupSize;
        const UINT groupsY = (dstHeight + kThreadGroupSize - 1u) / kThreadGroupSize;
        cl->Dispatch(groupsX, groupsY, arraySliceCount_);

        renderer->UAVBarrier(cl, displacement_.Get());

        dstMip += batchCount;
    }
}

void OceanSimulation::InitializeFoamTexture(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer || !cl || !foamInitMaterial_ || !foamTurbulence_ || cascadeCount_ == 0 || resolution_ == 0)
    {
        return;
    }

    renderer->Transition(cl, foamTurbulence_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = ctxHandle.ref();

    ctx.constants[0] = {
        resolution_,
        cascadeCount_,
        0u,
        0u,
    };

    auto srvTable = renderer->StageSrvUavTable({ displacementSrvs_.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{} : displacementSrvs_[0] });
    auto uavTable = renderer->StageSrvUavTable({ foamUav_ });

    ctx.srvTable[0] = srvTable.gpu;
    ctx.uavTable[0] = uavTable.gpu;

    foamInitMaterial_->Bind(cl, ctx);

    const UINT groups = (resolution_ + kThreadGroupSize - 1u) / kThreadGroupSize;
    cl->Dispatch(groups, groups, cascadeCount_);

    renderer->UAVBarrier(cl, foamTurbulence_.Get());
}

void OceanSimulation::DispatchFoam(Renderer* renderer, ID3D12GraphicsCommandList* cl, float simTime)
{
    if (!renderer || !cl || !foamSimMaterial_ || !foamTurbulence_ || cascadeCount_ == 0 || resolution_ == 0)
    {
        return;
    }

    if (foamNeedsInit_)
    {
        InitializeFoamTexture(renderer, cl);
        foamNeedsInit_ = false;
        lastFoamSimTime_ = simTime;
    }

    float deltaTime = simTime - lastFoamSimTime_;
    if (deltaTime < 0.0f)
    {
        deltaTime = 0.0f;
    }
    lastFoamSimTime_ = simTime;

    renderer->Transition(cl, foamTurbulence_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, displacement_.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = ctxHandle.ref();

    ctx.constants[0] = {
        resolution_,
        cascadeCount_,
        Math::FloatToUint32(deltaTime),
        Math::FloatToUint32(inputs_.foam.decayRate)
    };

    auto srvTable = renderer->StageSrvUavTable({ displacementSrvs_.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{} : displacementSrvs_[0] });
    auto uavTable = renderer->StageSrvUavTable({ foamUav_ });

    ctx.srvTable[0] = srvTable.gpu;
    ctx.uavTable[0] = uavTable.gpu;

    foamSimMaterial_->Bind(cl, ctx);

    const UINT groups = (resolution_ + kThreadGroupSize - 1u) / kThreadGroupSize;
    cl->Dispatch(groups, groups, cascadeCount_);

    renderer->UAVBarrier(cl, foamTurbulence_.Get());

    // Second user of mipMaterial_ (the displacement chain is the other). Same shape, so it takes
    // the same UAV-source binding — a shared material means a shared binding contract, and
    // converting only one of the two call sites left this one setting the OLD layout: the dest
    // table landed in the source slot and the dest root argument was never set at all. GBV said
    // "Uninitialized root argument accessed" and named the dispatch, not the site.
    if (mipMaterial_ && mipCount_ > 1 && cascadeCount_ > 0 && foamUavs_.size() >= mipCount_)
    {
        if (mipExtents_.size() < mipCount_)
        {
            return;
        }

        for (UINT dstMip = 1; dstMip < mipCount_;)
        {
            const UINT srcMip = dstMip - 1u;
            const UINT batchCount = std::min<UINT>(kMipsPerDispatch, mipCount_ - dstMip);
            if (batchCount == 0)
            {
                break;
            }
            const UINT srcWidth = mipExtents_[srcMip];
            const UINT srcHeight = mipExtents_[srcMip];
            const UINT dstWidth = mipExtents_[dstMip];
            const UINT dstHeight = mipExtents_[dstMip];

            auto ctxHandleMip = renderer->GetRenderContextPool()->Acquire();
            auto& ctxMip = ctxHandleMip.ref();

            ctxMip.constants[0] = {
                srcWidth,
                srcHeight,
                cascadeCount_,
                srcMip,
                dstMip,
                batchCount,
                dstWidth,
                dstHeight
            };

            auto srcTable = renderer->StageSrvUavTable({ foamUavs_[srcMip] });

            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMipsPerDispatch> uavs{};
            for (UINT i = 0; i < batchCount; ++i)
            {
                uavs[i] = foamUavs_[dstMip + i];
            }
            if (batchCount < kMipsPerDispatch)
            {
                const D3D12_CPU_DESCRIPTOR_HANDLE pad = uavs[batchCount - 1];
                for (UINT i = batchCount; i < kMipsPerDispatch; ++i)
                {
                    uavs[i] = pad;
                }
            }
            auto uavTable = renderer->StageSrvUavTable(uavs);

            ctxMip.uavTable[0] = srcTable.gpu;
            ctxMip.uavTable[1] = uavTable.gpu;

            mipMaterial_->Bind(cl, ctxMip);

            const UINT groupsX = (dstWidth + kThreadGroupSize - 1u) / kThreadGroupSize;
            const UINT groupsY = (dstHeight + kThreadGroupSize - 1u) / kThreadGroupSize;
            cl->Dispatch(groupsX, groupsY, cascadeCount_);

            renderer->UAVBarrier(cl, foamTurbulence_.Get());

            dstMip += batchCount;
        }
    }
}

float OceanSimulation::GetLocalWindDirectionRadians() const
{
    return localWindDirection_ * Math::DEG2RAD;
}

Math::float2 OceanSimulation::GetLocalWindDirectionVector() const
{
    const float radians = GetLocalWindDirectionRadians();
    return Math::float2(std::cos(radians), std::sin(radians));
}

float OceanSimulation::GetFoamTrailUpdateTime() const
{
    return inputs_.foamTrailUpdateTime;
}
