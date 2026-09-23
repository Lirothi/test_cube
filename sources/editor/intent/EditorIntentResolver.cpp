#include "editor/intent/EditorIntentResolver.h"
#if WITH_EDITOR
#include "editor/scene/EditorZone.h"
#endif
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
        // THE IDENTITY, NOT THE LABEL, whatever the name of this function suggests. It is
        // used twice and the two uses pull opposite ways: the preview's breakdown wants
        // something readable, and `filterFromSelection` -- "select the ones like this" --
        // feeds the result straight back in as a FILTER, matched against the object's asset
        // path. Making this pretty broke that instantly: the filter became "Coconut Palm",
        // which matches nothing, and the gate said "No object in the level matches".
        //
        // So this stays the path. The breakdown tidies it at the point of display.
        return editormatch::AssetLabel(object);
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
        if (!where.zone.empty())
        {
            // Resolved per call rather than cached: the zone is an ordinary object and can
            // be dragged between one phrase and the next, and an answer computed against
            // where it USED to be would be wrong in the way hardest to notice.
            editorzone::Zone zone;
            std::string whyNot;
            if (!editorzone::Find(ctx.document, where.zone, zone, whyNot))
            {
                // An unresolvable zone must exclude everything, not pass everything: a
                // typo'd name turning "delete the palms in zone Beach" into "delete the
                // palms" is the one failure this layer must never have. The preview then
                // reports nothing matched, and nothing runs.
                return false;
            }
            // A CLOSED spline means the area it encloses when we are FINDING things, whatever
            // its Plant setting says. That setting answers "where do new objects go", and
            // for a ribbon along a coastline it is right; but asked which palms are "in zone
            // test", nobody means the ones within the band's half-width of the line. The
            // screenshot that produced this: a closed loop around half an island, half width
            // 0.2 m, and "удали пальмы в зоне test" deleted one -- the only palm that
            // happened to stand within 20 cm of the curve.
            editorzone::Zone forQuery = zone;
            if (forQuery.shape == editorzone::Shape::Spline && forQuery.closed)
            {
                forQuery.fill = editorzone::Fill::Inside;
            }
            if (!editorzone::Contains(forQuery, position))
            {
                return false;
            }
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

// "IN THE SELECTED ZONE" IS A PLACE, NOT A SET. Selecting a zone and saying "randomise
// the palms in the selected zone" produced "Nothing in the selection matches": scope
// `selected` means "among the selected objects", the selection held one zone, and a
// zone is not a palm. But nobody selecting a region means the region itself -- they
// mean what is inside it.
//
// So when the selection is ZONES ONLY, the scope becomes the whole level narrowed by
// those zones. When it holds anything else the old reading stands, because then
// "selected" really is a set of things.
//
// Done for EVERY verb, before the target kind is looked at. This used to live inside the
// object path, which `spawn` never enters -- so "разбросай по выделенной зоне" arrived at
// spawn with no zone at all and scattered a disc around the camera, which looks like it
// worked. A rule about what a selection MEANS belongs to the intent, not to one branch.
void NormaliseSelectedZone(const EditorActionContext& actionCtx, EditorIntentTarget& target)
{
    const EditorContext& ctx = actionCtx.editor;
    if (target.scope == EditorIntentScope::Selected && target.where.zone.empty() &&
        !ctx.selection.Empty())
    {
        const EditorObject* onlyZone = nullptr;
        bool zonesOnly = true;
        for (const EditorObjectId id : ctx.selection.Ordered())
        {
            const EditorObject* object = ctx.document.Find(id);
            if (!object || object->type != editorzone::kTypeName)
            {
                zonesOnly = false;
                break;
            }
            // More than one selected zone has no single answer -- "in the selected zone" is
            // singular -- so the old reading is left alone rather than picking one.
            if (onlyZone)
            {
                zonesOnly = false;
                break;
            }
            onlyZone = object;
        }
        if (zonesOnly && onlyZone)
        {
            target.where.zone = onlyZone->name;
            target.scope = EditorIntentScope::All;
        }
    }
}

void ResolveTargetObjects(const EditorActionContext& actionCtx,
    EditorIntentTarget& target,
    std::vector<EditorObjectId>& outTargets,
    std::vector<EditorIntentPreview::Group>& outGroups)
{
    const EditorContext& ctx = actionCtx.editor;

    // An unfiltered request is the one that may take environment entities as they are:
    // they carry no searchable asset and no transform, so a filter can never reach them.
    const bool saysNothingAboutWhich =
        target.filter.empty() && target.exclude.empty() && !target.where.Any();

    const auto consider = [&](const EditorObject& object)
    {
        // A ZONE IS A PLACE, AND A PLACE IS NOT ONE OF THE THINGS IN IT. "удали всё в
        // выделенной зоне" resolves to the whole level narrowed by that zone -- and a
        // circle's centre is inside itself, so the zone object landed in its own target
        // list and was deleted along with the palms. The region then could not be reused,
        // which is the opposite of what someone drawing a region wants.
        //
        // Only an UNFILTERED phrase is affected. "удали зону Beach" names it, so the filter
        // matches below and the zone is a perfectly ordinary target.
        if (object.type == editorzone::kTypeName && target.filter.empty())
        {
            return;
        }
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
        outTargets.push_back(object.id);
        AppendGroup(outGroups, editormatch::PrettyAssetLabel(object));
    };

    if (target.scope == EditorIntentScope::Selected)
    {
        for (const EditorObjectId id : ctx.selection.Ordered())
        {
            const EditorObject* object = ctx.document.Find(id);
            if (!object)
            {
                if (saysNothingAboutWhich)
                {
                    outTargets.push_back(id);
                    AppendGroup(outGroups, "environment");
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

    // Before the target kind is branched on, so every verb gets the same reading of what a
    // selected zone means -- including the ones that never reach the object path.
    NormaliseSelectedZone(actionCtx, preview.resolved.target);

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

    // `replace` is the verb with TWO nouns: it selects existing objects AND names the asset
    // they should become. Which verbs those are is DECLARED, not inferred from whether an
    // asset happens to be present -- the grammar permits `asset` inside any target, and a
    // stray one on a delete used to change the verdict.
    if (action->target == EditorTargetKind::Asset || action->takesDestinationAsset)
    {
        if (intent.target.asset.empty())
        {
            preview.problem = "'" + std::string(action->id) + "' needs an asset to place";
            return preview;
        }
        // EVERY asset the intent names, not just the first one. `spawn` plants from
        // `target.assets` and the reader mirrors a lone `asset` into that list, so ALL of
        // them arrive populated -- while this resolved only the singular and described only
        // the singular. The multi-asset shape the prompt teaches by example ("20 palms of
        // different types") therefore previewed as "spawn up to 20 x Coconut Palm" and then
        // planted seven each of three kinds. Someone pressed Run on a sentence naming one.
        //
        // Resolving the whole list here also collapses the second half of that bug: the
        // preview used a substring search with an exact-match tie-break while the builder
        // used an exact lookup, so a name the preview rescued could still fail at Run. The
        // builder is handed resolved keys now and never sees the phrase.
        // Both spellings, mirrored HERE rather than in one source's reader. The model's
        // reader mirrors them; the hand-written grammar source writes only `asset`, so a
        // loop over `assets` alone silently refused every grammar-built spawn -- which the
        // gate caught, because the gate drives the grammar. The resolver is the one place
        // both roads pass through, so it is the place the two spellings become one.
        std::vector<std::string> named = intent.target.assets;
        if (named.empty() && !intent.target.asset.empty())
        {
            named.push_back(intent.target.asset);
        }

        std::vector<std::string> resolvedAssets;
        std::vector<const EditorAssetRecord*> records;
        for (const std::string& name : named)
        {
            std::string assetProblem;
            const EditorAssetRecord* record = ResolveAsset(actionCtx.assets, name, assetProblem);
            if (!record)
            {
                preview.problem = assetProblem;
                return preview;
            }
            if (std::find(resolvedAssets.begin(), resolvedAssets.end(), record->id.key) ==
                resolvedAssets.end())
            {
                resolvedAssets.push_back(record->id.key);
                records.push_back(record);
            }
        }
        if (resolvedAssets.empty())
        {
            preview.problem = "'" + std::string(action->id) + "' needs an asset to place";
            return preview;
        }
        // Downstream works with the resolved keys, never the phrase the user typed.
        preview.resolved.target.assets = resolvedAssets;
        preview.resolved.target.asset = resolvedAssets.front();

        if (action->target == EditorTargetKind::Asset)
        {
            const int count =
                std::clamp(static_cast<int>(preview.resolved.params.value("count", 1.0f)), 1, 200);
            // The count is the TOTAL, shared between the kinds -- which is what the builder
            // does with it, and what the prompt says. Splitting it here so the preview adds
            // up to the same number keeps the two telling one story.
            const std::size_t kinds = records.size();
            std::size_t assigned = 0;
            for (std::size_t i = 0; i < kinds; ++i)
            {
                const std::size_t share = i + 1 == kinds
                    ? static_cast<std::size_t>(count) - assigned
                    : static_cast<std::size_t>(count) / kinds;
                assigned += share;
                preview.groups.push_back({ records[i]->displayName, share });
            }
            // "up to", because the ground decides: water, cliffs and whatever is already
            // standing there can refuse a spot, and the run reports what actually landed.
            preview.summary = std::string(action->id) + " up to " + std::to_string(count) +
                " x " + (kinds == 1 ? records.front()->displayName
                                    : std::to_string(kinds) + " kinds");
            preview.executable = true;
            return preview;
        }
    }

    if (action->target == EditorTargetKind::None)
    {
        preview.summary = std::string(action->id);
        // `place` carries its objects in a list, and how many is what the button will do.
        if (preview.resolved.params.is_object() && preview.resolved.params.contains("items") &&
            preview.resolved.params["items"].is_array())
        {
            const std::size_t count = preview.resolved.params["items"].size();
            preview.summary += " " + std::to_string(count) + (count == 1 ? " object" : " objects");
        }
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

    // A NEGATIVE RADIUS IS A REFUSAL, NOT A ZERO. `Any()` counts the anchor, so a `where`
    // with an anchor and radius -50 looks like a narrowing and passes this guard -- and then
    // PassesSpatialFilter bails on `radius <= 0` and lets everything through. The limit
    // evaporates and the command quietly reaches the whole level, with only the count in the
    // preview to warn anybody. Zero is left alone: it is the "not set" default.
    if (preview.resolved.target.where.radius < 0.0f)
    {
        preview.problem = "A radius cannot be negative";
        return preview;
    }

    // An EMPTY filter with All scope would mean the whole level. No source is allowed to
    // produce it, and if one does the answer is "no" rather than "all". An `exclude` list
    // is a filter too -- "everything except the palms" discriminates, "everything" does not.
    const bool saysNothingAboutWhich = preview.resolved.target.filter.empty() &&
        preview.resolved.target.exclude.empty() && !preview.resolved.target.where.Any();
    // ...unless the action's whole point is everything. Organising the outliner is not the
    // same risk as deleting, and refusing it made the model narrow to the first filter it
    // could think of: "наведи порядок в аутлайнере" came back as one group of coconut
    // palms out of six hundred objects.
    const EditorActionDesc* wholeLevelDesc =
        EditorActionRegistry::Builtin().Find(preview.resolved.action);
    bool mayTakeEverything = wholeLevelDesc && wholeLevelDesc->wholeLevelIsFine;
    if (wholeLevelDesc && wholeLevelDesc->wholeLevelNeedsParam)
    {
        const auto it = preview.resolved.params.find(wholeLevelDesc->wholeLevelNeedsParam);
        mayTakeEverything = it != preview.resolved.params.end() &&
            it->is_boolean() && it->get<bool>();
    }
    if (preview.resolved.target.scope == EditorIntentScope::All && saysNothingAboutWhich &&
        !mayTakeEverything)
    {
        preview.problem = "Refusing to act on the whole level without a filter";
        return preview;
    }

    EditorIntentTarget& target = preview.resolved.target;
    ResolveTargetObjects(actionCtx, target, preview.targets, preview.groups);

    // A ZONE THAT DOES NOT RESOLVE IS A DIFFERENT ANSWER FROM AN EMPTY ONE, and until now
    // they were the same sentence. PassesSpatialFilter turns a failed lookup into "this
    // object is not inside", correctly -- a typo'd zone must exclude everything rather than
    // pass everything -- but that makes EVERY object fail, and the report came out as "No
    // object in the level matches": indistinguishable from there genuinely being no palms
    // there. The lookup's own reason says which it is, and it was being discarded.
    //
    // Checked after the resolve, not before, because the selected-zone rewrite inside it is
    // what puts a name in `where.zone` for "удали пальмы в выделенной зоне".
    if (preview.targets.empty() && !target.where.zone.empty())
    {
        editorzone::Zone zone;
        std::string whyNot;
        if (!editorzone::Find(actionCtx.editor.document, target.where.zone, zone, whyNot))
        {
            preview.problem = whyNot;
            return preview;
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
    // Only for the verb that actually turns them into it. The field can hold a stray asset
    // the grammar allowed into any target, and appending it read as a replace: "delete 183
    // objects -> models/coconut_palm.mesh.json" is a sentence about a different command.
    if (action->takesDestinationAsset && !preview.resolved.target.asset.empty())
    {
        preview.summary += " -> " + preview.resolved.target.asset;
    }
    preview.executable = true;

    // An action that touches fewer things than its target names gets the last word on what
    // the line above says. Only `thin` uses it so far; the generic summary counts what the
    // filter reached, which for a thinning verb is the wrong number in front of the button.
    if (action->refinePreview)
    {
        action->refinePreview(actionCtx, preview.resolved, preview);
    }
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
