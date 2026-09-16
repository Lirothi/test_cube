#include "editor/ui/ModelChatPanel.h"
#if WITH_EDITOR

#include <string>
#include <vector>

#include "editor/intent/EditorActionRegistry.h"
#include "editor/intent/LlmIntentSource.h"
#include "editor/ui/ImGuiTextInput.h"
#include "imgui.h"
// ClearActiveID: an input keeps its own copy of the text while active, and emptying
// the bound string behind its back leaves the old text on screen.
#include "imgui_internal.h"

namespace
{
    // Reasoning models put their working in a <think> block before the answer. Split it out
    // rather than dropping it: it is the most interesting part of talking to one, and also
    // the part that would bury the answer if left inline.
    void SplitThinking(const std::string& raw, std::string& outThinking, std::string& outAnswer)
    {
        const std::size_t open = raw.find("<think>");
        const std::size_t close = raw.find("</think>");
        if (open == std::string::npos || close == std::string::npos || close < open)
        {
            outAnswer = raw;
            return;
        }
        outThinking = raw.substr(open + 7, close - open - 7);
        outAnswer = raw.substr(close + 8);

        const auto trim = [](std::string& text)
        {
            const std::size_t first = text.find_first_not_of(" \t\r\n");
            const std::size_t last = text.find_last_not_of(" \t\r\n");
            text = first == std::string::npos ? std::string{}
                                              : text.substr(first, last - first + 1);
        };
        trim(outThinking);
        trim(outAnswer);
    }
}

std::string ModelChatPanel::BuildPrompt(const LlmIntentSettings& settings,
    std::size_t& outDropped) const
{
    // A conversation has no natural end, so it eventually outgrows the context. Left
    // alone that surfaces as a server error several turns after the actual cause; trimming
    // the OLDEST exchanges keeps it working and the panel says how many went, so the model
    // does not merely appear to forget things.
    //
    // Room is reserved for the answer AND for the reasoning that precedes it, plus a
    // margin, because characters-per-token is an estimate. Being generous costs one
    // dropped exchange; being stingy costs a failed request nobody can explain.
    const int charsPerToken = settings.charsPerTokenEstimate > 0
        ? settings.charsPerTokenEstimate : 3;
    const int reserved = maxTokens_ + 512;
    const std::size_t promptTokenBudget = settings.contextTokens > reserved
        ? static_cast<std::size_t>(settings.contextTokens - reserved) : std::size_t{ 512 };
    const std::size_t charBudget = promptTokenBudget * static_cast<std::size_t>(charsPerToken);

    std::size_t used = system_.size() + 64;
    // Walk BACKWARDS: the newest exchanges are the ones the next answer depends on.
    std::size_t firstKept = turns_.size();
    while (firstKept > 0)
    {
        const std::size_t cost = turns_[firstKept - 1].text.size() + 32;
        if (used + cost > charBudget)
        {
            break;
        }
        used += cost;
        --firstKept;
    }
    outDropped = firstKept;

    // ChatML, applied here rather than through the server's chat endpoint, for the same
    // reason the command bar does it: the prefix stays byte-identical as the conversation
    // grows, so the server's prefill cache keeps everything but the newest exchange.
    //
    // The editable system message, plus what the editor actually has open. The second half
    // is not editable because it is not an opinion -- it is the state of the program, and
    // without it the model answers editor questions from imagination. Asked to select the
    // palms it produced pseudocode against a SceneManager that does not exist, which is the
    // correct answer for a model that has been told nothing.
    std::string text = "<|im_start|>system\n" + system_;
    if (!levelPath_.empty())
    {
        text += "\n\nRight now this editor has " + levelPath_ + " open, with " +
            std::to_string(objectCount_) + " objects in it.\n"
            "It also has a Command Bar, a separate window, which turns a phrase in any "
            "language into a real edit with a preview and an undo. It already handles: ";
        const std::vector<EditorActionDesc>& actions =
            EditorActionRegistry::Builtin().Actions();
        for (std::size_t index = 0; index < actions.size(); ++index)
        {
            if (index > 0)
            {
                text += ", ";
            }
            text.append(actions[index].id);
        }
        text += ".\nThose are internal names, NOT what gets typed. The Command Bar reads "
            "ordinary language in any tongue.\n"
            "YOU ARE A CONVERSATION FIRST. Answer normally, in their language: greetings, "
            "how you are, what things mean, why something looks wrong, graphics, maths, the "
            "engine, anything at all. Never write code for an editor action -- there is no "
            "API for them to call, and a plausible invented one is worse than saying you do "
            "not know.\n"
            "You have exactly two ways to reach the Command Bar, and both are LAST lines:\n"
            "  COMMAND: <phrase>   -- only when they plainly told you to DO it (\"сделай\", "
            "\"выполни\", \"давай\", \"go ahead\", or a bare instruction like \"выдели все "
            "пальмы\").\n"
            "  SUGGEST: <phrase>   -- when you notice the editor COULD do what they are "
            "talking about but they did not ask you to. Say your sentence first, then the "
            "SUGGEST line; they get a button and decide.\n"
            "THE PHRASE IS A SENTENCE, NOT AN ACTION NAME. The ids listed above are internal; "
            "the Command Bar reads ordinary language. Write what a person would say -- "
            "\"разверни пальмы случайно\", not \"randomizeRotation\" -- in the user's own "
            "language, naming what it applies to. Use the phrase THEY just used when they "
            "gave one, never one from earlier in this conversation.\n"
            "Most turns have NEITHER line. Wondering aloud, asking what something is, "
            "complaining that the palms look wrong -- those are conversation, and at most "
            "they earn a SUGGEST. When unsure, suggest rather than do: a suggestion costs a "
            "glance, a command costs undoing it.";
    }
    text += "<|im_end|>\n";
    for (std::size_t index = firstKept; index < turns_.size(); ++index)
    {
        const Turn& turn = turns_[index];
        text += turn.fromUser ? "<|im_start|>user\n" : "<|im_start|>assistant\n";
        text += (turn.promptText.empty() ? turn.text : turn.promptText) + "<|im_end|>\n";
    }
    text += "<|im_start|>assistant\n";
    if (!reasoning_)
    {
        // An already-closed, empty think block. The model finds its reasoning "done" and
        // goes straight to the answer -- the same trick the command bar uses, and the
        // reason a command comes back in seconds while a chat turn used to take half a
        // minute to say hello. It is part of the PROMPT, so the server's cache keeps it.
        text += "<think>\n\n</think>\n\n";
    }
    return text;
}

