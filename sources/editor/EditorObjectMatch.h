#pragma once
#if WITH_EDITOR

#include <cstdio>
#include <string>

#include "core/StringMatch.h"
#include "editor/scene/EditorSceneDocument.h"

// The outliner's search predicate, lifted out of SceneOutlinerPanel so the
// natural-language command bar resolves a selector through EXACTLY the same path
// the search box does (docs/editor_llm_plan.md, E5). Two copies of "what does this
// needle match" would drift, and the drift would show up as a command that hits a
// different set of objects than the outliner just previewed.
namespace editormatch
{
    // Object fields that carry an asset name. A needle matching one of these is how
    // "coconut_palm" finds the palms: their NAME may be anything ("Palm_017"), but
    // the mesh they were built from is the same string for all of them.
    constexpr const char* kSearchPropertyKeys[] = {
        // "mesh" FIRST because it is what current levels actually use: a .mesh.json
        // reference carries the geometry, layout, shader and material defaults, and the
        // atoll's hundred palms are all `"mesh": "models/date_palm.mesh.json"`. It was
        // missing from this list, which meant the outliner's search box -- and anything
        // else sharing this predicate -- could not find them by asset at all. Typing
        // "date_palm" into the search box returned nothing on a level made of date palms.
        "mesh",
        "model",
        "material",
        "texture",
        "preset",
        "shader",
        "inputLayout",
    };

    // True when `needle` (any case) appears in the object's name, type, decimal id,
    // or one of the asset-name properties above. An empty needle matches everything,
    // which is what the search box wants and what the command bar must never pass.
    inline bool MatchesSearch(const EditorObject& object, const std::string& needle)
    {
        if (needle.empty())
        {
            return true;
        }

        if (textmatch::ContainsCaseInsensitive(object.name, needle) ||
            textmatch::ContainsCaseInsensitive(object.type, needle))
        {
            return true;
        }

        char idText[32];
        std::snprintf(idText, sizeof(idText), "%llu",
            static_cast<unsigned long long>(object.id.value));
        if (textmatch::ContainsCaseInsensitive(idText, needle))
        {
            return true;
        }

        if (object.properties.is_object())
        {
            for (const char* key : kSearchPropertyKeys)
            {
                const auto it = object.properties.find(key);
                if (it != object.properties.end() && it->is_string() &&
                    textmatch::ContainsCaseInsensitive(it->get<std::string>(), needle))
                {
                    return true;
                }
            }
        }

        return false;
    }
}

#endif // WITH_EDITOR
