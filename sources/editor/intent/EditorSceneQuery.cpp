#include "editor/intent/EditorSceneQuery.h"
#if WITH_EDITOR

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>

#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "editor/EditorFraming.h"
#include "editor/intent/EditorIntentResolver.h"
#include "editor/scene/EditorZone.h"
#include "ocean/OceanRenderable.h"

namespace
{
    // What to call an object when reporting it. The asset key where there is one, because
    // that is the string the model's own filter vocabulary is written in -- reporting a
    // display name it cannot then use as a needle would be a dead end.
    std::string AssetKeyOf(const EditorObject& object)
    {
        return object.properties.value("asset",
            object.properties.value("mesh", object.type));
    }

    // Rounded to a decimetre. The level is metres of sand and palm trees; the digits past
    // this one are noise the model has to carry through every later turn.
    //
    // ROUNDED IN DOUBLE, NOT IN FLOAT, which is not fussiness. Rounding 263.20001f to one
    // place gives 263.2f, and widening THAT to double gives 263.20001220703125 back again --
    // so the first version of this printed the full float error after carefully removing it.
    double Round1(float value)
    {
        return std::round(static_cast<double>(value) * 10.0) / 10.0;
    }

    nlohmann::json Xyz(const Math::float3& v)
    {
        return nlohmann::json::array({ Round1(v.x), Round1(v.y), Round1(v.z) });
    }

    struct Aabb
    {
        Math::float3 min{ std::numeric_limits<float>::max(),
                          std::numeric_limits<float>::max(),
                          std::numeric_limits<float>::max() };
        Math::float3 max{ -std::numeric_limits<float>::max(),
                          -std::numeric_limits<float>::max(),
                          -std::numeric_limits<float>::max() };
        bool valid = false;

        void Add(const Math::float3& lo, const Math::float3& hi)
        {
            min.x = std::min(min.x, lo.x);
            min.y = std::min(min.y, lo.y);
            min.z = std::min(min.z, lo.z);
            max.x = std::max(max.x, hi.x);
            max.y = std::max(max.y, hi.y);
            max.z = std::max(max.z, hi.z);
            valid = true;
        }

        void WriteInto(nlohmann::json& object) const
        {
            if (!valid)
            {
                return;
            }
            const Math::float3 size(max.x - min.x, max.y - min.y, max.z - min.z);
            const Math::float3 centre((min.x + max.x) * 0.5f,
                (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f);
            object["min"] = Xyz(min);
            object["max"] = Xyz(max);
            object["centre"] = Xyz(centre);
            object["size"] = Xyz(size);
        }
    };

    // Bounds of a set of document objects, from the RUNTIME world AABBs. editorframing
    // already owns "where is this object and how big is it" -- the F key and "show me the
    // palms" go through it -- and a second answer to that question here would be a second
    // answer to it in the editor.
    Aabb BoundsOf(const EditorContext& ctx, const std::vector<EditorObjectId>& ids)
    {
        Aabb box;
        for (const EditorObjectId id : ids)
        {
            Math::float3 lo;
            Math::float3 hi;
            if (editorframing::TryGetWorldBounds(ctx.scene, ctx.document, id, lo, hi))
            {
                box.Add(lo, hi);
            }
        }
        return box;
    }

    // [x, z] is accepted alongside [x, y, z]: the ground probe uses two of the three, and
    // refusing the shorter spelling would be a rule with nothing behind it.
    bool ReadOnePoint(const nlohmann::json& value, Math::float3& out)
    {
        if (!value.is_array())
        {
            return false;
        }
        if (value.size() == 2 && value[0].is_number() && value[1].is_number())
        {
            out = Math::float3(value[0].get<float>(), 0.0f, value[1].get<float>());
            return true;
        }
        if (value.size() == 3 && value[0].is_number() && value[1].is_number() &&
            value[2].is_number())
        {
            out = Math::float3(value[0].get<float>(), value[1].get<float>(),
                value[2].get<float>());
            return true;
        }
        return false;
    }

