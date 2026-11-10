#include "ocean/WaterMask.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <queue>
#include <unordered_map>

#include "core/logging/Log.h"
#include "physics/PhysicsWorld.h"
#include "rendering/renderables/RenderableObject.h"

namespace ocean
{
namespace
{
    struct Entry
    {
        RenderableObject* object = nullptr;
        std::shared_ptr<const WaterMaskField> field;
        bool looked = false; // the field was asked for (it may have none)
    };
    struct Registry
    {
        std::mutex mutex;
        std::vector<Entry> entries;
        std::unordered_map<std::string, std::shared_ptr<const WaterMaskField>> fields; // by geometry; null = none
    };
    Registry& TheRegistry()
    {
        static Registry registry;
        return registry;
    }

    // Under the registry's lock: the field of a geometry, built the first time it is asked for.
    std::shared_ptr<const WaterMaskField> FieldOf(Registry& registry, const std::string& path)
    {
        if (path.empty()) { return nullptr; }
        const auto found = registry.fields.find(path);
        if (found != registry.fields.end()) { return found->second; }
        std::shared_ptr<const WaterMaskField> field = watermask::BuildField(path);
        if (!field)
        {
            LOG_WARNING(logging::LogCategory::Ocean, "water mask: no columns for {} -- the water is drawn inside it", path);
        }
        registry.fields.emplace(path, field);
        return field;
    }

    // A triangle clipped to an axis-aligned cell in xz (Sutherland-Hodgman, y carried along): the highest y of what is left,
    // -FLT_MAX when nothing is. A vertical triangle's projection is a segment, and the clip keeps that segment's part in
    // the cell -- a wall with no top face still raises its cells' rim.
    float ClippedTop(const Math::float3& a, const Math::float3& b, const Math::float3& c, float x0, float x1, float z0, float z1)
    {
        std::array<Math::float3, 12> buffers[2];
        std::size_t count = 3;
        buffers[0][0] = a;
        buffers[0][1] = b;
        buffers[0][2] = c;
        int current = 0;
        // Each edge of the cell: keep the side where `inside` holds, cut where it changes.
        const auto clip = [&](auto inside, auto cut)
        {
            const auto& in = buffers[current];
            auto& out = buffers[1 - current];
            std::size_t n = 0;
            for (std::size_t i = 0; i < count; ++i)
            {
                const Math::float3& p = in[i];
                const Math::float3& q = in[(i + 1) % count];
                const bool pIn = inside(p);
                const bool qIn = inside(q);
                if (pIn && n < out.size()) { out[n++] = p; }
                if (pIn != qIn && n < out.size()) { out[n++] = cut(p, q); }
            }
            count = n;
            current = 1 - current;
        };
        const auto atX = [](float x)
        {
            return [x](const Math::float3& p, const Math::float3& q)
            {
                const float t = (x - p.x) / (q.x - p.x);
                return Math::float3(x, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t);
            };
        };
        const auto atZ = [](float z)
        {
            return [z](const Math::float3& p, const Math::float3& q)
            {
                const float t = (z - p.z) / (q.z - p.z);
                return Math::float3(p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, z);
            };
        };
        clip([x0](const Math::float3& p) { return p.x >= x0; }, atX(x0));
        if (count == 0) { return -FLT_MAX; }
        clip([x1](const Math::float3& p) { return p.x <= x1; }, atX(x1));
        if (count == 0) { return -FLT_MAX; }
        clip([z0](const Math::float3& p) { return p.z >= z0; }, atZ(z0));
        if (count == 0) { return -FLT_MAX; }
        clip([z1](const Math::float3& p) { return p.z <= z1; }, atZ(z1));
        float top = -FLT_MAX;
        for (std::size_t i = 0; i < count; ++i) { top = std::max(top, buffers[current][i].y); }
        return top;
    }
}

namespace watermask
{
    std::shared_ptr<const WaterMaskField> BuildField(const std::string& geometryPath)
    {
        std::vector<Math::float3> positions;
        std::vector<std::uint32_t> indices;
        if (!physics::ReadSolidTriangles(geometryPath, positions, indices) || indices.size() < 3u) { return nullptr; }
        Math::float3 lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const Math::float3& p : positions)
        {
            lo = Math::float3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = Math::float3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        }
        const float cell = std::max(hi.x - lo.x, hi.z - lo.z) / static_cast<float>(kWaterMaskCellsAlong);
        if (!(cell > 1e-5f)) { return nullptr; }
        // Openings up to kWaterMaskGap wide are closed before the flood: a cockpit's walkthrough to the swim platform
        // otherwise drains it to its sill, and the water stands in the cockpit wherever the sea is over that sill.
        const int gap = std::max(1, static_cast<int>(std::ceil(0.5f * kWaterMaskGap / cell)));

