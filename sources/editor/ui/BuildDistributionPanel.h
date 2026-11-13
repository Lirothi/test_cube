#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "third_party/json/json.hpp"

namespace dist { struct BuildRequest; }

// Level Editor > File > Build Distribution... (docs/distribution_build_plan.md §8.4): a build profile (tools/dist/*.json)
// edited and saved, the levels it ships (and the one it starts), what goes in beside them, its options and the package
// key, the checks before a run, the run itself -- tools/build_dist.py in a process of its own (dist::TheBuildJob), its
// stages, its log and Cancel -- the result and the history. The same profile from the console gives the same bytes.
class BuildDistributionPanel
{
public:
    struct Hooks
    {
        std::function<bool()> levelDirty;                    // the open level has unsaved edits
        std::function<bool(std::string& why)> saveLevel;     // Ctrl+S
        std::function<bool()> playing;                       // a Play session runs
    };

    void Draw(bool* open, const Hooks& hooks);

    // What MCP's build_dist refuses before starting anything (the window refuses the same): a level that is not a file,
    // a boot level that is not shipped, a mode that is not one. Empty `why` = fine.
    static bool ValidateRequest(const dist::BuildRequest& request, std::string& why);

private:
    struct LevelStat
    {
        std::uint64_t files = 0;
        std::uint64_t bytes = 0;
        std::uint64_t sharedFiles = 0;
    };

    void Refresh();                       // profiles, levels on disk, key, history
    void LoadProfile(const std::string& name);
    bool SaveProfile(const std::string& name, std::string& why);
    void StartRun(const std::string& mode, const Hooks& hooks);
    void ReadCheckReport(const std::string& path, bool allLevels);
    void DrawProfile();
    void DrawLevels();
    void DrawContent();
    void DrawOptions();
    void DrawChecks(const Hooks& hooks);
    void DrawRun(const Hooks& hooks);
    void DrawResult();
    void DrawHistory();
    [[nodiscard]] bool Dirty() const { return profile_ != saved_; }
    [[nodiscard]] std::string OutFolder() const;

    bool refreshed_ = false;
    std::vector<std::string> profiles_;   // names in tools/dist
    std::string profileName_;
    nlohmann::json profile_;
    nlohmann::json saved_;
    char saveAsName_[64] = {};
    char includeText_[4096] = {};
    bool includeLoaded_ = false;
    std::vector<std::string> levelsOnDisk_;
    std::map<std::string, LevelStat> levelStats_;
    std::vector<std::string> allLevelsErrors_;
    std::vector<std::string> checkErrors_;   // the last check of the selected levels
    std::vector<std::string> checkWarnings_;
    bool selectionChecked_ = false;
    std::string keyPath_;
    std::string keyGuid_;
    std::vector<std::string> gitChanged_;
    bool gitKnown_ = false;
    nlohmann::json history_ = nlohmann::json::array();
    int handledJobId_ = 0;                  // the last finished job whose result was read
    bool sizesRequested_ = false;
    bool sizesStarted_ = false;
    bool sizesJob_ = false;                 // the running job is the "all levels" size check
    std::string message_;
    std::uint64_t shaderListLines_ = 0;
    std::string shaderListDate_;
};
