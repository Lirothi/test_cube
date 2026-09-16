#include "editor/intent/LlmIntentSource.h"
#if WITH_EDITOR

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include "core/logging/Log.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/intent/EditorActionRegistry.h"
#include "editor/intent/IntentNotes.h"
#include "editor/intent/IntentPrompt.h"
#include "editor/scene/EditorSceneDocument.h"

namespace
{
    double NowSeconds()
    {
        using Clock = std::chrono::steady_clock;
        return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    }

    bool FileExists(const std::string& path)
    {
        if (path.empty())
        {
            return false;
        }
        std::error_code ec;
        return std::filesystem::exists(std::filesystem::path(path), ec);
    }

    std::string ReadStringOr(const nlohmann::json& object, const char* key, const std::string& fallback)
    {
        const auto it = object.find(key);
        return (it != object.end() && it->is_string()) ? it->get<std::string>() : fallback;
    }

    int ReadIntOr(const nlohmann::json& object, const char* key, int fallback)
    {
        const auto it = object.find(key);
        return (it != object.end() && it->is_number()) ? it->get<int>() : fallback;
    }

    bool ReadBoolOr(const nlohmann::json& object, const char* key, bool fallback)
    {
        const auto it = object.find(key);
        return (it != object.end() && it->is_boolean()) ? it->get<bool>() : fallback;
    }

    // Did the answer stop because it ran out of budget, rather than because it was finished?
    //
    // THIS WAS READING A FIELD THE SERVER DOES NOT SEND. The check was `stopped_limit`, which
    // is not in llama-server's reply at all any more -- so the flag was false every time and
    // an answer that hit n_predict lost its tail in silence, in the chat and in the command
    // bar alike. Probed against the running server: the reply carries `stop_type`, one of
    // "eos", "word" or "limit".
    //
    // NOT the field called `truncated`, tempting as the name is: that one means the PROMPT
    // was cut to fit the context, which is a different failure with a different fix.
    bool HitTheTokenLimit(const nlohmann::json& reply)
    {
        const auto it = reply.find("stop_type");
        if (it != reply.end() && it->is_string())
        {
            return it->get<std::string>() == "limit";
        }
        // Older llama-server builds, which said it this way instead.
        return ReadBoolOr(reply, "stopped_limit", false);
    }
}

LlmIntentSource::LlmIntentSource(LlmIntentSettings settings)
    : settings_(std::move(settings))
{
}

LlmIntentSource::~LlmIntentSource()
{
    Cancel();
    // Only a server THIS process started is ours to stop -- one the user already had
    // running is theirs. Closing the job handle is what actually guarantees it goes, and
    // it happens here on a clean exit and by the OS on any other kind.
    if (serverOwned_ && !keepsServerAlive_)
    {
        server_.Terminate();
    }
    else if (keepsServerAlive_)
    {
        LOG_INFO(logging::LogCategory::Editor,
            "intent model: leaving llama-server pid={} running; its watchdog retires it "
            "after {}s with no requests", server_.processId, settings_.keepAliveSeconds);
    }
}

bool LlmIntentSource::Available() const
{
    return settings_.enabled && settings_.Configured() &&
        FileExists(settings_.modelPath) && FileExists(settings_.serverExe);
}

std::string LlmIntentSource::UnavailableReason() const
{
    if (!settings_.enabled)
    {
        return "Local model is switched off in settings.";
    }
    if (settings_.modelPath.empty() || settings_.serverExe.empty())
    {
        return "No model configured. Run tools/fetch_intent_model.py, then set the paths below.";
    }
    if (!FileExists(settings_.modelPath))
    {
        return "Model file not found: " + settings_.modelPath;
    }
    if (!FileExists(settings_.serverExe))
    {
        return "llama-server.exe not found: " + settings_.serverExe;
    }
    return {};
}

void LlmIntentSource::SetSettings(LlmIntentSettings settings)
{
    const bool endpointChanged = settings.endpoint != settings_.endpoint ||
        settings.modelPath != settings_.modelPath ||
        settings.gpuLayers != settings_.gpuLayers ||
        settings.webUi != settings_.webUi;
    settings_ = std::move(settings);
    if (endpointChanged && serverOwned_)
    {
        server_.Terminate();
        serverOwned_ = false;
        serverHealthy_ = false;
    }
}

