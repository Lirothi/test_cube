#include "rendering/core/DlssHandler.h"
#include "rendering/core/BarrierTranslation.h"
#include "rendering/core/TextureCreate.h"

#include <array>
#include <cmath>
#include <cstring>

#include "rendering/core/Renderer.h"
#include "app/camera/Camera.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "streamline/include/sl.h"

namespace
{
    inline sl::float4x4 ToSlMatrix(const Math::mat4& m)
    {
        sl::float4x4 out{};
        out.row[0] = sl::float4(m.m._11, m.m._12, m.m._13, m.m._14);
        out.row[1] = sl::float4(m.m._21, m.m._22, m.m._23, m.m._24);
        out.row[2] = sl::float4(m.m._31, m.m._32, m.m._33, m.m._34);
        out.row[3] = sl::float4(m.m._41, m.m._42, m.m._43, m.m._44);
        return out;
    }

    inline sl::float3 ToSlFloat3(const Math::float3& v)
    {
        return sl::float3(v.x, v.y, v.z);
    }

    float Halton(uint32_t index, uint32_t base)
    {
        float result = 0.0f;
        float f = 1.0f;
        while (index > 0)
        {
            f /= static_cast<float>(base);
            result += f * static_cast<float>(index % base);
            index /= base;
        }
        return result;
    }
}

DlssHandler::DlssHandler(Renderer& renderer)
    : renderer_(renderer)
{
    options_.mode = renderer_.dlssMode_;
    options_.preExposure = 1.0f;
    options_.exposureScale = 1.0f;
    options_.colorBuffersHDR = sl::Boolean::eTrue;
    options_.useAutoExposure = sl::Boolean::eTrue;
    options_.alphaUpscalingEnabled = sl::Boolean::eFalse;

    // Force the CNN model (preset F, the only CNN preset left on 310.x DLLs; A-E are removed).
    // The transformer default (K) drops history trust over temporally unstable content —
    // ocean refraction/specular — and smears it during camera motion. Verified via the NGX
    // debug HUD (ShowDlssIndicator): eDefault resolves to K on our 310.4 DLL.
    // NOTE: the water fix is the COMBO of preset F + AUTO-exposure. Manual exposure (a 1x1
    // tagged texture, see kTagManualExposure in Evaluate) brings the smearing back: the
    // network normalizes input by exposure before its history decisions, and our raw HDR at
    // exposure=1 sits outside the range the CNN behaves well in.
    options_.dlaaPreset = sl::DLSSPreset::ePresetF;
    options_.qualityPreset = sl::DLSSPreset::ePresetF;
    options_.balancedPreset = sl::DLSSPreset::ePresetF;
    options_.performancePreset = sl::DLSSPreset::ePresetF;
    options_.ultraPerformancePreset = sl::DLSSPreset::ePresetF;
    options_.ultraQualityPreset = sl::DLSSPreset::ePresetF;

    constants_.depthInverted = sl::Boolean::eTrue;
    constants_.cameraMotionIncluded = sl::Boolean::eTrue;
    constants_.motionVectors3D = sl::Boolean::eFalse;
    constants_.orthographicProjection = sl::Boolean::eFalse;
    constants_.motionVectorsDilated = sl::Boolean::eFalse;
    constants_.motionVectorsJittered = sl::Boolean::eFalse;
    constants_.mvecScale = sl::float2(1.0f, 1.0f);
    constants_.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants_.jitterOffset = sl::float2(0.0f, 0.0f);
    constants_.motionVectorsInvalidValue = sl::INVALID_FLOAT;
}

void DlssHandler::ResetJitterSequence()
{
    haltonIndex_ = 0;
    jitterPixels_ = Math::float2(0.0f, 0.0f);
    constants_.jitterOffset = sl::float2(0.0f, 0.0f);
    constants_.motionVectorsJittered = sl::Boolean::eFalse;
}

Math::float2 DlssHandler::GenerateJitterSample()
{
    //return Math::float2(0, 0);
    const uint32_t sampleIndex = (haltonIndex_ % kHaltonSequenceLength_) + 1;
    const float jitterX = Halton(sampleIndex, 2) - 0.5f;
    const float jitterY = Halton(sampleIndex, 3) - 0.5f;
    haltonIndex_ = (haltonIndex_ + 1) % kHaltonSequenceLength_;
    return Math::float2(jitterX, jitterY);
}

