#include "editor/intent/EditorActionRegistry.h"
#if WITH_EDITOR

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <chrono>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "core/logging/Log.h"
#include "app/camera/Camera.h"
#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "editor/EditorFraming.h"
#include "editor/EditorObjectMatch.h"
#include "editor/intent/EditorIntentResolver.h"
#include "editor/intent/EditorSceneQuery.h"
#include "editor/intent/EnvironmentSettings.h"
#include "editor/EditorExtensionRegistry.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/commands/CompositeCommand.h"
#include "editor/commands/CreateDocumentObjectCommand.h"
#include "editor/commands/EditObjectPropertiesCommand.h"
#include "editor/commands/DeleteObjectCommand.h"
#include "editor/commands/DuplicateObjectCommand.h"
#include "editor/commands/EditEnvironmentCommand.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/RenameObjectCommand.h"
#include "editor/scene/EditorZone.h"
#include "editor/commands/SetEnabledCommand.h"
#include "editor/commands/SetMaterialCommand.h"
#include "editor/commands/SetMaterialSlotCommand.h"
#include "editor/commands/SetMeshAssetCommand.h"
#include "editor/commands/SpawnMeshCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "ocean/OceanRenderable.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/renderables/RenderableObject.h"

// Works out what a setting BECOMES: an absolute `value`, or `scale` times what it is now.
// Exactly one of the two, because "make the fog thicker" and "set the fog to 0.02" are
// different requests and guessing between them silently is how a look gets wrecked.
bool ComputeEnvironmentValue(const envsettings::Setting& setting,
    const nlohmann::json& params,
    nlohmann::json& outValue,
    std::string& outError)
{
    const auto valueIt = params.find("value");
    const auto scaleIt = params.find("scale");
    const bool hasValue = valueIt != params.end() && !valueIt->is_null();
    const bool hasScale = scaleIt != params.end() && scaleIt->is_number();

    if (hasValue == hasScale)
    {
        outError = hasValue
            ? "give either 'value' or 'scale' for " + setting.path + ", not both"
            : "setEnvironment needs a 'value' (the new setting) or a 'scale' (times its "
              "current value) for " + setting.path;
        return false;
    }

    if (hasScale)
    {
        if (setting.kind != envsettings::ValueKind::Number)
        {
            outError = "'scale' only means something for a number; " + setting.path +
                " is " + envsettings::KindName(setting.kind);
            return false;
        }
        outValue = setting.current.get<double>() * scaleIt->get<double>();
        return true;
    }

    // The declared kind is the contract. A boolean setting handed 1 would otherwise become
    // a number in the level file and quietly stop being read.
    switch (setting.kind)
    {
    case envsettings::ValueKind::Number:
        if (!valueIt->is_number())
        {
            outError = setting.path + " is a number";
            return false;
        }
        break;
    case envsettings::ValueKind::Bool:
        if (!valueIt->is_boolean())
        {
            outError = setting.path + " is true/false";
            return false;
        }
        break;
    case envsettings::ValueKind::String:
        if (!valueIt->is_string())
        {
            outError = setting.path + " is text";
            return false;
        }
        break;
    case envsettings::ValueKind::Vec3:
        if (!valueIt->is_array() || valueIt->size() != 3)
        {
            outError = setting.path + " is three numbers [x, y, z]";
            return false;
        }
        break;
    }
    outValue = *valueIt;
    return true;
}

namespace
{
    // ----------------------------------------------------------- shared helpers

    const EditorObject* FindEnvironmentObject(const EditorSceneDocument& document, EditorObjectId id)
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

    // Folds N commands into one history entry. One typed phrase must cost exactly one
    // Ctrl+Z, whether it moved a single palm or two hundred of them (E6).
    std::unique_ptr<EditorCommand> FoldIntoOneEntry(
        std::vector<std::unique_ptr<EditorCommand>> commands,
        const std::string& label)
    {
        if (commands.empty())
        {
            return nullptr;
        }
        if (commands.size() == 1)
        {
            return std::move(commands.front());
        }
        auto composite = std::make_unique<CompositeCommand>(label);
        for (std::unique_ptr<EditorCommand>& command : commands)
        {
            composite->Add(std::move(command));
        }
        return composite;
    }

    std::string CountedObjects(std::size_t count)
    {
        return std::to_string(count) + (count == 1 ? " object" : " objects");
    }

    // --------------------------------------------------------- parameter access
    //
    // Everything below assumes ValidateParams has already run, so the shapes are known
    // good and these only have to supply defaults.

    float NumberOr(const nlohmann::json& params, const char* key, float fallback)
    {
        const auto it = params.find(key);
        return (it != params.end() && it->is_number()) ? it->get<float>() : fallback;
    }

    bool BoolOr(const nlohmann::json& params, const char* key, bool fallback)
    {
        const auto it = params.find(key);
        return (it != params.end() && it->is_boolean()) ? it->get<bool>() : fallback;
    }

    std::string StringOr(const nlohmann::json& params, const char* key, const char* fallback)
    {
        const auto it = params.find(key);
        return (it != params.end() && it->is_string()) ? it->get<std::string>() : std::string(fallback);
    }

    void RangeOr(const nlohmann::json& params, const char* key, float& lo, float& hi)
    {
        const auto it = params.find(key);
        if (it != params.end() && it->is_array() && it->size() == 2 &&
            (*it)[0].is_number() && (*it)[1].is_number())
        {
            lo = (*it)[0].get<float>();
            hi = (*it)[1].get<float>();
            if (hi < lo)
            {
                std::swap(lo, hi);
            }
        }
    }

    Math::float3 Vec3Or(const nlohmann::json& params, const char* key, const Math::float3& fallback)
    {
        const auto it = params.find(key);
        if (it != params.end() && it->is_array() && it->size() == 3 &&
            (*it)[0].is_number() && (*it)[1].is_number() && (*it)[2].is_number())
        {
            return Math::float3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
        }
        return fallback;
    }

    // A seed that is DETERMINISTIC but not constant. An explicit `seed` wins; otherwise
    // it comes from how many objects the document holds, so re-running the same phrase
    // after a spawn lays things out differently while a gate that pins the seed gets the
    // same answer every time.
    std::uint32_t SeedFor(const nlohmann::json& params, const EditorSceneDocument& document)
    {
        const auto it = params.find("seed");
        if (it != params.end() && it->is_number())
        {
            return static_cast<std::uint32_t>(std::max(0.0f, it->get<float>()));
        }
        return static_cast<std::uint32_t>(document.Objects().size()) * 2654435761u + 12345u;
    }

    // ---------------------------------------------------------------- selection

