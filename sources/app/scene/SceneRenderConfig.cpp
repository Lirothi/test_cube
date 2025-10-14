#include "app/scene/SceneRenderConfig.h"

#include <algorithm>

std::array<float, 5> CascadeShadowConfig::BuildSplitScheme(float zNear, float zFar) const
{
    std::array<float, 5> splits{};
    const float cappedFar = std::min(zFar, maxDistance);
    splits[0] = zNear;
    for (size_t i = 0; i < sliceDistances.size(); ++i)
    {
        splits[i + 1] = std::min(sliceDistances[i], cappedFar);
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
