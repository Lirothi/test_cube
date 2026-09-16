#pragma once
#if WITH_EDITOR

#include <memory>
#include <string>
#include <vector>

#include "editor/intent/EditorActionRegistry.h"
#include "editor/intent/EditorIntent.h"
#include "editor/intent/EditorIntentResolver.h"
#include "editor/intent/EditorIntentSource.h"
#include "editor/intent/LlmIntentSource.h"

struct EditorContext;
class AssetRegistry;
class EditorCommandStack;
class EditorExtensionRegistry;
class IEditorIntentSource;

// Type a phrase, see exactly what it would do, then press the button
// (docs/editor_llm_plan.md, E6). The panel owns no editing logic of its own: it asks
// the intent sources for an EditorIntent, asks the resolver what that intent reaches,
// and hands the result to the shared action dispatcher. Everything it can do is
// reachable from the menus too -- what it adds is saying it in one line.
class CommandBarPanel
{
public:
    CommandBarPanel();
    ~CommandBarPanel();

    // `buryDepthPercent` comes from Level Editor > Placement, so a phrase and the End
    // key bury by the same amount instead of two settings drifting apart.
    void Draw(EditorContext& ctx,
        EditorCommandStack& commandStack,
        const AssetRegistry& assets,
        const EditorExtensionRegistry& extensions,
        float buryDepthPercent,
        bool* open);

    // True when at least one intent source can answer. When this is false the editor
    // does not register the panel at all: a command bar that parses nothing is an
    // inert control, and an inert control lies about what the editor can do (E8).
    bool AnySourceAvailable() const;

    // The level changed: any per-level grammar and prompt must be rebuilt.
    void OnLevelChanged();

    // --- headless (`--intent=<phrase>`) ---------------------------------------------
    // The same pipeline the panel drives, without the panel. Submit once, then poll each
    // frame until it settles; the verdict is a sentence describing what was understood and
    // what it reaches. `execute` runs it, otherwise it is a dry run.
    enum class HeadlessState { Idle, Pending, Done };
    // Fill the bar with a phrase and parse it, exactly as if it had been typed and Enter
    // pressed. Used by the chat panel when the model decides a message was an edit rather
    // than a question. NOT headless: the preview appears and waits to be confirmed, which
    // is the difference between handing something over and doing it behind someone's back.
    // Phrase history, for the editor to persist alongside the rest of the panel state.
    // Newest last. `TakeHistoryDirty` reports (and clears) whether it changed since the
    // last save, so the state file is not rewritten every frame.
    const std::vector<std::string>& PhraseHistory() const { return phraseHistory_; }
    void SetPhraseHistory(std::vector<std::string> history);
    bool TakeHistoryDirty();

    void SubmitPhrase(const EditorActionContext& actionCtx,
        const std::string& phrase,
        float buryDepthPercent);

    void BeginHeadless(const EditorActionContext& actionCtx,
        const std::string& phrase,
        float buryDepthPercent);
    HeadlessState PollHeadless(const EditorActionContext& actionCtx,
        EditorCommandStack& commandStack,
        bool execute,
        std::string& outVerdict);

    // The chat panel talks to the SAME model and the same server. One 38 GB load, one
    // job-owned process, one idle timer.
    LlmIntentSource& Model() { return *model_; }

    const LlmIntentSettings& ModelSettings() const;
    void SetModelSettings(const LlmIntentSettings& settings);

private:
    void Begin(const EditorActionContext& actionCtx, float buryDepthPercent);
    void PollSources(const EditorActionContext& actionCtx);
    void FinishIntent(const EditorActionContext& actionCtx);
    void ClearResult();
    void RememberPhrase(const std::string& phrase);
    // Record what came back. `phrase` empty attaches the line to the newest exchange, which
    // is how a preview later becomes "ran" without becoming a second entry.
    struct Exchange
    {
        std::string phrase;
        std::string verdict;
        // Colour-coded by outcome, because at a glance the difference between "ran" and
        // "cannot run" matters more than the words.
        enum class Kind { Ran, Preview, Refused, Failed } kind = Kind::Preview;
    };
    void RecordOutcome(const std::string& verdict, Exchange::Kind kind);
    // Up/Down while the box has focus. Returns true when it changed the text, so the caller
    // knows to move the caret to the end rather than leaving it mid-phrase.
    bool WalkHistory(int direction);
    void ClearThread();
    void DrawSettings();

    std::vector<std::unique_ptr<IEditorIntentSource>> sources_;
    LlmIntentSource* model_ = nullptr;   // owned by sources_, kept for the settings UI

    // Grows as it is typed into (editorui::InputText). 512 bytes was about 250 Cyrillic
    // characters, and a phrase naming several assets and a spatial filter reaches that.
    std::string input_;
    // The exchange, as it happened: what was typed and what came back. The bar used to show
    // only the newest result, so the answer to the previous phrase vanished the moment the
    // next one was typed -- and with a preview that needs confirming, the thing you were
    // deciding about disappeared as soon as you asked anything else.
    std::vector<Exchange> transcript_;
    bool transcriptScrollToBottom_ = false;
    // Everything typed here, newest LAST, deduplicated against the previous entry and
    // capped. Walked with Up/Down in the box the way a shell's history is, and persisted in
    // editor_state.json -- a phrase that took three tries to word is worth more than the
    // session it was worded in, and retyping it from memory is where the wording drifts.
    std::vector<std::string> phraseHistory_;
    // Where Up/Down currently is. Size() means "at the live line, below the history".
    std::size_t historyCursor_ = 0;
    // What was in the box before Up was first pressed, so Down all the way back restores it
    // instead of losing a half-typed phrase.
    std::string historyDraft_;
    bool historyDirty_ = false;
    std::string phrase_;
    // The exchanges that led to the phrase being typed now. Only ever non-empty after the
    // model answered `unclear`: it asked which palms, and this is where the answer goes so
    // the whole request does not have to be retyped. Cleared the moment a command lands.
    std::vector<IntentTurn> history_;
    // Index into sources_ of the one currently being asked. Sources are tried in cost
    // order, so this walks forward as cheaper ones decline (E7).
    std::size_t activeSource_ = 0;
    bool waiting_ = false;

    EditorIntent intent_;
    EditorIntentPreview preview_;
    std::string status_;
    std::string whyNot_;
    float buryDepthPercent_ = 1.0f;
    bool hasResult_ = false;
    bool focusInput_ = false;
    bool headless_ = false;
    bool showSettings_ = false;
    char modelPathBuffer_[512] = {};
    char serverExeBuffer_[512] = {};
    char endpointBuffer_[128] = {};
};

#endif // WITH_EDITOR