void LlmIntentSource::InvalidateWorld()
{
    worldBuilt_ = false;
}

void LlmIntentSource::EnsureWorld(const EditorIntentWorld& world)
{
    if (worldBuilt_)
    {
        return;
    }
    levelPathForNotes_ = world.document.LevelPath();
    vocabulary_ = intentschema::BuildVocabulary(world.document, world.assets);
    gbnf_ = intentschema::BuildGbnf(vocabulary_);
    systemPrompt_ = intentprompt::BuildSystemPrompt(world.document, world.assets, vocabulary_);
    worldBuilt_ = true;
    LOG_INFO(logging::LogCategory::Editor,
        "intent model: grammar rebuilt for level -- {} filter names, {} spawnable assets, "
        "{} bytes of grammar",
        vocabulary_.needles.size(), vocabulary_.assets.size(), gbnf_.size());
}

bool LlmIntentSource::EnsureServer(std::string& outStatus)
{
    if (serverHealthy_)
    {
        return true;
    }

    // Health is polled, never waited on: the first load of a 38 GB model takes a while and
    // the editor has frames to draw in the meantime.
    const double now = NowSeconds();
    if (now < nextHealthPollSec_)
    {
        outStatus = serverStatus_;
        return false;
    }
    nextHealthPollSec_ = now + 1.0;

    const llmclient::Response health = llmclient::Get(settings_.endpoint, "/health", 2);
    if (health.ok)
    {
        serverHealthy_ = true;
        serverStatus_.clear();
        LOG_INFO(logging::LogCategory::Editor, "intent model: server healthy on {}",
            settings_.endpoint);
        return true;
    }

    if (serverOwned_ && server_.Running())
    {
        // HTTP 503 while loading is llama-server's normal answer, not a failure.
        serverStatus_ = "Model loading...";
        outStatus = serverStatus_;
        return false;
    }
    if (serverOwned_ && !server_.Running())
    {
        serverOwned_ = false;
        serverStatus_ = "llama-server exited; check the model path";
        outStatus = serverStatus_;
        return false;
    }

    if (!settings_.autoStart)
    {
        serverStatus_ = "No server on " + settings_.endpoint + " (auto-start is off)";
        outStatus = serverStatus_;
        return false;
    }

    std::string error;
    // `reaperUnavailable_` is why this is not simply the setting. A watchdog that could not
    // be started once will not start on the next frame either -- it fails for reasons that
    // do not change within a session, such as this process sitting in a job object that
    // forbids breakaway -- so asking again every frame buys nothing and costs everything.
    const bool keepAlive = settings_.keepServerAfterExit && settings_.keepAliveSeconds > 0 &&
        !reaperUnavailable_;
    server_ = llmclient::StartServer(settings_.serverExe, settings_.modelPath,
        settings_.endpoint, settings_.gpuLayers, settings_.contextTokens,
        settings_.threads, settings_.webUi, keepAlive, error);
    if (!server_.handle)
    {
        serverStatus_ = error;
        outStatus = serverStatus_;
        return false;
    }
    if (keepAlive)
    {
        // The guarantee moved out of the job object, so it has to be picked up here in the
        // same breath: a kept-alive server with no watchdog is the 38 GB orphan this whole
        // arrangement exists to prevent.
        //
        // BUT THE ANSWER IS TO FALL BACK, NOT TO REFUSE. The first version terminated the
        // server and returned false, leaving `serverOwned_` unset -- so the next poll
        // started another one, failed again, killed it again, and did that once a second
        // forever. The editor never got a model and the headless harness sat there until
        // its 180 s deadline with no verdict, while a 38 GB process was created and
        // destroyed under it the whole time. A permanent failure handled as a temporary one
        // is worse than either.
        //
        // The fallback is the arrangement that came before the watchdog and needed no
        // watchdog: job-owned, dies with the editor. It loses `keepServerAfterExit` and
        // nothing else, and it cannot orphan anything by construction.
        std::string reaperError;
        if (!llmclient::StartReaper(settings_.endpoint, server_.processId,
                settings_.keepAliveSeconds, reaperError))
        {
            server_.Terminate();
            reaperUnavailable_ = true;
            LOG_WARNING(logging::LogCategory::Editor,
                "intent model: {} -- falling back to a job-owned server that dies with the "
                "editor; it will not outlive this session", reaperError);
            server_ = llmclient::StartServer(settings_.serverExe, settings_.modelPath,
                settings_.endpoint, settings_.gpuLayers, settings_.contextTokens,
                settings_.threads, settings_.webUi, false, error);
            if (!server_.handle)
            {
                serverStatus_ = error;
                outStatus = serverStatus_;
                return false;
            }
        }
    }
    serverOwned_ = true;
    // Not `keepAlive`: the fallback above may have started a job-owned server instead, and
    // this flag decides whether the editor leaves it running on the way out and whether the
    // idle stop applies. Saying it keeps a server alive when it does not would strand the
    // idle timeout on a process that dies with us anyway.
    keepsServerAlive_ = keepAlive && !reaperUnavailable_;
    lastUseSec_ = NowSeconds();
    serverStatus_ = "Starting llama-server...";
    outStatus = serverStatus_;
    return false;
}

