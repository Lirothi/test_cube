#pragma once

#include <cmath>

#include "ocean/BuoyancyLayout.h"
#include "third_party/json/json.hpp"

// mesh.json / level object "buoyancy" block:
//   "buoyancy": { "draft": 0.32, "inertia": 1.5, "damping": 0.25, "pontoons": [[x, y, z, r], ...] }
// Every key is optional; an absent one is automatic / default (see buoyancy::Settings), and
// WriteSettings leaves defaults OUT so an untouched asset stays byte-identical.
// The per-object on/off switch is a separate key, "buoyant": true -- not this block, which
// ResolveMeshAsset folds from the asset into every placed instance.
namespace buoyancy
{
    inline bool ReadSettings(const nlohmann::json& block, Settings& out)
    {
        if (!block.is_object()) { return false; }
        if (const auto it = block.find("draft"); it != block.end() && it->is_number())
        {
            out.draft = it->get<float>();
        }
        if (const auto it = block.find("inertia"); it != block.end() && it->is_number())
        {
            out.inertia = it->get<float>();
        }
        if (const auto it = block.find("damping"); it != block.end() && it->is_number())
        {
            out.damping = it->get<float>();
        }
        out.pontoons.clear();
        if (const auto it = block.find("pontoons"); it != block.end() && it->is_array())
        {
            for (const nlohmann::json& p : *it)
            {
                if (!p.is_array() || p.size() != 4) { continue; }
                bool numeric = true;
                for (const nlohmann::json& v : p) { numeric = numeric && v.is_number(); }
                if (!numeric) { continue; }
                Pontoon pontoon;
                pontoon.center = Math::float3(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
                pontoon.radius = p[3].get<float>();
                if (pontoon.radius > 0.0f) { out.pontoons.push_back(pontoon); }
            }
        }
        return true;
    }

    inline nlohmann::json WriteSettings(const Settings& s)
    {
        const auto round = [](float v) { return std::round(v * 10000.0f) / 10000.0f; };
        nlohmann::json block = nlohmann::json::object();
        if (s.draft >= 0.0f) { block["draft"] = round(s.draft); }
        if (s.inertia != 1.0f) { block["inertia"] = round(s.inertia); }
        if (s.damping != Settings{}.damping) { block["damping"] = round(s.damping); }
        if (!s.pontoons.empty())
        {
            nlohmann::json list = nlohmann::json::array();
            for (const Pontoon& p : s.pontoons)
            {
                list.push_back({ round(p.center.x), round(p.center.y), round(p.center.z), round(p.radius) });
            }
            block["pontoons"] = std::move(list);
        }
        return block;
    }
}