        auto field = std::make_shared<WaterMaskField>();
        WaterMaskField& f = *field;
        f.cell = cell;
        const int pad = gap + 1; // free cells all round: the flood starts outside the mesh, the closing has room
        f.originX = lo.x - static_cast<float>(pad) * cell;
        f.originZ = lo.z - static_cast<float>(pad) * cell;
        f.nx = static_cast<std::uint32_t>(std::ceil((hi.x - lo.x) / cell)) + 2u * static_cast<std::uint32_t>(pad);
        f.nz = static_cast<std::uint32_t>(std::ceil((hi.z - lo.z) / cell)) + 2u * static_cast<std::uint32_t>(pad);
        f.minY = lo.y;
        f.maxY = hi.y;
        const std::size_t cells = static_cast<std::size_t>(f.nx) * f.nz;
        if (cells * 3u > kWaterMaskMaxCells) { return nullptr; } // [columns | the samples' skin | the quarters' tops]

        // Per cell its rim (the highest surface, clipped exactly); per quarter of a cell the lowest and the highest surface
        // over its centre (a vertical line through it).
        constexpr int kSamples = 2; // per side of a cell
        static_assert(kSamples == 2, "a cell's samples are one float4 on the GPU (water_mask_columns.hlsli)");
        std::vector<float> top(cells, -FLT_MAX);
        std::vector<float> rim(cells * kSamples * kSamples, -FLT_MAX); // the rim of each quarter of a cell
        std::vector<float> low(cells * kSamples * kSamples, FLT_MAX);
        std::vector<float> high(cells * kSamples * kSamples, -FLT_MAX);
        const auto cellOf = [&f](float v, float origin, std::uint32_t n)
        {
            return std::clamp(static_cast<int>(std::floor((v - origin) / f.cell)), 0, static_cast<int>(n) - 1);
        };
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3)
        {
            const Math::float3& a = positions[indices[t]];
            const Math::float3& b = positions[indices[t + 1]];
            const Math::float3& c = positions[indices[t + 2]];
            const int i0 = cellOf(std::min({ a.x, b.x, c.x }), f.originX, f.nx);
            const int i1 = cellOf(std::max({ a.x, b.x, c.x }), f.originX, f.nx);
            const int k0 = cellOf(std::min({ a.z, b.z, c.z }), f.originZ, f.nz);
            const int k1 = cellOf(std::max({ a.z, b.z, c.z }), f.originZ, f.nz);
            // The triangle's plane over xz, for the samples: y = a.y + w1 (b - a).y + w2 (c - a).y.
            const float e1x = b.x - a.x, e1z = b.z - a.z, e2x = c.x - a.x, e2z = c.z - a.z;
            const float det = e1x * e2z - e2x * e1z;
            const bool flat = std::fabs(det) > 1e-12f;
            for (int k = k0; k <= k1; ++k)
            {
                const float z0 = f.originZ + static_cast<float>(k) * f.cell;
                for (int i = i0; i <= i1; ++i)
                {
                    const float x0 = f.originX + static_cast<float>(i) * f.cell;
                    const std::size_t index = static_cast<std::size_t>(k) * f.nx + static_cast<std::size_t>(i);
                    if (ClippedTop(a, b, c, x0, x0 + f.cell, z0, z0 + f.cell) == -FLT_MAX) { continue; }
                    // The rim, a quarter of the cell at a time: a wall with no top face raises the quarters it crosses.
                    const float half = f.cell / kSamples;
                    for (int sk = 0; sk < kSamples; ++sk)
                    {
                        for (int si = 0; si < kSamples; ++si)
                        {
                            const float qx = x0 + static_cast<float>(si) * half;
                            const float qz = z0 + static_cast<float>(sk) * half;
                            const float clipped = ClippedTop(a, b, c, qx, qx + half, qz, qz + half);
                            float& quarter = rim[index * kSamples * kSamples + static_cast<std::size_t>(sk) * kSamples + si];
                            quarter = std::max(quarter, clipped);
                            top[index] = std::max(top[index], clipped);
                        }
                    }
                    if (!flat) { continue; }
                    for (int sk = 0; sk < kSamples; ++sk)
                    {
                        for (int si = 0; si < kSamples; ++si)
                        {
                            const float px = x0 + (static_cast<float>(si) + 0.5f) * f.cell / kSamples - a.x;
                            const float pz = z0 + (static_cast<float>(sk) + 0.5f) * f.cell / kSamples - a.z;
                            const float w1 = (px * e2z - e2x * pz) / det;
                            const float w2 = (e1x * pz - px * e1z) / det;
                            constexpr float kEdge = -1e-5f; // a shared edge belongs to both: no sample falls between
                            if (w1 < kEdge || w2 < kEdge || w1 + w2 > 1.0f - kEdge) { continue; }
                            const float y = a.y + w1 * (b.y - a.y) + w2 * (c.y - a.y);
                            const std::size_t s = index * kSamples * kSamples + static_cast<std::size_t>(sk) * kSamples + si;
                            low[s] = std::min(low[s], y);
                            high[s] = std::max(high[s], y);
                        }
                    }
                }
            }
        }

