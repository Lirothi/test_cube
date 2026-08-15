#pragma once

#include <algorithm>
#include <utility>

#include "rendering/core/PhotographicSettings.h"

#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

// JSON round-trip for the photographic camera settings, kept in its own header for the same reason
// OceanRenderConfigJson is: Scene.h must not drag nlohmann into every translation unit that only
// wants the plain struct.
//
// Both the boot loader (JsonLevel) and the editor's live-edit path (EnvironmentRuntime) go through
// ApplyOverrides, so the defaults exist in exactly one place -- PhotographicSettings.h -- and a
// level that omits a field cannot disagree with a level that omits the whole section.
namespace render::PhotographicSettingsJson
{

inline void ApplyOverrides(const nlohmann::json& j, CameraExposureSettings& out)
{
    if (!j.is_object())
    {
        return;
    }
    out.enabled        = j.value("enabled",        out.enabled);
    out.autoExposure   = j.value("autoExposure",   out.autoExposure);
    out.compensationEv = j.value("compensationEv", out.compensationEv);
    out.minEv100       = j.value("minEv100",       out.minEv100);
    out.maxEv100       = j.value("maxEv100",       out.maxEv100);
    out.lowPercentile  = j.value("lowPercentile",  out.lowPercentile);
    out.highPercentile = j.value("highPercentile", out.highPercentile);
    out.speedUp        = j.value("speedUp",        out.speedUp);
    out.speedDown      = j.value("speedDown",      out.speedDown);
    out.manualEv100    = j.value("manualEv100",    out.manualEv100);

    // Clamp at the settings boundary only (plan section 6.2: no hidden clamps scattered through
    // shaders). An inverted or degenerate authored range would otherwise reach the P2 solve.
    if (out.maxEv100 < out.minEv100)
    {
        std::swap(out.minEv100, out.maxEv100);
    }
    out.lowPercentile  = std::clamp(out.lowPercentile,  0.0f, 1.0f);
    out.highPercentile = std::clamp(out.highPercentile, 0.0f, 1.0f);
    if (out.highPercentile < out.lowPercentile)
    {
        std::swap(out.lowPercentile, out.highPercentile);
    }
    out.speedUp   = std::max(out.speedUp,   0.0f);
    out.speedDown = std::max(out.speedDown, 0.0f);
}

inline nlohmann::json ToJson(const CameraExposureSettings& s)
{
    return nlohmann::json{
        { "enabled",        s.enabled },
        { "autoExposure",   s.autoExposure },
        { "compensationEv", s.compensationEv },
        { "minEv100",       s.minEv100 },
        { "maxEv100",       s.maxEv100 },
        { "lowPercentile",  s.lowPercentile },
        { "highPercentile", s.highPercentile },
        { "speedUp",        s.speedUp },
        { "speedDown",      s.speedDown },
        { "manualEv100",    s.manualEv100 },
    };
}

} // namespace render::PhotographicSettingsJson
