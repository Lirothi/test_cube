#pragma once
#if WITH_EDITOR

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "editor/intent/EditorIntentSource.h"
#include "editor/intent/IntentSchema.h"
#include "editor/intent/LlmClient.h"

// Settings, persisted in editor_state.json under levelEditor.intentModel. The model is a
// PARAMETER, not an architectural decision (docs/editor_llm_plan.md): swapping it is a
// path change, not a rebuild, and neither the model nor the runtime lives in the repo.
struct LlmIntentSettings
{
    bool enabled = true;
    std::string modelPath;          // .gguf
    std::string serverExe;          // llama-server.exe
    std::string endpoint = "127.0.0.1:8127";
    bool autoStart = true;
    // 99 = every layer the card will take. With --cpu-moe the expert tensors -- nearly the
    // whole 38 GB -- stay in RAM, so what actually goes to the card is the dense and
    // attention weights plus the KV cache: about 7.5 GB, measured (6.7 -> 14.2 GB used on a
    // 24.5 GB card while the atoll renders).
    //
    // This was 0, on the reasoning that a phrase is typed once a minute and VRAM spent here
    // is VRAM the renderer does not have. Measurement disagreed: inside the running editor
    // the same phrase takes 5.0 s on the CPU and 2.2 s with the offload, and the first
    // token of a chat answer arrives in 33 s rather than 5 because the renderer and the
    // experts are fighting over the same cores. Set it back to 0 to give the card its
    // memory back; everything still works, just slower.
    int gpuLayers = 99;
    // The KV-cache allocation -- the only one of these numbers that costs memory, and it is
    // paid once at startup rather than per answer. Measured on this machine (private bytes,
    // and load time, which does not move at all):
    //
    //      -c   8192 ->  4.5 GB, 4.3 s        -c 131072 ->  7.4 GB, 4.3 s
    //      -c  32768 ->  5.3 GB, 4.3 s        -c 262144 -> 10.2 GB, 4.6 s
    //
    // Those figures are also the proof of the mmap claim made elsewhere: 4.5 GB of private
    // bytes against a 35.8 GB model means the weights are mapped, not allocated.
    //
    // 262144 is the model's own native context, and on a machine with this much RAM there
    // is no reason to ask it to forget anything. The 5.7 GB over the smallest setting buys
    // a conversation that never has to drop its own history.
    int contextTokens = 262144;
    // Inference threads. 0 leaves llama.cpp's own default, which is every core -- and the
    // engine wants every core too, for the render graph, the ocean sim and the wind. The
    // two fighting is why a phrase that takes 22 s on an idle machine took 141 s inside a
    // running editor. See the measurement in docs/editor_llm_plan.md before changing it.
    int threads = 0;
    // A HARD FAILURE BOUNDARY, not a comfort setting, so it is generous. The first request
    // of a session pays for the whole system prompt's prefill -- about 22 s here, and worse
    // if the model is sharing the GPU with the renderer (which is why gpuLayers defaults to
    // 0). 60 s looked reasonable and turned a slow first answer into "no response from the
    // server", which sends someone debugging a network problem that is really a queue.
    int timeoutSeconds = 300;
    // Estimated characters per token, for keeping a conversation inside the context. Mixed
    // Russian and English runs about 3; erring low means trimming a turn early rather than
    // discovering the overflow as a server error mid-sentence.
    int charsPerTokenEstimate = 3;
    std::string chatTemplate = "chatml";
    // Where a refusal's note for the implementer is appended. Empty turns the notes off.
    std::string apiRequestNotesPath = "docs/editor_api_requests.md";
    // llama-server ships a chat page, and it is ON by default. The command bar only ever
    // asks this model one narrow, grammar-constrained question; it is a capable general
    // model underneath, and making its own chat page reachable costs nothing.
    bool webUi = true;
    // Stop the server after this many seconds with no request from the editor. The server
    // is job-owned and so cannot outlive the editor at all; this is the other half -- an
    // editing session where nobody types a phrase for an hour should not hold the model.
    // 0 disables it. NOTE: the chat page does not count as activity, because the editor
    // cannot see requests it did not make -- raise this while chatting.
    //
    // 600, not the 60 it started at. Restarting costs 22 s on the GPU split -- the weights
    // have to go back across to the card -- and a minute is a perfectly ordinary pause in
    // the middle of a thought, so the old value charged that 22 s over and over for the
    // crime of thinking before typing. It is cheap to keep: the model is mmapped rather
    // than loaded, so an idle server holds reclaimable page cache, not committed RAM.
    int idleTimeoutSeconds = 600;
    // Let the server OUTLIVE the editor, and retire it after this long with no requests
    // from anybody. 0 keeps the old behaviour: the server is job-owned and dies with the
    // editor, no orphan possible by construction.
    //
    // Keeping it alive means giving that construction up, so the guarantee moves to a
    // watchdog process (llmreaper) launched beside it. That is a real trade and it is worth
    // naming: a machine that loses power mid-session, or a reaper killed from Task Manager,
    // leaves a 38 GB server with nobody watching. What it buys is the second launch of the
    // editor within the window starting instantly instead of paying 22 s to put the weights
    // back on the card.
    bool keepServerAfterExit = true;
    int keepAliveSeconds = 300;