    // `point` for one, `points` for many. Capped, because a probe is a scene raycast and a
    // model asked for a shoreline could otherwise request ten thousand of them inside one
    // frame -- the bury action already learned that lesson, at 64 rays per object.
    bool ReadPoints(const nlohmann::json& params, std::vector<Math::float3>& out)
    {
        constexpr std::size_t kMaxPoints = 64;
        const auto many = params.find("points");
        if (many != params.end() && many->is_array())
        {
            for (const nlohmann::json& entry : *many)
            {
                Math::float3 point;
                if (ReadOnePoint(entry, point))
                {
                    out.push_back(point);
                }
                if (out.size() >= kMaxPoints)
                {
                    break;
                }
            }
            return !out.empty();
        }
        const auto one = params.find("point");
        Math::float3 point;
        if (one != params.end() && ReadOnePoint(*one, point))
        {
            out.push_back(point);
            return true;
        }
        return false;
    }

    const char* ShapeName(editorzone::Shape shape)
    {
        switch (shape)
        {
        case editorzone::Shape::Circle: return "circle";
        case editorzone::Shape::Rect:   return "rect";
        case editorzone::Shape::Spline: return "spline";
        }
        return "?";
    }

    const char* PlaneName(editorzone::Plane plane)
    {
        switch (plane)
        {
        case editorzone::Plane::XZ: return "XZ";
        case editorzone::Plane::XY: return "XY";
        case editorzone::Plane::ZY: return "ZY";
        }
        return "?";
    }

    bool WaterLevel(EditorContext& ctx, float& outLevel)
    {
        if (const OceanRenderable* ocean = ctx.scene.FindOceanRenderable())
        {
            outLevel = ocean->GetWaterLevel();
            return true;
        }
        return false;
    }

    const std::vector<EditorQueryDesc>& Queries()
    {
        // Short on purpose. A query costs a whole extra generation -- measured at 3-4 s for
        // a command-shaped answer -- so an entry has to answer something a designer really
        // asks and that the prompt cannot carry. Counting objects is already an ACTION
        // (`count`) because its answer is for the user; these are for the model.
        static const std::vector<EditorQueryDesc> queries = {
            { "sceneSummary",
              "how big this level is and what is in it: object count, the commonest assets, "
              "the world bounds of everything, and the zones that exist",
              false, "" },
            { "bounds",
              "the world bounding box, centre and size of whatever the target names. Use it "
              "before placing something relative to an existing thing",
              true, "" },
            { "waterLevel",
              "the Y of the ocean surface, or that this level has no ocean. Everything below "
              "it is under water",
              false, "" },
            { "groundHeight",
              "the height of the surface under world points, by casting a ray down through "
              "the scene, and whether each one is above the waterline. Ask for MANY points "
              "at once -- tracing a shoreline is this question along a line",
              false, "point: [x, z] or [x, y, z]; or points: a list of those, up to 64" },
            { "zoneInfo",
              "the shape of one named zone: its kind, size, control points, whether it is "
              "closed, and whether it fills along the line or inside it",
              false, "zone: the zone's name" },
            { "selection",
              "what the designer has selected right now. The prompt cannot say -- it is "
              "built once per level and the selection changes constantly",
              false, "" },
            // Asked for by the model reading its own query list: bounds is a box, and a box
            // cannot say which way a thing is facing. "Put another one like that beside it"
            // needs the rotation, and approximating it from an axis-aligned box is exactly
            // the kind of guess this layer exists to avoid.
            { "getTransform",
              "the exact position, rotation in degrees and scale of each object the target "
              "names. bounds gives a box; this gives orientation, which cloning or aligning "
              "needs",
              true, "" },
        };
        return queries;
    }

