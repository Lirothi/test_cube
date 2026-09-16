#pragma once
#if WITH_EDITOR

#include <string>
#include <string_view>
#include <vector>

#include "editor/intent/EditorIntent.h"

class AssetRegistry;
class EditorSceneDocument;

// Everything a source needs to know about what it is parsing against: the level's own
// objects (so a filter can only name things that are there) and the asset registry (so
// `spawn` can only name assets that exist). Both are what the GBNF grammar is built
// from -- see IntentSchema.
struct EditorIntentWorld
{
    const EditorSceneDocument& document;
    const AssetRegistry& assets;
};

// One exchange already had with the model about the CURRENT request. This is the whole
// of the conversation support, and it is deliberately that small: the `unclear` branch
// asks a question, and without somewhere to put the answer the user has to retype the
// original phrase with the missing detail wedged in. It is a dialogue about ONE command,
// not a chat -- the preview and the single undo entry depend on the request staying one.
struct IntentTurn
{
    std::string user;
    std::string assistant;   // the model's raw JSON answer
};

enum class IntentParseState
{
    Idle,
    // Still working. The frame must not wait for this -- inference takes seconds.
    Pending,
    // An intent is available.
    Ready,
    // This source does not recognise the phrase; try the next one.
    Declined,
    // The source itself failed (no server, bad response). Carries a reason.
    Failed,
};

// Anything that turns a typed phrase into an EditorIntent. There are two: the
// hand-written grammar (instant, exact formulations only) and the local model (slow,
// understands what the words MEAN). The editor asks the grammar first and only pays for
// the model when the grammar could not answer -- E7.
//
// THE INTERFACE IS ASYNCHRONOUS BECAUSE ONE IMPLEMENTATION IS SLOW. A second of
// inference inside Draw is a frozen editor, so Begin starts the work and Poll is called
// each frame until it settles. The grammar simply settles on the first Poll, which costs
// it nothing and keeps one code path in the panel instead of two.
class IEditorIntentSource
{
public:
    virtual ~IEditorIntentSource() = default;

    virtual std::string_view Name() const = 0;

    // True when this source can answer at all right now. A source that answers nothing
    // must report false so the UI can drop the affordance rather than present a control
    // that does nothing (E8).
    virtual bool Available() const = 0;

    // One line about why it is unavailable, for the panel to show instead of the source.
    virtual std::string UnavailableReason() const { return {}; }

    // Start parsing `phrase`. Any previous request is abandoned. `history` carries the
    // exchanges that led here, so an answer to "which palms?" is read as an answer.
    virtual void Begin(const std::string& phrase,
        const EditorIntentWorld& world,
        const std::vector<IntentTurn>& history) = 0;

    // Non-blocking. On Ready fills `outIntent`; on Declined/Failed fills `outWhyNot`.
    virtual IntentParseState Poll(EditorIntent& outIntent, std::string& outWhyNot) = 0;

    // Abandon the in-flight request, if any.
    virtual void Cancel() {}

    // Called when the level changed, so a source that caches per-level state (the
    // generated grammar, the system prompt) rebuilds it.
    virtual void InvalidateWorld() {}
};

#endif // WITH_EDITOR
