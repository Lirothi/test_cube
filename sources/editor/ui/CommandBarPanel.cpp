#include "editor/ui/CommandBarPanel.h"
#if WITH_EDITOR

#include <cstring>
#include <string>

#include "core/logging/Log.h"
#include "editor/EditorContext.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/intent/EditorIntentSource.h"
#include "editor/intent/EditorSceneQuery.h"
#include "editor/intent/GrammarIntentSource.h"
#include "editor/ui/ImGuiTextInput.h"
#include "imgui.h"
// ClearActiveID: an input keeps its own copy of the text while active, and emptying
// the bound string behind its back leaves the old text on screen.
#include "imgui_internal.h"

namespace
{
    const char* ParamKindName(EditorParamKind kind)
    {
        switch (kind)
        {
        case EditorParamKind::Number: return "number";
        case EditorParamKind::Bool:   return "true/false";
        case EditorParamKind::String: return "text";
        case EditorParamKind::Range:  return "[low, high]";
        case EditorParamKind::Vec3:   return "[x, y, z]";
        case EditorParamKind::Enum:   return "one of";
        case EditorParamKind::Any:    return "value";
        }
        return "?";
    }

    const char* TargetKindName(EditorTargetKind kind)
    {
        switch (kind)
        {
        case EditorTargetKind::Objects:     return "objects in the level";
        case EditorTargetKind::Asset:       return "an asset to create from";
        case EditorTargetKind::Environment: return "an engine setting";
        case EditorTargetKind::None:        return "nothing";
        }
        return "?";
    }

    void CopyToBuffer(char* buffer, std::size_t size, const std::string& text)
    {
        const std::size_t count = text.size() < size - 1 ? text.size() : size - 1;
        std::memcpy(buffer, text.data(), count);
        buffer[count] = '\0';
    }
}

CommandBarPanel::CommandBarPanel()
{
    // Order is the cost order: the grammar answers exact formulations for free, and only
    // what it declines is worth waking a model for (E7). A phrase like "bury coconut_palm"
    // never reaches the model at all.
    sources_.push_back(std::make_unique<GrammarIntentSource>());

    auto model = std::make_unique<LlmIntentSource>(LlmIntentSettings{});
    model_ = model.get();
    sources_.push_back(std::move(model));

    CopyToBuffer(endpointBuffer_, sizeof(endpointBuffer_), model_->Settings().endpoint);
}

CommandBarPanel::~CommandBarPanel() = default;

bool CommandBarPanel::AnySourceAvailable() const
{
    for (const std::unique_ptr<IEditorIntentSource>& source : sources_)
    {
        if (source && source->Available())
        {
            return true;
        }
    }
    return false;
}

void CommandBarPanel::OnLevelChanged()
{
    for (std::unique_ptr<IEditorIntentSource>& source : sources_)
    {
        if (source)
        {
            source->InvalidateWorld();
        }
    }
}

const LlmIntentSettings& CommandBarPanel::ModelSettings() const
{
    return model_->Settings();
}

void CommandBarPanel::SetModelSettings(const LlmIntentSettings& settings)
{
    model_->SetSettings(settings);
    CopyToBuffer(modelPathBuffer_, sizeof(modelPathBuffer_), settings.modelPath);
    CopyToBuffer(serverExeBuffer_, sizeof(serverExeBuffer_), settings.serverExe);
    CopyToBuffer(endpointBuffer_, sizeof(endpointBuffer_), settings.endpoint);
}

void CommandBarPanel::ClearThread()
{
    history_.clear();
    assistRounds_ = 0;
}

void CommandBarPanel::SetPhraseHistory(std::vector<std::string> history)
{
    phraseHistory_ = std::move(history);
    historyCursor_ = phraseHistory_.size();
    historyDirty_ = false;
}

bool CommandBarPanel::TakeHistoryDirty()
{
    const bool dirty = historyDirty_;
    historyDirty_ = false;
    return dirty;
}

void CommandBarPanel::RememberPhrase(const std::string& phrase)
{
    // The same phrase twice in a row is one entry: re-running something after an undo is
    // the commonest thing anyone does here, and it would otherwise fill the history with
    // copies of itself and push the interesting older ones off the end.
    constexpr std::size_t kMaxHistory = 100;
    if (phraseHistory_.empty() || phraseHistory_.back() != phrase)
    {
        phraseHistory_.push_back(phrase);
        if (phraseHistory_.size() > kMaxHistory)
        {
            phraseHistory_.erase(phraseHistory_.begin(),
                phraseHistory_.begin() + (phraseHistory_.size() - kMaxHistory));
        }
        historyDirty_ = true;
    }
    historyCursor_ = phraseHistory_.size();
    historyDraft_.clear();
}

