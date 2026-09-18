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
        // The GROUP a designer put it in. Not an asset name like the rest, and it earns its
        // place here for the same reason they did: it is a word somebody will type into the
        // search box, and typing it must find the same set the command bar finds. Naming a
        // group in a filter is how "удали северную рощу" works at all.
        "group",
    };

    // What to CALL a group of these objects, in priority order. A subset of the list above,
    // and a different question from it: matching tries every key, naming picks one.
    // "texture" and "shader" are searchable and make poor labels; "group" is searchable and
    // is not the object's identity at all.
    //
    // FOUR COPIES OF THIS EXISTED -- the outliner's preview label, the prompt's filter
    // vocabulary, the query layer's report and the thinning preview -- in a file whose own
    // comment warns that two would drift. They agreed by luck: the query layer's copy also
    // read a key called "asset" that no other list knew, so an object carrying one would
    // have been reported to the model under a name the prompt never offered and the search
    // predicate could not find.
    constexpr const char* kAssetNameKeys[] = {
        "mesh",
        "model",
        "preset",
        "material",
    };

    // The asset this object was built from, or its type, or its name -- the first of those
    // that says anything. This is the string a designer means by "the palms".
    inline std::string AssetLabel(const EditorObject& object)
    {
        if (object.properties.is_object())
        {
            for (const char* key : kAssetNameKeys)
            {
                const auto it = object.properties.find(key);
                if (it != object.properties.end() && it->is_string() &&
                    !it->get<std::string>().empty())
                {
                    return it->get<std::string>();
                }
            }
        }
        return object.type.empty() ? object.name : object.type;
    }

    // The same thing, as a person would write it: "models/coconut_palm.mesh.json" becomes
    // "Coconut Palm". AssetLabel returns the PATH, which is right for matching and filtering
    // -- it is the identity -- and wrong the moment it is used as a label somebody reads.
    // Asked to give the level sensible names, the first version named 107 objects
    // "models/coconut_palm.mesh.json 001", which is the opposite of what was asked for.
    inline std::string PrettyAssetLabel(const EditorObject& object)
    {
        std::string label = AssetLabel(object);
        const std::size_t slash = label.find_last_of("/\\");
        if (slash != std::string::npos)
        {
            label.erase(0, slash + 1);
        }
        // Everything from the first dot: ".mesh.json", ".json", ".mesh" all go the same way.
        const std::size_t dot = label.find('.');
        if (dot != std::string::npos && dot > 0)
        {
            label.erase(dot);
        }
        bool startOfWord = true;
        for (char& ch : label)
        {
            if (ch == '_' || ch == '-')
            {
                ch = ' ';
                startOfWord = true;
                continue;
            }
            if (startOfWord && ch >= 'a' && ch <= 'z')
            {
                ch = static_cast<char>(ch - 'a' + 'A');
            }
            startOfWord = (ch == ' ');
        }
        return label;
    }

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
