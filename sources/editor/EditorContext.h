#pragma once
#if WITH_EDITOR

class Renderer;
class Scene;
class LevelManager;

// References to the app systems the editor UI reads and mutates. Extended in
// later steps (scene document, command stack, current selection).
struct EditorContext
{
    Renderer& renderer;
    Scene& scene;
    LevelManager& levelManager;
};

#endif // WITH_EDITOR