    std::unique_ptr<EditorCommand> BuildDelete(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent&,
        std::string& outStatus)
    {
        const EditorSceneDocument& document = actionCtx.editor.document;
        if (targets.empty())
        {
            outStatus = "Nothing to delete";
            return nullptr;
        }
        for (const EditorObjectId id : targets)
        {
            if (!IsBulkObjectSupported(document, id))
            {
                outStatus = "Selection contains an object that cannot be deleted";
                return nullptr;
            }
        }

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            commands.push_back(std::make_unique<DeleteObjectCommand>(id));
        }
        outStatus = "Deleted " + CountedObjects(targets.size());
        return FoldIntoOneEntry(std::move(commands),
            "Delete " + std::to_string(targets.size()) + " Objects");
    }

    std::unique_ptr<EditorCommand> BuildDuplicate(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent&,
        std::string& outStatus)
    {
        const EditorSceneDocument& document = actionCtx.editor.document;
        if (targets.empty())
        {
            outStatus = "Nothing to duplicate";
            return nullptr;
        }
        for (const EditorObjectId id : targets)
        {
            if (!IsBulkObjectSupported(document, id))
            {
                outStatus = "Selection contains an object that cannot be duplicated";
                return nullptr;
            }
        }

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        // The first copy REPLACES the selection and the rest join it, so the result of
        // the whole action is "the new copies are selected" rather than "the last one is".
        std::size_t index = 0;
        for (const EditorObjectId id : targets)
        {
            commands.push_back(std::make_unique<DuplicateObjectCommand>(id, index != 0));
            ++index;
        }
        outStatus = "Duplicated " + CountedObjects(targets.size());
        return FoldIntoOneEntry(std::move(commands),
            "Duplicate " + std::to_string(targets.size()) + " Objects");
    }

    std::unique_ptr<EditorCommand> BuildSetEnabledOne(const EditorSceneDocument& document,
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

    std::unique_ptr<EditorCommand> BuildSetEnabled(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        const bool enabled = BoolOr(intent.params, "enabled", true);
        if (targets.empty())
        {
            outStatus = "Nothing to show or hide";
            return nullptr;
        }

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            std::unique_ptr<EditorCommand> command =
                BuildSetEnabledOne(actionCtx.editor.document, id, enabled);
            if (!command)
            {
                outStatus = "Selection contains an object that cannot be enabled or disabled";
                return nullptr;
            }
            commands.push_back(std::move(command));
        }

        outStatus = (enabled ? "Enabled " : "Disabled ") + CountedObjects(targets.size());
        return FoldIntoOneEntry(std::move(commands),
            std::string(enabled ? "Enable " : "Disable ") +
            std::to_string(commands.size()) + " Objects");
    }

    // ------------------------------------------------------------------ rename

    // Defined further down with the scatter it was written for. Declared here because
    // createZone wants the same answer to "where is the designer pointing": a zone made by
    // a phrase and a scatter made by a phrase must land in the same place.
    Math::float3 ResolveScatterAnchor(EditorContext& ctx,
        const EditorSpatialFilter& where,
        float radius);

    // ---------------------------------------------------------------- traceZone

    // DRAW THE WATERLINE. Asked for in the refusal log, in the designer's own words --
    // "ÑÐ¾Ð·Ð´Ð°Ð¹ ÑÐ¿Ð»Ð°Ð¹Ð½ Ð·Ð¾Ð½Ñƒ Ð¿Ð¾ ÐºÐ¾Ð½Ñ‚ÑƒÑ€Ñƒ Ð¾ÑÑ‚Ñ€Ð¾Ð²Ð° Ð½Ð°Ð´ Ð²Ð¾Ð´Ð¾Ð¹" -- and the model's note named the
    // real difficulty exactly: the hard part is not the zone, it is finding the points.
    //
    // NOT BY READING THE MESH. The geometry is a 4 MB binary of local-space floats; the
    // island's world transform lives in the document, not in it; and clipping triangles
    // against a plane answers with an edge soup that includes undercuts and the underside.
    // A ray straight down through the SCENE answers the question that was actually asked --
    // "what is the top surface here, and is it above the water" -- with the transform, the
    // terrain's chunking and anything standing on top already accounted for. It is also the
    // same probe the scatter uses to decide where a palm may stand, so the contour and the
    // planting inside it agree by construction.
    //
    // The water level is the ocean object's Y, not zero, and it is a MEAN: the surface moves
    // with the waves. `margin` is what lets someone ask for the line a little above it,
    // which is what "dry land" means on a beach.
    constexpr int kTraceMinResolution = 24;
    constexpr int kTraceMaxResolution = 160;

    std::unique_ptr<EditorCommand> BuildTraceZone(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        if (targets.empty())
        {
            outStatus = "Nothing to trace around";
            return nullptr;
        }

        // The plan-view box of what we are tracing, from the runtime bounds.
        Math::float3 lo(0.0f, 0.0f, 0.0f);
        Math::float3 hi(0.0f, 0.0f, 0.0f);
        bool haveBounds = false;
        for (const EditorObjectId id : targets)
        {
            Math::float3 objectLo;
            Math::float3 objectHi;
            if (!editorframing::TryGetWorldBounds(ctx.scene, ctx.document, id, objectLo, objectHi))
            {
                continue;
            }
            if (!haveBounds)
            {
                lo = objectLo;
                hi = objectHi;
                haveBounds = true;
                continue;
            }
            lo = Math::float3((std::min)(lo.x, objectLo.x), (std::min)(lo.y, objectLo.y),
                (std::min)(lo.z, objectLo.z));
            hi = Math::float3((std::max)(hi.x, objectHi.x), (std::max)(hi.y, objectHi.y),
                (std::max)(hi.z, objectHi.z));
        }
        if (!haveBounds)
        {
            outStatus = "Cannot measure what to trace around";
            return nullptr;
        }

        float water = 0.0f;
        bool hasWater = false;
        if (const OceanRenderable* ocean = ctx.scene.FindOceanRenderable())
        {
            water = ocean->GetWaterLevel();
            hasWater = true;
        }
        const float margin = NumberOr(intent.params, "margin", 0.0f);
        const float cutoff = NumberOr(intent.params, "height",
            hasWater ? water + margin : (lo.y + hi.y) * 0.5f);

        const int resolution = std::clamp(
            static_cast<int>(NumberOr(intent.params, "resolution", 96.0f)),
            kTraceMinResolution, kTraceMaxResolution);

        // One cell of padding all round, so a shape touching the bounds still has an
        // "outside" for the boundary walk to find.
        const float cellX = (hi.x - lo.x) / static_cast<float>(resolution - 2);
        const float cellZ = (hi.z - lo.z) / static_cast<float>(resolution - 2);
        if (cellX <= 0.0f || cellZ <= 0.0f)
        {
            outStatus = "That has no footprint to trace";
            return nullptr;
        }
        const float originX = lo.x - cellX;
        const float originZ = lo.z - cellZ;
        const auto worldX = [&](int gx) { return originX + static_cast<float>(gx) * cellX; };
        const auto worldZ = [&](int gz) { return originZ + static_cast<float>(gz) * cellZ; };

        // The probe grid. THIS IS THE EXPENSIVE PART: every cell is a scene raycast, and at
        // the default resolution that is about nine thousand of them. It runs once, on Run,
        // and not in the preview -- a preview that costs what the command costs would make
        // looking at what a command would do as slow as doing it.
        const std::vector<Scene::SceneObjectId> noIgnores;
        // ONLY WHAT IS BEING TRACED. "Обведи остров" is a question about the island, and
        // answering it with every palm standing on the island is wrong twice over: the
        // contour follows the canopies instead of the ground, and each of the 9216 rays
        // walks the triangles of everything whose bounding box it crosses. Measured before
        // this existed: 7.8 ms a ray, 71.6 seconds for one contour, with the frame thread
        // held for all of it -- Windows then paints the window white and calls the editor
        // not responding, which is exactly what was reported.
        std::vector<Scene::SceneObjectId> traceOnly;
        traceOnly.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            traceOnly.push_back(id.value);
        }
        const float probeStart = hi.y + editorquery::kGroundProbeUp;
        std::vector<unsigned char> above(static_cast<std::size_t>(resolution) * resolution, 0u);
        std::vector<float> height(static_cast<std::size_t>(resolution) * resolution, 0.0f);
        const auto at = [resolution](int gx, int gz)
        {
            return static_cast<std::size_t>(gz) * static_cast<std::size_t>(resolution) +
                static_cast<std::size_t>(gx);
        };
        std::size_t aboveCount = 0;
        const auto sampleBegin = std::chrono::steady_clock::now();

        // RASTERISED, NOT PROBED. This grid used to be filled by casting a ray straight
        // down through each of its cells. That is the obvious way to ask "how high is the
        // ground here", and it is quadratic in the wrong thing: `Mesh::RaycastLocal` is a
        // LINEAR walk over every triangle -- there is no BVH behind it -- so 9216 cells
        // meant 9216 walks of the island's whole triangle list. Measured: 7.8 ms a cell,
        // 71.6 seconds for one contour, all of it on the frame thread. Windows painted the
        // window white and titled it "Not Responding", which is what was reported.
        //
        // Restricting the rays to the traced object alone changed nothing (71.2 s -- the
        // palms were never the cost), which is what said the ALGORITHM was wrong rather
        // than its inputs. Sweeping the triangles instead visits each one once and drops
        // it into the cells it covers: one pass over the mesh, not one pass per cell.
        //
        // The value kept per cell is the HIGHEST surface over it, which is what a ray
        // pointing down would have found.
        std::vector<unsigned char> touched(above.size(), 0u);
        for (const EditorObjectId targetId : targets)
        {
            const RenderableObjectBase* object = ctx.scene.FindEditorObject(targetId.value);
            RtInstanceDesc instance{};
            if (!object || !object->GetRtInstance(instance) || !instance.mesh ||
                !instance.mesh->HasRaycastTriangles())
            {
                continue;
            }
            const std::vector<Math::float3>& positions = instance.mesh->RaycastPositions();
            const std::vector<uint32_t>& indices = instance.mesh->RaycastIndices();
            for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                if (indices[i] >= positions.size() || indices[i + 1] >= positions.size() ||
                    indices[i + 2] >= positions.size())
                {
                    continue;
                }
                const Math::float3 a = instance.world.TransformPoint(positions[indices[i]]);
                const Math::float3 b = instance.world.TransformPoint(positions[indices[i + 1]]);
                const Math::float3 c = instance.world.TransformPoint(positions[indices[i + 2]]);

                // The cells this triangle can possibly cover, from its own XZ box.
                const float minX = std::min(a.x, std::min(b.x, c.x));
                const float maxX = std::max(a.x, std::max(b.x, c.x));
                const float minZ = std::min(a.z, std::min(b.z, c.z));
                const float maxZ = std::max(a.z, std::max(b.z, c.z));
                const int gx0 = std::max(0, static_cast<int>(std::floor((minX - originX) / cellX)));
                const int gx1 = std::min(resolution - 1,
                    static_cast<int>(std::ceil((maxX - originX) / cellX)));
                const int gz0 = std::max(0, static_cast<int>(std::floor((minZ - originZ) / cellZ)));
                const int gz1 = std::min(resolution - 1,
                    static_cast<int>(std::ceil((maxZ - originZ) / cellZ)));

                // Edge functions in XZ, computed once per triangle.
                const float area = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
                if (std::fabs(area) < 1.0e-12f)
                {
                    continue;   // degenerate seen from above; it covers no cell centre
                }
                const float invArea = 1.0f / area;
                for (int gz = gz0; gz <= gz1; ++gz)
                {
                    for (int gx = gx0; gx <= gx1; ++gx)
                    {
                        const float px = worldX(gx);
                        const float pz = worldZ(gz);
                        const float w0 = ((b.x - px) * (c.z - pz) - (c.x - px) * (b.z - pz)) * invArea;
                        const float w1 = ((c.x - px) * (a.z - pz) - (a.x - px) * (c.z - pz)) * invArea;
                        const float w2 = 1.0f - w0 - w1;
                        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                        {
                            continue;
                        }
                        const float y = w0 * a.y + w1 * b.y + w2 * c.y;
                        const std::size_t cell = at(gx, gz);
                        if (!touched[cell] || y > height[cell])
                        {
                            height[cell] = y;
                            touched[cell] = 1u;
                        }
                    }
                }
            }
        }
        for (std::size_t cell = 0; cell < height.size(); ++cell)
        {
            if (touched[cell] && height[cell] >= cutoff)
            {
                above[cell] = 1u;
                ++aboveCount;
            }
        }
        // WHAT THE GRID COST, because it is the part that can make the editor stop
        // responding and nothing said so. It is resolution^2 raycasts against the whole
        // scene, on the frame thread: at 128 that is 16384 rays, each of which hits the
        // island's bounding box and then walks its triangles. A request with resolution
        // 128 ran for over two minutes and was killed before it produced a point.
        LOG_INFO(logging::LogCategory::Editor,
            "traceZone: sampled {}x{} = {} ground probes in {:.1f}s, {} above the cutoff",
            resolution, resolution, resolution * resolution,
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - sampleBegin).count(),
            aboveCount);

        if (aboveCount < 8)
        {
            outStatus = hasWater
                ? "Nothing there rises above the waterline"
                : "Nothing there rises above that height";
            return nullptr;
        }

        // THE LARGEST CONNECTED PIECE, then walk its edge. An atoll is often several islets
        // and a zone is one region; taking the biggest is a rule somebody can predict, and
        // the status line says when others were left out.
        std::vector<int> label(above.size(), -1);
        std::vector<std::size_t> componentSize;
        std::vector<int> stack;
        for (int gz = 0; gz < resolution; ++gz)
        {
            for (int gx = 0; gx < resolution; ++gx)
            {
                if (!above[at(gx, gz)] || label[at(gx, gz)] >= 0)
                {
                    continue;
                }
                const int id = static_cast<int>(componentSize.size());
                componentSize.push_back(0);
                stack.clear();
                stack.push_back(gz * resolution + gx);
                label[at(gx, gz)] = id;
                while (!stack.empty())
                {
                    const int cell = stack.back();
                    stack.pop_back();
                    ++componentSize[static_cast<std::size_t>(id)];
                    const int cx = cell % resolution;
                    const int cz = cell / resolution;
                    const int neighbours[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                    for (const auto& step : neighbours)
                    {
                        const int nx = cx + step[0];
                        const int nz = cz + step[1];
                        if (nx < 0 || nz < 0 || nx >= resolution || nz >= resolution)
                        {
                            continue;
                        }
                        if (!above[at(nx, nz)] || label[at(nx, nz)] >= 0)
                        {
                            continue;
                        }
                        label[at(nx, nz)] = id;
                        stack.push_back(nz * resolution + nx);
                    }
                }
            }
        }
        int biggest = 0;
        for (std::size_t i = 1; i < componentSize.size(); ++i)
        {
            if (componentSize[i] > componentSize[static_cast<std::size_t>(biggest)])
            {
                biggest = static_cast<int>(i);
            }
        }

        // Moore boundary tracing: start at the first cell of the component and keep the
        // outside on one hand. It hands back an ORDERED loop, which marching squares does
        // not -- that produces a soup of segments somebody then has to stitch, and stitching
        // is where the seams and the double-counted corners come from.
        int startX = -1;
        int startZ = -1;
        for (int gz = 0; gz < resolution && startX < 0; ++gz)
        {
            for (int gx = 0; gx < resolution; ++gx)
            {
                if (label[at(gx, gz)] == biggest)
                {
                    startX = gx;
                    startZ = gz;
                    break;
                }
            }
        }
        const auto inComponent = [&](int gx, int gz)
        {
            return gx >= 0 && gz >= 0 && gx < resolution && gz < resolution &&
                label[at(gx, gz)] == biggest;
        };

        static const int kDx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
        static const int kDz[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
        std::vector<std::pair<int, int>> loop;
        {
            int cx = startX;
            int cz = startZ;
            int direction = 6;   // came from "above"; start looking there
            const std::size_t guard = above.size() * 4;
            for (std::size_t steps = 0; steps < guard; ++steps)
            {
                loop.emplace_back(cx, cz);
                int found = -1;
                for (int i = 0; i < 8; ++i)
                {
                    const int d = (direction + 6 + i) % 8;   // turn left, then sweep right
                    if (inComponent(cx + kDx[d], cz + kDz[d]))
                    {
                        found = d;
                        break;
                    }
                }
                if (found < 0)
                {
                    break;   // a single isolated cell
                }
                cx += kDx[found];
                cz += kDz[found];
                direction = found;
                if (cx == startX && cz == startZ && loop.size() > 2)
                {
                    break;
                }
            }
        }
        if (loop.size() < 4)
        {
            outStatus = "The above-water part is too small to draw a contour around";
            return nullptr;
        }

        // Each boundary cell is pulled OUT to where the ground actually crosses the cutoff,
        // by interpolating against its first outside neighbour. Without this the contour is
        // a staircase on the grid, and the grid is coarse on purpose.
        std::vector<Math::float3> contour;
        contour.reserve(loop.size());
        for (const auto& cell : loop)
        {
            const float insideY = height[at(cell.first, cell.second)];
            float px = worldX(cell.first);
            float pz = worldZ(cell.second);
            for (int i = 0; i < 8; ++i)
            {
                const int nx = cell.first + kDx[i];
                const int nz = cell.second + kDz[i];
                if (nx < 0 || nz < 0 || nx >= resolution || nz >= resolution)
                {
                    continue;
                }
                if (above[at(nx, nz)])
                {
                    continue;
                }
                const float outsideY = height[at(nx, nz)];
                const float span = insideY - outsideY;
                // Where the straight line between the two heights crosses the cutoff.
                const float t = std::fabs(span) > 1e-4f
                    ? std::clamp((insideY - cutoff) / span, 0.0f, 1.0f) : 0.5f;
                px += (worldX(nx) - px) * t;
                pz += (worldZ(nz) - pz) * t;
                break;
            }
            contour.emplace_back(px, cutoff, pz);
        }

        // Douglas-Peucker down to something a person can drag. A zone whose control points
        // outnumber the pixels it is drawn in is not editable, and the curve between them is
        // a Catmull-Rom that rounds the corners back anyway.
        const int wanted = std::clamp(
            static_cast<int>(NumberOr(intent.params, "points", 32.0f)), 4, 120);
        std::vector<Math::float3> simplified;
        {
            const std::size_t stride = (std::max)(std::size_t{ 1 },
                contour.size() / static_cast<std::size_t>(wanted));
            for (std::size_t i = 0; i < contour.size(); i += stride)
            {
                simplified.push_back(contour[i]);
            }
        }
        if (simplified.size() < 3)
        {
            outStatus = "Could not find enough of a contour to make a zone";
            return nullptr;
        }

        // The zone's own transform is the centre; the points are stored relative to it.
        Math::float3 centre(0.0f, cutoff, 0.0f);
        for (const Math::float3& point : simplified)
        {
            centre.x += point.x / static_cast<float>(simplified.size());
            centre.z += point.z / static_cast<float>(simplified.size());
        }

        std::string name = StringOr(intent.params, "name", "");
        if (name.empty())
        {
            int ordinal = 1;
            for (const EditorObject& object : ctx.document.Objects())
            {
                if (object.type == editorzone::kTypeName)
                {
                    ++ordinal;
                }
            }
            name = "Shoreline " + std::to_string(ordinal);
        }

        EditorObject zone = editorzone::BuildObject(editorzone::Shape::Spline, centre, 1.0f, name);
        nlohmann::json points = nlohmann::json::array();
        for (const Math::float3& point : simplified)
        {
            points.push_back(nlohmann::json::array(
                { point.x - centre.x, 0.0f, point.z - centre.z }));
        }
        zone.properties["points"] = points;
        // CLOSED and filling INSIDE, because a shoreline is the edge of a region and the
        // thing anybody wants next is to plant on the land it encloses.
        zone.properties["closed"] = true;
        zone.properties["fill"] = "inside";

        // The contour's own extent, logged so the result is CHECKABLE. "37 points" says the
        // command ran; it does not say the points went round the island rather than round a
        // box or a puddle. Compared against the target's bounds, these numbers do.
        Math::float3 traceLo(simplified.front().x, cutoff, simplified.front().z);
        Math::float3 traceHi = traceLo;
        for (const Math::float3& point : simplified)
        {
            traceLo.x = (std::min)(traceLo.x, point.x);
            traceLo.z = (std::min)(traceLo.z, point.z);
            traceHi.x = (std::max)(traceHi.x, point.x);
            traceHi.z = (std::max)(traceHi.z, point.z);
        }
        LOG_INFO(logging::LogCategory::Editor,
            "traceZone: cutoff y={:.2f}, {} of {} cells above it, contour x[{:.1f}..{:.1f}] "
            "z[{:.1f}..{:.1f}] inside target x[{:.1f}..{:.1f}] z[{:.1f}..{:.1f}]",
            cutoff, aboveCount, above.size(), traceLo.x, traceHi.x, traceLo.z, traceHi.z,
            lo.x, hi.x, lo.z, hi.z);

        outStatus = "Traced '" + name + "' with " + std::to_string(simplified.size()) +
            " points from " + std::to_string(resolution) + "x" + std::to_string(resolution) +
            " probes";
        if (componentSize.size() > 1)
        {
            outStatus += " (largest of " + std::to_string(componentSize.size()) +
                " separate pieces)";
        }
        return std::make_unique<CreateDocumentObjectCommand>(std::move(zone));
    }

    // ---------------------------------------------------------------- setColor

    // THE ONE GENUINE GAP IN THE REFUSAL LOG. "Ð¿Ð¾ÐºÑ€Ð°ÑÑŒ Ð¿Ð°Ð»ÑŒÐ¼Ñ‹ Ð² ÑÑ€ÐºÐ¾-ÐºÑ€Ð°ÑÐ½Ñ‹Ð¹" was answered
    // needs_api twice, and the model's reasoning was right both times: `replace` swaps the
    // whole mesh and `setMaterial` swaps the whole material, and neither is "the same tree,
    // red". The engine could already do it -- MaterialParams::baseColor multiplies the
    // albedo and reaches the shader every frame -- but nothing could mark it as authored
    // per object, so no level could carry the value and no action could set it.
    //
    // Written through EditObjectPropertiesCommand, like `group`: the property is the level's
    // own JSON, the runtime picks it up through the inspector's live path, and Ctrl+Z puts
    // the old colour back.
    std::unique_ptr<EditorCommand> BuildSetColor(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        if (targets.empty())
        {
            outStatus = "Nothing to colour";
            return nullptr;
        }

        const auto colourIt = intent.params.find("color");
        Math::float3 rgb(1.0f, 1.0f, 1.0f);
        if (colourIt == intent.params.end() || !colourIt->is_array() || colourIt->size() < 3 ||
            !(*colourIt)[0].is_number())
        {
            outStatus = "setColor needs a color as [r, g, b], each 0..1";
            return nullptr;
        }
        rgb = Math::float3(std::clamp((*colourIt)[0].get<float>(), 0.0f, 1.0f),
            std::clamp((*colourIt)[1].get<float>(), 0.0f, 1.0f),
            std::clamp((*colourIt)[2].get<float>(), 0.0f, 1.0f));

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            const EditorObject* object = ctx.document.Find(id);
            if (!object)
            {
                continue;
            }
            nlohmann::json before = object->properties.is_object()
                ? object->properties : nlohmann::json::object();
            nlohmann::json after = before;
            after["baseColor"] = nlohmann::json::array({ rgb.x, rgb.y, rgb.z });
            if (after == before)
            {
                continue;
            }
            commands.push_back(std::make_unique<EditObjectPropertiesCommand>(
                id, std::move(before), std::move(after), "Set Colour"));
        }
        if (commands.empty())
        {
            outStatus = "They are that colour already";
            return nullptr;
        }
        outStatus = "Coloured " + std::to_string(commands.size()) + " objects";
        return FoldIntoOneEntry(std::move(commands), "Set Colour");
    }

    // ---------------------------------------------------------------- group

    // PUT THESE IN A NAMED GROUP, which is a flat label rather than a hierarchy.
    //
    // The flat version is not a lesser one here. A group name lives in the object's own
    // `properties`, so it serialises with the level for free, it is matched by the
    // outliner's search predicate the moment the key is in kSearchPropertyKeys, and every
    // action that takes a filter can therefore act on a group without knowing groups exist.
    // "ÑƒÐ´Ð°Ð»Ð¸ ÑÐµÐ²ÐµÑ€Ð½ÑƒÑŽ Ñ€Ð¾Ñ‰Ñƒ" is a delete whose filter is a group name, and nothing had to be
    // taught about it. A parent/child tree would have brought inherited transforms, a
    // serialisation format change and a question about what deleting a parent means -- none
    // of which anyone asked for.
    std::unique_ptr<EditorCommand> BuildGroup(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        if (targets.empty())
        {
            outStatus = "Nothing to group";
            return nullptr;
        }
        // An EMPTY name is how you leave a group, and saying so explicitly beats a second
        // action that does only that.
        const std::string name = StringOr(intent.params, "name", "");

        // ONE GROUP PER KIND, when asked for that. "Ð“Ñ€ÑƒÐ¿Ð¿Ñ‹ Ð´Ð»Ñ ÐºÐ°Ð¶Ð´Ð¾Ð³Ð¾ Ñ‚Ð¸Ð¿Ð° Ð¿Ð°Ð»ÑŒÐ¼" is a
        // single intention and used to need three commands, because the action took one
        // name; the model correctly did the first and stopped, and from outside that reads
        // as it being unable to finish. The name is derived from the asset, so the groups
        // come out called what the things in them are.
        const bool perAsset = BoolOr(intent.params, "perAsset", false);

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        std::map<std::string, std::size_t> madeGroups;
        for (const EditorObjectId id : targets)
        {
            const EditorObject* object = ctx.document.Find(id);
            if (!object)
            {
                continue;
            }
            nlohmann::json before = object->properties.is_object()
                ? object->properties : nlohmann::json::object();
            nlohmann::json after = before;
            std::string wanted = name;
            if (perAsset)
            {
                // ONE spelling of "make the asset path readable", shared with rename. There
                // were two, and they had already drifted: the folder said `coconut_palm`
                // while the objects inside it would have been named `Coconut Palm`.
                const std::string label = editormatch::PrettyAssetLabel(*object);
                wanted = label.empty() ? object->type : label;
            }
            if (wanted.empty())
            {
                after.erase("group");
            }
            else
            {
                after["group"] = wanted;
            }
            if (after == before)
            {
                continue;
            }
            if (!wanted.empty())
            {
                ++madeGroups[wanted];
            }
            commands.push_back(std::make_unique<EditObjectPropertiesCommand>(
                id, std::move(before), std::move(after),
                wanted.empty() ? "Ungroup" : "Group"));
        }
        if (commands.empty())
        {
            outStatus = (name.empty() && !perAsset) ? "None of them was in a group"
                                                    : "They are already grouped that way";
            return nullptr;
        }
        if (perAsset)
        {
            outStatus = "Put " + std::to_string(commands.size()) + " into " +
                std::to_string(madeGroups.size()) + " groups by kind: ";
            std::size_t shown = 0;
            for (const auto& entry : madeGroups)
            {
                outStatus += (shown++ ? ", " : "") + entry.first + " x" +
                    std::to_string(entry.second);
            }
        }
        else
        {
            outStatus = name.empty()
                ? "Removed " + std::to_string(commands.size()) + " from their group"
                : "Put " + std::to_string(commands.size()) + " in '" + name + "'";
        }
        return FoldIntoOneEntry(std::move(commands),
            (name.empty() && !perAsset) ? "Ungroup" : "Group");
    }

    // ---------------------------------------------------------------- thin

    // THE VERB SPAWN HAS NO INVERSE, and a scatter that came out too dense had no answer
    // but undo-and-retry with a different seed -- which throws away every hand placement
    // made since. Thinning is the same min-separation rule spawn already applies to
    // candidates, run over what is actually standing there.
    //
    // KEEPS THE FIRST OF EACH CROWDED PAIR rather than choosing by some quality, because
    // there is no quality to choose by and a rule anybody can predict beats a clever one
    // nobody can. Document order is placement order, so the oldest survives.
    // WHICH ONES GO. Shared by the builder and by the preview, because the preview's whole
    // job is to say what the builder will do, and a second implementation of "which ones
    // are too close together" would be a second answer to that.
    std::vector<EditorObjectId> CollectThinVictims(EditorContext& ctx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outProblem)
    {
        std::vector<EditorObjectId> doomed;
        if (targets.empty())
        {
            outProblem = "Nothing to thin";
            return doomed;
        }

        const float spacing = NumberOr(intent.params, "minSeparation", 0.0f);
        const float keepFraction = NumberOr(intent.params, "keepPercent", 0.0f) * 0.01f;
        if (spacing <= 0.0f && keepFraction <= 0.0f)
        {
            outProblem = "thin needs either minSeparation (metres) or keepPercent";
            return doomed;
        }

        if (spacing > 0.0f)
        {
            std::vector<Math::float3> kept;
            kept.reserve(targets.size());
            for (const EditorObjectId id : targets)
            {
                const EditorObject* object = ctx.document.Find(id);
                if (!object)
                {
                    continue;
                }
                const Math::float3& p = object->transform.position;
                bool crowded = false;
                for (const Math::float3& other : kept)
                {
                    const float dx = p.x - other.x;
                    const float dz = p.z - other.z;
                    // In PLAN, like the scatter's own test: two palms on a slope are not
                    // further apart because one is higher up.
                    if (dx * dx + dz * dz < spacing * spacing)
                    {
                        crowded = true;
                        break;
                    }
                }
                if (crowded)
                {
                    doomed.push_back(id);
                }
                else
                {
                    kept.push_back(p);
                }
            }
        }
        else
        {
            // Evenly through the list rather than at random: asked to keep a third, the
            // designer means a thinner version of the same arrangement, not a new one.
            const float keep = std::clamp(keepFraction, 0.01f, 1.0f);
            const double step = 1.0 / static_cast<double>(keep);
            double next = 0.0;
            for (std::size_t i = 0; i < targets.size(); ++i)
            {
                if (static_cast<double>(i) + 1e-6 >= next)
                {
                    next += step;
                    continue;
                }
                doomed.push_back(targets[i]);
            }
        }

        if (doomed.empty())
        {
            outProblem = "Nothing was closer together than that";
        }
        return doomed;
    }

    // Same label the resolver's preview uses -- the asset name where there is one, because
    // a hundred palms are "Palm_0xx" individually and `coconut_palm` together.
    void AppendVictimGroup(std::vector<EditorIntentPreview::Group>& groups,
        const EditorObject& object)
    {
        const std::string label = editormatch::AssetLabel(object);
        for (EditorIntentPreview::Group& group : groups)
        {
            if (group.label == label)
            {
                ++group.count;
                return;
            }
        }
        groups.push_back({ label, 1 });
    }

    // The preview's own number: how many actually go, not how many were considered.
    void RefineThinPreview(const EditorActionContext& actionCtx,
        const EditorIntent& intent,
        EditorIntentPreview& preview)
    {
        std::string problem;
        const std::vector<EditorObjectId> doomed =
            CollectThinVictims(actionCtx.editor, preview.targets, intent, problem);
        if (doomed.empty())
        {
            preview.executable = false;
            preview.problem = problem.empty() ? "Nothing to thin" : problem;
            return;
        }
        // The groups are rebuilt from the victims, so the line under the summary names what
        // is about to disappear rather than what was searched.
        preview.groups.clear();
        for (const EditorObjectId id : doomed)
        {
            if (const EditorObject* object = actionCtx.editor.document.Find(id))
            {
                AppendVictimGroup(preview.groups, *object);
            }
        }
        preview.summary = "thin: remove " + std::to_string(doomed.size()) + " of " +
            std::to_string(preview.targets.size());
    }

    std::unique_ptr<EditorCommand> BuildThin(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        const std::vector<EditorObjectId> doomed =
            CollectThinVictims(ctx, targets, intent, outStatus);
        if (doomed.empty())
        {
            return nullptr;
        }

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(doomed.size());
        for (const EditorObjectId id : doomed)
        {
            commands.push_back(std::make_unique<DeleteObjectCommand>(id));
        }
        outStatus = "Removed " + std::to_string(doomed.size()) + " of " +
            std::to_string(targets.size());
        return FoldIntoOneEntry(std::move(commands), "Thin");
    }

    // ---------------------------------------------------------------- createZone

    // DRAW THE REGION, so that saying WHERE stops costing a round trip.
    //
    // A level with no zones is the common case -- atoll has none -- and on such a level
    // every spatial phrase went the long way: ask for the island's bounds, then scatter in
    // a disc around a point, which is not the shape anybody meant. `params.zone`,
    // `target.where.zone` and the whole "the selected zone means its area" rule were
    // unreachable until somebody opened the Create menu by hand.
    //
    // Circle and rect only. A spline is a list of control points somebody drags, and there
    // is no sentence that places eight of them where they were wanted.
    std::unique_ptr<EditorCommand> BuildCreateZone(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        (void)targets;
        EditorContext& ctx = actionCtx.editor;

        const std::string shapeName = StringOr(intent.params, "shape", "circle");
        const editorzone::Shape shape = shapeName == "rect"
            ? editorzone::Shape::Rect : editorzone::Shape::Circle;
        const float size = std::clamp(NumberOr(intent.params, "size", 25.0f), 1.0f, 2000.0f);

        std::string name = StringOr(intent.params, "name", "");
        if (name.empty())
        {
            // Numbered, like the menu does it. Two zones called "Zone" cannot be told apart
            // in a phrase, which is the one thing a zone name is for.
            int ordinal = 1;
            for (const EditorObject& object : ctx.document.Objects())
            {
                if (object.type == editorzone::kTypeName)
                {
                    ++ordinal;
                }
            }
            name = "Zone " + std::to_string(ordinal);
        }
        for (const EditorObject& object : ctx.document.Objects())
        {
            if (object.type == editorzone::kTypeName && object.name == name)
            {
                outStatus = "There is already a zone called '" + name + "'";
                return nullptr;
            }
        }

        // Where the scatter would have gone, so "make a zone here" and "plant here" agree
        // about where "here" is.
        Math::float3 centre = ResolveScatterAnchor(ctx, intent.target.where, size);
        if (intent.params.contains("at"))
        {
            centre = Vec3Or(intent.params, "at", centre);
        }

        outStatus = "Created zone '" + name + "'";
        return std::make_unique<CreateDocumentObjectCommand>(
            editorzone::BuildObject(shape, centre, size, name));
    }

    // Both of these run AFTER the targets are resolved and the per-kind breakdown is built,
    // so the counting is already done -- they only have to turn it into a sentence.
    void RefineGroupPreview(const EditorActionContext& actionCtx,
        const EditorIntent& intent,
        EditorIntentPreview& preview)
    {
        (void)actionCtx;
        const bool perAsset = BoolOr(intent.params, "perAsset", false);
        const std::string name = StringOr(intent.params, "name", "");
        const std::string count = std::to_string(preview.targets.size()) +
            (preview.targets.size() == 1 ? " object" : " objects");
        if (perAsset)
        {
            preview.summary = count + " into " + std::to_string(preview.groups.size()) +
                " groups, one per kind, each named after its asset";
        }
        else if (name.empty())
        {
            preview.summary = count + " taken out of whatever group they are in";
        }
        else
        {
            preview.summary = count + " into one group called \"" + name + "\"";
        }
    }

    void RefineRenamePreview(const EditorActionContext& actionCtx,
        const EditorIntent& intent,
        EditorIntentPreview& preview)
    {
        (void)actionCtx;
        const bool perAsset = BoolOr(intent.params, "perAsset", false);
        const std::string name = StringOr(intent.params, "name", "");
        const std::string count = std::to_string(preview.targets.size()) +
            (preview.targets.size() == 1 ? " object" : " objects");
        if (perAsset)
        {
            preview.summary = count + " renamed after their own kind, numbered within it"
                + (name.empty() ? "" : " and prefixed \"" + name + "\"");
        }
        else
        {
            preview.summary = count + " renamed to \"" + name +
                (preview.targets.size() > 1 ? " 001\"..." : "\"");
        }
    }

    std::unique_ptr<EditorCommand> BuildRename(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        // PER ASSET: name each one after WHAT IT IS. "Ð½Ð°Ð·Ð¾Ð²Ð¸ Ð¾Ð±ÑŠÐµÐºÑ‚Ñ‹ Ð½Ð¾Ñ€Ð¼Ð°Ð»ÑŒÐ½Ñ‹Ð¼Ð¸ Ð¸Ð¼ÐµÐ½Ð°Ð¼Ð¸"
        // is not one name for six hundred objects, it is six hundred objects each called
        // after its kind -- and with one `name` parameter there was no way to say that. The
        // model tried, was refused, tried again and ended up claiming the job was already
        // done. `group` grew the same parameter for the same reason.
        const bool perAsset = BoolOr(intent.params, "perAsset", false);
        const std::string base = StringOr(intent.params, "name", "");
        if (base.empty() && !perAsset)
        {
            outStatus = "rename needs a name to give them";
            return nullptr;
        }
        if (targets.empty())
        {
            outStatus = "Nothing to rename";
            return nullptr;
        }

        const EditorSceneDocument& document = actionCtx.editor.document;
        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        // One target takes the name as given. Several cannot: a level where two hundred
        // objects are all called "spheres" is a level whose outliner has stopped being
        // useful, and the search predicate behind every filter matches on name. So they are
        // numbered -- which is also what the level's own "Palm 001" naming already does.
        const bool numbered = targets.size() > 1;
        int ordinal = 0;
        // Per-asset numbering counts WITHIN a kind, so the palms are 001..222 and the rocks
        // start at 001 again. A single running counter would name the third rock "rock 604"
        // and tell the reader nothing.
        std::map<std::string, int> perKindOrdinal;
        std::map<std::string, std::size_t> renamedPerKind;
        for (const EditorObjectId id : targets)
        {
            const EditorObject* object = document.Find(id);
            if (!object)
            {
                outStatus = "Selection contains an object that is no longer in the level";
                return nullptr;
            }
            std::string after;
            if (perAsset)
            {
                const std::string kind = editormatch::PrettyAssetLabel(*object);
                if (kind.empty())
                {
                    continue;   // nothing to name it after; leave it alone rather than guess
                }
                const std::string stem = base.empty() ? kind : base + " " + kind;
                char suffix[8] = {};
                std::snprintf(suffix, sizeof(suffix), " %03d", ++perKindOrdinal[kind]);
                after = stem + suffix;
                ++renamedPerKind[kind];
            }
            else
            {
                after = base;
                if (numbered)
                {
                    char suffix[8] = {};
                    std::snprintf(suffix, sizeof(suffix), " %03d", ++ordinal);
                    after += suffix;
                }
            }
            if (after == object->name)
            {
                continue;
            }
            commands.push_back(std::make_unique<RenameObjectCommand>(id, object->name, after));
        }
        if (perAsset)
        {
            if (commands.empty())
            {
                outStatus = "They already carry those names";
                return nullptr;
            }
            std::string breakdown;
            for (const auto& [kind, count] : renamedPerKind)
            {
                breakdown += (breakdown.empty() ? "" : ", ") + kind + " x" +
                    std::to_string(count);
            }
            outStatus = "Renamed " + CountedObjects(commands.size()) + " after their kind: " +
                breakdown;
            return FoldIntoOneEntry(std::move(commands),
                "Rename " + std::to_string(commands.size()) + " Objects");
        }

        outStatus = "Renamed " + CountedObjects(commands.size()) + " to \"" + base +
            (numbered ? " 001\"..." : "\"");
        return FoldIntoOneEntry(std::move(commands),
            "Rename " + std::to_string(commands.size()) + " Objects");
    }

    // ------------------------------------------------------------- setMaterial

    std::unique_ptr<EditorCommand> BuildSetMaterial(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        const std::string material = StringOr(intent.params, "material", "");
        if (material.empty())
        {
            outStatus = "setMaterial needs a material to apply";
            return nullptr;
        }
        if (targets.empty())
        {
            outStatus = "Nothing to apply a material to";
            return nullptr;
        }
        // Refuse a material the level does not have, by name, rather than writing a
        // dangling reference that only shows up as a missing-asset error later (E5: the
        // resolver answers by name, the model never picks from a closed list).
        if (!actionCtx.assets.FindById({ EditorAssetType::MaterialPreset, material }) &&
            !actionCtx.assets.FindByPath(material))
        {
            outStatus = "Unknown material '" + material + "'";
            return nullptr;
        }

        // A slot is optional: without one the object's whole material is replaced, with one
        // only that slot changes. Two commands exist for exactly this distinction.
        const bool hasSlot = intent.params.contains("slot");
        const int slot = hasSlot
            ? std::max(0, static_cast<int>(NumberOr(intent.params, "slot", 0.0f))) : 0;

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            if (hasSlot)
            {
                commands.push_back(std::make_unique<SetMaterialSlotCommand>(id, slot, material));
            }
            else
            {
                commands.push_back(std::make_unique<SetMaterialCommand>(id, material));
            }
        }

        outStatus = "Set " + CountedObjects(targets.size()) + " to material '" + material + "'" +
            (hasSlot ? " (slot " + std::to_string(slot) + ")" : "");
        return FoldIntoOneEntry(std::move(commands),
            "Set Material on " + std::to_string(commands.size()) + " Objects");
    }

    // ------------------------------------------------------------------ isolate

    // Hide everything else and make sure the chosen ones are visible. Not the same as
    // `setEnabled false` with an exclude list, which is why it earns its own entry: that
    // hides the others but leaves an already-hidden target hidden, so "isolate the palms"
    // would show an empty island if somebody had hidden the palms an hour ago. Isolating
    // is two statements -- these ON, everything else OFF -- and the second half is the one
    // people forget they need.
    std::unique_ptr<EditorCommand> BuildIsolate(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent&,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        if (targets.empty())
        {
            outStatus = "Nothing to isolate";
            return nullptr;
        }

        std::vector<std::unique_ptr<EditorCommand>> commands;
        std::size_t shown = 0;
        std::size_t hidden = 0;
        for (const EditorObject& object : ctx.document.Objects())
        {
            const bool keep = std::find_if(targets.begin(), targets.end(),
                [&object](EditorObjectId id) { return id.value == object.id.value; }) != targets.end();
            if (object.enabled == keep)
            {
                continue;   // already in the state it should end in
            }
            std::unique_ptr<EditorCommand> command =
                BuildSetEnabledOne(ctx.document, object.id, keep);
            if (!command)
            {
                continue;   // not something that can be hidden; leave it alone
            }
            (keep ? shown : hidden) += 1;
            commands.push_back(std::move(command));
        }

        if (commands.empty())
        {
            outStatus = "Already isolated";
            return nullptr;
        }
        outStatus = "Isolated " + CountedObjects(targets.size()) +
            " (hid " + std::to_string(hidden);
        if (shown > 0)
        {
            outStatus += ", showed " + std::to_string(shown);
        }
        outStatus += ")";
        return FoldIntoOneEntry(std::move(commands),
            "Isolate " + std::to_string(targets.size()) + " Objects");
    }

    // ------------------------------------------------------------------- ground
    //
    // Shared by `bury` and by `spawn`'s placement: both answer "where is the surface
    // under this point", and both have to ignore the objects they are about to move or
    // create, or they measure against themselves.

    // The ground probe and its start height now live in editor/intent/EditorSceneQuery,
    // because the scatter is no longer the only caller: the model can ask how high the
    // ground is at a point, and the two answers have to be the same answer. They were a
    // copy here for exactly as long as there was one caller.
    using editorquery::kGroundProbeUp;
    using editorquery::ProbeGroundHeight;

    // Horizontal step for the two extra probes that estimate the slope.
    constexpr float kSlopeProbeStep = 0.75f;

    // ---------------------------------------------------------------------- bury

    // Bury depth limits, as a fraction of the object's world height. The caller passes the tuned
    // value (Level Editor > Placement); these only keep it sane.
    //
    // THE FRACTION IS THE DEPTH, which is not obvious and is why the first attempt overshot. It is
    // the thickness of the footing band that must end up under the surface, so demanding the WHOLE
    // band go under means the object sinks at least as deep as the band is tall. At a tenth of a
    // 15 m palm that was a metre and a half of trunk, and it looked exactly as buried as it sounds.
    // It only has to swallow the ground's unevenness under the footprint -- centimetres on sand.
    //
    // It is only the EXTRA depth, though: an object hovering a metre up still travels the whole
    // metre, because the band's vertices measure their real gap to the ground. The fraction decides
    // only how far PAST contact it ends up.
    constexpr float kBuryFootingFractionMin = 0.0005f;
    constexpr float kBuryFootingFractionMax = 0.25f;
    // Ray budget per object, per keypress. The answer is a maximum over a ring of points, so a few
    // dozen evenly spread samples find it; thousands only cost time the user can feel.
    constexpr std::size_t kBuryMaxProbes = 64;

    // BURY the targets: drop each until EVERY ONE of its footing vertices is under the surface it
    // is being buried into. This replaced a "snap to surface below" that cast ONE ray from the
    // bounds centre and rested the bottom of the AABB on what it hit; the owner asked for burying
    // instead and did not want the old behaviour kept, so it was removed rather than rebound.
    //
    // IT IS THE FOOTING THAT GETS BURIED, NOT THE WHOLE MESH -- and that is the lesson of the
    // first version. Taking "every vertex ends up under the surface" literally across ALL vertices
    // lets the HIGHEST one decide, so a 15 m palm sank fifteen metres and disappeared. Correct,
    // and useless. What burying has to fix is a footing hanging in the air on uneven ground: the
    // bottom ring of the trunk must be under the sand all the way round, while the crown is none
    // of this function's business. So only vertices within the footing fraction of the object's
    // world height, measured up from its lowest point, get a vote -- a fraction of the OBJECT
    // rather than a world distance, so it means the same thing for a palm and for a pebble.
    //
    // It still cannot use the AABB: its corners are not points on the mesh, so on a slope the
    // highest footing vertex is nowhere near a corner. For each footing vertex, in world space,
    // cast straight down -- a hit means that vertex is still ABOVE the surface by exactly that
    // distance. Drop by the LARGEST such distance and the whole footing goes under. Vertices
    // already beneath the surface find nothing below them and contribute nothing.
    //
    // AND THE RAYS ARE CAPPED at kBuryMaxProbes, because the first version was slow enough for the
    // user to notice. Every ray runs the scene broad phase and then exact triangles of whatever it
    // finds, and a terrain chunk is a great many triangles; thousands of rays per keypress is a
    // visible stall for an answer that is a MAXIMUM over a ring of points, which a few dozen evenly
    // spread samples locate just as well.
    std::unique_ptr<EditorCommand> BuildBury(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        if (targets.empty())
        {
            outStatus = "Nothing to bury";
            return nullptr;
        }

        float footingFraction = NumberOr(intent.params, "depthPercent", 1.0f) * 0.01f;
        footingFraction = std::clamp(footingFraction, kBuryFootingFractionMin, kBuryFootingFractionMax);

        // The whole target set is ignored, not just the object being moved: burying one palm into
        // the sand must not measure against another palm that is being buried with it.
        std::vector<Scene::SceneObjectId> ignoredObjectIds;
        ignoredObjectIds.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            ignoredObjectIds.push_back(id.value);
        }

        const Math::float3 rayDirection(0.0f, -1.0f, 0.0f);
        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        std::size_t meshCount = 0;
        std::size_t noSurfaceCount = 0;
        std::size_t noGeometryCount = 0;
        std::size_t alreadyBuriedCount = 0;

        for (const EditorObjectId id : targets)
        {
            EditorObject* object = ctx.document.Find(id);
            RenderableObjectBase* runtime = ctx.scene.FindEditorObject(id.value);
            RenderableObject* renderable = runtime ? runtime->AsRenderableObject() : nullptr;
            if (!object || !renderable)
            {
                continue;
            }
            ++meshCount;

            const Mesh* mesh = renderable->GetMesh();
            if (!mesh || !mesh->HasRaycastTriangles())
            {
                // No CPU geometry (a runtime generator, an instanced batch): there is no honest
                // per-vertex answer, and quietly falling back to the AABB would bury it wrong.
                ++noGeometryCount;
                continue;
            }

            const Math::mat4& world = renderable->GetModelMatrix();
            const std::vector<Math::float3>& localPositions = mesh->RaycastPositions();

            // The footing band is measured in WORLD height: the object can be rotated, so the
            // mesh's own local Y is not the direction gravity cares about.
            float lowestY = std::numeric_limits<float>::max();
            float highestY = std::numeric_limits<float>::lowest();
            for (const Math::float3& local : localPositions)
            {
                const float y = world.TransformPoint(local).y;
                lowestY = std::min(lowestY, y);
                highestY = std::max(highestY, y);
            }
            if (!(highestY >= lowestY))
            {
                ++noGeometryCount;
                continue;
            }
            const float footingTopY = lowestY + (highestY - lowestY) * footingFraction;

            // Gather the footing FIRST and thin it after: striding the raw vertex list would spend
            // the ray budget on the crown and leave the ring underneath barely sampled.
            std::vector<Math::float3> footing;
            for (const Math::float3& local : localPositions)
            {
                const Math::float3 worldPos = world.TransformPoint(local);
                if (worldPos.y <= footingTopY)
                {
                    footing.push_back(worldPos);
                }
            }
            if (footing.empty())
            {
                ++noGeometryCount;
                continue;
            }
            const std::size_t stride = (footing.size() + kBuryMaxProbes - 1) / kBuryMaxProbes;

            // A PROBE THAT IS ALREADY UNDERGROUND MUST NOT VOTE, and finding that out needs the
            // upward ray. Casting only downwards cannot tell "hovering above the sand" from
            // "buried in it": a vertex inside the terrain still reports a hit below it -- the far
            // side of the surface, or the slope further down -- so pressing the key again would
            // sink an already-buried object deeper every time, without limit.
            //
            // Anything hit going UP means this vertex has surface over it, which is the definition
            // of buried. Overhanging geometry (a neighbour's crown) can answer this too, and that
            // is the safe direction to be wrong in: such a probe abstains, so the object is buried
            // slightly less rather than run away downwards.
            const Math::float3 rayUp(0.0f, 1.0f, 0.0f);
            float deepest = 0.0f;
            bool hitAnything = false;
            std::size_t probeCount = 0;
            std::size_t coveredProbes = 0;
            for (std::size_t v = 0; v < footing.size(); v += stride)
            {
                ++probeCount;
                float upDistance = 0.0f;
                if (ctx.scene.RaycastEditorObject(
                        footing[v], rayUp, &upDistance, 0, &ignoredObjectIds) != 0)
                {
                    ++coveredProbes;
                    continue;   // already under a surface: contributes nothing
                }
                float hitDistance = 0.0f;
                const Scene::SceneObjectId hit = ctx.scene.RaycastEditorObject(
                    footing[v], rayDirection, &hitDistance, 0, &ignoredObjectIds);
                if (hit == 0 || !std::isfinite(hitDistance))
                {
                    continue;   // nothing below this vertex either
                }
                hitAnything = true;
                deepest = std::max(deepest, hitDistance);
            }

            // Every probe covered: the footing is fully under. Leave the object exactly where it
            // is -- re-running the tool on a finished object is a no-op, not a nudge.
            if (probeCount > 0 && coveredProbes == probeCount)
            {
                ++alreadyBuriedCount;
                continue;
            }

            if (!hitAnything)
            {
                ++noSurfaceCount;
                continue;
            }
            if (deepest <= 1.0e-4f)
            {
                ++alreadyBuriedCount;
                continue;
            }

            EditorTransform after = object->transform;
            // A hair past contact, so the highest vertex ends up INSIDE rather than coplanar with
            // the surface -- coplanar is where z-fighting lives.
            after.position.y -= deepest + 1.0e-3f;
            commands.push_back(std::make_unique<TransformObjectCommand>(
                object->id, object->transform, after));
        }

        if (meshCount == 0)
        {
            outStatus = "Select one or more meshes to bury";
            return nullptr;
        }
        if (commands.empty())
        {
            if (noGeometryCount == meshCount)
            {
                outStatus = "Selected object has no CPU geometry to bury";
            }
            else if (alreadyBuriedCount > 0 && noSurfaceCount == 0)
            {
                outStatus = meshCount == 1 ? "Selection is already buried"
                                           : "Selected meshes are already buried";
            }
            else
            {
                outStatus = "No visible editor object below selected meshes";
            }
            return nullptr;
        }

        const std::size_t buriedCount = commands.size();
        outStatus = buriedCount == 1 ? "Buried selection below surface"
                                     : "Buried " + std::to_string(buriedCount) + " objects below surface";
        return FoldIntoOneEntry(std::move(commands),
            "Bury " + std::to_string(buriedCount) + " Objects");
    }

    // --------------------------------------------------------------- transforms

    using TransformFn = void (*)(EditorTransform& t, const Math::float3& value, bool relative);

    void ApplyMove(EditorTransform& t, const Math::float3& value, bool relative)
    {
        t.position = relative
            ? Math::float3(t.position.x + value.x, t.position.y + value.y, t.position.z + value.z)
            : value;
    }

    void ApplyRotate(EditorTransform& t, const Math::float3& value, bool relative)
    {
        t.rotationDeg = relative
            ? Math::float3(t.rotationDeg.x + value.x, t.rotationDeg.y + value.y, t.rotationDeg.z + value.z)
            : value;
    }

    void ApplyScale(EditorTransform& t, const Math::float3& value, bool relative)
    {
        // Relative scaling MULTIPLIES. "make them 20% bigger" is x1.2, not +1.2, and the
        // additive reading would flip a 0.05 pebble into a boulder.
        t.scale = relative
            ? Math::float3(t.scale.x * value.x, t.scale.y * value.y, t.scale.z * value.z)
            : value;
    }

    std::unique_ptr<EditorCommand> BuildTransform(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        TransformFn apply,
        const char* verbPast,
        const char* historyVerb,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        if (targets.empty())
        {
            outStatus = "Nothing to transform";
            return nullptr;
        }

        const Math::float3 value = Vec3Or(intent.params, "value", Math::float3(0.0f, 0.0f, 0.0f));
        const bool relative = BoolOr(intent.params, "relative", true);

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            EditorObject* object = ctx.document.Find(id);
            if (!object)
            {
                // Environment entities have no EditorTransform to edit through this path.
                continue;
            }
            EditorTransform after = object->transform;
            apply(after, value, relative);
            commands.push_back(std::make_unique<TransformObjectCommand>(
                object->id, object->transform, after));
        }

        if (commands.empty())
        {
            outStatus = "No object in the target set has a transform";
            return nullptr;
        }
        outStatus = std::string(verbPast) + " " + CountedObjects(commands.size());
        return FoldIntoOneEntry(std::move(commands),
            std::string(historyVerb) + " " + std::to_string(commands.size()) + " Objects");
    }

    std::unique_ptr<EditorCommand> BuildMove(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets, const EditorIntent& intent, std::string& outStatus)
    {
        return BuildTransform(actionCtx, targets, intent, &ApplyMove, "Moved", "Move", outStatus);
    }

    std::unique_ptr<EditorCommand> BuildRotate(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets, const EditorIntent& intent, std::string& outStatus)
    {
        return BuildTransform(actionCtx, targets, intent, &ApplyRotate, "Rotated", "Rotate", outStatus);
    }

    std::unique_ptr<EditorCommand> BuildScale(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets, const EditorIntent& intent, std::string& outStatus)
    {
        EditorIntent adjusted = intent;
        if (!adjusted.params.contains("value"))
        {
            adjusted.params["value"] = nlohmann::json::array({ 1.0f, 1.0f, 1.0f });
        }
        return BuildTransform(actionCtx, targets, adjusted, &ApplyScale, "Scaled", "Scale", outStatus);
    }

    // ----------------------------------------------------- align / distribute / snap

    // Which component of a position an axis name refers to. Shared by all three placement
    // actions so "y" cannot mean one thing to align and another to snap.
    int AxisIndex(const std::string& axis)
    {
        if (axis == "x") { return 0; }
        if (axis == "y") { return 1; }
        return 2;   // "z"
    }

    float AxisValue(const Math::float3& v, int axis)
    {
        return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
    }

    // Math::float3 has no operator==, and these want an EXACT comparison anyway: the
    // question is "did this transform change at all", so that a no-op does not become an
    // undo step that undoes nothing.
    bool SameVec3(const Math::float3& a, const Math::float3& b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    void SetAxisValue(Math::float3& v, int axis, float value)
    {
        if (axis == 0) { v.x = value; }
        else if (axis == 1) { v.y = value; }
        else { v.z = value; }
    }

    // The objects a placement action can actually move: document objects with a transform.
    // Environment entities have no transform to line up.
    std::vector<EditorObject*> CollectMovable(EditorContext& ctx,
        const std::vector<EditorObjectId>& targets)
    {
        std::vector<EditorObject*> movable;
        movable.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            if (EditorObject* object = ctx.document.Find(id))
            {
                movable.push_back(object);
            }
        }
        return movable;
    }

    std::unique_ptr<EditorCommand> FoldTransforms(std::vector<EditorObject*>& movable,
        const std::vector<Math::float3>& after,
        const std::string& label,
        std::string& outStatus,
        const std::string& doneWord)
    {
        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(movable.size());
        for (std::size_t i = 0; i < movable.size(); ++i)
        {
            if (SameVec3(movable[i]->transform.position, after[i]))
            {
                continue;   // already there; a no-op entry is an undo step that does nothing
            }
            EditorTransform t = movable[i]->transform;
            t.position = after[i];
            commands.push_back(std::make_unique<TransformObjectCommand>(
                movable[i]->id, movable[i]->transform, t));
        }
        if (commands.empty())
        {
            outStatus = "Nothing moved -- they were already " + doneWord;
            return nullptr;
        }
        outStatus = doneWord + " " + CountedObjects(commands.size());
        return FoldIntoOneEntry(std::move(commands),
            label + " " + std::to_string(commands.size()) + " Objects");
    }

    std::unique_ptr<EditorCommand> BuildAlign(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        std::vector<EditorObject*> movable = CollectMovable(actionCtx.editor, targets);
        if (movable.size() < 2)
        {
            outStatus = "Aligning needs at least two objects with a transform";
            return nullptr;
        }

        const int axis = AxisIndex(StringOr(intent.params, "axis", "x"));
        const std::string to = StringOr(intent.params, "to", "average");

        float lo = AxisValue(movable.front()->transform.position, axis);
        float hi = lo;
        float sum = 0.0f;
        for (const EditorObject* object : movable)
        {
            const float value = AxisValue(object->transform.position, axis);
            lo = std::min(lo, value);
            hi = std::max(hi, value);
            sum += value;
        }

        float target = sum / static_cast<float>(movable.size());
        if (to == "min")   { target = lo; }
        else if (to == "max")   { target = hi; }
        else if (to == "center") { target = (lo + hi) * 0.5f; }
        else if (to == "first")  { target = AxisValue(movable.front()->transform.position, axis); }

        std::vector<Math::float3> after;
        after.reserve(movable.size());
        for (const EditorObject* object : movable)
        {
            Math::float3 position = object->transform.position;
            SetAxisValue(position, axis, target);
            after.push_back(position);
        }
        return FoldTransforms(movable, after, "Align", outStatus, "aligned");
    }

    std::unique_ptr<EditorCommand> BuildDistribute(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        std::vector<EditorObject*> movable = CollectMovable(actionCtx.editor, targets);
        if (movable.size() < 3)
        {
            // Two objects are already evenly spaced, whatever the spacing is.
            outStatus = "Distributing needs at least three objects";
            return nullptr;
        }

        const int axis = AxisIndex(StringOr(intent.params, "axis", "x"));
        // Order along the axis FIRST. Distributing in selection order would shuffle them
        // past each other, which is never what the request means.
        std::sort(movable.begin(), movable.end(),
            [axis](const EditorObject* a, const EditorObject* b)
            {
                return AxisValue(a->transform.position, axis) <
                       AxisValue(b->transform.position, axis);
            });

        const float first = AxisValue(movable.front()->transform.position, axis);
        const float last = AxisValue(movable.back()->transform.position, axis);
        const float spanSteps = static_cast<float>(movable.size() - 1);
        // An explicit spacing walks out from the first one; without it the two ends stay
        // put and everything between them is evened out -- which is what "space them
        // evenly" means to someone who has already placed the ends deliberately.
        float step = (last - first) / spanSteps;
        const auto spacingIt = intent.params.find("spacing");
        if (spacingIt != intent.params.end() && spacingIt->is_number())
        {
            step = spacingIt->get<float>();
        }

        std::vector<Math::float3> after;
        after.reserve(movable.size());
        for (std::size_t i = 0; i < movable.size(); ++i)
        {
            Math::float3 position = movable[i]->transform.position;
            SetAxisValue(position, axis, first + step * static_cast<float>(i));
            after.push_back(position);
        }
        return FoldTransforms(movable, after, "Distribute", outStatus, "spaced");
    }

    std::unique_ptr<EditorCommand> BuildSnap(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        std::vector<EditorObject*> movable = CollectMovable(actionCtx.editor, targets);
        if (movable.empty())
        {
            outStatus = "Nothing with a transform to snap";
            return nullptr;
        }

        const float grid = std::max(0.0001f, NumberOr(intent.params, "grid", 1.0f));
        // DEFAULT IS THE HORIZONTAL PLANE, and that is a decision, not an oversight.
        // Snapping height to a grid on uneven ground lifts objects off it -- it undoes
        // exactly what `bury` exists to do. Someone who wants it can ask for "all"; nobody
        // should get it by typing "snap to grid" on a hillside.
        const std::string axes = StringOr(intent.params, "axes", "xz");
        const bool snapX = axes != "y" && axes != "z";
        const bool snapY = axes == "all" || axes == "y";
        const bool snapZ = axes != "x" && axes != "y";
        const float rotationStep = std::max(0.0f, NumberOr(intent.params, "rotationStep", 0.0f));

        const auto quantise = [grid](float value)
        {
            return std::round(value / grid) * grid;
        };

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(movable.size());
        for (EditorObject* object : movable)
        {
            EditorTransform t = object->transform;
            if (snapX) { t.position.x = quantise(t.position.x); }
            if (snapY) { t.position.y = quantise(t.position.y); }
            if (snapZ) { t.position.z = quantise(t.position.z); }
            if (rotationStep > 0.0f)
            {
                t.rotationDeg.y = std::round(t.rotationDeg.y / rotationStep) * rotationStep;
            }
            if (SameVec3(t.position, object->transform.position) &&
                SameVec3(t.rotationDeg, object->transform.rotationDeg))
            {
                continue;
            }
            commands.push_back(std::make_unique<TransformObjectCommand>(
                object->id, object->transform, t));
        }

        if (commands.empty())
        {
            outStatus = "Nothing moved -- already on the grid";
            return nullptr;
        }
        outStatus = "Snapped " + CountedObjects(commands.size()) + " to a " +
            envsettings::ToText(grid) + " m grid";
        return FoldIntoOneEntry(std::move(commands),
            "Snap " + std::to_string(commands.size()) + " Objects");
    }

    // -------------------------------------------------------------- randomizing
    //
    // This is what makes a scattered set look scattered. Ten palms spawned from one asset
    // with one rotation and one scale are ten copies of the same palm standing in the same
    // pose; the eye reads the repetition instantly, long before it reads the positions.
    // So it is not decoration on top of `spawn` -- it is half of what "scatter" means, and
    // it is separately useful on props that were placed by hand.

    std::unique_ptr<EditorCommand> BuildRandomizeRotation(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        if (targets.empty())
        {
            outStatus = "Nothing to randomize";
            return nullptr;
        }

        float lo = 0.0f;
        float hi = 360.0f;
        RangeOr(intent.params, "range", lo, hi);
        const bool allAxes = StringOr(intent.params, "axis", "yaw") == "all";

        std::mt19937 rng(SeedFor(intent.params, ctx.document));
        std::uniform_real_distribution<float> dist(lo, hi);

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            EditorObject* object = ctx.document.Find(id);
            if (!object)
            {
                continue;
            }
            EditorTransform after = object->transform;
            // Yaw only by default: a randomly pitched tree lies on its side. "all" exists
            // for rocks and debris, where any orientation is a legal one.
            after.rotationDeg.y = dist(rng);
            if (allAxes)
            {
                after.rotationDeg.x = dist(rng);
                after.rotationDeg.z = dist(rng);
            }
            commands.push_back(std::make_unique<TransformObjectCommand>(
                object->id, object->transform, after));
        }

        if (commands.empty())
        {
            outStatus = "No object in the target set has a transform";
            return nullptr;
        }
        outStatus = "Randomized rotation on " + CountedObjects(commands.size());
        return FoldIntoOneEntry(std::move(commands),
            "Randomize Rotation " + std::to_string(commands.size()) + " Objects");
    }

    std::unique_ptr<EditorCommand> BuildRandomizeScale(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        if (targets.empty())
        {
            outStatus = "Nothing to randomize";
            return nullptr;
        }

        float lo = 0.9f;
        float hi = 1.1f;
        RangeOr(intent.params, "range", lo, hi);
        const bool uniform = BoolOr(intent.params, "uniform", true);

        std::mt19937 rng(SeedFor(intent.params, ctx.document));
        std::uniform_real_distribution<float> dist(lo, hi);

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            EditorObject* object = ctx.document.Find(id);
            if (!object)
            {
                continue;
            }
            EditorTransform after = object->transform;
            // The range MULTIPLIES whatever scale the object already has, so a mesh whose
            // asset spawns at 0.03 stays at its own size and only varies around it.
            const float fx = dist(rng);
            const float fy = uniform ? fx : dist(rng);
            const float fz = uniform ? fx : dist(rng);
            after.scale = Math::float3(after.scale.x * fx, after.scale.y * fy, after.scale.z * fz);
            commands.push_back(std::make_unique<TransformObjectCommand>(
                object->id, object->transform, after));
        }

        if (commands.empty())
        {
            outStatus = "No object in the target set has a transform";
            return nullptr;
        }
        outStatus = "Randomized scale on " + CountedObjects(commands.size());
        return FoldIntoOneEntry(std::move(commands),
            "Randomize Scale " + std::to_string(commands.size()) + " Objects");
    }

    // -------------------------------------------------------------------- replace

    std::unique_ptr<EditorCommand> BuildReplace(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        if (targets.empty())
        {
            outStatus = "Nothing to replace";
            return nullptr;
        }
        if (intent.target.asset.empty())
        {
            outStatus = "replace needs an asset to switch to";
            return nullptr;
        }

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        std::size_t skipped = 0;
        for (const EditorObjectId id : targets)
        {
            const EditorObject* object = ctx.document.Find(id);
            if (!object || object->type != "staticMesh")
            {
                ++skipped;
                continue;
            }
            commands.push_back(std::make_unique<SetMeshAssetCommand>(id, intent.target.asset));
        }

        if (commands.empty())
        {
            outStatus = "No static mesh in the target set to replace";
            return nullptr;
        }
        outStatus = "Replaced the mesh on " + CountedObjects(commands.size());
        if (skipped > 0)
        {
            outStatus += " (" + std::to_string(skipped) + " skipped: not a static mesh)";
        }
        return FoldIntoOneEntry(std::move(commands),
            "Replace Mesh " + std::to_string(commands.size()) + " Objects");
    }

    // ---------------------------------------------------------- setEnvironment

    // The look of the world -- fog, sun, wind, water, exposure, grading. It goes through
    // EditEnvironmentCommand and nothing else, which is the entire point: the engine's
    // `--set` namespace could reach the same knobs in one line, but it writes into runtime
    // state, so the edit would not be in the undo stack, would not be in the document, and
    // would vanish on the next load. An edit the user cannot take back is worse than one
    // they cannot make.
    std::unique_ptr<EditorCommand> BuildSetEnvironment(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>&,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;

        const std::vector<envsettings::Setting> settings = envsettings::Enumerate(ctx.document);
        const envsettings::Setting* setting = envsettings::Find(settings, intent.target.setting);
        if (!setting)
        {
            outStatus = intent.target.setting.empty()
                ? "setEnvironment needs a setting to change"
                : "No environment setting called '" + intent.target.setting + "'";
            return nullptr;
        }

        const EditorObject* entity = nullptr;
        for (const EditorObject& candidate : ctx.document.Environment())
        {
            if (candidate.id.value == setting->owner.value)
            {
                entity = &candidate;
                break;
            }
        }
        if (!entity)
        {
            outStatus = "The entity holding '" + setting->path + "' is gone";
            return nullptr;
        }

        nlohmann::json newValue;
        if (!ComputeEnvironmentValue(*setting, intent.params, newValue, outStatus))
        {
            return nullptr;
        }

        nlohmann::json after = entity->properties;
        if (!envsettings::Write(after, setting->subPath, newValue))
        {
            outStatus = "Cannot write '" + setting->path + "'";
            return nullptr;
        }
        if (after == entity->properties)
        {
            outStatus = setting->path + " is already " + envsettings::ToText(newValue);
            return nullptr;
        }

        outStatus = setting->path + ": " + envsettings::ToText(setting->current) +
            " -> " + envsettings::ToText(newValue);
        return std::make_unique<EditEnvironmentCommand>(
            entity->id, entity->properties, std::move(after), "Set " + setting->path);
    }

    // ---------------------------------------------------------------------- spawn

    // Where a scatter is centred when the phrase did not say. Looking at a spot and saying
    // "plant ten palms" has one obvious meaning, and it is this one.
    Math::float3 ResolveScatterAnchor(EditorContext& ctx, const EditorSpatialFilter& where, float radius)
    {
        if (where.anchor == EditorSpatialAnchor::Point)
        {
            return where.point;
        }

        if (where.anchor == EditorSpatialAnchor::Selection && !ctx.selection.Empty())
        {
            Math::float3 sum(0.0f, 0.0f, 0.0f);
            std::size_t count = 0;
            for (const EditorObjectId id : ctx.selection.Ordered())
            {
                if (const EditorObject* object = ctx.document.Find(id))
                {
                    sum = Math::float3(sum.x + object->transform.position.x,
                        sum.y + object->transform.position.y,
                        sum.z + object->transform.position.z);
                    ++count;
                }
            }
            if (count > 0)
            {
                const float inv = 1.0f / static_cast<float>(count);
                return Math::float3(sum.x * inv, sum.y * inv, sum.z * inv);
            }
        }

        // Camera, or nothing stated: the point the camera is actually looking AT, not where
        // it stands -- otherwise a scatter lands around the user's own feet.
        const Math::float3 origin = ctx.scene.CameraRef().GetPosition();
        const Math::float3 direction = ctx.scene.CameraRef().GetDirection();
        float distance = 0.0f;
        if (ctx.scene.RaycastEditorObject(origin, direction, &distance) != 0 && std::isfinite(distance))
        {
            return origin + direction * distance;
        }
        return origin + direction * std::max(radius, 10.0f);
    }

    std::unique_ptr<EditorCommand> BuildSpawn(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>&,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;

        // Both spellings, composed HERE rather than trusting the parser to have normalised
        // them: an intent can be built by hand -- the grammar source does it, the gate does
        // it -- and a builder that only reads `assets` quietly refuses those.
        std::vector<std::string> assetNames = intent.target.assets;
        if (assetNames.empty() && !intent.target.asset.empty())
        {
            assetNames.push_back(intent.target.asset);
        }
        if (assetNames.empty())
        {
            outStatus = "spawn needs an asset to place";
            return nullptr;
        }
        const IEditorObjectFactory* factory = actionCtx.extensions.FindObjectFactory("staticMesh");
        if (!factory)
        {
            outStatus = "No static-mesh factory is registered";
            return nullptr;
        }
        // Every named asset is resolved UP FRONT, so a phrase naming three kinds and
        // misspelling one says which one rather than planting two thirds of what was asked
        // for and reporting success.
        std::vector<const EditorAssetRecord*> records;
        records.reserve(assetNames.size());
        for (const std::string& name : assetNames)
        {
            // For mesh records the registry sets id.key to the path, but not for every asset
            // type, so try both spellings rather than depending on that.
            const EditorAssetRecord* record =
                actionCtx.assets.FindById({ EditorAssetType::Mesh, name });
            if (!record)
            {
                record = actionCtx.assets.FindByPath(name);
            }
            if (!record)
            {
                outStatus = "Unknown asset '" + name + "'";
                return nullptr;
            }
            if (!factory->CanBuildFromAsset(record))
            {
                outStatus = "'" + record->displayName + "' cannot be spawned as a static mesh";
                return nullptr;
            }
            records.push_back(record);
        }
        const EditorAssetRecord* record = records.front();

        const int requested = std::clamp(static_cast<int>(NumberOr(intent.params, "count", 1.0f)), 1, 200);
        const float radius = std::max(0.5f, NumberOr(intent.params, "radius", 25.0f));
        const float minSeparation = std::max(0.0f, NumberOr(intent.params, "minSeparation", 6.0f));
        const bool alignToGround = BoolOr(intent.params, "alignToGround", true);
        const float minNormalY = std::clamp(NumberOr(intent.params, "minGroundNormalY", 0.82f), 0.0f, 1.0f);
        float yawLo = 0.0f;
        float yawHi = 360.0f;
        RangeOr(intent.params, "yawRange", yawLo, yawHi);
        float scaleLo = 0.9f;
        float scaleHi = 1.1f;
        RangeOr(intent.params, "scaleRange", scaleLo, scaleHi);

        // A named zone replaces the disc entirely: its own centre, its own shape, its own
        // size. This is the answer to the phrases that had nowhere to say WHERE -- "only on
        // the beach, not in the water" was refused outright, and "along the shore" got a
        // 15 m circle, which is worse than a refusal because it looks like it worked.
        const std::string zoneName = StringOr(intent.params, "zone", "");
        bool hasZone = false;
        editorzone::Zone zone;
        if (!zoneName.empty())
        {
            std::string whyNot;
            if (!editorzone::Find(ctx.document, zoneName, zone, whyNot))
            {
                outStatus = whyNot;
                return nullptr;
            }
            hasZone = true;
        }

        const float scatterRadius = hasZone ? editorzone::BoundingRadius(zone) : radius;
        // Zone first, then an explicit point, then wherever the camera is looking. The
        // order is the order of how specific each one is, and a zone is the most specific
        // thing anyone can say -- it is a shape somebody drew, not a guess at a radius.
        const bool hasPoint = !hasZone && intent.params.contains("at");
        const Math::float3 anchor = hasZone
            ? zone.centre
            : (hasPoint ? Vec3Or(intent.params, "at", Math::float3(0.0f, 0.0f, 0.0f))
                        : ResolveScatterAnchor(ctx, intent.target.where, radius));

        // Anything already in the level within reach is an obstacle, so a scatter does not
        // grow a palm out of a rock. Terrain is excluded by the fact that it is what we
        // measure AGAINST -- only objects with a transform inside the disc count.
        // WITH THEIR SIZE, not just their position. `minSeparation` is a distance between
        // CENTRES, so a boulder ten metres across and a palm six metres apart satisfy it
        // and intersect anyway -- which is precisely what "чтобы меши не пересекались"
        // is asking not to happen. The terrain is a different matter and is deliberately
        // still excluded: things are meant to sit IN the ground.
        //
        // The extent comes from the scene's world bounds, which exist for anything already
        // placed. What is being spawned has no bounds yet -- it is not in the scene, the
        // asset record carries none and the .mesh.json does not store them -- so a new
        // object's own size is taken from another instance of the same asset when the level
        // has one, and otherwise falls back to the plain separation. That gap is real and
        // it is the one place this can still put two new meshes into each other.
        struct Obstacle
        {
            Math::float3 position;
            float radius = 0.0f;
        };
        std::vector<Obstacle> obstacles;
        float newObjectRadius = 0.0f;
        const auto xzRadiusOf = [&ctx](const EditorObject& object) -> float
        {
            const RenderableObjectBase* renderable =
                ctx.scene.FindEditorObject(object.id.value);
            if (!renderable)
            {
                return 0.0f;
            }
            const AABB& bounds = renderable->GetWorldBounds();
            if (!bounds.IsValid())
            {
                return 0.0f;
            }
            const Math::float3 mn = bounds.GetMin();
            const Math::float3 mx = bounds.GetMax();
            return std::max(mx.x - mn.x, mx.z - mn.z) * 0.5f;
        };
        const float obstacleReach = scatterRadius + minSeparation;
        for (const EditorObject& object : ctx.document.Objects())
        {
            const float objectRadius = xzRadiusOf(object);
            if (!record->id.key.empty() && newObjectRadius <= 0.0f &&
                editormatch::MatchesSearch(object, record->id.key))
            {
                newObjectRadius = objectRadius;
            }
            // THE GROUND IS NOT AN OBSTACLE. Anything wider than the scatter disc itself is
            // something you stand ON, not something you go around -- an island, a terrain
            // chunk, a lagoon floor. The old code got this right by accident: it stored
            // positions only, and the island's position is one point in the middle. Giving
            // obstacles their true extent made that accident visible immediately -- the
            // island's 180-metre radius rejected every candidate on the level and the
            // scatter reported "found nowhere to place coconut_palm".
            if (objectRadius > scatterRadius)
            {
                continue;
            }
            const Math::float3& p = object.transform.position;
            const float dx = p.x - anchor.x;
            const float dz = p.z - anchor.z;
            if (dx * dx + dz * dz <= obstacleReach * obstacleReach)
            {
                obstacles.push_back({ p, objectRadius });
            }
        }

        // The ocean is not a surface to plant on. Where the level has one, the waterline is
        // the floor: a palm whose root is under it is a palm growing out of the sea.
        bool hasWaterLevel = false;
        float waterLevel = 0.0f;
        if (const OceanRenderable* ocean = ctx.scene.FindOceanRenderable())
        {
            hasWaterLevel = true;
            waterLevel = ocean->GetWaterLevel();
        }
        const float minHeight = NumberOr(intent.params, "minHeight",
            hasWaterLevel ? waterLevel + 0.45f : -std::numeric_limits<float>::max());

        std::mt19937 rng(SeedFor(intent.params, ctx.document));
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::uniform_real_distribution<float> yawDist(yawLo, yawHi);
        std::uniform_real_distribution<float> scaleDist(scaleLo, scaleHi);

        const std::vector<Scene::SceneObjectId> noIgnores;
        std::vector<Math::float3> placed;
        placed.reserve(static_cast<std::size_t>(requested));
        const int maxAttempts = std::min(20000, requested * 200);
        std::size_t rejectedNoGround = 0;
        std::size_t rejectedUnderwater = 0;
        std::size_t rejectedSteep = 0;
        std::size_t rejectedCrowded = 0;

        for (int attempt = 0; attempt < maxAttempts && static_cast<int>(placed.size()) < requested; ++attempt)
        {
            float x = 0.0f;
            float z = 0.0f;
            if (hasZone)
            {
                // The zone samples itself, uniformly by area, in its own rotated frame.
                const Math::float3 point = editorzone::SamplePoint(zone, unit(rng), unit(rng));
                // ...and is asked whether it meant it. A spline filling INSIDE samples its
                // bounding box, so a concave coastline hands back points outside itself;
                // rejecting them here costs one attempt out of a budget that already
                // rejects water and slopes, and keeps every shape honest through one path.
                if (!editorzone::Contains(zone, point))
                {
                    ++rejectedCrowded;
                    continue;
                }
                x = point.x;
                z = point.z;
            }
            else
            {
                // sqrt of a uniform makes the disc UNIFORM by area; without it everything
                // bunches at the centre, which reads as a clump rather than a scatter.
                const float r = radius * std::sqrt(unit(rng));
                const float theta = unit(rng) * 6.2831853f;
                x = anchor.x + r * std::cos(theta);
                z = anchor.z + r * std::sin(theta);
            }
            const float probeStart = anchor.y + kGroundProbeUp;

            float height = 0.0f;
            if (alignToGround)
            {
                if (!ProbeGroundHeight(ctx.scene, x, z, probeStart, noIgnores, height))
                {
                    ++rejectedNoGround;
                    continue;
                }
                if (height < minHeight)
                {
                    ++rejectedUnderwater;
                    continue;
                }

                // Slope from two extra probes. For a height field the up-normal's Y is
                // 1/sqrt(dhdx^2 + dhdz^2 + 1), so the two gradients are the whole answer and
                // no cross product is needed.
                float hx = 0.0f;
                float hz = 0.0f;
                if (ProbeGroundHeight(ctx.scene, x + kSlopeProbeStep, z, probeStart, noIgnores, hx) &&
                    ProbeGroundHeight(ctx.scene, x, z + kSlopeProbeStep, probeStart, noIgnores, hz))
                {
                    const float dhdx = (hx - height) / kSlopeProbeStep;
                    const float dhdz = (hz - height) / kSlopeProbeStep;
                    const float normalY = 1.0f / std::sqrt(dhdx * dhdx + dhdz * dhdz + 1.0f);
                    if (normalY < minNormalY)
                    {
                        ++rejectedSteep;
                        continue;
                    }
                }
            }
            else
            {
                height = anchor.y;
            }

            const Math::float3 candidate(x, height, z);
            // Two of the new kind keep `minSeparation` between centres, plus their own
            // width where it is known. Against something already standing there, the gap
            // is measured from its EDGE -- that is the difference between "six metres
            // apart" and "not inside each other".
            const float mineSq = (minSeparation + newObjectRadius * 2.0f) *
                (minSeparation + newObjectRadius * 2.0f);
            const auto tooCloseToNew = [&](const Math::float3& other)
            {
                const float dx = other.x - candidate.x;
                const float dz = other.z - candidate.z;
                return dx * dx + dz * dz < mineSq;
            };
            const auto tooCloseToStanding = [&](const Obstacle& other)
            {
                const float dx = other.position.x - candidate.x;
                const float dz = other.position.z - candidate.z;
                const float clear = minSeparation + other.radius + newObjectRadius;
                return dx * dx + dz * dz < clear * clear;
            };
            if ((minSeparation > 0.0f || newObjectRadius > 0.0f) &&
                (std::any_of(placed.begin(), placed.end(), tooCloseToNew) ||
                 std::any_of(obstacles.begin(), obstacles.end(), tooCloseToStanding)))
            {
                ++rejectedCrowded;
                continue;
            }
            placed.push_back(candidate);
        }

        if (placed.empty())
        {
            outStatus = "Found nowhere to place " + record->displayName +
                " (no ground: " + std::to_string(rejectedNoGround) +
                ", underwater: " + std::to_string(rejectedUnderwater) +
                ", too steep: " + std::to_string(rejectedSteep) +
                ", too crowded: " + std::to_string(rejectedCrowded) + ")";
            return nullptr;
        }

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(placed.size());
        for (std::size_t index = 0; index < placed.size(); ++index)
        {
            // Round-robin rather than random when several kinds were named: twenty palms of
            // three types come out 7/7/6 every time instead of occasionally 12/5/3, and
            // "Ñ€Ð°Ð·Ð½Ð¾Ð³Ð¾ Ñ‚Ð¸Ð¿Ð°" means a mix, not a lottery. The positions are already random,
            // so nothing about the result looks regular.
            const EditorAssetRecord* kind = records[index % records.size()];
            nlohmann::json objectJson =
                factory->BuildDefaultJson(kind, ctx, actionCtx.assets, &placed[index]);

            // The factory already chose the asset's own spawn scale; the jitter MULTIPLIES it
            // rather than replacing it, so a mesh authored at 0.03 stays its own size.
            const float jitter = scaleDist(rng);
            const auto scaleIt = objectJson.find("scale");
            if (scaleIt != objectJson.end() && scaleIt->is_array() && scaleIt->size() == 3)
            {
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    if ((*scaleIt)[axis].is_number())
                    {
                        (*scaleIt)[axis] = (*scaleIt)[axis].get<float>() * jitter;
                    }
                }
            }
            objectJson["rotationDeg"] = nlohmann::json::array({ 0.0f, yawDist(rng), 0.0f });

            char nameBuffer[128];
            std::snprintf(nameBuffer, sizeof(nameBuffer), "%s %03zu",
                kind->displayName.c_str(), index + 1);
            objectJson["name"] = nameBuffer;

            commands.push_back(std::make_unique<SpawnMeshCommand>(std::move(objectJson)));
        }

        // Named by kind, with the split shown: "20 x coconut_palm, curly_palm, date_palm"
        // would hide whether the mix actually happened.
        std::string mix;
        for (std::size_t kind = 0; kind < records.size(); ++kind)
        {
            const std::size_t share = placed.size() / records.size() +
                (kind < placed.size() % records.size() ? 1u : 0u);
            if (share == 0)
            {
                continue;
            }
            if (!mix.empty())
            {
                mix += ", ";
            }
            mix += records[kind]->displayName + " x" + std::to_string(share);
        }
        outStatus = "Spawned " + std::to_string(placed.size()) + " (" + mix + ")";
        if (static_cast<int>(placed.size()) < requested)
        {
            // Saying "10" and placing 6 without a word is the kind of quiet shortfall that
            // gets noticed three edits later.
            outStatus += " (asked for " + std::to_string(requested) +
                "; the rest had no suitable ground " +
                (hasZone ? "inside zone " + zone.name
                         : "within " + std::to_string(static_cast<int>(radius)) + " m") + ")";
        }
        return FoldIntoOneEntry(std::move(commands),
            "Spawn " + std::to_string(placed.size()) + " x " +
            (records.size() == 1 ? record->displayName : "mixed assets"));
    }
}

