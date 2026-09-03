#pragma once
// Occlusion plan S2: CPU mirror of shaders/hzb_cull.hlsli -- the same functions, the same order of
// float operations, on the same inputs. It exists for two consumers: the self-test
// (`--hzb-cull-selftest`, HzbCullSelfTest.cpp) that holds it equal to the GPU, and every later
// validator that wants to know what the GPU SHOULD have decided for a box (S3b/S5 readbacks).
// Change one side, change the other: the header comment in the .hlsli lists the deltas from UE.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <intrin.h>

#include "core/math/Math.h"

namespace hzb
{
struct Int2 { int x = 0, y = 0; };
struct Int4 { int x = 0, y = 0, z = 0, w = 0; };
struct F2 { float x = 0.0f, y = 0.0f; };
struct F4 { float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f; };

inline constexpr float kInfiniteFloat = 3.402823466e+38f;

struct FrustumCull
{
    Math::float3 rectMin{};
    Math::float3 rectMax{};
    bool crossesNearPlane = false;
    bool crossesFarPlane = false;
    bool frustumSideCulled = false;
    bool isVisible = false;
};

struct ScreenRect
{
    Int4 pixels{};
    bool overlapsPixelCenter = false;
    Int4 hzbTexels{};
    int hzbLevel = 0;
    float depth = 0.0f;
};

// HLSL firstbithigh on a signed int that is never negative here: -1 for 0.
inline int FirstBitHigh(int v)
{
    if (v <= 0) { return -1; }
    unsigned long index = 0;
    _BitScanReverse(&index, static_cast<unsigned long>(v));
    return static_cast<int>(index);
}

inline int MipLevelForRect(const Int4& rectPixels, int desiredFootprintPixels)
{
    const int maxPixelOffset = desiredFootprintPixels - 1;
    const int mipOffset = static_cast<int>(std::log2(static_cast<float>(desiredFootprintPixels))) - 1;
    const int mipX = FirstBitHigh(rectPixels.z - rectPixels.x);
    const int mipY = FirstBitHigh(rectPixels.w - rectPixels.y);
    int mipLevel = std::max(std::max(mipX, mipY) - mipOffset, 0);
    const bool bump = ((rectPixels.z >> mipLevel) - (rectPixels.x >> mipLevel) > maxPixelOffset) ||
                      ((rectPixels.w >> mipLevel) - (rectPixels.y >> mipLevel) > maxPixelOffset);
    mipLevel += bump ? 1 : 0;
    return mipLevel;
}

inline float Saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }

inline ScreenRect GetScreenRect(const Int4& viewRect, const Math::float3& cullRectMin,
                                const Math::float3& cullRectMax, int desiredFootprintPixels)
{
    ScreenRect rect;
    rect.depth = cullRectMax.z;

    // saturate(float4(min.xy, max.xy) * (0.5, -0.5, 0.5, -0.5) + 0.5).xwzy
    const float u0 = Saturate(cullRectMin.x * 0.5f + 0.5f);
    const float v0 = Saturate(cullRectMin.y * -0.5f + 0.5f);
    const float u1 = Saturate(cullRectMax.x * 0.5f + 0.5f);
    const float v1 = Saturate(cullRectMax.y * -0.5f + 0.5f);
    const F4 rectUV{ u0, v1, u1, v0 };

    const float sizeX = static_cast<float>(viewRect.z - viewRect.x);
    const float sizeY = static_cast<float>(viewRect.w - viewRect.y);
    // int4(...) truncates toward zero, as static_cast does.
    rect.pixels.x = static_cast<int>(rectUV.x * sizeX + static_cast<float>(viewRect.x) + 0.5f);
    rect.pixels.y = static_cast<int>(rectUV.y * sizeY + static_cast<float>(viewRect.y) + 0.5f);
    rect.pixels.z = static_cast<int>(rectUV.z * sizeX + static_cast<float>(viewRect.x) - 0.5f);
    rect.pixels.w = static_cast<int>(rectUV.w * sizeY + static_cast<float>(viewRect.y) - 0.5f);
    rect.pixels.x = std::max(rect.pixels.x, viewRect.x);
    rect.pixels.y = std::max(rect.pixels.y, viewRect.y);
    rect.pixels.z = std::min(rect.pixels.z, viewRect.z - 1);
    rect.pixels.w = std::min(rect.pixels.w, viewRect.w - 1);

    rect.overlapsPixelCenter = rect.pixels.z >= rect.pixels.x && rect.pixels.w >= rect.pixels.y;

    rect.hzbTexels = Int4{ rect.pixels.x, rect.pixels.y,
                           std::max(rect.pixels.x, rect.pixels.z), std::max(rect.pixels.y, rect.pixels.w) };
    rect.hzbTexels = Int4{ rect.hzbTexels.x >> 1, rect.hzbTexels.y >> 1, rect.hzbTexels.z >> 1, rect.hzbTexels.w >> 1 };

    rect.hzbLevel = MipLevelForRect(rect.hzbTexels, desiredFootprintPixels);

    const int l = rect.hzbLevel;
    rect.hzbTexels = Int4{ rect.hzbTexels.x >> l, rect.hzbTexels.y >> l, rect.hzbTexels.z >> l, rect.hzbTexels.w >> l };
    return rect;
}

