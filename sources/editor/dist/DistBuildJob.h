#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// One distribution build run from the editor (docs/distribution_build_plan.md §8.4): `python tools/build_dist.py
// --progress=json ...` as a child process inside a job object -- Cancel ends its whole tree (MSBuild, the cook, the staged
// game), and so does the editor going away. The events it prints (one JSON object a line) are read on a thread of its own.
// The Build Distribution window and MCP's build_dist / build_dist_status share the one job: one build at a time.
namespace dist
{
    struct BuildRequest
    {
        std::string profile;                 // "test", or a path to a profile .json
        std::string mode = "content";        // check | record | content | full | delete (the window's History only)
        std::vector<std::string> levels;     // empty = the profile's
        std::string bootLevel;               // empty = the profile's (or the first of `levels`)
        std::string version;                 // empty = the profile's
        // shipping | development (docs/scalability_plan.md §7): which exe the folder gets; empty = the profile's (shipping).
        std::string flavour;
        bool noEncrypt = false;
        // --override-licences: ship the assets the profile's "strictLicences" refuses (a warning each); the profile keeps it.
        bool overrideLicences = false;
        bool skipBuild = false;
        // mode "delete": `--delete-build=<n>` -- the build out of the history and its own files off the disk (the window's
        // History "Delete", after its confirmation; never MCP's, which sets no build).
        int deleteBuild = 0;
    };

    struct BuildEvent
    {
        std::string stage;
        std::string message;
        std::string level;                   // info | warning | error
        std::string event;                   // stage | stage_end | totals | done | failed | "" (a line), "raw" (not JSON)
        double fraction = 0.0;
        double seconds = 0.0;
    };

    struct StageTime
    {
        std::string stage;
        double seconds = 0.0;                // < 0 while it runs
    };

    struct BuildStatus
    {
        int id = 0;                          // 0 = never started
        bool running = false;
        bool finished = false;
        bool cancelled = false;
        int exitCode = 0;
        BuildRequest request;
        std::string stage;
        double fraction = 0.0;
        double seconds = 0.0;
        std::vector<BuildEvent> events;      // the newest Status(maxEvents) of them (the job keeps 4000)
        std::size_t eventCount = 0;          // how many the job holds
        std::vector<StageTime> stages;
        std::vector<std::string> errors;
        int warningCount = 0;
        // From the "done" event.
        std::string folder;
        std::string zip;
        std::string report;
        std::string version;
        std::uint64_t bytes = 0;
    };

    class BuildJob
    {
    public:
        BuildJob();
        ~BuildJob();
        BuildJob(const BuildJob&) = delete;
        BuildJob& operator=(const BuildJob&) = delete;

        // False (and why) when one is running or the process cannot start.
        bool Start(const BuildRequest& request, std::string& why);
        void Cancel();
        [[nodiscard]] bool Running() const;
        // A copy taken under the lock, with the newest `maxEvents` events (the window draws 200 a frame; a caller that
        // reads none asks for 0 -- the whole list was copied up to four times a frame).
        [[nodiscard]] BuildStatus Status(std::size_t maxEvents = 0) const;
        // build_dist_status's answer: a JSON object (the last 12 events, the errors, the result).
        [[nodiscard]] std::string StatusJson(int id) const;

    private:
        struct State;
        std::unique_ptr<State> state_;
    };

    // The editor's one job (MCP and the window).
    BuildJob& TheBuildJob();

    // The command line build_dist.py gets for a request (testable without a process).
    std::wstring BuildCommandLine(const std::wstring& python, const BuildRequest& request);
}
