#pragma once
#if WITH_EDITOR

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "editor/intent/EditorIntent.h"
#include "editor/intent/EnvironmentSettings.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObjectId, nlohmann::json

class AssetRegistry;
class EditorCommand;
// Forward-declared rather than included: EditorIntentResolver.h includes THIS header, and
// refinePreview below only needs to name the type in a function-pointer signature.
struct EditorIntentPreview;
class EditorCommandStack;
class EditorExtensionRegistry;
struct EditorContext;

// The editor's action vocabulary, in ONE place (docs/editor_llm_plan.md, E3.1).
//
// This exists because the natural-language command bar has to reason about the
// BOUNDARIES of the API -- it must be able to answer "there is no such action"
// rather than reach for the nearest thing that happens to be implemented. That is
// only possible if the set of actions, their human-readable descriptions and their
// execution all come from the same list: the GBNF grammar that constrains the model,
// the action descriptions in its prompt, and the dispatcher below are all built from
// this registry. Adding an action is one entry here, not three edits that drift.
//
// The DESCRIPTIONS are not documentation garnish. A model handed bare names guesses
// the semantics from the spelling and confuses `bury` (push it into the ground) with
// "hide it" (`setEnabled false`); the description is what stops that.
//
// THE PARAMETERS ARE TYPED FOR THE SAME REASON THE NAMES ARE. E3 guarantees an action
// name cannot be invented, because the grammar lists them literally. Nothing guaranteed
// the same for arguments, and a `count` of "ten" or a range where a scalar belongs is
// just as wrong as an invented verb -- only it fails later, half-applied. So every
// parameter declares its kind, and ValidateParams refuses an intent that does not fit
// before anything is previewed, let alone executed.

enum class EditorParamKind
{
    Number,
    Bool,
    String,
    Range,      // [lo, hi]
    Vec3,       // [x, y, z]; a lone number is broadened to (n, n, n)
    Enum,       // one of `values`
    // Whatever the thing being edited happens to be. Used where a LATER check knows the
    // real type and can say something better: `setEnvironment`'s value is a number, a
    // boolean, text or a vector depending on which setting was named, and
    // ComputeEnvironmentValue rejects a mismatch by naming the setting and its type --
    // which beats a generic "must be a number" from here.
    Any,
};

struct EditorActionParam
{
    std::string_view name;
    EditorParamKind kind = EditorParamKind::Number;
    bool required = false;
    std::string_view description;
    // Enum only: the permitted spellings.
    std::vector<std::string_view> values;
};

enum class EditorActionEffect
{
    // Produces an undoable EditorCommand. One phrase becomes one history entry, so
    // Ctrl+Z reverses the whole thing (E6).
    DocumentEdit,
    // Changes only what is selected. Not a document edit and deliberately NOT a
    // history entry -- the command bar must not promise Ctrl+Z for it.
    SelectionOnly,
    // Answers a question and changes nothing at all. "How many palms are there" needs no
    // command behind it: resolving the target set IS the answer, and the preview already
    // computes exactly that -- the count and the breakdown by asset. So a query is a
    // preview with nothing to run, and it needs no new intent kind, no new resolve path
    // and no Run button.
    ReadOnly,
    // Moves the camera and nothing else. Not a document edit -- where the user happens to
    // be looking is not part of the level -- so it is not undoable and must not claim to be.
    ViewChange,
};

// Everything an action may need besides the document. Passed by reference so adding a
// service later does not touch every build function's signature again.
struct EditorActionContext
{
    EditorContext& editor;
    const AssetRegistry& assets;
    const EditorExtensionRegistry& extensions;
};

struct EditorActionDesc
{
    // Builds the command. `targets` is the resolved object set for actions whose target
    // is Objects, and is empty for the ones that create from an asset -- those read what
    // to build from `intent.target.asset`. Returns nullptr when nothing can be done;
    // `outStatus` always carries the user-facing sentence, on success as well.
    using BuildFn = std::unique_ptr<EditorCommand> (*)(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);