void DlssHandler::Shutdown()
{
    ClearResourceTags();

    if (resourcesAllocated_)
    {
        slFreeResources(sl::kFeatureDLSS, viewport_);
        resourcesAllocated_ = false;
    }

    exposureUpload_.Reset();
    exposureTex_.Reset();
    exposureUploaded_ = false;
    frameToken_ = nullptr;
    active_ = false;
    available_ = false;
    outputValid_ = false;
    ResetJitterSequence();
}

void DlssHandler::ClearResourceTags()
{
    if (!available_ || frameToken_ == nullptr)
    {
        return;
    }

    // eValidUntilPresent tags make Streamline hold references to the resources.
    // Null-tag every buffer type before those resources and the swap chain are
    // destroyed. All-null tags intentionally need no command list.
    const std::array<sl::ResourceTag, 5> tags = {
        sl::ResourceTag(nullptr, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeExposure, sl::ResourceLifecycle::eValidUntilPresent)
    };
    slSetTagForFrame(*frameToken_, viewport_, tags.data(), static_cast<uint32_t>(tags.size()), nullptr);
}

void DlssHandler::OnStreamlineInitialized(sl::Result initResult)
{
    available_ = (initResult == sl::Result::eOk);
    resourcesAllocated_ = false;
    frameToken_ = nullptr;
    outputValid_ = false;
    resetPending_ = true;
    ResetJitterSequence();

    if (available_)
    {
        UpdateSettings();
    }
    else
    {
        active_ = false;
        dlssRenderWidth_ = std::max(renderer_.width_, 1u);
        dlssRenderHeight_ = std::max(renderer_.height_, 1u);
        RefreshRenderResolution();
    }
}

void DlssHandler::OnBeginFrame()
{
    // DLSS-split: the failure backoff ticks here, BEFORE the graph is built and asks WillEvaluate,
    // so the frame right after a failure is the one that takes the scene-colour path.
    if (skipEvaluateFrames_ > 0) { --skipEvaluateFrames_; }
    if (!available_)
    {
        frameToken_ = nullptr;
        ResetJitterSequence();
        return;
    }

    if (slGetNewFrameToken(frameToken_) != sl::Result::eOk)
    {
        frameToken_ = nullptr;
    }

    if (frameToken_ == nullptr)
    {
        ResetJitterSequence();
        outputValid_ = false;
        return;
    }

    if (IsActive())
    {
        // Paused: hold the offset at zero on BOTH sides. The Halton index is deliberately not
        // advanced, so unpausing resumes the sequence where it left off rather than jumping.
        jitterPixels_ = jitterPaused_ ? Math::float2(0.0f, 0.0f) : GenerateJitterSample();
        constants_.jitterOffset = sl::float2(jitterPixels_.x, jitterPixels_.y);
        //constants_.motionVectorsJittered = sl::Boolean::eTrue;
        constants_.motionVectorsJittered = sl::Boolean::eFalse;
    }
    else
    {
        ResetJitterSequence();
    }
}

void DlssHandler::OnDisplaySizeChanged()
{
    if (available_)
    {
        UpdateSettings();
    }
    else
    {
        RefreshRenderResolution();
    }

    resetPending_ = true;
    outputValid_ = false;
    ResetJitterSequence();
}

void DlssHandler::OnRenderResolutionScaleChanged()
{
    resetPending_ = true;
    outputValid_ = false;
    RefreshRenderResolution();

    if (renderer_.rtManager_.IsCreated())
    {
        renderer_.RecreateDeferredTargets();
    }

    ResetJitterSequence();
}

