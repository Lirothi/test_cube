#pragma once
// Where a mesh asset's LODs switch, and what is out of its sane range -- with what to do about it (2026-11-11, owner: "all the
// LOD switching stats in the Mesh Editor with warnings where a number leaves sane sense", "and when the importer writes a
// warning, it must say how to fix it"). One copy at scale 1 on UE's reference view (1920 x 1080, 90 degrees -- the screen
// the automatic screen sizes are estimated on, LodSelect.h): its pixels are 1080p pixels, the units of PixelError, so the
// report does not change with the editor's window. The rule is RenderableObject::LodSelectionBounds / LodSelectionRadius:
// the measured deviation through UE's screen size, the authored screen sizes, the global distance/radius curve for foliage;
// a chunked mesh switches per chunk. Used by the Mesh Editor's "LOD switching" section and the importer's log.
#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "rendering/meshes/LodSelect.h"
#include "rendering/meshes/MeshManager.h"
#include "third_party/json/json.hpp"

namespace lodreport
{
struct Row
{
    std::uint32_t tris = 0;
    float deviation = 0.0f;  // m, the bin's (measured at the bake)
    float screenSize = 0.0f; // UE's (0 = none: the curve or a chunked mesh)
    float distance = 0.0f;   // m where this LOD comes in (LOD0 0); FLT_MAX = never
    float meshPx = 0.0f;     // the mesh's diameter there, 1080p pixels
    float popPx = 0.0f;      // its deviation there, 1080p pixels
    bool skipped = false;    // the next LOD comes in at the same distance: this one is never drawn (harmless)
};

struct Warning
{
    std::string text; // what is wrong
    std::string fix;  // what to do about it
};

struct Report
{
    std::string method;  // how this asset selects its LOD
    std::string context; // the numbers it was computed with
    std::vector<Row> rows;
    std::vector<Warning> warnings;
    bool switches = false; // rows carry distances (not a chunked mesh)
    float pixelError = 0.0f;
};

inline std::string Metres(float m)
{
    char buf[32];
    if (m >= FLT_MAX * 0.5f) { return "never"; }
    if (m >= 1000.0f) { std::snprintf(buf, sizeof(buf), "%.2f km", static_cast<double>(m) / 1000.0); }
    else if (m >= 10.0f) { std::snprintf(buf, sizeof(buf), "%.0f m", static_cast<double>(m)); }
    else { std::snprintf(buf, sizeof(buf), "%.2f m", static_cast<double>(m)); }
    return buf;
}

// `doc` = the asset's mesh.json; `foliage` = any "windFoliage" slot weight > 0 (the runtime's own test).
inline Report Build(const MeshManager::BinaryInfo& info, const nlohmann::json& doc, bool foliage)
{
    Report r;
    if (info.lods.empty() || !doc.is_object()) { return r; }
    const size_t count = std::min<size_t>(info.lods.size(), render::kLodScreenSizes);
    const float radius = info.sphereRadius;
    constexpr float kPpu = render::kUeLodPixelsPerUnit;
    constexpr float kMultiple = render::kUeLodScreenMultiple;
    const float assetDistance = doc.value("lodDistanceScale", 1.0f);
    const float globalDistance = std::max(render::g_lodDistanceScale_game, 1e-3f);
    const float selRadius = radius * assetDistance; // LodSelectionRadius at scale 1
    const float assetPixelError = doc.value("lodPixelError", 0.0f);
    const std::string group = doc.value("lodGroup", std::string());
    const float groupPixelError = render::UeLodGroupPixelError(group);
    const float pixelErrorScale = doc.value("lodPixelErrorScale", 1.0f);
    const float basePixelError = assetPixelError > 0.0f ? assetPixelError
                               : (groupPixelError > 0.0f ? groupPixelError : render::g_lodPixelError_game);
    const float pixelError = basePixelError * pixelErrorScale;
    r.pixelError = pixelError;
    const auto screenIt = doc.find("lodScreenSize");
    const bool authored = screenIt != doc.end() && screenIt->is_array();
    const bool chunked = doc.value("chunkGrid", 0) > 1;
    const float ratioScale = doc.value("lodRatioScale", 1.0f);
    const bool permissive = doc.value("lodPermissive", false);
    const bool dropSmall = doc.value("lodDropSmallParts", false);
    std::optional<bool> lod3Stated;
    if (const auto la = doc.find("lod3Aggressive"); la != doc.end() && la->is_boolean()) { lod3Stated = la->get<bool>(); }
    std::vector<float> slotFoliage;
    if (const auto wf = doc.find("windFoliage"); wf != doc.end() && wf->is_array())
    {
        for (const nlohmann::json& v : *wf) { slotFoliage.push_back(v.is_number() ? v.get<float>() : 0.0f); }
    }

    r.rows.resize(count);
    for (size_t i = 0; i < count; ++i)
    {
        r.rows[i].tris = info.lods[i].totalTris;
        r.rows[i].deviation = i == 0 ? 0.0f : info.lods[i].error;
        r.rows[i].distance = i == 0 ? 0.0f : FLT_MAX;
    }
    char buf[400];
    std::snprintf(buf, sizeof(buf), "UE's reference view: 1920 x 1080, 90 degrees (pixels below are its pixels; a wider view "
                  "switches nearer). The mesh's radius %.2f m; distance scale %.2f (asset) x %.2f (lod.distanceScale).",
                  static_cast<double>(radius), static_cast<double>(assetDistance), static_cast<double>(globalDistance));
    r.context = buf;

    float bound[3] = { FLT_MAX, FLT_MAX, FLT_MAX }; // distance / selection radius where LOD k+1 comes in
    if (chunked)
    {
        r.method = "Chunked terrain: each chunk switches by its own distance (lod.chunkDistance x chunkFactor^tier); the "
                   "deviation is not used.";
    }
    else if (!render::g_lodUeScreenSize_game || (foliage && !authored))
    {
        r.method = foliage ? "Foliage: the global distance/radius curve (8 / 16 / 32 radii, Developer Controls > LOD); its "
                             "chain is the leaf prune and the slot drops, whose deviation is not used (UE's foliage makes no "
                             "LODs; \"lodScreenSize\" overrides)."
                           : "The global distance/radius curve (lod.ueScreenSize is off).";
        const float b[3] = { render::g_lodBound0_game, render::g_lodBound1_game, render::g_lodBound2_game };
        for (size_t k = 0; k + 1 < count && k < 3; ++k) { bound[k] = b[k]; }
    }
    else
    {
        float ss[render::kLodScreenSizes] = {};
        if (authored)
        {
            for (size_t i = 0; i < render::kLodScreenSizes && i < screenIt->size(); ++i)
            {
                ss[i] = (*screenIt)[i].is_number() ? (*screenIt)[i].get<float>() : 0.0f;
            }
            r.method = "Authored screen sizes (mesh.json \"lodScreenSize\").";
        }
        else
        {
            float deviation[render::kLodScreenSizes] = {};
            for (size_t i = 0; i < count; ++i) { deviation[i] = info.lods[i].error; }
            render::AutoLodScreenSizes(deviation, static_cast<unsigned int>(count), radius, pixelError, ss);
            const char* from = assetPixelError > 0.0f ? "the asset's lodPixelError" : nullptr;
            std::string fromText = from ? from : (groupPixelError > 0.0f ? "LOD group " + group : "lod.pixelError");
            std::snprintf(buf, sizeof(buf), "UE's screen size from the measured deviation: a LOD comes in where its deviation "
                          "spans %.2f px (PixelError %.2f from %s, x %.2f lodPixelErrorScale).", static_cast<double>(pixelError),
                          static_cast<double>(basePixelError), fromText.c_str(), static_cast<double>(pixelErrorScale));
            r.method = buf;
        }
        for (size_t k = 0; k + 1 < count && k < 3; ++k)
        {
            const float t = ss[k + 1] * globalDistance;
            if (t > 0.0f) { bound[k] = 2.0f * kMultiple / t; }
            r.rows[k + 1].screenSize = ss[k + 1];
        }
    }
    if (!chunked)
    {
        r.switches = true;
        for (size_t i = 1; i < count; ++i)
        {
            Row& row = r.rows[i];
            if (bound[i - 1] >= FLT_MAX * 0.5f) { continue; }
            row.distance = bound[i - 1] * selRadius;
            if (row.distance > 1e-4f)
            {
                row.meshPx = 2.0f * radius * kPpu / row.distance;
                row.popPx = row.deviation * kPpu / row.distance;
            }
        }
    }

    // --- warnings: a number out of its sane range, what it means, what to do ---------------------------------------
    // A level under kWorthTris triangles costs nothing worth a warning; the curve's distances are foliage's own choice.
    constexpr std::uint32_t kWorthTris = 1000u;
    const auto add = [&r](const std::string& text, const std::string& fix) { r.warnings.push_back({ text, fix }); };
    char fixBuf[400];
    for (size_t i = 1; i < count; ++i)
    {
        const Row& row = r.rows[i];
        const Row& prev = r.rows[i - 1];
        if (prev.tris >= kWorthTris && row.tris > prev.tris * 85u / 100u)
        {
            if (row.tris >= prev.tris)
            {
                std::snprintf(buf, sizeof(buf), "LOD%zu has %s triangles than LOD%zu (%u vs %u): it saves nothing.", i,
                              row.tris > prev.tris ? "MORE" : "as many", i - 1, row.tris, prev.tris);
            }
            else
            {
                std::snprintf(buf, sizeof(buf), "LOD%zu keeps %.0f %% of LOD%zu's triangles (%u of %u): a weak step.", i,
                              100.0 * row.tris / prev.tris, i - 1, row.tris, prev.tris);
            }
            if (foliage && i < 3)
            {
                std::snprintf(fixBuf, sizeof(fixBuf), "Leaf cards (and double-sided twins) do not collapse: drop a slot at this "
                              "level (lod%zuDropSlots) or leave the leaves to LOD3's prune. Do not tick 'Allow collapses across "
                              "attribute seams' on foliage: it shreds the blades.", i);
            }
            else if (foliage && i == 3)
            {
                std::snprintf(fixBuf, sizeof(fixBuf), "LOD3's leaf prune keeps more than LOD2's simplification: lower 'Leaf prune "
                              "keep' (foliagePruneKeep, now %.2f) or 'LOD3 triangle target' (lod3RatioScale).",
                              static_cast<double>(doc.value("foliagePruneKeep", 0.35f)));
            }
            else if (!permissive)
            {
                std::snprintf(fixBuf, sizeof(fixBuf), "The simplifier stops at the mesh's seams (UV / normal splits lock their "
                              "vertices): tick 'Allow collapses across attribute seams' (lodPermissive)%s, then Save.",
                              dropSmall ? "" : " -- and 'Drop small disconnected parts' (lodDropSmallParts) for loose bits");
            }
            else
            {
                std::snprintf(fixBuf, sizeof(fixBuf), "Seams are already open: the mesh has no collapse left that keeps its "
                              "shape%s. Harmless when it is small on screen; else drop the level's slot (lod%zuDropSlots).",
                              dropSmall ? "" : " (try 'Drop small disconnected parts')", i);
            }
            add(buf, fixBuf);
        }
        if (!r.switches || foliage) { continue; } // the curve is foliage's own; a chunk switches by distance
        if (row.distance >= FLT_MAX * 0.5f)
        {
            std::snprintf(buf, sizeof(buf), "LOD%zu is never selected: it has no screen size.", i);
            add(buf, authored ? "Author a screen size for it under 'Author LOD screen sizes', or untick that box for UE's estimate."
                              : "Save re-bakes the mesh, which measures its deviation.");
            continue;
        }
        // The next LOD comes in at the same distance (a part both lose decides both deviations, or the same triangles):
        // this one is never drawn. Harmless -- it costs its index buffer -- so a note in the table, not a warning.
        if (i + 1 < count && r.rows[i + 1].distance <= row.distance * 1.001f) { r.rows[i].skipped = true; }
        const float area = 3.14159265f * 0.25f * row.meshPx * row.meshPx;
        const float density = area > 1e-6f ? static_cast<float>(prev.tris) / area : 0.0f;
        if (row.meshPx < 4.0f && prev.tris >= kWorthTris)
        {
            std::snprintf(buf, sizeof(buf), "LOD%zu only comes in at %s, where the mesh is %.1f px across: LOD%zu (%u triangles) "
                          "is all anyone sees. Its deviation %.3f m is %.0f %% of the radius.", i,
                          Metres(row.distance).c_str(), static_cast<double>(row.meshPx), i - 1, prev.tris,
                          static_cast<double>(row.deviation), radius > 0.0f ? 100.0 * row.deviation / radius : 0.0);
            std::snprintf(fixBuf, sizeof(fixBuf), "One part decides it (a thin piece the level loses whole): tick 'Drop small "
                          "disconnected parts' so it goes at an earlier level, or give the asset a coarser PixelError "
                          "(\"lodGroup\" / \"lodPixelError\", now %.1f) -- the pop is then that many pixels.",
                          static_cast<double>(pixelError));
            add(buf, fixBuf);
        }
        else if (density > 2.0f && prev.tris >= 1000u)
        {
            std::snprintf(buf, sizeof(buf), "LOD%zu still draws %u triangles at %s, %.0f px across (%.1f a pixel), until LOD%zu.",
                          i - 1, prev.tris, Metres(row.distance).c_str(), static_cast<double>(row.meshPx),
                          static_cast<double>(density), i);
            if (radius > 0.0f && row.deviation > 0.05f * radius)
            {
                std::snprintf(fixBuf, sizeof(fixBuf), "LOD%zu's deviation (%.3f m, %.0f %% of the radius) is one collapse the "
                              "simplifier took for want of a budget: cap it -- 'Error budget' (lodErrorScale) 0.1, a tenth of the "
                              "mesh's size -- and LOD%zu keeps that part and comes in nearer. Then Save.", i,
                              static_cast<double>(row.deviation), 100.0 * row.deviation / radius, i);
            }
            else
            {
                std::snprintf(fixBuf, sizeof(fixBuf), "The mesh is denser than its levels thin it: lower 'Triangle target' "
                              "(lodRatioScale, now %.2f) so every level keeps fewer, or give it a coarser PixelError (\"lodGroup\" "
                              "/ \"lodPixelError\", now %.1f).", static_cast<double>(ratioScale), static_cast<double>(pixelError));
            }
            add(buf, fixBuf);
        }
        if (authored && row.popPx > 2.0f * std::max(pixelError, 1.0f))
        {
            std::snprintf(buf, sizeof(buf), "LOD%zu pops %.1f px where it comes in (%s): the authored screen size is larger than "
                          "its deviation allows at PixelError %.1f.", i, static_cast<double>(row.popPx),
                          Metres(row.distance).c_str(), static_cast<double>(pixelError));
            add(buf, "Lower LOD" + std::to_string(i) + "'s screen size, or untick 'Author LOD screen sizes' for UE's estimate.");
        }
        if (i >= 2 && r.rows[i - 1].distance > 1e-3f && r.rows[i - 1].distance < FLT_MAX * 0.5f &&
            row.distance / r.rows[i - 1].distance > 25.0f)
        {
            std::snprintf(buf, sizeof(buf), "A gap: LOD%zu holds from %s to %s (x%.0f): LOD%zu is far coarser than LOD%zu.", i - 1,
                          Metres(r.rows[i - 1].distance).c_str(), Metres(row.distance).c_str(),
                          static_cast<double>(row.distance / r.rows[i - 1].distance), i, i - 1);
            add(buf, (i == 3 && Lod3AggressiveOf(lod3Stated, slotFoliage))
                         ? "Untick 'Aggressive LOD3' or raise 'LOD3 triangle target' (lod3RatioScale)."
                         : "One collapse the simplifier took for want of a budget: cap it -- 'Error budget' (lodErrorScale) 0.1, a "
                           "tenth of the mesh's size -- so LOD" + std::to_string(i) + " keeps that part. Then Save.");
        }
        if (row.deviation <= 0.0f && row.tris < r.rows[0].tris && !authored)
        {
            std::snprintf(buf, sizeof(buf), "LOD%zu has no measured deviation: its screen size falls back to 0.75^%zu.", i, i);
            add(buf, "Save re-bakes the mesh, which measures it.");
        }
    }
    return r;
}

// Every "windFoliage" weight > 0 -- the runtime's foliage test (SceneObjectFactory).
inline bool ManifestHasFoliage(const nlohmann::json& doc)
{
    const auto it = doc.find("windFoliage");
    if (it == doc.end() || !it->is_array()) { return false; }
    for (const nlohmann::json& v : *it) { if (v.is_number() && v.get<float>() > 0.0f) { return true; } }
    return false;
}
} // namespace lodreport