bool CommandBarPanel::WalkHistory(int direction)
{
    if (phraseHistory_.empty())
    {
        return false;
    }
    if (direction < 0)   // Up: older
    {
        if (historyCursor_ == 0)
        {
            return false;
        }
        if (historyCursor_ == phraseHistory_.size())
        {
            historyDraft_ = input_;   // keep whatever was half-typed
        }
        --historyCursor_;
        input_ = phraseHistory_[historyCursor_];
        return true;
    }
    // Down: newer, and one step past the newest returns the draft rather than an empty box.
    if (historyCursor_ >= phraseHistory_.size())
    {
        return false;
    }
    ++historyCursor_;
    input_ = historyCursor_ == phraseHistory_.size()
        ? historyDraft_ : phraseHistory_[historyCursor_];
    return true;
}

void CommandBarPanel::RecordOutcome(const std::string& verdict, Exchange::Kind kind)
{
    if (transcript_.empty() || verdict.empty())
    {
        return;
    }
    transcript_.back().verdict = verdict;
    transcript_.back().kind = kind;
    transcriptScrollToBottom_ = true;
}

void CommandBarPanel::ClearResult()
{
    intent_ = EditorIntent{};
    preview_ = EditorIntentPreview{};
    hasResult_ = false;
    whyNot_.clear();
}

void CommandBarPanel::Begin(const EditorActionContext& actionCtx, float buryDepthPercent)
{
    for (std::unique_ptr<IEditorIntentSource>& source : sources_)
    {
        if (source)
        {
            source->Cancel();
        }
    }
    ClearResult();
    status_.clear();
    buryDepthPercent_ = buryDepthPercent;

    phrase_ = input_;
    sendPhrase_ = phrase_;
    assistRounds_ = 0;
    if (phrase_.find_first_not_of(" \t\r\n") == std::string::npos)
    {
        waiting_ = false;
        return;
    }

    // EVERY phrase, logged once, here -- the single point all three roads pass through:
    // typed, handed over by the chat, or driven by the harness. The log used to name a
    // phrase only when it FAILED (unparsed, or needs_api), so a session's successful work
    // was invisible: the log showed "action=select ... 610 objects" with no record of the
    // sentence that asked for it, which is the half worth reading back.
    LOG_INFO(logging::LogCategory::Editor, "command bar: \"{}\"", phrase_);
    RememberPhrase(phrase_);
    constexpr std::size_t kMaxTranscript = 200;
    transcript_.push_back({ phrase_, "thinking...", Exchange::Kind::Preview });
    if (transcript_.size() > kMaxTranscript)
    {
        transcript_.erase(transcript_.begin(),
            transcript_.begin() + (transcript_.size() - kMaxTranscript));
    }
    transcriptScrollToBottom_ = true;

    const EditorIntentWorld world{ actionCtx.editor.document, actionCtx.assets };
    activeSource_ = 0;
    waiting_ = true;
    while (activeSource_ < sources_.size() &&
        (!sources_[activeSource_] || !sources_[activeSource_]->Available()))
    {
        ++activeSource_;
    }
    if (activeSource_ >= sources_.size())
    {
        waiting_ = false;
        whyNot_ = "No intent source is available.";
        return;
    }
    sources_[activeSource_]->Begin(sendPhrase_, world, history_);
}