void LlmIntentSource::Begin(const std::string& phrase,
    const EditorIntentWorld& world,
    const std::vector<IntentTurn>& history)
{
    Cancel();

    if (!Available())
    {
        return;
    }
    EnsureWorld(world);

    std::string status;
    if (!EnsureServer(status))
    {
        // NOT A FAILURE. Loading a 38 GB model takes a moment, and calling that "failed"
        // made the panel say "Not understood: Starting llama-server..." and made the
        // headless harness give up on its first frame. Park the request; Poll sends it the
        // moment /health answers.
        pendingServerStart_ = true;
        pendingPhrase_ = phrase;
        pendingHistory_ = history;
        request_ = std::make_shared<Request>();
        request_->state.store(static_cast<int>(IntentParseState::Pending));
        lastPhrase_ = phrase;
        return;
    }

    pendingPhrase_ = phrase;
    pendingHistory_ = history;
    SendPending();
}

void LlmIntentSource::SendPending()
{
    const std::string& phrase = pendingPhrase_;
    const std::vector<IntentTurn>& history = pendingHistory_;
    pendingServerStart_ = false;

    nlohmann::json body;
    body["prompt"] = intentprompt::ApplyChatTemplate(systemPrompt_, phrase,
        settings_.chatTemplate, history);
    body["grammar"] = gbnf_;
    // Deterministic: the same phrase on the same level must not mean two different things
    // on two days.
    body["temperature"] = 0.0;
    body["top_k"] = 1;
    // With a grammar in place, generation ends when the JSON closes; this is only a cap
    // against a runaway, so raising it costs nothing that is not already going wrong.
    body["n_predict"] = 1024;
    // The system prompt is identical between requests, so the server keeps its prefill
    // and each phrase costs a dozen input tokens instead of the whole vocabulary (E4).
    body["cache_prompt"] = true;
    body["stream"] = false;

    auto request = std::make_shared<Request>();
    request->state.store(static_cast<int>(IntentParseState::Pending));
    request_ = request;
    lastUseSec_ = NowSeconds();

    lastPhrase_ = phrase;
    const std::string endpoint = settings_.endpoint;
    const int timeout = settings_.timeoutSeconds;
    const std::string payload = body.dump();

    // Detached, and outside the engine's task system on purpose: a tracked task is joined
    // at the top of the next frame, which would give the stall straight back.
    std::thread([request, endpoint, timeout, payload]()
    {
        const llmclient::Response response =
            llmclient::PostJson(endpoint, "/completion", payload, timeout);
        if (request->abandoned.load())
        {
            return;
        }
        if (!response.ok)
        {
            request->error = response.error.empty() ? "no answer from the local model"
                                                    : response.error;
            request->state.store(static_cast<int>(IntentParseState::Failed));
            return;
        }

        const nlohmann::json parsed = nlohmann::json::parse(response.body, nullptr, false);
        if (parsed.is_discarded() || !parsed.contains("content"))
        {
            request->error = "unexpected reply shape from llama-server";
            request->state.store(static_cast<int>(IntentParseState::Failed));
            return;
        }
        request->answer = parsed["content"].get<std::string>();
        request->state.store(static_cast<int>(IntentParseState::Ready));
    }).detach();
}

