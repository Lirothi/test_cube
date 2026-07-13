#include "editor/EditorController.h"
#if WITH_EDITOR

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "app/camera/Camera.h"
#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "editor/commands/CreateDocumentObjectCommand.h"
#include "editor/commands/CreateEnvironmentCommand.h"
#include "editor/commands/DeleteObjectCommand.h"
#include "editor/commands/DuplicateObjectCommand.h"
#include "editor/commands/EditEnvironmentCommand.h"
#include "editor/commands/RenameObjectCommand.h"
#include "editor/commands/SetEnabledCommand.h"
#include "editor/commands/SetMaterialCommand.h"
#include "editor/commands/SpawnMeshCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "editor/scene/EnvironmentRuntime.h"
#include "editor/serialization/LevelDocumentSerializer.h"
#include "imgui.h"
#include "ocean/OceanRenderable.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "ocean/OceanSimulation.h"
#include "app/Systems.h"

namespace
{
    constexpr size_t kMaxRecentLevels = 8;
    constexpr const char* kEditorStatePath = "editor_state.json";

    bool MenuItemWithDisabledReason(const char* label, bool enabled, const char* disabledReason)
    {
        const bool pressed = ImGui::MenuItem(label, nullptr, false, enabled);
        if (!enabled && disabledReason &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", disabledReason);
        }
        return pressed;
    }

    void ShowDisabledItemTooltip(bool disabled, const char* reason)
    {
        if (disabled && reason && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", reason);
        }
    }

    struct FileDialogEntry
    {
        std::string name;
        bool directory = false;
    };

    struct LevelCameraState
    {
        Math::float3 position{ 0.0f, 0.0f, 0.0f };
        float yaw = 0.0f;
        float pitch = 0.0f;
    };

    std::string DefaultLevelsDirectory()
    {
        return "data/levels";
    }

    std::string DefaultOceanPresetPath()
    {
        return "data/ocean/default.json";
    }

    std::string NormalizeLevelPath(std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        return path;
    }

    std::string LevelPathString(const std::filesystem::path& path)
    {
        return NormalizeLevelPath(path.string());
    }

    nlohmann::json SpawnPositionJson(const Scene& scene)
    {
        const Math::float3& camPos = scene.CameraRef().GetPosition();
        const Math::float3& camDir = scene.CameraRef().GetDirection();
        return nlohmann::json::array({
            camPos.x + camDir.x * 5.0f,
            camPos.y + camDir.y * 5.0f,
            camPos.z + camDir.z * 5.0f });
    }

    nlohmann::json CameraDirectionJson(const Scene& scene)
    {
        const Math::float3& camDir = scene.CameraRef().GetDirection();
        return nlohmann::json::array({ camDir.x, camDir.y, camDir.z });
    }

    std::string PickDefaultMesh(const AssetRegistry& registry)
    {
        const EditorAssetRecord* first = nullptr;
        for (const EditorAssetRecord& rec : registry.Assets())
        {
            if (rec.id.type != EditorAssetType::Mesh)
            {
                continue;
            }
            if (rec.id.key == "models/box.obj")
            {
                return rec.id.key;
            }
            if (!first)
            {
                first = &rec;
            }
        }
        return first ? first->id.key : std::string{};
    }

    std::string PickDefaultStaticMaterial(const AssetRegistry& registry)
    {
        const EditorAssetRecord* first = nullptr;
        for (const EditorAssetRecord& rec : registry.Assets())
        {
            if (rec.id.type != EditorAssetType::MaterialPreset)
            {
                continue;
            }
            if (rec.id.key == "damaged_plaster")
            {
                return rec.id.key;
            }
            if (!first)
            {
                first = &rec;
            }
        }
        return first ? first->id.key : std::string{};
    }

    std::string PickDefaultSkyboxTexture(const AssetRegistry& registry)
    {
        const EditorAssetRecord* preferredSkybox = nullptr;
        const EditorAssetRecord* firstCube = nullptr;
        for (const EditorAssetRecord& rec : registry.Assets())
        {
            if (rec.id.type != EditorAssetType::Texture)
            {
                continue;
            }
            const bool isCube = rec.texture.valid &&
                rec.texture.kind == EditorTextureKind::TextureCube;
            if (rec.id.key == "textures/skybox.dds")
            {
                if (isCube)
                {
                    return rec.id.key;
                }
                preferredSkybox = &rec;
            }
            if (!firstCube && isCube)
            {
                firstCube = &rec;
            }
        }
        if (firstCube)
        {
            return firstCube->id.key;
        }
        return preferredSkybox ? preferredSkybox->id.key : std::string("textures/skybox.dds");
    }

    bool HasEnvironmentObject(const EditorSceneDocument& document, const char* type)
    {
        for (const EditorObject& env : document.Environment())
        {
            if (env.type == type)
            {
                return true;
            }
        }
        return false;
    }

    nlohmann::json BuildStaticMeshObjectJson(const Scene& scene, const AssetRegistry& registry)
    {
        nlohmann::json o = nlohmann::json::object();
        o["name"] = "Static Mesh";
        o["type"] = "staticMesh";
        o["model"] = PickDefaultMesh(registry);
        o["position"] = SpawnPositionJson(scene);
        o["scale"] = nlohmann::json::array({ 1.0f, 1.0f, 1.0f });
        o["material"] = PickDefaultStaticMaterial(registry);
        o["shader"] = "shaders/gbuffer.hlsl";
        o["inputLayout"] = "PosNormTanUV";
        return o;
    }

    nlohmann::json BuildTransparentMeshObjectJson(const Scene& scene, const AssetRegistry& registry)
    {
        nlohmann::json o = nlohmann::json::object();
        o["name"] = "Transparent Mesh";
        o["type"] = "transparentMesh";
        o["model"] = PickDefaultMesh(registry);
        o["position"] = SpawnPositionJson(scene);
        o["scale"] = nlohmann::json::array({ 1.0f, 1.0f, 1.0f });
        o["tint"] = nlohmann::json::array({ 0.8f, 0.95f, 1.0f });
        o["absorption"] = nlohmann::json::array({ 0.15f, 0.06f, 0.02f });
        o["thickness"] = 0.15f;
        o["reflectionStrength"] = 0.35f;
        o["refractionDistortion"] = 0.03f;
        o["roughness"] = 0.04f;
        o["ior"] = 1.45f;
        return o;
    }

    EditorObject BuildPointLightObject(const Scene& scene)
    {
        EditorObject light;
        light.name = "Point Light";
        light.type = "pointLight";
        light.properties = {
            { "enabled", true },
            { "position", SpawnPositionJson(scene) },
            { "radius", 6.0f },
            { "color", nlohmann::json::array({ 1.0f, 0.92f, 0.78f }) },
            { "intensity", 8.0f },
            { "shadowsEnabled", false }
        };
        return light;
    }

    EditorObject BuildSpotLightObject(const Scene& scene)
    {
        EditorObject light;
        light.name = "Spot Light";
        light.type = "spotLight";
        light.properties = {
            { "enabled", true },
            { "position", SpawnPositionJson(scene) },
            { "direction", CameraDirectionJson(scene) },
            { "range", 14.0f },
            { "innerAngleDeg", 15.0f },
            { "outerAngleDeg", 28.0f },
            { "color", nlohmann::json::array({ 1.0f, 0.92f, 0.78f }) },
            { "intensity", 12.0f },
            { "shadowNormalBias", 0.05f },
            { "shadowDepthBias", 0.0001f },
            { "shadowsEnabled", true }
        };
        return light;
    }

    EditorObject BuildDirectionalLightObject()
    {
        EditorObject light;
        light.name = "Directional Light";
        light.type = "directionalLight";
        light.properties = {
            { "enabled", true },
            { "direction", nlohmann::json::array({ -1.5f, -0.7f, -0.5f }) },
            { "color", nlohmann::json::array({ 1.0f, 0.9f, 0.85f }) },
            { "exposure", 1.0f },
            { "ambient", 0.05f }
        };
        return light;
    }

    EditorObject BuildSkyboxObject(const AssetRegistry& registry)
    {
        EditorObject skybox;
        skybox.name = "Skybox";
        skybox.type = "skybox";
        skybox.properties = {
            { "texture", PickDefaultSkyboxTexture(registry) }
        };
        return skybox;
    }

    EditorObject BuildFreeCameraStartObject(const Scene& scene)
    {
        const Camera& camera = scene.CameraRef();

        EditorObject object;
        object.name = "FreeCameraStart";
        object.type = "freeCameraStart";
        object.enabled = true;
        object.transform.position = camera.GetPosition();
        object.transform.rotationDeg = Math::float3(
            camera.GetPitch() * Math::RAD2DEG,
            camera.GetYaw() * Math::RAD2DEG,
            0.0f);
        object.transform.scale = Math::float3(1.0f, 1.0f, 1.0f);
        object.properties = nlohmann::json::object();
        return object;
    }

    void DestroyLiveOcean(Renderer& renderer, Scene& scene)
    {
        renderer.WaitForPreviousFrame();
        scene.RemoveOceanObjects();
        Systems::DestroyOceanSimulation();
    }

    bool CreateLiveOcean(Renderer& renderer, Scene& scene, const std::string& presetPath, std::string& levelStatus)
    {
        const std::string normalizedPreset = NormalizeLevelPath(presetPath.empty() ? DefaultOceanPresetPath() : presetPath);

        renderer.WaitForPreviousFrame();
        scene.RemoveOceanObjects();
        Systems::DestroyOceanSimulation();

        OceanSimulation* ocean = Systems::CreateOceanSimulation(std::wstring(normalizedPreset.begin(), normalizedPreset.end()));
        if (!ocean)
        {
            levelStatus = "Ocean create failed";
            return false;
        }

        auto renderable = std::make_unique<OceanRenderable>(&scene.CameraRef(), &scene, ocean);
        UploadBatch uploads;
        if (!uploads.Begin(&renderer))
        {
            Systems::DestroyOceanSimulation();
            levelStatus = "Ocean upload batch failed";
            return false;
        }

        if (!scene.AddInitializedObject(renderer, uploads, std::move(renderable)))
        {
            Systems::DestroyOceanSimulation();
            levelStatus = "Ocean scene add failed";
            return false;
        }

        uploads.SubmitAndWait(&renderer);
        levelStatus = "Ocean: " + normalizedPreset;
        return true;
    }