// ----------------------------------------------------------------- the registry

EditorActionRegistry::EditorActionRegistry()
{
    // Descriptions are written for a reader who has never seen this editor -- that reader is
    // the model. "bury" without its sentence gets confused with hiding; "select" without its
    // sentence gets picked for "delete the palms" because selecting them is a step towards it.
    actions_.push_back({
        "select",
        "Select the matching objects and change nothing else. Use it when the user only wants to "
        "find or highlight objects, never as a step towards another action.",
        EditorActionEffect::SelectionOnly,
        EditorTargetKind::Objects,
        {},
        nullptr,
    });

    actions_.push_back({
        "count",
        "Answer how many objects match, and which assets they are, WITHOUT changing "
        "anything. Use it for questions -- 'how many palms are there', 'what is on the "
        "island', 'are there any rocks near the camera'. Never use it when the designer "
        "asked for something to happen.",
        EditorActionEffect::ReadOnly,
        EditorTargetKind::Objects,
        {},
        nullptr,
    });

    actions_.push_back({
        "setEnabled",
        "Show or hide the matching objects. Hidden objects stay in the level at their position and "
        "can be shown again; this does NOT move or remove anything.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        { { "enabled", EditorParamKind::Bool, true, "true shows the objects, false hides them" } },
        &BuildSetEnabled,
    });

    actions_.push_back({
        "delete",
        "Remove the matching objects from the level entirely.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {},
        &BuildDelete,
    });

    actions_.push_back({
        "duplicate",
        "Create a copy of each matching object in place; the copies become the new selection.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {},
        &BuildDuplicate,
    });

    actions_.push_back({
        "bury",
        "Move each matching object straight DOWN until the bottom of its geometry is under the "
        "surface below it (terrain, sand, rock). Fixes objects floating above uneven ground or "
        "standing on it with a visible gap. Only changes height, never the horizontal position.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        // THE CEILING IS PART OF THE DESCRIPTION, because the value is clamped to it. Said
        // as "default 1" with no upper bound, "Ð·Ð°Ñ€Ð¾Ð¹ Ð² Ð´Ð²Ð° Ñ€Ð°Ð·Ð° Ð³Ð»ÑƒÐ±Ð¶Ðµ" came back as 200
        // and silently became 25 -- a control that accepts a number and does something
        // else with it.
        { { "depthPercent", EditorParamKind::Number, false,
            "how far past contact it ends up, as a percent of the object's height "
            "(default 1, and anything over 25 is treated as 25 -- it is the footing that "
            "gets buried, not the whole mesh)" } },
        &BuildBury,
    });

    actions_.push_back({
        "spawn",
        "Create COUNT new copies, scattered over the ground around the point the camera is "
        "looking at (or around the selection). Each copy is dropped onto the surface, given "
        "a random yaw and a slightly random size, and kept clear of water, steep slopes and "
        "whatever is already standing there. This is the action for planting, scattering or "
        "adding new objects that are not in the level yet.\n"
        "For SEVERAL KINDS AT ONCE put them all in target.assets -- \"palms of different "
        "types\" is one spawn with three assets, not three spawns -- and the count is shared "
        "out evenly between them.\n"
        "The spacing is already EVEN rather than clumped: minSeparation is a hard floor "
        "between any two, so raising it spreads them out and lowering it lets them cluster. "
        "Cover a wide area by raising radius; there is no separate 'distribute evenly' mode "
        "to ask for, because that is what this already does.\n"
        "When the level has ZONES and the phrase says where -- \"on the beach\", \"in the "
        "north zone\" -- pass that zone's name as the `zone` parameter and leave radius "
        "alone: the zone decides the shape and the area, and nothing lands outside it. "
        "When the phrase says where but NO zone covers it -- \"by that rock\", \"around the "
        "island\" -- ask for the bounds and pass the point as `at`. Do not invent a zone "
        "name: only the zones listed in the prompt exist.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Asset,
        {
            { "count", EditorParamKind::Number, true, "how many to create (1-200)" },
            { "radius", EditorParamKind::Number, false, "radius of the scatter disc in metres (default 25)" },
            { "minSeparation", EditorParamKind::Number, false,
              "closest two of them may stand, in metres (default 6)" },
            { "alignToGround", EditorParamKind::Bool, false,
              "drop each onto the surface below (default true)" },
            { "minGroundNormalY", EditorParamKind::Number, false,
              "reject ground steeper than this, 1 = flat only (default 0.82)" },
            { "minHeight", EditorParamKind::Number, false,
              "reject ground below this world height (defaults to just above the waterline)" },
            { "yawRange", EditorParamKind::Range, false, "random yaw in degrees (default [0, 360])" },
            { "scaleRange", EditorParamKind::Range, false,
              "random size multiplier on the asset's own scale (default [0.9, 1.1])" },
            { "seed", EditorParamKind::Number, false, "fixes the layout; omit for a fresh one" },
            { "zone", EditorParamKind::String, false,
              "name of a zone to fill instead of a disc around the camera" },
            // The one that makes asking worth it. Without it a model that has just been
            // told where the island's north edge is has nowhere to PUT that answer: it can
            // scatter around the camera or inside a named zone, and neither is "there".
            // Asked to place a rock on the north shore of a level with no zones, it
            // invented a zone name -- not a guess about the world, a guess about the API.
            { "at", EditorParamKind::Vec3, false,
              "world point to scatter around, instead of the camera. Y is ignored when "
              "alignToGround is on, which it is by default" },
        },
        &BuildSpawn,
    });

    // Both of these were REFUSALS first. The editor could already do them -- RenameObject
    // and SetMaterial commands have existed all along, undoable and tested through the
    // Inspector -- but an action the registry does not name is an action the model cannot
    // reach, so it correctly answered needs_api and wrote a note. The gap was in the
    // vocabulary, not the engine. "rename" was asked three separate times.
    actions_.push_back({
        "rename",
        "Give the matching objects a new name. Renaming several numbers them -- \"spheres\" "
        "becomes \"spheres 001\", \"spheres 002\" -- because the outliner and every filter "
        "match on name, and two hundred objects sharing one is a level you cannot search. "
        "This changes the LABEL only; nothing moves and no mesh changes.\n"
        "FOR \"Ð½Ð°Ð·Ð¾Ð²Ð¸ Ð¾Ð±ÑŠÐµÐºÑ‚Ñ‹ Ð½Ð¾Ñ€Ð¼Ð°Ð»ÑŒÐ½Ñ‹Ð¼Ð¸ Ð¸Ð¼ÐµÐ½Ð°Ð¼Ð¸\" -- naming the level's things after WHAT "
        "THEY ARE rather than giving them all one name -- pass perAsset and no name at all, "
        "with scope \"all\" and no filter. Each object is then called after its own asset "
        "and numbered within its kind: coconut_palm 001..222, rock_boulder 001, tent 001. "
        "That is one command and one undo entry for the whole level.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "name", EditorParamKind::String, false,
              "the new name. Leave it out when using perAsset, or give it to prefix every "
              "generated name" },
            { "perAsset", EditorParamKind::Bool, false,
              "name each object after its own asset instead of giving them all one name" },
        },
        &BuildRename,
        false,             // filterFromSelection
        &RefineRenamePreview,
        false,             // takesDestinationAsset
        false,             // wholeLevelIsFine -- only with perAsset, below
        "perAsset",
    });

    actions_.push_back({
        "setMaterial",
        "Give the matching objects a different material -- the thing that decides how they "
        "look: colour, roughness, metalness. Use it for 'make the rocks look wet' or 'put "
        "the sand material on this'. Name a material that exists in the level; an object "
        "with several material slots takes an optional slot number, and without one the "
        "whole object changes.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "material", EditorParamKind::String, true, "material asset name" },
            { "slot", EditorParamKind::Number, false,
              "which material slot to change; omit to change the whole object" },
        },
        &BuildSetMaterial,
    });

    actions_.push_back({
        "replace",
        "Swap the mesh of each matching object for a different mesh asset, keeping its position, "
        "rotation and scale. Use it to change WHICH model existing objects use.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {},
        &BuildReplace,
        false,   // filterFromSelection
        nullptr, // refinePreview
        true,    // takesDestinationAsset -- the one verb with two nouns
    });

    actions_.push_back({
        "move",
        "Move the matching objects. With relative=true (the default) the value is metres to add to "
        "the current position; with relative=false it is the absolute world position to put them at.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "value", EditorParamKind::Vec3, true, "metres as [x, y, z]; a single number means all three" },
            { "relative", EditorParamKind::Bool, false, "add to the current position (default true)" },
        },
        &BuildMove,
    });

    actions_.push_back({
        "rotate",
        "Rotate the matching objects. The value is degrees as [pitch, yaw, roll]; yaw is the middle "
        "one and is the one that turns an upright object on the spot.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "value", EditorParamKind::Vec3, true, "degrees as [pitch, yaw, roll]" },
            { "relative", EditorParamKind::Bool, false, "add to the current rotation (default true)" },
        },
        &BuildRotate,
    });

    actions_.push_back({
        "scale",
        "Resize the matching objects. With relative=true (the default) the value MULTIPLIES the "
        "current scale, so 1.2 means 20% bigger; with relative=false it replaces it outright.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "value", EditorParamKind::Vec3, true, "multiplier as [x, y, z]; a single number scales uniformly" },
            { "relative", EditorParamKind::Bool, false, "multiply the current scale (default true)" },
        },
        &BuildScale,
    });

    actions_.push_back({
        "randomizeRotation",
        "Give each matching object its own random rotation, so a group of copies stops looking like "
        "copies. Yaw only by default, which is what upright things like trees and posts want.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "range", EditorParamKind::Range, false, "degrees to pick from (default [0, 360])" },
            { "axis", EditorParamKind::Enum, false, "yaw = turn on the spot, all = any orientation",
              { "yaw", "all" } },
            { "seed", EditorParamKind::Number, false, "fixes the result; omit for a fresh one" },
        },
        &BuildRandomizeRotation,
    });

    actions_.push_back({
        "isolate",
        "Show ONLY the matching objects: hide everything else in the level, and make sure "
        "the matching ones are visible. Use it for 'show me just the palms' or 'hide "
        "everything except the rocks'. Undo restores what was visible before.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {},
        &BuildIsolate,
    });

    actions_.push_back({
        "traceZone",
        "Draw a spline zone around the ABOVE-WATER part of whatever the target names -- the "
        "shoreline of an island, the dry part of a sandbank. The editor probes the ground on "
        "a grid, finds where it crosses the waterline, and makes a closed zone from that "
        "contour, filling INSIDE it. This is the action for \"Ð¾Ð±Ð²ÐµÐ´Ð¸ Ð¾ÑÑ‚Ñ€Ð¾Ð²\", \"ÑÐ´ÐµÐ»Ð°Ð¹ "
        "Ð·Ð¾Ð½Ñƒ Ð¿Ð¾ ÐºÐ¾Ð½Ñ‚ÑƒÑ€Ñƒ Ð±ÐµÑ€ÐµÐ³Ð°\", \"Ð·Ð¾Ð½Ð° Ð¿Ð¾ ÑƒÑ€ÐµÐ·Ñƒ Ð²Ð¾Ð´Ñ‹\". Afterwards the zone's name works "
        "like any other: spawn can fill it, and target.where.zone narrows to what is in it.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "margin", EditorParamKind::Number, false,
              "metres above the waterline to draw the line at (default 0); raise it to stay "
              "clear of the surf" },
            { "height", EditorParamKind::Number, false,
              "trace this world height instead of the waterline" },
            { "resolution", EditorParamKind::Number, false,
              "probe grid across the target, 24-160 (default 96). Finer follows the coast "
              "more closely and costs more" },
            { "points", EditorParamKind::Number, false,
              "how many control points to keep, 4-120 (default 32)" },
            { "name", EditorParamKind::String, false, "what to call the zone" },
        },
        &BuildTraceZone,
    });

    actions_.push_back({
        "setColor",
        "THE ACTION FOR \"Ð¿Ð¾ÐºÑ€Ð°ÑÑŒ\", \"Ð¿ÐµÑ€ÐµÐºÑ€Ð°ÑÑŒ\", \"ÑÐ´ÐµÐ»Ð°Ð¹ <Ñ†Ð²ÐµÑ‚>\", \"paint\", \"tint\". "
        "It sets the object's colour and leaves everything else alone -- the same tree, red. "
        "Use it whenever a phrase names a colour for objects that already exist; it is not a "
        "near-miss for those phrases, it is the answer to them, so do NOT reach for "
        "needs_api. (It is not replace, which swaps the mesh, and not setMaterial, which "
        "swaps the whole material.)\n"
        "Colour as [r, g, b], each 0..1, and a plain colour word is a plain value: red "
        "[1, 0, 0], bright red [1, 0, 0], dark red [0.4, 0, 0], green [0, 1, 0], blue "
        "[0, 0, 1], warm sand [0.9, 0.8, 0.6]. White [1, 1, 1] puts them back to normal. "
        "A textured object keeps its texture and takes the colour over it, which is what "
        "painting something means -- that is not a limitation to warn about.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "color", EditorParamKind::Vec3, true, "[r, g, b], each 0..1" },
        },
        &BuildSetColor,
    });

    // WHAT THE PREVIEW SAYS WHEN THE ANSWER IS "into what". "group 621 objects" is true
    // and useless: it names the verb and the count and leaves out the only thing being
    // decided -- how many folders, called what. The person is looking at that line to
    // choose whether to press Run.
    //
    // The breakdown under it is per-KIND, and with perAsset each kind IS a group, so the
    // sentence only has to say so. Without perAsset they all go into one named folder,
    // which is a different sentence and just as short.
    actions_.push_back({
        "group",
        "Put the matching objects into a named GROUP -- a label they carry, shown as a "
        "folder in the outliner. Once grouped, the group's name works as a filter "
        "everywhere: \"ÑÐ¿Ñ€ÑÑ‡ÑŒ ÑÐµÐ²ÐµÑ€Ð½ÑƒÑŽ Ñ€Ð¾Ñ‰Ñƒ\" is setEnabled with that name in the filter, "
        "\"ÑƒÐ´Ð°Ð»Ð¸ ÐµÑ‘\" is a delete. Use it for \"ÑÐ³Ñ€ÑƒÐ¿Ð¿Ð¸Ñ€ÑƒÐ¹\", \"ÑÐ¾Ð±ÐµÑ€Ð¸ Ð² Ð³Ñ€ÑƒÐ¿Ð¿Ñƒ\", "
        "\"Ð½Ð°Ð·Ð¾Ð²Ð¸ ÑÑ‚Ð¾\". An EMPTY name takes them out of whatever group they were in.\n"
        "FOR \"a group for each kind\" pass perAsset instead of naming one, and do it in a "
        "SINGLE command: perAsset splits whatever it is given, and the whole thing is one "
        "undo entry.\n"
        "TO TIDY THE WHOLE OUTLINER -- \"Ð½Ð°Ð²ÐµÐ´Ð¸ Ð¿Ð¾Ñ€ÑÐ´Ð¾Ðº\", \"Ñ€Ð°Ð·Ð»Ð¾Ð¶Ð¸ Ð¿Ð¾ Ð¿Ð°Ð¿ÐºÐ°Ð¼\", "
        "\"ÑÐ³Ñ€ÑƒÐ¿Ð¿Ð¸Ñ€ÑƒÐ¹ Ð¾Ð±ÑŠÐµÐºÑ‚Ñ‹ ÐºÐ°Ðº Ð½Ð°Ð´Ð¾\" -- send perAsset with scope \"all\" and NO filter "
        "at all. That is the one case where an empty filter is right, and it is the only "
        "way to catch every kind: the level summary lists the commonest assets and says "
        "`otherAssetKinds` for the rest, so a filter you type out by hand will silently "
        "miss the ones it never showed you -- the island, the rocks, the tents.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "name", EditorParamKind::String, false,
              "the group's name; empty removes them from their group" },
            { "perAsset", EditorParamKind::Bool, false,
              "one group per KIND instead of one group for all of them, each named after "
              "its asset. This is what \"Ð³Ñ€ÑƒÐ¿Ð¿Ñ‹ Ð´Ð»Ñ ÐºÐ°Ð¶Ð´Ð¾Ð³Ð¾ Ñ‚Ð¸Ð¿Ð°\" means -- use it instead "
              "of sending one command per kind" },
        },
        &BuildGroup,
        false,             // filterFromSelection
        &RefineGroupPreview,
        false,             // takesDestinationAsset
        true,              // wholeLevelIsFine -- "Ð½Ð°Ð²ÐµÐ´Ð¸ Ð¿Ð¾Ñ€ÑÐ´Ð¾Ðº Ð² Ð°ÑƒÑ‚Ð»Ð°Ð¹Ð½ÐµÑ€Ðµ" IS the level
    });

    actions_.push_back({
        "thin",
        "Thin OUT objects that are already in the level, by deleting some of them. Two ways "
        "to say how much: minSeparation removes whatever stands closer together than that "
        "many metres, keepPercent keeps roughly that share and drops the rest evenly. This "
        "is the answer to \"Ð¿Ñ€Ð¾Ñ€ÐµÐ´ÑŒ\", \"ÑÐ»Ð¸ÑˆÐºÐ¾Ð¼ Ð³ÑƒÑÑ‚Ð¾\", \"ÑƒÐ±ÐµÑ€Ð¸ Ð¿Ð¾Ð»Ð¾Ð²Ð¸Ð½Ñƒ\" -- it is the "
        "undo spawn does not have, and it keeps the arrangement rather than replacing it.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "minSeparation", EditorParamKind::Number, false,
              "closest two may stand, in metres; anything nearer loses the newer one" },
            { "keepPercent", EditorParamKind::Number, false,
              "keep about this share, 50 = half. Use one of these two, not both" },
        },
        &BuildThin,
        false,
        &RefineThinPreview,
    });

    actions_.push_back({
        "createZone",
        "Draw a named region on the ground -- a circle or a rectangle -- around the point "
        "the camera is looking at, or around an explicit `at`. A zone is how the editor says "
        "WHERE: once one exists, spawn can fill it with params.zone, and every other action "
        "can narrow to what is inside it with target.where.zone. Make one when the designer "
        "names a place the level does not have yet (\"Ð·Ð°Ð²ÐµÐ´Ð¸ Ð·Ð¾Ð½Ñƒ Ð½Ð° Ð¿Ð»ÑÐ¶Ðµ\", \"ÑÐ´ÐµÐ»Ð°Ð¹ "
        "Ð¾Ð±Ð»Ð°ÑÑ‚ÑŒ Ð²Ð¾ÐºÑ€ÑƒÐ³ Ñ‚Ð¾Ð³Ð¾ ÐºÐ°Ð¼Ð½Ñ\"), and when a later command will need to refer to it.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::None,
        {
            { "shape", EditorParamKind::Enum, false,
              "circle (default) or rect", { "circle", "rect" } },
            { "size", EditorParamKind::Number, false,
              "radius for a circle, half-extent for a rect, in metres (default 25)" },
            { "name", EditorParamKind::String, false,
              "what to call it; numbered automatically when omitted" },
            { "at", EditorParamKind::Vec3, false,
              "world centre. Omit to use what the camera is looking at" },
        },
        &BuildCreateZone,
    });

    actions_.push_back({
        "frame",
        "Point the camera at the matching objects so they all fit on screen, without "
        "changing anything in the level. Use it for 'show me', 'take me to', 'where are'. "
        "It only moves the camera; it does not select, hide or alter anything.",
        EditorActionEffect::ViewChange,
        EditorTargetKind::Objects,
        {},
        nullptr,
    });

    actions_.push_back({
        "selectSimilar",
        "Select everything that uses the same asset as what is selected right now. The "
        "designer clicks one palm and asks for 'all the ones like this'; you do NOT need to "
        "know which one that is -- leave the filter empty and the editor works it out.",
        EditorActionEffect::SelectionOnly,
        EditorTargetKind::Objects,
        {},
        nullptr,
        true,
    });

    actions_.push_back({
        "align",
        "Line the matching objects up: give them all the same position on ONE axis, leaving "
        "the other two alone. Use it for 'line these up', 'put them at the same height', "
        "'align them on X'.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "axis", EditorParamKind::Enum, true,
              "which coordinate they should share; y is height", { "x", "y", "z" } },
            { "to", EditorParamKind::Enum, false,
              "which value to use (default average)",
              { "min", "max", "center", "average", "first" } },
        },
        &BuildAlign,
    });

    actions_.push_back({
        "distribute",
        "Space the matching objects evenly along ONE axis. Without a spacing the two "
        "outermost stay where they are and everything between them is evened out; with a "
        "spacing they are walked out from the first one at that interval.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "axis", EditorParamKind::Enum, true, "the axis to space them along",
              { "x", "y", "z" } },
            { "spacing", EditorParamKind::Number, false,
              "metres between neighbours; omit to even out what is already there" },
        },
        &BuildDistribute,
    });

    actions_.push_back({
        "snap",
        "Round the matching objects' positions onto a grid. By default only the horizontal "
        "plane is snapped, because rounding height on uneven ground lifts things off it; "
        "ask for axes 'all' if that is really wanted. Can also round the yaw to a step.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "grid", EditorParamKind::Number, false, "grid size in metres (default 1)" },
            { "axes", EditorParamKind::Enum, false,
              "which axes to round (default xz -- the ground plane)",
              { "xz", "all", "x", "y", "z" } },
            { "rotationStep", EditorParamKind::Number, false,
              "round the yaw to this many degrees too; 0 leaves rotation alone" },
        },
        &BuildSnap,
    });

    actions_.push_back({
        "setEnvironment",
        "Change one setting of the world's look: fog, sun, wind, water, exposure or colour "
        "grading. target.setting names it, e.g. \"wind.strength\" or \"gtao.intensity\". Give "
        "params.value for an absolute new value, or params.scale to multiply what it is now "
        "(scale 1.5 = half again as much, 0.5 = half). This is the action for 'thicker fog', "
        "'stronger wind', 'brighter sun' -- anything about how the level LOOKS rather than "
        "about particular objects.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Environment,
        {
            { "value", EditorParamKind::Any, false,
              "the new value; must match the setting's own type" },
            { "scale", EditorParamKind::Number, false,
              "multiply the current value instead (numbers only)" },
        },
        &BuildSetEnvironment,
    });

    actions_.push_back({
        "randomizeScale",
        "Vary the size of each matching object by a random multiplier on its current scale, so a "
        "group of copies reads as individuals rather than as one model repeated.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "range", EditorParamKind::Range, false, "multiplier to pick from (default [0.9, 1.1])" },
            { "uniform", EditorParamKind::Bool, false,
              "same factor on all three axes (default true); false stretches" },
            { "seed", EditorParamKind::Number, false, "fixes the result; omit for a fresh one" },
        },
        &BuildRandomizeScale,
    });
}

