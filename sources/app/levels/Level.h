#pragma once

#include <string>
#include <string_view>

class Renderer;
class Scene;
class UploadBatch;
class LevelGameSink;
#if WITH_EDITOR
class LevelDocumentSink;
#endif

// Everything a level needs to load/unload, passed explicitly instead of being
// fetched through Systems::Get*().
struct LevelLoadContext
{
    // Open upload batch for the duration of the load; the caller submits and
    // waits after the level finishes loading.
    UploadBatch& uploads;
    Renderer& renderer;
    Scene& scene;
    LevelGameSink* game = nullptr; // R6.2b: the game's part of a level (game::PlayWorld), or null in a tool without one
#if WITH_EDITOR
    LevelDocumentSink* editorDocument = nullptr; // the editor's document (EditorSceneDocument), or null
#endif
};

class Level
{
public:
    virtual ~Level() = default;

    virtual std::string_view GetName() const = 0;
    virtual std::string_view GetSourcePath() const { return {}; }
    virtual void Load(const LevelLoadContext& ctx) = 0;
    virtual void Unload(const LevelLoadContext& ctx);
    virtual void Tick(float deltaTime) { (void)deltaTime; }

    // Optional: override the JSON file a data-driven level loads from. Used by the
    // editor to reload a saved copy through the normal load path. Default: no-op.
    virtual void SetSourcePath(std::string /*path*/) {}
};
