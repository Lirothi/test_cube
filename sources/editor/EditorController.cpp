#include "editor/EditorController.h"
#if WITH_EDITOR

#include <algorithm>
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
#include <vector>

#include "app/camera/Camera.h"
#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "editor/commands/DeleteObjectCommand.h"
#include "editor/commands/SetEnabledCommand.h"
#include "editor/commands/SpawnMeshCommand.h"
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

    // damaged_plaster if present, else the first material preset, else "".
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
                return "damaged_plaster";
            }
            if (!first)
            {
                first = &rec;
            }
        }
        return first ? first->id.key : std::string{};
    }

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
        return queueLevelPathLoad(normalizedPath, false, PendingLevelAction::Open, "Opening " + normalizedPath);
    };
    const auto saveLevel = [&](const std::string& path) -> bool
    {
        const std::string normalizedPath = NormalizeLevelPath(path);
        if (!LevelDocumentSerializer::SaveToFile(document_, normalizedPath))
        {
            levelStatus_ = "Save failed";
            return false;
        }

        saveCurrentLevelCameraState(true);
        return queueLevelPathLoad(normalizedPath, true, PendingLevelAction::Save, "Saving " + normalizedPath);
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

    // Main editor window: status, undo/redo, and per-window visibility toggles.
    // Closing it (its X) closes the whole editor interface.
    ImGui::SetNextWindowSize(ImVec2(360.0f, 180.0f), ImGuiCond_FirstUseEver);
    bool open = open_;
    if (ImGui::Begin("Level Editor###LevelEditor", &open, ImGuiWindowFlags_MenuBar))
    {
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("New"))
                {
                    newLevel();
                }
                if (ImGui::MenuItem("Open..."))
                {
                    beginOpenLevelDialog();
                }
                if (ImGui::MenuItem("Save"))
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
                if (ImGui::MenuItem("Save As..."))
                {
                    beginSaveLevelAsDialog();
                }
                ImGui::BeginDisabled(document_.LevelPath().empty());
                if (ImGui::MenuItem("Reload"))
                {
                    saveCurrentLevelCameraState(true);
                    const std::string reloadPath = NormalizeLevelPath(document_.LevelPath());
                    if (!reloadPath.empty())
                    {
                        queueLevelPathLoad(reloadPath, true, PendingLevelAction::Reload, "Reloading " + reloadPath);
                    }
                }
                ImGui::EndDisabled();
                if (ImGui::BeginMenu("Recent", !recentLevelPaths_.empty()))
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
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Ocean"))
            {
                bool hasOcean = false;
                for (const EditorObject& e : document_.Environment())
                {
                    if (e.type == "ocean") { hasOcean = true; break; }
                }
                if (ImGui::MenuItem("Create Ocean", nullptr, false, !hasOcean))
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
                if (ImGui::MenuItem("Remove Ocean", nullptr, false, hasOcean))
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
                if (ImGui::MenuItem("Preset Editor...", nullptr, false, hasOcean))
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
        ImGui::Text("Document objects: %d", static_cast<int>(document_.Objects().size()));
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
        ImGui::SameLine();
        ImGui::BeginDisabled(!commandStack_.CanRedo());
        if (ImGui::Button("Redo")) { commandStack_.Redo(ctx); }
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextUnformatted("Windows");
        ImGui::Checkbox("Content Browser", &showContentBrowser_);
        ImGui::Checkbox("Scene Outliner", &showOutliner_);
        ImGui::Checkbox("Inspector", &showInspector_);

        ImGui::Separator();
        ImGui::TextUnformatted("Selection");
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderInt("Outline width", &selectionOutlineRadius_, 1, 8))
        {
            selectionOutlineRadius_ = std::clamp(selectionOutlineRadius_, 1, 8);
            SaveEditorState(recentLevelPaths_, selectionOutlineRadius_);
        }

        ImGui::Separator();
        viewportGizmo_.DrawModeButtons();
    }
    ImGui::End();
    open_ = open;

    // Each panel is its own window, drawn only while the editor is open and its
    // visibility toggle is on. Panels draw their own window and return an action.
    if (showOutliner_)
    {
        const OutlinerAction outlinerAction = outliner_.Draw(document_, selectedObject_, &showOutliner_);
        if (outlinerAction.type == OutlinerAction::Type::DeleteObject)
        {
            commandStack_.Execute(ctx, std::make_unique<DeleteObjectCommand>(outlinerAction.target));
        }
        else if (outlinerAction.type == OutlinerAction::Type::SetEnabled)
        {
            commandStack_.Execute(ctx, std::make_unique<SetEnabledCommand>(outlinerAction.target, outlinerAction.enabledValue));
        }
        else if (outlinerAction.type == OutlinerAction::Type::SetEnvEnabled)
        {
            // Environment entities live in document_.Environment(), not objects_,
            // so this is a direct live-patch (non-undoable, like other env edits)
            // rather than a SetEnabledCommand.
            for (EditorObject& env : document_.Environment())
            {
                if (env.id.value == outlinerAction.target.value)
                {
                    EnvironmentRuntime::SetEnabled(ctx, env, outlinerAction.enabledValue);
                    break;
                }
            }
        }
    }

    if (showContentBrowser_)
    {
        const ContentBrowserAction action = contentBrowser_.Draw(assetRegistry_, selectedAsset_, &showContentBrowser_);
        if (action.type == ContentBrowserAction::Type::SpawnStaticMesh)
        {
            commandStack_.Execute(ctx, std::make_unique<SpawnMeshCommand>(
                SpawnMeshCommand::Kind::StaticMesh, action.asset.key, PickDefaultStaticMaterial(assetRegistry_)));
        }
        else if (action.type == ContentBrowserAction::Type::SpawnTransparentMesh)
        {
            commandStack_.Execute(ctx, std::make_unique<SpawnMeshCommand>(
                SpawnMeshCommand::Kind::TransparentMesh, action.asset.key, std::string{}));
        }
    }

    if (showInspector_)
    {
        inspector_.Draw(ctx, commandStack_, assetRegistry_, &showInspector_);
    }

    // Viewport gizmo + click-to-select (after panels so ImGui knows whether the
    // mouse is over an editor window this frame).
    viewportGizmo_.Update(ctx, commandStack_);
    selectionOutlineRadius_ = std::clamp(selectionOutlineRadius_, 1, 8);
    scene.SetEditorSelectionOutlineRadius(static_cast<std::uint32_t>(selectionOutlineRadius_));
    scene.SetSelectedEditorObjectId(open_ ? selectedObject_.value : 0);
    saveCurrentLevelCameraState(false);
}

#endif // WITH_EDITOR
