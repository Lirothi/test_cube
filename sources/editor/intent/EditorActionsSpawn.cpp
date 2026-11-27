#include "editor/intent/EditorActionBuilders.h"
#if WITH_EDITOR

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "app/game/RaceCourse.h" // game::TrackDisplayName (came through Scene.h before R6.2b)
#include "rendering/view/Camera.h"
#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "editor/EditorObjectMatch.h"
#include "editor/intent/EditorIntentResolver.h"
#include "editor/EditorExtensionRegistry.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/scene/EditorZone.h"
#include "editor/commands/SpawnMeshCommand.h"
#include "ocean/OceanRenderable.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/renderables/RenderableObject.h"
#include "editor/assets/MeshRestPose.h"

// R10.3: the editor's actions -- making things: spawn (scatter), place, and the scatter's anchor.
// Cut out of EditorActionRegistry.cpp; the table that names them is still there (EditorActionBuilders.h).

namespace editoractions
{
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

    // A NEW OBJECT JOINS THE GROUP ITS KIND ALREADY USES, and gets one when its kind has
    // none. Tidying the outliner and then spawning used to undo the tidying one command
    // later: the new rocks landed in the ungrouped "Meshes" bucket while three rock
    // folders sat above them.
    //
    // An existing example of the kind has the last word on the name, because somebody may
    // have renamed the folder; only when no example carries a group is the name derived,
    // and then by the SAME spelling `group perAsset` uses, or the two would disagree.
    // Shared by spawn and place: two ways of creating a thing must not file it differently.
    std::string GroupForKind(const EditorSceneDocument& document, const EditorAssetRecord* kind)
    {
        if (!kind || kind->id.key.empty())
        {
            return {};
        }
        for (const EditorObject& object : document.Objects())
        {
            if (!editormatch::MatchesSearch(object, kind->id.key))
            {
                continue;
            }
            const auto it = object.properties.is_object()
                ? object.properties.find("group") : object.properties.end();
            if (it != object.properties.end() && it->is_string() &&
                !it->get<std::string>().empty())
            {
                return it->get<std::string>();
            }
        }
        return editormatch::PrettyLabelFromPath(kind->id.key);
    }