IntentParseState LlmIntentSource::Poll(EditorIntent& outIntent, std::string& outWhyNot)
{
    if (!request_)
    {
        return IntentParseState::Idle;
    }

    if (pendingServerStart_)
    {
        std::string status;
        if (!EnsureServer(status))
        {
            if (!server_.handle && !serverOwned_ && !settings_.autoStart)
            {
                // Auto-start is off and nothing is listening: that IS a failure, and
                // waiting for a server nobody is going to start would hang instead.
                pendingServerStart_ = false;
                outWhyNot = status.empty() ? "Local model not ready" : status;
                request_.reset();
                return IntentParseState::Failed;
            }
            return IntentParseState::Pending;
        }
        SendPending();
        return IntentParseState::Pending;
    }

    const auto state = static_cast<IntentParseState>(request_->state.load());
    if (state == IntentParseState::Pending)
    {
        return IntentParseState::Pending;
    }
    if (state == IntentParseState::Failed)
    {
        outWhyNot = request_->error;
        request_.reset();
        return IntentParseState::Failed;
    }

    const std::string answer = request_->answer;
    request_.reset();
    lastRawAnswer_ = answer;

    std::string error;
    if (!intentschema::ParseAnswer(answer, outIntent, error))
    {
        // The grammar makes this impossible, which is exactly why it is checked: if it
        // ever fires, the grammar and the reader have drifted.
        LOG_WARNING(logging::LogCategory::Editor,
            "intent model: unreadable answer ({}) -- raw: {}", error, answer);
        outWhyNot = "The model's answer did not fit the schema: " + error;
        return IntentParseState::Failed;
    }

    LOG_INFO(logging::LogCategory::Editor, "intent model: answered {}", answer);
    if (outIntent.kind == EditorIntentKind::NeedsApi)
    {
        RequestDeveloperNote(lastPhrase_, outIntent);
    }
    return IntentParseState::Ready;
}

void LlmIntentSource::Cancel()
{
    pendingServerStart_ = false;
    pendingPhrase_.clear();
    pendingHistory_.clear();
    if (request_)
    {
        request_->abandoned.store(true);
        request_.reset();
    }
}

bool LlmIntentSource::BeginWarmup(const EditorIntentWorld& world)
{
    if (warmedUp_)
    {
        return true;
    }
    // Not on disk, switched off, or no llama-server: nothing to warm, and nothing to
    // complain about either -- an editor without the model is a supported way to work.
    if (!Available())
    {
        return true;
    }
    EnsureWorld(world);

    std::string status;
    if (!EnsureServer(status))
    {
        return false;   // still mapping 38 GB; ask again next frame
    }
    warmedUp_ = true;

    nlohmann::json body;
    // The same template a real phrase uses, with an empty one in it. What is being cached
    // is the system prompt in front of it, which is all but a dozen tokens of the cost.
    body["prompt"] = intentprompt::ApplyChatTemplate(systemPrompt_, std::string{},
        settings_.chatTemplate, {});
    // One token, because the answer is worthless and the prefill is the entire point.
    body["n_predict"] = 1;
    body["temperature"] = 0.0;
    body["cache_prompt"] = true;
    body["stream"] = false;

    auto request = std::make_shared<Request>();
    request->state.store(static_cast<int>(IntentParseState::Pending));
    // Deliberately NOT request_: nothing polls this and nothing must see it as an answer.
    // It is fire-and-forget, and if it never lands the first real phrase simply pays the
    // prefill it would have paid anyway.
    noteRequest_ = request;
    lastUseSec_ = NowSeconds();

    const std::string endpoint = settings_.endpoint;
    const int timeout = settings_.timeoutSeconds;
    const std::string payload = body.dump();
    const auto started = std::chrono::steady_clock::now();
    std::thread([request, endpoint, timeout, payload, started]()
    {
        const llmclient::Response response =
            llmclient::PostJson(endpoint, "/completion", payload, timeout);
        request->state.store(static_cast<int>(
            response.ok ? IntentParseState::Ready : IntentParseState::Failed));
        LOG_INFO(logging::LogCategory::Editor, "intent model: prompt warmed in {:.1f}s{}",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
            response.ok ? "" : " (FAILED -- the first phrase will pay for the prefill)");
    }).detach();
    return true;
}

bool LlmIntentSource::Busy() const
{
    const auto pending = [](const std::shared_ptr<Request>& r)
    {
        return r && static_cast<IntentParseState>(r->state.load()) == IntentParseState::Pending;
    };
    // Someone is WAITING. pendingServerStart_ counts: the phrase is parked while a 38 GB
    // file maps, and that load is exactly as entitled to the cores as the generation after
    // it. The warmup and the developer note are deliberately NOT here -- see BusyInBackground.
    return pendingServerStart_ || pending(request_) || pending(freeformRequest_);
}

