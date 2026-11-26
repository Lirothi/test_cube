#pragma once
// Architecture plan R9.1 (docs/architecture_review_plan.md, section 11.2, wave 3): the table of a section's (or an
// object's) keys -- one record per key: what the file calls it, what kind of value it is, which member of the settings
// struct holds it, the range a read clamps into, and for a panel its label, drag speed, hint and group. The level
// reader, the editor's serializer and its runtime apply walk the SAME table, so a key added once is read, written and
// applied everywhere (R9.2 onwards). A key that does not fit a kind -- a colour array with its own checks, a default
// that is not the struct's, two members behind one key -- is a Custom record with its own read and write, still a row
// of the table.
//
// Reading is the hand code's, exactly: `s.m = j.value(key, s.m)` (clamped when the record says so) -- an absent key
// leaves the member as it is, a key of another type throws nlohmann's type_error, as value() always did (R9.6 decides
// where that becomes jsonfield::Get). Writing is `j[key] = s.m`.
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/math/Math.h"
#include "third_party/json/json.hpp"

namespace schema
{
enum class Kind : std::uint8_t
{
    Bool,
    Int,
    UInt,
    Float,
    Float3, // a JSON array of three numbers; read only when it IS an array of three (else the member stays)
    FloatArray3, // a float[3] member, the same JSON as Float3
    String,
    Section, // a nested object read and written by a table of the same struct (Key::section)
    Custom,
};

template <class S>
struct Table;

template <class S>
struct Key
{
    const char* key = "";
    Kind kind = Kind::Custom;
    bool S::* boolMember = nullptr;
    int S::* intMember = nullptr;
    std::uint32_t S::* uintMember = nullptr;
    float S::* floatMember = nullptr;
    Math::float3 S::* float3Member = nullptr;
    float (S::* floatArray3Member)[3] = nullptr;
    std::string S::* stringMember = nullptr;
    // Section: the nested object's table (a static of the caller's).
    const Table<S>* section = nullptr;
    // Read clamps into [lo, hi] (Int, UInt, Float) when set; `atLeast` only raises to lo (std::max(lo, v), Float).
    bool clamped = false;
    bool atLeast = false;
    // Tolerant (Bool, Float): a value of another type is skipped as if absent -- DirectionalLightJson's Number / Flag
    // -- instead of value()'s throw.
    bool tolerant = false;
    float lo = 0.0f;
    float hi = 0.0f;
    // Custom: the key's own read (it decides what an absent or odd value does) and write.
    void (*read)(const nlohmann::json& section, S& s) = nullptr;
    void (*write)(nlohmann::json& section, const S& s) = nullptr;
    // For a panel: what it shows, how fast a drag moves it, what it does, where it sits.
    const char* label = nullptr;
    float speed = 0.0f;
    const char* hint = nullptr;
    int group = 0;