    // For mesh records the registry sets id.key to the path, but not for every asset type,
    // so both spellings are tried rather than depending on that.
    const EditorAssetRecord* FindMeshAsset(const AssetRegistry& assets, const std::string& name)
    {
        const EditorAssetRecord* record = assets.FindById({ EditorAssetType::Mesh, name });
        return record ? record : assets.FindByPath(name);
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
            const EditorAssetRecord* record = FindMeshAsset(actionCtx.assets, name);
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

        // The number ASKED FOR is kept beside the number allowed, because the cap is silent
        // otherwise: told "раскидай 300 камней", the editor answered "asked for 200" and the
        // 300 vanished without anyone saying it had been refused.
        const int wanted = static_cast<int>(NumberOr(intent.params, "count", 1.0f));
        const int requested = std::clamp(wanted, 1, 200);
        const float radius = std::max(0.5f, NumberOr(intent.params, "radius", 25.0f));
        // 1.5, NOT 6, AND THE CHANGE IS A CORRECTION. This number used to be the distance
        // between CENTRES, and 6 was chosen to stand in for the sizes nobody measured. Then
        // the code learned each object's true radius and started adding it to this -- without
        // anyone lowering the number whose whole job had just been taken over. A rock then
        // had to keep six metres of clear air from the EDGE of every palm.
        //
        // Measured on wind_test, which is where it showed: 610 palms, median nearest
        // neighbour 2.5 m, and a palm's bounds are its CANOPY (~4 m). Clearance came to
        // ~11.5 m, which leaves 572 m2 of the island's ~15,000 usable -- and the scatter
        // duly placed 14 of 200 and reported the island full. At 1.5 the same island offers
        // ~2,500 m2 and fits of the order of 150.
        const float minSeparation = std::max(0.0f, NumberOr(intent.params, "minSeparation", 1.5f));
        const bool alignToGround = BoolOr(intent.params, "alignToGround", true);
        const float minNormalY = std::clamp(NumberOr(intent.params, "minGroundNormalY", 0.82f), 0.0f, 1.0f);
        float yawLo = 0.0f;
        float yawHi = 360.0f;
        RangeOr(intent.params, "yawRange", yawLo, yawHi);
        float scaleLo = 0.9f;
        float scaleHi = 1.1f;
        RangeOr(intent.params, "scaleRange", scaleLo, scaleHi);
        // "РАНДОМНЫМ СКЕЙЛОМ -0.5+0.1" IS A DELTA, NOT A RANGE. People write the spread of a
        // size the way they write the spread of an angle, as a signed pair around what is
        // already there. Read literally it asks for a scale of minus one half, which mirrors
        // the mesh through the origin -- so a negative low end cannot mean what it says, and
        // the only reading left is the one that was meant: multiply by 1 + each end.
        const bool scaleWasADelta = scaleLo < 0.0f;
        if (scaleWasADelta)
        {
            scaleLo += 1.0f;
            scaleHi += 1.0f;
        }
        // Whatever arrives, a size multiplier is positive. Zero would place an invisible
        // object and negative turns it inside out; neither is ever what was asked for.
        scaleLo = std::clamp(scaleLo, 0.01f, 100.0f);
        scaleHi = std::clamp(scaleHi, scaleLo, 100.0f);

        // A named zone replaces the disc entirely: its own centre, its own shape, its own
        // size. This is the answer to the phrases that had nowhere to say WHERE -- "only on
        // the beach, not in the water" was refused outright, and "along the shore" got a
        // 15 m circle, which is worse than a refusal because it looks like it worked.
        // "В ВЫДЕЛЕННОЙ ЗОНЕ" ARRIVES SOMEWHERE ELSE. A zone the user selected is turned into
        // `target.where.zone` by the resolver, once, for every verb -- that is what makes
        // "удали пальмы в выделенной зоне" work. Spawn had its own `zone` parameter and read
        // only that, so the one phrase that names the place without knowing its name
        // scattered around the camera instead, which looks like it worked.
        std::string zoneName = StringOr(intent.params, "zone", "");
        if (zoneName.empty())
        {
            zoneName = intent.target.where.zone;
        }
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
        //
        // KNOWN AND NOT FIXED HERE: for a tree the world bounds are the CANOPY, and a rock
        // may sit under fronds perfectly happily -- the thing it must not share ground with
        // is the trunk. Measured on wind_test a palm reads as ~4 m of radius where its
        // footprint is well under one, so every palm reserves fifty times the ground it
        // stands on. Fixing that needs a footprint per asset, recorded at import, which is
        // the same missing number as the one two paragraphs up.
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
        // Its own bucket. A point that fell in the zone's bounding box but outside the
        // spline was being counted as CROWDED, which is a different thing entirely and the
        // one number a reader would act on.
        std::size_t rejectedOutsideZone = 0;

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
                    ++rejectedOutsideZone;
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

        // WHY IT WENT NOWHERE, in the counts it already keeps. These existed but were printed
        // only when NOTHING was placed; a partial result got the sentence "the rest had no
        // suitable ground", which names one cause out of four and was usually the wrong one.
        // On an island already carrying six hundred palms, "crowded" is the whole story, and
        // "no suitable ground" sends the reader to look at the terrain.
        const auto whyNot = [&]()
        {
            std::string why = "no ground: " + std::to_string(rejectedNoGround) +
                ", underwater: " + std::to_string(rejectedUnderwater) +
                ", too steep: " + std::to_string(rejectedSteep) +
                ", too crowded: " + std::to_string(rejectedCrowded);
            if (hasZone)
            {
                why += ", outside the zone: " + std::to_string(rejectedOutsideZone);
            }
            // The one number a person can act on, named as a knob rather than a diagnosis.
            if (rejectedCrowded > rejectedNoGround + rejectedUnderwater + rejectedSteep)
            {
                why += " -- lower minSeparation to fit more in";
            }
            return why;
        };

        if (placed.empty())
        {
            outStatus = "Found nowhere to place " + record->displayName + " (" + whyNot() + ")";
            return nullptr;
        }

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(placed.size());
        std::map<std::string, std::size_t> intoGroups;
        NameCounter names(ctx.document);
        for (std::size_t index = 0; index < placed.size(); ++index)
        {
            // Round-robin rather than random when several kinds were named: twenty palms of
            // three types come out 7/7/6 every time instead of occasionally 12/5/3, and
            // "разного типа" means a mix, not a lottery. The positions are already random,
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
            // An asset with a rest pose lies in it, turned by the yaw, its lowest point on the
            // ground -- a scatter of shells, not a field of shells standing on their hinges.
            const float yaw = yawDist(rng);
            if (!restpose::ApplyToNewObject(objectJson, yaw, placed[index].y))
            {
                objectJson["rotationDeg"] = nlohmann::json::array({ 0.0f, yaw, 0.0f });
            }

            objectJson["name"] = names.Next(kind->displayName);

            const std::string group = GroupForKind(ctx.document, kind);
            if (!group.empty())
            {
                objectJson["group"] = group;
                ++intoGroups[group];
            }

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
        if (scaleWasADelta)
        {
            // WHEN THE EDITOR REINTERPRETS THE INPUT IT SAYS SO. A signed spread was read as
            // a delta around 1 rather than as the literal range it cannot have been; the
            // person who wrote it gets to see what that turned into.
            char span[64];
            std::snprintf(span, sizeof(span), " (scale read as %.2f-%.2f)", scaleLo, scaleHi);
            outStatus += span;
        }
        if (!intoGroups.empty())
        {
            outStatus += " into ";
            std::size_t shownGroups = 0;
            for (const auto& entry : intoGroups)
            {
                outStatus += (shownGroups++ ? ", " : "") + entry.first;
            }
        }
        if (static_cast<int>(placed.size()) < requested)
        {
            // Saying "10" and placing 6 without a word is the kind of quiet shortfall that
            // gets noticed three edits later -- and saying WHY is what turns it from a
            // complaint into something the next phrase can fix.
            outStatus += " (asked for " + std::to_string(requested) +
                (wanted > requested ? " -- 200 is the cap, you asked for " +
                    std::to_string(wanted) : std::string()) + " " +
                (hasZone ? "inside zone " + zone.name
                         : "within " + std::to_string(static_cast<int>(radius)) + " m") +
                "; " + whyNot() + ")";
        }
        return FoldIntoOneEntry(std::move(commands),
            "Spawn " + std::to_string(placed.size()) + " x " +
            (records.size() == 1 ? record->displayName : "mixed assets"));
    }

    // PLACE: objects at the positions the caller worked out, as given. spawn cannot do this
    // and should not learn to: its whole job is deciding WHERE -- probing the ground,
    // rejecting water and slopes, keeping things apart by their bounds. That is the wrong
    // job for a layout computed elsewhere, and it failed it twice on wind_test: the ground
    // probe starts 500 m up and lands on palm CROWNS, and the separation rule counts a palm
    // as its ~4 m canopy, so no palm can stand closer than ~8 m to another where the real
    // belt is spaced at 2.5. A scripted infill of the palm belt had no honest way in.
    //
    // Nothing here is probed, jittered or kept apart. The one thing it shares with spawn is
    // what a new object IS: the factory's default for the asset (its own spawn scale, which
    // `scale` multiplies), a readable name, and the group its kind already uses.
    std::unique_ptr<EditorCommand> BuildPlace(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>&,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        const IEditorObjectFactory* factory = actionCtx.extensions.FindObjectFactory("staticMesh");
        if (!factory)
        {
            outStatus = "No static-mesh factory is registered";
            return nullptr;
        }
        const auto itemsIt = intent.params.find("items");
        if (itemsIt == intent.params.end() || !itemsIt->is_array() || itemsIt->empty())
        {
            outStatus = "place needs a list of items";
            return nullptr;
        }
        const std::string groupOverride = StringOr(intent.params, "group", "");

        // Every asset resolved UP FRONT, as spawn does: a list with one misspelt kind says
        // which item, rather than placing the rest and reporting success.
        std::map<std::string, const EditorAssetRecord*> records;
        for (std::size_t index = 0; index < itemsIt->size(); ++index)
        {
            const std::string name = (*itemsIt)[index].value("asset", std::string());
            if (records.count(name))
            {
                continue;
            }
            const EditorAssetRecord* record = FindMeshAsset(actionCtx.assets, name);
            if (!record)
            {
                outStatus = "Item " + std::to_string(index + 1) + ": unknown asset '" + name + "'";
                return nullptr;
            }
            if (!factory->CanBuildFromAsset(record))
            {
                outStatus = "Item " + std::to_string(index + 1) + ": '" + record->displayName +
                    "' cannot be placed as a static mesh";
                return nullptr;
            }
            records[name] = record;
        }

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(itemsIt->size());
        std::map<std::string, std::size_t> perKind;
        std::map<std::string, std::size_t> intoGroups;
        NameCounter names(ctx.document);
        for (std::size_t index = 0; index < itemsIt->size(); ++index)
        {
            const nlohmann::json& item = (*itemsIt)[index];
            const EditorAssetRecord* kind = records[item.value("asset", std::string())];
            const nlohmann::json& p = item["position"];
            const Math::float3 position(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
            nlohmann::json objectJson = factory->BuildDefaultJson(kind, ctx, actionCtx.assets, &position);
            // The factory may have its own opinion of where a new object goes; the caller's
            // point is the whole reason this action exists, so it is written last.
            objectJson["position"] = nlohmann::json::array({ position.x, position.y, position.z });

            // A number multiplies all three axes, three numbers multiply each -- both ON TOP
            // of the asset's own spawn scale, so a mesh authored at 0.03 stays its own size.
            const auto scaleIt = objectJson.find("scale");
            const auto askedScale = item.find("scale");
            if (askedScale != item.end() && scaleIt != objectJson.end() && scaleIt->is_array() &&
                scaleIt->size() == 3)
            {
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    const float factor = askedScale->is_number()
                        ? askedScale->get<float>() : (*askedScale)[axis].get<float>();
                    (*scaleIt)[axis] = (*scaleIt)[axis].get<float>() * factor;
                }
            }
            // rotationDeg is the whole orientation; yawDeg is the common case of an upright
            // thing turned on the spot. Given both, the full one wins -- it says more, and it is
            // taken as given together with the position. Without it, an asset with a rest pose
            // lies in it (yawDeg turning it about the vertical) with its lowest point AT the
            // given height.
            const auto rotationIt = item.find("rotationDeg");
            if (rotationIt != item.end())
            {
                objectJson["rotationDeg"] = *rotationIt;
            }
            else
            {
                const float yaw = item.value("yawDeg", 0.0f);
                if (!restpose::ApplyToNewObject(objectJson, yaw, position.y))
                {
                    objectJson["rotationDeg"] = nlohmann::json::array({ 0.0f, yaw, 0.0f });
                }
            }

            ++perKind[kind->displayName];
            objectJson["name"] = names.Next(kind->displayName);
            // The item's own place in the physics world (setPhysics' keys): "dynamic" falls when the simulation runs.
            if (const auto motion = item.find("physics"); motion != item.end()) { objectJson["physics"] = *motion; }
            if (const auto mass = item.find("mass"); mass != item.end()) { objectJson["mass"] = *mass; }
            if (const auto floats = item.find("buoyant"); floats != item.end()) { objectJson["buoyant"] = *floats; }
            if (const auto collision = item.find("collision"); collision != item.end()) { objectJson["collision"] = *collision; }
            // A mark of the race (app/game/RaceCourse.h): two items with one number are a gate.
            if (const auto gate = item.find("raceGate"); gate != item.end() && gate->is_number() && gate->get<float>() >= 1.0f)
            {
                objectJson["raceGate"] = static_cast<int>(std::lround(gate->get<float>()));
                // And whose course it is: absent or "" = the level's first.
                if (const auto track = item.find("raceTrack"); track != item.end() && track->is_string() && !track->get<std::string>().empty())
                {
                    objectJson["raceTrack"] = *track;
                }
            }

            const std::string group = groupOverride.empty() ? GroupForKind(ctx.document, kind) : groupOverride;
            if (!group.empty())
            {
                objectJson["group"] = group;
                ++intoGroups[group];
            }
            commands.push_back(std::make_unique<SpawnMeshCommand>(std::move(objectJson)));
        }

        std::string mix;
        for (const auto& entry : perKind)
        {
            mix += (mix.empty() ? "" : ", ") + entry.first + " x" + std::to_string(entry.second);
        }
        outStatus = "Placed " + std::to_string(commands.size()) + " (" + mix + ")";
        if (!intoGroups.empty())
        {
            outStatus += " into ";
            std::size_t shownGroups = 0;
            for (const auto& entry : intoGroups)
            {
                outStatus += (shownGroups++ ? ", " : "") + entry.first;
            }
        }
        const std::size_t count = commands.size();
        return FoldIntoOneEntry(std::move(commands), "Place " + std::to_string(count) + " objects");
    }
}

#endif // WITH_EDITOR
