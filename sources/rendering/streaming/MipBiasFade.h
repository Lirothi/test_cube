#pragma once

#include <algorithm>
#include <cmath>

namespace streaming {

// UE FMipBiasFade (RenderCore/Public/RenderResource.h:289-334, RenderResource.cpp:633-690): after a
// mip count change, a bias that walks from the old count to the new one at fadeIn/fadeOut seconds
// per mip. Applied here as the SRV's ResourceMinLODClamp, so newly arrived mips reveal gradually.
struct MipBiasFade
{
    float totalMipCount = 0.0f;   // mips in memory
    float mipCountDelta = 0.0f;   // negative when fading out
    float startTime = 0.0f;
    float mipCountFadingRate = 0.0f;
    float biasOffset = 0.0f;

    static constexpr float kAgeThreshold = 0.5f; // GMipLevelFadingAgeThreshold: older = "new texture", no fade

    void SetNewMipCount(float actualMipCount, float targetMipCount, float timeSinceLastRendered, float now,
                        float fadeInSecPerMip, float fadeOutSecPerMip, bool enabled)
    {
        if (totalMipCount == 0.0f || timeSinceLastRendered >= kAgeThreshold || !enabled)
        {
            totalMipCount = actualMipCount;
            mipCountDelta = 0.0f;
            mipCountFadingRate = 0.0f;
            startTime = now;
            biasOffset = 0.0f;
            return;
        }
        const float currentTargetMipCount = totalMipCount - biasOffset + mipCountDelta;
        if (std::fabs(totalMipCount - actualMipCount) < 1.0e-4f && std::fabs(targetMipCount - currentTargetMipCount) < 1.0e-4f) { return; }
        float currentInterpolated = totalMipCount - CalcMipBias(now);
        currentInterpolated = std::clamp(currentInterpolated, 0.0f, actualMipCount);
        startTime = now;
        totalMipCount = actualMipCount;
        mipCountDelta = targetMipCount - currentInterpolated;
        if (std::fabs(mipCountDelta) < 1.0e-4f)
        {
            mipCountDelta = 0.0f;
            biasOffset = 0.0f;
            mipCountFadingRate = 0.0f;
        }
        else
        {
            biasOffset = totalMipCount - currentInterpolated;
            mipCountFadingRate = mipCountDelta > 0.0f ? 1.0f / (fadeInSecPerMip * mipCountDelta)
                                                      : -1.0f / (fadeOutSecPerMip * mipCountDelta);
        }
    }

    float CalcMipBias(float now) const
    {
        const float timeFactor = std::min((now - startTime) * mipCountFadingRate, 1.0f);
        return biasOffset - mipCountDelta * timeFactor;
    }

    bool Active(float now) const { return mipCountDelta != 0.0f && (now - startTime) * mipCountFadingRate < 1.0f; }
};

} // namespace streaming
