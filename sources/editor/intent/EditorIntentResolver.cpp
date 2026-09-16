#include "editor/intent/EditorIntentResolver.h"
#if WITH_EDITOR

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "app/camera/Camera.h"
#include "app/scene/Scene.h"
#include "core/logging/Log.h"
#include "editor/EditorContext.h"
#include "editor/EditorObjectMatch.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/intent/EnvironmentSettings.h"

namespace
{
    // What to call a group of objects in the preview. The ASSET is the useful name:
    // a hundred palms are all "Palm_0xx" individually and `coconut_palm` together, and
    // it is the asset the user was thinking of when they said "palms".
    std::string GroupLabel(const EditorObject& object)
    {
        if (object.properties.is_object())
        {
            for (const char* key : { "mesh", "model", "preset", "material" })
            {
                const auto it = object.properties.find(key);
                if (it != object.properties.end() && it->is_string())
                {
                    const std::string value = it->get<std::string>();
                    if (!value.empty())
                    {
                        return value;
                    }
                }
            }
        }
        return object.type.empty() ? object.name : object.type;
    }

    bool MatchesAnyNeedle(const EditorObject& object, const std::vector<std::string>& needles)
    {
        for (const std::string& needle : needles)
        {
            if (!needle.empty() && editormatch::MatchesSearch(object, needle))
            {
                return true;
            }
        }
        return false;
    }

    void AppendGroup(std::vector<EditorIntentPreview::Group>& groups, const std::string& label)
    {
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

    Math::float3 SelectionCentre(const EditorContext& ctx, bool& outValid)
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
        outValid = count > 0;
        if (!outValid)
        {
            return sum;
        }
        const float inv = 1.0f / static_cast<float>(count);
        return Math::float3(sum.x * inv, sum.y * inv, sum.z * inv);
    }

    // Narrowing by PLACE. An empty filter passes everything, which is what a phrase that
    // said nothing about location means.
    bool PassesSpatialFilter(const EditorContext& ctx,
        const EditorSpatialFilter& where,
        const Math::float3& position)
    {
        if (where.hasMinY && position.y < where.minY)
        {
            return false;
        }
        if (where.hasMaxY && position.y > where.maxY)
        {
            return false;
        }
        if (where.radius <= 0.0f || where.anchor == EditorSpatialAnchor::None)
        {
            return true;
        }

        Math::float3 anchor = where.point;
        if (where.anchor == EditorSpatialAnchor::Camera)
        {
            anchor = ctx.scene.CameraRef().GetPosition();
        }
        else if (where.anchor == EditorSpatialAnchor::Selection)
        {
            bool valid = false;
            const Math::float3 centre = SelectionCentre(ctx, valid);
            if (!valid)
            {
                return true;   // nothing to measure from; do not silently drop everything
            }
            anchor = centre;
        }

        const float dx = position.x - anchor.x;
        const float dy = position.y - anchor.y;
        const float dz = position.z - anchor.z;
        return dx * dx + dy * dy + dz * dz <= where.radius * where.radius;
    }

    // Resolve a name the user typed to ONE asset record. Substring search can match
    // several -- "coconut_palm" hits both the modern `.mesh.json` and the raw glTF it was
    // imported from -- so the tie is broken deliberately rather than by list order.
    const EditorAssetRecord* ResolveAsset(const AssetRegistry& registry,
        const std::string& needle,
        std::string& outProblem)
    {
        const std::vector<const EditorAssetRecord*> matches =
            registry.Search(needle, EditorAssetType::Mesh);
        if (matches.empty())
        {
            outProblem = "No mesh asset matches '" + needle + "'";
            return nullptr;
        }

        // An exact name wins outright.
        for (const EditorAssetRecord* record : matches)
        {
            if (record->displayName == needle || record->id.key == needle || record->path == needle)
            {
                return record;
            }
        }

        // Then the modern mesh asset: a `.mesh.json` carries the geometry, layout, shader
        // and material defaults, so spawning it needs no guesswork about the plumbing.
        const EditorAssetRecord* meshAsset = nullptr;
        std::size_t meshAssetCount = 0;
        for (const EditorAssetRecord* record : matches)
        {
            if (record->path.size() >= 10 &&
                record->path.compare(record->path.size() - 10, 10, ".mesh.json") == 0)
            {
                meshAsset = record;
                ++meshAssetCount;
            }
        }
        if (meshAssetCount == 1)
        {
            return meshAsset;
        }

        if (matches.size() == 1)
        {
            return matches.front();
        }

        // Genuinely ambiguous: say so and list a few, rather than picking one and being
        // wrong about which palm the user meant.
        outProblem = "'" + needle + "' matches " + std::to_string(matches.size()) + " assets: ";
        for (std::size_t i = 0; i < matches.size() && i < 4; ++i)
        {
            if (i > 0)
            {
                outProblem += ", ";
            }
            outProblem += matches[i]->displayName;
        }
        if (matches.size() > 4)
        {
            outProblem += ", ...";
        }
        return nullptr;
    }
}

