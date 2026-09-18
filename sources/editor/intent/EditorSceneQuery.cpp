#include "editor/intent/EditorSceneQuery.h"
#if WITH_EDITOR

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>

#include "app/camera/Camera.h"
#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "editor/EditorObjectMatch.h"
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
        return editormatch::AssetLabel(object);
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
              "the GROUPS objects have been put into, the world bounds of everything, and "
              "the zones that exist. Ask it again after grouping something to see the result",
              false, "", EditorQueryDesc::ChatArg::None },
            { "bounds",
              "the world bounding box, centre and size of whatever the target names. Use it "
              "before placing something relative to an existing thing",
              true, "", EditorQueryDesc::ChatArg::Filter },
            { "waterLevel",
              "the Y of the ocean surface, or that this level has no ocean. Everything below "
              "it is under water",
              false, "", EditorQueryDesc::ChatArg::None },
            { "groundHeight",
              "the height of the surface under world points, by casting a ray down through "
              "the scene, and whether each one is above the waterline. Ask for MANY points "
              "at once -- tracing a shoreline is this question along a line",
              false, "point: [x, z] or [x, y, z]; or points: a list of those, up to 64",
              EditorQueryDesc::ChatArg::Point },
            { "zoneInfo",
              "the shape of one named zone: its kind, size, control points, whether it is "
              "closed, and whether it fills along the line or inside it",
              false, "zone: the zone's name", EditorQueryDesc::ChatArg::ZoneName },
            { "selection",
              "what the designer has selected right now. The prompt cannot say -- it is "
              "built once per level and the selection changes constantly",
              false, "", EditorQueryDesc::ChatArg::None },
            // Asked for by the model reading its own query list: bounds is a box, and a box
            // cannot say which way a thing is facing. "Put another one like that beside it"
            // needs the rotation, and approximating it from an axis-aligned box is exactly
            // the kind of guess this layer exists to avoid.
            // Deixis. "вон тот камень", "а это что?" -- the designer is pointing with the
            // camera, and until now that was answerable only if they had also clicked the
            // thing. The answer is exactly what the next turn needs: a name to filter on
            // and a position to place `at`.
            { "lookingAt",
              "what the camera is pointed at right now: the first object under the centre of "
              "the view, with its name, asset and position. Use it for \"that one\", \"вон "
              "тот\", \"это\" -- anything the designer is pointing at rather than naming",
              false, "", EditorQueryDesc::ChatArg::None },
            { "getTransform",
              "the exact position, rotation in degrees and scale of each object the target "
              "names. bounds gives a box; this gives orientation, which cloning or aligning "
              "needs",
              true, "", EditorQueryDesc::ChatArg::Filter },
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
            std::map<std::string, std::size_t> perGroup;
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
                if (object.properties.is_object())
                {
                    const auto it = object.properties.find("group");
                    if (it != object.properties.end() && it->is_string() &&
                        !it->get<std::string>().empty())
                    {
                        ++perGroup[it->get<std::string>()];
                    }
                }
            }
            out["objects"] = ctx.document.Objects().size();

            // THE CAP WAS EIGHT AND EIGHT WAS TOO FEW. A level with sixty kinds in it
            // should not spend this answer on the fifty nobody asked about -- but wind_test
            // has twelve, so four kinds were replaced by the number 4, and among the four
            // were the island, the tents and the rocks. Asked to tidy the outliner, the
            // model wrote a filter naming the eight it could see and grouped only those:
            // the answer looked complete and left a third of the level out. Twenty covers
            // every level in this project with room to spare and costs a few dozen tokens.
            constexpr std::size_t kListedAssets = 20;
            std::vector<std::pair<std::string, std::size_t>> sorted(perType.begin(), perType.end());
            std::sort(sorted.begin(), sorted.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
            nlohmann::json assets = nlohmann::json::object();
            for (std::size_t i = 0; i < sorted.size() && i < kListedAssets; ++i)
            {
                assets[sorted[i].first] = sorted[i].second;
            }
            out["assets"] = assets;
            if (sorted.size() > kListedAssets)
            {
                // NAMED, not counted. "otherAssetKinds: 4" tells the reader that something
                // is missing and gives it no way to ask about it.
                out["otherAssetKinds"] = sorted.size() - kListedAssets;
                out["assetsNote"] = "the list above is the " + std::to_string(kListedAssets) +
                    " commonest kinds and is NOT complete -- do not build a filter from it "
                    "when you mean everything; leave the filter out instead";
            }
            BoundsOf(ctx, everything).WriteInto(out);
            float level = 0.0f;
            if (WaterLevel(ctx, level))
            {
                out["waterLevel"] = Round1(level);
            }
            out["zones"] = zoneNames;

            // ALWAYS PRESENT, EVEN WHEN EMPTY. An absent key is read as "not mentioned" and
            // filled in from imagination; an empty object is read as "none", which is the
            // fact. This is the same rule the prompt's ZONES section learned the hard way.
            nlohmann::json groups = nlohmann::json::object();
            for (const auto& entry : perGroup)
            {
                groups[entry.first] = entry.second;
            }
            out["groups"] = groups;
            if (perGroup.empty())
            {
                out["groupsNote"] = "nothing in this level is in a group yet";
            }
            else
            {
                out["groupsNote"] = "a group's name works as a filter, like an asset name";
            }
            return out;
        }

        if (ask.query == "lookingAt")
        {
            const Math::float3 origin = ctx.scene.CameraRef().GetPosition();
            const Math::float3 direction = ctx.scene.CameraRef().GetDirection();
            float distance = 0.0f;
            const std::vector<std::uint64_t> noIgnores;
            const Scene::SceneObjectId hit =
                ctx.scene.RaycastEditorObject(origin, direction, &distance, 0, &noIgnores);
            if (hit == 0 || !std::isfinite(distance))
            {
                out["hit"] = false;
                // Where the view meets the ground is still an answer: "put it over there"
                // with nothing standing there means the ground there.
                float height = 0.0f;
                if (direction.y < -0.05f)
                {
                    const float t = std::min(-origin.y / direction.y, 1000.0f);
                    const float x = origin.x + direction.x * t;
                    const float z = origin.z + direction.z * t;
                    if (editorquery::ProbeGroundHeight(ctx.scene, x, z,
                            editorquery::kGroundProbeUp, noIgnores, height))
                    {
                        out["groundUnderView"] = Xyz(Math::float3(x, height, z));
                    }
                }
                return out;
            }
            out["hit"] = true;
            out["distance"] = Round1(distance);
            if (const EditorObject* object = ctx.document.Find(EditorObjectId{ hit }))
            {
                out["name"] = object->name;
                out["asset"] = AssetKeyOf(*object);
                out["position"] = Xyz(object->transform.position);
                out["rotationDeg"] = Xyz(object->transform.rotationDeg);
                Math::float3 lo;
                Math::float3 hi;
                if (editorframing::TryGetWorldBounds(ctx.scene, ctx.document,
                        EditorObjectId{ hit }, lo, hi))
                {
                    Aabb box;
                    box.Add(lo, hi);
                    box.WriteInto(out);
                }
            }
            else
            {
                // A runtime object with no document entry -- terrain generated at load, for
                // instance. Saying so beats reporting nothing.
                out["name"] = "(not an editable object)";
            }
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
            nlohmann::json selectedGroups = nlohmann::json::object();
            for (const EditorObjectId id : ids)
            {
                const EditorObject* object = ctx.document.Find(id);
                if (!object || !object->properties.is_object())
                {
                    continue;
                }
                const auto it = object->properties.find("group");
                if (it != object->properties.end() && it->is_string() &&
                    !it->get<std::string>().empty())
                {
                    const std::string name = it->get<std::string>();
                    selectedGroups[name] = selectedGroups.value(name, std::size_t{ 0 }) + 1;
                }
            }
            out["groups"] = selectedGroups;
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
        float& outHeight,
        const std::vector<std::uint64_t>* only)
    {
        const Math::float3 origin(x, startY, z);
        const Math::float3 down(0.0f, -1.0f, 0.0f);
        float distance = 0.0f;
        if (scene.RaycastEditorObject(origin, down, &distance, 0, &ignored,
                (only && !only->empty()) ? only : nullptr) == 0 ||
            !std::isfinite(distance))
        {
            return false;
        }
        outHeight = startY - distance;
        return true;
    }

    std::string AnswerChatLine(const EditorActionContext& actionCtx, const std::string& line)
    {
        // "bounds coconut_palm" -> name, then the rest.
        const std::size_t nameEnd = line.find_first_of(" \t");
        const std::string name = line.substr(0, nameEnd);
        std::string rest = nameEnd == std::string::npos ? std::string{} : line.substr(nameEnd);
        const std::size_t from = rest.find_first_not_of(" \t");
        rest = from == std::string::npos ? std::string{} : rest.substr(from);
        while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t'))
        {
            rest.pop_back();
        }

        const EditorQueryDesc* desc = Find(name);
        if (!desc)
        {
            std::string known;
            for (const EditorQueryDesc& query : All())
            {
                known += (known.empty() ? "" : ", ") + std::string(query.id);
            }
            return "no such scene query '" + name + "'. There are: " + known;
        }

        EditorIntent intent;
        intent.kind = EditorIntentKind::Query;
        EditorIntentQuery ask;
        ask.query = name;
        switch (desc->chatArg)
        {
        case EditorQueryDesc::ChatArg::Filter:
            if (rest.empty())
            {
                return "'" + name + "' needs something to look at, e.g. \"" + name +
                    " coconut_palm\"";
            }
            ask.target.filter.push_back(rest);
            ask.target.scope = EditorIntentScope::All;
            break;
        case EditorQueryDesc::ChatArg::Point:
        {
            // "12 -40" or "12 -40 3". Two numbers is a ground plan question, which is what
            // this is for; the third is accepted because refusing it would be a rule with
            // nothing behind it.
            std::vector<float> numbers;
            std::size_t at = 0;
            while (at < rest.size() && numbers.size() < 3)
            {
                std::size_t used = 0;
                try
                {
                    numbers.push_back(std::stof(rest.substr(at), &used));
                }
                catch (const std::exception&)
                {
                    break;
                }
                at += used;
                while (at < rest.size() && (rest[at] == ' ' || rest[at] == '\t' ||
                    rest[at] == ',')) { ++at; }
            }
            if (numbers.size() < 2)
            {
                return "'" + name + "' needs a point, e.g. \"" + name + " 12 -40\"";
            }
            ask.params["point"] = numbers.size() >= 3
                ? nlohmann::json::array({ numbers[0], numbers[1], numbers[2] })
                : nlohmann::json::array({ numbers[0], numbers[1] });
            break;
        }
        case EditorQueryDesc::ChatArg::ZoneName:
            if (rest.empty())
            {
                return "'" + name + "' needs a zone name, e.g. \"" + name + " Beach\"";
            }
            ask.params["zone"] = rest;
            break;
        case EditorQueryDesc::ChatArg::None:
            // AN ARGUMENT NOBODY ASKED FOR IS A MISUNDERSTANDING, not noise to drop. Asked
            // how wind_test differed from the open level, the model wrote
            // `scene sceneSummary wind_test`, this branch discarded the name, and the query
            // described the OPEN level for the second time -- so the model compared the
            // level to itself and reported them identical. Saying "I ignored that" is the
            // whole fix: it can then go and read the file, which is where that answer is.
            if (!rest.empty())
            {
                return "'" + name + "' takes no argument and always describes the level "
                    "that is OPEN in the editor, so '" + rest + "' was not used. These "
                    "queries cannot be pointed at another level. To see one that is not "
                    "open, read its file: TOOL: read data/levels/" + rest + ".json 1 120";
            }
            break;
        }
        intent.asks.push_back(std::move(ask));
        return Answer(actionCtx, intent);
    }

    std::string ChatProtocolPrompt()
    {
        std::string text =
            "\nYOU CAN ALSO ASK ABOUT THE LEVEL THAT IS OPEN, the same way:\n";
        for (const EditorQueryDesc& query : All())
        {
            text += "  TOOL: scene " + std::string(query.id);
            switch (query.chatArg)
            {
            case EditorQueryDesc::ChatArg::Filter:   text += " <asset or group name>"; break;
            case EditorQueryDesc::ChatArg::Point:    text += " <x> <z>"; break;
            case EditorQueryDesc::ChatArg::ZoneName: text += " <zone name>"; break;
            case EditorQueryDesc::ChatArg::None:     break;
            }
            text += "\n      " + std::string(query.description) + "\n";
        }
        text +=
            "These read the LIVE level, not a file, so they are the only way to answer "
            "\"which of these objects is the island\" or \"how big is it\" -- searching the "
            "source cannot, and guessing from the object count is how a wrong answer gets "
            "written confidently. Start with sceneSummary when you do not know what is in "
            "the level.\n"
            // WHERE THE OTHER LEVELS ARE, said here rather than only in the file-search
            // block, because here is where the question comes up. Asked how wind_test
            // differed from the open one, the model ran sceneSummary, did not find
            // wind_test in it, and answered that no such level existed -- while
            // data/levels/wind_test.json sat there readable. It had both tools and did not
            // connect them, which is a gap in what it was told, not in what it can do.
            "THESE SEE ONE LEVEL: the open one. They cannot be pointed at another, and a\n"
            "level missing from sceneSummary is not a level that does not exist -- every\n"
            "level in this project is a file in data/levels. To answer about one that is\n"
            "not open, `ls data/levels` for the names and `read` the file. Opening a level\n"
            "is not something you can offer either; the editor has no action for it.\n";
        return text;
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