bool LlmIntentSource::BusyInBackground() const
{
    const auto pending = [](const std::shared_ptr<Request>& r)
    {
        return r && static_cast<IntentParseState>(r->state.load()) == IntentParseState::Pending;
    };
    return pending(noteRequest_);
}

void LlmIntentSource::Tick()
{
    // The watchdog owns retirement for a kept-alive server, and it counts EVERY request --
    // including ones made from the server's own chat page, which this timer cannot see.
    if (!serverOwned_ || keepsServerAlive_ || settings_.idleTimeoutSeconds <= 0)
    {
        return;
    }
    // An in-flight request is use, even though it has not come back yet -- ALL three kinds.
    // The command request and the chat both also refresh the clock as they are polled; the
    // developer note does not, because nothing polls it: it is fired after a refusal and
    // left to land on its own. Without it here, a note that takes longer than the idle
    // timeout gets its own server shot out from under it.
    const auto pending = [](const std::shared_ptr<Request>& r)
    {
        return r && static_cast<IntentParseState>(r->state.load()) == IntentParseState::Pending;
    };
    if (pending(request_) || pending(freeformRequest_) || pending(noteRequest_))
    {
        lastUseSec_ = NowSeconds();
        return;
    }
    if (NowSeconds() - lastUseSec_ >= static_cast<double>(settings_.idleTimeoutSeconds))
    {
        LOG_INFO(logging::LogCategory::Editor,
            "intent model: idle for {}s, stopping llama-server pid={}",
            settings_.idleTimeoutSeconds, server_.processId);
        StopServer();
        serverStatus_ = "Stopped after being idle; the next phrase starts it again";
    }
}

double LlmIntentSource::SecondsUntilIdleStop() const
{
    if (!serverOwned_ || settings_.idleTimeoutSeconds <= 0)
    {
        return -1.0;
    }
    return static_cast<double>(settings_.idleTimeoutSeconds) - (NowSeconds() - lastUseSec_);
}

bool LlmIntentSource::OwnsRunningServer() const
{
    return serverOwned_ && server_.Running();
}

void LlmIntentSource::StopServer()
{
    if (!serverOwned_)
    {
        return;
    }
    Cancel();
    LOG_INFO(logging::LogCategory::Editor, "intent model: stopping llama-server pid={}",
        server_.processId);
    server_.Terminate();
    serverOwned_ = false;
    serverHealthy_ = false;
    serverStatus_ = "Server stopped";
}

std::string LlmIntentSource::WebUiUrl() const
{
    // Only when the running server was actually started with its chat page. Printing a
    // URL that answers 404 would be worse than printing nothing.
    if (!settings_.webUi || !(serverHealthy_ || serverOwned_))
    {
        return {};
    }
    return "http://" + settings_.endpoint;
}

bool LlmIntentSource::EnsureServerReady(std::string& outStatus)
{
    if (!Available())
    {
        outStatus = UnavailableReason();
        return false;
    }
    return EnsureServer(outStatus);
}

