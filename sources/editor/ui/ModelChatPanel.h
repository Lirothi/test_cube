#pragma once
#if WITH_EDITOR

#include <string>
#include <cstddef>
#include <vector>

#include "editor/intent/LlmIntentSource.h"

// A plain conversation with the local model. No grammar, no action registry, no preview,
// nothing to run -- just talk.
//
// It exists BESIDE the Command Bar rather than inside it because the two are different
// things and pretending otherwise would spoil both. The Command Bar is one phrase, one
// constrained answer, one undoable command, and a preview between them; that shape is what
// makes it safe to point at 600 objects. A chat has no target, no preview and nothing to
// undo. Merging them would either put a Run button on a conversation or a conversation
// behind a Run button.
//
// It shares the Command Bar's model and its server: a second one would mean a second
// 38 GB load. Chatting also counts as using the model, so the idle timer will not retire
// the server in the middle of a conversation.
class ModelChatPanel
{
public:
    void Draw(LlmIntentSource& model, bool* open);

    // "--chat=<phrase>": put a phrase in the box and send it, so a headless screenshot can
    // catch the panel mid-answer. The streamed reply is the one thing here that no gate can
    // look at -- it exists only while it is arriving.
    // False while the server is still loading: the phrase is kept and the caller retries,
    // exactly as a person would press Send again.
    bool SendHeadless(LlmIntentSource& model, const std::string& phrase);

    // What the editor currently has open. Without it the model answers editor questions as
    // if it were a chatbot in a browser: asked to "выдели все пальмы" it wrote PSEUDOCODE
    // for a SceneManager it invented, because nothing told it that a level was loaded or
    // that the Command Bar two windows over does exactly that. Set every frame; cheap, and
    // it only reaches the prompt as a few hundred characters that the prefill cache keeps.
    void SetEditorContext(std::string levelPath, std::size_t objectCount);

    // A phrase the model decided belongs in the Command Bar, taken exactly once. The chat
    // deliberately cannot execute anything itself: it hands the phrase over and the Command
    // Bar does what it always does -- resolve it, show what would change, and wait to be
    // told to go. An editing path with no preview and no undo is the one thing this whole
    // layer exists to avoid.
    std::string TakePendingCommand();

private:
    struct Turn
    {
        bool fromUser = false;
        std::string text;
        // What this turn contributes to the next prompt, when that differs from what is
        // shown. It differs for exactly one reason, and it matters: a routed message is
        // DISPLAYED as "-> Command Bar: ..." and if that also went into the history the
        // model would read it as its own previous answer and imitate it -- which is what
        // happened, and why an ordinary "как дела твои?" ended up routed, twice, with the
        // same stale phrase both times. Empty means `text` is used.
        std::string promptText;
        // Reasoning models answer with a <think> block first. It is genuinely interesting
        // and genuinely in the way, so it is kept apart and shown folded.
        std::string thinking;
        // The answer stopped at the token budget rather than finishing.
        bool truncated = false;
        // Part of the search round trip -- either the model's request or the output it
        // got back. Shown folded: the conversation is what the person reads, and the
        // plumbing underneath it is available rather than in the way.
        bool toolRequest = false;
        // A phrase the model offered to run but did not run. The turn keeps it so the
        // button stays with the sentence that proposed it -- an offer that scrolls away
        // from its own answer is an offer nobody connects to anything.
        std::string suggestion;
    };

    void Send(LlmIntentSource& model);
    // Builds the prompt and, if the conversation no longer fits, drops the OLDEST
    // exchanges until it does -- reporting how many went. A chat has no natural end, so
    // without this it grows until the server refuses the request, which would surface as
    // an unexplained failure several turns after the actual cause.
    std::string BuildPrompt(const LlmIntentSettings& settings, std::size_t& outDropped) const;

    std::vector<Turn> turns_;
    // Grows as it is typed into (editorui::InputTextMultiline). A fixed buffer here meant
    // the box quietly stopped taking keys at ~1000 Cyrillic characters, which is nothing
    // next to the model's context.
    std::string input_;
    std::string error_;
    // How many oldest exchanges have been dropped to stay inside the context, cumulative.
    std::size_t droppedTurns_ = 0;
    std::string serverStatus_;
    bool waiting_ = false;
    bool focusInput_ = true;
    bool scrollToBottom_ = false;
    // Length of the streamed answer at the last frame, so the log follows the stream
    // without stealing the scrollbar from someone reading further up.
    std::size_t lastPartialSize_ = 0;
    // Sent as the system message. Editable, because the useful thing about having a
    // general model in the editor is asking it things the editor knows nothing about.
    std::string system_ =
        "You are a helpful assistant embedded in a DirectX 12 game engine's level editor. "
        "Answer concisely, in the language the user writes in.";
    float temperature_ = 0.7f;
    // OFF, and that is a speed decision with a visible switch rather than a quiet one.
    // This model reasons before it answers and the reasoning is most of the work: "привет"
    // spent 1278 characters thinking to produce 44 characters of answer -- 97% of the
    // tokens, and every one of them is real seconds. The command bar has always suppressed
    // it (a grammar is waiting for JSON there); the chat now offers the same choice.
    // Turn it on for anything where the working is the interesting part.
    bool reasoning_ = false;
    // Reading this engine's own source, so "how does the ocean work here" is answered
    // about THIS engine instead of about game engines in general. On by default: the whole
    // reason for a local model sitting inside the editor is that it can see the editor.
    bool searchEnabled_ = true;
    // Search rounds spent on the message being answered now. See the cap's reasoning where
    // it is enforced.
    int toolRounds_ = 0;
    static constexpr int kMaxToolRounds = 3;
    std::string levelPath_;
    std::size_t objectCount_ = 0;
    std::string pendingCommand_;
    // Generous because this model THINKS before it answers and the reasoning comes out of
    // the same allowance: at 800 the whole budget went on thinking and the answer never
    // arrived. The command bar does not have that problem -- a grammar is waiting for JSON
    // there, so reasoning is suppressed.
    //
    // This is a TIME budget, not a limit anyone imposes: at the ~28 tok/s measured here,
    // 8192 tokens is about five minutes of generation. The panel shows the estimate beside
    // the slider, computed from the rate the last answer actually ran at.
    // The model's whole slider range, because n_predict is a CEILING and not a target: the
    // model still stops at EOS, so a short answer costs exactly what it costs. 8192 was the
    // old default and its only effect was to cut off the long answer that actually needed
    // the room. The request's deadline is derived from this number in BeginFreeform, so the
    // top of the range is now reachable instead of guaranteed to time out.
    int maxTokens_ = 32768;
};

#endif // WITH_EDITOR
