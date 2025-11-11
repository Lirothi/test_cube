#include "rendering/core/DlssHandler.h"

#include <array>
#include <cmath>

#include "rendering/core/Renderer.h"
#include "app/camera/Camera.h"
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
}

DlssHandler::DlssHandler(Renderer& renderer)
    : renderer_(renderer)
{
    options_.mode = renderer_.dlssMode_;
    options_.preExposure = 1.0f;
    options_.exposureScale = 1.0f;
    options_.colorBuffersHDR = sl::Boolean::eTrue;
    options_.useAutoExposure = sl::Boolean::eFalse;
    options_.alphaUpscalingEnabled = sl::Boolean::eFalse;

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

void DlssHandler::Shutdown()
{
    if (resourcesAllocated_)
    {
        slFreeResources(sl::kFeatureDLSS, viewport_);
        resourcesAllocated_ = false;
    }

    frameToken_ = nullptr;
    active_ = false;
    available_ = false;
    outputValid_ = false;
}

void DlssHandler::OnStreamlineInitialized(sl::Result initResult)
{
    available_ = (initResult == sl::Result::eOk);
    resourcesAllocated_ = false;
    frameToken_ = nullptr;
    outputValid_ = false;
    resetPending_ = true;

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
    if (!available_)
    {
        frameToken_ = nullptr;
        return;
    }

    if (slGetNewFrameToken(frameToken_) != sl::Result::eOk)
    {
        frameToken_ = nullptr;
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
}

void DlssHandler::OnRenderResolutionScaleChanged()
{
    resetPending_ = true;
    outputValid_ = false;
    RefreshRenderResolution();

    if (renderer_.deferredRtvHeap_)
    {
        renderer_.RecreateDeferredTargets();
    }
}

void DlssHandler::UpdateSettings()
{
    if (renderer_.dlssMode_ == sl::DLSSMode::eOff)
    {
        active_ = false;
        dlssRenderWidth_ = std::max(renderer_.width_, 1u);
        dlssRenderHeight_ = std::max(renderer_.height_, 1u);
        RefreshRenderResolution();
        return;
    }

    if (!available_ || renderer_.width_ == 0 || renderer_.height_ == 0)
    {
        active_ = false;
        dlssRenderWidth_ = std::max(renderer_.width_, 1u);
        dlssRenderHeight_ = std::max(renderer_.height_, 1u);
        RefreshRenderResolution();
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

    const Math::mat4& proj = camera.GetProjMatrix();
    const Math::mat4& invProj = camera.GetInvProjMatrix();
    const Math::mat4& invView = camera.GetInvViewMatrix();
    const Math::mat4& prevView = camera.GetPrevViewMatrix();
    const Math::mat4& prevProj = camera.GetPrevProjMatrix();

    Math::mat4 clipToView = invProj;
    Math::mat4 clipToPrevClip = clipToView * invView * prevView * prevProj;
    Math::mat4 prevClipToClip = Math::mat4::Inverse(clipToPrevClip);

    constants_.cameraViewToClip = ToSlMatrix(proj);
    constants_.clipToCameraView = ToSlMatrix(invProj);
    constants_.clipToPrevClip = ToSlMatrix(clipToPrevClip);
    constants_.prevClipToClip = ToSlMatrix(prevClipToClip);

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

bool DlssHandler::Evaluate(ID3D12GraphicsCommandList* cl)
{
    outputValid_ = false;
    if (!IsActive() || cl == nullptr || frameToken_ == nullptr)
    {
        return false;
    }

    auto& deferred = renderer_.deferred_[renderer_.currentFrameIndex_];
    if (!deferred.scene || !deferred.depth || !deferred.gbVelocity || !deferred.dlssOutput)
    {
        return false;
    }

    renderer_.Transition(cl, deferred.scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer_.Transition(cl, deferred.gbVelocity.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer_.Transition(cl, deferred.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer_.Transition(cl, deferred.dlssOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    sl::Resource color(sl::ResourceType::eTex2d, deferred.scene.Get(), static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    color.width = std::max(renderer_.renderWidth_, 1u);
    color.height = std::max(renderer_.renderHeight_, 1u);
    color.nativeFormat = static_cast<uint32_t>(Renderer::kSceneColorFormat);
    color.mipLevels = 1;

    sl::Resource motion(sl::ResourceType::eTex2d, deferred.gbVelocity.Get(), static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    motion.width = std::max(renderer_.renderWidth_, 1u);
    motion.height = std::max(renderer_.renderHeight_, 1u);
    motion.nativeFormat = static_cast<uint32_t>(Renderer::kGBufferVelocityFormat);
    motion.mipLevels = 1;

    sl::Resource depth(sl::ResourceType::eTex2d, deferred.depth.Get(), static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    depth.width = std::max(renderer_.renderWidth_, 1u);
    depth.height = std::max(renderer_.renderHeight_, 1u);
    depth.nativeFormat = static_cast<uint32_t>(Renderer::kDeferredDepthFormat);
    depth.mipLevels = 1;

    sl::Resource output(sl::ResourceType::eTex2d, deferred.dlssOutput.Get(), static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    output.width = std::max(renderer_.width_, 1u);
    output.height = std::max(renderer_.height_, 1u);
    output.nativeFormat = static_cast<uint32_t>(Renderer::kSceneColorFormat);
    output.mipLevels = 1;

    std::array<sl::ResourceTag, 4> tags = {
        sl::ResourceTag(&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent)
    };

    if (slSetTagForFrame(*frameToken_, viewport_, tags.data(), static_cast<uint32_t>(tags.size()),
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

    const sl::BaseStructure* inputs[] = { &viewport_ };
    if (slEvaluateFeature(sl::kFeatureDLSS, *frameToken_, inputs, _countof(inputs), reinterpret_cast<sl::CommandBuffer*>(cl)) != sl::Result::eOk)
    {
        return false;
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
	active_ = active;
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

    if (renderer_.deferredRtvHeap_)
    {
        renderer_.RecreateDeferredTargets();
    }
}