    bool Configured() const { return !modelPath.empty() && !serverExe.empty(); }
};

// The half of the feature that understands what words MEAN (E2/E3/E4).
//
// Everything expensive happens on a detached worker: llama.cpp is reached over HTTP on
// localhost, and the frame never waits. The worker is deliberately NOT a task-system job
// -- tracked tasks are joined at the top of the next frame, which would hand the stall
// straight back to the thing this design exists to protect.
class LlmIntentSource final : public IEditorIntentSource
{
public:
    explicit LlmIntentSource(LlmIntentSettings settings);
    ~LlmIntentSource() override;

    std::string_view Name() const override { return "llm"; }
    bool Available() const override;
    std::string UnavailableReason() const override;

    void Begin(const std::string& phrase,
        const EditorIntentWorld& world,
        const std::vector<IntentTurn>& history) override;
    IntentParseState Poll(EditorIntent& outIntent, std::string& outWhyNot) override;
    void Cancel() override;
    void InvalidateWorld() override;

    const LlmIntentSettings& Settings() const { return settings_; }
    void SetSettings(LlmIntentSettings settings);

    // What the panel shows while the server is still loading a 38 GB file.
    std::string StatusLine() const;

    // A free-form request to the same server: no grammar, no action vocabulary, no
    // three-branch schema. This is what the chat panel runs on, and it shares the server
    // the command bar already owns -- a second one would mean a second 38 GB model.
    //
    // Using it counts as activity, so chatting keeps the idle timer from retiring the
    // server out from under the conversation.
    void BeginFreeform(const std::string& prompt, float temperature, int maxTokens);

    // Answer in PROSE, with the conversation so far, on the same cached system prompt.
    //
    // This is what a `chat` verdict turns into. It deliberately goes through the same
    // server, the same weights and the same prefix as a command: one 38 GB load, and the
    // expensive half of the prompt is already in the cache when it starts.
    //
    // `toolProtocol` is appended after the shared prefix -- the source search and the scene
    // queries a prose turn may ask for. Empty leaves them out.
    void BeginConversation(const std::vector<IntentTurn>& history,
        const std::string& phrase,
        const std::string& toolProtocol,
        float temperature,
        int maxTokens,
        bool reasoning);
    // `outTruncated` is set when the answer stopped because it hit the token budget rather
    // than because it finished. A reasoning model can spend the whole budget thinking, and
    // an answer that just stops mid-sentence with no explanation is the kind of quiet lie
    // the rest of this layer exists to avoid.
    IntentParseState PollFreeform(std::string& outText, std::string& outError, bool& outTruncated);

    // What has arrived so far on a chat request that is still running. Empty until the
    // first token lands, which is the point at which the wait stops looking like a hang.
    std::string FreeformPartial() const;

    // Generation rate of the last completed request, or 0 before there has been one.
    // Measured, not assumed: ~28 tok/s here with the experts on the CPU.
    float LastTokensPerSecond() const { return lastTokensPerSecond_; }
    void CancelFreeform();

    // Starts the server if it is not up yet; false while it is still loading, with a line
    // to show. The chat needs this because, unlike the command bar, it may be the first
    // thing in a session to want the model.
    bool EnsureServerReady(std::string& outStatus);

    enum class NoteState { None, Writing, Written, Failed };

    // A refusal fires a SECOND, ungrammared request: the short JSON answer is for the
    // designer, the prose note is for whoever extends the editor. It runs on its own
    // worker and nothing waits for it -- if it never lands, the refusal still stood.
    NoteState NoteStatus() const;
    const std::string& NotePath() const { return settings_.apiRequestNotesPath; }

    // The model's last raw answer, which becomes the assistant half of the next turn when
    // the user replies to an `unclear` question.
    const std::string& LastRawAnswer() const { return lastRawAnswer_; }

    // http://<endpoint> when the server was started with its chat page enabled.
    std::string WebUiUrl() const;

    // Start the server and prefill the system prompt, so the first phrase of a session does
    // not pay for both at once. Measured, that first phrase costs 33 s against 1.5 s for
    // every later one, and nearly all of the difference is prefill of a 16 KB prompt that
    // never changes. Call it every frame until it returns true; it does nothing at all when
    // the model is not on disk, and nothing twice.
    //
    // The warmed prefix must be BYTE-IDENTICAL to what a real phrase sends or the server's
    // cache will not match it, which is why this goes through the same template.
    bool BeginWarmup(const EditorIntentWorld& world);

    // A request someone is WAITING for -- a command or a chat turn. The frame loop uses this
    // to stop rendering flat out while the model works; see App.h g_modelBusy.
    bool Busy() const;

