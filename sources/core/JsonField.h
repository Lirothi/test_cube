#pragma once

#include <string>
#include <type_traits>
#include <vector>

#include "third_party/json/json.hpp"

// One field of a JSON object as its type -- or the fallback. For a file somebody may have edited by hand, or that another
// program writes (a build profile, the build history, a server's reply, the player's settings).
//
// nlohmann's own `object.value(key, fallback)` returns the fallback only when the key is ABSENT. A key that is there with
// another type -- `"version": 1.0` where a string is read, `"zip": null`, `"levels": "a.json"` where an array is -- throws
// type_error 302, and so does value() on anything that is not an object (306). Out of a frame or a thread of its own
// that is std::terminate: the editor went down with the level in it when the Build Distribution window walked a history
// that held the script's own `"zip": null` (docs/architecture_review_plan.md 10.14). Where a wrong type must be an ERROR
// the reader names (a material, a level), keep value() and catch nlohmann::json::exception around the read instead.
namespace jsonfield
{
    // bool: a JSON boolean. An integer type: a JSON integer (signed or unsigned; 3.0 is not one). A floating type: any
    // JSON number. std::string: a JSON string. nlohmann::json: the field as it is, whatever it holds.
    template <class T>
    [[nodiscard]] T Get(const nlohmann::json& object, const char* key, T fallback)
    {
        if (!object.is_object()) { return fallback; }
        const auto it = object.find(key);
        if (it == object.end()) { return fallback; }
        if constexpr (std::is_same_v<T, bool>)
        {
            return it->is_boolean() ? it->template get<bool>() : fallback;
        }
        else if constexpr (std::is_integral_v<T>)
        {
            return it->is_number_integer() ? it->template get<T>() : fallback;
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            return it->is_number() ? it->template get<T>() : fallback;
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            return it->is_string() ? it->template get<std::string>() : fallback;
        }
        else
        {
            static_assert(std::is_same_v<T, nlohmann::json>, "jsonfield::Get: bool, an arithmetic type, std::string, "
                                                             "std::vector<std::string> or nlohmann::json");
            return *it;
        }
    }

    // A string literal as the fallback reads a string.
    [[nodiscard]] inline std::string Get(const nlohmann::json& object, const char* key, const char* fallback)
    {
        return Get<std::string>(object, key, std::string(fallback ? fallback : ""));
    }

    // An array of strings: its string elements, in order (another element is left out); the fallback when the field is
    // absent or is not an array.
    [[nodiscard]] inline std::vector<std::string> Get(const nlohmann::json& object, const char* key,
                                                      std::vector<std::string> fallback)
    {
        if (!object.is_object()) { return fallback; }
        const auto it = object.find(key);
        if (it == object.end() || !it->is_array()) { return fallback; }
        std::vector<std::string> out;
        out.reserve(it->size());
        for (const nlohmann::json& element : *it)
        {
            if (element.is_string()) { out.push_back(element.get<std::string>()); }
        }
        return out;
    }
} // namespace jsonfield
