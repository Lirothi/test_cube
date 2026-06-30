#pragma once
#if WITH_EDITOR

#include <string>

class EditorSceneDocument;

// Writes an EditorSceneDocument to a level JSON file. The document's preserved
// top-level sections (camera/skybox/ocean/lights) are re-emitted and "objects"
// is rebuilt from the current EditorObjects. The output is readable by JsonLevel.
namespace LevelDocumentSerializer
{
    bool SaveToFile(const EditorSceneDocument& document, const std::string& path);
}

#endif // WITH_EDITOR
