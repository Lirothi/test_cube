#pragma once
#if WITH_EDITOR

#include <algorithm>

#include "editor/scene/EditorSceneDocument.h"

namespace InspectorPropertyDelta
{
    using Json = nlohmann::json;

    inline const Json& Member(const Json& object, const std::string& key)
    {
        static const Json absent;
        const auto it = object.find(key);
        return it == object.end() ? absent : *it;
    }

    // Vector/color controls write an entire array. Broadcast only components that
    // actually changed, using each recipient's EFFECTIVE (possibly inherited) value.
    inline Json MergeComponents(const Json& before, const Json& after, const Json& target)
    {
        if (!before.is_array() || !after.is_array() || !target.is_array() ||
            before.size() != after.size())
        {
            return after;
        }
        Json result = target;
        for (size_t i = 0; i < std::min(after.size(), target.size()); ++i)
        {
            if (before[i] != after[i])
            {
                result[i] = MergeComponents(before[i], after[i], target[i]);
            }
        }
        return result;
    }

    // The authored delta selects the edited fields. Effective values supply
    // defaults for vector components, never additional fields to broadcast.
    inline void Apply(const Json& before, const Json& after,
        const Json& effectiveBefore, const Json& effectiveAfter,
        Json& target, Json& effectiveTarget)
    {
        if (!target.is_object()) { target = Json::object(); }
        if (!effectiveTarget.is_object()) { effectiveTarget = Json::object(); }
        for (auto it = before.begin(); it != before.end(); ++it)
        {
            if (!after.contains(it.key()))
            {
                target.erase(it.key());
                effectiveTarget.erase(it.key());
            }
        }
        for (auto it = after.begin(); it != after.end(); ++it)
        {
            const std::string& key = it.key();
            const Json& previous = Member(before, key);
            if (before.contains(key) && previous == *it) { continue; }
            if (it->is_object())
            {
                Apply(previous.is_object() ? previous : Json::object(), *it,
                    Member(effectiveBefore, key), Member(effectiveAfter, key),
                    target[key], effectiveTarget[key]);
            }
            else
            {
                const Json& oldValue = Member(effectiveBefore, key);
                const Json& newValue = Member(effectiveAfter, key);
                const Json value = MergeComponents(oldValue, newValue, Member(effectiveTarget, key));
                target[key] = value;
                effectiveTarget[key] = value;
            }
        }
    }
}

#endif