void CommandBarPanel::PollSources(const EditorActionContext& actionCtx)
{
    if (!waiting_)
    {
        return;
    }

    const EditorIntentWorld world{ actionCtx.editor.document, actionCtx.assets };
    while (activeSource_ < sources_.size())
    {
        IEditorIntentSource* source = sources_[activeSource_].get();
        if (!source || !source->Available())
        {
            ++activeSource_;
            continue;
        }

        EditorIntent intent;
        std::string whyNot;
        const IntentParseState state = source->Poll(intent, whyNot);
        if (state == IntentParseState::Pending)
        {
            return;   // still thinking; the frame goes on without it
        }
        if (state == IntentParseState::Ready)
        {
            // A QUESTION FROM THE GRAMMAR IS NOT AN ANSWER, it is the cheap source saying it
            // cannot tell. Treated as Ready it ended the thread at the first source: the
            // model -- which understands what the words MEAN and would have answered, or
            // asked something better -- was never consulted, and `FinishIntent` dropped the
            // turn entirely, because its Unclear branch is guarded on the LLM source. The
            // transcript line kept the "thinking..." placeholder forever.
            //
            // The question is kept as the fallback reason, so if nobody after this can
            // answer either, the user reads "Randomize what -- rotation or scale?" rather
            // than a generic shrug.
            if (intent.kind == EditorIntentKind::Unclear && intent.sourceLabel != "llm")
            {
                if (!intent.question.empty())
                {
                    whyNot_ = intent.question;
                }
                ++activeSource_;
                while (activeSource_ < sources_.size() &&
                    (!sources_[activeSource_] || !sources_[activeSource_]->Available()))
                {
                    ++activeSource_;
                }
                if (activeSource_ < sources_.size())
                {
                    sources_[activeSource_]->Begin(sendPhrase_, world, history_);
                    return;
                }
                waiting_ = false;
                RecordOutcome(whyNot_, Exchange::Kind::Refused);
                return;
            }
            intent_ = std::move(intent);
            hasResult_ = true;
            waiting_ = false;
            FinishIntent(actionCtx);
            return;
        }
        if (state == IntentParseState::Failed)
        {
            // A source that BROKE is not a source that declined: stop rather than quietly
            // falling through to a weaker answer the user did not ask for.
            waiting_ = false;
            whyNot_ = whyNot.empty() ? "The local model could not answer." : whyNot;
            RecordOutcome(whyNot_, Exchange::Kind::Failed);
            return;
        }

        // Declined: remember why, in case nobody can answer, and try the next one.
        if (!whyNot.empty())
        {
            whyNot_ = whyNot;
        }
        ++activeSource_;
        while (activeSource_ < sources_.size() &&
            (!sources_[activeSource_] || !sources_[activeSource_]->Available()))
        {
            ++activeSource_;
        }
        if (activeSource_ < sources_.size())
        {
            sources_[activeSource_]->Begin(sendPhrase_, world, history_);
            return;   // give it a frame; the model answers on a later poll
        }
    }

    waiting_ = false;
    if (!hasResult_)
    {
        if (whyNot_.empty())
        {
            whyNot_ = "No intent source understood that.";
        }
        RecordOutcome(whyNot_, Exchange::Kind::Failed);
        LOG_INFO(logging::LogCategory::Editor, "command bar: unparsed phrase \"{}\" ({})",
            phrase_, whyNot_);
    }
}

