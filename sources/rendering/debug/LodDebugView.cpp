#include "rendering/debug/LodDebugView.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <vector>

#include "app/camera/Camera.h"
#include "core/math/AABB.h"
#include "rendering/core/Renderer.h"
#include "rendering/debug/DebugDraw.h"
#include "rendering/meshes/LodSelect.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/renderables/RenderableObject.h"
#include "text/TextManager.h"

namespace render
{
namespace
{
constexpr unsigned int kTiers = kMaxShadowLods;

Math::float4 TierColor(unsigned int tier)
{
    switch (tier)
    {
    case 0u:  return { 0.25f, 1.00f, 0.30f, 0.85f }; // green  - full detail
    case 1u:  return { 0.95f, 0.90f, 0.20f, 0.85f }; // yellow
    case 2u:  return { 1.00f, 0.55f, 0.10f, 0.85f }; // orange
    default:  return { 1.00f, 0.22f, 0.22f, 0.85f }; // red    - coarsest
    }
}

// Apparent triangle size -> colour, on the SAME green..red ramp as the tiers so the two modes are
// directly comparable. This is the mode that shows what the tier colour cannot: two chunks at the
// same tier can differ several-fold in delivered detail, because the simplifier reduces each chunk
// by a fixed RATIO of its own LOD0 density and chunk densities are not equal.
Math::float4 DensityColor(float mrad)
{
    static constexpr float kStops[3] = { 15.0f, 35.0f, 70.0f };
    if (mrad <= kStops[0]) { return TierColor(0u); }
    if (mrad >= kStops[2]) { return TierColor(3u); }
    const unsigned int lo = mrad < kStops[1] ? 0u : 1u;
    const float a = (mrad - kStops[lo]) / (kStops[lo + 1u] - kStops[lo]);
    const Math::float4 c0 = TierColor(lo);
    const Math::float4 c1 = TierColor(lo + 1u);
    return { c0.x + (c1.x - c0.x) * a, c0.y + (c1.y - c0.y) * a,
             c0.z + (c1.z - c0.z) * a, 0.85f };
}

// The exact metric RenderableObject::SelectLod measures against: the closest point of the world
// AABB, not its centre. Reproduced here so the view can DRAW the segment it was taken along.
Math::float3 ClosestPointOnBox(const AABB& box, const Math::float3& p)
{
    const Math::float3 mn = box.GetMin();
    const Math::float3 mx = box.GetMax();
    return { p.x < mn.x ? mn.x : (p.x > mx.x ? mx.x : p.x),
             p.y < mn.y ? mn.y : (p.y > mx.y ? mx.y : p.y),
             p.z < mn.z ? mn.z : (p.z > mx.z ? mx.z : p.z) };
}

// Mean world edge length implied by `tris` triangles spread over the box, assuming grid triangles
// (area = e^2/2). Checked against the edge lengths measured off every submesh of every LOD in
// atoll_island: median estimate/measured 0.94 with a 0.75..1.22 p10-p90 band, i.e. ~13% typical
// error. That is far inside the several-fold differences this view exists to show, and it costs no
// vertex readback. The XZ-footprint-only form it replaced sat at 0.83 median / ~23% error.
float EstimateEdgeMeters(const AABB& box, unsigned int tris)
{
    if (tris == 0u) { return 0.0f; }
    const Math::float3 mn = box.GetMin();
    const Math::float3 mx = box.GetMax();
    const float ex = mx.x - mn.x;
    const float ey = mx.y - mn.y;
    const float ez = mx.z - mn.z;
    // Half the box's surface area: the share of it a camera can see at once. For a flat terrain
    // chunk (ey small) this collapses to the XZ footprint, which is the right area for a heightfield
    // tile; for a tall thin prop it does not, which is the point.
    const float area = ex * ey + ey * ez + ez * ex;
    if (area < 1e-6f) { return 0.0f; }
    return std::sqrt(2.0f * area / static_cast<float>(tris));
}

struct Entry
{
    AABB box;             // world-space
    Math::float3 metricAt;// the point the selector measured TO (chunk: closest point; mesh: centre)
    float dist = 0.0f;    // the distance the selector actually used
    unsigned int tier = 0u;
    unsigned int tris = 0u;
    float edgeM = 0.0f;
    float mrad = 0.0f;    // apparent triangle size = 1000 * edge / distance
    int chunk = -1;       // chunk ordinal, or -1 for a whole (non-chunked) mesh
    // Regular meshes only: the selector's real input is distance / instance RADIUS, not metres.
    float radius = 0.0f;
    float ratio = 0.0f;
    float fade = 0.0f;    // crossfade weight into tier+1 (0 = solid); chunked meshes never fade
    float sx = 0.0f, sy = 0.0f;
    bool onScreen = false;
};
} // namespace

void DrawLodDebug(Renderer* renderer,
                  const Camera& camera,
                  const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
                  const RenderableObjectBase* const* selected,
                  std::size_t selectedCount)
{
    if (g_lodDebugMode == LodDebugMode::Off || renderer == nullptr) { return; }

    const bool selectedOnly = g_lodDebugFilter == LodDebugFilter::Selected;
    if (selectedOnly && selected == nullptr) { selectedCount = 0; }
    auto isSelected = [&](const RenderableObjectBase* o)
    {
        for (std::size_t i = 0; i < selectedCount; ++i) { if (selected[i] == o) { return true; } }
        return false;
    };

    DebugDrawSystem* dd = renderer->GetDebugDrawSystem();
    if (dd != nullptr && !dd->IsInitialized()) { dd = nullptr; }
    TextManager* tm = renderer->GetTextManager();
    if (dd == nullptr && tm == nullptr) { return; }

    const Math::float3 cam = camera.GetPosition();
    // No-jitter viewProj on purpose: with DLSS on, the jittered one moves every label by a pixel
    // per frame, which reads as instability in the thing being debugged.
    const Math::mat4 vp = camera.GetViewProjMatrixNoJitter();
    const float vw = static_cast<float>(renderer->GetWidth());
    const float vh = static_cast<float>(renderer->GetHeight());

    auto project = [&](const Math::float3& p, float& sx, float& sy) -> bool
    {
        const Math::float4 clip = vp.Transform(Math::float4{ p.x, p.y, p.z, 1.0f });
        if (clip.w <= 1e-4f) { return false; }
        const float nx = clip.x / clip.w;
        const float ny = clip.y / clip.w;
        if (nx < -1.25f || nx > 1.25f || ny < -1.25f || ny > 1.25f) { return false; }
        sx = (nx * 0.5f + 0.5f) * vw;
        sy = (1.0f - (ny * 0.5f + 0.5f)) * vh;
        return true;
    };

    const float range = std::max(10.0f, g_lodDebugRange);
    std::vector<Entry> entries;
    entries.reserve(256);

    for (const auto& objPtr : objects)
    {
        const RenderableObjectBase* base = objPtr.get();
        if (base == nullptr) { continue; }
        if (selectedOnly && !isSelected(base)) { continue; }
        const RenderableObject* ro = base->AsRenderableObject();
        if (ro == nullptr) { continue; }
        const Mesh* mesh = ro->GetMesh();
        if (mesh == nullptr) { continue; }

        // The "regular meshes" switch is about not drowning a whole-level view in props. An
        // explicitly selected mesh was asked for by name, so it is never filtered out by it.
        const bool chunked = mesh->IsChunkedSubmeshes() && !ro->ChunkCameraLods().empty();
        if (!chunked && !g_lodDebugRegularMeshes && !selectedOnly) { continue; }

        if (chunked)
        {
            const std::vector<AABB>& local = mesh->GetSubmeshBounds();
            const std::vector<std::uint8_t>& tiers = ro->ChunkCameraLods();
            const Math::mat4 model = ro->GetModelMatrix();
            const size_t n = std::min(local.size(), tiers.size());
            for (size_t s = 0; s < n; ++s)
            {
                if (!local[s].IsValid()) { continue; }
                Entry e;
                e.box = local[s].Transform(model);
                e.metricAt = ClosestPointOnBox(e.box, cam);
                e.dist = (cam - e.metricAt).Length();
                if (!selectedOnly && e.dist > range) { continue; }
                e.tier = tiers[s];
                // Resolved through the same clamp the draw uses, so a label can never claim a LOD
                // the mesh does not actually have.
                const unsigned int lod = mesh->ClampExplicitLod(e.tier);
                const std::vector<Mesh::Submesh>& table = mesh->SubmeshesForLod(lod);
                e.tris = s < table.size() ? table[s].indexCount / 3u : 0u;
                e.edgeM = EstimateEdgeMeters(e.box, e.tris);
                e.mrad = e.dist > 1e-3f ? 1000.0f * e.edgeM / e.dist : 0.0f;
                e.chunk = static_cast<int>(s);
                e.onScreen = project(e.box.GetCenter(), e.sx, e.sy);
                entries.push_back(e);
            }
        }
        else
        {
            // A regular mesh selects on distance / instance RADIUS, from the bounds CENTRE -- an
            // object-size-relative criterion, so the metres a chunk is judged by would be the
            // wrong number to show here. Both inputs are reported, and both are drawn by the probe.
            Entry e;
            e.box = ro->GetWorldBounds();
            if (!e.box.IsValid()) { continue; }
            e.metricAt = e.box.GetCenter();
            e.dist = (cam - e.metricAt).Length();
            if (!selectedOnly && e.dist > range) { continue; }
            e.radius = ro->GetLodRadius();
            e.ratio = e.radius > 1e-4f ? e.dist / e.radius : 0.0f;
            e.tier = ro->GetCameraLod();
            e.fade = ro->GetCameraLodFade();
            const unsigned int lod = mesh->ClampExplicitLod(e.tier);
            e.tris = mesh->GetLodIndexCount(lod) / 3u;
            e.edgeM = EstimateEdgeMeters(e.box, e.tris);
            e.mrad = e.dist > 1e-3f ? 1000.0f * e.edgeM / e.dist : 0.0f;
            e.onScreen = project(e.metricAt, e.sx, e.sy);
            entries.push_back(e);
        }
    }

    const bool density = g_lodDebugMode == LodDebugMode::Density;

    // --- boxes -----------------------------------------------------------------------------------
    if (dd != nullptr && g_lodDebugBoxes)
    {
        std::vector<const Entry*> order;
        order.reserve(entries.size());
        for (const Entry& e : entries) { order.push_back(&e); }
        // Chunks first, then regular meshes nearest-first, so the budget spends itself on the
        // terrain and on whatever the camera is standing in front of.
        std::sort(order.begin(), order.end(), [](const Entry* a, const Entry* b)
        {
            const bool ac = a->chunk >= 0;
            const bool bc = b->chunk >= 0;
            if (ac != bc) { return ac; }
            return a->dist < b->dist;
        });
        const size_t budget = g_lodDebugMaxBoxes > 0
            ? static_cast<size_t>(g_lodDebugMaxBoxes) : order.size();
        const size_t drawn = std::min(order.size(), budget);
        for (size_t i = 0; i < drawn; ++i)
        {
            const Entry& e = *order[i];
            dd->AddBox(e.box, density ? DensityColor(e.mrad) : TierColor(e.tier), true);
        }
    }

    // --- the criteria themselves -----------------------------------------------------------------
    // The boundary for tier t is a SPHERE of radius dist0*factor^t around the camera. Drawn as its
    // intersection with sea level, which is the slice that lines up with the terrain being judged;
    // a boundary whose radius is under the camera's height never reaches the ground at all, and
    // that case is called out in the readout instead of silently drawing nothing.
    const float d0 = std::max(1.0f, g_chunkLodDist0);
    const float factor = std::max(1.01f, g_chunkLodDistFactor);
    float bounds[kTiers - 1u];
    {
        float b = d0;
        for (unsigned int i = 0; i + 1u < kTiers; ++i) { bounds[i] = b; b *= factor; }
    }
    if (dd != nullptr && g_lodDebugCriteria)
    {
        constexpr int kSegments = 96;
        for (unsigned int i = 0; i + 1u < kTiers; ++i)
        {
            const float r2 = bounds[i] * bounds[i] - cam.y * cam.y;
            if (r2 <= 1.0f) { continue; } // boundary sphere does not reach sea level
            const float r = std::sqrt(r2);
            const Math::float4 col = density ? Math::float4{ 0.60f, 0.62f, 0.72f, 0.5f } : TierColor(i + 1u);
            Math::float3 prev{ cam.x + r, 0.0f, cam.z };
            for (int k = 1; k <= kSegments; ++k)
            {
                const float a = 6.2831853f * static_cast<float>(k) / static_cast<float>(kSegments);
                const Math::float3 cur{ cam.x + r * std::cos(a), 0.0f, cam.z + r * std::sin(a) };
                dd->AddLine(prev, cur, col);
                prev = cur;
            }
        }
    }

    // --- probe: the entry nearest the crosshair ---------------------------------------------------
    const Entry* probe = nullptr;
    {
        float best = 1e30f;
        for (const Entry& e : entries)
        {
            if (!e.onScreen) { continue; }
            const float dx = e.sx - vw * 0.5f;
            const float dy = e.sy - vh * 0.5f;
            const float d = dx * dx + dy * dy;
            if (d < best) { best = d; probe = &e; }
        }
    }
    if (probe != nullptr && dd != nullptr && g_lodDebugCriteria)
    {
        const Math::float4 white{ 1.0f, 1.0f, 1.0f, 0.95f };
        dd->AddBox(probe->box, white, true);
        dd->AddLine(cam, probe->metricAt, white);          // the measured segment, exactly
        dd->AddSphere(probe->metricAt, 0.6f, white, true); // the point it was measured to
        if (probe->chunk < 0 && probe->radius > 1e-4f)
        {
            // The second input of a regular mesh's criterion. Drawn because the ratio is only
            // readable as a pair: the same distance is LOD0 for a palm and LOD3 for a pebble.
            dd->AddSphere(probe->metricAt, probe->radius, Math::float4{ 0.4f, 0.8f, 1.0f, 0.8f }, true);
        }
    }

    if (tm == nullptr) { return; }

    // --- labels ----------------------------------------------------------------------------------
    if (g_lodDebugLabels)
    {
        std::vector<const Entry*> visible;
        visible.reserve(entries.size());
        for (const Entry& e : entries) { if (e.onScreen) { visible.push_back(&e); } }
        std::sort(visible.begin(), visible.end(),
                  [](const Entry* a, const Entry* b) { return a->dist < b->dist; });

        // Greedy screen-space thinning: the nearest label wins its neighbourhood. Without it a
        // hundred chunks overlap into an unreadable smear.
        constexpr float kMinSepX = 92.0f;
        constexpr float kMinSepY = 20.0f;
        constexpr size_t kMaxLabels = 72;
        std::vector<const Entry*> placed;
        placed.reserve(kMaxLabels);
        for (const Entry* e : visible)
        {
            if (placed.size() >= kMaxLabels) { break; }
            bool clear = true;
            for (const Entry* p : placed)
            {
                if (std::abs(p->sx - e->sx) < kMinSepX && std::abs(p->sy - e->sy) < kMinSepY)
                {
                    clear = false;
                    break;
                }
            }
            if (!clear) { continue; }
            placed.push_back(e);
            const Math::float4 col = density ? DensityColor(e->mrad) : TierColor(e->tier);
            tm->AddTextfShadow(static_cast<int>(e->sx) - 34, static_cast<int>(e->sy) - 7, 14.0f,
                               col, true, L"L%u %.0fm %.1fmr", e->tier, e->dist, e->mrad);
        }
    }

    // --- readout ---------------------------------------------------------------------------------
    // Per-family stats. The two families answer to DIFFERENT curves, so folding them into one
    // histogram would produce a number that describes neither.
    struct Stats
    {
        unsigned int hist[kTiers] = {};
        float mradMin[kTiers];
        float mradMax[kTiers];
        size_t total = 0;
        size_t onScreen = 0;
        Stats() { for (unsigned int i = 0; i < kTiers; ++i) { mradMin[i] = 1e30f; mradMax[i] = 0.0f; } }
        void Add(const Entry& e)
        {
            const unsigned int t = e.tier < kTiers ? e.tier : kTiers - 1u;
            ++hist[t];
            ++total;
            if (e.onScreen)
            {
                ++onScreen;
                // Degenerate entries (an empty submesh, a box with no extent) would pin the
                // minimum at zero and turn the spread ratio into a number about nothing.
                if (e.mrad > 0.1f)
                {
                    mradMin[t] = std::min(mradMin[t], e.mrad);
                    mradMax[t] = std::max(mradMax[t], e.mrad);
                }
            }
        }
    };
    Stats chunkStats;
    Stats meshStats;
    for (const Entry& e : entries) { (e.chunk >= 0 ? chunkStats : meshStats).Add(e); }

    const Math::float4 head{ 1.0f, 1.0f, 1.0f, 0.95f };
    const Math::float4 dim{ 0.75f, 0.80f, 0.90f, 0.90f };

    // Height the block needs, so it always sits just above the bottom edge however many
    // families and tiers are populated this frame.
    int lines = 1;
    for (unsigned int t = 0; t < kTiers; ++t)
    {
        if (chunkStats.hist[t] != 0u) { ++lines; }
        if (meshStats.hist[t] != 0u) { ++lines; }
    }
    if (chunkStats.total != 0) { ++lines; }
    if (meshStats.total != 0) { ++lines; }
    if (cam.y > d0 * 0.85f && chunkStats.total != 0) { ++lines; }
    if (probe != nullptr) { lines += 2; }
    int y = static_cast<int>(vh) - 26 - lines * 18;

    tm->AddTextfShadow(8, y, 16.0f, head, true,
                       L"LOD debug [%s]   %s   %zu chunks + %zu meshes   (%zu on screen)",
                       density ? L"DENSITY: apparent triangle size" : L"TIER: selected LOD",
                       selectedOnly ? L"selection only" : L"whole level",
                       chunkStats.total, meshStats.total, chunkStats.onScreen + meshStats.onScreen);
    y += 21;

    if (selectedOnly && entries.empty())
    {
        // Said out loud rather than quietly falling back to the whole level: an empty view here
        // means "nothing is selected", and that has to be distinguishable from "the view is broken".
        tm->AddTextfShadow(16, y, 14.0f, Math::float4{ 1.0f, 0.75f, 0.35f, 0.95f }, true,
                           selectedCount == 0
                               ? L"filter is SELECTION ONLY and nothing is selected - pick a mesh in the viewport, or switch the filter to the whole level"
                               : L"filter is SELECTION ONLY and the selection holds no LODable mesh");
        return;
    }

    // The spread is the point of these lines: one tier is one band of the criterion, but the
    // detail it DELIVERS is that entry's own LOD0 density times a fixed simplification ratio. A
    // wide spread means the picture will not look like the tier map, and the tier map is not wrong.
    auto emitFamily = [&](const Stats& st, const wchar_t* title, const wchar_t* unit)
    {
        if (st.total == 0) { return; }
        tm->AddTextfShadow(16, y, 14.0f, dim, true, L"%s   %s", title, unit);
        y += 18;
        for (unsigned int t = 0; t < kTiers; ++t)
        {
            if (st.hist[t] == 0u) { continue; }
            if (st.mradMax[t] > 0.0f)
            {
                tm->AddTextfShadow(32, y, 14.0f, TierColor(t), true,
                                   L"LOD%u   %u   apparent triangle size on screen %.1f .. %.1f mrad   (%.1fx spread)",
                                   t, st.hist[t], st.mradMin[t], st.mradMax[t],
                                   st.mradMin[t] > 0.01f ? st.mradMax[t] / st.mradMin[t] : 0.0f);
            }
            else
            {
                tm->AddTextfShadow(32, y, 14.0f, TierColor(t), true,
                                   L"LOD%u   %u   (none on screen)", t, st.hist[t]);
            }
            y += 18;
        }
    };

    wchar_t chunkCurve[192];
    std::swprintf(chunkCurve, 192,
                  L"metres to the closest box point   dist0 %.0f  factor %.2f  hyst 15%%  ->  boundaries %.0f / %.0f / %.0f m",
                  d0, factor, bounds[0], bounds[1], bounds[2]);
    wchar_t meshCurve[192];
    std::swprintf(meshCurve, 192,
                  L"distance / instance RADIUS from the centre   hyst 15%%  ->  boundaries %.0f / %.0f / %.0f   fade band %.2f",
                  g_lodBound0, g_lodBound1, g_lodBound2, g_lodFadeBand);
    emitFamily(chunkStats, L"chunked terrain:", chunkCurve);
    emitFamily(meshStats, L"regular meshes:", meshCurve);

    // The floor the camera height puts under EVERY distance. At altitude this is what makes the
    // near tiers unreachable, and it is invisible in a plan-view mental model of the curve.
    if (cam.y > d0 * 0.85f && chunkStats.total != 0)
    {
        tm->AddTextfShadow(16, y, 14.0f, Math::float4{ 1.0f, 0.75f, 0.35f, 0.95f }, true,
                           L"camera is %.0f m up -> no ground chunk can be nearer than that; LOD0 (< %.0f m) is unreachable from this height",
                           cam.y, d0);
        y += 18;
    }

    if (probe != nullptr)
    {
        // Why THAT tier: which boundary the measured value sits past, and how much slack is left
        // before the next flip -- stated as the numbers the selector actually compares against, in
        // the units THAT family is selected in.
        const unsigned int t = probe->tier;
        if (probe->chunk >= 0)
        {
            tm->AddTextfShadow(8, y, 15.0f, head, true,
                               L"probe: chunk %d   dist %.1f m   LOD%u   %u tris   ~%.1f m edge   %.1f mrad",
                               probe->chunk, probe->dist, probe->tier, probe->tris, probe->edgeM, probe->mrad);
            y += 18;
            if (t == 0u)
            {
                tm->AddTextfShadow(24, y, 14.0f, dim, true,
                                   L"%.1f m is below the first boundary (%.1f m = %.0f * 1.15) -> LOD0",
                                   probe->dist, bounds[0] * 1.15f, d0);
            }
            else
            {
                tm->AddTextfShadow(24, y, 14.0f, dim, true,
                                   L"%.1f m is past boundary %u (%.1f m = %.0f * 1.15) -> LOD%u; drops back to LOD%u below %.1f m",
                                   probe->dist, t, bounds[t - 1u] * 1.15f, bounds[t - 1u], t, t - 1u,
                                   bounds[t - 1u] * 0.85f);
            }
        }
        else
        {
            tm->AddTextfShadow(8, y, 15.0f, head, true,
                               L"probe: mesh   dist %.1f m / radius %.2f m = ratio %.1f   LOD%u%s   %u tris   ~%.2f m edge   %.1f mrad",
                               probe->dist, probe->radius, probe->ratio, probe->tier,
                               probe->fade > 0.001f ? L" (crossfading)" : L"",
                               probe->tris, probe->edgeM, probe->mrad);
            y += 18;
            float rb[3];
            rb[0] = g_lodBound0 > 0.5f ? g_lodBound0 : 0.5f;
            rb[1] = g_lodBound1 > rb[0] * 1.05f ? g_lodBound1 : rb[0] * 1.05f;
            rb[2] = g_lodBound2 > rb[1] * 1.05f ? g_lodBound2 : rb[1] * 1.05f;
            if (probe->fade > 0.001f)
            {
                // Inside the crossfade band selection is STATELESS and BOTH tiers draw, so quoting
                // a hysteresis edge here would be quoting a rule that is not currently in force.
                tm->AddTextfShadow(24, y, 14.0f, dim, true,
                                   L"ratio %.1f is inside boundary %u's fade band (%.1f) -> LOD%u and LOD%u both draw, %.0f%% handed over",
                                   probe->ratio, t, rb[t < 3u ? t : 2u], t, t + 1u, probe->fade * 100.0f);
            }
            else if (t == 0u)
            {
                tm->AddTextfShadow(24, y, 14.0f, dim, true,
                                   L"ratio %.1f is below the first boundary (%.1f = %.0f * 1.15) -> LOD0; that is %.0f m for this radius",
                                   probe->ratio, rb[0] * 1.15f, rb[0], rb[0] * 1.15f * probe->radius);
            }
            else
            {
                tm->AddTextfShadow(24, y, 14.0f, dim, true,
                                   L"ratio %.1f is past boundary %u (%.1f = %.0f * 1.15) -> LOD%u; drops back to LOD%u below ratio %.1f (%.0f m)",
                                   probe->ratio, t, rb[t - 1u] * 1.15f, rb[t - 1u], t, t - 1u,
                                   rb[t - 1u] * 0.85f, rb[t - 1u] * 0.85f * probe->radius);
            }
        }
    }
}
} // namespace render
