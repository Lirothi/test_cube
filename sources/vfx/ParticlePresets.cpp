#include "vfx/ParticlePresets.h"
#include "core/JsonField.h"
#include "core/vfs/Vfs.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

namespace vfx
{
namespace
{
    // Every read takes the field only when it is of its type (jsonfield::Get; docs/architecture_review_plan.md M6): a
    // preset or a level is a file a person edits, and nlohmann's value() and get<float>() threw on a field of another
    // type -- out of the level load, or out of the frame for an Inspector edit.

    // The first `n` elements of an array, when it has them and they are all numbers.
    bool Numbers(const nlohmann::json& a, size_t n, float* out)
    {
        if (!a.is_array() || a.size() < n) { return false; }
        for (size_t i = 0; i < n; ++i)
        {
            if (!a[i].is_number()) { return false; }
        }
        for (size_t i = 0; i < n; ++i) { out[i] = a[i].get<float>(); }
        return true;
    }
    bool Numbers(const nlohmann::json& j, const char* key, size_t n, float* out)
    {
        const auto it = j.find(key);
        return it != j.end() && Numbers(*it, n, out);
    }

    void ReadRange(const nlohmann::json& j, const char* key, float& lo, float& hi)
    {
        float v[2];
        if (Numbers(j, key, 2, v))
        {
            lo = v[0];
            hi = v[1];
        }
    }

    Math::float3 ReadF3(const nlohmann::json& j, const char* key, const Math::float3& def)
    {
        float v[3];
        return Numbers(j, key, 3, v) ? Math::float3(v[0], v[1], v[2]) : def;
    }

    // A count: any JSON number (3.0 included, as value() took it), clamped into [lo, hi]; another type = unchanged.
    uint32_t ReadCount(const nlohmann::json& j, const char* key, uint32_t current, uint32_t lo, uint32_t hi)
    {
        const double v = jsonfield::Get<double>(j, key, static_cast<double>(current));
        if (!std::isfinite(v)) { return current; }
        return static_cast<uint32_t>(std::clamp(v, static_cast<double>(lo), static_cast<double>(hi)));
    }

    bool HasSort(const nlohmann::json& j)
    {
        return j.is_object() && j.contains("sort");
    }
}

void ApplyEmitterJson(const nlohmann::json& j, EmitterDesc& d)
{
    if (!j.is_object())
    {
        return;
    }

    // --- sim ---
    d.maxParticles = ReadCount(j, "maxParticles", d.maxParticles, 0u, kMaxParticlesPerEmitter);
    d.spawnRate = jsonfield::Get(j, "spawnRate", d.spawnRate);
    ReadRange(j, "lifetime", d.lifetimeMin, d.lifetimeMax);
    d.coneDir = ReadF3(j, "coneDir", d.coneDir);
    d.coneAngleDeg = jsonfield::Get(j, "coneAngleDeg", d.coneAngleDeg);
    ReadRange(j, "speed", d.speedMin, d.speedMax);
    d.gravity = jsonfield::Get(j, "gravity", d.gravity);
    d.drag = jsonfield::Get(j, "drag", d.drag);
    d.windInfluence = jsonfield::Get(j, "windInfluence", d.windInfluence);
    d.seed = ReadCount(j, "seed", d.seed, 0u, UINT32_MAX);
    ReadRange(j, "rotation", d.rotMin, d.rotMax);
    ReadRange(j, "spin", d.spinMin, d.spinMax);

    // --- rendering ---
    ReadRange(j, "size", d.sizeStart, d.sizeEnd);
    d.texture = jsonfield::Get(j, "texture", d.texture);
    d.additive = jsonfield::Get(j, "additive", d.additive);
    d.softFade = jsonfield::Get(j, "softFade", d.softFade);
    d.luminanceCdM2 = jsonfield::Get(j, "luminanceCdM2", d.luminanceCdM2); // P16.7
    d.sortParticles = jsonfield::Get(j, "sort", d.sortParticles);
    d.localSpace = jsonfield::Get(j, "localSpace", d.localSpace);
    d.flipbookCols = ReadCount(j, "flipCols", d.flipbookCols, 0u, UINT32_MAX);
    d.flipbookRows = ReadCount(j, "flipRows", d.flipbookRows, 0u, UINT32_MAX);
    d.flipbookFps = jsonfield::Get(j, "flipFps", d.flipbookFps);
    d.flipbookRandomStart = jsonfield::Get(j, "flipRandomStart", d.flipbookRandomStart);
    d.frameBlend = jsonfield::Get(j, "frameBlend", d.frameBlend);
    d.lit = jsonfield::Get(j, "lit", d.lit);
    d.forwardScatter = jsonfield::Get(j, "forwardScatter", d.forwardScatter);
    d.phaseG = jsonfield::Get(j, "phaseG", d.phaseG);
    d.litFlat = jsonfield::Get(j, "litFlat", d.litFlat);
    d.velocityStretch = jsonfield::Get(j, "velocityStretch", d.velocityStretch);
    d.lineOffset = ReadF3(j, "lineOffset", d.lineOffset);
    d.killBelowY = jsonfield::Get(j, "killBelowY", d.killBelowY);
    d.collideHulls = jsonfield::Get(j, "collideHulls", d.collideHulls);
    if (const auto it = j.find("colorKeys"); it != j.end() && it->is_array())
    {
        for (size_t k = 0; k < 4 && k < it->size(); ++k)
        {
            float v[4];
            if (Numbers((*it)[k], 4, v))
            {
                d.colorKeys[k] = Math::float4(v[0], v[1], v[2], v[3]);
            }
        }
    }
}

EmitterDesc ResolveEmitterDesc(const nlohmann::json& o)
{
    EmitterDesc d;
    bool sortExplicit = false;

    // 1) preset file (relative to the working dir, like ocean/material presets).
    if (const auto it = o.find("preset"); it != o.end() && it->is_string())
    {
        vfs::InStream f(it->get<std::string>(), /*binary*/ false);
        if (f)
        {
            std::stringstream ss;
            ss << f.rdbuf();
            const nlohmann::json pj = nlohmann::json::parse(ss.str(), nullptr, false, /*ignore_comments=*/true);
            if (!pj.is_discarded())
            {
                ApplyEmitterJson(pj, d);
                sortExplicit = sortExplicit || HasSort(pj);
            }
        }
    }

    // 2) per-instance overrides, then 3) top-level inline fields (back-compat). Later wins.
    if (const auto it = o.find("overrides"); it != o.end() && it->is_object())
    {
        ApplyEmitterJson(*it, d);
        sortExplicit = sortExplicit || HasSort(*it);
    }
    ApplyEmitterJson(o, d);
    sortExplicit = sortExplicit || HasSort(o);

    // Convenience: alpha (non-additive) emitters sort back-to-front by default unless the author
    // set "sort" explicitly somewhere. Matches the pre-E3 inline behavior.
    if (!sortExplicit)
    {
        d.sortParticles = !d.additive;
    }
    return d;
}
} // namespace vfx
