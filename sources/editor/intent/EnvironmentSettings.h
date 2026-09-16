#pragma once
#if WITH_EDITOR

#include <string>
#include <string_view>
#include <vector>

#include "editor/scene/EditorSceneDocument.h"

// The level's environment knobs -- fog, sun, wind, ocean, exposure, colour grading --
// enumerated FROM THE LEVEL rather than from a hand-kept table.
//
// The engine already has ~270 named settings behind `--set` (`ApplySweepValue` in
// app/App.cpp), and reaching for those was the obvious move. It is the wrong one: `--set`
// writes straight into runtime state, so an edit made that way is invisible to the
// document, absent from the undo stack, and gone the next time the level loads. The
// editor's own path -- an environment entity's `properties` JSON plus
// EditEnvironmentCommand -- is undoable and saved, and the entity's properties ARE the
// level's section verbatim. So walking them yields the knobs that actually exist in THIS
// level, with their CURRENT values, and every edit lands where edits belong.
namespace envsettings
{
    enum class ValueKind
    {
        Number,
        Bool,
        String,
        Vec3,
    };

    const char* KindName(ValueKind kind);

    struct Setting
    {
        EditorObjectId owner;     // the environment entity that holds it
        std::string path;         // "wind.strength" -- what a phrase names
        std::string subPath;      // "strength" -- inside that entity's properties
        ValueKind kind = ValueKind::Number;
        nlohmann::json current;
    };

    // Every leaf under every environment entity, depth-limited. Values that are neither a
    // number, a boolean, a string nor a three-number vector are skipped: there is no
    // sensible single-value edit for them, and listing them would only invite one.
    std::vector<Setting> Enumerate(const EditorSceneDocument& document);

    // Exact match first, then a unique case-insensitive suffix match, so "strength" finds
    // "wind.strength" while an ambiguous stem finds nothing rather than the wrong one.
    const Setting* Find(const std::vector<Setting>& settings, std::string_view path);

    // Writes `value` at `subPath` inside a copy of an entity's properties, creating
    // intermediate objects as needed. Returns false if the path runs through a non-object.
    bool Write(nlohmann::json& properties, const std::string& subPath, const nlohmann::json& value);

    // "1.0", "true", "[0.3, -0.8, 0.2]" -- for the preview line and the prompt listing.
    std::string ToText(const nlohmann::json& value);
}

#endif // WITH_EDITOR
