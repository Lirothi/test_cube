#pragma once
#if WITH_EDITOR

#include <cstddef>
#include <string>
#include <vector>

#include "editor/intent/EditorActionRegistry.h"
#include "editor/intent/EditorIntent.h"
#include "editor/scene/EditorSceneDocument.h"

class EditorCommandStack;

// What the user is shown BEFORE anything happens (docs/editor_llm_plan.md, E6).
//
// The preview is not politeness. Taking "every vertex under the surface" literally
// already sank a 15 m palm by 15 m during development; the same literalism across 621
// objects with no preview is a lost level. So the intent names what it understood and
// how many objects it reaches, and a human presses the button.
struct EditorIntentPreview
{
    struct Group
    {
        std::string label;      // asset name when the objects have one, else the type
        std::size_t count = 0;
    };

    bool executable = false;
    // "bury 247 objects" -- the action and the size of the blast radius.
    std::string summary;
    // "coconut_palm x183, date_palm x64" -- WHAT it is about to touch. This is the
    // line that catches a filter which matched the wrong family of assets.
    std::vector<Group> groups;
    std::vector<EditorObjectId> targets;
    // Why it cannot run, when it cannot. Never empty while executable is false.
    std::string problem;
    // True when running this leaves one entry on the undo stack; false for a
    // selection-only action, which must not be advertised as undoable.
    bool undoable = false;
    // True when this was a question. There is nothing to run: `summary` and `groups` ARE
    // the answer, and the panel must not offer a button that does nothing.
    bool answerOnly = false;
    // True when running this only moves the camera. Not undoable, and the panel must say
    // which of the several not-undoable things it is.
    bool viewOnly = false;
    // The intent with defaults filled in and shapes broadened by ValidateParams, plus
    // the asset resolved to a concrete registry key. THIS is what must be executed --
    // running the raw intent would skip the checks the preview was based on.
    EditorIntent resolved;
};

// The objects a target names: the outliner's search predicate over `filter`, then the
// `exclude` list, then the spatial narrowing. `target` is taken by reference because
// resolving it can REWRITE it -- "the selected zone" means the level narrowed by that
// zone, not the zone itself -- and the caller must act on what was actually resolved.
//
// Public because the preview is no longer the only caller: a query answers questions
// about the same sets ("how big is what I would be deleting"), and a second copy of this
// would be a second opinion about which objects a phrase reaches.
void ResolveTargetObjects(const EditorActionContext& actionCtx,
    EditorIntentTarget& target,
    std::vector<EditorObjectId>& outTargets,
    std::vector<EditorIntentPreview::Group>& outGroups);

// Resolve the intent's target -- existing objects through the outliner's own search
// predicate (E5), or an asset through the AssetRegistry -- validate its parameters
// against the action's declared list, and describe what would happen. Touches nothing.
EditorIntentPreview BuildIntentPreview(const EditorActionContext& actionCtx,
    const EditorIntent& intent);

// Execute a previewed intent through the command stack, as ONE history entry.
// `outStatus` always carries a user-facing sentence.
bool ExecuteIntent(const EditorActionContext& actionCtx,
    EditorCommandStack& commandStack,
    const EditorIntentPreview& preview,
    std::string& outStatus);

#endif // WITH_EDITOR