void LlmIntentSource::BeginFreeform(const std::string& prompt, float temperature, int maxTokens)
{
    CancelFreeform();

    auto request = std::make_shared<Request>();
    request->state.store(static_cast<int>(IntentParseState::Pending));
    freeformRequest_ = request;

    // Chatting is using the model, so it must hold off the idle stop. Without this a
    // conversation with a pause in it would find the server gone mid-thought.
    lastUseSec_ = NowSeconds();

    nlohmann::json body;
    body["prompt"] = prompt;
    body["temperature"] = temperature;
    body["n_predict"] = maxTokens;
    // The chat's prefix is the conversation so far, which grows by one exchange each turn,
    // so the cache hits on everything but the newest words.
    body["cache_prompt"] = true;
    // STREAMED, unlike the command bar. The command bar waits on one short JSON object and
    // has a preview to show afterwards; a conversation does not, and with stream=false
    // nothing at all comes back -- not even the HTTP headers -- until the last token. A
    // one-word "привет" costs about 37 s here because the model reasons first, and for all
    // of those 37 s the panel could only say "thinking...". That is not a slow feature,
    // that is a feature indistinguishable from a hang.
    //
    // Streaming also fixes the timeout honestly rather than by inflating it: each read now
    // waits for the NEXT token instead of for the whole generation, so the flat timeout is
    // back to meaning what it says and a 32768-token budget needs no special deadline.
    body["stream"] = true;

    const std::string endpoint = settings_.endpoint;
    const int timeout = settings_.timeoutSeconds;
    const std::string payload = body.dump();

    std::thread([request, endpoint, timeout, payload]()
    {
        // llama-server speaks server-sent events: "data: {json}\n\n" per token. Reads do
        // not respect event boundaries, so a partial line is carried to the next chunk.
        std::string pending;
        std::string answer;
        bool truncated = false;
        float rate = 0.0f;
        // When the first token lands is the number that decides whether this feels like a
        // conversation or like a hang, and it is invisible from outside -- so it is logged.
        const auto started = std::chrono::steady_clock::now();
        bool loggedFirst = false;

        const auto consumeEvent = [&](std::string_view line)
        {
            constexpr std::string_view kPrefix = "data:";
            if (line.size() < kPrefix.size() || line.substr(0, kPrefix.size()) != kPrefix)
            {
                return;
            }
            line.remove_prefix(kPrefix.size());
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            {
                line.remove_prefix(1);
            }
            const nlohmann::json event =
                nlohmann::json::parse(line.begin(), line.end(), nullptr, false);
            if (event.is_discarded() || !event.is_object())
            {
                return;
            }
            const auto content = event.find("content");
            if (content != event.end() && content->is_string())
            {
                const std::string piece = content->get<std::string>();
                if (!piece.empty())
                {
                    answer += piece;
                    if (!loggedFirst)
                    {
                        loggedFirst = true;
                        LOG_INFO(logging::LogCategory::Editor,
                            "chat: first token after {:.1f}s",
                            std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - started).count());
                    }
                    std::lock_guard<std::mutex> lock(request->partialMutex);
                    request->partial = answer;
                }
            }
            // The final event carries the verdict and the timings.
            truncated = truncated || HitTheTokenLimit(event);
            const auto timings = event.find("timings");
            if (timings != event.end() && timings->is_object())
            {
                rate = timings->value("predicted_per_second", rate);
            }
        };

        const llmclient::StreamCallback onChunk = [&](std::string_view bytes)
        {
            if (request->abandoned.load())
            {
                return;
            }
            pending.append(bytes);
            std::size_t start = 0;
            for (;;)
            {
                const std::size_t nl = pending.find('\n', start);
                if (nl == std::string::npos)
                {
                    break;
                }
                std::string_view line(pending);
                line = line.substr(start, nl - start);
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                if (!line.empty())
                {
                    consumeEvent(line);
                }
                start = nl + 1;
            }
            pending.erase(0, start);
        };

        const llmclient::Response response =
            llmclient::PostJsonStreaming(endpoint, "/completion", payload, timeout, onChunk);
        if (request->abandoned.load())
        {
            return;
        }
        if (!response.ok)
        {
            request->error = response.error.empty() ? "no answer from the local model"
                                                    : response.error;
            request->state.store(static_cast<int>(IntentParseState::Failed));
            return;
        }
        if (answer.empty())
        {
            // Not an SSE body after all -- an older server, or an error page. Fall back to
            // reading it as the single object the non-streaming path would have returned,
            // so a server that ignores "stream" still works.
            const nlohmann::json parsed =
                nlohmann::json::parse(response.body, nullptr, false);
            if (parsed.is_discarded() || !parsed.contains("content"))
            {
                request->error = "unexpected reply shape from llama-server";
                request->state.store(static_cast<int>(IntentParseState::Failed));
                return;
            }
            answer = parsed["content"].get<std::string>();
            truncated = HitTheTokenLimit(parsed);
            const auto timings = parsed.find("timings");
            if (timings != parsed.end() && timings->is_object())
            {
                rate = timings->value("predicted_per_second", 0.0f);
            }
        }
        request->answer = answer;
        request->truncated = truncated;
        request->tokensPerSecond = rate;
        request->state.store(static_cast<int>(IntentParseState::Ready));
    }).detach();
}

std::string LlmIntentSource::FreeformPartial() const
{
    if (!freeformRequest_)
    {
        return {};
    }
    std::lock_guard<std::mutex> lock(freeformRequest_->partialMutex);
    return freeformRequest_->partial;
}

