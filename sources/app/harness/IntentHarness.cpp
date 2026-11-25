#include "app/harness/IntentHarness.h"

#include "app/BootOptions.h"
#include "core/Helpers.h" // GetTimeSeconds
#include "core/logging/Log.h"
#include "imgui.h" // GetTime: a pass's time, as the editor measured it

namespace harness
{
void IntentHarness::OnFrame(FrameContext& ctx)
{
    if (!started_) { started_ = true; start_ = GetTimeSeconds(); }
    if (Boot().harness.intentFinished || (!Boot().harness.intentPhrase.empty() &&
        GetTimeSeconds() - start_ >= Boot().harness.intentTimeoutSec))
    {
        if (!Boot().harness.intentFinished)
        {
            LOG_ERROR(logging::LogCategory::Editor,
                "intent harness: no verdict after {:.0f}s", Boot().harness.intentTimeoutSec);
        }
        // ...unless a --shot is still pending. What a command DID is the interesting
        // part, and quitting on the verdict meant the only photograph that could ever
        // be taken was of the moment before it happened.
        //
        // A pending --profdump holds it open for a different reason: the question
        // that pairing answers is what the MODEL costs the frame while it is
        // thinking, and quitting the moment the verdict lands threw the dump away
        // unwritten. Size --intent-repeat so the passes outlast --shot-delay, or the
        // dump measures an idle editor and says inference is free.
        if (Boot().frame.shotPath.empty() && Boot().frame.profDumpPath.empty())
        {
            ctx.running = false;
        }
    }
}

namespace
{
// What EditorController held as headless* members until R3.3f.
struct EditorScriptState
{
    // "--intent=<phrase>" is in flight: submitted, waiting for the source to settle.
    bool intentPending = false;
    // Editor frames drawn since boot. The harness waits for a few before submitting: firing on frame zero ran a command
    // while the renderer was still standing its resources up, and took the process with it.
    int intentWarmupFrames = 0;
    // --intent-repeat: which pass this is, the phrase to repeat, and when it was submitted. The elapsed time is the whole
    // point of repeating -- the first pass carries the system prompt's prefill and no later one does.
    int intentPass = 0;
    std::string intentPhrase;
    double intentStartedSec = 0.0;
    // True from the first pass's submission until the last one's verdict. Separate from intentPending, which is only
    // true while ONE pass is in flight -- without this the run silently ended after pass 1.
    bool intentRunning = false;
    // Same warmup reasoning, counted separately because --chat can be used without --intent.
    int chatWarmupFrames = 0;
    // True when `--then=` was given: the harness then presses Run for every command it produces, because there is nobody
    // there to press it. Latched at startup so it stays a harness behaviour.
    bool runsCommands = false;
    // `--then=`: the second phrase waits for the first to finish, and "finished" is a busy->idle transition.
    bool chatSawBusy = false;
    int chatSettleFrames = 0;
};

EditorScriptState& State()
{
    static EditorScriptState state;
    return state;
}
}

void EditorIntentFrame(EditorScriptHost& host)
{
    EditorScriptState& s = State();
    // "--intent=<phrase>": the headless harness. Submitted once the editor has its level and
    // its asset registry, then polled every frame -- the model answers on its own thread and
    // this must not block one. App quits as soon as the verdict is logged.
    // s.intentRunning is what keeps a multi-pass run alive. The two obvious flags do
    // not: Boot().harness.intentPhrase is cleared when a pass is SUBMITTED and s.intentPending
    // drops when it COMPLETES, so with both as the gate the block stopped executing after
    // pass 1, Boot().harness.intentFinished never got set, and the process sat out its whole timeout
    // while the idle timer reaped the server underneath it.
    if (!Boot().harness.intentPhrase.empty() || s.intentPending || s.intentRunning)
    {
        ++s.intentWarmupFrames;
        // 30 frames of warmup: the level's runtime objects, the shadow resources and the
        // acceleration structures all finish arriving over the first few frames, and a
        // command executed before then takes the process down with it.
        constexpr int kHeadlessIntentWarmupFrames = 30;
        if (s.intentWarmupFrames < kHeadlessIntentWarmupFrames)
        {
            // fall through to the rest of Draw; try again next frame
        }
        else if (s.intentPending)
        {
            // Polling belongs to a pass that is actually in flight. Ordering this AFTER the
            // submit branch, as it was, meant that any frame where the submit conditions
            // were not met fell through to here and polled nothing -- which logged a verdict
            // of "pass 0/2" every frame, several hundred times, the moment a gate was added.
            std::string verdict;
            if (host.PollIntent(Boot().harness.intentRun, verdict))
            {
                s.intentPending = false;
                const double elapsed = ImGui::GetTime() - s.intentStartedSec;
                LOG_INFO(logging::LogCategory::Editor,
                    "intent harness pass {}/{} in {:.1f}s: {}",
                    s.intentPass, Boot().harness.intentRepeat, elapsed, verdict);
                if (s.intentPass >= Boot().harness.intentRepeat)
                {
                    s.intentRunning = false;
                    Boot().harness.intentFinished = true;
                }
            }
        }
        else if (host.ModelWarmed() && !host.ModelBusy())
        {
            // Not until the warmup has been SENT and has COME BACK. Both halves are needed.
            // llama-server answers one request at a time, so a phrase submitted during the
            // warmup queues behind it and the harness reports that wait as the phrase's own
            // cost. Waiting on Busy() alone is not enough either: for the first thirty frames
            // the server is still starting, nothing is in flight, and a phrase submitted then
            // is simply PARKED -- to be sent the instant the server answers, in a dead heat
            // with the warmup. That is how the first two attempts at measuring this came out
            // slower than no warmup at all.
            if (!Boot().harness.intentPhrase.empty())
            {
                s.intentPhrase = Boot().harness.intentPhrase;
                Boot().harness.intentPhrase.clear();
                LOG_INFO(logging::LogCategory::Editor, "intent harness: \"{}\" on level {} "
                    "({} pass(es))",
                    s.intentPhrase, host.LevelPath(),
                    Boot().harness.intentRepeat);
            }
            s.intentPending = true;
            s.intentRunning = true;
            ++s.intentPass;
            s.intentStartedSec = ImGui::GetTime();
            host.BeginIntent(s.intentPhrase);
        }
    }
}

void EditorChatFrame(EditorScriptHost& host)
{
    EditorScriptState& s = State();
    // "--chat=<phrase>": press Send once the editor is warm, then leave it alone -- the
    // panel polls its own stream. Retried every frame until it goes out, because the first
    // press only gets as far as "the server is still loading a 38 GB file".
    s.runsCommands = s.runsCommands || !Boot().harness.chatThenPhrase.empty();
    if (!Boot().harness.chatPhrase.empty())
    {
        if (++s.chatWarmupFrames >= 30 && host.ModelWarmed() && !host.ModelBusy())
        {
            // Through the same box everything else goes through. `--chat=` stays as a flag
            // because a conversational answer is the one thing no gate can look at -- it
            // exists only while it is streaming -- but there is no second panel behind it.
            host.ShowCommandBar();
            host.SubmitPhrase(Boot().harness.chatPhrase);
            LOG_INFO(logging::LogCategory::Editor, "chat harness: sent \"{}\"", Boot().harness.chatPhrase);
            Boot().harness.chatPhrase.clear();
        }
    }
    // "--then=<phrase>": a SECOND phrase in the SAME editor session, once the first has
    // finished. It exists because what the model remembers can only be tested inside one
    // session -- a second process starts with an empty one by construction -- and the case
    // it was written for is "сгруппируй пальмы" followed by "и что ты сделал?".
    //
    // The first phrase's preview is RUN here: a command that only ever got previewed did
    // not happen, and a question about what happened would be answered correctly by saying
    // nothing did.
    // PRESSES RUN FOR EVERY COMMAND, not only the first. Once a `--then` phrase is on the
    // table this session is a sequence of edits with nobody at the keyboard, and a second
    // command that only ever reaches its preview has not happened -- which makes the whole
    // point of a two-step run ("scatter the palms, THEN the rocks so they miss the palms")
    // impossible to check. `s.runsCommands` is latched at startup so this stays a
    // harness behaviour and never becomes a thing the editor does to somebody.
    else if (s.runsCommands && host.PreviewAwaitsRun())
    {
        host.RunPendingPreview();
        LOG_INFO(logging::LogCategory::Editor, "chat harness: pressed Run");
    }
    else if (!Boot().harness.chatThenPhrase.empty())
    {
        if (host.ModelBusy())
        {
            s.chatSawBusy = true;
        }
        else if (s.chatSawBusy && ++s.chatSettleFrames >= 30)
        {
            host.SubmitPhrase(Boot().harness.chatThenPhrase);
            LOG_INFO(logging::LogCategory::Editor, "chat harness: then sent \"{}\"",
                Boot().harness.chatThenPhrase);
            Boot().harness.chatThenPhrase.clear();
        }
    }
}
}