void CommandBarPanel::FinishIntent(const EditorActionContext& actionCtx)
{
    // A QUESTION TO THE EDITOR, ANSWERED HERE AND HANDED STRAIGHT BACK. The designer is not
    // asked anything: they typed a sentence and are waiting for it to happen, and stopping
    // to say "the island is 355 m wide, carry on?" would be an interruption with no decision
    // in it. So the editor answers, the answer becomes the next user turn, and the model
    // gets another go at the ORIGINAL phrase -- which is why `phrase_` is left alone and
    // only `sendPhrase_` moves.
    if (intent_.kind == EditorIntentKind::Query && intent_.sourceLabel == "llm")
    {
        const std::string answer = editorquery::Answer(actionCtx, intent_);
        std::string asked;
        for (const EditorIntentQuery& ask : intent_.asks)
        {
            asked += (asked.empty() ? "" : ", ") + ask.query;
        }
        LOG_INFO(logging::LogCategory::Editor,
            "command bar: query [{}] -> {}", asked, answer);

        if (assistRounds_ >= kMaxAssistRounds)
        {
            // Out of rounds. The answer still goes in the log, because a model that spent
            // three turns asking was probably asking something worth reading.
            ClearThread();
            RecordOutcome("asked " + std::to_string(kMaxAssistRounds) +
                " times without deciding; last answer was: " + answer,
                Exchange::Kind::Refused);
            return;
        }
        ++assistRounds_;

        history_.push_back({ sendPhrase_, model_->LastRawAnswer() });
        sendPhrase_ = "EDITOR ANSWERS: " + answer +
            "\n\nNow answer the original request: \"" + phrase_ + "\"";

        RecordOutcome("asked the editor -- " + asked + ": " + answer, Exchange::Kind::Asked);
        transcript_.push_back({ phrase_, "thinking...", Exchange::Kind::Preview });
        transcriptScrollToBottom_ = true;

        const EditorIntentWorld world{ actionCtx.editor.document, actionCtx.assets };
        ClearResult();
        waiting_ = true;
        sources_[activeSource_]->Begin(sendPhrase_, world, history_);
        return;
    }

    if (intent_.kind == EditorIntentKind::Unclear && intent_.sourceLabel == "llm")
    {
        // Remember the exchange so the next thing typed is read as an ANSWER. Without
        // this the question is a dead end: the user would have to retype the original
        // phrase with the missing detail wedged into it.
        history_.push_back({ phrase_, model_->LastRawAnswer() });
        RecordOutcome(intent_.question.empty() ? std::string("needs a clearer phrase")
                                               : intent_.question, Exchange::Kind::Refused);
        return;
    }
    // ClearThread belongs in each terminal branch, NOT here. It used to sit at this point,
    // which meant the history and the round budget were already gone by the time the
    // command's preview was built -- and the preview is exactly where the last round of
    // the conversation can still be spent usefully.
    if (intent_.kind == EditorIntentKind::NeedsApi)
    {
        ClearThread();
        // The backlog entry. Over a week of use these lines are the honest list of what
        // was actually wanted from the editor, sorted by how often it was asked for --
        // which is the list worth extending the action registry from (E3.1).
        LOG_INFO(logging::LogCategory::Editor,
            "command bar: NO SUCH ACTION for \"{}\" | requested={} | proposed={} | rejected={}",
            phrase_, intent_.requested, intent_.proposed, intent_.whyExistingDontFit);
        RecordOutcome("the editor cannot do this: " + intent_.whyExistingDontFit,
            Exchange::Kind::Refused);
        return;
    }
    if (intent_.kind != EditorIntentKind::Command)
    {
        ClearThread();
        return;
    }

    // One tuned depth for both roads into bury.
    if (intent_.action == "bury" && !intent_.params.contains("depthPercent"))
    {
        intent_.params["depthPercent"] = buryDepthPercent_;
    }
    preview_ = BuildIntentPreview(actionCtx, intent_);

    // THE MODEL NEVER SAW WHAT ITS COMMAND WOULD DO. The preview was computed, shown to the
    // designer and thrown away -- so a command the editor refused ("No mesh asset matches
    // 'palm'", "'delete' has no parameter 'zone'") ended the thread with a sentence only a
    // human could act on, while the one participant who could rewrite the command was told
    // nothing. The model asked for this channel itself, in those words, when it was shown
    // its own query list.
    //
    // ONLY ON A REFUSAL. Feeding every successful preview back would charge a whole extra
    // generation for every phrase, to tell the model something it already predicted; a
    // refusal is the case where it has something to learn and a reason to try again.
    //
    // Drawn from the same budget as the queries, deliberately: the number the designer
    // cares about is how many turns they wait, not what the editor spent them on.
    if (!preview_.executable && !preview_.answerOnly && intent_.sourceLabel == "llm" &&
        assistRounds_ < kMaxAssistRounds && !preview_.problem.empty())
    {
        ++assistRounds_;
        history_.push_back({ sendPhrase_, model_->LastRawAnswer() });
        sendPhrase_ = "THE EDITOR REFUSED THAT COMMAND: " + preview_.problem +
            "\n\nThat is the editor's own words, not a guess. Fix the command and answer the "
            "original request again: \"" + phrase_ + "\". If it cannot be fixed with the "
            "actions you have, say needs_api.";
        LOG_INFO(logging::LogCategory::Editor,
            "command bar: preview refused ({}), handing it back to the model", preview_.problem);
        RecordOutcome("the editor refused -- " + preview_.problem + " (asking the model to fix it)",
            Exchange::Kind::Asked);
        transcript_.push_back({ phrase_, "thinking...", Exchange::Kind::Preview });
        transcriptScrollToBottom_ = true;

        const EditorIntentWorld world{ actionCtx.editor.document, actionCtx.assets };
        ClearResult();
        waiting_ = true;
        sources_[activeSource_]->Begin(sendPhrase_, world, history_);
        return;
    }

    // The preview IS the outcome until something is run: a line saying what it would touch
    // is what the user is deciding about, and it has to survive the next phrase being typed.
    ClearThread();
    RecordOutcome(preview_.executable || preview_.answerOnly ? preview_.summary
                                                             : preview_.problem,
        preview_.executable || preview_.answerOnly ? Exchange::Kind::Preview
                                                   : Exchange::Kind::Failed);
}

void CommandBarPanel::BeginHeadless(const EditorActionContext& actionCtx,
    const std::string& phrase,
    float buryDepthPercent)
{
    // Deliberately goes through the same Begin the panel uses. A headless path that built
    // its own request would test a pipeline that does not ship, which is the mistake this
    // harness exists to stop making.
    input_ = phrase;
    headless_ = true;
    Begin(actionCtx, buryDepthPercent);
}

void CommandBarPanel::SubmitPhrase(const EditorActionContext& actionCtx,
    const std::string& phrase,
    float buryDepthPercent)
{
    input_ = phrase;
    headless_ = false;
    Begin(actionCtx, buryDepthPercent);
}

