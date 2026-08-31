#include "app/scene/SceneRenderConfig.h"

#include <algorithm>

// UE: FDirectionalLightSceneProxy::ComputeAccumulatedScale (DirectionalLightComponent.cpp:753),
// transcribed rather than re-derived. Weights are exponent^i; the split sits at the running sum
// over the total. For exponent 3 and 4 cascades the weights are 1,3,9,27 (total 40), giving
// fractions 1/40, 4/40, 13/40, 1 -- i.e. {7.5, 30, 97.5, 300} m over a 300 m range.
float CascadeShadowConfig::UeAccumulatedScale(float exponent, int index, int count)
{
    if (index <= 0 || count <= 0) { return 0.0f; }
    float currentScale = 1.0f;
    float totalScale = 0.0f;
    float ret = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        if (i < index) { ret += currentScale; }
        totalScale += currentScale;
        currentScale *= exponent;
    }
    return totalScale > 0.0f ? (ret / totalScale) : 0.0f;
}

std::array<float, 4> CascadeShadowConfig::ComputeUeSplitDistances(float zNear) const
{
    // UE's GetSplitDistance for the near cascades:
    //   split(k) = ShadowNear + ComputeAccumulatedScale(exp, k, N) * (CSMMaxDistance - ShadowNear)
    // ShadowNear is the VIEW's near clipping distance, not 0 -- so the first cascade starts where
    // the camera does, and at our 0.01 m near plane the difference is cosmetic but kept exact.
    const float exponent = std::clamp(cascadeDistributionExponent, 0.1f, 10.0f);
    const int count = static_cast<int>(sliceDistances.size());
    std::array<float, 4> out{};
    for (int k = 0; k < count; ++k)
    {
        out[static_cast<size_t>(k)] =
            zNear + UeAccumulatedScale(exponent, k + 1, count) * (maxDistance - zNear);
    }
    return out;
}

std::array<float, 5> CascadeShadowConfig::BuildSplitScheme(float zNear, float zFar) const
{
    std::array<float, 5> splits{};
    const float cappedFar = std::min(zFar, maxDistance);
    splits[0] = zNear;
    // The UE distribution is computed here and NEVER written back into sliceDistances: the two
    // schemes coexist so the toggle is reversible without losing the authored numbers.
    const std::array<float, 4> src = useUeSplitDistribution ? ComputeUeSplitDistances(zNear)
                                                            : sliceDistances;
    for (size_t i = 0; i < src.size(); ++i)
    {
        splits[i + 1] = std::min(src[i], cappedFar);
    }
    splits.back() = cappedFar;
    for (size_t i = 1; i < splits.size(); ++i)
    {
        if (splits[i] < splits[i - 1])
        {
            splits[i] = splits[i - 1];
        }
    }
    return splits;
}
