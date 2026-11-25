#pragma once
// What a level load hands the editor's document while it parses (architecture plan R1.7): the loader (app/levels/
// JsonLevel) knows only this, and editor/scene/EditorSceneDocument implements it. A build without the editor has no
// document and no sink.
#if WITH_EDITOR
#include <cstdint>
#include <string>

#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

class LevelDocumentSink
{
public:
    // Begin a rebuild from the JSON the loader already parsed; the objects follow in the order the loader creates them,
    // so the ids stay in step.
    virtual void BeginLevel(const std::string& path, const nlohmann::json& levelJson) = 0;
    // The object's explicit non-zero "id", or a new one (the allocator is kept past explicit ids).
    virtual std::uint64_t LevelObjectId(const nlohmann::json& objectJson) = 0;
    // The document's mirror of one level object, under the id LevelObjectId gave.
    virtual void AddLevelObject(std::uint64_t id, const nlohmann::json& objectJson) = 0;
    // After the objects: the environment entities, whose fresh ids never collide with the objects'.
    virtual void EndLevel() = 0;

protected:
    ~LevelDocumentSink() = default;
};
#endif