    bool TryReadFloat(const nlohmann::json& value, float& out)
    {
        if (!value.is_number())
        {
            return false;
        }

        const float parsed = value.get<float>();
        if (!std::isfinite(parsed))
        {
            return false;
        }

        out = parsed;
        return true;
    }

    bool TryReadFloat3(const nlohmann::json& value, Math::float3& out)
    {
        if (!value.is_array() || value.size() < 3)
        {
            return false;
        }

        Math::float3 parsed;
        if (!TryReadFloat(value[0], parsed.x) ||
            !TryReadFloat(value[1], parsed.y) ||
            !TryReadFloat(value[2], parsed.z))
        {
            return false;
        }

        out = parsed;
        return true;
    }

    bool IsFinite(const Math::float3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool IsReasonableLevelCameraState(const LevelCameraState& cameraState)
    {
        constexpr float kMaxCameraDistance = 1000000.0f;
        constexpr float kMaxStoredYawPitch = Math::TWO_PI * 1024.0f;
        return IsFinite(cameraState.position) &&
            std::fabs(cameraState.position.x) <= kMaxCameraDistance &&
            std::fabs(cameraState.position.y) <= kMaxCameraDistance &&
            std::fabs(cameraState.position.z) <= kMaxCameraDistance &&
            std::fabs(cameraState.yaw) <= kMaxStoredYawPitch &&
            std::fabs(cameraState.pitch) <= kMaxStoredYawPitch;
    }

    LevelCameraState CaptureLevelCameraState(const Camera& camera)
    {
        return LevelCameraState{ camera.GetPosition(), camera.GetYaw(), camera.GetPitch() };
    }

    bool CameraStateMatches(const LevelCameraState& lhs, const LevelCameraState& rhs)
    {
        constexpr float kEpsilon = 0.0001f;
        return std::fabs(lhs.position.x - rhs.position.x) <= kEpsilon &&
            std::fabs(lhs.position.y - rhs.position.y) <= kEpsilon &&
            std::fabs(lhs.position.z - rhs.position.z) <= kEpsilon &&
            std::fabs(lhs.yaw - rhs.yaw) <= kEpsilon &&
            std::fabs(lhs.pitch - rhs.pitch) <= kEpsilon;
    }

    nlohmann::json LevelCameraStateToJson(const std::string& levelPath, const LevelCameraState& cameraState)
    {
        return {
            { "level", levelPath },
            { "position", nlohmann::json::array({
                cameraState.position.x,
                cameraState.position.y,
                cameraState.position.z }) },
            { "orientation", {
                { "yawRad", cameraState.yaw },
                { "pitchRad", cameraState.pitch }
            } }
        };
    }

    bool TryReadLevelCameraState(const nlohmann::json& stateJson, LevelCameraState& out)
    {
        if (!stateJson.is_object())
        {
            return false;
        }

        LevelCameraState parsed;
        const auto positionIt = stateJson.find("position");
        if (positionIt == stateJson.end() || !TryReadFloat3(*positionIt, parsed.position))
        {
            return false;
        }

        const auto orientationIt = stateJson.find("orientation");
        if (orientationIt == stateJson.end() || !orientationIt->is_object())
        {
            return false;
        }

        const auto yawIt = orientationIt->find("yawRad");
        const auto pitchIt = orientationIt->find("pitchRad");
        if (yawIt == orientationIt->end() ||
            pitchIt == orientationIt->end() ||
            !TryReadFloat(*yawIt, parsed.yaw) ||
            !TryReadFloat(*pitchIt, parsed.pitch))
        {
            return false;
        }

        if (!IsReasonableLevelCameraState(parsed))
        {
            return false;
        }

        out = parsed;
        return true;
    }

    template <size_t N>
    void SetTextBuffer(char (&buffer)[N], const std::string& text)
    {
        const std::string normalized = NormalizeLevelPath(text);
        std::snprintf(buffer, N, "%s", normalized.c_str());
    }

    template <size_t N>
    void NormalizeTextBuffer(char (&buffer)[N])
    {
        SetTextBuffer(buffer, buffer);
    }

    std::string LowerCopy(std::string text)
    {
        for (char& ch : text)
        {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return text;
    }

    bool HasJsonExtension(const std::filesystem::path& path)
    {
        return LowerCopy(path.extension().string()) == ".json";
    }

    std::filesystem::path BuildDialogPath(const char* directoryText, const char* fileNameText)
    {
        std::filesystem::path filePath(fileNameText ? fileNameText : "");
        if (filePath.is_absolute())
        {
            return filePath.lexically_normal();
        }
        return (std::filesystem::path(directoryText ? directoryText : "") / filePath).lexically_normal();
    }

    std::filesystem::path EnsureJsonExtension(std::filesystem::path path)
    {
        if (!path.has_extension())
        {
            path += ".json";
        }
        return path;
    }

    std::vector<FileDialogEntry> ReadLevelDirectory(const std::string& directory, std::string& status)
    {
        std::vector<FileDialogEntry> entries;
        status.clear();

        std::error_code ec;
        std::filesystem::directory_iterator it(directory, ec);
        if (ec)
        {
            status = "Cannot read directory";
            return entries;
        }

        const std::filesystem::directory_iterator end;
        while (it != end)
        {
            const std::filesystem::directory_entry& entry = *it;
            std::error_code entryEc;
            const bool isDirectory = entry.is_directory(entryEc);
            const bool isRegularFile = entry.is_regular_file(entryEc);

            if (!entryEc && (isDirectory || (isRegularFile && HasJsonExtension(entry.path()))))
            {
                entries.push_back(FileDialogEntry{ LevelPathString(entry.path().filename()), isDirectory });
            }

            it.increment(ec);
            if (ec)
            {
                status = "Cannot read all directory entries";
                break;
            }
        }

        std::sort(entries.begin(), entries.end(), [](const FileDialogEntry& a, const FileDialogEntry& b)
        {
            if (a.directory != b.directory)
            {
                return a.directory && !b.directory;
            }
            return LowerCopy(a.name) < LowerCopy(b.name);
        });
        return entries;
    }

    void StartLevelFileDialog(bool saveAs,
        const std::string& currentPath,
        char (&directoryBuffer)[1024],
        char (&fileNameBuffer)[260],
        std::string& status)
    {
        const std::filesystem::path current(currentPath);
        std::filesystem::path directory = current.parent_path();
        if (directory.empty())
        {
            directory = DefaultLevelsDirectory();
        }

        std::string fileName;
        if (saveAs)
        {
            fileName = current.filename().empty() ? "untitled.json" : LevelPathString(current.filename());
        }

        SetTextBuffer(directoryBuffer, LevelPathString(directory));
        SetTextBuffer(fileNameBuffer, fileName);
        status.clear();
    }

    nlohmann::json BuildNewLevelJson()
    {
        nlohmann::json root = nlohmann::json::object();
        root["camera"] = {
            { "hfovDeg", 90.0f },
            { "zNear", 0.01f },
            { "zFar", 10000.0f }
        };
        root["skybox"] = {
            { "texture", "textures/skybox.dds" }
        };
        root["directionalLight"] = {
            { "direction", nlohmann::json::array({ -1.5f, -0.7f, -0.5f }) },
            { "color", nlohmann::json::array({ 1.0f, 0.9f, 0.85f }) },
            { "exposure", 1.0f },
            { "ambient", 0.05f }
        };
        root["spotLights"] = nlohmann::json::array();
        root["pointLights"] = nlohmann::json::array();
        root["objects"] = nlohmann::json::array({
            {
                { "name", "Floor" },
                { "type", "staticMesh" },
                { "model", "models/box.obj" },
                { "material", "sandstone_cracks" },
                { "shader", "shaders/gbuffer.hlsl" },
                { "inputLayout", "PosNormTanUV" },
                { "texOffsScale", nlohmann::json::array({ 0.0f, 0.0f, 20.0f, 20.0f }) },
                { "position", nlohmann::json::array({ 0.0f, -0.1f, 0.0f }) },
                { "scale", nlohmann::json::array({ 40.0f, 0.1f, 40.0f }) },
                { "renderLayer", "Terrain" }
            }
        });
        return root;
    }

    void ResetDocumentFromGeneratedLevel(EditorSceneDocument& document,
        const std::string& path,
        const nlohmann::json& root)
    {
        document.ResetFromLevelJson(path, root);

        const auto objectsIt = root.find("objects");
        if (objectsIt == root.end() || !objectsIt->is_array())
        {
            return;
        }

        for (const nlohmann::json& objectJson : *objectsIt)
        {
            if (!objectJson.is_object())
            {
                continue;
            }

            const EditorObjectId objectId = document.ReadOrAllocateObjectId(objectJson);
            document.AddObjectFromJson(objectId, objectJson);
        }

        document.SetDirty(false);
    }

    std::string NewLevelScratchPath()
    {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
        if (ec || dir.empty())
        {
            dir = "data/levels";
        }

        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        return LevelPathString(dir / ("test_cube_new_level_" + std::to_string(ticks) + ".json"));
    }

    nlohmann::json LoadEditorStateJson()
    {
        std::ifstream file(kEditorStatePath, std::ios::binary);
        if (!file)
        {
            return nlohmann::json::object();
        }

        nlohmann::json root = nlohmann::json::parse(file, nullptr, false, /*ignore_comments=*/true);
        if (!root.is_object())
        {
            return nlohmann::json::object();
        }
        return root;
    }

    bool SaveEditorStateJson(const nlohmann::json& root)
    {
        std::ofstream file(kEditorStatePath, std::ios::binary);
        if (!file)
        {
            return false;
        }
        file << root.dump(2) << '\n';
        return file.good();
    }

    constexpr size_t kMaxPersistedContentItems = 512;
    constexpr size_t kMaxPersistedCollections = 128;

    nlohmann::json AssetIdToJson(const EditorAssetId& id)
    {
        return nlohmann::json{
            { "type", static_cast<int>(id.type) },
            { "key", id.key }
        };
    }

    bool TryReadAssetId(const nlohmann::json& value, EditorAssetId& out)
    {
        if (!value.is_object())
        {
            return false;
        }

        const auto typeIt = value.find("type");
        const auto keyIt = value.find("key");
        if (typeIt == value.end() || keyIt == value.end() ||
            !typeIt->is_number_integer() || !keyIt->is_string())
        {
            return false;
        }

        const std::int64_t typeValue = typeIt->get<std::int64_t>();
        if (typeValue < static_cast<int>(EditorAssetType::Mesh) ||
            typeValue > static_cast<int>(EditorAssetType::Shader) ||
            keyIt->get_ref<const std::string&>().empty())
        {
            return false;
        }

        out.type = static_cast<EditorAssetType>(typeValue);
        out.key = keyIt->get<std::string>();
        return true;
    }

    bool SameAssetId(const EditorAssetId& a, const EditorAssetId& b)
    {
        return a.type == b.type && a.key == b.key;
    }

    bool ContainsAssetId(const std::vector<EditorAssetId>& ids, const EditorAssetId& id)
    {
        return std::any_of(ids.begin(), ids.end(),
            [&id](const EditorAssetId& candidate)
            {
                return SameAssetId(candidate, id);
            });
    }

    bool TryReadAssetIds(const nlohmann::json& value, std::vector<EditorAssetId>& out)
    {
        if (!value.is_array())
        {
            return false;
        }

        std::vector<EditorAssetId> ids;
        for (const nlohmann::json& item : value)
        {
            if (ids.size() >= kMaxPersistedContentItems)
            {
                break;
            }

            EditorAssetId id;
            if (TryReadAssetId(item, id) && !ContainsAssetId(ids, id))
            {
                ids.push_back(std::move(id));
            }
        }

        out = std::move(ids);
        return true;
    }

    bool TryReadFolderPaths(const nlohmann::json& value, std::vector<std::string>& out)
    {
        if (!value.is_array())
        {
            return false;
        }

        std::vector<std::string> folders;
        for (const nlohmann::json& item : value)
        {
            if (folders.size() >= kMaxPersistedContentItems)
            {
                break;
            }
            if (!item.is_string())
            {
                continue;
            }

            const std::string& folder = item.get_ref<const std::string&>();
            if (!folder.empty() &&
                std::find(folders.begin(), folders.end(), folder) == folders.end())
            {
                folders.push_back(folder);
            }
        }

        out = std::move(folders);
        return true;
    }

    nlohmann::json ContentBrowserStateToJson(const ContentBrowserPanel::PersistentState& state)
    {
        nlohmann::json typeFilters = nlohmann::json::array();
        for (bool enabled : state.activeTypeFilters)
        {
            typeFilters.push_back(enabled);
        }

        nlohmann::json favoriteAssets = nlohmann::json::array();
        for (const EditorAssetId& id : state.favoriteAssets)
        {
            favoriteAssets.push_back(AssetIdToJson(id));
        }

        nlohmann::json collections = nlohmann::json::array();
        for (const ContentBrowserCollection& collection : state.collections)
        {
            nlohmann::json collectionAssets = nlohmann::json::array();
            for (const EditorAssetId& id : collection.assets)
            {
                collectionAssets.push_back(AssetIdToJson(id));
            }
            collections.push_back({
                { "name", collection.name },
                { "assets", std::move(collectionAssets) },
                { "folders", collection.folders }
            });
        }

        return nlohmann::json{
            { "typeFilters", std::move(typeFilters) },
            { "selectedFolder", state.selectedFolder },
            { "includeSubfolders", state.includeSubfolders },
            { "viewMode", state.viewMode == ContentBrowserPanel::ViewMode::Tiles ? "tiles" : "list" },
            { "sourcesWidth", state.sourcesWidth },
            { "favorites", {
                { "assets", std::move(favoriteAssets) },
                { "folders", state.favoriteFolders }
            } },
            { "collections", std::move(collections) }
        };
    }

    void ReadBoolMember(const nlohmann::json& object, const char* key, bool& out)
    {
        const auto it = object.find(key);
        if (it != object.end() && it->is_boolean())
        {
            out = it->get<bool>();
        }
    }

    void ReadFloatMember(const nlohmann::json& object,
        const char* key,
        float minValue,
        float maxValue,
        float& out)
    {
        const auto it = object.find(key);
        if (it == object.end() || !it->is_number())
        {
            return;
        }

        const double value = it->get<double>();
        if (std::isfinite(value))
        {
            out = std::clamp(static_cast<float>(value), minValue, maxValue);
        }
    }

    void LoadContentBrowserState(const nlohmann::json& value, ContentBrowserPanel& panel)
    {
        if (!value.is_object())
        {
            return;
        }

        ContentBrowserPanel::PersistentState state = panel.GetPersistentState();
        const auto typeFiltersIt = value.find("typeFilters");
        if (typeFiltersIt != value.end() && typeFiltersIt->is_array() &&
            typeFiltersIt->size() == state.activeTypeFilters.size())
        {
            std::array<bool, 5> typeFilters{};
            bool valid = true;
            for (size_t i = 0; i < typeFilters.size(); ++i)
            {
                const nlohmann::json& item = (*typeFiltersIt)[i];
                if (!item.is_boolean())
                {
                    valid = false;
                    break;
                }
                typeFilters[i] = item.get<bool>();
            }
            if (valid)
            {
                state.activeTypeFilters = typeFilters;
            }
        }

        const auto folderIt = value.find("selectedFolder");
        if (folderIt != value.end() && folderIt->is_string() &&
            !folderIt->get_ref<const std::string&>().empty())
        {
            state.selectedFolder = folderIt->get<std::string>();
        }
        ReadBoolMember(value, "includeSubfolders", state.includeSubfolders);

        const auto viewModeIt = value.find("viewMode");
        if (viewModeIt != value.end() && viewModeIt->is_string())
        {
            const std::string& viewMode = viewModeIt->get_ref<const std::string&>();
            if (viewMode == "tiles")
            {
                state.viewMode = ContentBrowserPanel::ViewMode::Tiles;
            }
            else if (viewMode == "list")
            {
                state.viewMode = ContentBrowserPanel::ViewMode::List;
            }
        }

        const auto sourcesWidthIt = value.find("sourcesWidth");
        if (sourcesWidthIt != value.end() && sourcesWidthIt->is_number())
        {
            const double width = sourcesWidthIt->get<double>();
            if (std::isfinite(width) && width >= 160.0 && width <= 4096.0)
            {
                state.sourcesWidth = static_cast<float>(width);
            }
        }

        const auto favoritesIt = value.find("favorites");
        if (favoritesIt != value.end() && favoritesIt->is_object())
        {
            const auto assetsIt = favoritesIt->find("assets");
            if (assetsIt != favoritesIt->end())
            {
                TryReadAssetIds(*assetsIt, state.favoriteAssets);
            }
            const auto foldersIt = favoritesIt->find("folders");
            if (foldersIt != favoritesIt->end())
            {
                TryReadFolderPaths(*foldersIt, state.favoriteFolders);
            }
        }

        const auto collectionsIt = value.find("collections");
        if (collectionsIt != value.end() && collectionsIt->is_array())
        {
            std::vector<ContentBrowserCollection> collections;
            for (const nlohmann::json& item : *collectionsIt)
            {
                if (collections.size() >= kMaxPersistedCollections || !item.is_object())
                {
                    break;
                }

                const auto nameIt = item.find("name");
                if (nameIt == item.end() || !nameIt->is_string() ||
                    nameIt->get_ref<const std::string&>().empty())
                {
                    continue;
                }

                ContentBrowserCollection collection;
                collection.name = nameIt->get<std::string>();
                const auto assetsIt = item.find("assets");
                if (assetsIt != item.end())
                {
                    TryReadAssetIds(*assetsIt, collection.assets);
                }
                const auto foldersIt = item.find("folders");
                if (foldersIt != item.end())
                {
                    TryReadFolderPaths(*foldersIt, collection.folders);
                }
                collections.push_back(std::move(collection));
            }
            state.collections = std::move(collections);
        }

        panel.SetPersistentState(state);
    }

    nlohmann::json OutlinerStateToJson(const SceneOutlinerPanel::PersistentState& state)
    {
        return nlohmann::json{
            { "meshesExpanded", state.meshesGroupOpen },
            { "lightsExpanded", state.lightsGroupOpen },
            { "camerasExpanded", state.camerasGroupOpen },
            { "environmentExpanded", state.environmentGroupOpen },
            { "otherExpanded", state.otherGroupOpen }
        };
    }

    void LoadOutlinerState(const nlohmann::json& value, SceneOutlinerPanel& panel)
    {
        if (!value.is_object())
        {
            return;
        }

        SceneOutlinerPanel::PersistentState state = panel.GetPersistentState();
        ReadBoolMember(value, "meshesExpanded", state.meshesGroupOpen);
        ReadBoolMember(value, "lightsExpanded", state.lightsGroupOpen);
        ReadBoolMember(value, "camerasExpanded", state.camerasGroupOpen);
        ReadBoolMember(value, "environmentExpanded", state.environmentGroupOpen);
        ReadBoolMember(value, "otherExpanded", state.otherGroupOpen);
        panel.SetPersistentState(state);
    }

    nlohmann::json ViewportGizmoStateToJson(const ViewportGizmo::PersistentState& state)
    {
        return nlohmann::json{
            { "snapEnabled", state.snapEnabled },
            { "translationIncrement", state.translationIncrement },
            { "rotationIncrement", state.rotationIncrement },
            { "scaleIncrement", state.scaleIncrement },
            { "transformSpace", state.transformSpace == ViewportGizmo::TransformSpace::Local ?
                "local" : "world" }
        };
    }

    void LoadViewportGizmoState(const nlohmann::json& value, ViewportGizmo& viewportGizmo)
    {
        if (!value.is_object())
        {
            return;
        }

        ViewportGizmo::PersistentState state = viewportGizmo.GetPersistentState();
        ReadBoolMember(value, "snapEnabled", state.snapEnabled);
        ReadFloatMember(value, "translationIncrement", 0.001f, 10000.0f,
            state.translationIncrement);
        ReadFloatMember(value, "rotationIncrement", 0.1f, 180.0f,
            state.rotationIncrement);
        ReadFloatMember(value, "scaleIncrement", 0.001f, 10.0f,
            state.scaleIncrement);

        const auto transformSpaceIt = value.find("transformSpace");
        if (transformSpaceIt != value.end() && transformSpaceIt->is_string())
        {
            const std::string& transformSpace =
                transformSpaceIt->get_ref<const std::string&>();
            if (transformSpace == "local")
            {
                state.transformSpace = ViewportGizmo::TransformSpace::Local;
            }
            else if (transformSpace == "world")
            {
                state.transformSpace = ViewportGizmo::TransformSpace::World;
            }
        }

        viewportGizmo.SetPersistentState(state);
    }

    nlohmann::json BuildPanelStateJson(bool showContentBrowser,
        bool showOutliner,
        bool showInspector,
        bool showCommandHistory,
        const ContentBrowserPanel& contentBrowser,
        const SceneOutlinerPanel& outliner,
        const ViewportGizmo& viewportGizmo)
    {
        return nlohmann::json{
            { "contentBrowserVisible", showContentBrowser },
            { "outlinerVisible", showOutliner },
            { "inspectorVisible", showInspector },
            { "commandHistoryVisible", showCommandHistory },
            { "contentBrowser", ContentBrowserStateToJson(contentBrowser.GetPersistentState()) },
            { "outliner", OutlinerStateToJson(outliner.GetPersistentState()) },
            { "viewportGizmo", ViewportGizmoStateToJson(viewportGizmo.GetPersistentState()) }
        };
    }

    void LoadEditorPanelState(bool& showContentBrowser,
        bool& showOutliner,
        bool& showInspector,
        bool& showCommandHistory,
        ContentBrowserPanel& contentBrowser,
        SceneOutlinerPanel& outliner,
        ViewportGizmo& viewportGizmo)
    {
        const nlohmann::json root = LoadEditorStateJson();
        const auto levelEditorIt = root.find("levelEditor");
        if (levelEditorIt == root.end() || !levelEditorIt->is_object())
        {
            return;
        }

        const auto panelStateIt = levelEditorIt->find("panelState");
        if (panelStateIt == levelEditorIt->end() || !panelStateIt->is_object())
        {
            return;
        }

        const nlohmann::json& panelState = *panelStateIt;
        ReadBoolMember(panelState, "contentBrowserVisible", showContentBrowser);
        ReadBoolMember(panelState, "outlinerVisible", showOutliner);
        ReadBoolMember(panelState, "inspectorVisible", showInspector);
        ReadBoolMember(panelState, "commandHistoryVisible", showCommandHistory);

        const auto contentBrowserIt = panelState.find("contentBrowser");
        if (contentBrowserIt != panelState.end())
        {
            LoadContentBrowserState(*contentBrowserIt, contentBrowser);
        }
        const auto outlinerIt = panelState.find("outliner");
        if (outlinerIt != panelState.end())
        {
            LoadOutlinerState(*outlinerIt, outliner);
        }
        const auto viewportGizmoIt = panelState.find("viewportGizmo");
        if (viewportGizmoIt != panelState.end())
        {
            LoadViewportGizmoState(*viewportGizmoIt, viewportGizmo);
        }
    }

    bool SaveEditorPanelState(const nlohmann::json& panelState)
    {
        nlohmann::json root = LoadEditorStateJson();
        if (!root["levelEditor"].is_object())
        {
            root["levelEditor"] = nlohmann::json::object();
        }
        root["levelEditor"]["panelState"] = panelState;
        return SaveEditorStateJson(root);
    }

    bool LoadLevelCameraState(const std::string& levelPath, LevelCameraState& out)
    {
        const std::string normalizedPath = NormalizeLevelPath(levelPath);
        if (normalizedPath.empty())
        {
            return false;
        }

        const nlohmann::json root = LoadEditorStateJson();
        const auto levelEditorIt = root.find("levelEditor");
        if (levelEditorIt == root.end() || !levelEditorIt->is_object())
        {
            return false;
        }

        const auto cameraStatesIt = levelEditorIt->find("levelCameraStates");
        if (cameraStatesIt != levelEditorIt->end() && cameraStatesIt->is_object())
        {
            const auto cameraStateIt = cameraStatesIt->find(normalizedPath);
            if (cameraStateIt != cameraStatesIt->end() && TryReadLevelCameraState(*cameraStateIt, out))
            {
                return true;
            }
        }

        const auto lastCameraIt = levelEditorIt->find("lastLevelCamera");
        if (lastCameraIt != levelEditorIt->end() && lastCameraIt->is_object() &&
            NormalizeLevelPath(lastCameraIt->value("level", std::string())) == normalizedPath)
        {
            return TryReadLevelCameraState(*lastCameraIt, out);
        }

        return false;
    }

    bool SaveLevelCameraState(const std::string& levelPath, const Camera& camera)
    {
        const std::string normalizedPath = NormalizeLevelPath(levelPath);
        if (normalizedPath.empty())
        {
            return false;
        }

        nlohmann::json root = LoadEditorStateJson();
        if (!root["levelEditor"].is_object())
        {
            root["levelEditor"] = nlohmann::json::object();
        }

        nlohmann::json& levelEditor = root["levelEditor"];
        if (!levelEditor["levelCameraStates"].is_object())
        {
            levelEditor["levelCameraStates"] = nlohmann::json::object();
        }

        const nlohmann::json stateJson = LevelCameraStateToJson(normalizedPath, CaptureLevelCameraState(camera));
        levelEditor["levelCameraStates"][normalizedPath] = stateJson;
        levelEditor["lastLevelCamera"] = stateJson;
        return SaveEditorStateJson(root);
    }

    bool RestoreLevelCameraState(Renderer& renderer, Scene& scene, const std::string& levelPath)
    {
        LevelCameraState cameraState;
        if (!LoadLevelCameraState(levelPath, cameraState))
        {
            return false;
        }

        Camera& camera = scene.CameraRef();
        camera.SetPosition(cameraState.position);
        camera.SetYawPitch(cameraState.yaw, cameraState.pitch);
        camera.CalcMatrices(&renderer);
        camera.ResetHistory();
        return true;
    }

    bool ApplyLevelCameraStateToLoadOptions(const std::string& levelPath, LevelLoadOptions& options)
    {
        LevelCameraState cameraState;
        if (!LoadLevelCameraState(levelPath, cameraState))
        {
            return false;
        }

        options.cameraOverride = LevelCameraOverride{ cameraState.position, cameraState.yaw, cameraState.pitch };
        return true;
    }

    void LoadEditorState(std::vector<std::string>& recentLevelPaths, int& selectionOutlineRadius)
    {
        recentLevelPaths.clear();
        const nlohmann::json root = LoadEditorStateJson();
        const auto levelEditorIt = root.find("levelEditor");
        if (levelEditorIt == root.end() || !levelEditorIt->is_object())
        {
            return;
        }

        const auto outlineRadiusIt = levelEditorIt->find("selectionOutlineRadius");
        if (outlineRadiusIt != levelEditorIt->end() && outlineRadiusIt->is_number_integer())
        {
            selectionOutlineRadius = std::clamp(outlineRadiusIt->get<int>(), 1, 8);
        }

        const auto recentIt = levelEditorIt->find("recentLevels");
        if (recentIt == levelEditorIt->end() || !recentIt->is_array())
        {
            return;
        }

        for (const nlohmann::json& item : *recentIt)
        {
            if (!item.is_string())
            {
                continue;
            }

            const std::string normalizedPath = NormalizeLevelPath(item.get<std::string>());
            if (normalizedPath.empty() ||
                std::find(recentLevelPaths.begin(), recentLevelPaths.end(), normalizedPath) != recentLevelPaths.end())
            {
                continue;
            }

            recentLevelPaths.push_back(normalizedPath);
            if (recentLevelPaths.size() >= kMaxRecentLevels)
            {
                break;
            }
        }
    }

    bool SaveEditorState(const std::vector<std::string>& recentLevelPaths, int selectionOutlineRadius)
    {
        nlohmann::json root = LoadEditorStateJson();
        nlohmann::json recent = nlohmann::json::array();
        for (const std::string& path : recentLevelPaths)
        {
            const std::string normalizedPath = NormalizeLevelPath(path);
            if (!normalizedPath.empty())
            {
                recent.push_back(normalizedPath);
            }
        }

        if (!root["levelEditor"].is_object())
        {
            root["levelEditor"] = nlohmann::json::object();
        }
        root["levelEditor"]["recentLevels"] = std::move(recent);
        root["levelEditor"]["selectionOutlineRadius"] = std::clamp(selectionOutlineRadius, 1, 8);
        return SaveEditorStateJson(root);
    }

    bool RememberRecentLevel(std::vector<std::string>& recentLevelPaths, const std::string& path)
    {
        const std::string normalizedPath = NormalizeLevelPath(path);
        if (normalizedPath.empty())
        {
            return false;
        }

        const std::vector<std::string> oldRecentLevelPaths = recentLevelPaths;

        for (auto it = recentLevelPaths.begin(); it != recentLevelPaths.end();)
        {
            if (*it == normalizedPath)
            {
                it = recentLevelPaths.erase(it);
            }
            else
            {
                ++it;
            }
        }

        recentLevelPaths.insert(recentLevelPaths.begin(), normalizedPath);
        if (recentLevelPaths.size() > kMaxRecentLevels)
        {
            recentLevelPaths.resize(kMaxRecentLevels);
        }
        return recentLevelPaths != oldRecentLevelPaths;
    }

    bool ForgetRecentLevel(std::vector<std::string>& recentLevelPaths, const std::string& path)
    {
        const std::string normalizedPath = NormalizeLevelPath(path);
        if (normalizedPath.empty())
        {
            return false;
        }

        const auto oldSize = recentLevelPaths.size();
        recentLevelPaths.erase(std::remove(recentLevelPaths.begin(), recentLevelPaths.end(), normalizedPath), recentLevelPaths.end());
        return recentLevelPaths.size() != oldSize;
    }

    bool LevelFileExists(const std::string& path)
    {
        const std::string normalizedPath = NormalizeLevelPath(path);
        if (normalizedPath.empty())
        {
            return false;
        }

        std::error_code ec;
        const std::filesystem::path fsPath(normalizedPath);
        const bool exists = std::filesystem::exists(fsPath, ec);
        const bool regularFile = exists && std::filesystem::is_regular_file(fsPath, ec);
        return !ec && regularFile;
    }

    bool TryGetSelectionFrameTarget(
        const Scene& scene,
        const EditorSceneDocument& document,
        EditorObjectId id,
        Math::float3& outCenter,
        float& outRadius)
    {
        if (id.value == 0)
        {
            return false;
        }

        if (const RenderableObjectBase* runtime = scene.FindEditorObject(id.value))
        {
            const AABB& bounds = runtime->GetWorldBounds();
            if (bounds.IsValid())
            {
                outCenter = bounds.GetCenter();
                outRadius = std::max(bounds.GetRadius(), 1.0f);
                return true;
            }
        }

        if (const EditorObject* object = document.Find(id))
        {
            outCenter = object->transform.position;
            outRadius = std::max(object->transform.scale.Length(), 1.0f);
            return true;
        }

        for (const EditorObject& env : document.Environment())
        {
            if (env.id.value != id.value)
            {
                continue;
            }

            const auto positionIt = env.properties.find("position");
            if (positionIt == env.properties.end() || !TryReadFloat3(*positionIt, outCenter))
            {
                return false;
            }

            if (env.type == "pointLight")
            {
                outRadius = std::max(env.properties.value("radius", 1.0f), 1.0f);
            }
            else if (env.type == "spotLight")
            {
                outRadius = std::max(env.properties.value("range", 4.0f) * 0.25f, 1.0f);
            }
            else
            {
                outRadius = 1.0f;
            }
            return true;
        }

        return false;
    }

    bool FrameSelection(Renderer& renderer, Scene& scene, const EditorSceneDocument& document, EditorObjectId id)
    {
        Math::float3 center;
        float radius = 1.0f;
        if (!TryGetSelectionFrameTarget(scene, document, id, center, radius))
        {
            return false;
        }

        Camera& camera = scene.CameraRef();
        Math::float3 forward = camera.GetDirection();
        if (forward.Length() <= Math::EPS)
        {
            forward = Math::float3(0.0f, 0.0f, 1.0f);
        }

        const float distance = std::max(radius * 2.5f, 3.0f);
        camera.SetPosition(center - forward.Normalized() * distance);
        camera.CalcMatrices(&renderer);
        camera.ResetHistory();
        return true;
    }

    std::string DropSelectionToGround(EditorContext& ctx, EditorCommandStack& commandStack)
    {
        EditorObject* object = ctx.document.Find(ctx.selectedObject);
        RenderableObjectBase* runtime = ctx.scene.FindEditorObject(ctx.selectedObject.value);
        if (!object || !runtime || !runtime->AsRenderableObject())
        {
            return "Select a mesh to drop to ground";
        }

        const AABB& bounds = runtime->GetWorldBounds();
        if (!bounds.IsValid())
        {
            return "Selected mesh has no valid bounds";
        }

        const Math::float3 center = bounds.GetCenter();
        const Math::float3 rayOrigin(center.x, bounds.GetMin().y, center.z);
        const Math::float3 rayDirection(0.0f, -1.0f, 0.0f);
        float hitDistance = 0.0f;
        const Scene::SceneObjectId hit = ctx.scene.RaycastEditorObject(
            rayOrigin, rayDirection, &hitDistance, ctx.selectedObject.value);
        if (hit == 0 || !std::isfinite(hitDistance))
        {
            return "No visible editor object below selection";
        }
        if (hitDistance <= 1.0e-4f)
        {
            return "Selection is already resting on a surface";
        }

        EditorTransform after = object->transform;
        after.position.y -= hitDistance;
        if (!commandStack.Execute(ctx, std::make_unique<TransformObjectCommand>(
                object->id, object->transform, after)))
        {
            return "Drop to ground failed";
        }
        return "Dropped selection to ground";
    }
}

void EditorController::OnLevelChangeRequestCompleted(const LevelChangeRequest& request,
    bool loaded,
    Renderer& renderer,
    Scene& scene,
    LevelManager& levelManager)
{
    (void)renderer;
    (void)levelManager;

    if (pendingLevelAction_ == PendingLevelAction::None || !request.loadFromPath)
    {
        return;
    }

    const std::string requestPath = NormalizeLevelPath(request.sourcePath);
    if (requestPath != pendingLevelPath_)
    {
        return;
    }

    const PendingLevelAction completedAction = pendingLevelAction_;
    pendingLevelAction_ = PendingLevelAction::None;
    pendingLevelPath_.clear();

    if (!loaded)
    {
        pendingNewLevelJson_ = nlohmann::json();
        switch (completedAction)
        {
        case PendingLevelAction::Save: levelStatus_ = "Save failed"; break;
        case PendingLevelAction::Reload: levelStatus_ = "Reload failed"; break;
        case PendingLevelAction::New: levelStatus_ = "New level failed"; break;
        default: levelStatus_ = "Open failed"; break;
        }
        return;
    }

    commandStack_.Clear();
    selectedObject_ = EditorObjectId{};

    if (completedAction == PendingLevelAction::New)
    {
        ResetDocumentFromGeneratedLevel(document_, std::string(), pendingNewLevelJson_);
        pendingNewLevelJson_ = nlohmann::json();
        levelStatus_ = "New unsaved level";
        lastSavedCameraStateValid_ = false;
        return;
    }

    pendingNewLevelJson_ = nlohmann::json();

    const std::string levelPath = NormalizeLevelPath(document_.LevelPath());
    if (RememberRecentLevel(recentLevelPaths_, levelPath))
    {
        SaveEditorState(recentLevelPaths_, selectionOutlineRadius_);
    }

    if (SaveLevelCameraState(levelPath, scene.CameraRef()))
    {
        const LevelCameraState cameraState = CaptureLevelCameraState(scene.CameraRef());
        lastSavedCameraLevelPath_ = levelPath;
        lastSavedCameraPosition_ = cameraState.position;
        lastSavedCameraYaw_ = cameraState.yaw;
        lastSavedCameraPitch_ = cameraState.pitch;
        lastSavedCameraStateValid_ = true;
        nextCameraStateSaveTimeSec_ = ImGui::GetTime() + 1.0;
    }

    switch (completedAction)
    {
    case PendingLevelAction::Save:
        levelStatus_ = "Saved " + levelPath;
        break;
    case PendingLevelAction::Reload:
        levelStatus_ = "Reloaded " + levelPath;
        break;
    default:
        levelStatus_ = "Opened " + levelPath;
        break;
    }
}

bool EditorController::RequestOpenLevelPath(LevelManager& levelManager,
    const std::string& path,
    bool preserveCameraTransform,
    bool bypassUnsavedChangesConfirmation)
{
    const std::string normalizedPath = NormalizeLevelPath(path);
    if (normalizedPath.empty())
    {
        return false;
    }
    if (!LevelFileExists(normalizedPath))
    {
        levelStatus_ = "Level file not found: " + normalizedPath;
        return false;
    }

    if (document_.IsDirty() && !bypassUnsavedChangesConfirmation)
    {
        confirmOpenLevelPath_ = normalizedPath;
        confirmOpenLevelPreserveCamera_ = preserveCameraTransform;
        confirmOpenLevelPopupRequested_ = true;
        levelStatus_ = "Confirm opening " + normalizedPath;
        open_ = true;
        return true;
    }

    LevelLoadOptions options;
    options.preserveCameraTransform = preserveCameraTransform;
    options.editorDocument = &document_;
    if (!preserveCameraTransform)
    {
        ApplyLevelCameraStateToLoadOptions(normalizedPath, options);
    }
    levelManager.RequestLevelPathChange(normalizedPath, options);
    pendingLevelAction_ = PendingLevelAction::Open;
    pendingLevelPath_ = normalizedPath;
    levelStatus_ = "Opening " + normalizedPath;
    return true;
}

void EditorController::Draw(Renderer& renderer, Scene& scene, LevelManager& levelManager)
{
    const auto markCameraStateSaved = [this](const std::string& levelPath, const LevelCameraState& cameraState)
    {
        lastSavedCameraLevelPath_ = NormalizeLevelPath(levelPath);
        lastSavedCameraPosition_ = cameraState.position;
        lastSavedCameraYaw_ = cameraState.yaw;
        lastSavedCameraPitch_ = cameraState.pitch;
        lastSavedCameraStateValid_ = true;
        nextCameraStateSaveTimeSec_ = ImGui::GetTime() + 1.0;
    };
    const auto saveCurrentLevelCameraState = [&](bool force) -> bool
    {
        const std::string levelPath = NormalizeLevelPath(document_.LevelPath());
        if (levelPath.empty())
        {
            return false;
        }

        const LevelCameraState cameraState = CaptureLevelCameraState(scene.CameraRef());
        const LevelCameraState lastSavedState{
            lastSavedCameraPosition_,
            lastSavedCameraYaw_,
            lastSavedCameraPitch_
        };
        const bool changed = !lastSavedCameraStateValid_ ||
            lastSavedCameraLevelPath_ != levelPath ||
            !CameraStateMatches(lastSavedState, cameraState);
        if (!force && (!changed || ImGui::GetTime() < nextCameraStateSaveTimeSec_))
        {
            return false;
        }

        if (!SaveLevelCameraState(levelPath, scene.CameraRef()))
        {
            return false;
        }

        markCameraStateSaved(levelPath, cameraState);
        return true;
    };

    // First-frame init, run even while the editor UI is closed: scan assets, load
    // the active level's object metadata, and restore the per-level camera from
    // editor_state.json. Doing this before the open_ check below means the camera
    // is restored at startup, not only after the editor is first opened with F2.
    // If there is no saved record, the camera keeps the loader's zero baseline.
    if (!firstOpenInitialized_)
    {
        assetRegistry_.Refresh();
        LoadEditorState(recentLevelPaths_, selectionOutlineRadius_);
        LoadEditorPanelState(showContentBrowser_, showOutliner_, showInspector_, showCommandHistory_,
            contentBrowser_, outliner_, viewportGizmo_);
        lastObservedPanelState_ = BuildPanelStateJson(showContentBrowser_, showOutliner_,
            showInspector_, showCommandHistory_, contentBrowser_, outliner_, viewportGizmo_);
        panelStateLoaded_ = true;
        if (document_.LoadFromLevelFile("data/levels/demo.json"))
        {
            if (RestoreLevelCameraState(renderer, scene, document_.LevelPath()))
            {
                markCameraStateSaved(document_.LevelPath(), CaptureLevelCameraState(scene.CameraRef()));
            }
            levelStatus_ = "Loaded " + NormalizeLevelPath(document_.LevelPath());
        }
        firstOpenInitialized_ = true;
    }

    selectionOutlineRadius_ = std::clamp(selectionOutlineRadius_, 1, 8);
    scene.SetEditorSelectionOutlineRadius(static_cast<std::uint32_t>(selectionOutlineRadius_));
    scene.SetSelectedEditorObjectId(open_ ? selectedObject_.value : 0);
    if (!open_)
    {
        return;
    }

    EditorContext ctx{ renderer, scene, levelManager, document_, selectedObject_ };
    if (!extensionsRegistered_)
    {
        EditorExtensionRegistry::RegisterBuiltins(extensions_);

        extensions_.RegisterPanel(std::make_unique<EditorLambdaPanel>(
            "contentBrowser",
            "Content Browser",
            &showContentBrowser_,
            true,
            [this](EditorContext& panelCtx)
            {
                const ContentBrowserAction action =
                    contentBrowser_.Draw(assetRegistry_, selectedAsset_, extensions_,
                        document_, selectedObject_, panelCtx.renderer, thumbnailCache_,
                        &showContentBrowser_);
                if (!action.HasAction())
                {
                    return;
                }

                const EditorAssetRecord* asset = assetRegistry_.FindById(action.asset);
                if (!asset)
                {
                    return;
                }

                if (action.type == ContentBrowserAction::Type::SpawnObject)
                {
                    const IEditorObjectFactory* factory = extensions_.FindObjectFactory(action.objectFactoryType);
                    if (!factory || !factory->CanBuildFromAsset(asset))
                    {
                        return;
                    }

                    nlohmann::json objectJson = factory->BuildDefaultJson(asset, panelCtx, assetRegistry_);
                    commandStack_.Execute(panelCtx, std::make_unique<SpawnMeshCommand>(std::move(objectJson)));
                }
                else if (action.type == ContentBrowserAction::Type::AssignMaterial)
                {
                    EditorObject* selected = document_.Find(selectedObject_);
                    if (asset->id.type != EditorAssetType::MaterialPreset ||
                        !selected ||
                        selected->type != "staticMesh")
                    {
                        levelStatus_ = "Select a static mesh before assigning a material";
                        return;
                    }

                    commandStack_.Execute(panelCtx,
                        std::make_unique<SetMaterialCommand>(selectedObject_, asset->id.key));
                }
                else if (action.type == ContentBrowserAction::Type::OpenLevel ||
                    action.type == ContentBrowserAction::Type::OpenLevelPreservingCamera)
                {
                    if (asset->id.type != EditorAssetType::Level)
                    {
                        return;
                    }
                    const bool preserveCamera =
                        action.type == ContentBrowserAction::Type::OpenLevelPreservingCamera;
                    RequestOpenLevelPath(panelCtx.levelManager, asset->path, preserveCamera);
                }
            }));

        extensions_.RegisterPanel(std::make_unique<EditorLambdaPanel>(
            "sceneOutliner",
            "Scene Outliner",
            &showOutliner_,
            true,
            [this](EditorContext& panelCtx)
            {
                const OutlinerAction outlinerAction = outliner_.Draw(document_, selectedObject_, &showOutliner_);
                if (outlinerAction.type == OutlinerAction::Type::DeleteObject)
                {
                    commandStack_.Execute(panelCtx, std::make_unique<DeleteObjectCommand>(outlinerAction.target));
                }
                else if (outlinerAction.type == OutlinerAction::Type::DuplicateObject)
                {
                    commandStack_.Execute(panelCtx,
                        std::make_unique<DuplicateObjectCommand>(outlinerAction.target));
                }
                else if (outlinerAction.type == OutlinerAction::Type::FrameSelection)
                {
                    selectedObject_ = outlinerAction.target;
                    if (!FrameSelection(panelCtx.renderer,
                            panelCtx.scene,
                            document_,
                            outlinerAction.target))
                    {
                        levelStatus_ = "Nothing to frame";
                    }
                }
                else if (outlinerAction.type == OutlinerAction::Type::RenameObject)
                {
                    const EditorObject* object = document_.Find(outlinerAction.target);
                    if (object)
                    {
                        commandStack_.Execute(panelCtx, std::make_unique<RenameObjectCommand>(
                            outlinerAction.target,
                            object->name,
                            outlinerAction.nameValue));
                    }
                }
                else if (outlinerAction.type == OutlinerAction::Type::SetEnabled)
                {
                    commandStack_.Execute(panelCtx, std::make_unique<SetEnabledCommand>(outlinerAction.target, outlinerAction.enabledValue));
                }
                else if (outlinerAction.type == OutlinerAction::Type::SetEnvEnabled)
                {
                    for (const EditorObject& env : document_.Environment())
                    {
                        if (env.id.value == outlinerAction.target.value)
                        {
                            nlohmann::json after = env.properties;
                            after["enabled"] = outlinerAction.enabledValue;
                            commandStack_.Execute(panelCtx,
                                std::make_unique<EditEnvironmentCommand>(
                                    env.id,
                                    env.properties,
                                    std::move(after),
                                    outlinerAction.enabledValue ?
                                        "Enable Environment" :
                                        "Disable Environment"));
                            break;
                        }
                    }
                }
            }));

        extensions_.RegisterPanel(std::make_unique<EditorLambdaPanel>(
            "inspector",
            "Inspector",
            &showInspector_,
            true,
            [this](EditorContext& panelCtx)
            {
                inspector_.Draw(panelCtx, commandStack_, assetRegistry_, extensions_, &showInspector_);
            }));

        extensions_.RegisterPanel(std::make_unique<EditorLambdaPanel>(
            "commandHistory",
            "Command History",
            &showCommandHistory_,
            true,
            [this](EditorContext& panelCtx)
            {
                commandHistory_.Draw(panelCtx, commandStack_, &showCommandHistory_);
            }));

        extensions_.RegisterPanel(std::make_unique<EditorLambdaPanel>(
            "viewportGizmo",
            "Viewport/Gizmo",
            nullptr,
            false,
            [this](EditorContext& panelCtx)
            {
                viewportGizmo_.Update(panelCtx, commandStack_, assetRegistry_, extensions_);
            }));

        extensionsRegistered_ = true;
    }

    const auto queueLevelPathLoad = [&](const std::string& path,
        bool preserveCameraTransform,
        PendingLevelAction action,
        std::string status) -> bool
    {
        const std::string normalizedPath = NormalizeLevelPath(path);
        if (normalizedPath.empty())
        {
            return false;
        }

        LevelLoadOptions options;
        options.preserveCameraTransform = preserveCameraTransform;
        options.editorDocument = &document_;
        if (!preserveCameraTransform)
        {
            ApplyLevelCameraStateToLoadOptions(normalizedPath, options);
        }

        levelManager.RequestLevelPathChange(normalizedPath, options);
        pendingLevelAction_ = action;
        pendingLevelPath_ = normalizedPath;
        levelStatus_ = std::move(status);
        return true;
    };
    const auto openLevel = [&](const std::string& path) -> bool
    {
        const std::string normalizedPath = NormalizeLevelPath(path);
        if (normalizedPath.empty())
        {
            return false;
        }
        if (!LevelFileExists(normalizedPath))
        {
            if (ForgetRecentLevel(recentLevelPaths_, normalizedPath))
            {
                SaveEditorState(recentLevelPaths_, selectionOutlineRadius_);
            }
            levelStatus_ = "Level file not found: " + normalizedPath;
            return false;
        }

        saveCurrentLevelCameraState(true);
        return RequestOpenLevelPath(levelManager, normalizedPath, false);
    };
    const auto saveLevel = [&](const std::string& path) -> bool
    {
        const std::string normalizedPath = NormalizeLevelPath(path);
        if (!LevelDocumentSerializer::SaveToFile(document_, normalizedPath))
        {
            levelStatus_ = "Save failed";
            return false;
        }

        document_.SetLevelPath(normalizedPath);
        document_.SetDirty(false);
        if (RememberRecentLevel(recentLevelPaths_, normalizedPath))
        {
            SaveEditorState(recentLevelPaths_, selectionOutlineRadius_);
        }

        if (SaveLevelCameraState(normalizedPath, scene.CameraRef()))
        {
            markCameraStateSaved(normalizedPath, CaptureLevelCameraState(scene.CameraRef()));
        }

        levelStatus_ = "Saved " + normalizedPath;
        return true;
    };
    bool requestOpenLevelDialog = false;
    bool requestSaveLevelAsDialog = false;
    const auto beginOpenLevelDialog = [&]()
    {
        requestOpenLevelDialog = true;
    };
    const auto beginSaveLevelAsDialog = [&]()
    {
        requestSaveLevelAsDialog = true;
    };
    const auto newLevel = [&]() -> bool
    {
        saveCurrentLevelCameraState(true);
        const nlohmann::json root = BuildNewLevelJson();
        ResetDocumentFromGeneratedLevel(document_, std::string(), root);

        const std::string scratchPath = NewLevelScratchPath();
        if (!LevelDocumentSerializer::SaveToFile(document_, scratchPath))
        {
            levelStatus_ = "New level failed";
            return false;
        }

        pendingNewLevelJson_ = root;
        return queueLevelPathLoad(scratchPath, false, PendingLevelAction::New, "Creating new level");
    };

    const EditorHotkeyActions hotkeyActions = hotkeys_.Poll(viewportGizmo_);
    if (hotkeyActions.undo)
    {
        commandStack_.Undo(ctx);
    }
    if (hotkeyActions.redo)
    {
        commandStack_.Redo(ctx);
    }
    if (hotkeyActions.save)
    {
        if (document_.LevelPath().empty())
        {
            beginSaveLevelAsDialog();
        }
        else
        {
            saveLevel(document_.LevelPath());
        }
    }
    if (hotkeyActions.duplicateSelection)
    {
        commandStack_.Execute(ctx, std::make_unique<DuplicateObjectCommand>(selectedObject_));
    }
    if (hotkeyActions.deleteSelection)
    {
        commandStack_.Execute(ctx, std::make_unique<DeleteObjectCommand>(selectedObject_));
    }
    if (hotkeyActions.focusSelection)
    {
        if (!FrameSelection(renderer, scene, document_, selectedObject_))
        {
            levelStatus_ = "Nothing to frame";
        }
    }
    if (hotkeyActions.dropSelectionToGround)
    {
        levelStatus_ = DropSelectionToGround(ctx, commandStack_);
    }
    if (hotkeyActions.clearSelection)
    {
        selectedObject_ = EditorObjectId{};
    }

    // Main editor window: status, undo/redo, and per-window visibility toggles.
    // Closing it (its X) closes the whole editor interface.
    ImGui::SetNextWindowSize(ImVec2(360.0f, 180.0f), ImGuiCond_FirstUseEver);
    bool open = open_;
    const char* editorWindowTitle = document_.IsDirty() ?
        "Level Editor *###LevelEditor" :
        "Level Editor###LevelEditor";
    if (ImGui::Begin(editorWindowTitle, &open, ImGuiWindowFlags_MenuBar))
    {
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("New Level"))
                {
                    newLevel();
                }
                if (ImGui::MenuItem("Open Level..."))
                {
                    beginOpenLevelDialog();
                }
                if (ImGui::MenuItem("Save Level"))
                {
                    if (document_.LevelPath().empty())
                    {
                        beginSaveLevelAsDialog();
                    }
                    else
                    {
                        saveLevel(document_.LevelPath());
                    }
                }
                if (ImGui::MenuItem("Save Level As..."))
                {
                    beginSaveLevelAsDialog();
                }
                if (MenuItemWithDisabledReason("Reload Level",
                        !document_.LevelPath().empty(),
                        "Save the level to a file before reloading it."))
                {
                    saveCurrentLevelCameraState(true);
                    const std::string reloadPath = NormalizeLevelPath(document_.LevelPath());
                    if (!reloadPath.empty())
                    {
                        queueLevelPathLoad(reloadPath, true, PendingLevelAction::Reload, "Reloading " + reloadPath);
                    }
                }
                const bool hasRecentLevels = !recentLevelPaths_.empty();
                if (ImGui::BeginMenu("Recent Levels", hasRecentLevels))
                {
                    const std::vector<std::string> recentSnapshot = recentLevelPaths_;
                    for (const std::string& recentPath : recentSnapshot)
                    {
                        if (ImGui::MenuItem(recentPath.c_str()))
                        {
                            openLevel(recentPath);
                        }
                    }
                    ImGui::EndMenu();
                }
                ShowDisabledItemTooltip(!hasRecentLevels, "No recently opened levels.");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Create"))
            {
                if (ImGui::BeginMenu("Camera"))
                {
                    if (ImGui::MenuItem("Free Camera Start"))
                    {
                        commandStack_.Execute(ctx, std::make_unique<CreateDocumentObjectCommand>(
                            BuildFreeCameraStartObject(scene)));
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Mesh"))
                {
                    const std::string defaultMesh = PickDefaultMesh(assetRegistry_);
                    if (MenuItemWithDisabledReason("Static Mesh", !defaultMesh.empty(),
                            "No mesh assets are available."))
                    {
                        commandStack_.Execute(ctx, std::make_unique<SpawnMeshCommand>(
                            BuildStaticMeshObjectJson(scene, assetRegistry_)));
                    }
                    if (MenuItemWithDisabledReason("Transparent Mesh", !defaultMesh.empty(),
                            "No mesh assets are available."))
                    {
                        commandStack_.Execute(ctx, std::make_unique<SpawnMeshCommand>(
                            BuildTransparentMeshObjectJson(scene, assetRegistry_)));
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Light"))
                {
                    const bool hasDirectionalLight = HasEnvironmentObject(document_, "directionalLight");
                    if (ImGui::MenuItem("Point Light"))
                    {
                        commandStack_.Execute(ctx, std::make_unique<CreateEnvironmentCommand>(
                            BuildPointLightObject(scene)));
                    }
                    if (ImGui::MenuItem("Spot Light"))
                    {
                        commandStack_.Execute(ctx, std::make_unique<CreateEnvironmentCommand>(
                            BuildSpotLightObject(scene)));
                    }
                    if (MenuItemWithDisabledReason("Directional Light", !hasDirectionalLight,
                            "Only one directional light is supported per level."))
                    {
                        commandStack_.Execute(ctx, std::make_unique<CreateEnvironmentCommand>(
                            BuildDirectionalLightObject()));
                    }
                    ImGui::EndMenu();
                }
                const bool hasSkybox = HasEnvironmentObject(document_, "skybox");
                if (MenuItemWithDisabledReason("Skybox", !hasSkybox,
                        "This level already has a skybox."))
                {
                    commandStack_.Execute(ctx, std::make_unique<CreateEnvironmentCommand>(
                        BuildSkyboxObject(assetRegistry_)));
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Ocean"))
            {
                bool hasOcean = false;
                for (const EditorObject& e : document_.Environment())
                {
                    if (e.type == "ocean") { hasOcean = true; break; }
                }
                if (MenuItemWithDisabledReason("Create Ocean", !hasOcean,
                        "This level already has an ocean."))
                {
                    const std::string preset = DefaultOceanPresetPath();
                    if (CreateLiveOcean(renderer, scene, preset, levelStatus_))
                    {
                        EditorObject oceanEntity;
                        oceanEntity.id = document_.AllocateId();
                        oceanEntity.type = "ocean";
                        oceanEntity.name = "Ocean";
                        oceanEntity.properties = nlohmann::json::object();
                        oceanEntity.properties["enabled"] = true;
                        oceanEntity.properties["preset"] = preset;
                        document_.Environment().push_back(std::move(oceanEntity));
                        document_.SetDirty(true);
                    }
                }
                if (MenuItemWithDisabledReason("Remove Ocean", hasOcean,
                        "This level has no ocean to remove."))
                {
                    DestroyLiveOcean(renderer, scene);
                    std::vector<EditorObject>& env = document_.Environment();
                    for (auto it = env.begin(); it != env.end(); ++it)
                    {
                        if (it->type == "ocean")
                        {
                            if (selectedObject_.value == it->id.value) { selectedObject_ = EditorObjectId{}; }
                            env.erase(it);
                            break;
                        }
                    }
                    document_.SetDirty(true);
                }
                ImGui::Separator();
                if (MenuItemWithDisabledReason("Open Preset Editor...", hasOcean,
                        "Create an ocean before opening its preset editor."))
                {
                    openOceanPresetEditorRequested_ = true;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        if (requestOpenLevelDialog || requestSaveLevelAsDialog)
        {
            const bool saveAs = requestSaveLevelAsDialog;
            levelFileDialogMode_ = saveAs ? LevelFileDialogMode::SaveAs : LevelFileDialogMode::Open;
            StartLevelFileDialog(saveAs, document_.LevelPath(), levelFileDialogDirectory_, levelFileDialogFileName_, levelFileDialogStatus_);
            ImGui::OpenPopup("Level File");
        }

        const std::string levelPath = NormalizeLevelPath(document_.LevelPath());
        ImGui::Text("Level: %s", levelPath.empty() ? "(untitled)" : levelPath.c_str());
        if (document_.IsDirty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.24f, 1.0f), "Unsaved Changes");
        }
        else
        {
            ImGui::TextDisabled("Saved");
        }
        ImGui::Text("Objects: %d", static_cast<int>(document_.Objects().size()));
        if (!levelStatus_.empty())
        {
            ImGui::TextDisabled("%s", levelStatus_.c_str());
        }


        if (levelFileDialogMode_ != LevelFileDialogMode::None)
        {
            ImGui::SetNextWindowSize(ImVec2(680.0f, 540.0f), ImGuiCond_Appearing);
            ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 440.0f), ImVec2(10000.0f, 10000.0f));
        }
        bool levelFileDialogOpen = levelFileDialogMode_ != LevelFileDialogMode::None;
        if (ImGui::BeginPopupModal("Level File", &levelFileDialogOpen, ImGuiWindowFlags_NoSavedSettings))
        {
            const bool saveAs = levelFileDialogMode_ == LevelFileDialogMode::SaveAs;
            const auto cancelDialog = [this]()
            {
                levelFileDialogMode_ = LevelFileDialogMode::None;
                levelFileDialogStatus_.clear();
                ImGui::CloseCurrentPopup();
            };

            ImGui::TextUnformatted(saveAs ? "Save Level As" : "Open Level");
            ImGui::Separator();

            ImGui::TextUnformatted("Directory");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##levelFileDirectory", levelFileDialogDirectory_, sizeof(levelFileDialogDirectory_)))
            {
                NormalizeTextBuffer(levelFileDialogDirectory_);
            }
            if (ImGui::Button("Up"))
            {
                const std::filesystem::path directoryPath(levelFileDialogDirectory_);
                const std::filesystem::path parentPath = directoryPath.parent_path();
                if (!parentPath.empty() && parentPath != directoryPath)
                {
                    SetTextBuffer(levelFileDialogDirectory_, parentPath.string());
                    levelFileDialogStatus_.clear();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Data Levels"))
            {
                SetTextBuffer(levelFileDialogDirectory_, DefaultLevelsDirectory());
                levelFileDialogStatus_.clear();
            }

            ImGui::TextUnformatted(saveAs ? "File name" : "File");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##levelFileName", levelFileDialogFileName_, sizeof(levelFileDialogFileName_)))
            {
                NormalizeTextBuffer(levelFileDialogFileName_);
            }

            std::string directoryStatus;
            const std::vector<FileDialogEntry> entries = ReadLevelDirectory(levelFileDialogDirectory_, directoryStatus);
            std::filesystem::path selectedPath = BuildDialogPath(levelFileDialogDirectory_, levelFileDialogFileName_);
            if (saveAs && levelFileDialogFileName_[0] != '\0')
            {
                selectedPath = EnsureJsonExtension(selectedPath);
            }
            const std::string selectedPathText = LevelPathString(selectedPath);
            const std::string shownStatus = levelFileDialogStatus_.empty() ? directoryStatus : levelFileDialogStatus_;
            const float footerLineHeight = ImGui::GetTextLineHeightWithSpacing();
            const float footerButtonHeight = ImGui::GetFrameHeightWithSpacing();
            const float footerReserveHeight = footerButtonHeight +
                footerLineHeight * (shownStatus.empty() ? 1.0f : 2.0f) +
                ImGui::GetStyle().ItemSpacing.y * 2.0f;
            const float fileListHeight = std::max(120.0f, ImGui::GetContentRegionAvail().y - footerReserveHeight);

            const auto commitDialog = [&]() -> bool
            {
                if (levelFileDialogFileName_[0] == '\0')
                {
                    levelFileDialogStatus_ = "Choose a level file";
                    return false;
                }

                std::filesystem::path path = BuildDialogPath(levelFileDialogDirectory_, levelFileDialogFileName_);
                if (saveAs)
                {
                    path = EnsureJsonExtension(path);
                }
                if (!HasJsonExtension(path))
                {
                    levelFileDialogStatus_ = "Level files must use .json";
                    return false;
                }

                if (!saveAs)
                {
                    std::error_code ec;
                    const bool exists = std::filesystem::exists(path, ec);
                    const bool regularFile = exists && std::filesystem::is_regular_file(path, ec);
                    if (ec || !regularFile)
                    {
                        levelFileDialogStatus_ = "Level file not found";
                        return false;
                    }
                }

                const std::string normalizedPath = LevelPathString(path);
                const bool ok = saveAs ? saveLevel(normalizedPath) : openLevel(normalizedPath);
                if (ok)
                {
                    levelFileDialogMode_ = LevelFileDialogMode::None;
                    levelFileDialogStatus_.clear();
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    levelFileDialogStatus_ = saveAs ? "Save failed" : "Open failed";
                }
                return ok;
            };

            ImGui::BeginChild("LevelFileEntries", ImVec2(0.0f, fileListHeight), true);
            for (const FileDialogEntry& entry : entries)
            {
                const std::string label = entry.directory ? ("[Dir] " + entry.name) : entry.name;
                const bool selected = !entry.directory && entry.name == levelFileDialogFileName_;
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    if (!entry.directory)
                    {
                        SetTextBuffer(levelFileDialogFileName_, entry.name);
                    }
                }

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    if (entry.directory)
                    {
                        const std::filesystem::path nextDirectory =
                            (std::filesystem::path(levelFileDialogDirectory_) / entry.name).lexically_normal();
                        SetTextBuffer(levelFileDialogDirectory_, nextDirectory.string());
                        SetTextBuffer(levelFileDialogFileName_, std::string());
                        levelFileDialogStatus_.clear();
                    }
                    else
                    {
                        SetTextBuffer(levelFileDialogFileName_, entry.name);
                        commitDialog();
                    }
                }
            }
            ImGui::EndChild();

            ImGui::TextDisabled("Path: %s", selectedPathText.c_str());
            if (!shownStatus.empty())
            {
                ImGui::TextDisabled("%s", shownStatus.c_str());
            }

            if (ImGui::Button(saveAs ? "Save" : "Open"))
            {
                commitDialog();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                cancelDialog();
            }
            if (!levelFileDialogOpen)
            {
                cancelDialog();
            }
            ImGui::EndPopup();
        }

        ImGui::BeginDisabled(!commandStack_.CanUndo());
        if (ImGui::Button("Undo")) { commandStack_.Undo(ctx); }
        ImGui::EndDisabled();
        ShowDisabledItemTooltip(!commandStack_.CanUndo(), "No command is available to undo.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!commandStack_.CanRedo());
        if (ImGui::Button("Redo")) { commandStack_.Redo(ctx); }
        ImGui::EndDisabled();
        ShowDisabledItemTooltip(!commandStack_.CanRedo(), "No command is available to redo.");

        ImGui::Separator();
        ImGui::TextUnformatted("Windows");
        const auto drawPanelToggle = [this](const char* panelId)
        {
            IEditorPanel* panel = extensions_.FindPanel(panelId);
            if (!panel || !panel->ShowInWindowList())
            {
                return;
            }

            bool visible = panel->IsVisible();
            const std::string label(panel->Label());
            if (ImGui::Checkbox(label.c_str(), &visible))
            {
                panel->SetVisible(visible);
            }
        };
        drawPanelToggle("contentBrowser");
        drawPanelToggle("sceneOutliner");
        drawPanelToggle("inspector");
        drawPanelToggle("commandHistory");

        ImGui::Separator();
        ImGui::TextUnformatted("Selection");
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderInt("Outline width", &selectionOutlineRadius_, 1, 8))
        {
            selectionOutlineRadius_ = std::clamp(selectionOutlineRadius_, 1, 8);
            SaveEditorState(recentLevelPaths_, selectionOutlineRadius_);
        }

        ImGui::Separator();
        viewportGizmo_.DrawModeButtons(EditorHotkeys::HintText());
    }
    ImGui::End();
    open_ = open;

    const auto drawPanel = [this, &ctx](const char* panelId)
    {
        IEditorPanel* panel = extensions_.FindPanel(panelId);
        if (panel && panel->IsVisible())
        {
            panel->Draw(ctx);
        }
    };

    // Each panel is registered, then drawn in the same order as before. The
    // viewport/gizmo panel runs after ImGui windows so mouse capture is current.
    drawPanel("sceneOutliner");
    drawPanel("contentBrowser");
    drawPanel("inspector");
    drawPanel("commandHistory");
    constexpr float kOpenDirtyConfirmContentWidth = 440.0f;
    if (confirmOpenLevelPopupRequested_)
    {
        ImGui::OpenPopup("Unsaved Changes###ContentBrowserOpenDirtyConfirm");
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 popupPos = ImGui::GetMousePos();
        popupPos.x += 12.0f;
        popupPos.y += 12.0f;
        if (viewport)
        {
            const float minX = viewport->WorkPos.x + 8.0f;
            const float minY = viewport->WorkPos.y + 8.0f;
            const float maxX = viewport->WorkPos.x + viewport->WorkSize.x -
                kOpenDirtyConfirmContentWidth - 40.0f;
            const float maxY = viewport->WorkPos.y + viewport->WorkSize.y - 170.0f;
            popupPos.x = std::clamp(popupPos.x, minX, std::max(minX, maxX));
            popupPos.y = std::clamp(popupPos.y, minY, std::max(minY, maxY));
        }
        ImGui::SetNextWindowPos(popupPos, ImGuiCond_Appearing);
        ImGui::SetNextWindowContentSize(ImVec2(kOpenDirtyConfirmContentWidth, 0.0f));
        confirmOpenLevelPopupRequested_ = false;
    }
    if (ImGui::BeginPopupModal("Unsaved Changes###ContentBrowserOpenDirtyConfirm",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("The current level has unsaved changes.");
        ImGui::TextWrapped("Loading another level will discard those changes.");
        ImGui::Separator();
        ImGui::Text("Level: %s", confirmOpenLevelPath_.c_str());
        if (ImGui::Button("Load Anyway"))
        {
            const std::string path = confirmOpenLevelPath_;
            const bool preserveCamera = confirmOpenLevelPreserveCamera_;
            confirmOpenLevelPath_.clear();
            confirmOpenLevelPreserveCamera_ = false;
            RequestOpenLevelPath(levelManager, path, preserveCamera, true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            confirmOpenLevelPath_.clear();
            confirmOpenLevelPreserveCamera_ = false;
            levelStatus_ = "Open level canceled";
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    drawPanel("viewportGizmo");
    selectionOutlineRadius_ = std::clamp(selectionOutlineRadius_, 1, 8);
    scene.SetEditorSelectionOutlineRadius(static_cast<std::uint32_t>(selectionOutlineRadius_));
    scene.SetSelectedEditorObjectId(open_ ? selectedObject_.value : 0);
    const nlohmann::json panelState = BuildPanelStateJson(showContentBrowser_, showOutliner_,
        showInspector_, showCommandHistory_, contentBrowser_, outliner_, viewportGizmo_);
    if (!panelStateLoaded_ || panelState != lastObservedPanelState_)
    {
        lastObservedPanelState_ = panelState;
        panelStateLoaded_ = true;
        panelStateDirty_ = true;
        nextPanelStateSaveTimeSec_ = ImGui::GetTime() + 0.25;
    }
    if (panelStateDirty_ && (!open_ || ImGui::GetTime() >= nextPanelStateSaveTimeSec_))
    {
        if (SaveEditorPanelState(lastObservedPanelState_))
        {
            panelStateDirty_ = false;
        }
        else
        {
            nextPanelStateSaveTimeSec_ = ImGui::GetTime() + 1.0;
        }
    }
    saveCurrentLevelCameraState(false);
}

#endif // WITH_EDITOR
