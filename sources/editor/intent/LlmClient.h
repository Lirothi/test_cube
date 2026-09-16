#pragma once
#if WITH_EDITOR

#include <functional>
#include <string>
#include <string_view>

// A very small HTTP client for one localhost server, plus the process management to put
// that server there (docs/editor_llm_plan.md, E2).
//
// The runtime is a SEPARATE PROCESS on purpose: the engine gains no new dependency in any
// configuration, an out-of-memory or a crash inside inference cannot take down an editor
// holding an unsaved level, and the model and llama.cpp update on their own schedule.
// A second of inference makes the price of IPC invisible.
//
// WinHTTP is a system library, not a third party one, and it is linked only into the two
// configurations that define WITH_EDITOR.
namespace llmclient
{
    struct Response
    {
        bool ok = false;
        long statusCode = 0;
        std::string body;
        std::string error;
    };

    // Blocking POST of `jsonBody` to http://<hostPort><path>. Called only from the
    // worker thread that LlmIntentSource detaches -- never from the frame.
    Response PostJson(const std::string& hostPort,
        const std::string& path,
        const std::string& jsonBody,
        int timeoutSeconds);

    // Called on the worker thread as bytes arrive, before the request has finished.
    using StreamCallback = std::function<void(std::string_view)>;

    // The same POST, but handing each piece of the body over as it lands instead of only
    // at the end. That difference is the whole reason it exists: with "stream": false a
    // llama.cpp answer arrives in one lump when generation is already over, so a reply
    // that takes half a minute to think is indistinguishable from a hung editor. It also
    // turns the receive timeout back into what it looks like -- a gap between reads --
    // rather than a deadline on the entire generation.
    //
    // `response.body` still holds everything received, so a caller that wants the whole
    // thing at the end does not have to reassemble it.
    Response PostJsonStreaming(const std::string& hostPort,
        const std::string& path,
        const std::string& jsonBody,
        int timeoutSeconds,
        const StreamCallback& onChunk);

    // GET, used for the health probe while the server is still loading the model.
    Response Get(const std::string& hostPort, const std::string& path, int timeoutSeconds);

    // Opaque handle to a server this process started, so it can be stopped again. A
    // server we did NOT start is never touched.
    struct ServerProcess
    {
        void* handle = nullptr;       // HANDLE
        void* job = nullptr;          // HANDLE to the job object that owns it
        unsigned long processId = 0;

        bool Running() const;
        void Terminate();
    };

    // Launch llama-server INSIDE A JOB OBJECT that kills it when this process goes away.
    // That is the part that cannot be left to a shutdown path: an editor that crashes, is
    // killed from Task Manager, or is stopped by a debugger never runs its destructors, and
    // a 38 GB inference server outliving it is exactly the orphan nobody wants. The job
    // handle rides in ServerProcess and closing it -- deliberately, or by the process
    // ending for any reason at all -- takes the server with it.
    //
    // Returns a process with a null handle and fills `outError` on failure. Does not wait
    // for it to become healthy: the caller polls.
    ServerProcess StartServer(const std::string& serverExe,
        const std::string& modelPath,
        const std::string& hostPort,
        int gpuLayers,
        int contextTokens,
        int threads,
        bool webUi,
        std::string& outError);
}

#endif // WITH_EDITOR