EditorIntentPreview BuildIntentPreview(const EditorActionContext& actionCtx,
    const EditorIntent& intent)
{
    const EditorContext& ctx = actionCtx.editor;
    EditorIntentPreview preview;
    preview.resolved = intent;

    if (intent.kind != EditorIntentKind::Command)
    {
        preview.problem = "Nothing to run";
        return preview;
    }

    const EditorActionDesc* action = EditorActionRegistry::Builtin().Find(intent.action);
    if (!action)
    {
        // Structurally impossible from either source -- the grammar emits registry ids
        // and the model is constrained to them -- so this can only mean the registry
        // lost an entry the parser still knows about.
        preview.problem = "Unknown action '" + intent.action + "'";
        return preview;
    }
    preview.undoable = action->effect == EditorActionEffect::DocumentEdit;
    preview.answerOnly = action->effect == EditorActionEffect::ReadOnly;
    preview.viewOnly = action->effect == EditorActionEffect::ViewChange;

    // Parameters are checked BEFORE the target is resolved and long before a command is
    // built, so a bad argument costs a sentence rather than a half-applied edit.
    std::string paramError;
    if (!ValidateParams(*action, preview.resolved.params, paramError))
    {
        preview.problem = paramError;
        return preview;
    }

    if (action->target == EditorTargetKind::Environment)
    {
        const std::vector<envsettings::Setting> settings = envsettings::Enumerate(ctx.document);
        const envsettings::Setting* setting =
            envsettings::Find(settings, intent.target.setting);
        if (!setting)
        {
            preview.problem = intent.target.setting.empty()
                ? "Name a setting to change, e.g. wind.strength"
                : "No environment setting called '" + intent.target.setting + "'";
            return preview;
        }

        // The preview does the SAME arithmetic the run will do, and shows the actual
        // numbers. "setEnvironment 1 setting" would be a preview of nothing: what matters
        // about a look change is what the value becomes, and a factor applied to the wrong
        // current value is exactly the mistake worth catching here.
        nlohmann::json newValue;
        std::string valueError;
        if (!ComputeEnvironmentValue(*setting, preview.resolved.params, newValue, valueError))
        {
            preview.problem = valueError;
            return preview;
        }

        preview.resolved.target.setting = setting->path;
        preview.summary = setting->path + ": " + envsettings::ToText(setting->current) +
            "  ->  " + envsettings::ToText(newValue);
        preview.groups.push_back({ std::string(envsettings::KindName(setting->kind)), 1 });
        preview.executable = true;
        return preview;
    }

    // `replace` is the verb with TWO nouns: it selects existing objects AND names the
    // asset they should become. Resolve the asset for any action that carries one.
    if (!intent.target.asset.empty() || action->target == EditorTargetKind::Asset)
    {
        if (intent.target.asset.empty())
        {
            preview.problem = "'" + std::string(action->id) + "' needs an asset to place";
            return preview;
        }
        std::string assetProblem;
        const EditorAssetRecord* record =
            ResolveAsset(actionCtx.assets, intent.target.asset, assetProblem);
        if (!record)
        {
            preview.problem = assetProblem;
            return preview;
        }
        // Downstream works with the resolved key, never the phrase the user typed.
        preview.resolved.target.asset = record->id.key;

        if (action->target == EditorTargetKind::Asset)
        {
            const int count =
                std::clamp(static_cast<int>(preview.resolved.params.value("count", 1.0f)), 1, 200);
            preview.groups.push_back({ record->displayName, static_cast<std::size_t>(count) });
            // "up to", because the ground decides: water, cliffs and whatever is already
            // standing there can refuse a spot, and the run reports what actually landed.
            preview.summary = std::string(action->id) + " up to " + std::to_string(count) +
                " x " + record->displayName;
            preview.executable = true;
            return preview;
        }
    }

    if (action->target == EditorTargetKind::None)
    {
        preview.summary = std::string(action->id);
        preview.executable = true;
        return preview;
    }

    // --- target is existing objects -------------------------------------------------

    // Some actions get their filter from the EDITOR rather than the phrase: "select the
    // ones like this" is answerable only here, because the model never sees the selection
    // (and putting it in the prompt would break the prefill cache every time it changed).
    if (action->filterFromSelection && preview.resolved.target.filter.empty())
    {
        for (const EditorObjectId id : ctx.selection.Ordered())
        {
            if (const EditorObject* object = ctx.document.Find(id))
            {
                const std::string label = GroupLabel(*object);
                if (!label.empty() &&
                    std::find(preview.resolved.target.filter.begin(),
                        preview.resolved.target.filter.end(), label) ==
                        preview.resolved.target.filter.end())
                {
                    preview.resolved.target.filter.push_back(label);
                }
            }
        }
        if (preview.resolved.target.filter.empty())
        {
            preview.problem = "Select an object first -- there is nothing to match against";
            return preview;
        }
        preview.resolved.target.scope = EditorIntentScope::All;
    }

    // An EMPTY filter with All scope would mean the whole level. No source is allowed to
    // produce it, and if one does the answer is "no" rather than "all". An `exclude` list
    // is a filter too -- "everything except the palms" discriminates, "everything" does not.
    const bool saysNothingAboutWhich = preview.resolved.target.filter.empty() &&
        preview.resolved.target.exclude.empty() && !preview.resolved.target.where.Any();
    if (preview.resolved.target.scope == EditorIntentScope::All && saysNothingAboutWhich)
    {
        preview.problem = "Refusing to act on the whole level without a filter";
        return preview;
    }

    const EditorIntentTarget& target = preview.resolved.target;
    const auto consider = [&](const EditorObject& object)
    {
        if (!target.filter.empty() && !MatchesAnyNeedle(object, target.filter))
        {
            return;
        }
        if (MatchesAnyNeedle(object, target.exclude))
        {
            return;
        }
        if (!PassesSpatialFilter(ctx, target.where, object.transform.position))
        {
            return;
        }
        preview.targets.push_back(object.id);
        AppendGroup(preview.groups, GroupLabel(object));
    };

    if (target.scope == EditorIntentScope::Selected)
    {
        for (const EditorObjectId id : ctx.selection.Ordered())
        {
            const EditorObject* object = ctx.document.Find(id);
            if (!object)
            {
                // Environment entities are selectable too; they carry no searchable asset
                // and no transform, so only an unfiltered request takes them as they are.
                if (saysNothingAboutWhich)
                {
                    preview.targets.push_back(id);
                    AppendGroup(preview.groups, "environment");
                }
                continue;
            }
            consider(*object);
        }
    }
    else
    {
        for (const EditorObject& object : ctx.document.Objects())
        {
            consider(object);
        }
    }

    if (preview.targets.empty())
    {
        if (preview.answerOnly)
        {
            preview.summary = "Nothing matches -- there are none";
            preview.executable = true;
            return preview;
        }
        preview.problem = target.scope == EditorIntentScope::Selected
            ? "Nothing in the selection matches"
            : "No object in the level matches";
        return preview;
    }

    std::sort(preview.groups.begin(), preview.groups.end(),
        [](const EditorIntentPreview::Group& a, const EditorIntentPreview::Group& b)
        {
            return a.count != b.count ? a.count > b.count : a.label < b.label;
        });

    preview.summary = preview.answerOnly
        ? std::to_string(preview.targets.size()) +
            (preview.targets.size() == 1 ? " object matches" : " objects match")
        : std::string(action->id) + " " + std::to_string(preview.targets.size()) +
            (preview.targets.size() == 1 ? " object" : " objects");
    if (!preview.resolved.target.asset.empty())
    {
        preview.summary += " -> " + preview.resolved.target.asset;
    }
    preview.executable = true;
    return preview;
}

bool ExecuteIntent(const EditorActionContext& actionCtx,
    EditorCommandStack& commandStack,
    const EditorIntentPreview& preview,
    std::string& outStatus)
{
    if (!preview.executable)
    {
        outStatus = preview.problem.empty() ? "Nothing to run" : preview.problem;
        return false;
    }

    const EditorIntent& intent = preview.resolved;
    const bool ran = RunEditorAction(actionCtx, commandStack, intent, preview.targets, outStatus);

    LOG_INFO(logging::LogCategory::Editor,
        "intent [{}] action={} scope={} targets={} asset={} -> {}",
        intent.sourceLabel.empty() ? "?" : intent.sourceLabel,
        intent.action,
        intent.target.scope == EditorIntentScope::Selected ? "selected" : "all",
        preview.targets.size(),
        intent.target.asset.empty() ? "-" : intent.target.asset,
        outStatus);
    return ran;
}

#endif // WITH_EDITOR
