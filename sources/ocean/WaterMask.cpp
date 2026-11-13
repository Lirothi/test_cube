#include "ocean/WaterMask.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

#include "meshoptimizer.h"

#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"
#include "core/task/TaskSystem.h"
#include "physics/PhysicsWorld.h"
#include "rendering/renderables/RenderableObject.h"

namespace ocean
{
namespace
{
    // A field being built on a worker (WaterMask.h, THE BAKE): the frame takes it once `done` says so, and logs `note`.
    struct Job
    {
        std::atomic<bool> done{ false };
        std::shared_ptr<const WaterMaskField> field;
        std::string note;
    };
    struct Entry
    {
        RenderableObject* object = nullptr;
        std::shared_ptr<const WaterMaskField> field;
        bool looked = false; // its key was made (its field may be building, or there may be none)
        std::string key;     // the registry's: the geometry and its bake key
    };
    struct Registry
    {
        std::mutex mutex;
        std::vector<Entry> entries;
        std::unordered_map<std::string, std::shared_ptr<const WaterMaskField>> fields; // by key; null = none
        std::unordered_map<std::string, std::shared_ptr<Job>> jobs;                     // by key; building
    };
    Registry& TheRegistry()
    {
        static Registry registry;
        return registry;
    }

    std::string RegistryKey(const std::string& path, const WaterMaskBakeKey& key)
    {
        std::string out = path + '|' + std::to_string(key.geometryBytes) + '|' + std::to_string(key.geometryTime);
        std::uint32_t bits[2];
        std::memcpy(&bits[0], &key.waterline, sizeof(float));
        std::memcpy(&bits[1], &key.draftOverLowest, sizeof(float));
        out += '|' + std::to_string(bits[0]) + '|' + std::to_string(bits[1]) + '|';
        for (const int slot : key.glassSlots) { out += std::to_string(slot) + ','; }
        return out;
    }

