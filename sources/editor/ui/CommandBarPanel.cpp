#include "editor/ui/CommandBarPanel.h"
#if WITH_EDITOR

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "core/logging/Log.h"
#include "editor/EditorContext.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/intent/EditorIntentSource.h"
#include "editor/intent/EditorRepoSearch.h"
#include "rendering/core/MemoryReport.h"
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

    // Greedy word wrap to a pixel width, measured with the font actually in use.
    //
    // ImGui's multiline INPUT -- the only widget here with real text selection -- does not
    // wrap: a paragraph becomes one line running off to the right. TextWrapped wraps and
    // cannot be selected. Neither is usable alone, so the wrapping is done here and the
    // result goes into the input; that is the whole trick behind selectable chat text.
    //
    // Splits on spaces, which are single bytes in UTF-8, so no multi-byte sequence is ever
    // cut. A single word longer than the line is broken at a CHARACTER boundary, found by
    // skipping continuation bytes (0b10xxxxxx) -- breaking mid-sequence would draw the
    // replacement glyph and, worse, would corrupt what gets copied out.
    std::string WrapToWidth(const std::string& text, float width)
    {
        if (width <= 1.0f)
        {
            return text;
        }
        std::string out;
        out.reserve(text.size() + text.size() / 32);
        std::size_t lineStart = 0;
        while (lineStart <= text.size())
        {
            const std::size_t hardEnd = text.find('\n', lineStart);
            const std::string line = text.substr(lineStart,
                hardEnd == std::string::npos ? std::string::npos : hardEnd - lineStart);
            std::size_t at = 0;
            std::string current;
            while (at < line.size())
            {
                std::size_t space = line.find(' ', at);
                if (space == std::string::npos)
                {
                    space = line.size();
                }
                const std::string word = line.substr(at, space - at);
                const std::string candidate = current.empty() ? word : current + " " + word;
                if (!current.empty() &&
                    ImGui::CalcTextSize(candidate.c_str()).x > width)
                {
                    out += current;
                    out += '\n';
                    current = word;
                }
                else
                {
                    current = candidate;
                }
                // One word wider than the whole line: break it rather than let it run off.
                while (ImGui::CalcTextSize(current.c_str()).x > width && current.size() > 1)
                {
                    std::size_t cut = current.size() - 1;
                    while (cut > 0 && (static_cast<unsigned char>(current[cut]) & 0xC0) == 0x80)
                    {
                        --cut;
                    }
                    std::string head = current.substr(0, cut);
                    while (!head.empty() && ImGui::CalcTextSize(head.c_str()).x > width)
                    {
                        std::size_t back = head.size() - 1;
                        while (back > 0 && (static_cast<unsigned char>(head[back]) & 0xC0) == 0x80)
                        {
                            --back;
                        }
                        head.erase(back);
                    }
                    if (head.empty())
                    {
                        break;
                    }
                    out += head;
                    out += '\n';
                    current.erase(0, head.size());
                }
                at = space + 1;
            }
            out += current;
            if (hardEnd == std::string::npos)
            {
                break;
            }
            out += '\n';
            lineStart = hardEnd + 1;
        }
        return out;
    }

    // Text the mouse can select, drawn to look like ordinary text: no frame, no background,
    // no padding. Read-only, so the buffer is never written to.
    void SelectableText(const char* id, const std::string& text, const ImVec4& colour)
    {
        std::string wrapped = WrapToWidth(text, ImGui::GetContentRegionAvail().x - 4.0f);
        int lines = 1;
        for (const char ch : wrapped)
        {
            if (ch == '\n') { ++lines; }
        }
        const float height = lines * ImGui::GetTextLineHeight() + 2.0f;

        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::InputTextMultiline(id, wrapped.data(), wrapped.size() + 1,
            ImVec2(-1.0f, height), ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
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

void CommandBarPanel::SwitchSessionTo(const std::string& levelPath)
{
    if (levelPath == sessionLevel_)
    {
        return;
    }
    if (!sessionLevel_.empty())
    {
        sessionsByLevel_[sessionLevel_] = conversation_;
    }
    const auto found = sessionsByLevel_.find(levelPath);
    conversation_ = found == sessionsByLevel_.end() ? std::vector<IntentTurn>{} : found->second;
    sessionLevel_ = levelPath;
    LOG_INFO(logging::LogCategory::Editor,
        "command bar: session for \"{}\" is now active -- {} remembered turns ({} chars); "
        "{} levels have a memory",
        levelPath, conversation_.size(), SessionChars(), sessionsByLevel_.size());
}

const std::vector<IntentTurn>* CommandBarPanel::SessionFor(const std::string& levelPath)
{
    // The ACTIVE level's memory lives in `conversation_` while it is open, so fold it back
    // before handing it out -- otherwise a save taken mid-session would write the level's
    // memory as it stood when the level was opened.
    if (!sessionLevel_.empty())
    {
        sessionsByLevel_[sessionLevel_] = conversation_;
    }
    const auto found = sessionsByLevel_.find(levelPath);
    return found == sessionsByLevel_.end() ? nullptr : &found->second;
}

void CommandBarPanel::SetSession(const std::string& levelPath, std::vector<IntentTurn> turns)
{
    if (levelPath.empty())
    {
        return;
    }
    sessionsByLevel_[levelPath] = std::move(turns);
    if (levelPath == sessionLevel_)
    {
        conversation_ = sessionsByLevel_[levelPath];
    }
}

std::string CommandBarPanel::TakeDirtySessionLevel()
{
    std::string level;
    level.swap(dirtySessionLevel_);
    return level;
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

void CommandBarPanel::CompactSession()
{
    // AUTOCOMPACT, and it needs no model turn. What is worth keeping from an old exchange
    // is not the prose and certainly not the kilobytes of grep output that were pasted in
    // to produce it -- it is WHAT THE EDITOR DID, which is already one line and already
    // written in `[editor] ...`. Keeping those verbatim and dropping everything else around
    // them is a summary that cannot hallucinate, costs nothing and takes no seconds off the
    // person's next answer.
    //
    // The cost that decides the numbers is prefill. llama.cpp caches the common PREFIX, so
    // history that only grows at the end is nearly free -- but compacting rewrites the
    // beginning and throws that cache away, at roughly 450 tokens a second. So it must fire
    // rarely and cut deep: at about 6000 tokens of session, down to a quarter of that.
    // THE BUDGET IS WHAT THE CONTEXT HAS LEFT, not a number chosen in the abstract.
    //
    // It was a flat 24000 characters -- roughly 8000 tokens -- and that was set while the
    // context was 262144 and nothing could ever collide. On a model that lives on the card
    // the context is 16384, of which the system prompt, the action list and the grammar
    // already take about 13000: a session allowed to reach 8000 tokens would push the
    // request past the end, and llama-server answers that with HTTP 400 and no explanation
    // the editor could pass on. Proven, not feared -- an 8192 context refuses our prompt
    // outright, which is how the size of it came to be known at all.
    //
    // So: whatever the context has spare after the last real request, less a margin for the
    // answer. Falls back to the old number when nothing has been measured yet.
    const std::size_t kBudgetChars = SessionBudgetChars();
    constexpr std::size_t kKeepChars = 6000;
    if (SessionChars() <= kBudgetChars)
    {
        return;
    }

    // Walk back from the newest, keeping whole turns until the keep-budget is spent.
    std::size_t kept = 0;
    std::size_t firstKept = conversation_.size();
    while (firstKept > 0)
    {
        const IntentTurn& turn = conversation_[firstKept - 1];
        const std::size_t cost = turn.user.size() + turn.assistant.size();
        if (kept + cost > kKeepChars && firstKept < conversation_.size())
        {
            break;
        }
        kept += cost;
        --firstKept;
    }
    if (firstKept == 0)
    {
        return;   // one enormous turn; there is nothing older to fold
    }

    std::string digest;
    std::size_t dropped = 0;
    for (std::size_t at = 0; at < firstKept; ++at)
    {
        const IntentTurn& turn = conversation_[at];
        ++dropped;
        const std::size_t mark = turn.assistant.find("[editor] ");
        if (mark == std::string::npos)
        {
            continue;   // a conversation turn: it changed nothing, and it goes
        }
        std::string did = turn.assistant.substr(mark + 9);
        const std::size_t eol = did.find('\n');
        if (eol != std::string::npos)
        {
            did.erase(eol);
        }
        digest += "- \"" + turn.user + "\" -> " + did + "\n";
    }

    const std::size_t before = SessionChars();
    conversation_.erase(conversation_.begin(),
        conversation_.begin() + static_cast<std::ptrdiff_t>(firstKept));
    if (!digest.empty())
    {
        // ABSENT AND EMPTY ARE DIFFERENT THINGS to a model: an earlier part of the session
        // that is simply missing reads as "nothing happened yet", which is the exact answer
        // that made this whole problem visible. The digest says outright that it is a
        // summary and that the talking around these actions was dropped.
        conversation_.insert(conversation_.begin(), IntentTurn{
            "(what happened earlier in this session -- the talk is gone, the edits are not)",
            "Earlier in this session the editor carried out, in order:\n" + digest +
                "Ask `TOOL: scene` if you need the level as it stands now." });
    }
    LOG_INFO(logging::LogCategory::Editor,
        "command bar: session compacted -- {} older turns folded into {} remembered edits, "
        "{} -> {} chars",
        dropped, digest.empty() ? 0 : 1, before, SessionChars());
}

std::string CommandBarPanel::ModelLabelFor(const std::string& path)
{
    if (path.empty())
    {
        return "(no model chosen)";
    }
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void CommandBarPanel::RefreshModelChoices(const std::string& current)
{
    // Rescanned each time the list is opened rather than cached: a model downloaded while
    // the editor was running should appear without restarting it, and the scan is one
    // directory listing of a folder holding a handful of very large files.
    modelChoices_.clear();
    if (current.empty())
    {
        return;
    }
    std::error_code ec;
    const std::filesystem::path folder = std::filesystem::path(current).parent_path();
    if (folder.empty() || !std::filesystem::is_directory(folder, ec))
    {
        return;
    }
    for (std::filesystem::directory_iterator it(folder, ec), end; it != end; it.increment(ec))
    {
        if (ec || !it->is_regular_file(ec) || it->path().extension() != ".gguf")
        {
            continue;
        }
        const std::string name = it->path().filename().string();
        // The projector is not a model to run; it rides ALONGSIDE one, and offering it here
        // would be offering a choice that cannot work.
        if (name.rfind("mmproj", 0) == 0)
        {
            continue;
        }
        const std::uintmax_t bytes = std::filesystem::file_size(it->path(), ec);
        char size[32] = {};
        std::snprintf(size, sizeof(size), "%.1f GB",
            static_cast<double>(ec ? 0 : bytes) / (1024.0 * 1024.0 * 1024.0));
        modelChoices_.push_back({ it->path().generic_string(), name + "   " + size });
    }
    std::sort(modelChoices_.begin(), modelChoices_.end(),
        [](const ModelChoice& a, const ModelChoice& b) { return a.label < b.label; });
}

std::size_t CommandBarPanel::SessionBudgetChars() const
{
    if (!model_)
    {
        return kSessionCompactAtChars;
    }
    const int context = model_->ContextTokens();
    const int prompt = model_->PromptTokens();
    if (context <= 0 || prompt <= 0)
    {
        return kSessionCompactAtChars;
    }
    // `prompt` already INCLUDES whatever session was sent with that request, so the spare
    // room is the context minus the prompt, plus what the session contributed -- which is
    // what the session may grow back into. Reserved on top: room for the answer itself,
    // since a reply that cannot be generated is as bad as a request that cannot be sent.
    constexpr int kReserveForAnswer = 2048;
    const int sessionTokens = static_cast<int>(SessionChars() / kCharsPerTokenGuess);
    const int spare = context - prompt + sessionTokens - kReserveForAnswer;
    if (spare <= 0)
    {
        return 0;   // nothing fits; compact everything that can be compacted
    }
    const std::size_t budget = static_cast<std::size_t>(spare) * kCharsPerTokenGuess;
    return std::min(budget, kSessionCompactAtChars);
}

void CommandBarPanel::LogModelStats()
{
    if (!model_ || !model_->ServerIsUp())
    {
        return;
    }
    const double now = ImGui::GetTime();
    if (now < nextStatsLogSec_)
    {
        return;
    }
    nextStatsLogSec_ = now + 30.0;

    std::uint64_t gpuUsed = 0, gpuTotal = 0;
    render::GpuMemoryTotals(gpuUsed, gpuTotal);
    const std::uint64_t toMb = 1024 * 1024;

    const int prompt = model_->PromptTokens();
    const int context = model_->ContextTokens();
    const std::size_t chars = SessionChars();
    // The SAME function CompactSession asks, so the countdown cannot drift away from the
    // thing it is counting down to -- and it moves with the context, which is the point:
    // a smaller context leaves less room and the number here says so.
    const std::size_t budget = SessionBudgetChars();
    const std::size_t toCompact = chars >= budget ? 0 : budget - chars;

    std::string vram = "vram unknown (no NVML)";
    if (gpuTotal > 0)
    {
        const std::uint64_t modelMb = model_->ModelVramBytes() / toMb;
        vram = "vram card " + std::to_string(gpuUsed / toMb) + " of " +
            std::to_string(gpuTotal / toMb) + " MB";
        if (modelMb > 0)
        {
            vram += ", of which the model " + std::to_string(modelMb) + " MB";
        }
        vram += ", free " + std::to_string((gpuTotal - gpuUsed) / toMb) + " MB";
    }

    // ZERO IS NOT A MEASUREMENT. Before the first request there is nothing to report, and
    // "context 0 of 32768 (0%)" reads as a broken counter rather than as an empty one.
    const std::string contextPart = prompt > 0
        ? "context " + std::to_string(prompt) + " of " + std::to_string(context) +
            " tokens at its fullest (" +
            std::to_string(context > 0 ? (prompt * 100) / context : 0) + "%)"
        : "context " + std::to_string(context) + " tokens, nothing asked yet";

    LOG_INFO(logging::LogCategory::Editor,
        "model stats: {} | {} | session {} of {} chars, compacts in {}",
        vram, contextPart, chars, budget, toCompact);
}

std::size_t CommandBarPanel::SessionChars() const
{
    std::size_t total = 0;
    for (const IntentTurn& turn : conversation_)
    {
        total += turn.user.size() + turn.assistant.size();
    }
    return total;
}

bool CommandBarPanel::PreviewAwaitsRun() const
{
    return hasResult_ && !waiting_ && preview_.executable && !preview_.answerOnly;
}

void CommandBarPanel::RunPendingPreview(const EditorActionContext& actionCtx,
    EditorCommandStack& commandStack)
{
    if (!PreviewAwaitsRun())
    {
        return;
    }
    ExecuteIntent(actionCtx, commandStack, preview_, status_);
    RecordOutcome(status_, Exchange::Kind::Ran);
    ClearResult();
}

std::vector<IntentTurn> CommandBarPanel::ThreadForModel() const
{
    std::vector<IntentTurn> thread = conversation_;
    thread.insert(thread.end(), history_.begin(), history_.end());
    return thread;
}

void CommandBarPanel::RememberTurn(const std::string& phrase,
    const std::string& modelAnswer,
    const std::string& editorDid)
{
    if (phrase.empty())
    {
        return;
    }
    std::string said = modelAnswer;
    if (!editorDid.empty())
    {
        // Attached to the MODEL's turn, not written as a user message: the editor is not a
        // participant in this conversation, it is what happened because of what the model
        // answered. Reading it back, the model sees its own choice and that choice's result
        // side by side, which is the only form in which "и что ты сделал?" is answerable.
        said += (said.empty() ? "" : "\n") + std::string("[editor] ") + editorDid;
    }
    if (said.empty())
    {
        return;
    }
    conversation_.push_back({ phrase, said });
    dirtySessionLevel_ = sessionLevel_;
    CompactSession();
    // ITS ACTIONS, IN THE LOG, in the same words the model will read them back in. The log
    // said what the resolver did (`intent [llm] action=group ... -> Put 222 into 1 groups`)
    // and separately what the model answered, and neither line said what the MODEL would
    // remember -- which is the thing that decides its next answer.
    LOG_INFO(logging::LogCategory::Editor, "command bar: session remembers ({} turns) \"{}\" -> {}",
        conversation_.size(), phrase, said);
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

bool CommandBarPanel::TakeModelSettingsDirty()
{
    const bool dirty = modelSettingsDirty_;
    modelSettingsDirty_ = false;
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
    // The same sentence was on screen twice: `status_` is drawn under the transcript, and a
    // run wrote its result into BOTH. Once it is in the transcript the loose line is a copy,
    // so it goes -- the transcript is the record, the status line is only for what has not
    // reached it yet.
    if (status_ == verdict)
    {
        status_.clear();
    }

    // EVERY ENDING OF A PHRASE GOES INTO THE SESSION, and it is done here because here is
    // the one place all of them pass through. Doing it at the call sites meant the three
    // that execute remembered nothing: the run button, the auto-run and "Select these" each
    // recorded a verdict for the screen and nothing for the model.
    //
    // Preview and Asked are deliberately NOT remembered -- they are mid-phrase, and the
    // ending that follows will carry the whole thing. Said is remembered by the prose path
    // itself, which has the answer AND the thinking to keep apart.
    if (kind == Exchange::Kind::Ran || kind == Exchange::Kind::Refused ||
        kind == Exchange::Kind::Failed)
    {
        // NO RAW JSON IN THE SESSION. It used to store `model_->LastRawAnswer()`, the
        // verdict exactly as the grammar turn emitted it -- and the prose turn reads this
        // same history, sees that every previous thing it "said" was a JSON object, and
        // says one too. Right after grouping 610 palms, "и что ты сделал?" came back as the
        // literal text {"kind":"chat"}.
        //
        // What is worth remembering was never the JSON: it is which action ran and what it
        // did, and both read as a sentence. The raw answer still lives in `history_`, the
        // thread inside ONE command, where a model fixing its own malformed output needs to
        // see it.
        // NOT A SENTENCE IN THE FIRST PERSON. It used to record "I used the editor's
        // `group` action." -- and asked "что ты сделал?", the model replied with exactly
        // that string: an English canned line, in a Russian conversation, saying nothing.
        // A record it can READ is not a line it should REPEAT, so it is written as a record:
        // no pronoun, no verb, nothing that reads like speech. What it then says is its own.
        const std::string did = intent_.action.empty()
            ? std::string{}
            : ("<action>" + intent_.action + "</action> ");
        RememberTurn(transcript_.back().phrase, did, verdict);
    }
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
    inConversation_ = false;
    conversationTools_ = 0;
    verdictRetries_ = 0;
    lastPartialSize_ = 0;
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
    waitingSinceSec_ = ImGui::GetTime();
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
    sources_[activeSource_]->Begin(sendPhrase_, world, ThreadForModel());
}

void CommandBarPanel::PollSources(const EditorActionContext& actionCtx)
{
    if (!waiting_)
    {
        return;
    }
    // A PROSE TURN IS ALSO "waiting", and it is not this function's to poll. Without this
    // the next frame asked the intent source for an answer it no longer had, read the Idle
    // that came back as a decline, ran out of sources and reported the phrase as not
    // understood -- two seconds after the model had understood it perfectly and said so.
    if (inConversation_)
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
                    sources_[activeSource_]->Begin(sendPhrase_, world, ThreadForModel());
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
            sources_[activeSource_]->Begin(sendPhrase_, world, ThreadForModel());
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

        // THE PERSON GETS THE GIST, THE MODEL GETS THE JSON. `answer` is the query result
        // verbatim -- six hundred characters of braces and mesh paths -- and putting it in
        // the transcript filled the window with something no human reads. It still goes to
        // the model in full, one line above; this is only what is shown.
        std::string shownAnswer = answer;
        for (char& ch : shownAnswer)
        {
            if (ch == '\n' || ch == '\r') { ch = ' '; }
        }
        constexpr std::size_t kShownAnswer = 140;
        if (shownAnswer.size() > kShownAnswer)
        {
            shownAnswer = shownAnswer.substr(0, kShownAnswer) + "...";
        }
        RecordOutcome("asked the editor -- " + asked + ": " + shownAnswer,
            Exchange::Kind::Asked);
        transcript_.push_back({ phrase_, "thinking...", Exchange::Kind::Preview });
        transcriptScrollToBottom_ = true;

        const EditorIntentWorld world{ actionCtx.editor.document, actionCtx.assets };
        ClearResult();
        waiting_ = true;
        waitingSinceSec_ = ImGui::GetTime();
        sources_[activeSource_]->Begin(sendPhrase_, world, ThreadForModel());
        return;
    }

    // NOT AN EDIT AT ALL: answer it in prose, in this same window. The verdict cost about
    // a second and twenty tokens; the sentence comes from the turn that starts here.
    if (intent_.kind == EditorIntentKind::Chat)
    {
        ClearThread();
        RecordOutcome("...", Exchange::Kind::Said);
        BeginConversationTurn(actionCtx);
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
        waitingSinceSec_ = ImGui::GetTime();
        sources_[activeSource_]->Begin(sendPhrase_, world, ThreadForModel());
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


void CommandBarPanel::BeginConversationTurn(const EditorActionContext& actionCtx)
{
    std::string protocol;
    if (model_->Settings().chatSearch)
    {
        protocol = reposearch::ProtocolPrompt();
        protocol += editorquery::ChatProtocolPrompt();
    }
    // The offer, which is how a conversation reaches the editor without becoming one.
    protocol +=
        "\nWHEN THE EDITOR COULD DO WHAT YOU ARE TALKING ABOUT, end your answer with one\n"
        "last line:\n"
        "  RUN: <the phrase a person would type>\n"
        "They get a button saying so, and nothing happens until they press it -- it then\n"
        "goes through the ordinary path, with a preview and an undo, so an offer costs a\n"
        "glance and never costs an edit. Write a SENTENCE in their language naming what it\n"
        "applies to, never an action id.\n"
        // The action list is ALREADY in this prompt: BeginConversation reuses the same
        // system block the grammar turn uses, so the prose turn is loaded with every
        // action, its description and its parameters. Saying so is what turns that from a
        // thing it happens to have read into a thing it knows it may offer from.
        "THE ACTION LIST ABOVE IS YOURS TO OFFER FROM -- the same list, in this same\n"
        "prompt, with every parameter each one takes. You are the one who can tell whether\n"
        "something on it would answer what they just said, so use your judgement and offer\n"
        "freely: a suggestion they ignore costs nothing.\n"
        // The one offer that costs something: one for a thing the editor cannot do. The
        // button goes down the ordinary command path, which answers "no such action" --
        // a round of waiting to be told no, for a button that should never have appeared.
        // Seen live: asked which other levels exist, it listed them correctly and then
        // offered "покажи содержимое demo.json" -- work it can do itself, this turn, with
        // no button and no editor.
        "BUT THE OFFER MUST BE AN EDIT, from that list. Looking something up is not an\n"
        "offer: if reading a file or asking about the level would answer them, use TOOL:\n"
        "and answer -- do not hand them a button for it. No RUN: line is the normal case.\n";

    ClearResult();
    inConversation_ = true;
    // NOT `conversationTools_ = 0` -- this function is re-entered for every round of the
    // search loop, so resetting here reset the loop's own budget and the cap never bit.
    // The log said "round 1/3" twice in a row for the same question, which is what a
    // counter that has forgotten looks like. It is reset in SubmitPhrase, where a NEW
    // question starts, and only there.
    lastPartialSize_ = 0;
    waiting_ = true;
    waitingSinceSec_ = ImGui::GetTime();
    model_->BeginConversation(conversation_, sendPhrase_, protocol,
        0.7f, model_->Settings().chatAnswerTokens, model_->Settings().chatReasoning);
    (void)actionCtx;
}

void CommandBarPanel::PollConversation(const EditorActionContext& actionCtx)
{
    if (!inConversation_)
    {
        return;
    }

    std::string text;
    std::string error;
    bool truncated = false;
    const IntentParseState state = model_->PollFreeform(text, error, truncated);
    if (state == IntentParseState::Pending)
    {
        // Streamed into the transcript as it lands. The alternative is a box that says
        // nothing for half a minute, which is indistinguishable from a hang.
        const std::string partial = model_->FreeformPartial();
        if (partial.size() != lastPartialSize_)
        {
            lastPartialSize_ = partial.size();
            const std::size_t close = partial.find("</think>");
            const std::string shown = close == std::string::npos
                ? (model_->Settings().chatReasoning ? partial : std::string("thinking..."))
                : partial.substr(close + 8);
            RecordOutcome(shown.empty() ? "..." : shown, Exchange::Kind::Said);
        }
        return;
    }

    inConversation_ = false;
    waiting_ = false;
    lastPartialSize_ = 0;
    if (state == IntentParseState::Failed || state == IntentParseState::Idle)
    {
        RecordOutcome(error.empty() ? "The model did not answer." : error,
            Exchange::Kind::Failed);
        return;
    }

    std::string thinking;
    std::string answer = text;
    const std::size_t open = text.find("<think>");
    const std::size_t close = text.find("</think>");
    if (open != std::string::npos && close != std::string::npos && close > open)
    {
        thinking = text.substr(open + 7, close - open - 7);
        answer = text.substr(close + 8);
    }
    const auto trim = [](std::string& value)
    {
        const std::size_t first = value.find_first_not_of(" \t\r\n");
        const std::size_t last = value.find_last_not_of(" \t\r\n");
        value = first == std::string::npos ? std::string{}
                                           : value.substr(first, last - first + 1);
    };
    trim(thinking);
    trim(answer);

    // Did it ask to look something up? Same protocol and same budget reasoning as the
    // command side: each round is a whole generation the person waits through.
    const reposearch::SceneQueryFn askScene = [&actionCtx](const std::string& rest)
    {
        return editorquery::AnswerChatLine(actionCtx, rest);
    };
    std::string toolReport;
    if (model_->Settings().chatSearch && conversationTools_ < kMaxConversationTools &&
        reposearch::RunRequestedTools(answer, askScene, toolReport))
    {
        ++conversationTools_;
        // THE COMMANDS, not just how much came back. A byte count cannot tell you whether
        // an answer was grounded: the one time this mattered, the model described four
        // level files it had never opened, and the log said only "2438 characters back" --
        // enough to know it looked something up, not enough to know what. The bodies stay
        // out; they are kilobytes and they are what the answer is made of anyway.
        for (std::size_t lineStart = 0; lineStart < toolReport.size();)
        {
            const std::size_t eol = toolReport.find('\n', lineStart);
            if (toolReport.compare(lineStart, 2, "$ ") == 0)
            {
                LOG_INFO(logging::LogCategory::Editor, "command bar: chat ran  {}",
                    toolReport.substr(lineStart + 2,
                        eol == std::string::npos ? eol : eol - lineStart - 2));
            }
            if (eol == std::string::npos)
            {
                break;
            }
            lineStart = eol + 1;
        }
        LOG_INFO(logging::LogCategory::Editor,
            "command bar: chat searched (round {}/{}), {} characters back",
            conversationTools_, kMaxConversationTools, toolReport.size());
        // Through RememberTurn like everything else, so a search round is compacted and
        // persisted on the same rules. It matters most here: `sendPhrase_` on a later round
        // IS the tool output, kilobytes of it, and a push straight into the vector would put
        // that on disk and keep it there past the point where anything still needed it.
        RememberTurn(sendPhrase_, answer, std::string{});
        // HOW MANY LOOKUPS ARE LEFT, said every round. A budget the model cannot see is a
        // budget it walks into: on the last round it asked for two more greps, they were
        // never run, and those `TOOL:` lines became the visible answer -- the person got a
        // request for a shell command instead of a reply. Told the count, it spends the
        // last one on the answer instead.
        const int left = kMaxConversationTools - conversationTools_;
        sendPhrase_ = "Tool output:\n\n```\n" + toolReport + "\n```\n\n";
        sendPhrase_ += left > 0
            ? "Now answer using what it says. You may look up " + std::to_string(left) +
                  " more time" + (left == 1 ? "" : "s") + " for this question if you must."
            : "THAT WAS THE LAST LOOKUP for this question -- no further TOOL: line will be "
              "run. Answer now with what you have, and say plainly what you could not "
              "check rather than guessing at it.";
        RecordOutcome("looked it up -- " + std::to_string(toolReport.size()) +
            " characters back", Exchange::Kind::Asked);
        transcript_.push_back({ phrase_, "...", Exchange::Kind::Said });
        transcriptScrollToBottom_ = true;
        BeginConversationTurn(actionCtx);
        return;
    }

    // Any lookup still being asked for here was NOT run -- the budget is spent, or search
    // is off. It is a request, not a reply, and it must not be shown as one.
    if (answer.find("TOOL:") != std::string::npos)
    {
        const std::string before = answer;
        reposearch::StripToolLines(answer);
        if (before != answer)
        {
            LOG_INFO(logging::LogCategory::Editor,
                "command bar: chat asked for a lookup with none left -- hid the request");
        }
        if (answer.empty())
        {
            // Nothing but the request. Saying so is the only honest thing left: the person
            // asked a question and there is no answer, and a blank bubble reads as a hang.
            answer = "Мне не хватило запросов, чтобы это выяснить. Спроси ещё раз -- "
                     "счётчик обнулится, и я продолжу с того, что уже прочитал.";
        }
    }

    // The offer line, taken off the visible text: it is a protocol between the model and
    // this panel, and showing it would make the conversation read as a transcript of that.
    std::string offer;
    const std::size_t at = answer.rfind("RUN:");
    if (at != std::string::npos)
    {
        const std::size_t eol = answer.find('\n', at);
        offer = answer.substr(at + 4,
            eol == std::string::npos ? std::string::npos : eol - at - 4);
        const std::size_t a = offer.find_first_not_of(" \t\r\n`\"");
        const std::size_t b = offer.find_last_not_of(" \t\r\n`\"");
        offer = a == std::string::npos ? std::string{} : offer.substr(a, b - a + 1);
        // AN OFFER IS A PHRASE A PERSON WOULD TYPE, and this takes the whole line, so a
        // model that keeps writing past the command hands the button a paragraph. Seen:
        // "открой уровень wind_test.json** (Если бы у меня была возможность..." -- two
        // hundred characters of reasoning on a button. It is DROPPED rather than cut at
        // some length: a truncated command still looks like a command, and the person
        // would be pressing something nobody wrote.
        // The line comes off the visible text whether or not the offer survives -- it is
        // protocol either way, and leaving it in shows the person a bare "RUN: ..." where
        // prose belongs.
        if (!offer.empty())
        {
            answer.erase(at, eol == std::string::npos ? std::string::npos : eol - at + 1);
            trim(answer);
        }
        constexpr std::size_t kMaxOffer = 120;
        if (offer.size() > kMaxOffer)
        {
            LOG_INFO(logging::LogCategory::Editor,
                "command bar: chat offered {} characters -- too long to be a command, dropped",
                offer.size());
            offer.clear();
        }
    }

    if (answer.empty() && !thinking.empty())
    {
        answer = "(the whole token budget went on thinking -- raise the budget in settings)";
    }
    // A VERDICT IS NOT AN ANSWER, and it must never become one. The prose turn once came
    // back with the literal text {"kind":"chat"} -- the routing decision, echoed as though
    // it were a reply -- and the person saw that in the transcript. The prompt fix is in
    // BeginConversation; this is the guard, and it is here because the failure feeds
    // itself: remembered, the bad answer becomes an example the next turn copies.
    if (!answer.empty() && answer.front() == '{' && answer.size() < 200 &&
        answer.find("\"kind\"") != std::string::npos)
    {
        LOG_WARNING(logging::LogCategory::Editor,
            "command bar: the prose turn answered with a verdict ({}) instead of a sentence",
            answer);
        // ASK IT AGAIN RATHER THAN ASKING THE PERSON TO. This is a slip of the model's, and
        // making somebody retype their question because of it is charging them for it. One
        // retry only: twice in a row is a real fault and then they do need to know.
        if (++verdictRetries_ <= 1)
        {
            LOG_INFO(logging::LogCategory::Editor, "command bar: asking it again for prose");
            BeginConversationTurn(actionCtx);
            return;
        }
        // Kind::Said DELIBERATELY, not Failed: Failed is one of the kinds RecordOutcome
        // folds into the session, and remembering this would be the exact poisoning the
        // guard exists to prevent.
        RecordOutcome("(модель дважды ответила формой вместо фразы -- спроси ещё раз)",
            Exchange::Kind::Said);
        return;
    }
    RememberTurn(phrase_, answer, std::string{});

    LOG_INFO(logging::LogCategory::Editor, "command bar: chat < {}{}", answer,
        truncated ? "  [TRUNCATED at the token budget]" : "");
    RecordOutcome(answer, Exchange::Kind::Said);
    if (!transcript_.empty())
    {
        transcript_.back().thinking = thinking;
        transcript_.back().offer = offer;
    }
    if (!offer.empty())
    {
        LOG_INFO(logging::LogCategory::Editor, "command bar: offered to run \"{}\"", offer);
    }
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

    PollConversation(actionCtx);
    PollSources(actionCtx);
    if (waiting_)
    {
        return HeadlessState::Pending;
    }

    headless_ = false;
    input_.clear();

    // A CONVERSATION IS AN OUTCOME TOO. The headless path knew only about commands, so a
    // phrase the model answered perfectly well in prose came back as "not understood" --
    // the harness reporting the absence of a command as the absence of an answer.
    if (!transcript_.empty() && transcript_.back().kind == Exchange::Kind::Said)
    {
        outVerdict = "CHAT: " + transcript_.back().verdict;
        if (!transcript_.back().offer.empty())
        {
            outVerdict += " | offers to run: \"" + transcript_.back().offer + "\"";
        }
        return HeadlessState::Done;
    }

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

    // THE MASTER SWITCH, FIRST. It was called "Enabled" and sat two thirds of the way down
    // beside "Auto-start server", where it read as another detail of how the server starts
    // rather than as the thing that decides whether any of this runs at all. Somebody
    // looking for "turn the model off" scrolled past it.
    //
    // Off means off: no server at startup, no warmup, no requests -- and a running server
    // is stopped and its video memory handed back (see SetSettings). The editor works
    // perfectly well without it; the exact-form parser answers the phrases it knows and the
    // rest simply says it needs the model.
    {
        LlmIntentSettings master = model_->Settings();
        // NOT "Local model" -- that is the name of the fold this sits inside, and ImGui
        // derives a widget's identity from its label, so two visible items called the same
        // thing in the same window is a genuine ID collision. It says so in a red box.
        if (ImGui::Checkbox("Model enabled", &master.enabled))
        {
            model_->SetSettings(master);
            modelSettingsDirty_ = true;
            settings = model_->Settings();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("The whole feature. Off: nothing starts, nothing is asked, "
                "and a server already running is stopped -- about 16 GB of video memory "
                "back. On: it starts with the editor and runs while the editor does.");
        }
    }

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
    // BOTH BUTTONS, ALWAYS, and the state beside them says which one does anything.
    //
    // Stop used to appear only for a server this session started, which is precisely
    // backwards: the case somebody wants a stop button for is the one they did NOT start --
    // a server kept alive by a previous editor, holding 38 GB while they do something else.
    // It works on that one now, found by who is listening on the endpoint.
    {
        const bool up = model_->ServerIsUp();
        const bool ours = model_->OwnsRunningServer();
        if (up)
        {
            // NO COUNTDOWN. This said "stops in 571s if unused" against an idle timer that
            // had been disabled by default for weeks -- the stop could not happen and the
            // panel announced it anyway. The server now runs for as long as the editor
            // does; what comes after is the watchdog's business and it starts counting when
            // the last editor closes, not while somebody is thinking.
            ImGui::TextDisabled(ours ? "Server up (started here, runs while the editor does)"
                                     : "Server up (started elsewhere, kept alive)");
        }
        else
        {
            ImGui::TextDisabled("Server is not up");
        }

        ImGui::BeginDisabled(!model_->Available() || up);
        if (ImGui::SmallButton("Start server"))
        {
            std::string status;
            model_->StartServerNow(status);
            status_ = status.empty() ? std::string("Starting the model server...") : status;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Load the model now instead of on the next phrase. It takes a "
                "few seconds from the page cache and about twenty from cold, and the panel "
                "says \"Model loading...\" until it answers.");
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!up);
        if (ImGui::SmallButton("Stop server"))
        {
            std::string status;
            model_->StopServer(status);
            status_ = status;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Stops whatever is serving this endpoint, including a server "
                "this editor did not start. Its watchdog notices and exits too. The model is "
                "memory-mapped, so what an idle server costs is page cache the OS can "
                "reclaim rather than committed RAM -- stopping it frees the VRAM, which is "
                "the part the renderer wants back.");
        }
    }
    // THE COMMAND TURN'S OWN SWITCH, which had a setting and no control. `commandReasoning`
    // has been on by default since the lazy grammar made it possible, and the only
    // reasoning-shaped checkbox on this panel was "think out loud" -- which shows the
    // chat's working and has nothing to do with whether a COMMAND is thought about. Two
    // different things that sounded like one, and the one that matters was invisible.
    ImGui::SeparatorText("When it answers with a command");
    {
        LlmIntentSettings toggles = model_->Settings();
        if (ImGui::Checkbox("think before choosing an action", &toggles.commandReasoning))
        {
            model_->SetSettings(toggles);
            modelSettingsDirty_ = true;
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("On, the model reasons first and the grammar only takes over at "
            "the end of its thinking. Measured on this machine: an ordinary phrase costs "
            "1.8-7.7s instead of 0.5-2.1s, and a genuinely hard one can reach 30s -- but "
            "it is the difference between picking an action and choosing one.\n"
            "Off, an already-closed think block is pre-filled and it answers straight from "
            "the phrase. Faster, with no step in which it can notice anything.");
    }

    // The conversation half's knobs, which used to live in the chat panel.
    ImGui::SeparatorText("When it answers in prose");
    {
        LlmIntentSettings toggles = model_->Settings();
        if (ImGui::Checkbox("read source and scene", &toggles.chatSearch))
        {
            model_->SetSettings(toggles);
            modelSettingsDirty_ = true;
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Let it look things up before answering -- this engine's own "
            "source, and the level that is open. Each lookup costs a whole extra round "
            "trip, and it may take up to three.");
    }
    ImGui::SameLine();
    {
        LlmIntentSettings toggles = model_->Settings();
        if (ImGui::Checkbox("think before writing a reply", &toggles.chatReasoning))
        {
            model_->SetSettings(toggles);
            modelSettingsDirty_ = true;
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("On, it reasons before replying. Measured here: a greeting goes "
            "0.5s to 1.9s, a real question 19.8s to 32.7s, and a long one barely moves "
            "(26.7s to 28.6s) because the answer's own length dominates.\n"
            "The difference is what comes back: without it, general advice about game "
            "engines; with it, a diagnosis of THIS problem with numbers to try. Turn it off "
            "when you want a fast reply more than a considered one.");
    }
    ImGui::SetNextItemWidth(160.0f);
    // EDITS THE SETTING, not a copy of it. This slider used to move a panel member that
    // nothing ever wrote to disk, so it reset to its default on every launch and the person
    // who raised it had to raise it again tomorrow.
    {
        LlmIntentSettings budget = model_->Settings();
        // Up to 65536, which is past what most contexts can hold -- deliberately. The cap
        // is a ceiling on ONE answer, and the server clamps it to what the context has left
        // anyway, so a high setting costs nothing until the room exists. The status line
        // beside History is where the real limit is visible.
        if (ImGui::SliderInt("answer budget", &budget.chatAnswerTokens, 1024, 65536,
                "%d tokens"))
        {
            model_->SetSettings(budget);
            modelSettingsDirty_ = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("the longest ONE answer may be. A token is a piece of a word, "
                "not a character -- about 3 characters of Russian. This is a ceiling on how "
                "long you wait, not on quality: the model stops when it has finished.");
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("A ceiling, not a target: the model stops when it is finished, so "
            "a short answer costs what it costs. Raise it when an answer gets cut off.");
    }

    if (!model_->Available())
    {
        ImGui::TextDisabled("Run: python tools/fetch_intent_model.py");
    }

    changed |= ImGui::Checkbox("Auto-start server", &settings.autoStart);
    if (ImGui::Checkbox("Chat page (restarts the server)", &settings.webUi))
    {
        changed = true;
    }
    // THE "IDLE STOP" SLIDER IS GONE. It moved a number that stopped being reachable the
    // day the server was allowed to outlive the editor: the idle check begins with
    // `if (!serverOwned_ || keepsServerAlive_ || ...)` and keepServerAfterExit is on by
    // default, so dragging it changed nothing whatsoever. Retirement is the watchdog's job
    // now -- five minutes after the LAST editor closes -- and that is where the timer is.
    // A control that cannot affect anything is worse than a missing one: it answers the
    // question "how do I make it let go of the model" with a lie.

    // PICK A MODEL FROM THE ONES ON DISK. Swapping models is a settings edit by design --
    // the model is a parameter, not a build dependency -- but "a settings edit" had meant
    // typing an absolute path into a text box, and getting one character wrong reads as the
    // feature being broken rather than as a typo. Every .gguf beside the current one is
    // listed with its size, because size is what decides whether it fits on the card, and
    // that is the question anyone switching is actually asking.
    //
    // The text box stays underneath for a model kept somewhere else.
    if (ImGui::BeginCombo("##modelPick", ModelLabelFor(settings.modelPath).c_str()))
    {
        RefreshModelChoices(settings.modelPath);
        if (modelChoices_.empty())
        {
            ImGui::TextDisabled("no .gguf files beside the current one");
        }
        for (const ModelChoice& choice : modelChoices_)
        {
            const bool selected = choice.path == settings.modelPath;
            if (ImGui::Selectable(choice.label.c_str(), selected))
            {
                settings.modelPath = choice.path;
                CopyToBuffer(modelPathBuffer_, sizeof(modelPathBuffer_), choice.path);
                changed = true;
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("every .gguf in the same folder. Switching restarts the server, "
            "which costs a few seconds and reloads the weights. A vision model also needs "
            "its mmproj file beside it to see anything.");
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
        ImGui::SetTooltip("How many of the model's layers live on the card. 99 means all of "
            "them; lower puts the rest in RAM and each token then walks back across PCIe "
            "for them.\n"
            "This is the memory-for-speed dial. Measured on Qwen3.8-27B Q4_K_M, 32k "
            "context: all layers = 19.1 GB of VRAM and 1.0-2.1s a phrase; 56 layers = "
            "16.8 GB and 2.3-5.0s. Same answers either way -- you are buying latency, not "
            "quality.\n"
            "Leave room for the renderer: filling the card past about 22 GB makes the "
            "driver evict its resources and the whole editor stutters.");
    }

    if (changed)
    {
        model_->SetSettings(settings);
        modelSettingsDirty_ = true;
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

    // Which level's memory is the live one. Checked from the document every frame rather
    // than hung off a level-changed callback: the callback exists, but a memory that is
    // silently wrong about WHICH level it describes is exactly the failure this whole thing
    // is for, and the document is the only thing that always knows.
    SwitchSessionTo(ctx.document.LevelPath());
    LogModelStats();

    // NOT TIED TO THE SERVER, deliberately. The obvious place to forget is when the server
    // goes away -- its prefix cache dies with it, so the next one re-reads everything. But
    // the server is retired after five idle minutes, and the person never sees that happen:
    // group the palms, go for a coffee, come back and ask what was done, and the answer
    // would be "ничего" again. The cache is a question of COST, one re-prefill at about 450
    // tokens a second; the session is a question of TRUTH. Only the second one is worth
    // protecting, so the memory lives as long as the level does.
    //
    // A button pressed LAST frame, acted on now. Submitting from inside the transcript loop
    // would rewrite the vector being iterated, and the offer is the one control here that
    // adds an entry rather than changing one.
    if (!pendingOffer_.empty() && !waiting_)
    {
        input_ = pendingOffer_;
        pendingOffer_.clear();
        Begin(actionCtx, buryDepthPercent);
    }

    // The model answers on its own thread; this is where its answer is picked up, and
    // where an idle server is retired.
    model_->Tick();
    PollConversation(actionCtx);
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
        // (the matching EndChild is guarded the same way -- ImGui wants it whenever
        //  BeginChild was CALLED, and in the selectable branch above it never was)
        for (std::size_t index = 0; index < transcript_.size(); ++index)
        {
            const Exchange& exchange = transcript_[index];
            ImGui::PushID(static_cast<int>(index));
            // GROUPED so the whole exchange is one hover target for the copy menu below.
            // ImGui's TextWrapped cannot be selected with the mouse -- there is no text
            // selection in it at all -- so dragging across an answer to copy a number or a
            // path does nothing and looks broken. A right-click menu is the honest way to
            // get the text out.
            ImGui::BeginGroup();
            ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f), "you");
            SelectableText("##phrase", exchange.phrase, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
            const ImVec4 colour =
                exchange.kind == Exchange::Kind::Ran ? ImVec4(0.65f, 0.95f, 0.65f, 1.0f) :
                exchange.kind == Exchange::Kind::Refused ? ImVec4(1.0f, 0.78f, 0.24f, 1.0f) :
                exchange.kind == Exchange::Kind::Failed ? ImVec4(1.0f, 0.55f, 0.55f, 1.0f) :
                // A query is the model looking something up, not a result. Blue keeps it
                // legible as a step along the way rather than an answer to the phrase.
                exchange.kind == Exchange::Kind::Asked ? ImVec4(0.58f, 0.78f, 1.0f, 1.0f) :
                // Prose is the ordinary text colour: it is somebody talking, not a verdict
                // about the level, and colouring it would make a conversation look like a
                // status report.
                exchange.kind == Exchange::Kind::Said ? ImVec4(0.88f, 0.88f, 0.88f, 1.0f) :
                ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
            if (!exchange.thinking.empty() && ImGui::TreeNode("thinking"))
            {
                SelectableText("##thinking", exchange.thinking,
                    ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                ImGui::TreePop();
            }
            SelectableText("##verdict", exchange.verdict, colour);
            // AN OFFER, NOT AN ACTION. The phrase sits on a button beside the sentence that
            // proposed it -- an offer that scrolls away from its own answer is one nobody
            // connects to anything -- and pressing it submits the phrase the ordinary way,
            // so it still meets the grammar, the preview and the undo stack.
            if (!exchange.offer.empty())
            {
                if (ImGui::Button("Выполнить в редакторе"))
                {
                    pendingOffer_ = exchange.offer;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("\"%s\"", exchange.offer.c_str());
            }
            ImGui::EndGroup();
            if (ImGui::BeginPopupContextItem("##copy"))
            {
                if (ImGui::MenuItem("Copy answer"))
                {
                    ImGui::SetClipboardText(exchange.verdict.c_str());
                }
                if (ImGui::MenuItem("Copy question and answer"))
                {
                    ImGui::SetClipboardText((exchange.phrase + "\n" + exchange.verdict).c_str());
                }
                if (ImGui::MenuItem("Copy the whole conversation"))
                {
                    std::string all;
                    for (const Exchange& entry : transcript_)
                    {
                        all += "you: " + entry.phrase + "\n" + entry.verdict + "\n\n";
                    }
                    ImGui::SetClipboardText(all.c_str());
                }
                ImGui::EndPopup();
            }
            else if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("right-click to copy");
            }
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
                // NOT the summary again. The newest transcript entry, two lines above this
                // one, already IS that sentence -- RecordOutcome put it there when the
                // preview landed -- so printing it here showed the person the same words
                // twice with a blank line between them.
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
                // WITH A NUMBER ON IT. A command turn used to take under four seconds and
                // a still line was fine; now that the model reasons first it takes ten to
                // fifteen, and a line that does not move for fifteen seconds is
                // indistinguishable from a hang -- the first thing asked on seeing it was
                // "опять висит?". The seconds are the difference between waiting and
                // wondering, and they cost one float.
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.75f, 1.0f));
                const double waited = ImGui::GetTime() - waitingSinceSec_;
                ImGui::TextWrapped("Asking the local model -- %.0fs%s. The editor keeps "
                    "running.", waited,
                    model_ && model_->Settings().commandReasoning ? ", it is thinking first"
                                                                  : "");
                ImGui::PopStyleColor();
            }
        }
        // FOLLOW THE ANSWER ONLY IF THEY ARE ALREADY AT THE BOTTOM. A streamed reply sets
        // this flag on every token, so an unconditional jump meant the view was yanked back
        // down about fifty times a second: scrolling up to re-read anything was impossible
        // for as long as the model was talking, which is exactly when there is something
        // worth re-reading. Scrolled up, the person is left where they put themselves and
        // the new text simply arrives below.
        if (transcriptScrollToBottom_)
        {
            constexpr float kAtBottom = 8.0f;   // a line's worth of slack, in pixels
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - kAtBottom)
            {
                ImGui::SetScrollHereY(1.0f);
            }
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

    // THE STANDING COST, on the row where the buttons are. Everything here was learned the
    // hard way in one evening: the card filled to 22.7 of 24.5 GB and the editor stuttered
    // until somebody guessed why; the context ran out with nothing on screen saying it was
    // filling; the session compacted with no warning that it was about to. All three are
    // now one line somebody can glance at instead of a thing they find out afterwards.
    if (model_ && model_->ServerIsUp())
    {
        std::uint64_t gpuUsed = 0, gpuTotal = 0;
        render::GpuMemoryTotals(gpuUsed, gpuTotal);
        const int prompt = model_->PromptTokens();
        const int context = model_->ContextTokens();
        const std::size_t chars = SessionChars();
        const std::size_t budget = SessionBudgetChars();

        std::string status;
        if (context > 0 && prompt > 0)
        {
            status += "context " + std::to_string((prompt * 100) / context) + "%";
        }
        if (gpuTotal > 0)
        {
            char vram[64] = {};
            std::snprintf(vram, sizeof(vram), "%svram %.1f/%.0f GB",
                status.empty() ? "" : "  ·  ",
                static_cast<double>(gpuUsed) / (1024.0 * 1024.0 * 1024.0),
                static_cast<double>(gpuTotal) / (1024.0 * 1024.0 * 1024.0));
            status += vram;
        }
        if (budget > 0)
        {
            status += (status.empty() ? "" : "  ·  ");
            status += chars >= budget
                ? "compacting next turn"
                : "compact in " + std::to_string((budget - chars) / 1000) + "k";
        }
        if (!status.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", status.c_str());
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("context: how much of the model's window the prompt, the "
                    "grammar and this level's remembered session take up together.\n"
                    "vram: the whole card, not just this process -- the model server is a "
                    "separate one and holds most of it.\n"
                    "compact: how much more can be remembered before the oldest exchanges "
                    "are folded into a list of what was actually done.");
            }
        }
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
