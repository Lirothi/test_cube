#pragma once
#if WITH_EDITOR

#include "editor/EditorSelection.h"

class Renderer;
class Scene;
class LevelManager;
class EditorSceneDocument;

// References to the app systems and editor state that commands and panels read
// and mutate. `selection` and `document` are owned by EditorController.
struct EditorContext
{
    Renderer& renderer;
    Scene& scene;
    LevelManager& levelManager;
    EditorSceneDocument& document;
    EditorSelection& selection;
};

#endif // WITH_EDITOR
