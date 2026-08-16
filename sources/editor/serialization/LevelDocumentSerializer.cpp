#include "editor/serialization/LevelDocumentSerializer.h"
#if WITH_EDITOR

#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include "app/Systems.h"
#include "editor/scene/EditorSceneDocument.h"
#include "ocean/OceanRenderConfigJson.h"

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

        // Rebuild the environment sections from the document's environment entities
        // so future edits flow to save; with unmodified data this reproduces each
        // section verbatim. Unknown top-level sections are preserved from RootJson().
        {
            nlohmann::json spotLights = nlohmann::json::array();
            nlohmann::json pointLights = nlohmann::json::array();
            bool haveSpot = false;
            bool havePoint = false;
            bool haveOcean = false;
            for (const EditorObject& env : document.Environment())
            {
                if (env.type == "camera") { out["camera"] = env.properties; }
                else if (env.type == "directionalLight") { out["directionalLight"] = env.properties; }
                else if (env.type == "skybox") { out["skybox"] = env.properties; }
                else if (env.type == "wind") { out["wind"] = env.properties; } // W2: round-trip the wind entity
                else if (env.type == "cameraExposure") { out["cameraExposure"] = env.properties; } // P1
                else if (env.type == "colorPipeline") { out["colorPipeline"] = env.properties; } // P3C
                else if (env.type == "ocean")
                {
                    nlohmann::json ocean = env.properties;
                    if (const OceanSimulation* simulation = Systems::GetOceanSimulation())
                    {
                        ocean["render"] = OceanRenderConfigJson::ToJson(simulation->GetRenderConfig());
                    }
                    out["ocean"] = std::move(ocean);
                    haveOcean = true;
                }
                else if (env.type == "spotLight") { spotLights.push_back(env.properties); haveSpot = true; }
                else if (env.type == "pointLight") { pointLights.push_back(env.properties); havePoint = true; }
            }
            if (haveSpot) { out["spotLights"] = std::move(spotLights); }
            if (havePoint) { out["pointLights"] = std::move(pointLights); }
            // Ocean can be removed via the Ocean menu; if there's no ocean entity,
            // drop any stale section carried over from the loaded header.
            if (!haveOcean && out.contains("ocean")) { out.erase("ocean"); }
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
