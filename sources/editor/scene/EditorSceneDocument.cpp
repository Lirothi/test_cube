#include "editor/scene/EditorSceneDocument.h"
#if WITH_EDITOR

#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace
{
    Math::float3 ToFloat3(const nlohmann::json& j, const Math::float3& def)
    {
        if (!j.is_array() || j.size() < 3)
        {
            return def;
        }
        return Math::float3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
    }

    // Keys lifted into EditorObject; everything else stays in `properties` so it
    // round-trips unchanged (rotateSpeedDeg, material, texOffsScale,
    // instancedModels parameters, transparent-mesh fields, ...).
    const char* const kCommonKeys[] = {
        "id", "name", "type", "enabled", "position", "rotationDeg", "scale"
    };

    bool TryReadObjectId(const nlohmann::json& objectJson, uint64_t& outId)
    {
        const auto idIt = objectJson.find("id");
        if (idIt == objectJson.end() || !idIt->is_number_integer())
        {
            return false;
        }

        if (idIt->is_number_unsigned())
        {
            outId = idIt->get<uint64_t>();
            return outId != 0;
        }

        const int64_t signedId = idIt->get<int64_t>();
        if (signedId <= 0)
        {
            return false;
        }

        outId = static_cast<uint64_t>(signedId);
        return true;
    }
}

EditorObjectId EditorSceneDocument::AllocateId()
{
    return EditorObjectId{ nextId_++ };
}

EditorObject* EditorSceneDocument::Find(EditorObjectId id)
{
    for (EditorObject& obj : objects_)
    {
        if (obj.id.value == id.value)
        {
            return &obj;
        }
    }
    return nullptr;
}

const EditorObject* EditorSceneDocument::Find(EditorObjectId id) const
{
    for (const EditorObject& obj : objects_)
    {
        if (obj.id.value == id.value)
        {
            return &obj;
        }
    }
    return nullptr;
}

void EditorSceneDocument::Add(EditorObject object)
{
    if (object.id.value >= nextId_)
    {
        nextId_ = object.id.value + 1;
    }
    objects_.push_back(std::move(object));
}

bool EditorSceneDocument::Remove(EditorObjectId id)
{
    for (auto it = objects_.begin(); it != objects_.end(); ++it)
    {
        if (it->id.value == id.value)
        {
            objects_.erase(it);
            return true;
        }
    }
    return false;
}

EditorObject EditorSceneDocument::ObjectFromJson(EditorObjectId id, const nlohmann::json& o)
{
    EditorObject obj;
    obj.id = id;
    obj.type = o.value("type", std::string());
    obj.enabled = o.value("enabled", true);
    if (o.contains("name") && o["name"].is_string())
    {
        obj.name = o["name"].get<std::string>();
    }
    else
    {
        const std::string base = obj.type.empty() ? std::string("object") : obj.type;
        obj.name = base + " #" + std::to_string(id.value);
    }

    if (o.contains("position")) { obj.transform.position = ToFloat3(o["position"], obj.transform.position); }
    if (o.contains("rotationDeg")) { obj.transform.rotationDeg = ToFloat3(o["rotationDeg"], obj.transform.rotationDeg); }
    if (o.contains("scale")) { obj.transform.scale = ToFloat3(o["scale"], obj.transform.scale); }

    obj.properties = o;
    for (const char* key : kCommonKeys)
    {
        obj.properties.erase(std::string(key));
    }
    return obj;
}

nlohmann::json EditorSceneDocument::ObjectToJson(const EditorObject& obj)
{
    nlohmann::json o = obj.properties;
    o["id"] = obj.id.value;
    o["name"] = obj.name;
    o["type"] = obj.type;
    o["enabled"] = obj.enabled;
    o["position"] = { obj.transform.position.x, obj.transform.position.y, obj.transform.position.z };
    o["rotationDeg"] = { obj.transform.rotationDeg.x, obj.transform.rotationDeg.y, obj.transform.rotationDeg.z };
    o["scale"] = { obj.transform.scale.x, obj.transform.scale.y, obj.transform.scale.z };
    return o;
}

bool EditorSceneDocument::LoadFromLevelFile(const std::string& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return false;
    }

    std::ifstream file(path);
    if (!file)
    {
        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    nlohmann::json doc = nlohmann::json::parse(ss.str(), nullptr, false, /*ignore_comments=*/true);
    if (doc.is_discarded())
    {
        return false;
    }

    ResetFromLevelJson(path, doc);

    const auto objectsIt = doc.find("objects");
    if (objectsIt == doc.end() || !objectsIt->is_array())
    {
        return true; // a valid level may simply have no objects
    }

    for (const nlohmann::json& o : *objectsIt)
    {
        if (!o.is_object())
        {
            continue;
        }

        AddObjectFromJson(ReadOrAllocateObjectId(o), o);
    }

    dirty_ = false;
    return true;
}

void EditorSceneDocument::ResetFromLevelJson(const std::string& path, const nlohmann::json& levelJson)
{
    objects_.clear();
    nextId_ = 1;
    dirty_ = false;
    levelPath_ = path;
    rootJson_ = levelJson; // preserve top-level sections (camera/skybox/ocean/lights) for save
}

EditorObjectId EditorSceneDocument::ReadOrAllocateObjectId(const nlohmann::json& objectJson)
{
    uint64_t explicitId = 0;
    if (TryReadObjectId(objectJson, explicitId))
    {
        if (explicitId >= nextId_)
        {
            nextId_ = explicitId + 1;
        }
        return EditorObjectId{ explicitId };
    }

    return AllocateId();
}

void EditorSceneDocument::AddObjectFromJson(EditorObjectId id, const nlohmann::json& objectJson)
{
    Add(ObjectFromJson(id, objectJson));
}

#endif // WITH_EDITOR