    // Work nobody asked for and nobody is watching: the startup warmup, and the note a
    // refusal writes for whoever extends the editor. Kept apart from Busy() because the
    // answer differs. Throttling hard for a phrase someone just typed trades frames they
    // are not looking at for an answer they are; doing the same for 44 s of warmup right
    // after the editor opens trades frames they ARE looking at for nothing they asked for.
    bool BusyInBackground() const;

    // Called every frame the panel is open. Enforces the idle timeout; costs a clock read.
    void Tick();

    // Seconds until the idle timeout fires, or -1 when it is off or nothing is running.
    double SecondsUntilIdleStop() const;

    // True when THIS editor started the server that is running, so the panel can offer to
    // stop it. A server the user started themselves is never offered up.
    bool OwnsRunningServer() const;

    // Something is answering on the endpoint, whoever started it. Different from
    // OwnsRunningServer, which is only true for a server THIS session launched -- and the
    // difference is visible to the user the moment a kept-alive server outlives an editor.
    bool ServerIsUp() const { return serverHealthy_; }

    // Force it up or down, for the buttons in the settings. Stop works on a server this
    // editor did not start; Start clears the session latch that stops the watchdog being
    // retried, because pressing it is somebody saying "try again".
    bool StopServer(std::string& outStatus);
    bool StartServerNow(std::string& outStatus);
    // Stop the server we started. The model is mmapped, so an idle one costs reclaimable
    // page cache rather than committed RAM -- but "reclaimable" is still not "gone", and
    // someone about to do something memory-hungry should not have to find Task Manager.
    void StopServer();

    // The generated grammar and prompt, for the settings UI and for the gate. Built once
    // per level; see InvalidateWorld.
    const std::string& Gbnf() const { return gbnf_; }
    const std::string& SystemPrompt() const { return systemPrompt_; }

    static LlmIntentSettings LoadSettings(const nlohmann::json& levelEditorJson);
    static nlohmann::json SaveSettings(const LlmIntentSettings& settings);

private:
    // Shared with the worker so the source can be destroyed, or the request abandoned,
    // while inference is still running.
    struct Request
    {
        std::atomic<int> state{ 0 };   // IntentParseState
        std::atomic<bool> abandoned{ false };
        std::string answer;
        std::string error;
        bool truncated = false;
        // Written by the worker as tokens arrive, read by the frame. The mutex is held for
        // a string append and a copy, both trivial next to a token's arrival interval.
        std::mutex partialMutex;
        std::string partial;
        // From the server's own timings. Turning a token budget into seconds needs a real
        // rate, and assuming one would be exactly the kind of guess this file avoids.
        float tokensPerSecond = 0.0f;
    };

    void EnsureWorld(const EditorIntentWorld& world);
    bool EnsureServer(std::string& outStatus);

    LlmIntentSettings settings_;
    std::shared_ptr<Request> request_;
    llmclient::ServerProcess server_;
    bool serverOwned_ = false;
    // This server was started to OUTLIVE us, and a watchdog is minding it. The destructor
    // must then leave it alone, and the in-editor idle timer must stay out of the way too:
    // two things retiring the same server is how one of them kills it mid-answer.
    bool keepsServerAlive_ = false;
    // Set once, when the watchdog could not be launched. It fails for reasons that hold for
    // the whole session -- a missing binary, or this process sitting in a job object that
    // forbids CREATE_BREAKAWAY_FROM_JOB -- so retrying is a loop that starts and kills a
    // 38 GB server once a second and never succeeds. After this the server is job-owned:
    // it dies with the editor, which is the arrangement that never needed a watchdog.
    bool reaperUnavailable_ = false;
    std::string serverStatus_;
    double nextHealthPollSec_ = 0.0;
    double lastUseSec_ = 0.0;
    bool serverHealthy_ = false;

    std::string lastRawAnswer_;
    float lastTokensPerSecond_ = 0.0f;
    // The phrase and level the current request is about, kept so a refusal's note can name
    // them without the caller having to hand them back.
    std::string lastPhrase_;
    std::string levelPathForNotes_;
    std::shared_ptr<Request> noteRequest_;
    std::shared_ptr<Request> freeformRequest_;
    // A request that arrived before the server was up. "Still loading a 38 GB model" is not
    // a failure, it is "not yet" -- reporting it as failure made the panel say "Not
    // understood: Starting llama-server..." and made the headless harness give up on its
    // first frame. The request waits here and is sent the moment /health answers.
    bool pendingServerStart_ = false;
    std::string pendingPhrase_;
    std::vector<IntentTurn> pendingHistory_;
    void SendPending();
    void RequestDeveloperNote(const std::string& phrase, const EditorIntent& refusal);
    bool warmedUp_ = false;
    bool worldBuilt_ = false;
    intentschema::Vocabulary vocabulary_;
    std::string gbnf_;
    std::string systemPrompt_;
};

#endif // WITH_EDITOR
