#pragma once

// Micro-benchmark isolating the per-object frustum-cull cost from all frame/threading/scene
// noise: it times the legacy DirectXMath intersect against the precomputed-plane intersect in
// one binary over one fixed set of AABBs, single-threaded. Also cross-checks that the two
// paths agree on visibility (the plane test is a conservative superset). Invoked headless via
// the "cull-benchmark" command line; writes results to a text file. Benchmark-only.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/math/Math.h"
#include "core/math/AABB.h"
#include "core/math/Frustum.h"

inline int RunCullBenchmark(const char* outPath)
{
    using clock = std::chrono::high_resolution_clock;

    constexpr int N = 20000;
    constexpr int K = 100;

    // Deterministic pseudo-random AABBs spread through a large volume.
    std::vector<AABB> boxes;
    boxes.reserve(N);
    std::uint32_t seed = 0x12345678u;
    auto rnd01 = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) * (1.0f / 16777216.0f); // [0,1)
    };
    const float R = 500.0f;
    for (int i = 0; i < N; ++i)
    {
        const Math::float3 c((rnd01() * 2.0f - 1.0f) * R, (rnd01() * 2.0f - 1.0f) * R, (rnd01() * 2.0f - 1.0f) * R);
        const Math::float3 e(0.5f + rnd01() * 2.0f, 0.5f + rnd01() * 2.0f, 0.5f + rnd01() * 2.0f);
        boxes.emplace_back(c - e, c + e);
    }

    // Representative ORIENTED shadow ortho box — the hot path (12 of the 14 views are these).
    const Math::mat4 lightView = Math::mat4::LookAtLH(Math::float3(300, 400, -200), Math::float3(0, 0, 0), Math::float3(0, 1, 0));
    const Math::mat4 invLightView = Math::mat4::Inverse(lightView);
    const Frustum ortho = Frustum::FromOrthoBounds(invLightView, 250.0f, 250.0f, 400.0f, Math::float3(0, 0, 0));

    // Representative perspective camera.
    const Math::mat4 camView = Math::mat4::LookAtLH(Math::float3(0, 150, -400), Math::float3(0, 0, 0), Math::float3(0, 1, 0));
    const Math::mat4 camProj = Math::mat4::PerspectiveFovLH(1.0f, 16.0f / 9.0f, 0.5f, 2000.0f);
    const Frustum persp = Frustum::FromViewProj(camView, camProj);

    FILE* out = nullptr;
    if (fopen_s(&out, outPath, "w") != 0 || !out)
    {
        return 1;
    }
    std::fprintf(out, "Cull intersect micro-benchmark (single-thread)\nN=%d boxes, K=%d passes\n\n", N, K);

    volatile int sink = 0;
    auto bench = [&](const Frustum& f, const char* label)
    {
        // Warm cache + count visibility agreement (correctness cross-check).
        int visLegacy = 0, visPlane = 0;
        for (const AABB& b : boxes) { visLegacy += f.IntersectsLegacy(b) ? 1 : 0; }
        for (const AABB& b : boxes) { visPlane  += f.Intersects(b)       ? 1 : 0; }

        const auto t0 = clock::now();
        for (int k = 0; k < K; ++k) { int v = 0; for (const AABB& b : boxes) { v += f.IntersectsLegacy(b) ? 1 : 0; } sink += v; }
        const auto t1 = clock::now();
        for (int k = 0; k < K; ++k) { int v = 0; for (const AABB& b : boxes) { v += f.Intersects(b)       ? 1 : 0; } sink += v; }
        const auto t2 = clock::now();

        const double legacyNs = std::chrono::duration<double, std::nano>(t1 - t0).count();
        const double planeNs  = std::chrono::duration<double, std::nano>(t2 - t1).count();
        const double calls = static_cast<double>(N) * K;
        std::fprintf(out, "[%s]\n", label);
        std::fprintf(out, "  legacy (DirectXMath): %8.1f ms total   %7.2f ns/call   visible=%d\n", legacyNs / 1e6, legacyNs / calls, visLegacy);
        std::fprintf(out, "  plane  (scalar)     : %8.1f ms total   %7.2f ns/call   visible=%d\n", planeNs / 1e6, planeNs / calls, visPlane);
        std::fprintf(out, "  speedup             : %6.2fx           (plane-legacy visible delta = %+d)\n\n", legacyNs / planeNs, visPlane - visLegacy);
    };

    bench(ortho, "OrthoBox shadow (HOT PATH)");
    bench(persp, "Perspective camera");
    std::fprintf(out, "(sink=%d)\n", static_cast<int>(sink));
    std::fclose(out);
    return 0;
}