        // The rim with its narrow openings closed: a grey-scale closing (the max, then the min, over a square of `gap`
        // cells each way, separable), never below the rim itself. A wide low area -- the swim platform aft of the transom,
        // the sea -- keeps its height; only a gap walled on both sides narrower than kWaterMaskGap is lifted.
        std::vector<float> closed = top;
        {
            std::vector<float> scratch(cells);
            const auto pass = [&](std::vector<float>& src, std::vector<float>& dst, bool alongX, bool takeMax)
            {
                const int n = static_cast<int>(alongX ? f.nx : f.nz);
                const int m = static_cast<int>(alongX ? f.nz : f.nx);
                for (int line = 0; line < m; ++line)
                {
                    for (int at = 0; at < n; ++at)
                    {
                        float v = takeMax ? -FLT_MAX : FLT_MAX;
                        for (int d = -gap; d <= gap; ++d)
                        {
                            const int q = at + d;
                            // Off the grid is the open sea: it bounds an erosion, it raises nothing.
                            const float h = (q < 0 || q >= n) ? -FLT_MAX
                                : src[alongX ? static_cast<std::size_t>(line) * f.nx + q : static_cast<std::size_t>(q) * f.nx + line];
                            v = takeMax ? std::max(v, h) : std::min(v, h);
                        }
                        dst[alongX ? static_cast<std::size_t>(line) * f.nx + at : static_cast<std::size_t>(at) * f.nx + line] = v;
                    }
                }
            };
            pass(closed, scratch, true, true);
            pass(scratch, closed, false, true);
            pass(closed, scratch, true, false);
            pass(scratch, closed, false, false);
            for (std::size_t index = 0; index < cells; ++index) { closed[index] = std::max(closed[index], top[index]); }
        }

        // The depressions filled from outside (priority flood, 4-connected: water does not pass between two cells that
        // touch only at a corner): a cell's level is the lowest rim water from outside must climb to reach it.
        std::vector<float> level(cells, -FLT_MAX);
        std::vector<std::uint8_t> reached(cells, 0u);
        using Item = std::pair<float, std::uint32_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
        for (std::uint32_t k = 0; k < f.nz; ++k)
        {
            for (std::uint32_t i = 0; i < f.nx; ++i)
            {
                if (k != 0 && k != f.nz - 1u && i != 0 && i != f.nx - 1u) { continue; }
                const std::uint32_t index = k * f.nx + i;
                level[index] = closed[index];
                reached[index] = 1u;
                open.emplace(level[index], index);
            }
        }
        while (!open.empty())
        {
            const auto [height, index] = open.top();
            open.pop();
            const std::uint32_t i = index % f.nx;
            const std::uint32_t k = index / f.nx;
            const std::uint32_t next[4] = { i > 0 ? index - 1u : index, i + 1 < f.nx ? index + 1u : index,
                                            k > 0 ? index - f.nx : index, k + 1 < f.nz ? index + f.nx : index };
            for (const std::uint32_t n : next)
            {
                if (reached[n]) { continue; }
                reached[n] = 1u;
                level[n] = std::max(closed[n], height);
                open.emplace(level[n], n);
            }
        }