    // One question, answered into a JSON value. The caller keys it by name and dumps the lot.
    nlohmann::json AnswerOne(const EditorActionContext& actionCtx, const EditorIntentQuery& ask)
    {
        EditorContext& ctx = actionCtx.editor;
        nlohmann::json out = nlohmann::json::object();

        if (ask.query == "waterLevel")
        {
            float level = 0.0f;
            if (WaterLevel(ctx, level))
            {
                out["y"] = Round1(level);
                out["note"] = "anything below this y is under water";
            }
            else
            {
                out["error"] = "this level has no ocean, so there is no waterline";
            }
            return out;
        }

        if (ask.query == "sceneSummary")
        {
            std::map<std::string, std::size_t> perType;
            std::vector<EditorObjectId> everything;
            nlohmann::json zoneNames = nlohmann::json::array();
            everything.reserve(ctx.document.Objects().size());
            for (const EditorObject& object : ctx.document.Objects())
            {
                everything.push_back(object.id);
                if (object.type == editorzone::kTypeName)
                {
                    zoneNames.push_back(object.name);
                    continue;
                }
                ++perType[AssetKeyOf(object)];
            }
            out["objects"] = ctx.document.Objects().size();

            // The commonest eight. A level with sixty kinds of thing in it would otherwise
            // spend most of this answer on the fifty nobody asked about.
            std::vector<std::pair<std::string, std::size_t>> sorted(perType.begin(), perType.end());
            std::sort(sorted.begin(), sorted.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
            nlohmann::json assets = nlohmann::json::object();
            for (std::size_t i = 0; i < sorted.size() && i < 8; ++i)
            {
                assets[sorted[i].first] = sorted[i].second;
            }
            out["assets"] = assets;
            if (sorted.size() > 8)
            {
                out["otherAssetKinds"] = sorted.size() - 8;
            }
            BoundsOf(ctx, everything).WriteInto(out);
            float level = 0.0f;
            if (WaterLevel(ctx, level))
            {
                out["waterLevel"] = Round1(level);
            }
            out["zones"] = zoneNames;
            return out;
        }

        if (ask.query == "selection")
        {
            std::vector<EditorObjectId> ids;
            nlohmann::json items = nlohmann::json::object();
            for (const EditorObjectId id : ctx.selection.Ordered())
            {
                ids.push_back(id);
                if (const EditorObject* object = ctx.document.Find(id))
                {
                    const std::string key = object->type == editorzone::kTypeName
                        ? "zone " + object->name : AssetKeyOf(*object);
                    items[key] = items.value(key, std::size_t{ 0 }) + 1;
                }
            }
            out["count"] = ids.size();
            out["items"] = items;
            BoundsOf(ctx, ids).WriteInto(out);
            return out;
        }

        if (ask.query == "zoneInfo")
        {
            std::string name = ask.params.value("zone", std::string{});
            if (name.empty())
            {
                name = ask.target.where.zone;
            }
            editorzone::Zone zone;
            std::string whyNot;
            if (name.empty() || !editorzone::Find(ctx.document, name, zone, whyNot))
            {
                // The lookup's own reason, not a guess: it distinguishes "no such zone"
                // from "that matched two of them", and the second needs a different fix.
                out["error"] = name.empty() ? "zoneInfo needs params.zone naming a zone" : whyNot;
                return out;
            }
            out["name"] = zone.name;
            out["shape"] = ShapeName(zone.shape);
            out["centre"] = Xyz(zone.centre);
            out["boundingRadius"] = Round1(editorzone::BoundingRadius(zone));
            if (zone.shape == editorzone::Shape::Spline)
            {
                out["points"] = zone.points.size();
                out["closed"] = zone.closed;
                out["fill"] = zone.fill == editorzone::Fill::Inside ? "inside" : "along";
                out["halfWidth"] = Round1(zone.halfWidth);
                out["plane"] = PlaneName(zone.plane);
            }
            else
            {
                out["halfX"] = Round1(zone.halfX);
                out["halfZ"] = Round1(zone.halfZ);
                out["yawDeg"] = Round1(zone.yawDeg);
            }
            return out;
        }

        if (ask.query == "groundHeight")
        {
            // A LIST OF POINTS, not one. Tracing anything -- a shoreline, a path along a
            // ridge -- is the same question asked along a line, and one point per turn made
            // that cost a minute for an answer the editor finds in microseconds.
            std::vector<Math::float3> points;
            if (!ReadPoints(ask.params, points))
            {
                out["error"] = "groundHeight needs params.point as [x, z] or [x, y, z], "
                               "or params.points as a list of those";
                return out;
            }
            float level = 0.0f;
            const bool hasWater = WaterLevel(ctx, level);
            if (hasWater)
            {
                out["waterLevel"] = Round1(level);
            }
            const std::vector<std::uint64_t> noIgnores;
            nlohmann::json results = nlohmann::json::array();
            for (const Math::float3& point : points)
            {
                nlohmann::json entry = nlohmann::json::object();
                entry["at"] = nlohmann::json::array({ Round1(point.x),
                                                      Round1(point.z) });
                float height = 0.0f;
                if (!editorquery::ProbeGroundHeight(ctx.scene, point.x, point.z,
                        editorquery::kGroundProbeUp, noIgnores, height))
                {
                    entry["ground"] = nullptr;   // nothing below: open water or off the map
                }
                else
                {
                    entry["ground"] = Round1(height);
                    if (hasWater)
                    {
                        entry["aboveWater"] = height >= level;
                    }
                }
                results.push_back(entry);
            }
            out["samples"] = results;
            return out;
        }

        if (ask.query == "bounds" || ask.query == "getTransform")
        {
            EditorIntentTarget target = ask.target;
            std::vector<EditorObjectId> ids;
            std::vector<EditorIntentPreview::Group> groups;
            ResolveTargetObjects(actionCtx, target, ids, groups);
            out["count"] = ids.size();
            if (ids.empty())
            {
                out["error"] = "nothing in the level matches that target";
                return out;
            }

            if (ask.query == "getTransform")
            {
                // Capped, because "the transform of every palm" is 183 of them and the
                // model asked for orientation, not for an inventory it already has.
                constexpr std::size_t kMaxListed = 12;
                nlohmann::json objects = nlohmann::json::array();
                for (std::size_t i = 0; i < ids.size() && i < kMaxListed; ++i)
                {
                    const EditorObject* object = ctx.document.Find(ids[i]);
                    if (!object)
                    {
                        continue;
                    }
                    nlohmann::json entry = nlohmann::json::object();
                    entry["name"] = object->name;
                    entry["asset"] = AssetKeyOf(*object);
                    entry["position"] = Xyz(object->transform.position);
                    entry["rotationDeg"] = Xyz(object->transform.rotationDeg);
                    entry["scale"] = Xyz(object->transform.scale);
                    objects.push_back(entry);
                }
                out["objects"] = objects;
                if (ids.size() > kMaxListed)
                {
                    out["listed"] = kMaxListed;
                    out["note"] = "narrow the target to see the rest";
                }
                return out;
            }

            std::sort(groups.begin(), groups.end(),
                [](const EditorIntentPreview::Group& a, const EditorIntentPreview::Group& b)
                {
                    return a.count != b.count ? a.count > b.count : a.label < b.label;
                });
            nlohmann::json items = nlohmann::json::object();
            for (std::size_t i = 0; i < groups.size() && i < 6; ++i)
            {
                items[groups[i].label] = groups[i].count;
            }
            out["items"] = items;
            const Aabb box = BoundsOf(ctx, ids);
            box.WriteInto(out);
            if (!box.valid)
            {
                out["error"] = "none of them has bounds the editor can measure";
            }
            return out;
        }

        out["error"] = "query '" + ask.query + "' is listed but not implemented";
        return out;
    }
}

namespace editorquery
{
    const std::vector<EditorQueryDesc>& All()
    {
        return Queries();
    }