    std::string_view id;
    std::string_view description;
    EditorActionEffect effect = EditorActionEffect::DocumentEdit;
    EditorTargetKind target = EditorTargetKind::Objects;
    std::vector<EditorActionParam> params;
    BuildFn build = nullptr;      // null for SelectionOnly, ReadOnly and ViewChange
    // The filter comes from what is SELECTED rather than from the phrase. "Select the ones
    // like this" is answerable only by the editor: the model never sees the selection, and
    // putting it in the prompt would break the prefill cache every time it changed.
    //
    // LAST on purpose, so every existing entry keeps its aggregate initialiser and takes
    // the default -- adding a field in the middle would silently shift every one of them.
    bool filterFromSelection = false;

    // Rewrite the preview for an action that touches FEWER things than its target names.
    //
    // The generic summary is "<action> N objects", where N is what the filter reached, and
    // for nearly every verb that is also what it does. `thin` is the exception that made
    // this necessary: asked to thin a hundred palms it removes perhaps thirty, and a
    // preview reading "thin 100 objects" put the wrong number in front of the button --
    // true about the target, wrong about the consequence, which is the kind of honest-
    // looking line nobody double-checks.
    //
    // Runs after the targets resolve and before the preview is called executable. It may
    // refuse, by clearing `executable` and setting `problem`.
    using RefineFn = void (*)(const EditorActionContext& actionCtx,
        const EditorIntent& intent,
        EditorIntentPreview& preview);
    RefineFn refinePreview = nullptr;

    // This action acts on OBJECTS and also names an asset they become. `replace` is the only
    // verb with two nouns, and the resolver needs to know which those are rather than
    // guessing from whether an asset happens to be present.
    //
    // It used to guess: any Objects action carrying an `asset` entered asset resolution,
    // and the grammar lets `asset` appear in any target. A delete that picked one up was
    // then refused outright when the name did not resolve, or -- worse when it did --
    // previewed as "delete 183 objects -> models/coconut_palm.mesh.json", which any reader
    // takes for a replace.
    bool takesDestinationAsset = false;
};

class EditorActionRegistry
{
public:
    static const EditorActionRegistry& Builtin();

    const EditorActionDesc* Find(std::string_view id) const;
    const std::vector<EditorActionDesc>& Actions() const { return actions_; }

private:
    EditorActionRegistry();

    std::vector<EditorActionDesc> actions_;
};

// Works out what an environment setting BECOMES: an absolute `value`, or `scale` times
// what it is now. Shared with the resolver so the preview shows the same number the run
// will write -- a preview computed by different arithmetic is not a preview.
bool ComputeEnvironmentValue(const envsettings::Setting& setting,
    const nlohmann::json& params,
    nlohmann::json& outValue,
    std::string& outError);

// Type-checks `params` against the action's declared parameter list and fills in the
// broadenings the declaration allows (a lone number where a Vec3 or Range is expected).
// Returns false with a reason when a required parameter is missing or a value has the
// wrong shape. Runs BEFORE the preview, so a malformed intent never reaches a command.
bool ValidateParams(const EditorActionDesc& action, nlohmann::json& params, std::string& outError);

// The one dispatcher. Builds the action's command and pushes it through the stack, so
// every path -- menu item, hotkey, typed phrase -- lands in the same history.
// `outStatus` is filled either way. Returns true when the action took effect.
bool RunEditorAction(const EditorActionContext& actionCtx,
    EditorCommandStack& commandStack,
    const EditorIntent& intent,
    const std::vector<EditorObjectId>& targets,
    std::string& outStatus);

// Convenience for the selection-driven call sites (menu items, hotkeys) that have no
// intent object: builds a minimal Objects intent for `actionId` with `params`.
EditorIntent MakeActionIntent(std::string_view actionId, nlohmann::json params = nlohmann::json::object());

#endif // WITH_EDITOR