void ModelChatPanel::Send(LlmIntentSource& model)
{
    const std::string message = input_;
    if (message.find_first_not_of(" \t\r\n") == std::string::npos)
    {
        return;
    }

    error_.clear();
    if (!model.EnsureServerReady(serverStatus_))
    {
        // Not an error: a 38 GB model takes a moment. The message stays in the box so the
        // next press sends it rather than losing what was typed.
        return;
    }

    Turn mine;
    mine.fromUser = true;
    mine.text = message;
    turns_.push_back(std::move(mine));
    input_.clear();
    waiting_ = true;
    scrollToBottom_ = true;

    std::size_t dropped = 0;
    const std::string prompt = BuildPrompt(model.Settings(), dropped);
    droppedTurns_ = dropped;
    model.BeginFreeform(prompt, temperature_, maxTokens_);
}

std::string ModelChatPanel::TakePendingCommand()
{
    std::string phrase;
    phrase.swap(pendingCommand_);
    return phrase;
}

void ModelChatPanel::SetEditorContext(std::string levelPath, std::size_t objectCount)
{
    levelPath_ = std::move(levelPath);
    objectCount_ = objectCount;
}

bool ModelChatPanel::SendHeadless(LlmIntentSource& model, const std::string& phrase)
{
    // Through the same Send the button uses, for the same reason the command bar's headless
    // path does: a driver that built its own request would exercise a pipeline that does
    // not ship.
    input_ = phrase;
    Send(model);
    return waiting_;
}