// `load(x, y, level)` returns the pyramid texel; the clamp (delta 2) is applied HERE, so a loader
// may be a plain array lookup.
template <typename LoadFn>
inline float GetMinDepth(const Int2& hzbMip0Size, const ScreenRect& rect, LoadFn&& load)
{
    const int levelW = std::max(1, hzbMip0Size.x >> rect.hzbLevel);
    const int levelH = std::max(1, hzbMip0Size.y >> rect.hzbLevel);
    float minDepth = 1.0f;
    for (int j = 0; j < 4; ++j)
    {
        const int y = std::min(rect.hzbTexels.y + j, rect.hzbTexels.w);
        const int cy = std::min(std::max(y, 0), levelH - 1);
        for (int i = 0; i < 4; ++i)
        {
            const int x = std::min(rect.hzbTexels.x + i, rect.hzbTexels.z);
            const int cx = std::min(std::max(x, 0), levelW - 1);
            minDepth = std::min(minDepth, load(cx, cy, rect.hzbLevel));
        }
    }
    return minDepth;
}

// Row-vector transform: out[c] = sum_k v[k] * M[k][c] (the engine's Math::mat4, XMFLOAT4X4).
inline F4 MulRow(const F4& v, const Math::mat4& m)
{
    const auto& a = m.m.m;
    F4 r;
    r.x = v.x * a[0][0] + v.y * a[1][0] + v.z * a[2][0] + v.w * a[3][0];
    r.y = v.x * a[0][1] + v.y * a[1][1] + v.z * a[2][1] + v.w * a[3][1];
    r.z = v.x * a[0][2] + v.y * a[1][2] + v.z * a[2][2] + v.w * a[3][2];
    r.w = v.x * a[0][3] + v.y * a[1][3] + v.z * a[2][3] + v.w * a[3][3];
    return r;
}
inline F4 Row(const Math::mat4& m, int i)
{
    const auto& a = m.m.m;
    return F4{ a[i][0], a[i][1], a[i][2], a[i][3] };
}
inline F4 Add(const F4& a, const F4& b) { return F4{ a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w }; }
inline F4 Sub(const F4& a, const F4& b) { return F4{ a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w }; }
inline F4 Scale(float s, const F4& a) { return F4{ s * a.x, s * a.y, s * a.z, s * a.w }; }

