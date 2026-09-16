#include "editor/intent/EditorActionRegistry.h"
#if WITH_EDITOR

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

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

    // ---------------------------------------------------------------- setColor

    // THE ONE GENUINE GAP IN THE REFUSAL LOG. "покрась пальмы в ярко-красный" was answered
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
    // "удали северную рощу" is a delete whose filter is a group name, and nothing had to be
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
            if (name.empty())
            {
                after.erase("group");
            }
            else
            {
                after["group"] = name;
            }
            if (after == before)
            {
                continue;
            }
            commands.push_back(std::make_unique<EditObjectPropertiesCommand>(
                id, std::move(before), std::move(after),
                name.empty() ? "Ungroup" : "Group"));
        }
        if (commands.empty())
        {
            outStatus = name.empty() ? "None of them was in a group"
                                     : "They are already in '" + name + "'";
            return nullptr;
        }
        outStatus = name.empty()
            ? "Removed " + std::to_string(commands.size()) + " from their group"
            : "Put " + std::to_string(commands.size()) + " in '" + name + "'";
        return FoldIntoOneEntry(std::move(commands), name.empty() ? "Ungroup" : "Group");
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

    std::unique_ptr<EditorCommand> BuildRename(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        const std::string base = StringOr(intent.params, "name", "");
        if (base.empty())
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
        for (const EditorObjectId id : targets)
        {
            const EditorObject* object = document.Find(id);
            if (!object)
            {
                outStatus = "Selection contains an object that is no longer in the level";
                return nullptr;
            }
            std::string after = base;
            if (numbered)
            {
                char suffix[8] = {};
                std::snprintf(suffix, sizeof(suffix), " %03d", ++ordinal);
                after += suffix;
            }
            commands.push_back(std::make_unique<RenameObjectCommand>(id, object->name, after));
        }

        outStatus = "Renamed " + CountedObjects(targets.size()) + " to \"" + base +
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
        std::vector<Math::float3> obstacles;
        const float obstacleReach = scatterRadius + minSeparation;
        for (const EditorObject& object : ctx.document.Objects())
        {
            const Math::float3& p = object.transform.position;
            const float dx = p.x - anchor.x;
            const float dz = p.z - anchor.z;
            if (dx * dx + dz * dz <= obstacleReach * obstacleReach)
            {
                obstacles.push_back(p);
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
            const float sepSq = minSeparation * minSeparation;
            const auto tooClose = [&](const Math::float3& other)
            {
                const float dx = other.x - candidate.x;
                const float dz = other.z - candidate.z;
                return dx * dx + dz * dz < sepSq;
            };
            if (minSeparation > 0.0f &&
                (std::any_of(placed.begin(), placed.end(), tooClose) ||
                 std::any_of(obstacles.begin(), obstacles.end(), tooClose)))
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
        // as "default 1" with no upper bound, "зарой в два раза глубже" came back as 200
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
        "This changes the LABEL only; nothing moves and no mesh changes.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "name", EditorParamKind::String, true, "the new name" },
        },
        &BuildRename,
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
        "setColor",
        "THE ACTION FOR \"покрась\", \"перекрась\", \"сделай <цвет>\", \"paint\", \"tint\". "
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

    actions_.push_back({
        "group",
        "Put the matching objects into a named GROUP -- a label they carry, shown as a "
        "folder in the outliner. Once grouped, the group's name works as a filter "
        "everywhere: \"спрячь северную рощу\" is setEnabled with that name in the filter, "
        "\"удали её\" is a delete. Use it for \"сгруппируй\", \"собери в группу\", "
        "\"назови это\". An EMPTY name takes them out of whatever group they were in.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "name", EditorParamKind::String, false,
              "the group's name; empty removes them from their group" },
        },
        &BuildGroup,
    });

    actions_.push_back({
        "thin",
        "Thin OUT objects that are already in the level, by deleting some of them. Two ways "
        "to say how much: minSeparation removes whatever stands closer together than that "
        "many metres, keepPercent keeps roughly that share and drops the rest evenly. This "
        "is the answer to \"проредь\", \"слишком густо\", \"убери половину\" -- it is the "
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
        "names a place the level does not have yet (\"заведи зону на пляже\", \"сделай "
        "область вокруг того камня\"), and when a later command will need to refer to it.",
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
    // while every other action says it with target.where.zone. "удали пальмы в зоне Beach"
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
