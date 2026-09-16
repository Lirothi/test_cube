#pragma once
#if WITH_EDITOR

#include <functional>
#include <string>

// Read-only search over this repository, for the CHAT to use.
//
// WHY THE CHAT AND NOT THE COMMAND BAR. The command bar answers in about three seconds,
// constrained by a grammar, against a prompt whose prefix must stay byte-identical for the
// server's cache to hold. Searching would break all three: it costs a whole extra turn, the
// results cannot live in the cached prefix, and a file is not a command. The chat has none
// of those constraints -- it is a conversation where half a minute is ordinary -- and
// without source access it answers "how does the ocean work here" with general facts about
// game engines rather than anything about THIS engine.
//
// Everything here is read-only and confined to the repository. There is no write, no
// delete, no shell, and a path that resolves outside the repo is refused rather than
// clamped: the model is choosing these strings, and a rule that quietly reinterprets a bad
// one is a rule nobody can reason about.
//
// THE PROTOCOL IS A LINE OF TEXT, not a function-calling API, because the model reaching it
// is sampled freely -- no grammar is waiting. A line the panel does not understand is just
// a line of conversation, which is the right failure.
namespace reposearch
{
    // The block appended to the chat's system prompt when search is on. Describes the three
    // tools and, deliberately, their limits -- a tool whose caps the model cannot see is a
    // tool it wastes turns on.
    const char* ProtocolPrompt();

    // Execute every `TOOL:` line in `answer`, in order, and write a report of what they
    // returned. False when the answer asked for no tools -- which is most turns.
    //
    // Budgeted: at most a few calls per turn, capped output per call. The caps are not
    // politeness. A tool result is prefill on this turn and on every turn after it, so an
    // unbounded grep poisons the rest of the conversation as well as this answer.

    // Answers a `TOOL: scene ...` line. Supplied by the caller rather than implemented
    // here: this module knows about FILES, and what is in the level is a question only the
    // editor's live document can answer. Empty when the caller has no editor to ask.
    using SceneQueryFn = std::function<std::string(const std::string& rest)>;

    bool RunRequestedTools(const std::string& answer,
        const SceneQueryFn& scene,
        std::string& outReport);

    // Remove any `TOOL:` line from text that is about to be SHOWN. They are a protocol
    // between the model and the panel, and one that was never run is not a sentence: when
    // the lookup budget ran out mid-question the model's next request became the visible
    // answer, and the person was shown a grep command where a reply should have been.
    // Lives here because what a TOOL line looks like is this module's business.
    void StripToolLines(std::string& text);
}

#endif // WITH_EDITOR