const EditorActionRegistry& EditorActionRegistry::Builtin()
{
    static const EditorActionRegistry registry;
    return registry;
}

const EditorActionDesc* EditorActionRegistry::Find(std::string_view id) const
{
    for (const EditorActionDesc& action : actions_)
    {
        if (action.id == id)
        {
            return &action;
        }
    }
    return nullptr;
}

bool ValidateParams(const EditorActionDesc& action, nlohmann::json& params, std::string& outError)
{
    if (!params.is_object())
    {
        outError = "parameters must be an object";
        return false;
    }

    // A PARAMETER THE ACTION DOES NOT DECLARE IS A REFUSAL, NOT A SHRUG. This loop used to
    // be absent, and an unknown key was dropped without a word -- which is worse than a bad
    // value, because the command then runs and does something plausible.
    //
    // The prompt makes the collision likely on purpose: `spawn` says WHERE with params.zone
    // while every other action says it with target.where.zone. "ÑƒÐ´Ð°Ð»Ð¸ Ð¿Ð°Ð»ÑŒÐ¼Ñ‹ Ð² Ð·Ð¾Ð½Ðµ Beach"
    // answered with params.zone lost the zone silently, and the preview said "delete 183
    // objects" -- honest, plausible, and the whole level's palms instead of the twelve on
    // the beach. Naming the permitted keys is what turns that into a fixable message.
    for (const auto& entry : params.items())
    {
        const auto known = std::find_if(action.params.begin(), action.params.end(),
            [&entry](const EditorActionParam& param) { return param.name == entry.key(); });
        if (known != action.params.end())
        {
            continue;
        }
        outError = "'" + std::string(action.id) + "' has no parameter '" + entry.key() + "'";
        if (action.params.empty())
        {
            outError += "; it takes none";
        }
        else
        {
            outError += "; it takes ";
            for (std::size_t i = 0; i < action.params.size(); ++i)
            {
                outError += (i ? ", " : "") + std::string(action.params[i].name);
            }
        }
        return false;
    }

    for (const EditorActionParam& param : action.params)
    {
        const std::string name(param.name);
        auto it = params.find(name);
        if (it == params.end())
        {
            if (param.required)
            {
                outError = "'" + std::string(action.id) + "' needs a '" + name + "'";
                return false;
            }
            continue;
        }

        switch (param.kind)
        {
        case EditorParamKind::Number:
            if (!it->is_number())
            {
                outError = "'" + name + "' must be a number";
                return false;
            }
            break;
        case EditorParamKind::Bool:
            if (!it->is_boolean())
            {
                outError = "'" + name + "' must be true or false";
                return false;
            }
            break;
        case EditorParamKind::String:
            if (!it->is_string())
            {
                outError = "'" + name + "' must be text";
                return false;
            }
            break;
        case EditorParamKind::Range:
            // A lone number is a legal way to say "exactly this", so broaden it here rather
            // than making every source remember to write [n, n].
            if (it->is_number())
            {
                const float value = it->get<float>();
                *it = nlohmann::json::array({ value, value });
            }
            if (!it->is_array() || it->size() != 2 || !(*it)[0].is_number() || !(*it)[1].is_number())
            {
                outError = "'" + name + "' must be two numbers, [low, high]";
                return false;
            }
            break;
        case EditorParamKind::Vec3:
            // Likewise: "scale by 2" is a vec3 of twos, and refusing it would be pedantry
            // that the model would then have to be told about in the prompt.
            if (it->is_number())
            {
                const float value = it->get<float>();
                *it = nlohmann::json::array({ value, value, value });
            }
            if (!it->is_array() || it->size() != 3 ||
                !(*it)[0].is_number() || !(*it)[1].is_number() || !(*it)[2].is_number())
            {
                outError = "'" + name + "' must be three numbers, [x, y, z]";
                return false;
            }
            break;
        case EditorParamKind::Any:
            break;   // checked later, by something that knows the real type
        case EditorParamKind::Enum:
        {
            if (!it->is_string())
            {
                outError = "'" + name + "' must be text";
                return false;
            }
            const std::string value = it->get<std::string>();
            bool known = false;
            std::string permitted;
            for (const std::string_view candidate : param.values)
            {
                if (!permitted.empty())
                {
                    permitted += ", ";
                }
                permitted += std::string(candidate);
                known = known || value == candidate;
            }
            if (!known)
            {
                outError = "'" + name + "' must be one of: " + permitted;
                return false;
            }
            break;
        }
        }
    }

    return true;
}

