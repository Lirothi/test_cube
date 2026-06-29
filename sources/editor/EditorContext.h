#pragma once
#if WITH_EDITOR

class Renderer;
class Scene;
class LevelManager;
class EditorSceneDocument;
struct EditorObjectId;

// References to the app systems and editor state that commands and panels read
// and mutate. `selectedObject` and `document` are owned by EditorController.
struct EditorContext
{
    Renderer& renderer;
    Scene& scene;
    LevelManager& levelManager;
    EditorSceneDocument& document;
    EditorObjectId& selectedObject;
};

#endif // WITH_EDITOR