void DlssHandler::UpdateSettings()
{
    const bool wasActive = active_;

    if (renderer_.dlssMode_ == sl::DLSSMode::eOff)
    {
        active_ = false;
        dlssRenderWidth_ = std::max(renderer_.width_, 1u);
        dlssRenderHeight_ = std::max(renderer_.height_, 1u);
        RefreshRenderResolution();
        ResetJitterSequence();
        return;
    }

    if (!available_ || renderer_.width_ == 0 || renderer_.height_ == 0)
    {
        active_ = false;
        dlssRenderWidth_ = std::max(renderer_.width_, 1u);
        dlssRenderHeight_ = std::max(renderer_.height_, 1u);
        RefreshRenderResolution();
        ResetJitterSequence();
        return;
    }

    sl::DLSSOptions options = options_;
    options.mode = renderer_.dlssMode_;
    options.outputWidth = renderer_.width_;
    options.outputHeight = renderer_.height_;

    sl::DLSSOptimalSettings optimal{};
    if (SL_SUCCEEDED(result, slDLSSGetOptimalSettings(options, optimal)) &&
        optimal.optimalRenderWidth > 0 && optimal.optimalRenderHeight > 0)
    {
        dlssRenderWidth_ = optimal.optimalRenderWidth;
        dlssRenderHeight_ = optimal.optimalRenderHeight;
        options_ = options;
        active_ = true;
    }
    else
    {
        active_ = false;
        dlssRenderWidth_ = std::max(renderer_.width_, 1u);
        dlssRenderHeight_ = std::max(renderer_.height_, 1u);
    }

    outputValid_ = false;
    resetPending_ = true;
    RefreshRenderResolution();

    if (active_ != wasActive)
    {
        ResetJitterSequence();
    }
}

void DlssHandler::AllocateResourcesIfNeeded()
{
    if (!available_ || resourcesAllocated_)
    {
        return;
    }

    //auto res = slAllocateResources(nullptr, sl::kFeatureDLSS, viewport_);
    auto res = sl::Result::eOk;
    if (res == sl::Result::eOk)
    {
        resourcesAllocated_ = true;
        resetPending_ = true;
        outputValid_ = false;
    }
    else
    {
        HandleAllocationFailure();
    }
}

void DlssHandler::UpdateCameraData(const Camera& camera)
{
    if (!available_)
    {
        return;
    }

    const Math::mat4& proj = camera.GetProjMatrixNoJitter();
    const Math::mat4& invProj = camera.GetInvProjMatrixNoJitter();
    const Math::mat4& invView = camera.GetInvViewMatrix();
    const Math::mat4& prevView = camera.GetPrevViewMatrix();
    const Math::mat4& prevProj = camera.GetPrevProjMatrixNoJitter();

    Math::mat4 clipToView = invProj;
    Math::mat4 clipToPrevClip = clipToView * invView * prevView * prevProj;
    Math::mat4 prevClipToClip = Math::mat4::Inverse(clipToPrevClip);

    constants_.cameraViewToClip = ToSlMatrix(proj);
    constants_.clipToCameraView = ToSlMatrix(invProj);
    constants_.clipToPrevClip = ToSlMatrix(clipToPrevClip);
    constants_.prevClipToClip = ToSlMatrix(prevClipToClip);
    constants_.jitterOffset = sl::float2(jitterPixels_.x, jitterPixels_.y);

    constants_.cameraPos = ToSlFloat3(camera.GetPosition());

    Math::float3 right(invView.m._11, invView.m._21, invView.m._31);
    Math::float3 up(invView.m._12, invView.m._22, invView.m._32);
    Math::float3 forward(invView.m._13, invView.m._23, invView.m._33);
    constants_.cameraRight = ToSlFloat3(right.Normalized());
    constants_.cameraUp = ToSlFloat3(up.Normalized());
    constants_.cameraFwd = ToSlFloat3(forward.Normalized());

    constants_.cameraNear = camera.GetZNear();
    constants_.cameraFar = camera.GetZFar();

    const float aspect = (renderer_.height_ > 0) ? static_cast<float>(renderer_.width_) / static_cast<float>(renderer_.height_) : 1.0f;
    const float hfov = camera.GetHFov();
    const float safeAspect = std::max(aspect, 1e-6f);
    const float vfov = 2.0f * std::atan(std::tan(hfov * 0.5f) / safeAspect);
    constants_.cameraFOV = vfov;
    constants_.cameraAspectRatio = aspect;

    constants_.reset = resetPending_ ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    if (resetPending_)
    {
        resetPending_ = false;
    }
}