CommandBarPanel::HeadlessState CommandBarPanel::PollHeadless(const EditorActionContext& actionCtx,
    EditorCommandStack& commandStack,
    bool execute,
    std::string& outVerdict)
{
    if (!headless_)
    {
        return HeadlessState::Idle;
    }

    PollSources(actionCtx);
    if (waiting_)
    {
        return HeadlessState::Pending;
    }

    headless_ = false;
    input_.clear();

    if (!hasResult_)
    {
        // An action that changes nothing undoable runs the moment its preview resolves, and
        // clearing the result is part of that. Arriving here with a status but no result
        // means exactly that happened -- not that nothing was understood, which is what
        // this used to report and which made the harness call a successful selection a
        // failure. The oracle has to know the difference or it is not an oracle.
        if (!status_.empty())
        {
            outVerdict = "RAN: " + status_;
            return HeadlessState::Done;
        }
        outVerdict = "not understood: " + (whyNot_.empty() ? std::string("no source answered")
                                                          : whyNot_);
        return HeadlessState::Done;
    }

    switch (intent_.kind)
    {
    case EditorIntentKind::NeedsApi:
        outVerdict = "needs_api [" + intent_.sourceLabel + "] proposed=" + intent_.proposed +
            " because=" + intent_.whyExistingDontFit;
        return HeadlessState::Done;
    case EditorIntentKind::Unclear:
        outVerdict = "unclear [" + intent_.sourceLabel + "] question=" + intent_.question;
        return HeadlessState::Done;
    case EditorIntentKind::Command:
        break;
    default:
        outVerdict = "nothing to run";
        return HeadlessState::Done;
    }

    if (!preview_.executable)
    {
        outVerdict = "cannot run [" + intent_.sourceLabel + "] " + preview_.problem;
        return HeadlessState::Done;
    }

    std::string breakdown;
    for (const EditorIntentPreview::Group& group : preview_.groups)
    {
        if (!breakdown.empty())
        {
            breakdown += ", ";
        }
        breakdown += group.label + " x" + std::to_string(group.count);
    }

    outVerdict = "command [" + intent_.sourceLabel + "] action=" + intent_.action +
        " | " + preview_.summary;
    if (!breakdown.empty())
    {
        outVerdict += " | " + breakdown;
    }

    if (execute)
    {
        std::string status;
        const bool ran = ExecuteIntent(actionCtx, commandStack, preview_, status);
        outVerdict += std::string(" | ") + (ran ? "RAN: " : "REFUSED: ") + status;
    }
    else
    {
        // The default. Saying what a phrase WOULD do is the safe answer for something that
        // can reach six hundred objects, and it is the half a harness usually wants.
        outVerdict += " | dry run (pass --intent-run to execute)";
    }
    ClearResult();
    return HeadlessState::Done;
}

void CommandBarPanel::DrawSettings()
{
    if (!ImGui::CollapsingHeader("Local model"))
    {
        return;
    }

    LlmIntentSettings settings = model_->Settings();
    bool changed = false;

    ImGui::TextWrapped("%s", model_->StatusLine().c_str());
    // The command bar only ever asks this model one narrow, grammar-constrained question.
    // It is a capable general model underneath, and llama-server already ships a chat page
    // for it; pointing at that is more honest than pretending the command bar is a chat.
    const std::string webUi = model_->WebUiUrl();
    if (!webUi.empty())
    {
        ImGui::TextDisabled("Chat with it directly at %s", webUi.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy URL"))
        {
            ImGui::SetClipboardText(webUi.c_str());
        }
    }
    if (model_->OwnsRunningServer())
    {
        const double remaining = model_->SecondsUntilIdleStop();
        if (remaining >= 0.0)
        {
            ImGui::TextDisabled("Server stops in %ds if unused", static_cast<int>(remaining));
        }
        else
        {
            ImGui::TextDisabled("Server running (no idle timeout)");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop now"))
        {
            model_->StopServer();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("It also dies with the editor -- it runs inside a job object, "
                "so a crash or a kill takes it too. The model is memory-mapped, so while it "
                "is up the cost is page cache the OS can reclaim, not committed RAM.");
        }
    }
    if (!model_->Available())
    {
        ImGui::TextDisabled("Run: python tools/fetch_intent_model.py");
    }

    changed |= ImGui::Checkbox("Enabled", &settings.enabled);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Auto-start server", &settings.autoStart);
    if (ImGui::Checkbox("Chat page (restarts the server)", &settings.webUi))
    {
        changed = true;
    }
    ImGui::SetNextItemWidth(160.0f);
    changed |= ImGui::SliderInt("Idle stop (s)", &settings.idleTimeoutSeconds, 0, 600);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("0 = never. The chat page does NOT count as activity: the editor "
            "cannot see requests it did not make, so raise this while chatting.");
    }

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##modelPath", "path to .gguf",
            modelPathBuffer_, sizeof(modelPathBuffer_)))
    {
        settings.modelPath = modelPathBuffer_;
        changed = true;
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##serverExe", "path to llama-server.exe",
            serverExeBuffer_, sizeof(serverExeBuffer_)))
    {
        settings.serverExe = serverExeBuffer_;
        changed = true;
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputTextWithHint("endpoint", "127.0.0.1:8127",
            endpointBuffer_, sizeof(endpointBuffer_)))
    {
        settings.endpoint = endpointBuffer_;
        changed = true;
    }
    ImGui::SetNextItemWidth(160.0f);
    // 0 is CPU, which is the default: the renderer owns the GPU, and a phrase typed once
    // a minute does not need sub-100ms answers badly enough to take VRAM from it.
    changed |= ImGui::SliderInt("GPU layers", &settings.gpuLayers, 0, 99);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("0 = CPU only, and the renderer keeps all its VRAM. "
            "99 puts the dense layers on the GPU and leaves the experts in RAM. "
            "Measured here: 6.0s -> 3.5s per phrase, and 21.4s -> 17.1s for the first one "
            "after a restart (that first one is prompt prefill, not model loading).");
    }

    if (changed)
    {
        model_->SetSettings(settings);
    }

    if (!model_->Gbnf().empty())
    {
        ImGui::TextDisabled("grammar: %zu bytes, prompt: %zu bytes (generated from this level)",
            model_->Gbnf().size(), model_->SystemPrompt().size());
    }
}

