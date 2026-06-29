#pragma once
#if WITH_EDITOR

#include <cstdint>
#include <string>
#include <vector>

#include "core/math/Math.h"

// nlohmann/json — single header. EditorObject keeps object-specific fields as raw
// JSON so data the editor does not model round-trips verbatim.
#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

struct EditorObjectId
{
    uint64_t value = 0;
};

struct EditorTransform
{
    Math::float3 position{ 0.0f, 0.0f, 0.0f };
    Math::float3 rotationDeg{ 0.0f, 0.0f, 0.0f };
    Math::float3 scale{ 1.0f, 1.0f, 1.0f };
};

struct EditorObject
{
    EditorObjectId id;
    std::string name;
    std::string type;
    bool enabled = true;
    EditorTransform transform;
    nlohmann::json properties;
};

// Editor-side, serializable mirror of a level's objects with stable IDs. Step 4
// only loads object metadata from a level file; it never touches the runtime
// scene.
class EditorSceneDocument
{
public:
    EditorObjectId AllocateId();

    EditorObject* Find(EditorObjectId id);
    const EditorObject* Find(EditorObjectId id) const;

    void Add(EditorObject object);
    bool Remove(EditorObjectId id);

    std::vector<EditorObject>& Objects() { return objects_; }
    const std::vector<EditorObject>& Objects() const { return objects_; }

    bool IsDirty() const { return dirty_; }
    void SetDirty(bool dirty) { dirty_ = dirty; }

    // Replace document contents with object metadata parsed from a level JSON
    // file. Returns false if the file cannot be read or parsed. Common fields are
    // lifted into EditorObject; everything else is preserved verbatim in
    // `properties`. Leaves the document not-dirty.
    bool LoadFromLevelFile(const std::string& path);

    const std::string& LevelPath() const { return levelPath_; }

private:
    std::vector<EditorObject> objects_;
    uint64_t nextId_ = 1;
    bool dirty_ = false;
    std::string levelPath_;
};

#endif // WITH_EDITOR