void DlssHandler::EnsureExposureResources(ID3D12GraphicsCommandList* cl)
{
    if (exposureUploaded_)
    {
        return;
    }

    ID3D12Device* device = renderer_.GetDevice();
    if (device == nullptr)
    {
        return;
    }

    if (!exposureTex_)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 1;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(render::CreateCommittedTexture(device, heap, D3D12_HEAP_FLAG_NONE, desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, &exposureTex_)))
        {
            return;
        }
        exposureTex_->SetName(L"DlssExposure1x1");

        D3D12_HEAP_PROPERTIES upHeap{};
        upHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bufDesc{};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&exposureUpload_))))
        {
            exposureTex_.Reset();
            return;
        }

        void* mapped = nullptr;
        const D3D12_RANGE noRead{ 0, 0 };
        if (SUCCEEDED(exposureUpload_->Map(0, &noRead, &mapped)))
        {
            // Tonemap is plain ACES with no exposure multiplier (exposure is baked into light
            // intensities during shading), so the effective pre-tonemap exposure is 1.
            const float kExposure = 1.0f;
            std::memcpy(mapped, &kExposure, sizeof(kExposure));
            exposureUpload_->Unmap(0, nullptr);
        }
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = exposureTex_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = exposureUpload_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    src.PlacedFootprint.Footprint.Width = 1;
    src.PlacedFootprint.Footprint.Height = 1;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // One-time transition recorded on the frame list; the texture then lives in
    // NON_PIXEL_SHADER_RESOURCE forever (never registered with the state tracker).
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = exposureTex_.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers::EmitOne(cl, barrier);

    exposureUploaded_ = true;
}

bool DlssHandler::WillEvaluate() const
{
    if (!IsActive() || frameToken_ == nullptr || skipEvaluateFrames_ > 0) { return false; }
    const auto& deferred = renderer_.rtManager_.Deferred(renderer_.currentFrameIndex_);
    return deferred.scene && deferred.depth && deferred.gbVelocity && deferred.dlssOutput;
}

bool DlssHandler::Evaluate(ID3D12GraphicsCommandList* cl)
{
    // DLSS-split: the outcome steers the NEXT frames' prediction. A single wrapper rather than a
    // flag set at each of the six exits — one of them getting missed is exactly how a stuck
    // "failed" or a stuck "fine" would appear.
    //
    // A failure parks the prediction for kEvaluateBackoffFrames frames rather than forever: the
    // pass that calls this only EXISTS on frames the prediction said yes, so a permanently sticky
    // flag would have nothing left to clear it and DLSS would stay off until a mode change. The
    // backoff also stops a persistently broken Streamline from alternating DLSS/no-DLSS every
    // other frame — it costs one retry per backoff window instead.
    const bool ok = EvaluateInternal(cl);
    if (!ok)
    {
        if (skipEvaluateFrames_ == 0)
        {
            Renderer::DiagLog("[dlss] evaluate failed - falling back to scene colour, retrying shortly\n");
        }
        skipEvaluateFrames_ = kEvaluateBackoffFrames;
    }
    return ok;
}

