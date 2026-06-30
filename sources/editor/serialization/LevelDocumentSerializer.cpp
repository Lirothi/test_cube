#include "editor/serialization/LevelDocumentSerializer.h"
#if WITH_EDITOR

#include <fstream>
#include <utility>

#include "editor/scene/EditorSceneDocument.h"

namespace LevelDocumentSerializer
{
    bool SaveToFile(const EditorSceneDocument& document, const std::string& path)
    {
        // Start from the preserved level header (camera/skybox/ocean/lights), then
        // replace "objects" with the current document objects. Object order and ids
        // follow the document, so saves are stable (no id churn).
        nlohmann::json out = document.RootJson();
        if (!out.is_object())
        {
            out = nlohmann::json::object();
        }

        nlohmann::json objects = nlohmann::json::array();
        for (const EditorObject& obj : document.Objects())
        {
            objects.push_back(EditorSceneDocument::ObjectToJson(obj));
        }
        out["objects"] = std::move(objects);

        std::ofstream file(path, std::ios::binary);
        if (!file)
        {
            return false;
        }
        file << out.dump(2);
        return file.good();
    }
}

#endif // WITH_EDITOR
