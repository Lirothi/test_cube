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
// the reader names (a material, a level), keep value() and catch nlohmann::json::exception around the read instead --
// and a reader that both the level loader and the editor call reads through Value / As below (R9.6).
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

    // Architecture plan R9.6: a LEVEL's field -- an object's key, an environment section's, a mesh.json's or a material's
    // as the editor reads it. value()'s own conversions exactly (an int from 3.7 is 3, a float or an int from `true` is 1,
    // a double from `true` throws: nlohmann's from_json), so the editor shows and applies what the level loader read --
    // but where value() / get<T>() would THROW, the fallback: the editor reads its document in a frame, where a throw is
    // std::terminate with the unsaved level in it. Except on a thread inside a StrictScope, where it throws as value()
    // always did: the level loader opens one (JsonLevel::Load), so a key of another type still stops the load with the
    // file named. Get above stays for files other programs write: its integer is an integer (3.0 is not one).
    class StrictScope
    {
    public:
        StrictScope() { ++Depth(); }
        ~StrictScope() { --Depth(); }
        StrictScope(const StrictScope&) = delete;
        StrictScope& operator=(const StrictScope&) = delete;
        [[nodiscard]] static bool Active() { return Depth() > 0; }

    private:
        static int& Depth()
        {
            static thread_local int depth = 0;
            return depth;
        }
    };

    namespace detail
    {
        // Whether nlohmann's get<T>() takes this value without throwing (from_json, third_party/json/json.hpp): a bool
        // only from a boolean; the library's own number types (double, std::int64_t, std::uint64_t) and an enum only from
        // a number; any other arithmetic type from a number or a boolean; a string only from a string.
        template <class T>
        [[nodiscard]] bool Converts(const nlohmann::json& v)
        {
            using J = nlohmann::json;
            if constexpr (std::is_same_v<T, bool>) { return v.is_boolean(); }
            else if constexpr (std::is_same_v<T, J::number_float_t> || std::is_same_v<T, J::number_integer_t> ||
                               std::is_same_v<T, J::number_unsigned_t> || std::is_enum_v<T>) { return v.is_number(); }
            else if constexpr (std::is_arithmetic_v<T>) { return v.is_number() || v.is_boolean(); }
            else if constexpr (std::is_same_v<T, std::string>) { return v.is_string(); }
            else
            {
                static_assert(std::is_same_v<T, J>, "jsonfield::Value / As: bool, an arithmetic type, an enum, "
                                                    "std::string or nlohmann::json");
                return true;
            }
        }
    } // namespace detail

    // An element -- an array's member, a value already found: get<T>(), or the fallback where get<T>() would throw.
    template <class T>
    [[nodiscard]] T As(const nlohmann::json& value, T fallback)
    {
        if (StrictScope::Active() || detail::Converts<T>(value)) { return value.template get<T>(); }
        return fallback;
    }

    // A field: object.value(key, fallback), or the fallback where value() would throw (not an object, another type).
    template <class T>
    [[nodiscard]] T Value(const nlohmann::json& object, const char* key, T fallback)
    {
        if (StrictScope::Active()) { return object.value(key, fallback); }
        if (!object.is_object()) { return fallback; }
        const auto it = object.find(key);
        if (it == object.end()) { return fallback; }
        return detail::Converts<T>(*it) ? it->template get<T>() : fallback;
    }

    // A string literal as the fallback reads a string, as value() does.
    [[nodiscard]] inline std::string Value(const nlohmann::json& object, const char* key, const char* fallback)
    {
        return Value<std::string>(object, key, std::string(fallback ? fallback : ""));
    }
} // namespace jsonfield
