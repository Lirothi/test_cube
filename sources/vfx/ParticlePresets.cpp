#include "vfx/ParticlePresets.h"

#include <fstream>
#include <sstream>
#include <string>

namespace vfx
{
namespace
{
    void ReadRange(const nlohmann::json& j, const char* key, float& lo, float& hi)
    {
        const auto it = j.find(key);
        if (it != j.end() && it->is_array() && it->size() >= 2)
        {
            lo = (*it)[0].get<float>();
            hi = (*it)[1].get<float>();
        }
    }

    Math::float3 ReadF3(const nlohmann::json& j, const char* key, const Math::float3& def)
    {
        const auto it = j.find(key);
        if (it != j.end() && it->is_array() && it->size() >= 3)
        {
            return Math::float3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
        }
        return def;
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
    d.maxParticles = j.value("maxParticles", d.maxParticles);
    d.spawnRate = j.value("spawnRate", d.spawnRate);
    ReadRange(j, "lifetime", d.lifetimeMin, d.lifetimeMax);
    d.coneDir = ReadF3(j, "coneDir", d.coneDir);
    d.coneAngleDeg = j.value("coneAngleDeg", d.coneAngleDeg);
    ReadRange(j, "speed", d.speedMin, d.speedMax);
    d.gravity = j.value("gravity", d.gravity);
    d.drag = j.value("drag", d.drag);
    d.windInfluence = j.value("windInfluence", d.windInfluence);
    d.seed = j.value("seed", d.seed);
    ReadRange(j, "rotation", d.rotMin, d.rotMax);
    ReadRange(j, "spin", d.spinMin, d.spinMax);

    // --- rendering ---
    ReadRange(j, "size", d.sizeStart, d.sizeEnd);
    d.texture = j.value("texture", d.texture);
    d.additive = j.value("additive", d.additive);
    d.softFade = j.value("softFade", d.softFade);
    d.sortParticles = j.value("sort", d.sortParticles);
    d.localSpace = j.value("localSpace", d.localSpace);
    d.flipbookCols = j.value("flipCols", d.flipbookCols);
    d.flipbookRows = j.value("flipRows", d.flipbookRows);
    d.flipbookFps = j.value("flipFps", d.flipbookFps);
    d.flipbookRandomStart = j.value("flipRandomStart", d.flipbookRandomStart);
    d.frameBlend = j.value("frameBlend", d.frameBlend);
    if (const auto it = j.find("colorKeys"); it != j.end() && it->is_array())
    {
        for (size_t k = 0; k < 4 && k < it->size(); ++k)
        {
            const auto& key = (*it)[k];
            if (key.is_array() && key.size() >= 4)
            {
                d.colorKeys[k] = Math::float4(key[0].get<float>(), key[1].get<float>(),
                                              key[2].get<float>(), key[3].get<float>());
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
        std::ifstream f(it->get<std::string>());
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