IntentParseState LlmIntentSource::PollFreeform(std::string& outText, std::string& outError,
    bool& outTruncated)
{
    if (!freeformRequest_)
    {
        return IntentParseState::Idle;
    }
    const auto state = static_cast<IntentParseState>(freeformRequest_->state.load());
    if (state == IntentParseState::Pending)
    {
        // Still generating -- keep the idle timer away from the server it is generating on.
        lastUseSec_ = NowSeconds();
        return IntentParseState::Pending;
    }
    if (state == IntentParseState::Failed)
    {
        outError = freeformRequest_->error;
        freeformRequest_.reset();
        return IntentParseState::Failed;
    }
    outText = freeformRequest_->answer;
    outTruncated = freeformRequest_->truncated;
    if (freeformRequest_->tokensPerSecond > 0.0f)
    {
        lastTokensPerSecond_ = freeformRequest_->tokensPerSecond;
    }
    freeformRequest_.reset();
    return IntentParseState::Ready;
}

void LlmIntentSource::CancelFreeform()
{
    if (freeformRequest_)
    {
        freeformRequest_->abandoned.store(true);
        freeformRequest_.reset();
    }
}

LlmIntentSource::NoteState LlmIntentSource::NoteStatus() const
{
    if (!noteRequest_)
    {
        return NoteState::None;
    }
    switch (static_cast<IntentParseState>(noteRequest_->state.load()))
    {
    case IntentParseState::Pending: return NoteState::Writing;
    case IntentParseState::Ready:   return NoteState::Written;
    default:                        return NoteState::Failed;
    }
}

void LlmIntentSource::RequestDeveloperNote(const std::string& phrase, const EditorIntent& refusal)
{
    if (settings_.apiRequestNotesPath.empty())
    {
        return;
    }
    if (noteRequest_)
    {
        noteRequest_->abandoned.store(true);
    }

    // The action list, so the note can say which existing one SHOULD have covered it --
    // a wrong refusal is worth more to the implementer than a polished proposal.
    std::string actionList;
    for (const EditorActionDesc& action : EditorActionRegistry::Builtin().Actions())
    {
        actionList += "- " + std::string(action.id) + ": " +
            std::string(action.description).substr(0, 110) + "\n";
    }

    nlohmann::json body;
    // NO GRAMMAR here, deliberately: this one is prose for a human or an agent to read,
    // and a JSON muzzle would turn it into a form to fill in.
    // THINKING IS SUPPRESSED HERE and the budget is generous, because the first version of
    // this had neither and produced nothing usable: the model spent all 400 tokens
    // reasoning about how to write the note and the note itself never arrived. Summarising
    // a decision it has already made is not a task that needs a reasoning pass -- the
    // reasoning happened when it chose to refuse.
    body["prompt"] = intentprompt::ApplyChatTemplate(
        intentnotes::BuildNotePrompt(phrase, refusal, levelPathForNotes_, actionList),
        "Write the note.", settings_.chatTemplate);
    body["temperature"] = 0.4;
    body["n_predict"] = 512;
    body["cache_prompt"] = false;   // a one-off prefix; caching it would evict the useful one
    body["stream"] = false;

    auto request = std::make_shared<Request>();
    request->state.store(static_cast<int>(IntentParseState::Pending));
    noteRequest_ = request;

    const std::string endpoint = settings_.endpoint;
    const int timeout = settings_.timeoutSeconds;
    const std::string payload = body.dump();
    const std::string path = settings_.apiRequestNotesPath;
    const std::string levelPath = levelPathForNotes_;
    const EditorIntent refusalCopy = refusal;
    const std::string phraseCopy = phrase;

    std::thread([request, endpoint, timeout, payload, path, levelPath, refusalCopy, phraseCopy]()
    {
        const llmclient::Response response =
            llmclient::PostJson(endpoint, "/completion", payload, timeout);
        if (request->abandoned.load())
        {
            return;
        }

        std::string prose;
        if (response.ok)
        {
            const nlohmann::json parsed = nlohmann::json::parse(response.body, nullptr, false);
            if (!parsed.is_discarded() && parsed.contains("content"))
            {
                prose = parsed["content"].get<std::string>();
                // A thinking model may still emit a reasoning block here; the note is what
                // comes after it.
                const std::size_t endThink = prose.find("</think>");
                if (endThink != std::string::npos)
                {
                    prose = prose.substr(endThink + 8);
                }
                const std::size_t firstReal = prose.find_first_not_of(" \t\r\n");
                prose = firstReal == std::string::npos
                    ? std::string{} : prose.substr(firstReal);
            }
        }

        // The note is written even when the prose did not arrive: the REQUEST is the part
        // worth keeping, and an entry saying only what was asked still counts a repeat.
        const std::string note = intentnotes::FormatNote(phraseCopy, refusalCopy, levelPath,
            prose, intentnotes::Timestamp());
        const bool written = intentnotes::AppendNote(path, note);
        request->state.store(static_cast<int>(
            written ? IntentParseState::Ready : IntentParseState::Failed));
        if (written)
        {
            LOG_INFO(logging::LogCategory::Editor,
                "intent model: API request noted in {}", path);
        }
    }).detach();
}