    const EditorQueryDesc* Find(const std::string& id)
    {
        for (const EditorQueryDesc& query : Queries())
        {
            if (id == query.id)
            {
                return &query;
            }
        }
        return nullptr;
    }

    bool ProbeGroundHeight(const Scene& scene,
        float x,
        float z,
        float startY,
        const std::vector<std::uint64_t>& ignored,
        float& outHeight)
    {
        const Math::float3 origin(x, startY, z);
        const Math::float3 down(0.0f, -1.0f, 0.0f);
        float distance = 0.0f;
        if (scene.RaycastEditorObject(origin, down, &distance, 0, &ignored) == 0 ||
            !std::isfinite(distance))
        {
            return false;
        }
        outHeight = startY - distance;
        return true;
    }

    std::string Answer(const EditorActionContext& actionCtx, const EditorIntent& intent)
    {
        nlohmann::json out = nlohmann::json::object();
        for (const EditorIntentQuery& ask : intent.asks)
        {
            if (!Find(ask.query))
            {
                out[ask.query] = { { "error", "no such query" } };
                continue;
            }
            // Keyed by NAME, so a batch comes back labelled. Asking the same one twice in
            // one turn overwrites rather than duplicating, which is the right reading of a
            // question asked twice.
            out[ask.query] = AnswerOne(actionCtx, ask);
        }
        return out.dump();
    }
}

#endif // WITH_EDITOR
