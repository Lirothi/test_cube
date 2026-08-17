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
#include "app/scene/SceneObjectFactory.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "editor/EditorContext.h"
#include "editor/commands/CreateDocumentObjectCommand.h"
#include "editor/commands/CreateEnvironmentCommand.h"
#include "editor/commands/CompositeCommand.h"
#include "editor/commands/DeleteObjectCommand.h"
#include "editor/commands/DuplicateObjectCommand.h"
#include "editor/commands/EditEnvironmentCommand.h"
#include "editor/commands/PasteObjectCommand.h"
#include "editor/commands/RenameObjectCommand.h"
#include "editor/commands/SetEnabledCommand.h"
#include "editor/commands/SetMaterialCommand.h"
#include "editor/commands/SpawnMeshCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "editor/scene/EnvironmentRuntime.h"
#include "editor/serialization/LevelDocumentSerializer.h"
#include "imgui.h"
#include "ocean/OceanRenderable.h"
#include "ocean/OceanRenderConfigJson.h"
#include "rendering/core/PhotographicSettingsJson.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/lighting/Skybox.h"
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

    constexpr std::size_t kCameraBookmarkCount = 9;

    struct CameraPose
    {
        Math::float3 position{ 0.0f, 0.0f, 0.0f };
        float yaw = 0.0f;
        float pitch = 0.0f;
        float roll = 0.0f;
    };

    struct CameraBookmark
    {
        bool stored = false;
        CameraPose pose{};
    };

    struct LevelCameraState : CameraPose
    {
        std::array<CameraBookmark, kCameraBookmarkCount> bookmarks{};
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
            if (rec.id.key == "models/box.mesh.json")
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

    nlohmann::json BuildParticleEmitterObjectJson(const Scene& scene)
    {
        // Inline default: a small buoyant additive puff (no texture -> procedural disc), so a
        // freshly created emitter is immediately visible for tuning. E4 supplies the fire/smoke/
        // sparks presets referenced via "preset" in level JSON.
        nlohmann::json o = nlohmann::json::object();
        o["name"] = "Particle Emitter";
        o["type"] = "particleEmitter";
        o["position"] = SpawnPositionJson(scene);
        o["maxParticles"] = 1024;
        o["spawnRate"] = 120.0f;
        o["lifetime"] = nlohmann::json::array({ 0.6f, 1.2f });
        o["speed"] = nlohmann::json::array({ 0.6f, 1.4f });
        o["gravity"] = -2.0f;
        o["coneAngleDeg"] = 25.0f;
        o["size"] = nlohmann::json::array({ 0.5f, 0.15f });
        o["additive"] = true;
        o["colorKeys"] = nlohmann::json::array({
            nlohmann::json::array({ 1.0f, 0.7f, 0.3f, 0.85f }),
            nlohmann::json::array({ 1.0f, 0.45f, 0.12f, 0.7f }),
            nlohmann::json::array({ 0.6f, 0.12f, 0.05f, 0.4f }),
            nlohmann::json::array({ 0.2f, 0.05f, 0.02f, 0.0f }),
        });
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
            // P4: a freshly created sun is authored in the new model. `exposure` is deliberately
            // absent -- it is the legacy whole-scene multiplier and writing it would only invite
            // someone to tune a control that no longer does anything once sunIntensity exists.
            { "sunIntensity", 1.0f },
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

    // The global wind entity. Defaults mirror vfx::WindState's authored fields (see ApplyWind in
    // EnvironmentRuntime for the exact keys it reads) with a gentle breeze rather than dead calm, so
    // creating it from the menu produces something visibly moving instead of a no-op entity. The
    // "gust" sub-object must be present or ApplyWind leaves the gust fields at their previous values.
    EditorObject BuildWindObject()
    {
        EditorObject wind;
        wind.name = "Wind";
        wind.type = "wind";
        wind.properties = {
            { "directionDeg", 40.0f },
            { "strength", 0.5f },
            { "swayFrequency", 0.9f },
            { "foliageSwayMeters", 0.35f },
            { "gust", {
                { "amplitude", 0.5f },
                { "frequencyHz", 0.15f },
                { "seed", 3.0f }
            } }
        };
        return wind;
    }

    // P1: the photographic camera section. Seeded from the documented struct defaults, which means
    // adding it to a level is a no-op on the image until someone ticks Enabled.
    EditorObject BuildCameraExposureObject()
    {
        const render::CameraExposureSettings defaults{};
        EditorObject exposure;
        exposure.name = "Camera Exposure";
        exposure.type = "cameraExposure";
        exposure.properties = render::PhotographicSettingsJson::ToJson(defaults);
        return exposure;
    }

    // P3C: the display transform section (tone curve + colour grade).
    EditorObject BuildColorPipelineObject()
    {
        const render::ColorPipelineSettings defaults{};
        EditorObject color;
        color.name = "Color Pipeline";
        color.type = "colorPipeline";
        color.properties = render::PhotographicSettingsJson::ToJson(defaults);
        return color;
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

    bool IsReasonableCameraPose(const CameraPose& cameraState)
    {
        constexpr float kMaxCameraDistance = 1000000.0f;
        constexpr float kMaxStoredYawPitch = Math::TWO_PI * 1024.0f;
        return IsFinite(cameraState.position) &&
            std::fabs(cameraState.position.x) <= kMaxCameraDistance &&
            std::fabs(cameraState.position.y) <= kMaxCameraDistance &&
            std::fabs(cameraState.position.z) <= kMaxCameraDistance &&
            std::fabs(cameraState.yaw) <= kMaxStoredYawPitch &&
            std::fabs(cameraState.pitch) <= kMaxStoredYawPitch &&
            std::fabs(cameraState.roll) <= kMaxStoredYawPitch;
    }

    LevelCameraState CaptureLevelCameraState(const Camera& camera)
    {
        LevelCameraState state;
        state.position = camera.GetPosition();
        state.yaw = camera.GetYaw();
        state.pitch = camera.GetPitch();
        state.roll = camera.GetRoll();
        return state;
    }

    bool CameraStateMatches(const LevelCameraState& lhs, const LevelCameraState& rhs)
    {
        constexpr float kEpsilon = 0.0001f;
        return std::fabs(lhs.position.x - rhs.position.x) <= kEpsilon &&
            std::fabs(lhs.position.y - rhs.position.y) <= kEpsilon &&
            std::fabs(lhs.position.z - rhs.position.z) <= kEpsilon &&
            std::fabs(lhs.yaw - rhs.yaw) <= kEpsilon &&
            std::fabs(lhs.pitch - rhs.pitch) <= kEpsilon &&
            std::fabs(lhs.roll - rhs.roll) <= kEpsilon;
    }

    nlohmann::json CameraPoseToJson(const CameraPose& cameraState)
    {
        return {
            { "position", nlohmann::json::array({
                cameraState.position.x,
                cameraState.position.y,
                cameraState.position.z }) },
            { "orientation", {
                { "yawRad", cameraState.yaw },
                { "pitchRad", cameraState.pitch },
                { "rollRad", cameraState.roll }
            } }
        };
    }

    bool TryReadCameraPose(const nlohmann::json& stateJson, CameraPose& out)
    {
        if (!stateJson.is_object())
        {
            return false;
        }

        CameraPose parsed;
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
        // Roll is OPTIONAL: state files written before the camera could bank have no rollRad, and
        // rejecting them would silently discard everyone's saved camera positions and bookmarks.
        const auto rollIt = orientationIt->find("rollRad");
        if (rollIt != orientationIt->end() && !TryReadFloat(*rollIt, parsed.roll))
        {
            return false;
        }
        if (!IsReasonableCameraPose(parsed))
        {
            return false;
        }

        out = parsed;
        return true;
    }

    nlohmann::json LevelCameraStateToJson(const std::string& levelPath, const LevelCameraState& cameraState)
    {
        nlohmann::json stateJson = CameraPoseToJson(cameraState);
        stateJson["level"] = levelPath;

        nlohmann::json bookmarks = nlohmann::json::object();
        for (std::size_t index = 0; index < cameraState.bookmarks.size(); ++index)
        {
            const CameraBookmark& bookmark = cameraState.bookmarks[index];
            if (bookmark.stored)
            {
                bookmarks[std::to_string(index + 1)] = CameraPoseToJson(bookmark.pose);
            }
        }
        if (!bookmarks.empty())
        {
            stateJson["bookmarks"] = std::move(bookmarks);
        }
        return stateJson;
    }

    bool TryReadLevelCameraState(const nlohmann::json& stateJson, LevelCameraState& out)
    {
        if (!stateJson.is_object())
        {
            return false;
        }

        LevelCameraState parsed;
        CameraPose primaryPose;
        if (!TryReadCameraPose(stateJson, primaryPose))
        {
            return false;
        }
        parsed.position = primaryPose.position;
        parsed.yaw = primaryPose.yaw;
        parsed.pitch = primaryPose.pitch;

        const auto bookmarksIt = stateJson.find("bookmarks");
        if (bookmarksIt != stateJson.end() && bookmarksIt->is_object())
        {
            for (std::size_t index = 0; index < parsed.bookmarks.size(); ++index)
            {
                const auto bookmarkIt = bookmarksIt->find(std::to_string(index + 1));
                CameraPose bookmarkPose;
                if (bookmarkIt != bookmarksIt->end() &&
                    TryReadCameraPose(*bookmarkIt, bookmarkPose))
                {
                    parsed.bookmarks[index].stored = true;
                    parsed.bookmarks[index].pose = bookmarkPose;
                }
            }
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
            { "sunIntensity", 1.0f }, // P4: new-model field, see BuildDirectionalLightObject
            { "ambient", 0.05f }
        };
        root["spotLights"] = nlohmann::json::array();
        root["pointLights"] = nlohmann::json::array();
        root["objects"] = nlohmann::json::array({
            {
                { "name", "Floor" },
                { "type", "staticMesh" },
                { "mesh", "models/box.mesh.json" },
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
            { "otherExpanded", state.otherGroupOpen },
            { "trackSelection", state.trackSelection }
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
        ReadBoolMember(value, "trackSelection", state.trackSelection);
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

    nlohmann::json MeshEditorStateToJson(const MeshEditorPanel::PersistentState& state)
    {
        return nlohmann::json{
            { "previewPaneRatio", state.previewPaneRatio },
            { "previewLight", {
                { "direction", {
                    state.previewLight.direction.x,
                    state.previewLight.direction.y,
                    state.previewLight.direction.z } },
                { "color", {
                    state.previewLight.color.x,
                    state.previewLight.color.y,
                    state.previewLight.color.z } },
                { "exposure", state.previewLight.exposure },
                { "ambient", state.previewLight.ambient },
                { "showPosition", state.previewLight.showPosition },
                { "positionDistance", state.previewLight.positionDistance }
            } }
        };
    }

    void LoadMeshEditorState(const nlohmann::json& value, MeshEditorPanel& meshEditor)
    {
        if (!value.is_object())
        {
            return;
        }

        MeshEditorPanel::PersistentState state = meshEditor.GetPersistentState();
        ReadFloatMember(value, "previewPaneRatio", 0.1f, 0.9f, state.previewPaneRatio);
        const auto lightIt = value.find("previewLight");
        if (lightIt != value.end() && lightIt->is_object())
        {
            const auto directionIt = lightIt->find("direction");
            if (directionIt != lightIt->end())
            {
                TryReadFloat3(*directionIt, state.previewLight.direction);
            }
            const auto colorIt = lightIt->find("color");
            if (colorIt != lightIt->end())
            {
                TryReadFloat3(*colorIt, state.previewLight.color);
            }
            ReadFloatMember(*lightIt, "exposure", 0.0f, 100.0f,
                state.previewLight.exposure);
            ReadFloatMember(*lightIt, "ambient", 0.0f, 10.0f,
                state.previewLight.ambient);
            ReadBoolMember(*lightIt, "showPosition", state.previewLight.showPosition);
            ReadFloatMember(*lightIt, "positionDistance", 0.25f, 8.0f,
                state.previewLight.positionDistance);
        }
        meshEditor.SetPersistentState(state);
    }

    nlohmann::json BuildPanelStateJson(bool showContentBrowser,
        bool showOutliner,
        bool showInspector,
        bool showCommandHistory,
        const ContentBrowserPanel& contentBrowser,
        const SceneOutlinerPanel& outliner,
        const MeshEditorPanel& meshEditor,
        const ViewportGizmo& viewportGizmo)
    {
        return nlohmann::json{
            { "contentBrowserVisible", showContentBrowser },
            { "outlinerVisible", showOutliner },
            { "inspectorVisible", showInspector },
            { "commandHistoryVisible", showCommandHistory },
            { "contentBrowser", ContentBrowserStateToJson(contentBrowser.GetPersistentState()) },
            { "outliner", OutlinerStateToJson(outliner.GetPersistentState()) },
            { "meshEditor", MeshEditorStateToJson(meshEditor.GetPersistentState()) },
            { "viewportGizmo", ViewportGizmoStateToJson(viewportGizmo.GetPersistentState()) }
        };
    }

    bool AssetIdsMatch(const std::vector<EditorAssetId>& a,
        const std::vector<EditorAssetId>& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }

        for (size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].type != b[i].type || a[i].key != b[i].key)
            {
                return false;
            }
        }
        return true;
    }

    bool CollectionsMatch(const std::vector<ContentBrowserCollection>& a,
        const std::vector<ContentBrowserCollection>& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }

        for (size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].name != b[i].name ||
                !AssetIdsMatch(a[i].assets, b[i].assets) ||
                a[i].folders != b[i].folders)
            {
                return false;
            }
        }
        return true;
    }

    bool ContentBrowserStatesMatch(const ContentBrowserPanel::PersistentState& a,
        const ContentBrowserPanel::PersistentState& b)
    {
        return a.activeTypeFilters == b.activeTypeFilters &&
            a.selectedFolder == b.selectedFolder &&
            a.includeSubfolders == b.includeSubfolders &&
            a.viewMode == b.viewMode &&
            a.sourcesWidth == b.sourcesWidth &&
            AssetIdsMatch(a.favoriteAssets, b.favoriteAssets) &&
            a.favoriteFolders == b.favoriteFolders &&
            CollectionsMatch(a.collections, b.collections);
    }

    bool OutlinerStatesMatch(const SceneOutlinerPanel::PersistentState& a,
        const SceneOutlinerPanel::PersistentState& b)
    {
        return a.meshesGroupOpen == b.meshesGroupOpen &&
            a.lightsGroupOpen == b.lightsGroupOpen &&
            a.camerasGroupOpen == b.camerasGroupOpen &&
            a.environmentGroupOpen == b.environmentGroupOpen &&
            a.otherGroupOpen == b.otherGroupOpen &&
            a.trackSelection == b.trackSelection;
    }

    bool ViewportGizmoStatesMatch(const ViewportGizmo::PersistentState& a,
        const ViewportGizmo::PersistentState& b)
    {
        return a.snapEnabled == b.snapEnabled &&
            a.translationIncrement == b.translationIncrement &&
            a.rotationIncrement == b.rotationIncrement &&
            a.scaleIncrement == b.scaleIncrement &&
            a.transformSpace == b.transformSpace;
    }

    bool MeshEditorStatesMatch(const MeshEditorPanel::PersistentState& a,
        const MeshEditorPanel::PersistentState& b)
    {
        return a.previewPaneRatio == b.previewPaneRatio &&
            a.previewLight.direction.x == b.previewLight.direction.x &&
            a.previewLight.direction.y == b.previewLight.direction.y &&
            a.previewLight.direction.z == b.previewLight.direction.z &&
            a.previewLight.color.x == b.previewLight.color.x &&
            a.previewLight.color.y == b.previewLight.color.y &&
            a.previewLight.color.z == b.previewLight.color.z &&
            a.previewLight.exposure == b.previewLight.exposure &&
            a.previewLight.ambient == b.previewLight.ambient &&
            a.previewLight.showPosition == b.previewLight.showPosition &&
            a.previewLight.positionDistance == b.previewLight.positionDistance;
    }

    void LoadEditorPanelState(bool& showContentBrowser,
        bool& showOutliner,
        bool& showInspector,
        bool& showCommandHistory,
        ContentBrowserPanel& contentBrowser,
        SceneOutlinerPanel& outliner,
        MeshEditorPanel& meshEditor,
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
        const auto meshEditorIt = panelState.find("meshEditor");
        if (meshEditorIt != panelState.end())
        {
            LoadMeshEditorState(*meshEditorIt, meshEditor);
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

    std::array<bool, kCameraBookmarkCount> LoadCameraBookmarkSlots(const std::string& levelPath)
    {
        std::array<bool, kCameraBookmarkCount> slots{};
        LevelCameraState state;
        if (!LoadLevelCameraState(levelPath, state))
        {
            return slots;
        }

        for (std::size_t index = 0; index < slots.size(); ++index)
        {
            slots[index] = state.bookmarks[index].stored;
        }
        return slots;
    }

    bool WriteLevelCameraState(const std::string& levelPath, const LevelCameraState& cameraState)
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

        const nlohmann::json stateJson = LevelCameraStateToJson(normalizedPath, cameraState);
        levelEditor["levelCameraStates"][normalizedPath] = stateJson;
        levelEditor["lastLevelCamera"] = stateJson;
        return SaveEditorStateJson(root);
    }

    bool SaveLevelCameraState(const std::string& levelPath, const Camera& camera)
    {
        const std::string normalizedPath = NormalizeLevelPath(levelPath);
        if (normalizedPath.empty())
        {
            return false;
        }

        LevelCameraState state;
        LoadLevelCameraState(normalizedPath, state);
        const LevelCameraState current = CaptureLevelCameraState(camera);
        state.position = current.position;
        state.yaw = current.yaw;
        state.pitch = current.pitch;
        return WriteLevelCameraState(normalizedPath, state);
    }

    bool StoreCameraBookmark(const std::string& levelPath, int slot, const Camera& camera)
    {
        if (slot < 0 || static_cast<std::size_t>(slot) >= kCameraBookmarkCount)
        {
            return false;
        }

        const std::string normalizedPath = NormalizeLevelPath(levelPath);
        if (normalizedPath.empty())
        {
            return false;
        }

        LevelCameraState state;
        LoadLevelCameraState(normalizedPath, state);
        const LevelCameraState current = CaptureLevelCameraState(camera);
        state.position = current.position;
        state.yaw = current.yaw;
        state.pitch = current.pitch;
        state.bookmarks[static_cast<std::size_t>(slot)].stored = true;
        state.bookmarks[static_cast<std::size_t>(slot)].pose = current;
        return WriteLevelCameraState(normalizedPath, state);
    }

    bool RecallCameraBookmark(Renderer& renderer,
        Scene& scene,
        const std::string& levelPath,
        int slot)
    {
        if (slot < 0 || static_cast<std::size_t>(slot) >= kCameraBookmarkCount)
        {
            return false;
        }

        LevelCameraState state;
        if (!LoadLevelCameraState(levelPath, state))
        {
            return false;
        }

        const CameraBookmark& bookmark = state.bookmarks[static_cast<std::size_t>(slot)];
        if (!bookmark.stored)
        {
            return false;
        }

        Camera& camera = scene.CameraRef();
        camera.SetPosition(bookmark.pose.position);
        camera.SetYawPitchRoll(bookmark.pose.yaw, bookmark.pose.pitch, bookmark.pose.roll);
        camera.CalcMatrices(&renderer);
        camera.ResetHistory();
        return true;
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
        camera.SetYawPitchRoll(cameraState.yaw, cameraState.pitch, cameraState.roll);
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

        options.cameraOverride =
            LevelCameraOverride{ cameraState.position, cameraState.yaw, cameraState.pitch, cameraState.roll };
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

    bool FrameSelection(Renderer& renderer,
        Scene& scene,
        const EditorSceneDocument& document,
        const EditorSelection& selection)
    {
        Math::float3 firstCenter;
        float firstRadius = 1.0f;
        Math::float3 boundsMin;
        Math::float3 boundsMax;
        std::size_t validTargetCount = 0;
        for (const EditorObjectId id : selection.Ordered())
        {
            Math::float3 center;
            float radius = 1.0f;
            if (!TryGetSelectionFrameTarget(scene, document, id, center, radius))
            {
                continue;
            }

            if (validTargetCount == 0)
            {
                firstCenter = center;
                firstRadius = radius;
                boundsMin = center - Math::float3(radius);
                boundsMax = center + Math::float3(radius);
            }
            else
            {
                boundsMin.x = std::min(boundsMin.x, center.x - radius);
                boundsMin.y = std::min(boundsMin.y, center.y - radius);
                boundsMin.z = std::min(boundsMin.z, center.z - radius);
                boundsMax.x = std::max(boundsMax.x, center.x + radius);
                boundsMax.y = std::max(boundsMax.y, center.y + radius);
                boundsMax.z = std::max(boundsMax.z, center.z + radius);
            }
            ++validTargetCount;
        }

        if (validTargetCount == 0)
        {
            return false;
        }

        Math::float3 center = firstCenter;
        float radius = firstRadius;
        if (validTargetCount > 1)
        {
            center = (boundsMin + boundsMax) * 0.5f;
            radius = std::max((boundsMax - boundsMin).Length() * 0.5f, 1.0f);
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

    bool FrameVisibleScene(Renderer& renderer, Scene& scene, const EditorSceneDocument& document)
    {
        EditorSelection visibleObjects;
        for (const EditorObject& object : document.Objects())
        {
            const RenderableObjectBase* runtime = scene.FindEditorObject(object.id.value);
            if (object.enabled && runtime && runtime->IsVisible())
            {
                visibleObjects.Add(object.id, false);
            }
        }
        return FrameSelection(renderer, scene, document, visibleObjects);
    }

    std::string SnapSelectionToSurfaceBelow(EditorContext& ctx, EditorCommandStack& commandStack)
    {
        if (ctx.selection.Empty())
        {
            return "Select a mesh to snap below";
        }

        std::vector<Scene::SceneObjectId> ignoredObjectIds;
        ignoredObjectIds.reserve(ctx.selection.Size());
        for (const EditorObjectId id : ctx.selection.Ordered())
        {
            ignoredObjectIds.push_back(id.value);
        }

        const Math::float3 rayDirection(0.0f, -1.0f, 0.0f);
        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(ctx.selection.Size());
        std::size_t meshCount = 0;
        std::size_t alreadyRestingCount = 0;
        std::size_t noSurfaceCount = 0;

        for (const EditorObjectId id : ctx.selection.Ordered())
        {
            EditorObject* object = ctx.document.Find(id);
            RenderableObjectBase* runtime = ctx.scene.FindEditorObject(id.value);
            if (!object || !runtime || !runtime->AsRenderableObject())
            {
                continue;
            }
            ++meshCount;

            const AABB& bounds = runtime->GetWorldBounds();
            if (!bounds.IsValid())
            {
                ++noSurfaceCount;
                continue;
            }

            const Math::float3 center = bounds.GetCenter();
            const Math::float3 rayOrigin(center.x, bounds.GetMin().y, center.z);
            float hitDistance = 0.0f;
            const Scene::SceneObjectId hit = ctx.scene.RaycastEditorObject(
                rayOrigin, rayDirection, &hitDistance, 0, &ignoredObjectIds);
            if (hit == 0 || !std::isfinite(hitDistance))
            {
                ++noSurfaceCount;
                continue;
            }
            if (hitDistance <= 1.0e-4f)
            {
                ++alreadyRestingCount;
                continue;
            }

            EditorTransform after = object->transform;
            after.position.y -= hitDistance;
            commands.push_back(std::make_unique<TransformObjectCommand>(
                object->id, object->transform, after));
        }

        if (meshCount == 0)
        {
            return "Select one or more meshes to snap below";
        }
        if (commands.empty())
        {
            if (alreadyRestingCount == meshCount)
            {
                return meshCount == 1 ?
                    "Selection is already resting on a surface" :
                    "Selected meshes are already resting on surfaces";
            }
            return "No visible editor object below selected meshes";
        }

        const std::size_t snappedCount = commands.size();
        bool snapped = false;
        if (commands.size() == 1)
        {
            snapped = commandStack.Execute(ctx, std::move(commands.front()));
        }
        else
        {
            auto composite = std::make_unique<CompositeCommand>(
                "Snap " + std::to_string(commands.size()) + " Objects to Surface Below");
            for (std::unique_ptr<EditorCommand>& command : commands)
            {
                composite->Add(std::move(command));
            }
            snapped = commandStack.Execute(ctx, std::move(composite));
        }
        if (!snapped)
        {
            return "Snap to surface below failed";
        }

        std::string status = "Snapped " + std::to_string(snappedCount) +
            (snappedCount == 1 ? " object" : " objects") + " to surfaces below";
        const std::size_t skippedCount = alreadyRestingCount + noSurfaceCount;
        if (skippedCount > 0)
        {
            status += "; skipped " + std::to_string(skippedCount);
        }
        return status;
    }

    const EditorObject* FindEnvironmentObject(
        const EditorSceneDocument& document,
        EditorObjectId id)
    {
        for (const EditorObject& environment : document.Environment())
        {
            if (environment.id.value == id.value)
            {
                return &environment;
            }
        }
        return nullptr;
    }

    bool TryBuildClipboardObject(const EditorSceneDocument& document,
        EditorObjectId selection,
        nlohmann::json& outJson,
        std::string& outName,
        std::string& outStatus)
    {
        if (const EditorObject* object = document.Find(selection))
        {
            if (object->type == "ocean")
            {
                outStatus = "Ocean objects cannot be copied";
                return false;
            }
            outJson = EditorSceneDocument::ObjectToJson(*object);
            outName = object->name;
            return true;
        }

        if (const EditorObject* environment = FindEnvironmentObject(document, selection))
        {
            if (environment->type != "pointLight" && environment->type != "spotLight")
            {
                outStatus = environment->name.empty() ?
                    "Selected environment entity cannot be copied" :
                    environment->name + " cannot be copied";
                return false;
            }

            outJson = environment->properties;
            outJson["id"] = environment->id.value;
            outJson["name"] = environment->name;
            outJson["type"] = environment->type;
            outName = environment->name;
            return true;
        }

        outStatus = "Nothing selected to copy";
        return false;
    }

    bool CopySelectionToClipboard(const EditorSceneDocument& document,
        const EditorSelection& selection,
        std::string& editorClipboard,
        std::string& outStatus)
    {
        if (selection.Empty())
        {
            outStatus = "Nothing selected to copy";
            return false;
        }

        nlohmann::json objects = nlohmann::json::array();
        std::string copiedName;
        for (const EditorObjectId id : selection.Ordered())
        {
            nlohmann::json objectJson;
            std::string objectName;
            if (!TryBuildClipboardObject(document, id, objectJson, objectName, outStatus))
            {
                return false;
            }
            objects.push_back(std::move(objectJson));
            if (copiedName.empty())
            {
                copiedName = std::move(objectName);
            }
        }

        nlohmann::json clipboardJson;
        if (objects.size() == 1)
        {
            clipboardJson = std::move(objects[0]);
        }
        else
        {
            clipboardJson = {
                { "editorClipboard", "objectSelection" },
                { "version", 1 },
                { "objects", std::move(objects) }
            };
        }

        editorClipboard = clipboardJson.dump();
        ImGui::SetClipboardText(editorClipboard.c_str());
        outStatus = selection.Size() == 1 ?
            (copiedName.empty() ? "Copied object" : "Copied " + copiedName) :
            "Copied " + std::to_string(selection.Size()) + " objects";
        return true;
    }

    bool PasteObjectFromClipboard(EditorContext& ctx,
        EditorCommandStack& commandStack,
        std::string& editorClipboard,
        std::string& outStatus)
    {
        const char* osClipboard = ImGui::GetClipboardText();
        std::string clipboardText = osClipboard && osClipboard[0] != '\0' ?
            std::string(osClipboard) : editorClipboard;
        if (clipboardText.empty())
        {
            outStatus = "Nothing to paste";
            return false;
        }

        nlohmann::json clipboardJson =
            nlohmann::json::parse(clipboardText, nullptr, false);
        if (clipboardJson.is_discarded())
        {
            outStatus = "Paste failed: clipboard does not contain valid JSON";
            return false;
        }

        std::vector<nlohmann::json> objects;
        if (clipboardJson.is_object() &&
            clipboardJson.value("editorClipboard", std::string()) == "objectSelection" &&
            clipboardJson.value("version", 0) == 1)
        {
            const auto objectsIt = clipboardJson.find("objects");
            if (objectsIt == clipboardJson.end() || !objectsIt->is_array() || objectsIt->empty())
            {
                outStatus = "Paste failed: object selection is empty";
                return false;
            }
            for (const nlohmann::json& objectJson : *objectsIt)
            {
                objects.push_back(objectJson);
            }
        }
        else
        {
            objects.push_back(std::move(clipboardJson));
        }

        for (const nlohmann::json& objectJson : objects)
        {
            std::string reason;
            if (!PasteObjectCommand::Validate(objectJson, reason))
            {
                outStatus = "Paste failed: " + reason;
                return false;
            }
        }

        bool pasted = false;
        if (objects.size() == 1)
        {
            pasted = commandStack.Execute(ctx,
                std::make_unique<PasteObjectCommand>(objects.front()));
        }
        else
        {
            auto composite = std::make_unique<CompositeCommand>(
                "Paste " + std::to_string(objects.size()) + " Objects");
            for (std::size_t i = 0; i < objects.size(); ++i)
            {
                composite->Add(std::make_unique<PasteObjectCommand>(objects[i], i != 0));
            }
            pasted = commandStack.Execute(ctx, std::move(composite));
        }

        if (!pasted)
        {
            outStatus = "Paste failed: object could not be created";
            return false;
        }

        editorClipboard = std::move(clipboardText);
        outStatus = objects.size() == 1 ?
            "Pasted " + objects.front().value("type", std::string("object")) :
            "Pasted " + std::to_string(objects.size()) + " objects";
        return true;
    }

    bool IsBulkObjectSupported(const EditorSceneDocument& document, EditorObjectId id)
    {
        if (const EditorObject* object = document.Find(id))
        {
            return object->type != "ocean";
        }
        const EditorObject* environment = FindEnvironmentObject(document, id);
        return environment &&
            (environment->type == "pointLight" || environment->type == "spotLight");
    }

    bool DuplicateSelection(EditorContext& ctx,
        EditorCommandStack& commandStack,
        std::string& outStatus)
    {
        if (ctx.selection.Empty())
        {
            outStatus = "Nothing selected to duplicate";
            return false;
        }
        for (const EditorObjectId id : ctx.selection.Ordered())
        {
            if (!IsBulkObjectSupported(ctx.document, id))
            {
                outStatus = "Selection contains an object that cannot be duplicated";
                return false;
            }
        }

        if (ctx.selection.Size() == 1)
        {
            return commandStack.Execute(ctx,
                std::make_unique<DuplicateObjectCommand>(ctx.selection.Primary()));
        }

        auto composite = std::make_unique<CompositeCommand>(
            "Duplicate " + std::to_string(ctx.selection.Size()) + " Objects");
        std::size_t index = 0;
        for (const EditorObjectId id : ctx.selection.Ordered())
        {
            composite->Add(std::make_unique<DuplicateObjectCommand>(id, index != 0));
            ++index;
        }
        return commandStack.Execute(ctx, std::move(composite));
    }

    bool DeleteSelection(EditorContext& ctx,
        EditorCommandStack& commandStack,
        std::string& outStatus)
    {
        if (ctx.selection.Empty())
        {
            outStatus = "Nothing selected to delete";
            return false;
        }
        for (const EditorObjectId id : ctx.selection.Ordered())
        {
            if (!IsBulkObjectSupported(ctx.document, id))
            {
                outStatus = "Selection contains an object that cannot be deleted";
                return false;
            }
        }

        if (ctx.selection.Size() == 1)
        {
            return commandStack.Execute(ctx,
                std::make_unique<DeleteObjectCommand>(ctx.selection.Primary()));
        }

        auto composite = std::make_unique<CompositeCommand>(
            "Delete " + std::to_string(ctx.selection.Size()) + " Objects");
        for (const EditorObjectId id : ctx.selection.Ordered())
        {
            composite->Add(std::make_unique<DeleteObjectCommand>(id));
        }
        return commandStack.Execute(ctx, std::move(composite));
    }

    std::unique_ptr<EditorCommand> BuildSetEnabledCommand(
        const EditorSceneDocument& document,
        EditorObjectId id,
        bool enabled)
    {
        if (document.Find(id))
        {
            return std::make_unique<SetEnabledCommand>(id, enabled);
        }

        const EditorObject* environment = FindEnvironmentObject(document, id);
        if (!environment || (environment->type != "pointLight" &&
            environment->type != "spotLight" &&
            environment->type != "directionalLight" &&
            environment->type != "ocean"))
        {
            return nullptr;
        }

        nlohmann::json after = environment->properties;
        after["enabled"] = enabled;
        return std::make_unique<EditEnvironmentCommand>(
            id,
            environment->properties,
            std::move(after),
            enabled ? "Enable Environment" : "Disable Environment");
    }

    bool SetSelectionEnabled(EditorContext& ctx,
        EditorCommandStack& commandStack,
        bool enabled,
        std::string& outStatus)
    {
        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(ctx.selection.Size());
        for (const EditorObjectId id : ctx.selection.Ordered())
        {
            std::unique_ptr<EditorCommand> command =
                BuildSetEnabledCommand(ctx.document, id, enabled);
            if (!command)
            {
                outStatus = "Selection contains an object that cannot be enabled or disabled";
                return false;
            }
            commands.push_back(std::move(command));
        }

        if (commands.empty())
        {
            outStatus = "Nothing selected";
            return false;
        }
        if (commands.size() == 1)
        {
            return commandStack.Execute(ctx, std::move(commands.front()));
        }

        auto composite = std::make_unique<CompositeCommand>(
            std::string(enabled ? "Enable " : "Disable ") +
            std::to_string(commands.size()) + " Objects");
        for (std::unique_ptr<EditorCommand>& command : commands)
        {
            composite->Add(std::move(command));
        }
        return commandStack.Execute(ctx, std::move(composite));
    }
}

std::string EditorController::LoadLastOpenedLevelPath()
{
    const nlohmann::json root = LoadEditorStateJson();
    const auto levelEditorIt = root.find("levelEditor");
    if (levelEditorIt == root.end() || !levelEditorIt->is_object())
    {
        return {};
    }

    const auto recentIt = levelEditorIt->find("recentLevels");
    if (recentIt == levelEditorIt->end() || !recentIt->is_array() || recentIt->empty() ||
        !(*recentIt)[0].is_string())
    {
        return {};
    }

    const std::string path = NormalizeLevelPath((*recentIt)[0].get<std::string>());
    if (!LevelFileExists(path))
    {
        return {};
    }

    // JsonLevel::Load currently has a void contract, so validate here before choosing the boot
    // path. Otherwise malformed remembered JSON would leave the initial scene half-loaded and the
    // LevelManager could not report the failure back to App::InitScene for a demo-level fallback.
    std::ifstream file(path, std::ios::binary);
    const nlohmann::json level = nlohmann::json::parse(
        file,
        nullptr,
        false,
        /*ignore_comments=*/true);
    return level.is_object() ? path : std::string();
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
    selection_.Clear();

    if (completedAction == PendingLevelAction::New)
    {
        ResetDocumentFromGeneratedLevel(document_, std::string(), pendingNewLevelJson_);
        pendingNewLevelJson_ = nlohmann::json();
        cameraBookmarkSlots_.fill(false);
        levelStatus_ = "New unsaved level";
        lastSavedCameraStateValid_ = false;
        return;
    }

    pendingNewLevelJson_ = nlohmann::json();

    const std::string levelPath = NormalizeLevelPath(document_.LevelPath());
    cameraBookmarkSlots_ = LoadCameraBookmarkSlots(levelPath);
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

void EditorController::RefreshAssetErrors()
{
    const bool wasClean = assetErrors_.empty();
    assetErrors_.clear();
    // ONE cache for the whole pass: without it every object re-opens and re-parses the same
    // mesh.json and material .json files. Measured at 926 ms for a single pass over wind_test.
    SceneObjectFactory::MeshAssetScanCache scanCache;
    for (const EditorObject& obj : document_.Objects())
    {
        if (obj.type != "staticMesh" && obj.type != "transparentMesh") { continue; }
        std::vector<std::string> errs =
            SceneObjectFactory::MeshAssetErrors(EditorSceneDocument::ObjectToJson(obj), &scanCache);
        if (!errs.empty()) { assetErrors_.emplace(obj.id.value, std::move(errs)); }
    }
    // Pop the window open the moment a scan first surfaces problems (e.g. right after a level load).
    if (wasClean && !assetErrors_.empty()) { showLevelErrors_ = true; }
}

void EditorController::RefreshAssetErrorsIfStale()
{
    CPU_SCOPE(ProfilerScopes::kEditorAssetErrorsScan);

    const std::uint64_t v = document_.ContentVersion();
    const std::string& lvl = document_.LevelPath();
    const std::size_t n = document_.Objects().size();
    if (v == assetErrorsVersion_ && lvl == assetErrorsLevel_ && n == assetErrorsCount_) { return; }

    // DEBOUNCED, and that is the whole point. RefreshAssetErrors serialises EVERY mesh object to
    // JSON and then stats the filesystem for each of its assets — while `ContentVersion` bumps on
    // every inspector edit, i.e. once per frame for the whole time a slider is being dragged.
    // Measured on wind_test: 930 ms of a 946 ms frame. demo was tolerable only because it has far
    // fewer mesh objects, which is exactly the level-dependence that gave this away.
    //
    // Structural changes (a different level, objects added or removed) still scan immediately —
    // those are one-shot and the error list must be right at once. A value edit waits until the
    // user stops moving, because a scan mid-drag is thrown away by the next frame anyway.
    const bool structural = (lvl != assetErrorsLevel_) || (n != assetErrorsCount_);
    if (!structural)
    {
        constexpr double kQuietSeconds = 0.25;
        const double now = ImGui::GetTime();
        if (v != assetErrorsPendingVersion_)
        {
            assetErrorsPendingVersion_ = v;
            assetErrorsDueTimeSec_ = now + kQuietSeconds;
            return;
        }
        if (now < assetErrorsDueTimeSec_)
        {
            return;
        }
    }

    assetErrorsVersion_ = v; assetErrorsLevel_ = lvl; assetErrorsCount_ = n;
    assetErrorsPendingVersion_ = v;
    RefreshAssetErrors();
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

EditorController::PanelStateSnapshot EditorController::CapturePanelState() const
{
    CPU_SCOPE(ProfilerScopes::kEditorPanelStateCapture);
    return PanelStateSnapshot{
        showContentBrowser_,
        showOutliner_,
        showInspector_,
        showCommandHistory_,
        contentBrowser_.GetPersistentState(),
        outliner_.GetPersistentState(),
        meshEditor_.GetPersistentState(),
        viewportGizmo_.GetPersistentState()
    };
}

bool EditorController::PanelStateMatches(const PanelStateSnapshot& a,
    const PanelStateSnapshot& b)
{
    return a.showContentBrowser == b.showContentBrowser &&
        a.showOutliner == b.showOutliner &&
        a.showInspector == b.showInspector &&
        a.showCommandHistory == b.showCommandHistory &&
        ContentBrowserStatesMatch(a.contentBrowser, b.contentBrowser) &&
        OutlinerStatesMatch(a.outliner, b.outliner) &&
        MeshEditorStatesMatch(a.meshEditor, b.meshEditor) &&
        ViewportGizmoStatesMatch(a.viewportGizmo, b.viewportGizmo);
}

void EditorController::Draw(Renderer& renderer, Scene& scene, LevelManager& levelManager)
{
    CPU_SCOPE(ProfilerScopes::kEditorDraw);
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
        nextAssetRegistryPollTimeSec_ = ImGui::GetTime() + 2.0;
        LoadEditorState(recentLevelPaths_, selectionOutlineRadius_);
        LoadEditorPanelState(showContentBrowser_, showOutliner_, showInspector_, showCommandHistory_,
            contentBrowser_, outliner_, meshEditor_, viewportGizmo_);
        lastObservedPanelStateSnapshot_ = CapturePanelState();
        lastObservedPanelState_ = BuildPanelStateJson(showContentBrowser_, showOutliner_,
            showInspector_, showCommandHistory_, contentBrowser_, outliner_, meshEditor_, viewportGizmo_);
        panelStateLoaded_ = true;
        const std::string activeLevelPath =
            NormalizeLevelPath(std::string(levelManager.GetActiveLevelSourcePath()));
        if (!activeLevelPath.empty() && document_.LoadFromLevelFile(activeLevelPath))
        {
            if (RestoreLevelCameraState(renderer, scene, document_.LevelPath()))
            {
                markCameraStateSaved(document_.LevelPath(), CaptureLevelCameraState(scene.CameraRef()));
            }
            cameraBookmarkSlots_ = LoadCameraBookmarkSlots(document_.LevelPath());
            levelStatus_ = "Loaded " + NormalizeLevelPath(document_.LevelPath());
        }
        else if (activeLevelPath.empty())
        {
            levelStatus_ = "Active level has no source path";
        }
        else
        {
            levelStatus_ = "Failed to load " + activeLevelPath;
        }
        firstOpenInitialized_ = true;
    }

    selectionOutlineRadius_ = std::clamp(selectionOutlineRadius_, 1, 8);
    scene.SetEditorSelectionOutlineRadius(static_cast<std::uint32_t>(selectionOutlineRadius_));
    std::vector<Scene::SceneObjectId> selectedSceneObjects;
    if (open_)
    {
        selectedSceneObjects.reserve(selection_.Size());
        for (const EditorObjectId id : selection_.Ordered())
        {
            selectedSceneObjects.push_back(id.value);
        }
    }
    scene.SetSelectedEditorObjectIds(selectedSceneObjects);
    if (!open_)
    {
        return;
    }

    const double now = ImGui::GetTime();
    if (now >= nextAssetRegistryPollTimeSec_)
    {
        CPU_SCOPE(ProfilerScopes::kEditorAssetRegistryPoll);
        nextAssetRegistryPollTimeSec_ = now + 2.0;
        if (assetRegistry_.HasChangedOnDisk())
        {
            assetRegistry_.Refresh();
            if (!selectedAsset_.key.empty() && !assetRegistry_.FindById(selectedAsset_))
            {
                selectedAsset_ = {};
            }
            contentBrowser_.NotifyAutoRefresh(now);
        }
    }

    EditorContext ctx{ renderer, scene, levelManager, document_, selection_ };
    ctx.openMeshEditor = [this](const std::string& meshPath)
    {
        meshEditor_.Open(meshPath);
        showMeshEditor_ = true;
    };
    ctx.openMaterialEditor = [this](const std::string& materialName, const std::string& materialPath)
    {
        materialEditor_.Open(materialName, materialPath);
        showMaterialEditor_ = true;
    };
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
                const Skybox* skybox = panelCtx.scene.GetSkybox();
                std::string skyboxPath;
                std::uint64_t skyboxWriteTime = 0;
                if (skybox)
                {
                    skyboxPath = std::filesystem::path(skybox->GetPath()).generic_string();
                    if (const EditorAssetRecord* skyboxAsset =
                            assetRegistry_.FindByPath(skyboxPath))
                    {
                        skyboxWriteTime = skyboxAsset->fileWriteTime;
                    }
                }
                thumbnailCache_.SetEnvironment(
                    skybox ? skybox->GetTex() : nullptr,
                    skyboxPath,
                    skyboxWriteTime,
                    skybox ? skybox->GetExposure() : 1.0f);

                const ContentBrowserAction action =
                    contentBrowser_.Draw(assetRegistry_, selectedAsset_, extensions_,
                        document_, selection_.Primary(), panelCtx.renderer, thumbnailCache_,
                        &showContentBrowser_);
                if (!action.HasAction())
                {
                    return;
                }

                // Opening the importer needs no asset — handle before the asset lookup.
                if (action.type == ContentBrowserAction::Type::OpenImportWindow)
                {
                    showImportPanel_ = true;
                    return;
                }

                const EditorAssetRecord* asset = assetRegistry_.FindById(action.asset);
                if (!asset)
                {
                    return;
                }

                if (action.type == ContentBrowserAction::Type::ReimportAsset)
                {
                    showImportPanel_ = true;
                    importPanel_.BeginReimport(*asset, assetRegistry_);
                }
                else if (action.type == ContentBrowserAction::Type::EditMesh)
                {
                    showMeshEditor_ = true;
                    meshEditor_.Open(asset->path);
                }
                else if (action.type == ContentBrowserAction::Type::EditMaterial)
                {
                    // I2: material id.key is the NAME (file stem); asset->path is the file.
                    showMaterialEditor_ = true;
                    materialEditor_.Open(asset->id.key, asset->path);
                }
                else if (action.type == ContentBrowserAction::Type::SpawnObject)
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
                    const EditorObjectId primary = selection_.Primary();
                    EditorObject* selected = document_.Find(primary);
                    if (asset->id.type != EditorAssetType::MaterialPreset ||
                        !selected ||
                        selected->type != "staticMesh")
                    {
                        levelStatus_ = "Select a static mesh before assigning a material";
                        return;
                    }

                    commandStack_.Execute(panelCtx,
                        std::make_unique<SetMaterialCommand>(primary, asset->id.key));
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
            "importAssets",
            "Import Assets",
            &showImportPanel_,
            true,
            [this](EditorContext& /*panelCtx*/)
            {
                // H3: importer window. It refreshes the AssetRegistry itself on completion; the
                // 2s poll picks up the new DDS/models entries too.
                importPanel_.Draw(assetRegistry_, &showImportPanel_);
            }));

        extensions_.RegisterPanel(std::make_unique<EditorLambdaPanel>(
            "meshEditor",
            "Mesh Editor",
            &showMeshEditor_,
            true,
            [this](EditorContext& panelCtx)
            {
                // J: edit a models/<name>.mesh.json's render defaults (materials/renderLayer/
                // spawnScale/texOffsScale). Opened by double-clicking the mesh asset in the browser.
                // Save live-applies to placed instances via panelCtx (scene/document/renderer).
                meshEditor_.Draw(panelCtx, assetRegistry_, &showMeshEditor_,
                    [this](const std::string& materialName, const std::string& materialPath)
                    {
                        materialEditor_.Open(materialName, materialPath);
                        showMaterialEditor_ = true;
                    });
            }));

        extensions_.RegisterPanel(std::make_unique<EditorLambdaPanel>(
            "materialEditor",
            "Material Editor",
            &showMaterialEditor_,
            true,
            [this](EditorContext& panelCtx)
            {
                // I2: edit a data/materials/<name>.json (textures/params/flags/shader). Save
                // re-registers the preset, evicts its cache, and respawns referencing objects.
                materialEditor_.Draw(panelCtx, assetRegistry_, &showMaterialEditor_);
            }));

        extensions_.RegisterPanel(std::make_unique<EditorLambdaPanel>(
            "levelErrors",
            "Level Errors",
            &showLevelErrors_,
            true,
            [this](EditorContext& /*panelCtx*/)
            {
                // J: list every placed object with a missing asset (geometry / material / texture).
                // The objects still exist and are grouped under "Bad Assets" in the outliner; they
                // just render nothing. Clicking a row selects the object.
                if (!ImGui::Begin("Level Errors", &showLevelErrors_))
                {
                    ImGui::End();
                    return;
                }
                if (assetErrors_.empty())
                {
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "No asset errors in this level.");
                    ImGui::End();
                    return;
                }
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%zu object(s) with missing assets:",
                    assetErrors_.size());
                ImGui::Separator();
                for (const EditorObject& obj : document_.Objects())
                {
                    const auto it = assetErrors_.find(obj.id.value);
                    if (it == assetErrors_.end()) { continue; }
                    ImGui::PushID(static_cast<int>(obj.id.value));
                    const std::string header = (obj.name.empty() ? obj.type : obj.name) +
                        "  (" + std::to_string(it->second.size()) + ")";
                    if (ImGui::Selectable(header.c_str()))
                    {
                        selection_.Clear();
                        selection_.Add(obj.id, false);
                    }
                    ImGui::Indent();
                    for (const std::string& msg : it->second)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "- %s", msg.c_str());
                    }
                    ImGui::Unindent();
                    ImGui::PopID();
                }
                ImGui::End();
            }));

        extensions_.RegisterPanel(std::make_unique<EditorLambdaPanel>(
            "sceneOutliner",
            "Scene Outliner",
            &showOutliner_,
            true,
            [this](EditorContext& panelCtx)
            {
                const OutlinerAction outlinerAction = outliner_.Draw(document_, selection_, assetErrors_, &showOutliner_);
                if (outlinerAction.type == OutlinerAction::Type::DeleteObject)
                {
                    if (!selection_.Contains(outlinerAction.target))
                    {
                        selection_.Replace(outlinerAction.target);
                    }
                    DeleteSelection(panelCtx, commandStack_, levelStatus_);
                }
                else if (outlinerAction.type == OutlinerAction::Type::DuplicateObject)
                {
                    if (!selection_.Contains(outlinerAction.target))
                    {
                        selection_.Replace(outlinerAction.target);
                    }
                    DuplicateSelection(panelCtx, commandStack_, levelStatus_);
                }
                else if (outlinerAction.type == OutlinerAction::Type::FrameSelection)
                {
                    if (!selection_.Contains(outlinerAction.target))
                    {
                        selection_.Replace(outlinerAction.target);
                    }
                    if (!FrameSelection(panelCtx.renderer,
                            panelCtx.scene,
                            document_,
                            selection_))
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
                else if (outlinerAction.type == OutlinerAction::Type::SetEnabled ||
                    outlinerAction.type == OutlinerAction::Type::SetEnvEnabled)
                {
                    if (!selection_.Contains(outlinerAction.target))
                    {
                        selection_.Replace(outlinerAction.target);
                    }
                    SetSelectionEnabled(panelCtx,
                        commandStack_,
                        outlinerAction.enabledValue,
                        levelStatus_);
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
        cameraBookmarkSlots_ = LoadCameraBookmarkSlots(normalizedPath);

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
    const auto copySelection = [&]()
    {
        CopySelectionToClipboard(document_, selection_, objectClipboard_, levelStatus_);
    };
    const auto pasteObject = [&]()
    {
        PasteObjectFromClipboard(ctx, commandStack_, objectClipboard_, levelStatus_);
    };
    const auto recallCameraBookmark = [&](int slot)
    {
        const std::string levelPath = NormalizeLevelPath(document_.LevelPath());
        if (levelPath.empty())
        {
            levelStatus_ = "Save the level before recalling camera bookmarks";
        }
        else if (RecallCameraBookmark(renderer, scene, levelPath, slot))
        {
            if (SaveLevelCameraState(levelPath, scene.CameraRef()))
            {
                markCameraStateSaved(levelPath, CaptureLevelCameraState(scene.CameraRef()));
            }
            levelStatus_ = "Recalled camera bookmark " + std::to_string(slot + 1);
        }
        else
        {
            levelStatus_ = "Camera bookmark " + std::to_string(slot + 1) + " is empty";
        }
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
    if (hotkeyActions.storeCameraBookmark >= 0)
    {
        const std::string levelPath = NormalizeLevelPath(document_.LevelPath());
        const int slot = hotkeyActions.storeCameraBookmark;
        if (levelPath.empty())
        {
            levelStatus_ = "Save the level before storing camera bookmarks";
        }
        else if (StoreCameraBookmark(levelPath, slot, scene.CameraRef()))
        {
            cameraBookmarkSlots_[static_cast<std::size_t>(slot)] = true;
            markCameraStateSaved(levelPath, CaptureLevelCameraState(scene.CameraRef()));
            levelStatus_ = "Stored camera bookmark " + std::to_string(slot + 1);
        }
        else
        {
            levelStatus_ = "Failed to store camera bookmark " + std::to_string(slot + 1);
        }
    }
    if (hotkeyActions.recallCameraBookmark >= 0)
    {
        recallCameraBookmark(hotkeyActions.recallCameraBookmark);
    }
    if (hotkeyActions.duplicateSelection)
    {
        DuplicateSelection(ctx, commandStack_, levelStatus_);
    }
    if (hotkeyActions.copySelection)
    {
        copySelection();
    }
    if (hotkeyActions.pasteObject)
    {
        pasteObject();
    }
    if (hotkeyActions.deleteSelection)
    {
        DeleteSelection(ctx, commandStack_, levelStatus_);
    }
    if (hotkeyActions.focusSelection)
    {
        if (!FrameSelection(renderer, scene, document_, selection_))
        {
            levelStatus_ = "Nothing to frame";
        }
    }
    if (hotkeyActions.frameScene)
    {
        if (!FrameVisibleScene(renderer, scene, document_))
        {
            levelStatus_ = "No visible objects to frame";
        }
    }
    if (hotkeyActions.snapSelectionToSurfaceBelow)
    {
        levelStatus_ = SnapSelectionToSurfaceBelow(ctx, commandStack_);
    }
    if (hotkeyActions.clearSelection)
    {
        selection_.Clear();
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
            if (ImGui::BeginMenu("Edit"))
            {
                if (ImGui::MenuItem("Copy", "Ctrl+C"))
                {
                    copySelection();
                }
                if (ImGui::MenuItem("Paste", "Ctrl+V"))
                {
                    pasteObject();
                }
                ImGui::Separator();
                bool canSnapBelow = false;
                for (const EditorObjectId id : selection_.Ordered())
                {
                    const EditorObject* snapObject = document_.Find(id);
                    const RenderableObjectBase* snapRuntime = scene.FindEditorObject(id.value);
                    if (snapObject && snapRuntime && snapRuntime->AsRenderableObject())
                    {
                        canSnapBelow = true;
                        break;
                    }
                }
                if (ImGui::MenuItem("Snap to Surface Below", "End", false, canSnapBelow))
                {
                    levelStatus_ = SnapSelectionToSurfaceBelow(ctx, commandStack_);
                }
                ShowDisabledItemTooltip(!canSnapBelow, "Select a mesh to snap to the nearest surface below it.");
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
                // One wind per level: it is a global singleton driving BOTH the foliage sway and the
                // ocean's wave direction/force, so a second one would just fight the first.
                const bool hasWind = HasEnvironmentObject(document_, "wind");
                if (MenuItemWithDisabledReason("Wind", !hasWind,
                        "This level already has a wind entity."))
                {
                    commandStack_.Execute(ctx, std::make_unique<CreateEnvironmentCommand>(
                        BuildWindObject()));
                }
                // One photographic camera per level: it is the camera, not a light.
                const bool hasCameraExposure = HasEnvironmentObject(document_, "cameraExposure");
                if (MenuItemWithDisabledReason("Camera Exposure", !hasCameraExposure,
                        "This level already has camera exposure settings."))
                {
                    commandStack_.Execute(ctx, std::make_unique<CreateEnvironmentCommand>(
                        BuildCameraExposureObject()));
                }
                const bool hasColorPipeline = HasEnvironmentObject(document_, "colorPipeline");
                if (MenuItemWithDisabledReason("Color Pipeline", !hasColorPipeline,
                        "This level already has a color pipeline."))
                {
                    commandStack_.Execute(ctx, std::make_unique<CreateEnvironmentCommand>(
                        BuildColorPipelineObject()));
                }
                if (ImGui::BeginMenu("VFX"))
                {
                    if (ImGui::MenuItem("Particle Emitter"))
                    {
                        commandStack_.Execute(ctx, std::make_unique<SpawnMeshCommand>(
                            BuildParticleEmitterObjectJson(scene)));
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
                        if (const OceanSimulation* ocean = Systems::GetOceanSimulation())
                        {
                            oceanEntity.properties["render"] =
                                OceanRenderConfigJson::ToJson(ocean->GetRenderConfig());
                        }
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
                            selection_.Remove(it->id);
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
            if (ImGui::BeginMenu("Window"))
            {
                const auto drawPanelMenuItem = [this](const char* panelId)
                {
                    IEditorPanel* panel = extensions_.FindPanel(panelId);
                    if (!panel || !panel->ShowInWindowList())
                    {
                        return;
                    }

                    bool visible = panel->IsVisible();
                    const std::string label(panel->Label());
                    if (ImGui::MenuItem(label.c_str(), nullptr, &visible))
                    {
                        panel->SetVisible(visible);
                    }
                };
                drawPanelMenuItem("contentBrowser");
                drawPanelMenuItem("sceneOutliner");
                drawPanelMenuItem("inspector");
                drawPanelMenuItem("commandHistory");
                drawPanelMenuItem("importAssets");
                drawPanelMenuItem("meshEditor");
                drawPanelMenuItem("materialEditor");
                drawPanelMenuItem("levelErrors");
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

        std::size_t visibleBookmarkCount = 0;
        for (bool stored : cameraBookmarkSlots_)
        {
            visibleBookmarkCount += stored ? 1 : 0;
        }
        if (visibleBookmarkCount > 0)
        {
            ImGui::Separator();
            ImGui::TextUnformatted("Camera Bookmarks");
            std::size_t displayedBookmarkCount = 0;
            for (std::size_t index = 0; index < cameraBookmarkSlots_.size(); ++index)
            {
                if (!cameraBookmarkSlots_[index])
                {
                    continue;
                }

                if (displayedBookmarkCount > 0)
                {
                    ImGui::SameLine();
                }
                char label[16];
                std::snprintf(label, sizeof(label), "%zu", index + 1);
                if (ImGui::Button(label, ImVec2(28.0f, 0.0f)))
                {
                    recallCameraBookmark(static_cast<int>(index));
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Recall camera bookmark %zu", index + 1);
                }
                ++displayedBookmarkCount;
            }
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

    // J: keep the missing-asset scan current (cheap file-exists checks, only re-run when the loaded
    // level or an edit changed the document) so the outliner group + errors window are fresh.
    RefreshAssetErrorsIfStale();

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
    drawPanel("importAssets");
    drawPanel("meshEditor");
    drawPanel("materialEditor");
    drawPanel("levelErrors");
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
    {
        CPU_SCOPE(ProfilerScopes::kEditorSyncSceneSelection);
        selectionOutlineRadius_ = std::clamp(selectionOutlineRadius_, 1, 8);
        scene.SetEditorSelectionOutlineRadius(static_cast<std::uint32_t>(selectionOutlineRadius_));
        selectedSceneObjects.clear();
        if (open_)
        {
            for (const EditorObjectId id : selection_.Ordered())
            {
                selectedSceneObjects.push_back(id.value);
            }
        }
        scene.SetSelectedEditorObjectIds(selectedSceneObjects);
    }
    {
        CPU_SCOPE(ProfilerScopes::kEditorPanelStateSync);
        PanelStateSnapshot panelStateSnapshot = CapturePanelState();
        if (!panelStateLoaded_ || !PanelStateMatches(panelStateSnapshot, lastObservedPanelStateSnapshot_))
        {
            CPU_SCOPE(ProfilerScopes::kEditorPanelStateBuildJson);
            lastObservedPanelState_ = BuildPanelStateJson(showContentBrowser_, showOutliner_,
                showInspector_, showCommandHistory_, contentBrowser_, outliner_, meshEditor_, viewportGizmo_);
            lastObservedPanelStateSnapshot_ = std::move(panelStateSnapshot);
            panelStateLoaded_ = true;
            panelStateDirty_ = true;
            nextPanelStateSaveTimeSec_ = ImGui::GetTime() + 0.25;
        }
        if (panelStateDirty_ && (!open_ || ImGui::GetTime() >= nextPanelStateSaveTimeSec_))
        {
            CPU_SCOPE(ProfilerScopes::kEditorPanelStateSave);
            if (SaveEditorPanelState(lastObservedPanelState_))
            {
                panelStateDirty_ = false;
            }
            else
            {
                nextPanelStateSaveTimeSec_ = ImGui::GetTime() + 1.0;
            }
        }
    }
    saveCurrentLevelCameraState(false);
}

#endif // WITH_EDITOR