std::string LlmIntentSource::StatusLine() const
{
    if (!Available())
    {
        return UnavailableReason();
    }
    if (!serverHealthy_)
    {
        return serverStatus_.empty() ? "Local model idle" : serverStatus_;
    }
    return "Local model ready (" + settings_.endpoint + ")";
}

LlmIntentSettings LlmIntentSource::LoadSettings(const nlohmann::json& levelEditorJson)
{
    LlmIntentSettings settings;
    const auto it = levelEditorJson.find("intentModel");
    if (it == levelEditorJson.end() || !it->is_object())
    {
        return settings;
    }
    const nlohmann::json& json = *it;
    settings.enabled = ReadBoolOr(json, "enabled", settings.enabled);
    settings.modelPath = ReadStringOr(json, "modelPath", settings.modelPath);
    settings.serverExe = ReadStringOr(json, "serverExe", settings.serverExe);
    settings.endpoint = ReadStringOr(json, "endpoint", settings.endpoint);
    settings.autoStart = ReadBoolOr(json, "autoStart", settings.autoStart);
    settings.gpuLayers = ReadIntOr(json, "gpuLayers", settings.gpuLayers);
    settings.contextTokens = ReadIntOr(json, "contextTokens", settings.contextTokens);
    settings.threads = ReadIntOr(json, "threads", settings.threads);
    settings.charsPerTokenEstimate =
        ReadIntOr(json, "charsPerTokenEstimate", settings.charsPerTokenEstimate);
    settings.timeoutSeconds = ReadIntOr(json, "timeoutSeconds", settings.timeoutSeconds);
    settings.chatTemplate = ReadStringOr(json, "chatTemplate", settings.chatTemplate);
    settings.apiRequestNotesPath =
        ReadStringOr(json, "apiRequestNotesPath", settings.apiRequestNotesPath);
    settings.webUi = ReadBoolOr(json, "webUi", settings.webUi);
    settings.idleTimeoutSeconds = ReadIntOr(json, "idleTimeoutSeconds", settings.idleTimeoutSeconds);
    settings.keepServerAfterExit =
        ReadBoolOr(json, "keepServerAfterExit", settings.keepServerAfterExit);
    settings.keepAliveSeconds = ReadIntOr(json, "keepAliveSeconds", settings.keepAliveSeconds);
    return settings;
}

nlohmann::json LlmIntentSource::SaveSettings(const LlmIntentSettings& settings)
{
    return nlohmann::json{
        { "enabled", settings.enabled },
        { "modelPath", settings.modelPath },
        { "serverExe", settings.serverExe },
        { "endpoint", settings.endpoint },
        { "autoStart", settings.autoStart },
        { "gpuLayers", settings.gpuLayers },
        { "contextTokens", settings.contextTokens },
        { "threads", settings.threads },
        { "charsPerTokenEstimate", settings.charsPerTokenEstimate },
        { "timeoutSeconds", settings.timeoutSeconds },
        { "chatTemplate", settings.chatTemplate },
        { "apiRequestNotesPath", settings.apiRequestNotesPath },
        { "webUi", settings.webUi },
        { "idleTimeoutSeconds", settings.idleTimeoutSeconds },
        { "keepServerAfterExit", settings.keepServerAfterExit },
        { "keepAliveSeconds", settings.keepAliveSeconds },
    };
}

#endif // WITH_EDITOR
