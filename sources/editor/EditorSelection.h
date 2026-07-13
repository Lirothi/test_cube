#pragma once
#if WITH_EDITOR

#include <algorithm>
#include <utility>
#include <vector>

#include "editor/scene/EditorSceneDocument.h"

// Ordered editor selection with one primary item. The primary is always a
// member of ordered_ and retains the old single-selection meaning for panels
// that only edit one object at a time.
class EditorSelection
{
public:
    const std::vector<EditorObjectId>& Ordered() const { return ordered_; }
    EditorObjectId Primary() const { return primary_; }
    std::size_t Size() const { return ordered_.size(); }
    bool Empty() const { return ordered_.empty(); }

    bool Contains(EditorObjectId id) const
    {
        return id.value != 0 && std::any_of(ordered_.begin(), ordered_.end(),
            [id](EditorObjectId item) { return item.value == id.value; });
    }

    void Clear()
    {
        ordered_.clear();
        primary_ = EditorObjectId{};
    }

    void Replace(EditorObjectId id)
    {
        Clear();
        if (id.value != 0)
        {
            ordered_.push_back(id);
            primary_ = id;
        }
    }

    void Add(EditorObjectId id, bool makePrimary = true)
    {
        if (id.value == 0)
        {
            return;
        }
        if (!Contains(id))
        {
            ordered_.push_back(id);
        }
        if (makePrimary)
        {
            primary_ = id;
        }
    }

    void Remove(EditorObjectId id)
    {
        ordered_.erase(std::remove_if(ordered_.begin(), ordered_.end(),
            [id](EditorObjectId item) { return item.value == id.value; }),
            ordered_.end());
        if (primary_.value == id.value)
        {
            primary_ = ordered_.empty() ? EditorObjectId{} : ordered_.back();
        }
    }

    void Toggle(EditorObjectId id)
    {
        if (Contains(id))
        {
            Remove(id);
        }
        else
        {
            Add(id);
        }
    }

    void SetOrdered(std::vector<EditorObjectId> ids, EditorObjectId primary)
    {
        ordered_.clear();
        ordered_.reserve(ids.size());
        for (EditorObjectId id : ids)
        {
            if (id.value != 0 && !Contains(id))
            {
                ordered_.push_back(id);
            }
        }

        if (Contains(primary))
        {
            primary_ = primary;
        }
        else
        {
            primary_ = ordered_.empty() ? EditorObjectId{} : ordered_.back();
        }
    }

private:
    std::vector<EditorObjectId> ordered_;
    EditorObjectId primary_{};
};

#endif // WITH_EDITOR