bool DlssHandler::EvaluateInternal(ID3D12GraphicsCommandList* cl)
{
    CPU_SCOPE(ProfilerScopes::kDlssEvaluate);
    outputValid_ = false;
    if (!IsActive() || cl == nullptr || frameToken_ == nullptr)
    {
        return false;
    }

    auto& deferred = renderer_.rtManager_.Deferred(renderer_.currentFrameIndex_);
    if (!deferred.scene || !deferred.depth || !deferred.gbVelocity || !deferred.dlssOutput)
    {
        return false;
    }

    renderer_.Transition(cl, deferred.scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer_.Transition(cl, deferred.gbVelocity.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer_.Transition(cl, deferred.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer_.Transition(cl, deferred.dlssOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Barrier plan step 15 — THE HAND-OFF BRACKET.
    //
    // Streamline/NGX records LEGACY `ResourceBarrier` calls on the resources we tag. That is not a
    // guess: a module-attributed stack from the debug-layer message callback put the offending
    // call in `190_*.dll` (the NGX feature module) reached through `sl.interposer.dll` from the
    // `slEvaluateFeature` below. We cannot change that code, and mixing the two barrier models on
    // one resource is illegal — but the models are allowed to MEET in the COMMON layout.
    //
    // So the evaluate is bracketed: our enhanced barriers park the four tagged resources in
    // COMMON, NGX legacy-transitions them from and back to COMMON, and we take them back out.
    // The bracket is deliberately INVISIBLE to the barrier compile — every resource leaves in
    // exactly the state it entered — so the graph's model stays true and `Pass_Tonemap`'s Prepare
    // needs no change. That is why these are `TransitionExplicit` (a direct emission with a stated
    // before-state) rather than `Transition` (a request the compile must have predicted).
    constexpr D3D12_RESOURCE_STATES kNps = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr D3D12_RESOURCE_STATES kCommon = D3D12_RESOURCE_STATE_COMMON;

    // RAII, not a matching pair of statements: everything from here to the end of Evaluate has
    // FOUR early `return false` exits (tagging, options, constants, the evaluate itself). Leaving
    // through any of them with the resources parked in COMMON would desynchronise the compile's
    // model from reality for the rest of the frame.
    struct HandOffScope
    {
        ID3D12GraphicsCommandList* cl;
        ID3D12Resource* read[3];
        ID3D12Resource* write;
        bool armed;
        ~HandOffScope()
        {
            if (!armed) { return; }
            for (ID3D12Resource* r : read) { Renderer::TransitionExplicit(cl, r, kCommon, kNps); }
            Renderer::TransitionExplicit(cl, write, kCommon, kUav);
        }
    } handOffScope{ cl,
                    { deferred.scene.Get(), deferred.gbVelocity.Get(), deferred.depth.Get() },
                    deferred.dlssOutput.Get(),
                    renderer_.UseEnhancedBarriers() };
    if (handOffScope.armed)
    {
        for (ID3D12Resource* r : handOffScope.read)
        {
            Renderer::TransitionExplicit(cl, r, kNps, kCommon);
        }
        Renderer::TransitionExplicit(cl, handOffScope.write, kUav, kCommon);
    }
    // The state handed to Streamline must be the state the resource is actually IN, since NGX
    // barriers from it and restores to it.
    const D3D12_RESOURCE_STATES tagRead = handOffScope.armed ? kCommon : kNps;
    const D3D12_RESOURCE_STATES tagWrite = handOffScope.armed ? kCommon : kUav;

    // Manual exposure is DISABLED on purpose: tagging the 1x1 exposure texture turns NGX
    // auto-exposure off, and the preset-F water fix empirically depends on auto-exposure
    // (manual exposure=1 brings the motion smearing back). Kept for future experiments —
    // a correct adapted value (not 1.0) might work; flip the flag to try.
    constexpr bool kTagManualExposure = false;
    if (kTagManualExposure)
    {
        EnsureExposureResources(cl);
    }

    sl::Resource color(sl::ResourceType::eTex2d, deferred.scene.Get(), static_cast<uint32_t>(tagRead));
    color.width = std::max(renderer_.renderWidth_, 1u);
    color.height = std::max(renderer_.renderHeight_, 1u);
    color.nativeFormat = static_cast<uint32_t>(renderer_.GetSceneColorFormat());
    color.mipLevels = 1;

    sl::Resource motion(sl::ResourceType::eTex2d, deferred.gbVelocity.Get(), static_cast<uint32_t>(tagRead));
    motion.width = std::max(renderer_.renderWidth_, 1u);
    motion.height = std::max(renderer_.renderHeight_, 1u);
    motion.nativeFormat = static_cast<uint32_t>(renderer_.GetGBufferVelocityFormat());
    motion.mipLevels = 1;

    sl::Resource depth(sl::ResourceType::eTex2d, deferred.depth.Get(), static_cast<uint32_t>(tagRead));
    depth.width = std::max(renderer_.renderWidth_, 1u);
    depth.height = std::max(renderer_.renderHeight_, 1u);
    depth.nativeFormat = static_cast<uint32_t>(renderer_.GetDeferredDepthFormat());
    depth.mipLevels = 1;

    sl::Resource output(sl::ResourceType::eTex2d, deferred.dlssOutput.Get(), static_cast<uint32_t>(tagWrite));
    output.width = std::max(renderer_.width_, 1u);
    output.height = std::max(renderer_.height_, 1u);
    output.nativeFormat = static_cast<uint32_t>(renderer_.GetSceneColorFormat());
    output.mipLevels = 1;

    // Without an exposure tag NGX forces auto-exposure ON regardless of useAutoExposure
    // (confirmed via the NGX debug HUD); the 1x1 constant texture switches it to manual.
    sl::Resource exposure(sl::ResourceType::eTex2d, exposureTex_.Get(), static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    exposure.width = 1;
    exposure.height = 1;
    exposure.nativeFormat = static_cast<uint32_t>(DXGI_FORMAT_R32_FLOAT);
    exposure.mipLevels = 1;

    std::array<sl::ResourceTag, 5> tags = {
        sl::ResourceTag(&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(&exposure, sl::kBufferTypeExposure, sl::ResourceLifecycle::eValidUntilPresent)
    };
    const uint32_t tagCount = (kTagManualExposure && exposureUploaded_) ? 5u : 4u;

    {
        CPU_SCOPE(ProfilerScopes::kDlssSetTagsOptions);
        if (slSetTagForFrame(*frameToken_, viewport_, tags.data(), tagCount,
            reinterpret_cast<sl::CommandBuffer*>(cl)) != sl::Result::eOk)
        {
            return false;
        }

        options_.mode = renderer_.dlssMode_;
        options_.outputWidth = renderer_.width_;
        options_.outputHeight = renderer_.height_;

        if (slDLSSSetOptions(viewport_, options_) != sl::Result::eOk)
        {
            return false;
        }

        if (slSetConstants(constants_, *frameToken_, viewport_) != sl::Result::eOk)
        {
            return false;
        }
    }

    {
        CPU_SCOPE(ProfilerScopes::kDlssEvaluateFeature);
        const sl::BaseStructure* inputs[] = { &viewport_ };
        if (slEvaluateFeature(sl::kFeatureDLSS, *frameToken_, inputs, _countof(inputs), reinterpret_cast<sl::CommandBuffer*>(cl)) != sl::Result::eOk)
        {
            return false;
        }
    }

    outputValid_ = true;
    return true;
}

bool DlssHandler::IsActive() const
{
    return available_ && active_ && resourcesAllocated_;
}

void DlssHandler::SetActive(bool active)
{
    if (active_ == active)
    {
        return;
    }

    active_ = active;

    if (!active_)
    {
        ResetJitterSequence();
    }
    else
    {
        haltonIndex_ = 0;
    }
}

void DlssHandler::RefreshRenderResolution()
{
    if (active_ && available_)
    {
        renderer_.renderWidth_ = std::max(1u, dlssRenderWidth_);
        renderer_.renderHeight_ = std::max(1u, dlssRenderHeight_);
        if (renderer_.width_ > 0)
        {
            renderer_.renderResolutionScale_ = static_cast<float>(renderer_.renderWidth_) / static_cast<float>(renderer_.width_);
        }
        else
        {
            renderer_.renderResolutionScale_ = 1.0f;
        }
        return;
    }

    const float clampedScale = std::clamp(renderer_.renderResolutionScale_, 0.1f, 1.0f);
    renderer_.renderResolutionScale_ = clampedScale;
    const float baseWidth = static_cast<float>(std::max(renderer_.width_, 1u));
    const float baseHeight = static_cast<float>(std::max(renderer_.height_, 1u));

    renderer_.renderWidth_ = std::max(1u, static_cast<UINT>(baseWidth * clampedScale + 0.5f));
    renderer_.renderHeight_ = std::max(1u, static_cast<UINT>(baseHeight * clampedScale + 0.5f));
}

void DlssHandler::HandleAllocationFailure()
{
    resourcesAllocated_ = false;
    active_ = false;
    dlssRenderWidth_ = std::max(renderer_.width_, 1u);
    dlssRenderHeight_ = std::max(renderer_.height_, 1u);
    outputValid_ = false;
    resetPending_ = true;
    RefreshRenderResolution();
    ResetJitterSequence();

    if (renderer_.rtManager_.IsCreated())
    {
        renderer_.RecreateDeferredTargets();
    }
}

