#include "editor/intent/EditorActionBuilders.h"
#if WITH_EDITOR

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "core/logging/Log.h"
#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "editor/EditorFraming.h"
#include "editor/intent/EditorIntentResolver.h"
#include "editor/intent/EditorSceneQuery.h"
#include "editor/commands/CompositeCommand.h"
#include "editor/commands/CreateDocumentObjectCommand.h"
#include "editor/commands/DeleteObjectCommand.h"
#include "editor/scene/EditorZone.h"
#include "ocean/OceanRenderable.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/renderables/RenderableObject.h"
#include "editor/EditorObjectMatch.h"

// R10.3: the editor's actions -- zones: tracing a waterline, creating a zone, thinning what is inside one.
// Cut out of EditorActionRegistry.cpp; the table that names them is still there (EditorActionBuilders.h).

namespace editoractions
{
    // ---------------------------------------------------------------- traceZone

    // DRAW THE WATERLINE. Asked for in the refusal log, in the designer's own words --
    // "создай сплайн зону по контуру острова над водой" -- and the model's note named the
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

        // NAMING A ZONE THAT ALREADY EXISTS MEANS THAT ZONE. "можешь в этой зоне больше
        // точек сделать?" used to leave a SECOND contour lying on top of the first, because
        // the only thing this action could do with a name was hang it on something new --
        // and re-tracing is the obvious follow-up to tracing, which is how it came up.
        std::string name = StringOr(intent.params, "name", "");
        EditorObjectId replacing{ 0 };
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
        else
        {
            for (const EditorObject& object : ctx.document.Objects())
            {
                if (object.type == editorzone::kTypeName && object.name == name)
                {
                    replacing = object.id;
                    break;
                }
            }
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

        outStatus = (replacing.value != 0 ? "Re-traced '" : "Traced '") + name + "' with " +
            std::to_string(simplified.size()) + " points from " + std::to_string(resolution) +
            "x" + std::to_string(resolution) + " probes";
        if (componentSize.size() > 1)
        {
            outStatus += " (largest of " + std::to_string(componentSize.size()) +
                " separate pieces)";
        }

        auto create = std::make_unique<CreateDocumentObjectCommand>(std::move(zone));
        if (replacing.value == 0)
        {
            return create;
        }
        // One history entry, so undoing a re-trace brings the old contour back rather than
        // leaving the level with no zone of that name at all.
        auto replace = std::make_unique<CompositeCommand>("Re-trace zone");
        replace->Add(std::make_unique<DeleteObjectCommand>(replacing));
        replace->Add(std::move(create));
        return replace;
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
}

#endif // WITH_EDITOR
