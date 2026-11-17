#include "app/settings/UserSettings.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "app/camera/Camera.h"
#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"
#include "rendering/core/PhotographicSettings.h"
#include "third_party/json/json.hpp"

namespace settings
{
namespace
{
    using json = nlohmann::json;

    // The whole file as an object; empty when it is missing. `damaged`: it exists and is not a JSON object.
    json ReadAll(bool& damaged)
    {
        damaged = false;
        std::ifstream in(UserSettingsPath(), std::ios::binary); // vfs-ok: the user's settings file
        if (!in) { return json::object(); }
        json root = json::parse(in, nullptr, false, /*ignore_comments=*/true);
        if (!root.is_object())
        {
            damaged = true;
            return json::object();
        }
        return root;
    }

    float Finite(float v, float fallback) { return std::isfinite(v) ? v : fallback; }
}

std::filesystem::path UserSettingsPath()
{
    return vfs::WritablePath(vfs::Writable::Settings, kUserSettingsFile);
}

bool UserSettingsDamaged()
{
    bool damaged = false;
    ReadAll(damaged);
    if (damaged)
    {
        LOG_WARNING_ONCE(logging::LogCategory::App, "settings: {} is not a JSON object -- its sections read as missing, "
                         "and the next save writes a good file", vfs::Narrow(UserSettingsPath().wstring()));
    }
    return damaged;
}

std::string ReadUserSection(const std::string& key)
{
    bool damaged = false;
    const json root = ReadAll(damaged);
    if (damaged) { UserSettingsDamaged(); }
    if (!root.contains(key)) { return {}; }
    return root[key].dump();
}

bool WriteUserSections(const std::vector<std::pair<std::string, std::string>>& sections, std::string& why)
{
    const std::filesystem::path file = UserSettingsPath();
    bool damaged = false;
    json root = ReadAll(damaged); // the other sections are kept; a damaged file is replaced by a good one
    for (const auto& [key, text] : sections)
    {
        if (text.empty())
        {
            root.erase(key);
            continue;
        }
        json value = json::parse(text, nullptr, false);
        if (value.is_discarded())
        {
            why = "section " + key + " is not JSON";
            return false;
        }
        root[key] = std::move(value);
    }
    root["version"] = kUserSettingsVersion;
    std::error_code ec;
    if (file.has_parent_path()) { std::filesystem::create_directories(file.parent_path(), ec); }
    std::filesystem::path temp = file;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc); // vfs-ok: the user's settings file
        if (!out)
        {
            why = "cannot write " + vfs::Narrow(temp.wstring());
            return false;
        }
        out << root.dump(2) << "\n";
        out.flush();
        if (!out.good())
        {
            why = "write failed";
            out.close();
            std::filesystem::remove(temp, ec);
            return false; // the good file stays
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec)
    {
        why = "cannot replace " + vfs::Narrow(file.wstring()) + ": " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

ImageSettings LoadImageSettings()
{
    ImageSettings s = CurrentImageSettings();
    const json j = json::parse(ReadUserSection("image"), nullptr, false);
    if (!j.is_object()) { return s; }
    if (j.contains("brightnessEv") && j["brightnessEv"].is_number())
    {
        s.brightnessEv = std::clamp(Finite(j["brightnessEv"].get<float>(), 0.0f), kBrightnessMinEv, kBrightnessMaxEv);
    }
    if (j.contains("fovDeg") && j["fovDeg"].is_number())
    {
        const float fov = Finite(j["fovDeg"].get<float>(), 0.0f);
        s.fovDeg = fov <= 0.0f ? 0.0f : std::clamp(fov, kFovMinDeg, kFovMaxDeg);
    }
    return s;
}

ControlsSettings LoadControlsSettings()
{
    ControlsSettings s = CurrentControlsSettings();
    const json j = json::parse(ReadUserSection("controls"), nullptr, false);
    if (!j.is_object()) { return s; }
    if (j.contains("sensitivity") && j["sensitivity"].is_number())
    {
        s.sensitivity = std::clamp(Finite(j["sensitivity"].get<float>(), 1.0f), kSensitivityMin, kSensitivityMax);
    }
    if (j.contains("invertY") && j["invertY"].is_boolean()) { s.invertY = j["invertY"].get<bool>(); }
    return s;
}

ImageSettings CurrentImageSettings()
{
    return ImageSettings{ render::g_playerExposureEv_game, g_cameraPlayerFovDeg };
}

ControlsSettings CurrentControlsSettings()
{
    return ControlsSettings{ g_cameraLookSensitivity, g_cameraInvertY };
}

void ApplyImageSettings(const ImageSettings& s)
{
    render::g_playerExposureEv_game = std::clamp(s.brightnessEv, kBrightnessMinEv, kBrightnessMaxEv);
    g_cameraPlayerFovDeg = s.fovDeg <= 0.0f ? 0.0f : std::clamp(s.fovDeg, kFovMinDeg, kFovMaxDeg);
}

void ApplyControlsSettings(const ControlsSettings& s)
{
    g_cameraLookSensitivity = std::clamp(s.sensitivity, kSensitivityMin, kSensitivityMax);
    g_cameraInvertY = s.invertY;
}

bool SaveImageAndControls(const ImageSettings& image, const ControlsSettings& controls, std::string& why)
{
    const json i = { { "brightnessEv", image.brightnessEv }, { "fovDeg", image.fovDeg } };
    const json c = { { "sensitivity", controls.sensitivity }, { "invertY", controls.invertY } };
    return WriteUserSections({ { "image", i.dump() }, { "controls", c.dump() } }, why);
}

bool SaveControlsSettings(const ControlsSettings& controls, std::string& why)
{
    const json c = { { "sensitivity", controls.sensitivity }, { "invertY", controls.invertY } };
    return WriteUserSection("controls", c.dump(), why);
}

void ApplySavedImageAndControls()
{
    const bool hasImage = !ReadUserSection("image").empty();
    const bool hasControls = !ReadUserSection("controls").empty();
    if (!hasImage && !hasControls) { return; }
    const ImageSettings image = LoadImageSettings();
    const ControlsSettings controls = LoadControlsSettings();
    ApplyImageSettings(image);
    ApplyControlsSettings(controls);
    LOG_INFO(logging::LogCategory::App, "settings: the player's image -- brightness {:+.1f} EV, field of view {}; controls -- "
             "sensitivity x{:.2f}, invert Y {}", image.brightnessEv,
             image.fovDeg > 0.0f ? std::to_string(static_cast<int>(std::lround(image.fovDeg))) + " deg" : std::string("the level's"),
             controls.sensitivity, controls.invertY);
}
}
