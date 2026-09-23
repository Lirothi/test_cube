#pragma once
#if WITH_EDITOR

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "editor/scene/EditorSceneDocument.h" // nlohmann::json

struct EditorActionContext;
class EditorCommandStack;
class Renderer;

// Claude Code, driving the editor.
//
// An MCP server (Model Context Protocol, streamable-HTTP transport) inside the editor process,
// bound to 127.0.0.1 only. It exposes the SAME action registry the local model drives, through
// the SAME road: a call is read by intentschema::ParseAnswer, previewed by BuildIntentPreview and
// executed by ExecuteIntent. So everything the command bar guarantees holds for a caller that is
// not the local model -- typed parameters, the preview's refusals, one undo entry per call, a
// spawn joining its kind's group, a shortfall that says which rule refused it -- and anything
// added to the registry later is reachable from here the moment it exists.
//
// WHAT IT CANNOT DO IS SAVE. There is no tool for it and there is not going to be one: the level
// is saved by the person at the keyboard and by nobody else. Everything done through here lives
// in the open document, shows up in the command bar's transcript as it happens, and comes back
// out with Ctrl+Z.
namespace editormcp
{
    // One tool call's answer: text to read, and optionally a PNG to look at.
    struct ToolResult
    {
        std::string text;
        std::string pngBase64;   // non-empty: an image content block goes out with the text
        bool isError = false;
    };

    // Whoever actually runs a tool. The server's host queues onto the frame thread; the gate's
    // runs it on the spot against its own document.
    class ToolHost
    {
    public:
        virtual ~ToolHost() = default;
        virtual ToolResult Call(const std::string& name, const nlohmann::json& arguments) = 0;
    };

    // The protocol and nothing else: one JSON-RPC message in, its reply out -- or nullopt for a
    // notification, which gets none. No sockets and no threads, so the gate can drive it.
    std::optional<nlohmann::json> HandleMessage(const nlohmann::json& message, ToolHost& host);

    // The tools as tools/list reports them.
    nlohmann::json ToolList();

    // What one call did, for the command bar's transcript.
    struct CallRecord
    {
        std::string title;     // "run_action spawn"
        std::string verdict;   // the status line the call returned
        std::string action;    // the registry id when an action ran, for the session memory
        bool ran = false;      // the document changed
        bool failed = false;
        // Worth a line in the person's transcript: something happened to the level or to
        // their view. Reads (the guide, a query, a screenshot) are not -- they change nothing,
        // and a transcript full of them would bury the edits.
        bool show = false;
    };

    // The Import window, for list_staging / import_asset / import_status. It is not part of the
    // action context -- it owns files, not the document -- so the editor registers it here and a
    // host without one (the gate) answers those tools with a refusal. Frame thread only.
    class ImportHost
    {
    public:
        virtual ~ImportHost() = default;
        virtual std::string Staging() = 0;          // JSON array of what import_staging holds
        virtual bool Start(const std::string& name, float targetSizeM, bool split,
            std::string& outText) = 0;              // false + the reason when it cannot start
        virtual std::string Status() = 0;           // JSON object
    };
    void SetImportHost(ImportHost* host);

    // Runs one tool that touches the DOCUMENT, on the frame thread -- everything except
    // `screenshot`, which needs the frame loop's safe point instead. Public so the gate can
    // drive it directly.
    ToolResult RunDocumentTool(const EditorActionContext& actionCtx,
        EditorCommandStack& commandStack,
        const std::string& name,
        const nlohmann::json& arguments,
        CallRecord& outRecord);

    class Server
    {
    public:
        Server();
        ~Server();
        Server(const Server&) = delete;
        Server& operator=(const Server&) = delete;

        bool Start(int port, std::string& outError);
        void Stop();
        bool Running() const { return running_.load(); }
        int Port() const { return port_; }

        // FRAME THREAD, once a frame: runs every call the socket thread has queued and hands
        // back what each one did, so it can be shown where the person is looking.
        std::vector<CallRecord> Service(const EditorActionContext& actionCtx,
            EditorCommandStack& commandStack);

        // For the panel's status line.
        unsigned long long CallCount() const { return callCount_.load(); }
        std::string LastCall() const;

    private:
        struct Job;
        class QueueHost;

        void AcceptLoop();
        void HandleConnection(std::uintptr_t client);

        std::atomic<bool> running_{ false };
        std::atomic<bool> stopping_{ false };
        std::uintptr_t listenSocket_ = ~static_cast<std::uintptr_t>(0);
        // The connection being served, so Stop can cut a recv() short instead of waiting out
        // its timeout.
        std::atomic<std::uintptr_t> currentClient_{ ~static_cast<std::uintptr_t>(0) };
        int port_ = 0;
        std::thread thread_;
        std::unique_ptr<QueueHost> host_;

        mutable std::mutex jobsMutex_;
        std::vector<std::shared_ptr<Job>> jobs_;

        std::atomic<unsigned long long> callCount_{ 0 };
        mutable std::mutex lastCallMutex_;
        std::string lastCall_;
    };

    // The screenshot handoff. The socket thread asks; App's frame loop fulfils it at the same
    // safe point `--shot` uses, where the frame has been presented and the GPU can be waited
    // on. Called by App every frame; costs one uncontended lock when nobody asked.
    void ServiceScreenshot(Renderer& renderer);
}

#endif // WITH_EDITOR
