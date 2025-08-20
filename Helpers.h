#pragma once
#include <stdexcept>
#include <chrono>
#include <vector>
#include <cmath>

#include "Renderer.h"
#include "Mesh.h"

// Utility: Throw on failure
inline void ThrowIfFailed(HRESULT hr) {
    if (FAILED(hr)) {
        throw std::runtime_error("HRESULT failed");
    }
}

// Utility: Get time in seconds
inline double GetTimeSeconds() {
    using namespace std::chrono;
    static auto start = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double>(now - start).count();
}

inline void BuildSpoonMeshCW(std::vector<VertexPNTUV>& outVerts,
    std::vector<uint32_t>& outIdx,
    int radialSegments = 64,
    bool frontCW = true)
{
    struct Node { float y, r; };
    const Node profile[] = {
        // чаша (вогнутая)
        { -0.018f, 0.002f },
        { -0.016f, 0.008f },
        { -0.012f, 0.014f },
        { -0.006f, 0.018f },
        {  0.000f, 0.018f }, // кромка
        // переход к ручке
        {  0.010f, 0.013f },
        {  0.050f, 0.009f },
        // ручка
        {  0.080f, 0.0085f },
        {  0.110f, 0.0080f },
        {  0.140f, 0.0078f },
    };
    const int rows = int(sizeof(profile) / sizeof(profile[0]));
    outVerts.clear(); outIdx.clear();
    outVerts.reserve(size_t(rows) * size_t(radialSegments));

    // вершины
    for (int j = 0; j < rows; ++j) {
        const float y = profile[j].y;
        const float r = profile[j].r;
        const float v = float(j) / float(rows - 1);
        for (int i = 0; i < radialSegments; ++i) {
            const float u = float(i) / float(radialSegments);
            const float ang = u * 6.28318530718f;
            const float ca = std::cos(ang), sa = std::sin(ang);

            VertexPNTUV vtx{};
            vtx.position = { r * ca, y, r * sa };
            vtx.normal = { 0,0,0 };     // досчитаем в Mesh::CreateGPU_PNTUV(..., true)
            vtx.tangent = { 0,0,0,0 };
            vtx.uv = { u, v };
            outVerts.push_back(vtx);
        }
    }

    // индексы: следим за ориентацией
    const uint32_t ring = (uint32_t)radialSegments;
    auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (frontCW) { outIdx.push_back(a); outIdx.push_back(c); outIdx.push_back(b); } // CW
        else { outIdx.push_back(a); outIdx.push_back(b); outIdx.push_back(c); } // CCW
        };

    for (int j = 0; j < rows - 1; ++j) {
        const uint32_t row0 = uint32_t(j) * ring;
        const uint32_t row1 = uint32_t(j + 1) * ring;
        for (int i = 0; i < radialSegments; ++i) {
            const uint32_t i0 = (uint32_t)i;
            const uint32_t i1 = (i0 + 1) % ring;

            const uint32_t a = row0 + i0;
            const uint32_t b = row0 + i1;
            const uint32_t c = row1 + i1;
            const uint32_t d = row1 + i0;

            // два треугольника квада: a-b-c, a-c-d
            emit(a, b, c);
            emit(a, c, d);
        }
    }
}

inline void BuildCubeCW(std::vector<VertexPNTUV>& outVerts,
    std::vector<uint32_t>& outIdx)
{
    std::vector<VertexPNTUV> cubeVerts = {
        // +X (x=+0.5), N=(1,0,0)
        {{+0.5f,-0.5f,-0.5f}, {+1,0,0}, {0,0,0,0}, {0,0}},
        {{+0.5f,+0.5f,-0.5f}, {+1,0,0}, {0,0,0,0}, {0,1}},
        {{+0.5f,+0.5f,+0.5f}, {+1,0,0}, {0,0,0,0}, {1,1}},
        {{+0.5f,-0.5f,+0.5f}, {+1,0,0}, {0,0,0,0}, {1,0}},

        // -X (x=-0.5), N=(-1,0,0)
        {{-0.5f,-0.5f,+0.5f}, {-1,0,0}, {0,0,0,0}, {0,0}},
        {{-0.5f,+0.5f,+0.5f}, {-1,0,0}, {0,0,0,0}, {0,1}},
        {{-0.5f,+0.5f,-0.5f}, {-1,0,0}, {0,0,0,0}, {1,1}},
        {{-0.5f,-0.5f,-0.5f}, {-1,0,0}, {0,0,0,0}, {1,0}},

        // +Y (y=+0.5), N=(0,1,0)
        {{-0.5f,+0.5f,-0.5f}, {0,+1,0}, {0,0,0,0}, {0,0}},
        {{-0.5f,+0.5f,+0.5f}, {0,+1,0}, {0,0,0,0}, {0,1}},
        {{+0.5f,+0.5f,+0.5f}, {0,+1,0}, {0,0,0,0}, {1,1}},
        {{+0.5f,+0.5f,-0.5f}, {0,+1,0}, {0,0,0,0}, {1,0}},

        // -Y (y=-0.5), N=(0,-1,0)
        {{-0.5f,-0.5f,+0.5f}, {0,-1,0}, {0,0,0,0}, {0,0}},
        {{-0.5f,-0.5f,-0.5f}, {0,-1,0}, {0,0,0,0}, {0,1}},
        {{+0.5f,-0.5f,-0.5f}, {0,-1,0}, {0,0,0,0}, {1,1}},
        {{+0.5f,-0.5f,+0.5f}, {0,-1,0}, {0,0,0,0}, {1,0}},

        // +Z (z=+0.5), N=(0,0,1)
        {{-0.5f,-0.5f,+0.5f}, {0,0,+1}, {0,0,0,0}, {0,0}},
        {{-0.5f,+0.5f,+0.5f}, {0,0,+1}, {0,0,0,0}, {0,1}},
        {{+0.5f,+0.5f,+0.5f}, {0,0,+1}, {0,0,0,0}, {1,1}},
        {{+0.5f,-0.5f,+0.5f}, {0,0,+1}, {0,0,0,0}, {1,0}},

        // -Z (z=-0.5), N=(0,0,-1)
        {{+0.5f,-0.5f,-0.5f}, {0,0,-1}, {0,0,0,0}, {0,0}},
        {{+0.5f,+0.5f,-0.5f}, {0,0,-1}, {0,0,0,0}, {0,1}},
        {{-0.5f,+0.5f,-0.5f}, {0,0,-1}, {0,0,0,0}, {1,1}},
        {{-0.5f,-0.5f,-0.5f}, {0,0,-1}, {0,0,0,0}, {1,0}},
    };

    std::vector<uint32_t> cubeIndices = {
        0,1,2, 0,2,3,         // +X
        4,5,6, 4,6,7,         // -X
        8,9,10, 8,10,11,      // +Y
        12,13,14, 12,14,15,   // -Y
        16,18,17, 16,19,18,   // +Z
        20,22,21, 20,23,22    // -Z
    };

	outVerts = std::move(cubeVerts);
	outIdx = std::move(cubeIndices);
}