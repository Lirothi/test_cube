#include "app/levels/LevelMigration.h"

#include "app/scene/TimeOfDaySettingsJson.h"

#include <utility>

using nlohmann::json;

namespace levelmigrate
{
json Migrate(json level, bool convertDirectionalLight, Report* report)
{
    json out = std::move(level);
    if (!out.is_object())
    {
        return out;
    }
    const auto note = [report](std::string what) { if (report) { report->moved.push_back(std::move(what)); } };

    // The look groups under "postProcess". Where both readers looked, in this order: postProcess/<name>, then the
    // top-level <name>; the height fog under its name first, then under its old one.
    const auto folded = out.find("postProcess");
    const bool havePostProcess = folded != out.end() && folded->is_object();
    json postProcess = havePostProcess ? *folded : json::object();
    struct Group { const char* key; const char* legacyKey; };
    static constexpr Group kGroups[] = {
        { "cameraExposure", nullptr }, { "colorPipeline", nullptr }, { "gtao", nullptr },
        { "heightFog", "atmosphere" }, { "bloom", nullptr },
    };
    bool any = havePostProcess;
    for (const Group& g : kGroups)
    {
        const char* const names[] = { g.key, g.legacyKey };
        const json* found = nullptr;
        std::string from;
        for (const char* name : names)
        {
            if (name == nullptr) { continue; }
            if (havePostProcess)
            {
                const auto sub = folded->find(name);
                if (sub != folded->end() && sub->is_object()) { found = &*sub; from = std::string("postProcess/") + name; break; }
            }
            const auto top = out.find(name);
            if (top != out.end() && top->is_object()) { found = &*top; from = name; break; }
        }
        if (found)
        {
            postProcess[g.key] = *found; // `found` points into `out`, which is not edited until after this loop
            any = true;
            if (from != std::string("postProcess/") + g.key) { note(from + " -> postProcess/" + g.key); }
        }
        else
        {
            postProcess.erase(g.key); // not an object: neither reader took it
        }
        if (g.legacyKey) { postProcess.erase(g.legacyKey); }
    }
    for (const Group& g : kGroups)
    {
        out.erase(g.key);
        if (g.legacyKey) { out.erase(g.legacyKey); }
    }

    // P3B: the local-exposure keys moved from the colour pipeline to the camera (an exposure operation, not a grade).
    // A key the camera does not own yet is copied onto it -- one the camera owns wins, so an edit made on the camera is
    // never stomped back by the stale copy. The stale copy stays where it is (the colour pipeline reads none of these
    // keys): a level's saved bytes do not change for it. Only numbers, as the lift at apply time took them.
    if (const auto colorPipeline = postProcess.find("colorPipeline");
        colorPipeline != postProcess.end() && colorPipeline->is_object())
    {
        static constexpr const char* kLocalExposure[] = { "localHighlightContrast", "localShadowContrast",
            "localDetailStrength", "localHighlightThreshold", "localShadowThreshold" };
        for (const char* key : kLocalExposure)
        {
            const auto value = colorPipeline->find(key);
            if (value == colorPipeline->end() || !value->is_number()) { continue; }
            const auto camera = postProcess.find("cameraExposure");
            if (camera != postProcess.end() && camera->contains(key)) { continue; }
            const json lifted = *value; // `postProcess["cameraExposure"]` below may move what `value` points into
            postProcess["cameraExposure"][key] = lifted;
            note(std::string("postProcess/colorPipeline/") + key + " -> postProcess/cameraExposure/" + key);
        }
    }
    if (any)
    {
        out["postProcess"] = std::move(postProcess);
    }

    // The old sun. A level with its time of day keeps it (the old section beside it lit nothing).
    const auto timeOfDay = out.find("timeOfDay");
    const auto sun = out.find("directionalLight");
    if (convertDirectionalLight && sun != out.end())
    {
        if (!(timeOfDay != out.end() && timeOfDay->is_object()) && sun->is_object())
        {
            out["timeOfDay"] = TimeOfDaySettingsJson::FromDirectionalLight(*sun);
            if (report) { report->sunConverted = true; }
            note("directionalLight -> timeOfDay");
        }
        out.erase("directionalLight");
    }
    return out;
}
} // namespace levelmigrate
