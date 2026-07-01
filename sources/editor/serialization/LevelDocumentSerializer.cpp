#include "editor/serialization/LevelDocumentSerializer.h"
#if WITH_EDITOR

#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include "editor/scene/EditorSceneDocument.h"

namespace LevelDocumentSerializer
{
    bool SaveToFile(const EditorSceneDocument& document, const std::string& path)
    {
        if (path.empty())
        {
            return false;
        }

        // Start from the preserved level header (camera/skybox/ocean/lights), then
        // replace "objects" with the current document objects. Object order and ids
        // follow the document, so saves are stable (no id churn).
        nlohmann::json out = document.RootJson();
        if (!out.is_object())
        {
            out = nlohmann::json::object();
        }

        // Camera position is intentionally not persisted in level files; it lives
        // per-level in editor_state.json. Keep projection settings (fov/near/far).
        if (out.contains("camera") && out["camera"].is_object())
        {
            out["camera"].erase("position");
        }

        nlohmann::json objects = nlohmann::json::array();
        for (const EditorObject& obj : document.Objects())
        {
            objects.push_back(EditorSceneDocument::ObjectToJson(obj));
        }
        out["objects"] = std::move(objects);

        const std::filesystem::path outPath(path);
        const std::filesystem::path parent = outPath.parent_path();
        if (!parent.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec)
            {
                return false;
            }
        }

        std::ofstream file(outPath, std::ios::binary);
        if (!file)
        {
            return false;
        }
        file << out.dump(2) << '\n';
        return file.good();
    }
}

#endif // WITH_EDITOR
