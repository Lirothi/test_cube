#pragma once
#if WITH_EDITOR

#include <functional>
#include <string>

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

    // Editor-owned asset windows. Property drawers use these to open the exact asset represented
    // by a field without depending on EditorController or individual panel classes.
    std::function<void(const std::string& meshPath)> openMeshEditor;
    std::function<void(const std::string& materialName, const std::string& materialPath)>
        openMaterialEditor;
};

#endif // WITH_EDITOR
