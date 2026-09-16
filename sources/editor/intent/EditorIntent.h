#pragma once
#if WITH_EDITOR

#include <string>
#include <vector>

#include "core/math/Math.h"
#include "editor/scene/EditorSceneDocument.h" // nlohmann::json

// The one boundary between "a phrase the user typed" and "something the editor does"
// (docs/editor_llm_plan.md, E1). Everything downstream -- the selector resolve, the
// preview, the command stack -- knows only this struct and never learns whether a
// local model or a hand-written parser produced it. That is what lets the model be
// removed whole while the feature keeps working.
//
// WHY THE TARGET IS A STRUCT AND NOT A LIST OF NEEDLES. The first version of this
// file said `{action, filter, scope, params}`, which encodes exactly one shape of
// sentence: "a verb over existing objects found by name". The very next request --
// "plant ten new palms" -- broke it, because the noun there is an ASSET that may not
// be in the level at all. It was not the only one waiting:
//
//     "plant ten new palms"          the noun is an asset, not level objects
//     "replace the palms with X"     TWO nouns: a selector AND an asset
//     "make the fog thicker"         the noun is a subsystem field; no objects at all
//     "palms within 50 m of camera"  the filter is SPATIAL, not a substring
//     "scale between 0.8 and 1.2"    the parameter is a RANGE, not a scalar
//     "raise by 2 metres"            the value is RELATIVE, not absolute
//     "hide everything except palms" the filter NEGATES
//
// So the slots are named once, here, and an action declares which of them it consumes.
// A new verb is then an entry in the action registry, not another edit to this file.

enum class EditorIntentKind
{
    None,
    // A concrete editor action. `action` names a registry entry, `target` says what it
    // points at, `params` carries its arguments.
    Command,
    // The editor cannot do what was asked, and the model says so instead of reaching
    // for the nearest implemented action. This branch EXECUTES NOTHING by definition
    // -- its maximum effect is a log line and a sentence on screen (E3.1).
    NeedsApi,
    // Understood as a request, but the target or a parameter is genuinely ambiguous.
    Unclear,
};

// What the words of the sentence point AT, which is a different question from what the
// verb does to it. Each needs a different resolve path and a different source of truth.
enum class EditorTargetKind
{
    // Objects already in the level. Resolved against the document through the
    // outliner's own search predicate (E5).
    Objects,
    // An asset to instantiate or switch to. Resolved against the ASSET REGISTRY, not
    // the document: an asset that has never been placed in this level is still a
    // perfectly good thing to ask for.
    Asset,
    // A named engine setting (`fog.density`, `sky.*`, `ocean.*` -- the ~270 keys the
    // engine already exposes through `--set`). Declared here so the shape is nailed
    // down; no registry action consumes it yet, and the resolver refuses it explicitly
    // rather than pretending.
    Environment,
    // The action needs no noun.
    None,
};

enum class EditorIntentScope
{
    All,        // everything in the level that passes the filter
    Selected,   // only what is already selected, further narrowed by the filter
};

enum class EditorSpatialAnchor
{
    None,
    Camera,      // where the camera is / what it is looking at
    Selection,   // the centre of the current selection
    Point,       // an explicit world position
};

// Narrowing by PLACE rather than by name: "the palms near the camera", "everything
// below the waterline". Empty by default, and an empty one passes everything.
struct EditorSpatialFilter
{
    EditorSpatialAnchor anchor = EditorSpatialAnchor::None;
    Math::float3 point{ 0.0f, 0.0f, 0.0f };
    // Straight-line distance from the anchor. Zero or less means "no radius limit".
    float radius = 0.0f;
    bool  hasMinY = false;
    float minY = 0.0f;
    bool  hasMaxY = false;
    float maxY = 0.0f;

    bool Any() const
    {
        return anchor != EditorSpatialAnchor::None || radius > 0.0f || hasMinY || hasMaxY;
    }
};

struct EditorIntentTarget
{
    EditorTargetKind kind = EditorTargetKind::Objects;

    // Needles matched against name / type / id / asset properties. An object matching
    // ANY needle is in. These are ASSET and TYPE names, not a re-typed English phrase:
    // "coconut_palm", never "the palm trees".
    std::vector<std::string> filter;
    // Needles that take an object back OUT again -- "everything except the palms".
    std::vector<std::string> exclude;

    EditorIntentScope scope = EditorIntentScope::All;
    EditorSpatialFilter where;

    // kind == Asset: what to create. Also the DESTINATION asset of `replace`, whose
    // target is Objects -- that verb is the one with two nouns.
    std::string asset;
    // Every asset the intent names, `asset` included, so a consumer never has to check two
    // fields. More than one only means something to `spawn`: planting a mix of palms had no
    // way to be said at all while the target held a single asset, and the model -- reading
    // its own schema correctly -- refused the phrase rather than dropping half of it.
    std::vector<std::string> assets;
    // kind == Environment: the `--set` key. Not consumed yet; see EditorTargetKind.
    std::string setting;
};

struct EditorIntent
{
    EditorIntentKind kind = EditorIntentKind::None;

    // Registry action id. Never free text: a source may only emit ids that exist.
    std::string action;
    EditorIntentTarget target;
    // Action arguments, shaped and type-checked by the registry's parameter list.
    // Numbers, booleans, strings, two-element ranges `[lo, hi]` and three-element
    // vectors, plus the `relative` flag verbs like `move` and `scale` declare.
    nlohmann::json params = nlohmann::json::object();

    // --- kind == NeedsApi ------------------------------------------------------
    std::string requested;            // what the user asked for, in their words
    std::string proposed;             // the API that would be needed, e.g. "setBaseColor(objects, rgba)"
    std::string whyExistingDontFit;   // which existing actions were considered and rejected

    // --- kind == Unclear -------------------------------------------------------
    std::string question;

    // Which source produced this ("grammar" or "llm"). Shown in the preview and
    // logged, so a surprising result can be traced to the right half of the system.
    std::string sourceLabel;
};

#endif // WITH_EDITOR