void CommandBarPanel::Draw(EditorContext& ctx,
    EditorCommandStack& commandStack,
    const AssetRegistry& assets,
    const EditorExtensionRegistry& extensions,
    float buryDepthPercent,
    bool* open)
{
    const EditorActionContext actionCtx{ ctx, assets, extensions };

    // The model answers on its own thread; this is where its answer is picked up, and
    // where an idle server is retired.
    model_->Tick();
    PollSources(actionCtx);

    // An action that changes nothing undoable just runs. `undoable` is exactly that line:
    // true for the actions that edit the document, false for selecting, isolating, framing.
    // Asking someone to confirm "выдели все пальмы" is a dialog requesting permission to
    // highlight something -- they asked, nothing is at stake, and the next selection undoes
    // it anyway. This holds however the phrase arrived: routing it from the chat but making
    // it wait when typed here would be the same words behaving two different ways.
    // An actual EDIT still stops at the preview and waits for Run.
    if (hasResult_ && !waiting_)
    {
        if (preview_.executable && !preview_.undoable && !preview_.answerOnly)
        {
            ExecuteIntent(actionCtx, commandStack, preview_, status_);
            RecordOutcome(status_, Exchange::Kind::Ran);
            ClearResult();
            LOG_INFO(logging::LogCategory::Editor,
                "command bar: ran \"{}\" without asking (changes nothing undoable)", phrase_);
        }
    }

    // A first-use position, because without one every new window lands on the same default
    // spot and the first time both of these appear they are a pile on top of the outliner.
    // FirstUseEver only: once the user moves it, imgui.ini owns the placement.
    if (const ImGuiViewport* viewport = ImGui::GetMainViewport())
    {
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.30f,
                   viewport->WorkPos.y + 40.0f), ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Command Bar", open))
    {
        ImGui::End();
        return;
    }

    // THE LOG IS THE PANEL. Everything that happened, with the live preview as its newest
    // entry -- Run and Cancel included, so what is being decided about sits in the place it
    // was asked, not in a strip underneath. The input is pinned below it, the way the chat
    // does it, and the reserve is the input plus its button row and nothing else.
    const float reserve = ImGui::GetFrameHeightWithSpacing() * 2.6f;
    if (ImGui::BeginChild("##barLog", ImVec2(0.0f, -reserve), true))
    {
        for (std::size_t index = 0; index < transcript_.size(); ++index)
        {
            const Exchange& exchange = transcript_[index];
            ImGui::PushID(static_cast<int>(index));
            ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f), "you");
            ImGui::TextWrapped("%s", exchange.phrase.c_str());
            const ImVec4 colour =
                exchange.kind == Exchange::Kind::Ran ? ImVec4(0.65f, 0.95f, 0.65f, 1.0f) :
                exchange.kind == Exchange::Kind::Refused ? ImVec4(1.0f, 0.78f, 0.24f, 1.0f) :
                exchange.kind == Exchange::Kind::Failed ? ImVec4(1.0f, 0.55f, 0.55f, 1.0f) :
                // A query is the model looking something up, not a result. Blue keeps it
                // legible as a step along the way rather than an answer to the phrase.
                exchange.kind == Exchange::Kind::Asked ? ImVec4(0.58f, 0.78f, 1.0f, 1.0f) :
                ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::TextWrapped("%s", exchange.verdict.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::PopID();
        }

        if (waiting_)
        {
            // Said once, by the state line at the foot of the log.
        }
        else if (!whyNot_.empty() && !hasResult_)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.24f, 1.0f), "Not understood");
            ImGui::TextWrapped("%s", whyNot_.c_str());
        }
        else if (hasResult_ && intent_.kind == EditorIntentKind::NeedsApi)
        {
            // No Run button, because there is nothing to run. Offering one would be the lie
            // the three-branch answer exists to avoid.
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.24f, 1.0f), "No such action in the editor");
            if (!intent_.proposed.empty())
            {
                ImGui::TextWrapped("Would need: %s", intent_.proposed.c_str());
            }
            if (!intent_.whyExistingDontFit.empty())
            {
                ImGui::TextWrapped("Existing actions rejected because: %s",
                    intent_.whyExistingDontFit.c_str());
            }
            ImGui::TextDisabled("Logged to the session log as an API request.");
        }
        else if (hasResult_ && intent_.kind == EditorIntentKind::Unclear)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.24f, 1.0f), "Needs one more detail");
            ImGui::TextWrapped("%s", intent_.question.c_str());
            if (!history_.empty())
            {
                ImGui::TextDisabled("Type the answer -- the question is remembered.");
                ImGui::SameLine();
                if (ImGui::SmallButton("Start over"))
                {
                    ClearThread();
                    ClearResult();
                }
            }
        }
        else if (hasResult_ && intent_.kind == EditorIntentKind::Command)
        {
            if (preview_.executable)
            {
                ImGui::TextUnformatted(preview_.summary.c_str());
                std::string breakdown;
                for (const EditorIntentPreview::Group& group : preview_.groups)
                {
                    if (!breakdown.empty())
                    {
                        breakdown += ", ";
                    }
                    breakdown += group.label + " x" + std::to_string(group.count);
                }
                ImGui::TextWrapped("%s", breakdown.c_str());
                const char* note = " - not undoable";
                if (preview_.answerOnly)
                {
                    note = " - a question; nothing was changed";
                }
                else if (preview_.undoable)
                {
                    note = " - one Ctrl+Z undoes it";
                }
                else if (preview_.viewOnly)
                {
                    note = " - moves the camera only";
                }
                else
                {
                    note = " - selection only, not undoable";
                }
                ImGui::TextDisabled("via %s%s", intent_.sourceLabel.c_str(), note);

                if (preview_.answerOnly)
                {
                    // No Run button: the answer is already on screen, and a button that does
                    // nothing is exactly the inert control this panel exists not to have.
                    // Selecting what was just counted is the one thing worth offering next.
                    ImGui::BeginDisabled(preview_.targets.empty());
                    if (ImGui::Button("Select these"))
                    {
                        ctx.selection.SetOrdered(preview_.targets, preview_.targets.front());
                        status_ = "Selected " + std::to_string(preview_.targets.size()) +
                            (preview_.targets.size() == 1 ? " object" : " objects");
                        RecordOutcome(status_, Exchange::Kind::Ran);
                        ClearResult();
                        input_.clear();
                        ImGui::ClearActiveID();
                        focusInput_ = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::Button("Dismiss"))
                    {
                        ClearResult();
                    }
                }
                else
                {
                    if (ImGui::Button("Run"))
                    {
                        ExecuteIntent(actionCtx, commandStack, preview_, status_);
                        RecordOutcome(status_, Exchange::Kind::Ran);
                        ClearResult();
                        input_.clear();
                        ImGui::ClearActiveID();
                        focusInput_ = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel"))
                    {
                        ClearResult();
                    }
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.24f, 1.0f), "Cannot run");
                ImGui::TextWrapped("%s", preview_.problem.c_str());
            }
        }

        if (!status_.empty())
        {
            ImGui::TextWrapped("%s", status_.c_str());
        }
        // WHAT THE MODEL IS DOING, in the log where the answers are. This line existed only
        // inside the collapsed "Local model" fold, so the first minute of a session -- the
        // 22 s to put 38 GB on the card, then the prompt warming -- looked from here like an
        // editor ignoring the box. A phrase typed then is not lost, it is queued, and saying
        // so is the difference between waiting and wondering.
        {
            const std::string state = model_->StatusLine();
            const bool ready = state.rfind("Local model ready", 0) == 0 ||
                state == "Local model idle";
            if (!ready && !state.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.75f, 1.0f));
                ImGui::TextWrapped("%s", state.c_str());
                ImGui::PopStyleColor();
            }
            else if (waiting_)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.75f, 1.0f));
                ImGui::TextWrapped("Asking the local model. The editor keeps running.");
                ImGui::PopStyleColor();
            }
        }
        if (transcriptScrollToBottom_)
        {
            ImGui::SetScrollHereY(1.0f);
            transcriptScrollToBottom_ = false;
        }
    }
    ImGui::EndChild();

    if (focusInput_)
    {
        ImGui::SetKeyboardFocusHere();
        focusInput_ = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    const char* hint = !history_.empty() ? "answer the question above"
        : (model_->Available() ? "say what you want, in any language"
                               : "spawn 10 coconut_palm");
    // No InputTextWithHint here: the resizing callback belongs to editorui, and the hint is
    // drawn over the empty field instead so nothing is lost by dropping the variant.
    const ImVec2 inputPos = ImGui::GetCursorScreenPos();
    // Greyed out while the model is thinking, so a second Enter cannot queue a second
    // question behind the first. The server answers one request at a time, so the second
    // would not go faster -- it would cancel the first and start the wait again, which from
    // the outside looks like the editor ignoring the keypress.
    ImGui::BeginDisabled(waiting_);
    const bool submitted = editorui::InputText("##commandBarInput", input_,
        ImGuiInputTextFlags_EnterReturnsTrue) && !waiting_;
    ImGui::EndDisabled();
    // Up/Down walk the history, the way every prompt anyone has used since 1978 does.
    // Checked AFTER the widget so it owns the keys while it has focus, and the text is
    // replaced before the next frame draws it.
    if (ImGui::IsItemFocused())
    {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && WalkHistory(-1))
        {
            focusInput_ = true;
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && WalkHistory(1))
        {
            focusInput_ = true;
        }
    }
    if (input_.empty())
    {
        const ImVec2 padding = ImGui::GetStyle().FramePadding;
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(inputPos.x + padding.x, inputPos.y + padding.y),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
    }
    if (submitted)
    {
        Begin(actionCtx, buryDepthPercent);
        // Emptied on send, like the chat. The phrase is already the newest line in the log,
        // so leaving it in the box showed it twice and, worse, left the next phrase to be
        // typed on top of a failed one -- which is exactly when someone is retyping in a
        // hurry. Up recalls it if the retry wants the same words.
        input_.clear();
        ImGui::ClearActiveID();
        focusInput_ = true;
    }

    ImGui::BeginDisabled(input_.empty() || waiting_);
    if (ImGui::Button("Parse"))
    {
        Begin(actionCtx, buryDepthPercent);
        input_.clear();
        ImGui::ClearActiveID();
        focusInput_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        input_.clear();
        ImGui::ClearActiveID();
        ClearResult();
        status_.clear();
        waiting_ = false;
    }
    // The history, visible rather than only walkable. Up/Down is faster once you know what
    // is in there; this is how you find out, and how you pick something from twenty phrases
    // ago without pressing Up twenty times. Newest first, because that is what is wanted.
    //
    // Before "thinking...", so the button keeps its place instead of shifting along the row
    // every time a phrase is in flight.
    if (!phraseHistory_.empty())
    {
        ImGui::SameLine();
        // A plain Button, not SmallButton: it sits in a row with Parse and Clear, and a
        // SmallButton is shorter than they are, so the row read as misaligned.
        if (ImGui::Button("History"))
        {
            ImGui::OpenPopup("##commandBarHistory");
        }
        if (ImGui::BeginPopup("##commandBarHistory"))
        {
            ImGui::TextDisabled("%zu phrases  (Up/Down in the box walks these)",
                phraseHistory_.size());
            ImGui::Separator();
            for (std::size_t index = phraseHistory_.size(); index-- > 0;)
            {
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::Selectable(phraseHistory_[index].c_str()))
                {
                    input_ = phraseHistory_[index];
                    historyCursor_ = index;
                    focusInput_ = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::Selectable("Forget all"))
            {
                phraseHistory_.clear();
                historyCursor_ = 0;
                historyDirty_ = true;
            }
            ImGui::EndPopup();
        }
    }
    if (waiting_)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("thinking...");
    }



    ImGui::Separator();



    // The reference folds live at the BOTTOM. Above the log they pushed the conversation
    // down the panel, which is backwards: the newest exchange is what the panel is for, and
    // a syntax crib read once a week is not.
    if (ImGui::CollapsingHeader("Exact form (no model needed)"))
    {
        ImGui::TextDisabled("%s", GrammarIntentSource::HelpText().c_str());
    }
    DrawSettings();

    // The same list, word for word, that the model is handed as its action vocabulary.
    // Showing it here is not a help screen: it is the honest answer to "what can this box
    // actually do", and it is generated from the registry, so it cannot go stale.
    if (ImGui::CollapsingHeader("What the editor can do"))
    {
        for (const EditorActionDesc& action : EditorActionRegistry::Builtin().Actions())
        {
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextUnformatted(std::string(action.id).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", TargetKindName(action.target));
            ImGui::Indent();
            ImGui::TextWrapped("%s", std::string(action.description).c_str());
            for (const EditorActionParam& param : action.params)
            {
                ImGui::TextDisabled("%s%s %s: %s",
                    std::string(param.name).c_str(),
                    param.required ? "*" : "",
                    ParamKindName(param.kind),
                    std::string(param.description).c_str());
            }
            ImGui::Unindent();
        }
    }

    ImGui::End();
}

#endif // WITH_EDITOR