        // ---- the columns ----
        // A cell keeps water out only if every sample of it is under the mesh (a cell on the plan's outline keeps none).
        //
        // Its BOTTOM is the hull's skin: the lowest surface over each of its four samples, kept per sample and
        // interpolated by the surface's shader (water_mask_columns.hlsli HullKeepsWaterOut). It was one number a cell, the
        // HIGHEST of the four, so the mask stopped up to a cell inside the skin -- unseen in a hull with an inner liner
        // (small_yacht), a line of water along the waterline INSIDE a single-skinned one (police_boat; owner: "I see
        // a leak"). Where the lowest surface JUMPS between two samples (kWaterMaskJump: a wall with a deck overhanging the
        // sea behind it, a transom) the lower side is carried one sample across: the wall stands somewhere between the two,
        // and a strip of sea cut under an overhang is not seen where a strip of water on a cockpit's floor is.
        const int subX = static_cast<int>(f.nx) * kSamples;
        const int subZ = static_cast<int>(f.nz) * kSamples;
        const auto cellOfSub = [&f](int x, int z)
        {
            return static_cast<std::size_t>(z / kSamples) * f.nx + static_cast<std::size_t>(x / kSamples);
        };
        const auto subIndex = [&cellOfSub](int x, int z)
        {
            return cellOfSub(x, z) * kSamples * kSamples + static_cast<std::size_t>(z % kSamples) * kSamples + static_cast<std::size_t>(x % kSamples);
        };
        static constexpr int kStep[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
        std::vector<float> skin(low.size(), FLT_MAX);
        for (int z = 0; z < subZ; ++z)
        {
            for (int x = 0; x < subX; ++x)
            {
                const std::size_t s = subIndex(x, z);
                const float own = low[s];
                if (own == FLT_MAX) { continue; }
                float carried = own;
                for (const auto& step : kStep)
                {
                    const int qx = x + step[0];
                    const int qz = z + step[1];
                    if (qx < 0 || qz < 0 || qx >= subX || qz >= subZ) { continue; }
                    const float beside = low[subIndex(qx, qz)];
                    if (beside < own - kWaterMaskJump) { carried = std::min(carried, beside); }
                }
                skin[s] = carried;
            }
        }
        std::vector<float> bottom(cells, FLT_MAX), shared(cells, -FLT_MAX);
        std::vector<std::uint8_t> depression(cells, 0u);
        for (std::size_t index = 0; index < cells; ++index)
        {
            bool covered = true;
            float b = FLT_MAX, h = FLT_MAX;
            for (int s = 0; s < kSamples * kSamples; ++s)
            {
                const float under = skin[index * kSamples * kSamples + s];
                covered = covered && under != FLT_MAX;
                b = std::min(b, under);
                h = std::min(h, high[index * kSamples * kSamples + s]);
            }
            bottom[index] = covered ? b : FLT_MAX;
            shared[index] = h;
            depression[index] = covered && level[index] > top[index] + 0.01f ? 1u : 0u;
        }

        // Its TOP, a quarter of a cell at a time:
        //   in a filled depression (its level over its own rim): the level -- the cockpit's air to its edge;
        //   in a cell that is no depression only for what STANDS in it (a wall, a pillar, a rail over the floor: its rim
        //   reaches the level): the quarters that lie under the level all over are the same air, when they touch it -- the
        //   level goes from quarter to quarter, highest first. It used to reach a depression's direct neighbours alone, and
        //   the cells along a pillar that crosses the floor aslant kept their floor's slab and no air: a stair of water;
        //   a quarter the level did not enter (the wall itself) beside one it did: the higher of the level and its own
        //   highest surface, so no band of water lies along the wall;
        //   any other: its own highest surface -- the swim platform keeps only its slab, a cell on the outer skin half
        //   over a spray rail keeps the rail and not the air beside the topside.
        constexpr float kOpen = 0.02f; // a quarter's rim this far under a level is open to it
        std::vector<float> basin(low.size(), -FLT_MAX);
        using Seed = std::pair<float, std::uint32_t>; // a level, a quarter (z * subX + x)
        std::priority_queue<Seed> seeds;              // the highest level first
        for (int z = 0; z < subZ; ++z)
        {
            for (int x = 0; x < subX; ++x)
            {
                const std::size_t cellIndex = cellOfSub(x, z);
                if (!depression[cellIndex]) { continue; }
                basin[subIndex(x, z)] = level[cellIndex];
                seeds.emplace(level[cellIndex], static_cast<std::uint32_t>(z * subX + x));
            }
        }
        while (!seeds.empty())
        {
            const auto [height, at] = seeds.top();
            seeds.pop();
            const int x = static_cast<int>(at % static_cast<std::uint32_t>(subX));
            const int z = static_cast<int>(at / static_cast<std::uint32_t>(subX));
            for (const auto& step : kStep)
            {
                const int qx = x + step[0];
                const int qz = z + step[1];
                if (qx < 0 || qz < 0 || qx >= subX || qz >= subZ) { continue; }
                const std::size_t s = subIndex(qx, qz);
                const std::size_t cellIndex = cellOfSub(qx, qz);
                if (basin[s] != -FLT_MAX || depression[cellIndex] || bottom[cellIndex] == FLT_MAX) { continue; }
                if (!(top[cellIndex] > height - kOpen) || !(rim[s] < height - kOpen)) { continue; }
                basin[s] = height;
                seeds.emplace(height, static_cast<std::uint32_t>(qz * subX + qx));
            }
        }
        std::vector<float> tops(low.size(), -FLT_MAX);
        for (int z = 0; z < subZ; ++z)
        {
            for (int x = 0; x < subX; ++x)
            {
                const std::size_t s = subIndex(x, z);
                if (basin[s] != -FLT_MAX)
                {
                    tops[s] = basin[s];
                    continue;
                }
                float own = high[s];
                for (const auto& step : kStep)
                {
                    const int qx = x + step[0];
                    const int qz = z + step[1];
                    if (qx < 0 || qz < 0 || qx >= subX || qz >= subZ) { continue; }
                    own = std::max(own, basin[subIndex(qx, qz)]);
                }
                tops[s] = own;
            }
        }

        // On the GPU: [a column a cell | its four samples' skin | its four quarters' tops].
        f.columns.assign(cells, Math::float4(FLT_MAX, -FLT_MAX, -FLT_MAX, FLT_MAX));
        f.columns.resize(cells * 2u, Math::float4(FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX));
        f.columns.resize(cells * 3u, Math::float4(-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX));
        // The plan (the fourth component): the lowest surface over every cell the mesh touches at all, whether or not it
        // keeps water out there. A cell under one sloping sheet (a cockpit's inner wall with nothing beneath it in the
        // mesh), or with a sample the mesh does not cover, has no column -- right for the water, which must stop inside the
        // skin -- and a particle tested against the columns alone flew on through the cockpit there (police_boat). A thin
        // part no sample meets (a rail, an aerial) counts from its top.
        std::size_t plan = 0;
        for (std::size_t index = 0; index < cells; ++index)
        {
            if (top[index] == -FLT_MAX) { continue; }
            float lowest = FLT_MAX;
            for (int s = 0; s < kSamples * kSamples; ++s) { lowest = std::min(lowest, low[index * kSamples * kSamples + s]); }
            f.columns[index].w = lowest != FLT_MAX ? lowest : top[index];
            ++plan;
        }
        std::size_t kept = 0, filled = 0;
        for (std::size_t index = 0; index < cells; ++index)
        {
            if (bottom[index] == FLT_MAX) { continue; }
            const float* under = &skin[index * kSamples * kSamples];
            const float* over = &tops[index * kSamples * kSamples];
            const float columnTop = std::max(std::max(over[0], over[1]), std::max(over[2], over[3]));
            if (!(columnTop > bottom[index])) { continue; }
            // The solid: under a filled depression's air, the cell's own highest surface (the floor, a seat).
            const float solidTop = depression[index] ? top[index] : shared[index];
            f.columns[index] = Math::float4(bottom[index], columnTop, std::max(solidTop, bottom[index]), f.columns[index].w);
            f.columns[cells + index] = Math::float4(under[0], under[1], under[2], under[3]);
            f.columns[cells * 2u + index] = Math::float4(over[0], over[1], over[2], over[3]);
            ++kept;
            if (depression[index]) { ++filled; }
        }
        LOG_INFO(logging::LogCategory::Ocean,
                 "water mask: {} -- {} x {} cells of {:.1f} cm, {} keep the water out, {} of them in a filled depression, {} in its plan, from {} triangles",
                 geometryPath, f.nx, f.nz, f.cell * 100.0f, kept, filled, plan, indices.size() / 3u);
        return field;
    }

    void Register(RenderableObject* object)
    {
        Registry& registry = TheRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        const auto it = std::find_if(registry.entries.begin(), registry.entries.end(), [&](const Entry& e) { return e.object == object; });
        if (it == registry.entries.end()) { registry.entries.push_back(Entry{ object, nullptr, false }); }
    }

    void Unregister(RenderableObject* object)
    {
        Registry& registry = TheRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        registry.entries.erase(std::remove_if(registry.entries.begin(), registry.entries.end(),
                                              [&](const Entry& e) { return e.object == object; }),
                               registry.entries.end());
    }

    void Collect(std::vector<WaterMaskInstance>& out)
    {
        out.clear();
        Registry& registry = TheRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        for (Entry& entry : registry.entries)
        {
            if (!entry.looked)
            {
                entry.field = FieldOf(registry, entry.object->GetGeometryPath());
                entry.looked = true;
            }
            if (!entry.field || !entry.object->IsVisibleGame()) { continue; }
            out.push_back(WaterMaskInstance{ entry.field, Math::mat4::Inverse(entry.object->GetModelMatrix()) });
        }
    }
}
} // namespace ocean