void ModelChatPanel::Draw(LlmIntentSource& model, bool* open)
{
    // Picked up here, off the frame's critical path -- generation runs on its own thread.
    if (waiting_)
    {
        std::string text;
        std::string error;
        bool truncated = false;
        const IntentParseState state = model.PollFreeform(text, error, truncated);
        if (state == IntentParseState::Ready)
        {
            Turn turn;
            turn.fromUser = false;
            turn.truncated = truncated;
            SplitThinking(text, turn.thinking, turn.text);
            // Two trailing markers, and the difference between them is who decided.
            // COMMAND goes straight to the Command Bar because the user said to do it;
            // SUGGEST keeps the phrase on a button beside the sentence that proposed it,
            // and nothing happens until it is pressed.
            const auto takeMarker = [&](const char* marker, std::size_t markerLength)
            {
                std::string phrase;
                const std::size_t at = turn.text.rfind(marker);
                if (at == std::string::npos)
                {
                    return phrase;
                }
                const std::size_t from = at + markerLength;
                const std::size_t eol = turn.text.find('\n', from);
                phrase = turn.text.substr(from,
                    eol == std::string::npos ? std::string::npos : eol - from);
                const std::size_t a = phrase.find_first_not_of(" \t\r\n`\"");
                const std::size_t b = phrase.find_last_not_of(" \t\r\n`\"");
                phrase = a == std::string::npos ? std::string{} : phrase.substr(a, b - a + 1);
                if (!phrase.empty())
                {
                    // The marker line leaves the visible text: it is a protocol between the
                    // model and this panel, and showing it would make the conversation read
                    // like a transcript of that protocol.
                    turn.text.erase(at, eol == std::string::npos
                        ? std::string::npos : eol - at + 1);
                }
                return phrase;
            };

            const std::string commanded = takeMarker("COMMAND:", 8);
            const std::string suggested = takeMarker("SUGGEST:", 8);
            if (!commanded.empty())
            {
                pendingCommand_ = commanded;
                turn.text += (turn.text.empty() ? "" : "\n");
                turn.text += "-> Command Bar: \"" + commanded + "\"";
                // The history gets a plain acknowledgement instead, so the model reads its
                // previous turn as "that one was handled" rather than as a template to copy.
                turn.promptText = "Done -- that request was carried out in the editor.";
            }
            else if (!suggested.empty())
            {
                turn.suggestion = suggested;
                turn.promptText = turn.text + "\n(I offered to run: " + suggested + ")";
            }
            // A reasoning model can spend the entire budget thinking. Saying so beats
            // showing an empty answer under a fat "thinking" fold.
            if (turn.text.empty() && !turn.thinking.empty())
            {
                turn.text = "(the whole token budget went on thinking -- raise max tokens)";
            }
            turns_.push_back(std::move(turn));
            waiting_ = false;
            lastPartialSize_ = 0;
            scrollToBottom_ = true;
        }
        else if (state == IntentParseState::Failed || state == IntentParseState::Idle)
        {
            error_ = error.empty() ? "The model did not answer." : error;
            waiting_ = false;
            lastPartialSize_ = 0;
        }
    }

    // Offset from the Command Bar's first-use spot so the two do not open as one pile.
    if (const ImGuiViewport* viewport = ImGui::GetMainViewport())
    {
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.30f + 500.0f,
                   viewport->WorkPos.y + 40.0f), ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Model Chat", open))
    {
        ImGui::End();
        return;
    }

    if (!model.Available())
    {
        ImGui::TextWrapped("%s", model.UnavailableReason().c_str());
        ImGui::TextDisabled("Run: python tools/fetch_intent_model.py");
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("%s", model.StatusLine().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
    {
        turns_.clear();
        droppedTurns_ = 0;
        error_.clear();
        model.CancelFreeform();
        waiting_ = false;
        lastPartialSize_ = 0;
    }
    if (ImGui::CollapsingHeader("System message"))
    {
        editorui::InputTextMultiline("##chatSystem", system_, ImVec2(-1.0f, 60.0f));
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("temperature", &temperature_, 0.0f, 1.2f, "%.2f");
        ImGui::SameLine();
        ImGui::Checkbox("reasoning", &reasoning_);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Let the model think out loud before answering. It is most of "
                "the work -- \"привет\" spent 97%% of its tokens on it -- so leaving this "
                "off is the difference between a reply in seconds and one in half a minute. "
                "Turn it on when the reasoning is the part you want.");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        // The ceiling is the model's own room, not a number picked here. The budget is
        // reserved OUT of the context (see the trimming above), so a budget that ate the
        // whole context would leave nothing to hold the conversation and collapse the
        // prompt to the 512-token fallback -- the chat would forget everything, and the
        // cause would look like anything but this slider. Half the context, capped at the
        // 32768 that is already about fifty minutes of generation here.
        const int contextRoom = model.Settings().contextTokens / 2;
        const int budgetCeiling = contextRoom < 32768 ? contextRoom : 32768;
        if (maxTokens_ > budgetCeiling)
        {
            maxTokens_ = budgetCeiling;
        }
        ImGui::SliderInt("max tokens", &maxTokens_, 128, budgetCeiling);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("How long an answer may get. Nothing external imposes this -- "
                "the model runs on this machine. It is a TIME budget: every token costs real "
                "milliseconds, and this model thinks before it answers, so the reasoning "
                "comes out of the same allowance.");
        }
        // The cost in seconds, from the rate the LAST answer actually ran at. A tooltip
        // quoting a number measured on some other machine would be worse than nothing.
        const float rate = model.LastTokensPerSecond();
        if (rate > 0.0f)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("~%.0f s at %.0f tok/s",
                static_cast<float>(maxTokens_) / rate, rate);
        }
    }

    ImGui::Separator();

    const float inputHeight = ImGui::GetFrameHeightWithSpacing() * 3.0f;
    if (ImGui::BeginChild("##chatLog", ImVec2(0.0f, -inputHeight), true))
    {
        for (std::size_t index = 0; index < turns_.size(); ++index)
        {
            const Turn& turn = turns_[index];
            ImGui::PushID(static_cast<int>(index));
            if (turn.fromUser)
            {
                ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f), "you");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.65f, 0.95f, 0.65f, 1.0f), "model");
                if (!turn.thinking.empty() && ImGui::TreeNode("thinking"))
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                    ImGui::TextWrapped("%s", turn.thinking.c_str());
                    ImGui::PopStyleColor();
                    ImGui::TreePop();
                }
            }
            ImGui::TextWrapped("%s", turn.text.c_str());
            // An offer, not an action. The phrase sits on a button next to the sentence
            // that proposed it, because an offer that scrolls away from its own answer is
            // one nobody connects to anything.
            if (!turn.suggestion.empty())
            {
                if (ImGui::Button("Send to Command Bar"))
                {
                    pendingCommand_ = turn.suggestion;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("\"%s\"", turn.suggestion.c_str());
            }
            if (turn.truncated)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.24f, 1.0f),
                    "(cut off at the token budget -- raise max tokens and ask again)");
            }
            ImGui::Spacing();
            ImGui::PopID();
        }
        if (droppedTurns_ > 0)
        {
            // Say it, rather than let the model appear to forget things for no reason.
            ImGui::TextDisabled("(the %zu oldest exchange%s no longer fit the context and "
                "are not being sent)", droppedTurns_, droppedTurns_ == 1 ? "" : "s");
        }
        if (waiting_)
        {
            // The answer as far as it has arrived. Before streaming this said only
            // "thinking..." for the whole generation, which on a reasoning model is tens of
            // seconds of nothing for a one-word question -- and a wait with no sign of life
            // is read as a hang, correctly, because nothing distinguishes the two.
            const std::string partial = model.FreeformPartial();
            if (partial.empty())
            {
                ImGui::TextDisabled("thinking...");
            }
            else
            {
                // Reasoning arrives first and is part of the stream. Showing it raw would
                // bury the answer, so only the tail is shown while it runs; the finished
                // turn splits it into the fold like always.
                std::string shown = partial;
                const std::size_t close = shown.rfind("</think>");
                // With reasoning off there is no think block to be inside, so the label has
                // to come from the setting rather than from the absence of a closing tag --
                // otherwise it says "thinking..." over text that is plainly the answer.
                const bool stillThinking = reasoning_ && close == std::string::npos;
                if (close != std::string::npos)
                {
                    shown = shown.substr(close + 8);
                }
                else if (shown.rfind("<think>", 0) == 0)
                {
                    shown.erase(0, 7);
                }
                constexpr std::size_t kTail = 400;
                if (shown.size() > kTail)
                {
                    shown = shown.substr(shown.size() - kTail);
                }
                ImGui::TextDisabled("%s", stillThinking ? "thinking..." : "answering...");
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ImGui::TextWrapped("%s", shown.c_str());
                ImGui::PopStyleColor();
            }
            // Follow the stream, but only when it actually grew: scrolling every frame
            // would fight anyone trying to read back through the conversation.
            if (partial.size() != lastPartialSize_)
            {
                lastPartialSize_ = partial.size();
                scrollToBottom_ = true;
            }
        }
        if (!error_.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", error_.c_str());
        }
        if (!serverStatus_.empty() && !model.OwnsRunningServer())
        {
            ImGui::TextDisabled("%s", serverStatus_.c_str());
        }
        if (scrollToBottom_)
        {
            ImGui::SetScrollHereY(1.0f);
            scrollToBottom_ = false;
        }
    }
    ImGui::EndChild();

    if (focusInput_)
    {
        ImGui::SetKeyboardFocusHere();
        focusInput_ = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    // Ctrl+Enter sends; plain Enter is a newline, because a chat box that eats a paragraph
    // on the first line break is a chat box people stop using.
    editorui::InputTextMultiline("##chatInput", input_,
        ImVec2(-1.0f, ImGui::GetFrameHeightWithSpacing() * 2.0f));
    const bool submitted = ImGui::IsItemFocused() &&
        ImGui::IsKeyPressed(ImGuiKey_Enter) && ImGui::GetIO().KeyCtrl;

    ImGui::BeginDisabled(waiting_ || input_.empty());
    const bool pressed = ImGui::Button("Send  (Ctrl+Enter)");
    ImGui::EndDisabled();
    if (waiting_)
    {
        ImGui::SameLine();
        if (ImGui::Button("Stop"))
        {
            model.CancelFreeform();
            waiting_ = false;
            lastPartialSize_ = 0;
        }
    }

    if (pressed || submitted)
    {
        Send(model);
        // ImGui keeps its OWN copy of an input's text while that input is active, and
        // writes it back on deactivation. Send() empties the std::string behind the
        // widget's back, so without this the box goes on showing the sentence that was just
        // sent -- and the next Ctrl+Enter sends it a second time. Clearing the active id
        // makes the widget re-read the string it is actually bound to.
        ImGui::ClearActiveID();
        focusInput_ = true;
    }

    ImGui::End();
}

#endif // WITH_EDITOR