    bool WriteBakeFile(const std::string& path, const std::vector<std::uint8_t>& bytes, std::string& why)
    {
        // Beside the target and renamed over it: two editors (or the owner's and a script's) never leave half a file.
        const std::string temp = path + ".tmp" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) { why = "cannot open " + temp; return false; }
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!out) { why = "cannot write " + temp; return false; }
        }
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        if (ec)
        {
            std::filesystem::remove(temp, ec);
            why = "cannot replace " + path;
            return false;
        }
        return true;
    }

    // Under the registry's lock: the field of a key, from what is known, from the bake on disk, or -- neither -- a worker's job
    // started (null until it lands).
    std::shared_ptr<const WaterMaskField> Resolve(Registry& registry, const std::string& key, const std::string& path,
                                                  const WaterMaskBakeKey& bakeKey)
    {
        if (const auto found = registry.fields.find(key); found != registry.fields.end()) { return found->second; }
        if (registry.jobs.count(key) != 0u) { return nullptr; }
        const std::string bakePath = watermask::BakePathOf(path);
        std::string why = "no bake file";
        if (!bakePath.empty())
        {
            const auto started = std::chrono::steady_clock::now();
            std::vector<std::uint8_t> bytes;
            if (vfs::ReadAll(bakePath, bytes))
            {
                std::shared_ptr<WaterMaskField> field = watermask::DeserializeField(bytes.data(), bytes.size(), &bakeKey, why);
                if (field)
                {
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                    LOG_INFO(logging::LogCategory::Ocean, "water mask: {} -- read the bake {} ({} KB, {} basins, the lid {} triangles) in {:.1f} ms",
                             path, bakePath, bytes.size() / 1024u, field->basins.size(), field->lidIndices.size() / 3u, ms);
                    registry.fields.emplace(key, field);
                    return field;
                }
            }
        }
        // Built on a worker: a frame never waits for it (100-300 ms a boat), the hull keeps no water out until it lands.
        LOG_INFO(logging::LogCategory::Ocean, "water mask: {} -- {} ({}): building it on a worker", path,
                 bakePath.empty() ? std::string("no bake for this geometry") : "the bake does not serve", why);
        auto job = std::make_shared<Job>();
        registry.jobs.emplace(key, job);
        TaskSystem::Get().SubmitDetach([job, path, bakePath, bakeKey]()
        {
            const auto started = std::chrono::steady_clock::now();
            std::shared_ptr<const WaterMaskField> field =
                watermask::BuildField(path, bakeKey.glassSlots, bakeKey.waterline, bakeKey.draftOverLowest);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            char head[96];
            std::snprintf(head, sizeof(head), "built on a worker in %.0f ms", ms);
            std::string note = head;
            if (!field) { note += ", no mask -- the water is drawn inside it"; }
#if WITH_EDITOR
            else if (!bakePath.empty() && !vfs::IsLooseFile(path))
            {
                // A packaged geometry's key is its content hash, not a loose file's time: a bake written for it would be
                // stale for the loose file beside it, and the next run on loose files would write it back. The cook
                // (docs/distribution_build_plan.md §8.1) writes the package's bakes.
                note += ", not baked: the geometry comes from a package";
            }
            else if (!bakePath.empty())
            {
                const std::vector<std::uint8_t> bytes = watermask::SerializeField(*field, bakeKey);
                std::string why;
                note += WriteBakeFile(bakePath, bytes, why) ? ", baked into " + bakePath + " (" + std::to_string(bytes.size() / 1024u) + " KB)"
                                                            : ", NOT baked: " + why;
            }
#endif
            job->field = std::move(field);
            job->note = std::move(note);
            job->done.store(true, std::memory_order_release);
        });
        return nullptr;
    }

    // Under the registry's lock: the workers' fields that have landed join the known ones.
    void TakeFinishedJobs(Registry& registry)
    {
        for (auto it = registry.jobs.begin(); it != registry.jobs.end();)
        {
            if (!it->second->done.load(std::memory_order_acquire)) { ++it; continue; }
            const std::string path = it->first.substr(0, it->first.find('|'));
            if (it->second->field)
            {
                LOG_INFO(logging::LogCategory::Ocean, "water mask: {} -- {}", path, it->second->note);
            }
            else
            {
                LOG_WARNING(logging::LogCategory::Ocean, "water mask: {} -- {}", path, it->second->note);
            }
            registry.fields[it->first] = it->second->field;
            it = registry.jobs.erase(it);
        }
    }

    // A triangle clipped to an axis-aligned cell in xz (Sutherland-Hodgman, y carried along): the highest y of what is left,
    // -FLT_MAX when nothing is. A vertical triangle's projection is a segment, and the clip keeps that segment's part in
    // the cell -- a wall with no top face still raises its cells' rim.
    float ClippedTop(const Math::float3& a, const Math::float3& b, const Math::float3& c, float x0, float x1, float z0, float z1)
    {
        // Plain storage: Math::float3 zeroes itself, and two buffers of it cost more than the clip (called per cell a triangle
        // touches -- hundreds of thousands of times a hull).
        struct Point { float x, y, z; };
        constexpr std::size_t kRoom = 12;
        Point buffers[2][kRoom];
        std::size_t count = 3;
        buffers[0][0] = { a.x, a.y, a.z };
        buffers[0][1] = { b.x, b.y, b.z };
        buffers[0][2] = { c.x, c.y, c.z };
        int current = 0;
        // Each edge of the cell: keep the side where `inside` holds, cut where it changes.
        const auto clip = [&](auto inside, auto cut)
        {
            const Point* in = buffers[current];
            Point* out = buffers[1 - current];
            std::size_t n = 0;
            for (std::size_t i = 0; i < count; ++i)
            {
                const Point& p = in[i];
                const Point& q = in[(i + 1) % count];
                const bool pIn = inside(p);
                const bool qIn = inside(q);
                if (pIn && n < kRoom) { out[n++] = p; }
                if (pIn != qIn && n < kRoom) { out[n++] = cut(p, q); }
            }
            count = n;
            current = 1 - current;
        };
        const auto atX = [](float x)
        {
            return [x](const Point& p, const Point& q)
            {
                const float t = (x - p.x) / (q.x - p.x);
                return Point{ x, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t };
            };
        };
        const auto atZ = [](float z)
        {
            return [z](const Point& p, const Point& q)
            {
                const float t = (z - p.z) / (q.z - p.z);
                return Point{ p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, z };
            };
        };
        clip([x0](const Point& p) { return p.x >= x0; }, atX(x0));
        if (count == 0) { return -FLT_MAX; }
        clip([x1](const Point& p) { return p.x <= x1; }, atX(x1));
        if (count == 0) { return -FLT_MAX; }
        clip([z0](const Point& p) { return p.z >= z0; }, atZ(z0));
        if (count == 0) { return -FLT_MAX; }
        clip([z1](const Point& p) { return p.z <= z1; }, atZ(z1));
        float top = -FLT_MAX;
        for (std::size_t i = 0; i < count; ++i) { top = std::max(top, buffers[current][i].y); }
        return top;
    }

    // ---- the lid (WaterMask.h) ----
    // The cap lies this far under its basin's level (the plane must cut the rim's walls, whose top in a cell is the level), a
    // curtain reaches this far under the sill it hangs over, and a depression under this area (m2: a cup holder, a drain)
    // gets no cap.
    constexpr float kCapBelowLevel = 0.005f;
    constexpr float kCurtainBelowSill = 0.03f;
    constexpr float kMinBasinArea = 0.02f;

    inline float Cross2(float ax, float az, float bx, float bz) { return ax * bz - az * bx; }
    static_assert(sizeof(Math::float3) == 3 * sizeof(float), "the lid's vertices go to meshoptimizer and the GPU as packed float3");

    // Every surface over each of a cell's four samples (BuildField's grid pass): y[first[s] .. first[s + 1]).
    struct SampleSurfaces
    {
        std::vector<std::uint32_t> first;
        std::vector<float> y;
    };

    // The hull seen from above on the lids' raster (kWaterMaskLidSub pixels a cell side): per pixel centre the LOWEST point of
    // the triangles that cover it (+big where none does). A cap's support -- the hull under its plane -- is every pixel under
    // the plane: one pass for all the basins, where each lid rasterized the whole mesh again.
    std::vector<float> SupportLowest(const WaterMaskField& f, const std::vector<Math::float3>& positions, const std::vector<std::uint32_t>& indices)
    {
        const int w = static_cast<int>(f.nx * kWaterMaskLidSub), h = static_cast<int>(f.nz * kWaterMaskLidSub);
        const float r = f.cell / static_cast<float>(kWaterMaskLidSub);
        const float invR = 1.0f / r;
        std::vector<float> lowest(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), FLT_MAX);
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3)
        {
            const Math::float3& a = positions[indices[t]];
            const Math::float3& b = positions[indices[t + 1]];
            const Math::float3& c = positions[indices[t + 2]];
            const float e1x = b.x - a.x, e1z = b.z - a.z, e2x = c.x - a.x, e2z = c.z - a.z;
            const float det = e1x * e2z - e2x * e1z;
            if (!(std::fabs(det) > 1e-12f)) { continue; } // a wall seen edge-on: the cut marks it
            const float inv = 1.0f / det;
            const float y = std::min({ a.y, b.y, c.y });
            const int i0 = std::max(0, static_cast<int>(std::floor((std::min({ a.x, b.x, c.x }) - f.originX) * invR - 0.5f)));
            const int i1 = std::min(w - 1, static_cast<int>(std::ceil((std::max({ a.x, b.x, c.x }) - f.originX) * invR - 0.5f)));
            const int k0 = std::max(0, static_cast<int>(std::floor((std::min({ a.z, b.z, c.z }) - f.originZ) * invR - 0.5f)));
            const int k1 = std::min(h - 1, static_cast<int>(std::ceil((std::max({ a.z, b.z, c.z }) - f.originZ) * invR - 0.5f)));
            for (int k = k0; k <= k1; ++k)
            {
                const float pz = f.originZ + (static_cast<float>(k) + 0.5f) * r - a.z;
                // w1 = (px e2z - e2x pz) / det, w2 = (e1x pz - px e1z) / det: linear in px along the row.
                const float w1Row = -e2x * pz * inv, w2Row = e1x * pz * inv;
                const float w1Step = e2z * inv, w2Step = -e1z * inv;
                float* row = &lowest[static_cast<std::size_t>(k) * static_cast<std::size_t>(w)];
                for (int i = i0; i <= i1; ++i)
                {
                    const float px = f.originX + (static_cast<float>(i) + 0.5f) * r - a.x;
                    const float w1 = w1Row + px * w1Step;
                    const float w2 = w2Row + px * w2Step;
                    constexpr float kEdge = -1e-4f; // shared edges overlap: no centre falls between two triangles
                    if (w1 < kEdge || w2 < kEdge || w1 + w2 > 1.0f - kEdge) { continue; }
                    row[i] = std::min(row[i], y);
                }
            }
        }
        return lowest;
    }

    // What one basin's lid adds to the field (WaterMask.h): the flood of the cap's plane from the basin through the gaps
    // the hull's cross-section leaves, the cap over what it reaches (marching squares on a raster kWaterMaskLidSub times
    // finer than the cells, its outline moved onto the hull's walls exactly), the curtains where that outline is no wall.
    class BasinLid
    {
    public:
        // `done`: the basins capped (or taken in) already -- a cell of one bounds this flood like an open low cell; a cell
        // of a basin still to come is part of it (its level is over this plane: it holds water here too).
        // `glass`: the see-through slots' triangles (WaterMask.h): a wall for the flood, and where they part the cap's space
        // from the outside below its plane, faces of the lid.
        // `supportLowest`: SupportLowest's raster; `samples`: every surface over the cells' samples (the floors).
        BasinLid(WaterMaskField& f, const std::vector<Math::float3>& positions, const std::vector<std::uint32_t>& indices,
                 const std::vector<Math::float3>& glassPositions, const std::vector<std::uint32_t>& glassIndices,
                 const std::vector<float>& supportLowest, const SampleSurfaces& samples,
                 const std::vector<float>& top, const std::vector<int>& basinOf, int basin,
                 const std::vector<std::uint32_t>& basinCells, float capY, const std::vector<std::uint8_t>& done)
            : f_(f), positions_(positions), indices_(indices), glassPositions_(glassPositions), glassIndices_(glassIndices),
              supportLowest_(supportLowest), samples_(samples),
              top_(top), basinOf_(basinOf), basin_(basin), basinCells_(basinCells), capY_(capY), done_(done)
        {
            nx_ = static_cast<int>(f.nx);
            nz_ = static_cast<int>(f.nz);
            sub_ = static_cast<int>(kWaterMaskLidSub);
            w_ = nx_ * sub_;
            h_ = nz_ * sub_;
            r_ = f.cell / static_cast<float>(sub_);
        }

        // Appends this basin's lid to the field. Returns false (and adds nothing) when the flood reaches nothing.
        bool Build(std::string& report)
        {
            using Clock = std::chrono::steady_clock;
            const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            const Clock::time_point t0 = Clock::now();
            CutPlane();
            const Clock::time_point t1 = Clock::now();
            Support();
            const Clock::time_point t2 = Clock::now();
            Flood(report);
            const Clock::time_point t3 = Clock::now();
            if (inside_ == 0u) { return false; }
            Floors();
            const Clock::time_point t4 = Clock::now();
            const bool built = Mesh(report);
            const Clock::time_point t5 = Clock::now();
            char times[192];
            std::snprintf(times, sizeof(times), "; ms: cut %.1f, support %.1f, flood %.1f, floors %.1f, mesh %.1f (simplify %.1f)",
                          ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4), ms(t4, t5), simplifyMs_);
            report += times;
            return built;
        }

        // The other basins the cap took in -- the plane runs through their water (over their floor, under their level), not
        // under them (a basin on a roof the plane passes beneath, in the cabin): they get no cap of their own.
        std::vector<int> Absorbed() const
        {
            std::vector<int> out;
            for (int k = minK_; k <= maxK_; ++k)
            {
                for (int i = minI_; i <= maxI_; ++i)
                {
                    if (!in_[Pixel(i, k)]) { continue; }
                    const std::size_t cell = CellOfPixel(i, k);
                    const int other = basinOf_[cell];
                    if (other < 0 || other == basin_ || !(top_[cell] < capY_)) { continue; }
                    if (std::find(out.begin(), out.end(), other) == out.end()) { out.push_back(other); }
                }
            }
            return out;
        }

    private:
        std::size_t CellOfPixel(int i, int k) const
        {
            return static_cast<std::size_t>(k / sub_) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(i / sub_);
        }
        std::size_t Pixel(int i, int k) const { return static_cast<std::size_t>(k) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(i); }
        float CenterX(int i) const { return f_.originX + (static_cast<float>(i) + 0.5f) * r_; }
        float CenterZ(int k) const { return f_.originZ + (static_cast<float>(k) + 0.5f) * r_; }
        bool Allowed(std::size_t cell) const
        {
            const int other = basinOf_[cell];
            return other == basin_ || (other >= 0 && !done_[static_cast<std::size_t>(other)]) || top_[cell] >= capY_;
        }
        // Where the plane may go, per pixel: the cells' smooth allowed area, and the hull UNDER it -- a cap never lies over the
        // sea, whatever stands above the plane there (a bow rail overhanging the water made the cells under it "covered").
        bool Open(std::size_t p) const { return allowed_[p] > 0.0f && support_[p] != 0u; }

        // The hull under the plane, per pixel: the centres some triangle with a part below the plane covers, seen from above
        // (SupportLowest: the lowest point over each centre).
        void Support()
        {
            const std::size_t pixels = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
            support_.resize(pixels);
            for (std::size_t p = 0; p < pixels; ++p) { support_[p] = supportLowest_[p] < capY_ ? 1u : 0u; }
        }

        // The mesh cut by the cap's plane: its segments, the pixels they pass through (walls) and, per cell, the segments
        // that pass it (for the outline's exact place).
        void CutPlane()
        {
            const std::size_t cells = static_cast<std::size_t>(nx_) * static_cast<std::size_t>(nz_);
            wall_.assign(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), 0u);
            cellSegments_.assign(cells, {});
            segments_.clear();
            for (std::size_t t = 0; t + 2 < indices_.size(); t += 3)
            {
                const std::uint32_t idx[3] = { indices_[t], indices_[t + 1], indices_[t + 2] };
                bool above[3];
                for (int v = 0; v < 3; ++v) { above[v] = positions_[idx[v]].y > capY_; }
                if (above[0] == above[1] && above[1] == above[2]) { continue; }
                std::array<float, 4> s{};
                int got = 0;
                for (int e = 0; e < 3 && got < 2; ++e)
                {
                    std::uint32_t u = idx[e], v = idx[(e + 1) % 3];
                    if (above[e] == above[(e + 1) % 3]) { continue; }
                    if (u > v) { std::swap(u, v); } // the same point for both triangles on the edge
                    const Math::float3& pu = positions_[u];
                    const Math::float3& pv = positions_[v];
                    const float k = (capY_ - pu.y) / (pv.y - pu.y);
                    s[got * 2 + 0] = pu.x + (pv.x - pu.x) * k;
                    s[got * 2 + 1] = pu.z + (pv.z - pu.z) * k;
                    ++got;
                }
                if (got != 2) { continue; }
                const std::uint32_t id = static_cast<std::uint32_t>(segments_.size());
                segments_.push_back(s);
                Walk(s, id);
            }
        }

        // Every pixel a segment passes through (Amanatides-Woo, 4-connected: no flood squeezes between two of them).
        void Walk(const std::array<float, 4>& s, std::uint32_t id)
        {
            const float px0 = (s[0] - f_.originX) / r_, pz0 = (s[1] - f_.originZ) / r_;
            const float px1 = (s[2] - f_.originX) / r_, pz1 = (s[3] - f_.originZ) / r_;
            int i = static_cast<int>(std::floor(px0)), k = static_cast<int>(std::floor(pz0));
            const int ie = static_cast<int>(std::floor(px1)), ke = static_cast<int>(std::floor(pz1));
            const float dx = px1 - px0, dz = pz1 - pz0;
            const int si = dx > 0.0f ? 1 : (dx < 0.0f ? -1 : 0);
            const int sk = dz > 0.0f ? 1 : (dz < 0.0f ? -1 : 0);
            float tMaxX = si > 0 ? (static_cast<float>(i + 1) - px0) / dx : (si < 0 ? (px0 - static_cast<float>(i)) / -dx : FLT_MAX);
            float tMaxZ = sk > 0 ? (static_cast<float>(k + 1) - pz0) / dz : (sk < 0 ? (pz0 - static_cast<float>(k)) / -dz : FLT_MAX);
            const float tDeltaX = si != 0 ? 1.0f / std::fabs(dx) : FLT_MAX;
            const float tDeltaZ = sk != 0 ? 1.0f / std::fabs(dz) : FLT_MAX;
            const int steps = std::abs(ie - i) + std::abs(ke - k);
            std::size_t lastCell = static_cast<std::size_t>(-1);
            for (int n = 0; n <= steps; ++n)
            {
                if (i >= 0 && k >= 0 && i < w_ && k < h_)
                {
                    wall_[Pixel(i, k)] = 1u;
                    const std::size_t cell = CellOfPixel(i, k);
                    if (cell != lastCell)
                    {
                        cellSegments_[cell].push_back(id);
                        lastCell = cell;
                    }
                }
                if (i == ie && k == ke) { break; }
                if (tMaxX < tMaxZ)
                {
                    i += si;
                    tMaxX += tDeltaX;
                }
                else
                {
                    k += sk;
                    tMaxZ += tDeltaZ;
                }
            }
        }

        // The plane flooded from the basin: through every pixel no wall passes, in the basin's cells and in any cell the mesh
        // reaches the plane in (a wall stands there, or a deck or a roof is over it) -- never into a low open cell (the swim
        // platform, the sea). What the flood found beyond a cell's reach of the basin (under a deck: a cabin through its
        // door) stays only where it is closed: a part of it that touches a low open cell without a wall between is outside
        // (a side deck overhanging the sea), and is dropped.
        void Flood(std::string& report)
        {
            const std::size_t cells = static_cast<std::size_t>(nx_) * static_cast<std::size_t>(nz_);
            const std::size_t pixels = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
            std::vector<std::uint8_t> ring(cells, 0u);
            // Where the plane may go, SMOOTH: +1 an allowed cell, -1 not, interpolated between the cells' centres, a pixel in
            // where it is over 0 -- the edge of the allowed area (an opening's line: a curtain hangs from it) runs straight
            // across the cells instead of up their staircase, so a ray does not cross a zigzag of curtains in and out again.
            {
                std::vector<float> value(cells);
                for (std::size_t c = 0; c < cells; ++c) { value[c] = Allowed(c) ? 1.0f : -1.0f; }
                allowed_.assign(pixels, 0.0f);
                for (int k = 0; k < h_; ++k)
                {
                    const float v = (static_cast<float>(k) + 0.5f) / static_cast<float>(sub_) - 0.5f;
                    const int k0 = std::clamp(static_cast<int>(std::floor(v)), 0, nz_ - 1);
                    const int k1 = std::min(k0 + 1, nz_ - 1);
                    const float fk = std::clamp(v - static_cast<float>(k0), 0.0f, 1.0f);
                    for (int i = 0; i < w_; ++i)
                    {
                        const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(sub_) - 0.5f;
                        const int i0 = std::clamp(static_cast<int>(std::floor(u)), 0, nx_ - 1);
                        const int i1 = std::min(i0 + 1, nx_ - 1);
                        const float fi = std::clamp(u - static_cast<float>(i0), 0.0f, 1.0f);
                        const auto at = [&](int ci, int ck) { return value[static_cast<std::size_t>(ck) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(ci)]; };
                        const float a = at(i0, k0) + (at(i1, k0) - at(i0, k0)) * fi;
                        const float b = at(i0, k1) + (at(i1, k1) - at(i0, k1)) * fi;
                        allowed_[Pixel(i, k)] = a + (b - a) * fk;
                    }
                }
            }
            for (const std::uint32_t c : basinCells_)
            {
                const int i = static_cast<int>(c % f_.nx), k = static_cast<int>(c / f_.nx);
                for (int dk = -1; dk <= 1; ++dk)
                {
                    for (int di = -1; di <= 1; ++di)
                    {
                        const int qi = i + di, qk = k + dk;
                        if (qi < 0 || qk < 0 || qi >= nx_ || qk >= nz_) { continue; }
                        ring[static_cast<std::size_t>(qk) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(qi)] = 1u;
                    }
                }
            }
            in_.assign(pixels, 0u);
            std::vector<std::uint32_t> queue;
            queue.reserve(pixels / 8u);
            for (const std::uint32_t c : basinCells_)
            {
                // Only where the plane runs in the basin's air, over its floor (a cap held under the sea's reach passes under
                // the higher parts of its basin -- a foredeck -- and floods from there whatever lies beneath them).
                if (!(top_[c] < capY_)) { continue; }
                const int i0 = static_cast<int>(c % f_.nx) * sub_, k0 = static_cast<int>(c / f_.nx) * sub_;
                for (int k = k0; k < k0 + sub_; ++k)
                {
                    for (int i = i0; i < i0 + sub_; ++i)
                    {
                        const std::size_t p = Pixel(i, k);
                        if (wall_[p] || in_[p] || !Open(p)) { continue; }
                        in_[p] = 1u;
                        queue.push_back(static_cast<std::uint32_t>(p));
                    }
                }
            }
            const auto neighbours = [this](std::uint32_t p, int out[4][2])
            {
                const int i = static_cast<int>(p % static_cast<std::uint32_t>(w_)), k = static_cast<int>(p / static_cast<std::uint32_t>(w_));
                out[0][0] = i - 1; out[0][1] = k;
                out[1][0] = i + 1; out[1][1] = k;
                out[2][0] = i;     out[2][1] = k - 1;
                out[3][0] = i;     out[3][1] = k + 1;
            };
            for (std::size_t head = 0; head < queue.size(); ++head)
            {
                int next[4][2];
                neighbours(queue[head], next);
                for (const auto& q : next)
                {
                    if (q[0] < 0 || q[1] < 0 || q[0] >= w_ || q[1] >= h_) { continue; }
                    const std::size_t qp = Pixel(q[0], q[1]);
                    if (in_[qp] || wall_[qp] || !Open(qp)) { continue; }
                    in_[qp] = 1u;
                    queue.push_back(static_cast<std::uint32_t>(qp));
                }
            }

            // The reach beyond the basin's own cells and their ring, part by part.
            std::vector<std::uint8_t> seen(pixels, 0u);
            std::vector<std::uint32_t> part;
            int kept = 0, dropped = 0;
            for (const std::uint32_t start : queue)
            {
                if (seen[start] || ring[CellOfPixel(static_cast<int>(start % static_cast<std::uint32_t>(w_)), static_cast<int>(start / static_cast<std::uint32_t>(w_)))]) { continue; }
                part.clear();
                part.push_back(start);
                seen[start] = 1u;
                bool open = false;
                for (std::size_t head = 0; head < part.size(); ++head)
                {
                    int next[4][2];
                    neighbours(part[head], next);
                    for (const auto& q : next)
                    {
                        if (q[0] < 0 || q[1] < 0 || q[0] >= w_ || q[1] >= h_) { open = true; continue; }
                        const std::size_t qp = Pixel(q[0], q[1]);
                        const std::size_t qc = CellOfPixel(q[0], q[1]);
                        if (!wall_[qp] && !Open(qp)) { open = true; }
                        if (seen[qp] || !in_[qp] || ring[qc]) { continue; }
                        seen[qp] = 1u;
                        part.push_back(static_cast<std::uint32_t>(qp));
                    }
                }
                if (open)
                {
                    for (const std::uint32_t p : part) { in_[p] = 0u; }
                    ++dropped;
                }
                else
                {
                    ++kept;
                }
            }
            inside_ = 0;
            minI_ = w_; minK_ = h_; maxI_ = -1; maxK_ = -1;
            for (int k = 0; k < h_; ++k)
            {
                for (int i = 0; i < w_; ++i)
                {
                    if (!in_[Pixel(i, k)]) { continue; }
                    ++inside_;
                    minI_ = std::min(minI_, i); maxI_ = std::max(maxI_, i);
                    minK_ = std::min(minK_, k); maxK_ = std::max(maxK_, k);
                }
            }
            report += std::to_string(segments_.size()) + " segments of the cut, " +
                      std::to_string(kept) + " enclosed spaces reached beyond it, " + std::to_string(dropped) + " open ones dropped, ";
        }

        // Under the cap, per cell: the highest surface below its plane (the floor, a sill), at the cell's four samples.
        void Floors()
        {
            const std::size_t cells = static_cast<std::size_t>(nx_) * static_cast<std::size_t>(nz_);
            std::vector<std::uint8_t> capCell(cells, 0u);
            for (int k = minK_; k <= maxK_; ++k)
            {
                for (int i = minI_; i <= maxI_; ++i)
                {
                    if (in_[Pixel(i, k)]) { capCell[CellOfPixel(i, k)] = 1u; }
                }
            }
            floor_.assign(cells, -FLT_MAX);
            // The cell's four samples (the grid pass's: s = 2 sk + si), every surface over them, the highest under the plane.
            for (std::size_t cell = 0; cell < cells; ++cell)
            {
                if (!capCell[cell]) { continue; }
                for (std::size_t s = cell * 4u; s < cell * 4u + 4u; ++s)
                {
                    for (std::uint32_t at = samples_.first[s]; at < samples_.first[s + 1u]; ++at)
                    {
                        const float y = samples_.y[at];
                        if (y < capY_) { floor_[cell] = std::max(floor_[cell], y); }
                    }
                }
            }
        }

        // The outline's point on the grid edge from inside pixel A to outside pixel B: on the hull's wall where the cut's
        // segments cross the edge (the first from A), B's centre when a segment passes B's pixel but not the edge, the edge's
        // middle when B is no wall (an opening: a curtain hangs there).
        std::uint32_t Crossing(int ia, int ka, int ib, int kb)
        {
            const bool alongX = ka == kb;
            std::vector<std::int32_t>& map = alongX ? hVert_ : vVert_;
            const std::size_t key = alongX ? Pixel(std::min(ia, ib), ka) : Pixel(ia, std::min(ka, kb));
            if (map[key] >= 0) { return static_cast<std::uint32_t>(map[key]); }
            const float ax = CenterX(ia), az = CenterZ(ka), bx = CenterX(ib), bz = CenterZ(kb);
            const bool isWall = wall_[Pixel(ib, kb)] != 0u;
            float t = 0.5f;
            if (isWall)
            {
                t = 1.0f;
                const float dx = bx - ax, dz = bz - az;
                for (const std::uint32_t id : cellSegments_[CellOfPixel(ib, kb)])
                {
                    const std::array<float, 4>& s = segments_[id];
                    const float ex = s[2] - s[0], ez = s[3] - s[1];
                    const float denom = Cross2(dx, dz, ex, ez);
                    if (std::fabs(denom) < 1e-14f) { continue; }
                    const float cx = s[0] - ax, cz = s[1] - az;
                    const float tt = Cross2(cx, cz, ex, ez) / denom;
                    const float uu = Cross2(cx, cz, dx, dz) / denom;
                    if (tt < 0.0f || tt > 1.0f || uu < -1e-4f || uu > 1.0f + 1e-4f) { continue; }
                    t = std::min(t, tt);
                }
                t = std::max(t, 0.5f);
            }
            else if (!support_[Pixel(ib, kb)])
            {
                // The edge of the hull under the plane (a deck's edge, open air beyond): the cap stops just inside it, never
                // over the water.
                t = 0.1f;
            }
            else
            {
                // An opening: where the smooth allowed area ends between the two centres (WaterMask's Flood).
                const float fa = allowed_[Pixel(ia, ka)], fb = allowed_[Pixel(ib, kb)];
                if (fa > 0.0f && !(fb > 0.0f)) { t = std::clamp(fa / (fa - fb), 0.0f, 1.0f); }
            }
            const std::uint32_t index = static_cast<std::uint32_t>(vertices_.size());
            vertices_.emplace_back(ax + (bx - ax) * t, capY_, az + (bz - az) * t);
            wallVertex_.push_back(isWall ? 1u : 0u);
            sillCell_.push_back(static_cast<std::uint32_t>(CellOfPixel(ia, ka)));
            // Beyond an opening: a cell all under the plane (a step, a platform, a gunwale's top) blocks the way below its own
            // highest surface -- the curtain need not reach lower. -big: the sea, or a space the flood left (the inside sill).
            const float beyond = top_[CellOfPixel(ib, kb)];
            beyondTop_.push_back(beyond < capY_ && support_[Pixel(ib, kb)] ? beyond : -FLT_MAX);
            map[key] = static_cast<std::int32_t>(index);
            return index;
        }

        std::uint32_t Corner(int i, int k)
        {
            std::int32_t& slot = cornerVert_[Pixel(i, k)];
            if (slot >= 0) { return static_cast<std::uint32_t>(slot); }
            slot = static_cast<std::int32_t>(vertices_.size());
            vertices_.emplace_back(CenterX(i), capY_, CenterZ(k));
            wallVertex_.push_back(1u); // an inner vertex: never under a curtain
            sillCell_.push_back(0u);
            beyondTop_.push_back(-FLT_MAX);
            return static_cast<std::uint32_t>(slot);
        }

        // The cap: marching squares on the pixels' centres (an inside corner, a point on an edge between an inside and an
        // outside centre), 4-connected (the two diagonal cases are two corners, never a bridge), simplified inside its locked
        // outline; then the curtains under its outline where it is no wall.
        bool Mesh(std::string& report)
        {
            const std::size_t pixels = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
            cornerVert_.assign(pixels, -1);
            hVert_.assign(pixels, -1);
            vVert_.assign(pixels, -1);
            vertices_.clear();
            wallVertex_.clear();
            sillCell_.clear();
            beyondTop_.clear();
            std::vector<std::uint32_t> triangles;
            const auto in = [this](int i, int k) { return i >= 0 && k >= 0 && i < w_ && k < h_ && in_[Pixel(i, k)] != 0u; };
            for (int k = std::max(minK_ - 1, 0); k <= std::min(maxK_, h_ - 2); ++k)
            {
                for (int i = std::max(minI_ - 1, 0); i <= std::min(maxI_, w_ - 2); ++i)
                {
                    const int ci[4] = { i, i + 1, i + 1, i };
                    const int ck[4] = { k, k, k + 1, k + 1 };
                    bool inside[4];
                    int mask = 0;
                    for (int c = 0; c < 4; ++c)
                    {
                        inside[c] = in(ci[c], ck[c]);
                        mask |= inside[c] ? (1 << c) : 0;
                    }
                    if (mask == 0) { continue; }
                    std::uint32_t poly[8];
                    int n = 0;
                    for (int c = 0; c < 4; ++c)
                    {
                        const int cn = (c + 1) & 3;
                        if (inside[c]) { poly[n++] = Corner(ci[c], ck[c]); }
                        if (inside[c] != inside[cn])
                        {
                            poly[n++] = inside[c] ? Crossing(ci[c], ck[c], ci[cn], ck[cn]) : Crossing(ci[cn], ck[cn], ci[c], ck[c]);
                        }
                    }
                    const auto tri = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c)
                    {
                        triangles.push_back(a);
                        triangles.push_back(b);
                        triangles.push_back(c);
                    };
                    if (mask == 5) // corners 0 and 2: two triangles, no bridge
                    {
                        tri(poly[0], poly[1], poly[5]);
                        tri(poly[3], poly[4], poly[2]);
                        continue;
                    }
                    if (mask == 10) // corners 1 and 3
                    {
                        tri(poly[0], poly[1], poly[2]);
                        tri(poly[3], poly[4], poly[5]);
                        continue;
                    }
                    for (int m = 1; m + 1 < n; ++m) { tri(poly[0], poly[m], poly[m + 1]); }
                }
            }
            if (triangles.empty()) { return false; }

            // Inside its outline the cap is a plane: every inner vertex collapses (meshoptimizer refuses a collapse that flips
            // a triangle, so no two overlap -- the surface counts every crossing).
            std::vector<std::uint32_t> simplified(triangles.size());
            const std::size_t rawTriangles = triangles.size() / 3u;
            const auto simplifyStart = std::chrono::steady_clock::now();
            simplified.resize(meshopt_simplify(simplified.data(), triangles.data(), triangles.size(), &vertices_[0].x,
                                               vertices_.size(), sizeof(Math::float3), 0, 1e-3f, meshopt_SimplifyLockBorder, nullptr));
            simplifyMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - simplifyStart).count();
            if (simplified.empty()) { simplified = triangles; }

            // The outline's edges: those of one triangle.
            std::unordered_map<std::uint64_t, std::uint8_t> edges;
            edges.reserve(simplified.size() * 2u);
            const auto key = [](std::uint32_t a, std::uint32_t b) { return (static_cast<std::uint64_t>(a) << 32) | b; };
            for (std::size_t t = 0; t + 2 < simplified.size(); t += 3)
            {
                for (int e = 0; e < 3; ++e) { edges[key(simplified[t + e], simplified[t + (e + 1) % 3])] = 1u; }
            }
            std::vector<std::uint32_t> curtains;
            std::unordered_map<std::uint32_t, std::uint32_t> bottomOf;
            const auto bottom = [&](std::uint32_t v)
            {
                const auto found = bottomOf.find(v);
                if (found != bottomOf.end()) { return found->second; }
                // Down to the higher of what is under it on either side: the passage below is shut by that.
                const float sill = std::max(floor_[sillCell_[v]], beyondTop_[v]);
                const float y = std::min(sill > -FLT_MAX ? sill - kCurtainBelowSill : capY_ - 0.05f, capY_ - 0.01f);
                const std::uint32_t index = static_cast<std::uint32_t>(vertices_.size());
                vertices_.emplace_back(vertices_[v].x, y, vertices_[v].z);
                wallVertex_.push_back(1u);
                sillCell_.push_back(0u);
                beyondTop_.push_back(-FLT_MAX);
                bottomOf.emplace(v, index);
                return index;
            };
            for (std::size_t t = 0; t + 2 < simplified.size(); t += 3)
            {
                for (int e = 0; e < 3; ++e)
                {
                    const std::uint32_t a = simplified[t + e], b = simplified[t + (e + 1) % 3];
                    if (edges.count(key(b, a)) != 0u) { continue; }
                    if (wallVertex_[a] && wallVertex_[b]) { continue; }
                    const std::uint32_t ab = bottom(a), bb = bottom(b);
                    curtains.insert(curtains.end(), { a, bb, b, a, ab, bb });
                }
            }
            const std::uint32_t glassFaces = GlassFaces(curtains);

            // Into the field: only the vertices the lid uses.
            std::vector<std::uint32_t> lid = simplified;
            lid.insert(lid.end(), curtains.begin(), curtains.end());
            std::vector<std::uint32_t> remap(vertices_.size(), ~0u);
            const std::uint32_t base = static_cast<std::uint32_t>(f_.lidVertices.size());
            for (std::uint32_t& v : lid)
            {
                if (remap[v] == ~0u)
                {
                    remap[v] = static_cast<std::uint32_t>(f_.lidVertices.size());
                    f_.lidVertices.push_back(vertices_[v]);
                }
                v = remap[v];
            }
            WaterMaskBasin basin;
            basin.capY = capY_;
            basin.firstIndex = static_cast<std::uint32_t>(f_.lidIndices.size());
            basin.capIndexCount = static_cast<std::uint32_t>(simplified.size());
            basin.minX = f_.originX + static_cast<float>(minI_) * r_;
            basin.maxX = f_.originX + static_cast<float>(maxI_ + 1) * r_;
            basin.minZ = f_.originZ + static_cast<float>(minK_) * r_;
            basin.maxZ = f_.originZ + static_cast<float>(maxK_ + 1) * r_;
            basin.floor = std::move(floor_);
            f_.lidIndices.insert(f_.lidIndices.end(), lid.begin(), lid.end());
            f_.basins.push_back(std::move(basin));
            report += std::to_string(inside_) + " pixels of " + std::to_string(static_cast<int>(std::lround(r_ * 1000.0f))) +
                      " mm under the cap, " + std::to_string(rawTriangles) + " -> " + std::to_string(simplified.size() / 3u) +
                      " cap triangles, " + std::to_string((curtains.size() - glassFaces * 3u) / 6u) + " curtain quads, " +
                      std::to_string(glassFaces) + " glass faces, " + std::to_string(f_.lidVertices.size() - base) + " vertices";
            return true;
        }

        // The glass that parts the cap's space from the outside below its plane (a windshield before a helm the cap does not
        // reach, a window into a cabin under it): one sees through it, so a view ray passes it as it passes a curtain and must
        // be counted -- its part under the plane is a face of the lid. Glass with the cap's space on both sides (a partition)
        // or on neither is not. Which side is which: the pixels at the plane a few steps off the pane along its normal, one
        // side under the cap, the other open (neither under it nor a wall). Returns the triangles appended to `out`.
        std::uint32_t GlassFaces(std::vector<std::uint32_t>& out)
        {
            std::uint32_t added = 0;
            // A pane modelled two-sided twice (a triangle and its reversed twin at the same place) is one face: two would be
            // crossed together and count as none.
            std::unordered_map<std::uint64_t, std::uint8_t> seenPanes;
            const auto paneKey = [](const Math::float3* tri)
            {
                std::array<std::uint64_t, 3> q{};
                for (int v = 0; v < 3; ++v)
                {
                    const auto c = [](float x) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(x * 10000.0f)) & 0x1FFFFF); };
                    q[v] = (c(tri[v].x) << 42) | (c(tri[v].y) << 21) | c(tri[v].z);
                }
                std::sort(q.begin(), q.end());
                return q[0] * 0x9E3779B97F4A7C15ull ^ (q[1] + 0x632BE59BD9B4E019ull) * 0xBF58476D1CE4E5B9ull ^ q[2];
            };
            const auto inCap = [this](float x, float z, int& state)
            {
                const int i = static_cast<int>(std::floor((x - f_.originX) / r_));
                const int k = static_cast<int>(std::floor((z - f_.originZ) / r_));
                if (i < 0 || k < 0 || i >= w_ || k >= h_) { state = 2; return; }
                const std::size_t p = Pixel(i, k);
                state = in_[p] ? 1 : (wall_[p] ? 0 : 2); // 1 under the cap, 0 a wall, 2 open
            };
            for (std::size_t t = 0; t + 2 < glassIndices_.size(); t += 3)
            {
                const Math::float3 tri[3] = { glassPositions_[glassIndices_[t]], glassPositions_[glassIndices_[t + 1]],
                                              glassPositions_[glassIndices_[t + 2]] };
                if (std::min({ tri[0].y, tri[1].y, tri[2].y }) >= capY_) { continue; }
                if (!seenPanes.emplace(paneKey(tri), 1u).second) { continue; }
                // The part under the plane (one clip of the triangle).
                std::array<Math::float3, 4> poly{};
                int n = 0;
                for (int e = 0; e < 3; ++e)
                {
                    const Math::float3& p = tri[e];
                    const Math::float3& q = tri[(e + 1) % 3];
                    const bool pIn = p.y <= capY_, qIn = q.y <= capY_;
                    if (pIn && n < 4) { poly[n++] = p; }
                    if (pIn != qIn && n < 4)
                    {
                        const float k = (capY_ - p.y) / (q.y - p.y);
                        poly[n++] = Math::float3(p.x + (q.x - p.x) * k, capY_, p.z + (q.z - p.z) * k);
                    }
                }
                if (n < 3) { continue; }
                const float ux = tri[1].x - tri[0].x, uy = tri[1].y - tri[0].y, uz = tri[1].z - tri[0].z;
                const float vx = tri[2].x - tri[0].x, vy = tri[2].y - tri[0].y, vz = tri[2].z - tri[0].z;
                const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                const float horizontal = std::sqrt(nx * nx + nz * nz);
                const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (!(horizontal > 0.3f * length)) { continue; } // a pane seen from above: the cap's own plane takes its rays
                const float dx = nx / horizontal, dz = nz / horizontal;
                float cx = 0.0f, cz = 0.0f;
                for (int v = 0; v < n; ++v) { cx += poly[v].x / static_cast<float>(n); cz += poly[v].z / static_cast<float>(n); }
                // Each side: the first pixel off the pane that is no wall (a frame, an arch's leg beside the glass are walls),
                // within kGlassReach; none = unknown.
                constexpr int kGlassReach = 12;
                const auto side = [&](float sx, float sz, float sign, float& reach)
                {
                    for (int step = 0; step < kGlassReach; ++step)
                    {
                        reach = 1.5f + static_cast<float>(step);
                        const float d = reach * r_ * sign;
                        int state = 0;
                        inCap(sx + dx * d, sz + dz * d, state);
                        if (state != 0) { return state; }
                    }
                    return 0;
                };
                bool boundary = false;
                for (int s = 0; s <= n && !boundary; ++s)
                {
                    const float sx = s == n ? cx : poly[s].x + (cx - poly[s].x) * 0.1f;
                    const float sz = s == n ? cz : poly[s].z + (cz - poly[s].z) * 0.1f;
                    float plusReach = 0.0f, minusReach = 0.0f;
                    const int plus = side(sx, sz, 1.0f, plusReach), minus = side(sx, sz, -1.0f, minusReach);
                    // Under the cap on one side and not on the other (open, or walls as far as it looked)...
                    if (!((plus == 1 && minus != 1) || (minus == 1 && plus != 1))) { continue; }
                    // ...across a WALL at the plane: there the hull closes the cap's space, and glass in it lets rays through.
                    // Where the two meet with no wall between, a curtain closes it already, and the pane beneath it (a
                    // windshield's wing under a deck's edge) would be crossed with it: two faces, counted as none.
                    bool wallBetween = false;
                    for (float t = -minusReach + 0.5f; t <= plusReach - 0.5f && !wallBetween; t += 0.5f)
                    {
                        int state = 0;
                        inCap(sx + dx * t * r_, sz + dz * t * r_, state);
                        wallBetween = state == 0;
                    }
                    boundary = wallBetween;
                }
                if (!boundary) { continue; }
                const std::uint32_t first = static_cast<std::uint32_t>(vertices_.size());
                for (int v = 0; v < n; ++v)
                {
                    vertices_.push_back(poly[v]);
                    wallVertex_.push_back(1u);
                    sillCell_.push_back(0u);
                    beyondTop_.push_back(-FLT_MAX);
                }
                for (int v = 1; v + 1 < n; ++v)
                {
                    out.insert(out.end(), { first, first + static_cast<std::uint32_t>(v), first + static_cast<std::uint32_t>(v + 1) });
                    ++added;
                }
            }
            return added;
        }

        WaterMaskField& f_;
        const std::vector<Math::float3>& positions_;
        const std::vector<std::uint32_t>& indices_;
        const std::vector<Math::float3>& glassPositions_;
        const std::vector<std::uint32_t>& glassIndices_;
        const std::vector<float>& supportLowest_;
        const SampleSurfaces& samples_;
        const std::vector<float>& top_;
        const std::vector<int>& basinOf_;
        int basin_;
        const std::vector<std::uint32_t>& basinCells_;
        float capY_;
        const std::vector<std::uint8_t>& done_;
        std::vector<float> allowed_; // per pixel, the smooth allowed area (> 0 in)
        std::vector<std::uint8_t> support_; // per pixel, the hull under the plane
        int nx_ = 0, nz_ = 0, sub_ = 1, w_ = 0, h_ = 0;
        float r_ = 1.0f;
        std::vector<std::uint8_t> wall_, in_;
        std::vector<std::array<float, 4>> segments_;
        std::vector<std::vector<std::uint32_t>> cellSegments_;
        std::size_t inside_ = 0;
        int minI_ = 0, minK_ = 0, maxI_ = -1, maxK_ = -1;
        std::vector<float> floor_;
        std::vector<std::int32_t> cornerVert_, hVert_, vVert_;
        std::vector<Math::float3> vertices_;
        std::vector<std::uint8_t> wallVertex_;
        std::vector<std::uint32_t> sillCell_;
        std::vector<float> beyondTop_;
        double simplifyMs_ = 0.0;
    };
}