EditorIntent MakeActionIntent(std::string_view actionId, nlohmann::json params)
{
    EditorIntent intent;
    intent.kind = EditorIntentKind::Command;
    intent.action = std::string(actionId);
    intent.params = std::move(params);
    intent.sourceLabel = "editor";
    return intent;
}

bool RunEditorAction(const EditorActionContext& actionCtx,
    EditorCommandStack& commandStack,
    const EditorIntent& intent,
    const std::vector<EditorObjectId>& targets,
    std::string& outStatus)
{
    const EditorActionDesc* action = EditorActionRegistry::Builtin().Find(intent.action);
    if (!action)
    {
        outStatus = "Unknown editor action '" + intent.action + "'";
        return false;
    }

    if (action->effect == EditorActionEffect::ReadOnly)
    {
        // Nothing to do: the answer was computed by the resolver and is already on screen.
        // Reaching here at all means something asked to "run" a question.
        outStatus = "Nothing to run -- that was a question, and it is already answered";
        return false;
    }

    if (action->effect == EditorActionEffect::ViewChange)
    {
        if (targets.empty())
        {
            outStatus = "Nothing to look at";
            return false;
        }
        // Framing wants an EditorSelection, but it must NOT disturb the real one: "show me
        // the palms" is a request to look, not to select.
        EditorSelection framed;
        for (const EditorObjectId id : targets)
        {
            framed.Add(id, false);
        }
        if (!editorframing::FrameObjects(actionCtx.editor.renderer, actionCtx.editor.scene,
                actionCtx.editor.document, framed))
        {
            outStatus = "Could not work out where those are";
            return false;
        }
        outStatus = "Framed " + CountedObjects(targets.size());
        return true;
    }

    if (action->effect == EditorActionEffect::SelectionOnly)
    {
        if (targets.empty())
        {
            outStatus = "Nothing matched";
            return false;
        }
        actionCtx.editor.selection.SetOrdered(targets, targets.front());
        outStatus = "Selected " + CountedObjects(targets.size());
        return true;
    }

    std::string buildStatus;
    std::unique_ptr<EditorCommand> command = action->build(actionCtx, targets, intent, buildStatus);
    if (!command)
    {
        outStatus = buildStatus;
        return false;
    }

    if (!commandStack.Execute(actionCtx.editor, std::move(command)))
    {
        outStatus = buildStatus.empty() ? "Action failed" : buildStatus + " -- failed";
        return false;
    }
    outStatus = buildStatus;
    return true;
}

#endif // WITH_EDITOR
