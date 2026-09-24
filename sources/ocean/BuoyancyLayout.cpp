#include "ocean/BuoyancyLayout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

#include "core/logging/Log.h"
#include "rendering/meshes/MeshManager.h"

namespace buoyancy
{
namespace
{
    constexpr float kPi = 3.14159265358979f;
    constexpr float kDefaultDraftOfHull = 0.25f;
    // Plan-view grid resolution along the longer side. 64 cells put ~7 cm under a 4.5 m boat: fine
    // enough that a pontoon bin is dozens of cells, coarse enough to rasterise any prop instantly.
    constexpr int kGridCells = 64;
    constexpr int kMaxGridSide = 256;

    // Principal plan-view direction (the hull's length), area-weighted over the triangles. Snapped
    // to a model axis when within 8 degrees: authored boats are axis-aligned, and a layout 2 degrees
    // off its own hull looks like a mistake in the Mesh Editor for no gain.
    Math::float2 LengthAxis(const std::vector<Math::float3>& p, const std::vector<std::uint32_t>& idx)
    {
        double sumA = 0.0, mx = 0.0, mz = 0.0;
        const size_t triCount = idx.size() / 3;
        for (size_t t = 0; t < triCount; ++t)
        {
            const Math::float3& a = p[idx[t * 3 + 0]];
            const Math::float3& b = p[idx[t * 3 + 1]];
            const Math::float3& c = p[idx[t * 3 + 2]];
            const double area = 0.5 * (b - a).Cross(c - a).Length();
            sumA += area;
            mx += area * (a.x + b.x + c.x) / 3.0;
            mz += area * (a.z + b.z + c.z) / 3.0;
        }
        if (sumA <= 0.0) { return Math::float2(1.0f, 0.0f); }
        mx /= sumA;
        mz /= sumA;
        double cxx = 0.0, cxz = 0.0, czz = 0.0;
        for (size_t t = 0; t < triCount; ++t)
        {
            const Math::float3& a = p[idx[t * 3 + 0]];
            const Math::float3& b = p[idx[t * 3 + 1]];
            const Math::float3& c = p[idx[t * 3 + 2]];
            const double area = 0.5 * (b - a).Cross(c - a).Length();
            const double dx = (a.x + b.x + c.x) / 3.0 - mx;
            const double dz = (a.z + b.z + c.z) / 3.0 - mz;
            cxx += area * dx * dx;
            cxz += area * dx * dz;
            czz += area * dz * dz;
        }
        double angle = 0.5 * std::atan2(2.0 * cxz, cxx - czz);
        const double quarter = kPi * 0.5;
        const double snapped = std::round(angle / quarter) * quarter;
        if (std::abs(angle - snapped) < 8.0 * kPi / 180.0) { angle = snapped; }
        return Math::float2(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
    }

    struct CachedAnalysis
    {
        Layout layout;
        bool ok = false;
    };

    std::mutex g_cacheMutex;
    std::map<std::pair<std::string, int>, CachedAnalysis> g_cache;

    bool LoadGeometry(const std::string& path, std::vector<Math::float3>& positions,
        std::vector<std::uint32_t>& indices)
    {
        MeshCpuData cpu;
        MeshLoadOptions opt;
        opt.generateTangentSpace = false; // positions are all the analysis reads
        MeshManager manager;
        if (path.empty() || !manager.ParseFileCpu(path, cpu, opt) || cpu.vertices.empty())
        {
            return false;
        }
        positions.resize(cpu.vertices.size());
        for (size_t i = 0; i < cpu.vertices.size(); ++i)
        {
            positions[i] = Math::float3(cpu.vertices[i].position);
        }
        // LOD0 only: a .mesh.bin's vertex buffer can carry LOD3 leaf-prune copies appended past
        // LOD0's range, and walking the raw vertex array would count them.
        indices = std::move(cpu.indices);
        return indices.size() >= 3;
    }
}

Layout AutoLayout(const std::vector<Math::float3>& positions,
    const std::vector<std::uint32_t>& indices,
    float draft)
{
    Layout out;
    out.automatic = true;
    const size_t triCount = indices.size() / 3;
    if (triCount == 0) { return out; }
    for (const std::uint32_t i : indices) { if (i >= positions.size()) { return out; } }

    const Math::float2 axis = LengthAxis(positions, indices);
    const float ux = axis.x, uz = axis.y; // u = length, v = (-uz, ux) = beam
    const auto toU = [&](const Math::float3& p) { return p.x * ux + p.z * uz; };
    const auto toV = [&](const Math::float3& p) { return -p.x * uz + p.z * ux; };

    float u0 = std::numeric_limits<float>::max(), u1 = std::numeric_limits<float>::lowest();
    float v0 = u0, v1 = u1, keel = u0;
    for (const std::uint32_t i : indices)
    {
        const Math::float3& p = positions[i];
        u0 = std::min(u0, toU(p)); u1 = std::max(u1, toU(p));
        v0 = std::min(v0, toV(p)); v1 = std::max(v1, toV(p));
        keel = std::min(keel, p.y);
    }
    const float cell = std::max(std::max(u1 - u0, v1 - v0) / static_cast<float>(kGridCells), 1e-3f);
    const int nu = std::clamp(static_cast<int>(std::ceil((u1 - u0) / cell)) + 1, 1, kMaxGridSide);
    const int nv = std::clamp(static_cast<int>(std::ceil((v1 - v0) / cell)) + 1, 1, kMaxGridSide);
    std::vector<float> bottom(static_cast<size_t>(nu) * nv, std::numeric_limits<float>::max());
    std::vector<float> top(static_cast<size_t>(nu) * nv, std::numeric_limits<float>::lowest());
    const auto cellIndex = [&](int i, int j) { return static_cast<size_t>(j) * nu + i; };
    const auto mark = [&](int i, int j, float y)
    {
        if (i < 0 || j < 0 || i >= nu || j >= nv) { return; }
        const size_t k = cellIndex(i, j);
        bottom[k] = std::min(bottom[k], y);
        top[k] = std::max(top[k], y);
    };

    // Rasterise every triangle's plan view (cell centres inside it get the interpolated height),
    // plus every vertex into its own cell so a wall seen edge-on still registers.
    for (size_t t = 0; t < triCount; ++t)
    {
        const Math::float3& a = positions[indices[t * 3 + 0]];
        const Math::float3& b = positions[indices[t * 3 + 1]];
        const Math::float3& c = positions[indices[t * 3 + 2]];
        const float au = (toU(a) - u0) / cell, av = (toV(a) - v0) / cell;
        const float bu = (toU(b) - u0) / cell, bv = (toV(b) - v0) / cell;
        const float cu = (toU(c) - u0) / cell, cv = (toV(c) - v0) / cell;
        mark(static_cast<int>(au), static_cast<int>(av), a.y);
        mark(static_cast<int>(bu), static_cast<int>(bv), b.y);
        mark(static_cast<int>(cu), static_cast<int>(cv), c.y);
        const float area = (bu - au) * (cv - av) - (bv - av) * (cu - au);
        if (std::abs(area) < 1e-8f) { continue; }
        const int i0 = std::max(0, static_cast<int>(std::floor(std::min({ au, bu, cu }))));
        const int i1 = std::min(nu - 1, static_cast<int>(std::ceil(std::max({ au, bu, cu }))));
        const int j0 = std::max(0, static_cast<int>(std::floor(std::min({ av, bv, cv }))));
        const int j1 = std::min(nv - 1, static_cast<int>(std::ceil(std::max({ av, bv, cv }))));
        for (int j = j0; j <= j1; ++j)
        {
            for (int i = i0; i <= i1; ++i)
            {
                const float pu = static_cast<float>(i) + 0.5f, pv = static_cast<float>(j) + 0.5f;
                const float w0 = ((bu - pu) * (cv - pv) - (bv - pv) * (cu - pu)) / area;
                const float w1 = ((cu - pu) * (av - pv) - (cv - pv) * (au - pu)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) { continue; }
                mark(i, j, w0 * a.y + w1 * b.y + w2 * c.y);
            }
        }
    }

    // Hull height = keel to gunwale, the gunwale being the 90th percentile of the covered cells'
    // tops: the rim and thwarts, not a mast or an oar handle poking up through a few cells.
    std::vector<float> tops;
    for (size_t k = 0; k < top.size(); ++k)
    {
        if (bottom[k] <= top[k]) { tops.push_back(top[k]); }
    }
    if (tops.empty()) { return out; }
    std::nth_element(tops.begin(), tops.begin() + static_cast<std::ptrdiff_t>(tops.size() * 9 / 10), tops.end());
    const float gunwale = tops[tops.size() * 9 / 10];
    out.keel = keel;
    out.hullHeight = std::max(gunwale - keel, 0.01f);
    out.draft = draft >= 0.0f ? draft : std::max(out.hullHeight * kDefaultDraftOfHull, 0.02f);
    out.waterline = keel + out.draft;

    // The waterplane: covered cells whose underside is below the resting waterline.
    std::vector<std::uint8_t> wet(bottom.size(), 0u);
    int wetCount = 0;
    float wu0 = std::numeric_limits<float>::max(), wu1 = std::numeric_limits<float>::lowest();
    float wv0 = wu0, wv1 = wu1;
    for (int j = 0; j < nv; ++j)
    {
        for (int i = 0; i < nu; ++i)
        {
            const size_t k = cellIndex(i, j);
            if (bottom[k] > top[k] || bottom[k] >= out.waterline) { continue; }
            wet[k] = 1u;
            ++wetCount;
            wu0 = std::min(wu0, static_cast<float>(i)); wu1 = std::max(wu1, static_cast<float>(i));
            wv0 = std::min(wv0, static_cast<float>(j)); wv1 = std::max(wv1, static_cast<float>(j));
        }
    }
    if (wetCount == 0) { return out; }

    // Bins: nW across the beam (2 whenever the hull has a beam at all -- one row of pontoons on the
    // centreline has no righting moment and capsizes), nL along the length at about TWICE the
    // crosswise spacing, 2..3. Halved on the owner's call (2026-09-24) from "the same spacing, 2..6",
    // which gave a 4.6 m dinghy 12: a hull answers waves longer than itself, and three bins along it
    // still follow those; the total waterplane (and so the heave stiffness) is unchanged, only split
    // into fewer, larger spheres. Each u-slab splits across ITS OWN centreline, so a tapering bow
    // still gets a pontoon each side instead of an empty bin.
    const float lengthCells = wu1 - wu0 + 1.0f;
    const float beamCells = wv1 - wv0 + 1.0f;
    const int nW = beamCells >= 2.0f ? 2 : 1;
    const int nL = std::clamp(static_cast<int>(std::lround(0.5f * lengthCells / (beamCells / nW))), 2, 3);
    std::vector<double> slabV(nL, 0.0);
    std::vector<int> slabN(nL, 0);
    const auto slabOf = [&](int i)
    {
        return std::min(nL - 1, static_cast<int>((static_cast<float>(i) - wu0) / lengthCells * nL));
    };
    for (int j = 0; j < nv; ++j)
    {
        for (int i = 0; i < nu; ++i)
        {
            if (!wet[cellIndex(i, j)]) { continue; }
            const int s = slabOf(i);
            slabV[s] += j;
            ++slabN[s];
        }
    }
    struct Bin { double su = 0.0, sv = 0.0; int n = 0; };
    std::vector<Bin> bins(static_cast<size_t>(nL) * nW);
    for (int j = 0; j < nv; ++j)
    {
        for (int i = 0; i < nu; ++i)
        {
            if (!wet[cellIndex(i, j)]) { continue; }
            const int s = slabOf(i);
            const double mid = slabN[s] > 0 ? slabV[s] / slabN[s] : 0.0;
            const int w = nW == 2 ? (j < mid ? 0 : 1) : 0;
            Bin& bin = bins[static_cast<size_t>(s) * nW + w];
            bin.su += i + 0.5;
            bin.sv += j + 0.5;
            ++bin.n;
        }
    }
    const float cellArea = cell * cell;
    for (const Bin& bin : bins)
    {
        if (bin.n == 0) { continue; }
        const float u = u0 + static_cast<float>(bin.su / bin.n) * cell;
        const float v = v0 + static_cast<float>(bin.sv / bin.n) * cell;
        Pontoon pontoon;
        pontoon.center = Math::float3(u * ux - v * uz, out.waterline, u * uz + v * ux);
        pontoon.radius = std::sqrt(static_cast<float>(bin.n) * cellArea / kPi);
        out.pontoons.push_back(pontoon);
    }
    return out;
}

void ForgetGeometry(const std::string& geometryPath)
{
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    for (auto it = g_cache.begin(); it != g_cache.end();)
    {
        it = (geometryPath.empty() || it->first.first == geometryPath) ? g_cache.erase(it) : std::next(it);
    }
}

Layout Resolve(const Settings& settings, const std::string& geometryPath)
{
    // The geometry analysis depends on the path and (for automatic pontoons) the draft; keyed to
    // the millimetre so a draft typed as 0.3 and one computed as 0.30000001 share an entry.
    const bool autoPontoons = settings.pontoons.empty();
    const int draftKey = settings.draft >= 0.0f ? static_cast<int>(std::lround(settings.draft * 1000.0f)) : -1;
    const std::pair<std::string, int> key(geometryPath, draftKey);

    Layout analysis;
    bool analysed = false;
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        const auto it = g_cache.find(key);
        if (it != g_cache.end()) { analysis = it->second.layout; analysed = it->second.ok; }
        else
        {
            std::vector<Math::float3> positions;
            std::vector<std::uint32_t> indices;
            CachedAnalysis entry;
            if (LoadGeometry(geometryPath, positions, indices))
            {
                entry.layout = AutoLayout(positions, indices, settings.draft);
                entry.ok = entry.layout.hullHeight > 0.0f;
                float area = 0.0f;
                for (const Pontoon& p : entry.layout.pontoons) { area += kPi * p.radius * p.radius; }
                LOG_INFO(logging::LogCategory::Ocean,
                    "buoyancy: {} -> {} automatic pontoons, keel {:.3f}, hull {:.3f} m, draft {:.3f} m, waterplane {:.2f} m2",
                    geometryPath, entry.layout.pontoons.size(), entry.layout.keel, entry.layout.hullHeight,
                    entry.layout.draft, area);
            }
            else
            {
                LOG_WARNING(logging::LogCategory::Ocean,
                    "buoyancy: no CPU geometry for '{}' -- only explicit pontoons can float it", geometryPath);
            }
            g_cache.emplace(key, entry);
            analysis = entry.layout;
            analysed = entry.ok;
        }
    }

    Layout out;
    out.inertia = std::clamp(settings.inertia, 0.05f, 100.0f);
    out.damping = std::clamp(settings.damping, 0.0f, 4.0f);
    if (autoPontoons)
    {
        if (!analysed) { return out; }
        analysis.inertia = out.inertia;
        analysis.damping = out.damping;
        return analysis;
    }

    out.pontoons = settings.pontoons;
    out.automatic = false;
    if (analysed)
    {
        out.keel = analysis.keel;
        out.hullHeight = analysis.hullHeight;
    }
    else
    {
        // No geometry to measure: the pontoons themselves are the hull.
        float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
        for (const Pontoon& p : out.pontoons)
        {
            lo = std::min(lo, p.center.y - p.radius);
            hi = std::max(hi, p.center.y + p.radius);
        }
        out.keel = lo;
        out.hullHeight = std::max(hi - lo, 0.01f);
    }
    out.draft = settings.draft >= 0.0f ? settings.draft :
        std::max(out.hullHeight * kDefaultDraftOfHull, 0.02f);
    out.waterline = out.keel + out.draft;
    return out;
}
}