    Key Clamp(float low, float high) const
    {
        Key k = *this;
        k.clamped = true;
        k.lo = low;
        k.hi = high;
        return k;
    }
    Key Tolerant() const
    {
        Key k = *this;
        k.tolerant = true;
        return k;
    }
    Key AtLeast(float low) const
    {
        Key k = *this;
        k.atLeast = true;
        k.lo = low;
        return k;
    }
    Key Label(const char* text, float dragSpeed = 0.0f, const char* help = nullptr, int panelGroup = 0) const
    {
        Key k = *this;
        k.label = text;
        k.speed = dragSpeed;
        k.hint = help;
        k.group = panelGroup;
        return k;
    }
};

template <class S> Key<S> Field(const char* key, bool S::* m) { Key<S> k; k.key = key; k.kind = Kind::Bool; k.boolMember = m; return k; }
template <class S> Key<S> Field(const char* key, int S::* m) { Key<S> k; k.key = key; k.kind = Kind::Int; k.intMember = m; return k; }
template <class S> Key<S> Field(const char* key, std::uint32_t S::* m) { Key<S> k; k.key = key; k.kind = Kind::UInt; k.uintMember = m; return k; }
template <class S> Key<S> Field(const char* key, float S::* m) { Key<S> k; k.key = key; k.kind = Kind::Float; k.floatMember = m; return k; }
template <class S> Key<S> Field(const char* key, Math::float3 S::* m) { Key<S> k; k.key = key; k.kind = Kind::Float3; k.float3Member = m; return k; }
template <class S> Key<S> Field(const char* key, float (S::* m)[3]) { Key<S> k; k.key = key; k.kind = Kind::FloatArray3; k.floatArray3Member = m; return k; }
template <class S> Key<S> Field(const char* key, std::string S::* m) { Key<S> k; k.key = key; k.kind = Kind::String; k.stringMember = m; return k; }
template <class S> Key<S> Nested(const char* key, const Table<S>& table) { Key<S> k; k.key = key; k.kind = Kind::Section; k.section = &table; return k; }
template <class S>
Key<S> Custom(const char* key, void (*read)(const nlohmann::json&, S&), void (*write)(nlohmann::json&, const S&))
{
    Key<S> k;
    k.key = key;
    k.kind = Kind::Custom;
    k.read = read;
    k.write = write;
    return k;
}

template <class S>
void ReadKey(const Key<S>& k, const nlohmann::json& j, S& s)
{
    switch (k.kind)
    {
    case Kind::Bool:
        if (k.tolerant)
        {
            const auto it = j.find(k.key);
            if (it != j.end() && it->is_boolean()) { s.*k.boolMember = it->get<bool>(); }
        }
        else
        {
            s.*k.boolMember = j.value(k.key, s.*k.boolMember);
        }
        break;
    case Kind::Int:
    {
        const int v = j.value(k.key, s.*k.intMember);
        s.*k.intMember = k.clamped ? std::clamp(v, static_cast<int>(k.lo), static_cast<int>(k.hi)) : v;
        break;
    }
    case Kind::UInt:
    {
        const std::uint32_t v = j.value(k.key, s.*k.uintMember);
        s.*k.uintMember = k.clamped ? std::clamp(v, static_cast<std::uint32_t>(k.lo), static_cast<std::uint32_t>(k.hi)) : v;
        break;
    }
    case Kind::Float:
    {
        float v = s.*k.floatMember;
        if (k.tolerant)
        {
            const auto it = j.find(k.key);
            if (it != j.end() && it->is_number()) { v = it->get<float>(); }
        }
        else
        {
            v = j.value(k.key, v);
        }
        // The clamp applies to what the member holds afterwards, read or not -- as `std::clamp(j.value(...), ...)` did.
        s.*k.floatMember = k.clamped ? std::clamp(v, k.lo, k.hi) : (k.atLeast ? std::max(k.lo, v) : v);
        break;
    }
    case Kind::Section:
    {
        const auto it = j.find(k.key);
        if (it != j.end() && it->is_object()) { k.section->Read(*it, s); }
        break;
    }
    case Kind::FloatArray3:
    {
        const auto it = j.find(k.key);
        if (it != j.end() && it->is_array() && it->size() == 3)
        {
            for (int c = 0; c < 3; ++c) { (s.*k.floatArray3Member)[c] = (*it)[c].get<float>(); }
        }
        break;
    }
    case Kind::Float3:
    {
        const auto it = j.find(k.key);
        if (it != j.end() && it->is_array() && it->size() == 3)
        {
            s.*k.float3Member = Math::float3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
        }
        break;
    }
    case Kind::String:
        s.*k.stringMember = j.value(k.key, s.*k.stringMember);
        break;
    case Kind::Custom:
        if (k.read) { k.read(j, s); }
        break;
    }
}

template <class S>
void WriteKey(const Key<S>& k, nlohmann::json& j, const S& s)
{
    switch (k.kind)
    {
    case Kind::Bool: j[k.key] = s.*k.boolMember; break;
    case Kind::Int: j[k.key] = s.*k.intMember; break;
    case Kind::UInt: j[k.key] = s.*k.uintMember; break;
    case Kind::Float: j[k.key] = s.*k.floatMember; break;
    case Kind::Float3:
    {
        const Math::float3& v = s.*k.float3Member;
        j[k.key] = { v.x, v.y, v.z };
        break;
    }
    case Kind::FloatArray3:
    {
        const float (&a)[3] = s.*k.floatArray3Member;
        j[k.key] = { a[0], a[1], a[2] };
        break;
    }
    case Kind::String: j[k.key] = s.*k.stringMember; break;
    case Kind::Section: j[k.key] = k.section->Write(s); break;
    case Kind::Custom:
        if (k.write) { k.write(j, s); }
        break;
    }
}

template <class S>
struct Table
{
    std::vector<Key<S>> keys;
    // After every key: what the section does with the values together (a clamp of the result, a value derived from
    // two keys). Null = nothing.
    void (*finish)(const nlohmann::json& section, S& s) = nullptr;

    // Every key, in the table's order, over `s` (absent keys leave their members), then `finish`. Not an object:
    // nothing read.
    void Read(const nlohmann::json& section, S& s) const
    {
        if (!section.is_object()) { return; }
        for (const Key<S>& k : keys) { ReadKey(k, section, s); }
        if (finish) { finish(section, s); }
    }
    nlohmann::json Write(const S& s) const
    {
        nlohmann::json j;
        for (const Key<S>& k : keys) { WriteKey(k, j, s); }
        return j;
    }
    const Key<S>* Find(std::string_view key) const
    {
        for (const Key<S>& k : keys)
        {
            if (key == k.key) { return &k; }
        }
        return nullptr;
    }
};
} // namespace schema
