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
    // Not an editing request at all -- a greeting, a question about graphics, a complaint
    // about how something looks. The editor answers it in PROSE, in the same window, by
    // asking the same model again without the grammar.
    //
    // It is a value in the closed set rather than a guess made outside it, so the decision
    // is made by the thing that understands the sentence, and made once. The cost is about
    // a second: this answer is twenty tokens, and only then does a conversational turn
    // begin. A phrase that IS a command never pays it.
    Chat,
    // A question to the EDITOR, not to the user: "how big is the island", "where is the
    // waterline". Executes nothing and changes nothing -- the editor answers, the answer
    // is appended to the conversation, and the model gets another turn to act on it.
    //
    // This branch exists because the prompt cannot carry the scene. It is built when the
    // level loads and must stay byte-identical for the server's prefix cache to hold, so
    // anything that changes while editing cannot live in it. Asking is the way to see
    // something current, and it costs a whole extra generation -- which is why the query
    // list is short and the loop is capped.
    Query,
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
    // A named zone. The other fields describe a place in numbers the model has to guess;
    // this one names a region somebody drew, which is the difference between "удали пальмы
    // в радиусе 50" and "удали пальмы в зоне Beach". Only `spawn` could take a zone at
    // first, so everything else -- delete, select, count -- had no way to say where.
    std::string zone;

    bool Any() const
    {
        return anchor != EditorSpatialAnchor::None || radius > 0.0f || hasMinY || hasMaxY ||
            !zone.empty();
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

// One question inside a query answer. It carries the same target and params a command
// would: "the bounds of the palms in zone Beach" narrows exactly the way "delete the palms
// in zone Beach" does, and a second dialect for saying WHICH would be a second thing to
// keep in step.
struct EditorIntentQuery
{
    // An id from the query list. Closed like `action` is, and for the same reason: a
    // question the editor cannot answer must be impossible to ask, not answered wrongly.
    std::string query;
    EditorIntentTarget target;
    nlohmann::json params = nlohmann::json::object();
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

    // --- kind == Query ---------------------------------------------------------
    // Everything being asked in this one turn. ALWAYS A LIST, even for one question.
    //
    // A list because a round trip is the expensive part and a question is not: wanting the
    // waterline AND the island's extent is one thought, and making it two turns charged the
    // designer twice for it. Checking the ground at five points was five turns, which is
    // most of a minute for an answer the editor computes in microseconds.
    //
    // ALWAYS a list, with no singular spelling beside it, because this codebase has already
    // learned that lesson once: `asset` and `assets` both worked, every example used the
    // singular, and the plural went unused for weeks while the model refused phrases it had
    // the schema to express.
    std::vector<EditorIntentQuery> asks;

    // Which source produced this ("grammar" or "llm"). Shown in the preview and
    // logged, so a surprising result can be traced to the right half of the system.
    std::string sourceLabel;
};

#endif // WITH_EDITOR