namespace watermask
{
    std::shared_ptr<const WaterMaskField> BuildField(const std::string& geometryPath, const std::vector<int>& seeThroughSlots,
                                                     float waterline, float draftOverLowest)
    {
        const auto started = std::chrono::steady_clock::now();
        std::vector<Math::float3> positions;
        std::vector<std::uint32_t> indices;
        std::vector<Math::float3> glassPositions;
        std::vector<std::uint32_t> glassIndices;
        if (!physics::ReadSolidAndSlotTriangles(geometryPath, seeThroughSlots, positions, indices, glassPositions, glassIndices) ||
            indices.size() < 3u)
        {
            return nullptr;
        }
        const auto sinceStart = [&started]() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(); };
        const double readMs = sinceStart();
        Math::float3 lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const Math::float3& p : positions)
        {
            lo = Math::float3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = Math::float3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        }
        const float cell = std::max(hi.x - lo.x, hi.z - lo.z) / static_cast<float>(kWaterMaskCellsAlong);
        if (!(cell > 1e-5f)) { return nullptr; }
        // Openings up to kWaterMaskGap wide are closed before the flood: a cockpit's walkthrough to the swim platform
        // otherwise drains it to its sill, and it would hold no water at all.
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
        if (cells > kWaterMaskMaxCells) { return nullptr; }

        // Per cell its rim (the highest surface, clipped exactly); per quarter of a cell the lowest surface over its centre
        // (a vertical line through it).
        constexpr int kSamples = 2; // per side of a cell
        std::vector<float> top(cells, -FLT_MAX);
        std::vector<float> low(cells * kSamples * kSamples, FLT_MAX);
        // Every surface over each sample (a vertical line through it), not only the lowest: the lids' floors are the highest
        // of them under a cap's plane (BasinLid::Floors), and that plane is a basin's -- one pass here serves them all.
        std::vector<std::pair<std::uint32_t, float>> sampleHits;
        sampleHits.reserve(indices.size() / 3u);
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
            // A triangle inside one cell is its own clip: its top is its highest vertex (most of a dense mesh's triangles).
            const bool oneCell = i0 == i1 && k0 == k1;
            const float highest = std::max({ a.y, b.y, c.y });
            // The triangle's plane over xz, for the samples: y = a.y + w1 (b - a).y + w2 (c - a).y.
            const float e1x = b.x - a.x, e1z = b.z - a.z, e2x = c.x - a.x, e2z = c.z - a.z;
            const float det = e1x * e2z - e2x * e1z;
            const bool flat = std::fabs(det) > 1e-12f;
            const float inv = flat ? 1.0f / det : 0.0f;
            for (int k = k0; k <= k1; ++k)
            {
                const float z0 = f.originZ + static_cast<float>(k) * f.cell;
                for (int i = i0; i <= i1; ++i)
                {
                    const float x0 = f.originX + static_cast<float>(i) * f.cell;
                    const std::size_t index = static_cast<std::size_t>(k) * f.nx + static_cast<std::size_t>(i);
                    const float clipped = oneCell ? highest : ClippedTop(a, b, c, x0, x0 + f.cell, z0, z0 + f.cell);
                    if (clipped == -FLT_MAX) { continue; }
                    top[index] = std::max(top[index], clipped);
                    if (!flat) { continue; }
                    for (int sk = 0; sk < kSamples; ++sk)
                    {
                        for (int si = 0; si < kSamples; ++si)
                        {
                            const float px = x0 + (static_cast<float>(si) + 0.5f) * f.cell / kSamples - a.x;
                            const float pz = z0 + (static_cast<float>(sk) + 0.5f) * f.cell / kSamples - a.z;
                            const float w1 = (px * e2z - e2x * pz) * inv;
                            const float w2 = (e1x * pz - px * e1z) * inv;
                            constexpr float kEdge = -1e-5f; // a shared edge belongs to both: no sample falls between
                            if (w1 < kEdge || w2 < kEdge || w1 + w2 > 1.0f - kEdge) { continue; }
                            const float y = a.y + w1 * (b.y - a.y) + w2 * (c.y - a.y);
                            const std::size_t s = index * kSamples * kSamples + static_cast<std::size_t>(sk) * kSamples + si;
                            low[s] = std::min(low[s], y);
                            sampleHits.emplace_back(static_cast<std::uint32_t>(s), y);
                        }
                    }
                }
            }
        }
        // The hits by sample (a counting sort): sampleFirst[s] .. sampleFirst[s + 1] in sampleY.
        const double rasterMs = sinceStart() - readMs;
        const std::size_t hitCount = sampleHits.size();
        SampleSurfaces samples;
        {
            const std::size_t sampleCount = cells * kSamples * kSamples;
            samples.first.assign(sampleCount + 1u, 0u);
            for (const auto& hit : sampleHits) { ++samples.first[hit.first + 1u]; }
            for (std::size_t s = 0; s < sampleCount; ++s) { samples.first[s + 1u] += samples.first[s]; }
            samples.y.resize(sampleHits.size());
            std::vector<std::uint32_t> at(samples.first.begin(), samples.first.end() - 1);
            for (const auto& hit : sampleHits) { samples.y[at[hit.first]++] = hit.second; }
            std::vector<std::pair<std::uint32_t, float>>().swap(sampleHits);
        }

        const double gridMs = sinceStart() - readMs;
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

        // A depression: a cell the mesh covers at all four samples whose level stands over its own rim.
        std::vector<std::uint8_t> depression(cells, 0u);
        for (std::size_t index = 0; index < cells; ++index)
        {
            bool covered = true;
            for (int s = 0; s < kSamples * kSamples; ++s) { covered = covered && low[index * kSamples * kSamples + s] != FLT_MAX; }
            depression[index] = covered && level[index] > top[index] + 0.01f ? 1u : 0u;
        }

        // The plan, for the particles (and ocean.waterMask:3): the lowest surface over every cell the mesh touches at all. A
        // thin part no sample meets (a rail, an aerial) counts from its top.
        f.columns.assign(cells, Math::float4(FLT_MAX, -FLT_MAX, -FLT_MAX, FLT_MAX));
        std::size_t plan = 0;
        for (std::size_t index = 0; index < cells; ++index)
        {
            if (top[index] == -FLT_MAX) { continue; }
            float lowest = FLT_MAX;
            for (int s = 0; s < kSamples * kSamples; ++s) { lowest = std::min(lowest, low[index * kSamples * kSamples + s]); }
            f.columns[index].w = lowest != FLT_MAX ? lowest : top[index];
            ++plan;
        }

        // ---- the lid ----
        // The basins: the depressions' 4-connected parts. A basin's level is its spill height, the lowest of its cells' (a
        // closed gap's cells stand higher, at the closing).
        std::vector<int> basinOf(cells, -1);
        std::vector<std::vector<std::uint32_t>> basins;
        for (std::size_t start = 0; start < cells; ++start)
        {
            if (!depression[start] || basinOf[start] >= 0) { continue; }
            const int id = static_cast<int>(basins.size());
            basins.emplace_back();
            std::vector<std::uint32_t>& list = basins.back();
            list.push_back(static_cast<std::uint32_t>(start));
            basinOf[start] = id;
            for (std::size_t head = 0; head < list.size(); ++head)
            {
                const std::uint32_t at = list[head];
                const int i = static_cast<int>(at % f.nx), k = static_cast<int>(at / f.nx);
                const int ni[4] = { i - 1, i + 1, i, i };
                const int nk[4] = { k, k, k - 1, k + 1 };
                for (int s = 0; s < 4; ++s)
                {
                    if (ni[s] < 0 || nk[s] < 0 || ni[s] >= static_cast<int>(f.nx) || nk[s] >= static_cast<int>(f.nz)) { continue; }
                    const std::size_t n = static_cast<std::size_t>(nk[s]) * f.nx + static_cast<std::size_t>(ni[s]);
                    if (!depression[n] || basinOf[n] >= 0) { continue; }
                    basinOf[n] = id;
                    list.push_back(static_cast<std::uint32_t>(n));
                }
            }
        }
        std::vector<float> spills(basins.size(), FLT_MAX);
        for (std::size_t b = 0; b < basins.size(); ++b)
        {
            for (const std::uint32_t c : basins[b]) { spills[b] = std::min(spills[b], level[c]); }
        }
        // Neighbours at nearly one level are one basin: a ladder's stringer or a rail across a deck parts the columns' flood
        // into pockets a few centimetres apart in level, and two caps side by side meet in a seam -- a curtain from the
        // higher one down to the deck between them, which the count crosses passing from one to the other. Within
        // kMergeCells of each other and kMergeLevel apart in level: one basin, at the lower level.
        {
            constexpr int kMergeCells = 5;
            constexpr float kMergeLevel = 0.15f;
            std::vector<int> parent(basins.size());
            for (std::size_t b = 0; b < basins.size(); ++b) { parent[b] = static_cast<int>(b); }
            const auto root = [&parent](int b)
            {
                while (parent[static_cast<std::size_t>(b)] != b) { b = parent[static_cast<std::size_t>(b)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(b)])]; }
                return b;
            };
            for (std::size_t b = 0; b < basins.size(); ++b)
            {
                for (const std::uint32_t c : basins[b])
                {
                    const int i = static_cast<int>(c % f.nx), k = static_cast<int>(c / f.nx);
                    for (int dk = -kMergeCells; dk <= kMergeCells; ++dk)
                    {
                        for (int di = -kMergeCells; di <= kMergeCells; ++di)
                        {
                            const int qi = i + di, qk = k + dk;
                            if (qi < 0 || qk < 0 || qi >= static_cast<int>(f.nx) || qk >= static_cast<int>(f.nz)) { continue; }
                            const int other = basinOf[static_cast<std::size_t>(qk) * f.nx + static_cast<std::size_t>(qi)];
                            if (other < 0 || other == static_cast<int>(b)) { continue; }
                            if (std::fabs(spills[b] - spills[static_cast<std::size_t>(other)]) > kMergeLevel) { continue; }
                            const int ra = root(static_cast<int>(b)), rb = root(other);
                            if (ra != rb) { parent[static_cast<std::size_t>(std::max(ra, rb))] = std::min(ra, rb); }
                        }
                    }
                }
            }
            std::vector<std::vector<std::uint32_t>> merged;
            std::vector<float> mergedSpills;
            std::vector<int> slot(basins.size(), -1);
            for (std::size_t b = 0; b < basins.size(); ++b)
            {
                const int r = root(static_cast<int>(b));
                if (slot[static_cast<std::size_t>(r)] < 0)
                {
                    slot[static_cast<std::size_t>(r)] = static_cast<int>(merged.size());
                    merged.emplace_back();
                    mergedSpills.push_back(FLT_MAX);
                }
                const int m = slot[static_cast<std::size_t>(r)];
                merged[static_cast<std::size_t>(m)].insert(merged[static_cast<std::size_t>(m)].end(), basins[b].begin(), basins[b].end());
                mergedSpills[static_cast<std::size_t>(m)] = std::min(mergedSpills[static_cast<std::size_t>(m)], spills[b]);
            }
            for (std::size_t m = 0; m < merged.size(); ++m)
            {
                for (const std::uint32_t c : merged[m]) { basinOf[c] = static_cast<int>(m); }
            }
            basins = std::move(merged);
            spills = std::move(mergedSpills);
        }
        for (std::size_t b = 0; b < basins.size(); ++b)
        {
            float x0 = FLT_MAX, x1 = -FLT_MAX, z0 = FLT_MAX, z1 = -FLT_MAX;
            for (const std::uint32_t c : basins[b])
            {
                const float x = f.originX + (static_cast<float>(c % f.nx) + 0.5f) * cell;
                const float z = f.originZ + (static_cast<float>(c / f.nx) + 0.5f) * cell;
                x0 = std::min(x0, x); x1 = std::max(x1, x); z0 = std::min(z0, z); z1 = std::max(z1, z);
            }
            LOG_DEBUG(logging::LogCategory::Ocean, "water mask: {} -- basin {}: {} cells, level {:.3f} m, x {:.2f}..{:.2f}, z {:.2f}..{:.2f}",
                      geometryPath, b, basins[b].size(), spills[b], x0, x1, z0, z1);
        }
        // Lowest first: a basin whose flood reaches into another's water (a higher one -- it holds water at this plane too,
        // behind no wall) takes it under its own cap, and that one gets none.
        std::vector<int> order(basins.size());
        for (std::size_t b = 0; b < basins.size(); ++b) { order[b] = static_cast<int>(b); }
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return spills[static_cast<std::size_t>(a)] < spills[static_cast<std::size_t>(b)]; });
        std::vector<std::uint8_t> done(basins.size(), 0u);
        const double basinsMs = sinceStart() - readMs - gridMs;
        // The sea's reach (WaterMask.h kWaterMaskReach): no cap over it.
        if (!(waterline < 1.0e37f) && draftOverLowest > -1.0e37f)
        {
            waterline = lo.y + (draftOverLowest >= 0.0f ? draftOverLowest : 0.25f * (hi.y - lo.y));
        }
        const float ceiling = waterline < 1.0e37f ? waterline + kWaterMaskReach : FLT_MAX;
        int capped = 0, absorbed = 0, unreached = 0;
        std::vector<float> supportLowest; // SupportLowest, made before the first lid
        double supportMs = 0.0;
        for (const int b : order)
        {
            if (done[static_cast<std::size_t>(b)]) { continue; }
            done[static_cast<std::size_t>(b)] = 1u;
            const std::vector<std::uint32_t>& list = basins[static_cast<std::size_t>(b)];
            if (static_cast<float>(list.size()) * cell * cell < kMinBasinArea) { continue; }
            const float spill = spills[static_cast<std::size_t>(b)];
            const float capY = std::min(spill - kCapBelowLevel, ceiling);
            // Its lowest floor: over the cap's plane, the sea never stands in it.
            float lowest = FLT_MAX;
            for (const std::uint32_t c : list) { lowest = std::min(lowest, top[c]); }
            if (!(lowest < capY - 0.01f))
            {
                ++unreached;
                LOG_DEBUG(logging::LogCategory::Ocean, "water mask: {} -- basin {} (level {:.3f} m, floor {:.3f} m): over the sea's reach ({:.3f} m), no cap",
                          geometryPath, b, spill, lowest, ceiling);
                continue;
            }
            if (supportLowest.empty())
            {
                const auto supportStart = std::chrono::steady_clock::now();
                supportLowest = SupportLowest(f, positions, indices);
                supportMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - supportStart).count();
            }
            std::string report;
            BasinLid lid(f, positions, indices, glassPositions, glassIndices, supportLowest, samples, top, basinOf, b, list, capY, done);
            if (lid.Build(report))
            {
                ++capped;
                const std::vector<int> taken = lid.Absorbed();
                for (const int other : taken) { done[static_cast<std::size_t>(other)] = 1u; }
                absorbed += static_cast<int>(taken.size());
                LOG_INFO(logging::LogCategory::Ocean, "water mask: {} -- basin {} ({} cells, level {:.3f} m, cap {:.3f} m, {} more basins taken in): {}",
                         geometryPath, b, list.size(), spill, capY, taken.size(), report);
            }
        }
        const double ms = sinceStart();
        LOG_INFO(logging::LogCategory::Ocean,
                 "water mask: {} -- {} x {} cells of {:.1f} cm, {} basins ({} capped, {} taken in, {} over the sea's reach), the lid {} triangles / {} vertices, the plan {} cells, from {} triangles in {:.0f} ms (read {:.1f}, grid {:.1f} of which {} sample surfaces sorted {:.1f}, basins {:.1f}, support {:.1f}, lids {:.1f})",
                 geometryPath, f.nx, f.nz, f.cell * 100.0f, basins.size(), capped, absorbed, unreached, f.lidIndices.size() / 3u, f.lidVertices.size(),
                 plan, indices.size() / 3u, ms, readMs, gridMs, hitCount, gridMs - rasterMs, basinsMs, supportMs,
                 ms - readMs - gridMs - basinsMs - supportMs);
        if (capped == 0)
        {
            LOG_WARNING(logging::LogCategory::Ocean, "water mask: {} holds no water anywhere (no basin) -- nothing is kept out of it", geometryPath);
        }
        return field;
    }

    bool InsideLid(const WaterMaskField& field, const Math::float3& p)
    {
        for (const WaterMaskBasin& basin : field.basins)
        {
            if (!(p.y < basin.capY) || p.x < basin.minX || p.x > basin.maxX || p.z < basin.minZ || p.z > basin.maxZ) { continue; }
            const int i = static_cast<int>(std::floor((p.x - field.originX) / field.cell));
            const int k = static_cast<int>(std::floor((p.z - field.originZ) / field.cell));
            if (i < 0 || k < 0 || i >= static_cast<int>(field.nx) || k >= static_cast<int>(field.nz)) { continue; }
            const float floorY = basin.floor[static_cast<std::size_t>(k) * field.nx + static_cast<std::size_t>(i)];
            if (!(floorY > -FLT_MAX) || !(p.y > floorY)) { continue; }
            // Under the cap itself (its triangles, as the GPU draws them), not only its box.
            const std::size_t end = std::min<std::size_t>(static_cast<std::size_t>(basin.firstIndex) + basin.capIndexCount, field.lidIndices.size());
            for (std::size_t t = basin.firstIndex; t + 2 < end; t += 3)
            {
                const Math::float3& a = field.lidVertices[field.lidIndices[t]];
                const Math::float3& b = field.lidVertices[field.lidIndices[t + 1]];
                const Math::float3& c = field.lidVertices[field.lidIndices[t + 2]];
                const float d1 = Cross2(b.x - a.x, b.z - a.z, p.x - a.x, p.z - a.z);
                const float d2 = Cross2(c.x - b.x, c.z - b.z, p.x - b.x, p.z - b.z);
                const float d3 = Cross2(a.x - c.x, a.z - c.z, p.x - c.x, p.z - c.z);
                const bool neg = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
                const bool pos = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
                if (!(neg && pos)) { return true; }
            }
        }
        return false;
    }

    std::string BakePathOf(const std::string& geometryPath)
    {
        constexpr std::string_view kSuffix = ".mesh.bin";
        if (geometryPath.size() <= kSuffix.size() ||
            geometryPath.compare(geometryPath.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
        {
            return {};
        }
        return geometryPath.substr(0, geometryPath.size() - kSuffix.size()) + ".watermask.bin";
    }

    bool MakeBakeKey(const std::string& geometryPath, const std::vector<int>& seeThroughSlots, float waterline, float draftOverLowest,
                     WaterMaskBakeKey& out)
    {
        // The geometry's size and vfs::Stat::version: a loose file's last write time, the same FILETIME ticks
        // std::filesystem::last_write_time gave the bakes already on disk, so their keys still match.
        const std::optional<vfs::Stat> stat = vfs::GetStat(geometryPath);
        if (!stat || stat->directory) { return false; }
        out = WaterMaskBakeKey{};
        out.geometryBytes = stat->size;
        out.geometryTime = static_cast<std::int64_t>(stat->version);
        out.waterline = waterline;
        out.draftOverLowest = draftOverLowest;
        out.glassSlots = seeThroughSlots;
        std::sort(out.glassSlots.begin(), out.glassSlots.end());
        return true;
    }

    namespace
    {
        constexpr std::uint32_t kBakeMagic = 0x4B534D57u;    // "WMSK"
        constexpr std::uint32_t kBakeEndMagic = 0x444E454Bu; // "KEND"

        struct ByteWriter
        {
            std::vector<std::uint8_t>& out;
            template <class T> void Put(const T& value)
            {
                const std::size_t at = out.size();
                out.resize(at + sizeof(T));
                std::memcpy(out.data() + at, &value, sizeof(T));
            }
            template <class T> void PutArray(const std::vector<T>& values)
            {
                Put(static_cast<std::uint32_t>(values.size()));
                const std::size_t at = out.size();
                out.resize(at + values.size() * sizeof(T));
                if (!values.empty()) { std::memcpy(out.data() + at, values.data(), values.size() * sizeof(T)); }
            }
        };
        struct ByteReader
        {
            const std::uint8_t* at;
            const std::uint8_t* end;
            bool ok = true;
            template <class T> T Get()
            {
                T value{};
                if (!ok || static_cast<std::size_t>(end - at) < sizeof(T)) { ok = false; return value; }
                std::memcpy(&value, at, sizeof(T));
                at += sizeof(T);
                return value;
            }
            template <class T> void GetArray(std::vector<T>& values, std::size_t most)
            {
                const std::uint32_t count = Get<std::uint32_t>();
                if (!ok || count > most || static_cast<std::size_t>(end - at) / sizeof(T) < count) { ok = false; return; }
                values.resize(count);
                if (count != 0u) { std::memcpy(values.data(), at, count * sizeof(T)); }
                at += count * sizeof(T);
            }
        };
        template <class T> bool SameBytes(const std::vector<T>& a, const std::vector<T>& b)
        {
            return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
        }
        bool SameFloat(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }
    }

    std::vector<std::uint8_t> SerializeField(const WaterMaskField& field, const WaterMaskBakeKey& key)
    {
        std::vector<std::uint8_t> bytes;
        ByteWriter w{ bytes };
        w.Put(kBakeMagic);
        w.Put(kWaterMaskBakeVersion);
        w.Put(key.geometryBytes);
        w.Put(key.geometryTime);
        w.Put(key.waterline);
        w.Put(key.draftOverLowest);
        w.PutArray(key.glassSlots);
        w.Put(field.originX);
        w.Put(field.originZ);
        w.Put(field.cell);
        w.Put(field.nx);
        w.Put(field.nz);
        w.Put(field.minY);
        w.Put(field.maxY);
        std::vector<float> plan(field.columns.size());
        for (std::size_t c = 0; c < plan.size(); ++c) { plan[c] = field.columns[c].w; } // .xyz are BuildField's constants
        w.PutArray(plan);
        w.PutArray(field.lidVertices);
        w.PutArray(field.lidIndices);
        w.Put(static_cast<std::uint32_t>(field.basins.size()));
        for (const WaterMaskBasin& basin : field.basins)
        {
            w.Put(basin.capY);
            w.Put(basin.firstIndex);
            w.Put(basin.capIndexCount);
            w.Put(basin.minX);
            w.Put(basin.minZ);
            w.Put(basin.maxX);
            w.Put(basin.maxZ);
            w.PutArray(basin.floor);
        }
        w.Put(kBakeEndMagic);
        return bytes;
    }

    bool RekeyBakeForPackage(std::vector<std::uint8_t>& bytes, std::uint64_t looseBytes, std::uint64_t looseVersion,
                             std::uint64_t packagedVersion, std::string& why)
    {
        ByteReader r{ bytes.data(), bytes.data() + bytes.size() };
        if (r.Get<std::uint32_t>() != kBakeMagic || !r.ok) { why = "not a water mask bake"; return false; }
        const std::uint32_t version = r.Get<std::uint32_t>();
        if (version != kWaterMaskBakeVersion)
        {
            why = "made by version " + std::to_string(version) + ", this is " + std::to_string(kWaterMaskBakeVersion);
            return false;
        }
        const std::uint64_t geometryBytes = r.Get<std::uint64_t>();
        const std::size_t timeAt = static_cast<std::size_t>(r.at - bytes.data());
        const std::int64_t geometryTime = r.Get<std::int64_t>();
        if (!r.ok) { why = "cut short"; return false; }
        if (geometryBytes != looseBytes || geometryTime != static_cast<std::int64_t>(looseVersion))
        {
            why = "made from another bake of the geometry";
            return false;
        }
        const std::int64_t packaged = static_cast<std::int64_t>(packagedVersion);
        std::memcpy(bytes.data() + timeAt, &packaged, sizeof(packaged));
        return DeserializeField(bytes.data(), bytes.size(), nullptr, why) != nullptr;
    }

    bool ReadBakeKey(const std::uint8_t* bytes, std::size_t size, WaterMaskBakeKey& out, std::string& why)
    {
        ByteReader r{ bytes, bytes + size };
        if (r.Get<std::uint32_t>() != kBakeMagic || !r.ok) { why = "not a water mask bake"; return false; }
        const std::uint32_t version = r.Get<std::uint32_t>();
        if (version != kWaterMaskBakeVersion)
        {
            why = "made by version " + std::to_string(version) + ", this is " + std::to_string(kWaterMaskBakeVersion);
            return false;
        }
        out = WaterMaskBakeKey{};
        out.geometryBytes = r.Get<std::uint64_t>();
        out.geometryTime = r.Get<std::int64_t>();
        out.waterline = r.Get<float>();
        out.draftOverLowest = r.Get<float>();
        r.GetArray(out.glassSlots, 1024u);
        if (!r.ok) { why = "cut short"; return false; }
        return true;
    }

    std::shared_ptr<WaterMaskField> DeserializeField(const std::uint8_t* bytes, std::size_t size, const WaterMaskBakeKey* expect,
                                                     std::string& why)
    {
        ByteReader r{ bytes, bytes + size };
        if (r.Get<std::uint32_t>() != kBakeMagic || !r.ok) { why = "not a water mask bake"; return nullptr; }
        const std::uint32_t version = r.Get<std::uint32_t>();
        if (version != kWaterMaskBakeVersion)
        {
            why = "made by version " + std::to_string(version) + ", this is " + std::to_string(kWaterMaskBakeVersion);
            return nullptr;
        }
        WaterMaskBakeKey key;
        key.geometryBytes = r.Get<std::uint64_t>();
        key.geometryTime = r.Get<std::int64_t>();
        key.waterline = r.Get<float>();
        key.draftOverLowest = r.Get<float>();
        r.GetArray(key.glassSlots, 1024u);
        if (!r.ok) { why = "cut short"; return nullptr; }
        if (expect)
        {
            if (key.geometryBytes != expect->geometryBytes || key.geometryTime != expect->geometryTime)
            {
                why = "made from another bake of the geometry";
                return nullptr;
            }
            if (!SameFloat(key.waterline, expect->waterline) || !SameFloat(key.draftOverLowest, expect->draftOverLowest))
            {
                why = "made for another draft";
                return nullptr;
            }
            if (key.glassSlots != expect->glassSlots) { why = "made for other glass slots"; return nullptr; }
        }
        auto field = std::make_shared<WaterMaskField>();
        WaterMaskField& f = *field;
        f.originX = r.Get<float>();
        f.originZ = r.Get<float>();
        f.cell = r.Get<float>();
        f.nx = r.Get<std::uint32_t>();
        f.nz = r.Get<std::uint32_t>();
        f.minY = r.Get<float>();
        f.maxY = r.Get<float>();
        const std::size_t cells = static_cast<std::size_t>(f.nx) * f.nz;
        if (!r.ok || cells == 0u || cells > kWaterMaskMaxCells) { why = "a grid out of range"; return nullptr; }
        std::vector<float> plan;
        r.GetArray(plan, cells);
        if (!r.ok || plan.size() != cells) { why = "the plan cut short"; return nullptr; }
        f.columns.assign(cells, Math::float4(FLT_MAX, -FLT_MAX, -FLT_MAX, FLT_MAX));
        for (std::size_t c = 0; c < cells; ++c) { f.columns[c].w = plan[c]; }
        r.GetArray(f.lidVertices, kWaterMaskLidMaxVertices);
        r.GetArray(f.lidIndices, kWaterMaskLidMaxIndices);
        const std::uint32_t basins = r.Get<std::uint32_t>();
        if (!r.ok || basins > 4096u) { why = "the lid cut short"; return nullptr; }
        f.basins.resize(basins);
        for (WaterMaskBasin& basin : f.basins)
        {
            basin.capY = r.Get<float>();
            basin.firstIndex = r.Get<std::uint32_t>();
            basin.capIndexCount = r.Get<std::uint32_t>();
            basin.minX = r.Get<float>();
            basin.minZ = r.Get<float>();
            basin.maxX = r.Get<float>();
            basin.maxZ = r.Get<float>();
            r.GetArray(basin.floor, cells);
            if (r.ok && (basin.floor.size() != cells || static_cast<std::size_t>(basin.firstIndex) + basin.capIndexCount > f.lidIndices.size()))
            {
                r.ok = false;
            }
        }
        if (!r.ok || r.Get<std::uint32_t>() != kBakeEndMagic || !r.ok) { why = "the basins cut short"; return nullptr; }
        for (const std::uint32_t index : f.lidIndices)
        {
            if (index >= f.lidVertices.size()) { why = "a lid index out of range"; return nullptr; }
        }
        return field;
    }

    bool SameField(const WaterMaskField& a, const WaterMaskField& b)
    {
        if (!SameFloat(a.originX, b.originX) || !SameFloat(a.originZ, b.originZ) || !SameFloat(a.cell, b.cell) || a.nx != b.nx ||
            a.nz != b.nz || !SameFloat(a.minY, b.minY) || !SameFloat(a.maxY, b.maxY) || !SameBytes(a.columns, b.columns) ||
            !SameBytes(a.lidVertices, b.lidVertices) || !SameBytes(a.lidIndices, b.lidIndices) || a.basins.size() != b.basins.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.basins.size(); ++i)
        {
            const WaterMaskBasin& p = a.basins[i];
            const WaterMaskBasin& q = b.basins[i];
            if (!SameFloat(p.capY, q.capY) || p.firstIndex != q.firstIndex || p.capIndexCount != q.capIndexCount ||
                !SameFloat(p.minX, q.minX) || !SameFloat(p.minZ, q.minZ) || !SameFloat(p.maxX, q.maxX) || !SameFloat(p.maxZ, q.maxZ) ||
                !SameBytes(p.floor, q.floor))
            {
                return false;
            }
        }
        return true;
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
        TakeFinishedJobs(registry);
        for (Entry& entry : registry.entries)
        {
            if (!entry.looked)
            {
                entry.looked = true;
                std::vector<int> glass;
                entry.object->SeeThroughSlots(glass);
                // Where still water stands on it: a boat's draft (over its origin), else a floater's (over its lowest point).
                float waterline = 3.0e38f, draftOverLowest = -3.0e38f;
                if (const physics::VesselSettings* vessel = entry.object->GetVesselSettings()) { waterline = vessel->draft; }
                else if (entry.object->IsBuoyant()) { draftOverLowest = entry.object->GetBuoyancySettings().draft; }
                const std::string& path = entry.object->GetGeometryPath();
                WaterMaskBakeKey bakeKey;
                if (path.empty() || !MakeBakeKey(path, glass, waterline, draftOverLowest, bakeKey))
                {
                    LOG_WARNING(logging::LogCategory::Ocean, "water mask: no geometry file for '{}' -- the water is drawn inside it", path);
                    continue;
                }
                entry.key = RegistryKey(path, bakeKey);
                entry.field = Resolve(registry, entry.key, path, bakeKey);
            }
            if (!entry.field && !entry.key.empty())
            {
                // A worker's field that has landed since.
                if (const auto found = registry.fields.find(entry.key); found != registry.fields.end()) { entry.field = found->second; }
            }
            if (!entry.field || !entry.object->IsVisibleGame()) { continue; }
            const Math::mat4 model = entry.object->GetModelMatrix();
            out.push_back(WaterMaskInstance{ entry.field, Math::mat4::Inverse(model), model });
        }
    }
}
} // namespace ocean
