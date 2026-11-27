#include "editor/intent/EditorActionBuilders.h"
#if WITH_EDITOR

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/JsonField.h" // R9.6d: Value for the document's fields
#include "editor/EditorContext.h"
#include "editor/commands/EditObjectPropertiesCommand.h"
#include "editor/commands/DeleteObjectCommand.h"
#include "editor/commands/DuplicateObjectCommand.h"
#include "editor/commands/EditEnvironmentCommand.h"
#include "editor/commands/RenameObjectCommand.h"
#include "editor/commands/SetEnabledCommand.h"
#include "editor/commands/SetMeshAssetCommand.h"
#include "rendering/meshes/Mesh.h"
#include "editor/EditorObjectMatch.h"
#include "editor/intent/EditorIntentResolver.h"

// R10.3: the editor's actions -- select-and-change actions on objects: delete, duplicate, show / hide, group, rename, isolate, replace.
// Cut out of EditorActionRegistry.cpp; the table that names them is still there (EditorActionBuilders.h).

namespace editoractions
{
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

    // A document object carries `enabled` as a field, an environment entity in its properties.
    bool IsEnabled(const EditorSceneDocument& document, EditorObjectId id)
    {
        if (const EditorObject* object = document.Find(id))
        {
            return object->enabled;
        }
        const EditorObject* environment = FindEnvironmentObject(document, id);
        return environment ? jsonfield::Value(environment->properties, "enabled", true) : true; // R9.6d: the document's
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
            // Already in the asked-for state: nothing to do and nothing to undo. Not only tidy --
            // an environment edit that changes nothing refuses to run, and one refusal rolls the
            // whole fold back, so showing a selection that held an already-lit sun undid the rest
            // of it (the Outliner's eye runs this on the whole selection).
            if (IsEnabled(actionCtx.editor.document, id) == enabled)
            {
                continue;
            }
            commands.push_back(std::move(command));
        }
        if (commands.empty())
        {
            outStatus = enabled ? "Already shown" : "Already hidden";
            return nullptr;
        }

        const std::size_t changed = commands.size();
        outStatus = (enabled ? "Enabled " : "Disabled ") + CountedObjects(changed);
        return FoldIntoOneEntry(std::move(commands),
            std::string(enabled ? "Enable " : "Disable ") + std::to_string(changed) + " Objects");
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

        // ONE GROUP PER KIND, when asked for that. "Группы для каждого типа пальм" is a
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
        // PER ASSET: name each one after WHAT IT IS. "назови объекты нормальными именами"
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
}

#endif // WITH_EDITOR