inline FrustumCull BoxCullFrustumPerspective(const Math::float3& center, const Math::float3& extent,
                                             const Math::mat4& localToWorld, const Math::mat4& worldToClip,
                                             const Math::mat4& viewToClip, bool skipFrustumCull)
{
    FrustumCull cull;

    const F4 dx = Scale(2.0f * extent.x, MulRow(Row(localToWorld, 0), worldToClip));
    const F4 dy = Scale(2.0f * extent.y, MulRow(Row(localToWorld, 1), worldToClip));
    const F4 dz = Scale(2.0f * extent.z, MulRow(Row(localToWorld, 2), worldToClip));

    float minW = +kInfiniteFloat;
    float maxW = -kInfiniteFloat;
    F4 planesMin{ 1.0f, 1.0f, 1.0f, 1.0f };

    cull.rectMin = Math::float3(+1.0f, +1.0f, +1.0f);
    cull.rectMax = Math::float3(-1.0f, -1.0f, -1.0f);

    const auto evalPoints = [&](const F4& pc0, const F4& pc1)
    {
        minW = std::min(std::min(minW, pc0.w), pc1.w);
        maxW = std::max(std::max(maxW, pc0.w), pc1.w);
        const F4 p0{ pc0.x - pc0.w, pc0.y - pc0.w, -pc0.x - pc0.w, -pc0.y - pc0.w };
        const F4 p1{ pc1.x - pc1.w, pc1.y - pc1.w, -pc1.x - pc1.w, -pc1.y - pc1.w };
        planesMin.x = std::min(std::min(planesMin.x, p0.x), p1.x);
        planesMin.y = std::min(std::min(planesMin.y, p0.y), p1.y);
        planesMin.z = std::min(std::min(planesMin.z, p0.z), p1.z);
        planesMin.w = std::min(std::min(planesMin.w, p0.w), p1.w);
        const F2 ps0{ pc0.x / pc0.w, pc0.y / pc0.w };
        const F2 ps1{ pc1.x / pc1.w, pc1.y / pc1.w };
        cull.rectMin.x = std::min(std::min(cull.rectMin.x, ps0.x), ps1.x);
        cull.rectMin.y = std::min(std::min(cull.rectMin.y, ps0.y), ps1.y);
        cull.rectMax.x = std::max(std::max(cull.rectMax.x, ps0.x), ps1.x);
        cull.rectMax.y = std::max(std::max(cull.rectMax.y, ps0.y), ps1.y);
    };

    const F4 corner{ center.x - extent.x, center.y - extent.y, center.z - extent.z, 1.0f };
    const F4 pc000 = MulRow(MulRow(corner, localToWorld), worldToClip);
    const F4 pc100 = Add(pc000, dz);
    evalPoints(pc000, pc100);
    const F4 pc001 = Add(pc000, dx);
    const F4 pc101 = Add(pc100, dx);
    evalPoints(pc001, pc101);
    const F4 pc011 = Add(pc001, dy);
    const F4 pc111 = Add(pc101, dy);
    evalPoints(pc011, pc111);
    const F4 pc010 = Sub(pc011, dx);
    const F4 pc110 = Sub(pc111, dx);
    evalPoints(pc010, pc110);

    const auto& p = viewToClip.m.m;
    const float minZ = maxW * p[2][2] + p[3][2];
    const float maxZ = minW * p[2][2] + p[3][2];

    const bool inFrontNearPlane = minW <= maxZ;
    const bool behindNearPlane = maxW > minZ;
    const bool inFrontFarPlane = 0.0f < maxZ;
    const bool behindFarPlane = 0.0f >= minZ;

    cull.crossesNearPlane = inFrontNearPlane;
    cull.crossesFarPlane = behindFarPlane;
    cull.isVisible = behindNearPlane && inFrontFarPlane;

    if (minW <= 0.0f && maxW > 0.0f)
    {
        cull.rectMin = Math::float3(-1.0f, -1.0f, -1.0f);
        cull.rectMax = Math::float3(+1.0f, +1.0f, +1.0f);
    }
    else
    {
        cull.rectMin.z = minZ / maxW;
        cull.rectMax.z = maxZ / minW;
    }

    cull.frustumSideCulled = false;
    if (!skipFrustumCull)
    {
        const bool frustumCull = planesMin.x > 0.0f || planesMin.y > 0.0f || planesMin.z > 0.0f || planesMin.w > 0.0f;
        cull.frustumSideCulled = cull.isVisible && frustumCull;
        cull.isVisible = cull.isVisible && !frustumCull;
    }
    return cull;
}
} // namespace hzb
