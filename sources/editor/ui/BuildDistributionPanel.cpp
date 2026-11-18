#include "editor/ui/BuildDistributionPanel.h"

#if WITH_EDITOR

#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"
#include "editor/dist/DistBuildJob.h"

#include "imgui.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <tuple>

namespace
{
    namespace fs = std::filesystem;

    fs::path RepoPath(const std::string& rel) { return vfs::ContentRoot() / vfs::Widen(rel); }

    // build_dist.py version_key: a.b.c[-suffix] as something that sorts -- a release after its pre-releases, suffixes by
    // their text.
    struct VersionKey
    {
        bool valid = false;
        int a = 0, b = 0, c = 0;
        int release = 1;
        std::string suffix;
        [[nodiscard]] auto Tuple() const { return std::tie(a, b, c, release, suffix); }
    };
    VersionKey ParseVersion(const std::string& version)
    {
        VersionKey key;
        const std::size_t dash = version.find('-');
        const std::string core = version.substr(0, dash);
        char tail = 0;
        if (sscanf_s(core.c_str(), "%d.%d.%d%c", &key.a, &key.b, &key.c, &tail, 1u) != 3) { return key; }
        if (dash != std::string::npos)
        {
            key.release = 0;
            key.suffix = version.substr(dash + 1);
        }
        key.valid = true;
        return key;
    }

    std::string ReadFileText(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    std::string MegaBytes(std::uint64_t bytes)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f MB", bytes / 1048576.0);
        return text;
    }

    fs::path DefaultKeyPath()
    {
        wchar_t profile[MAX_PATH] = {};
        GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        return fs::path(profile) / L".test_cube" / L"pak_key.json";
    }

    // A child process run to its end (the key generator), no window.
    int RunAndWait(const std::wstring& commandLine, const fs::path& cwd)
    {
        std::wstring cmd = commandLine;
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &startup, &info))
        {
            return -1;
        }
        WaitForSingleObject(info.hProcess, 60000);
        DWORD code = 1;
        GetExitCodeProcess(info.hProcess, &code);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        return static_cast<int>(code);
    }

    ImVec4 LevelColour(const std::string& level)
    {
        if (level == "error") { return ImVec4(1.0f, 0.42f, 0.38f, 1.0f); }
        if (level == "warning") { return ImVec4(1.0f, 0.82f, 0.35f, 1.0f); }
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }

    bool InputString(const char* label, nlohmann::json& object, const char* key, const char* fallback = "")
    {
        char buffer[256] = {};
        const std::string value = object.value(key, std::string(fallback));
        std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
        if (ImGui::InputText(label, buffer, sizeof(buffer)))
        {
            object[key] = std::string(buffer);
            return true;
        }
        return false;
    }

    bool CheckboxKey(const char* label, nlohmann::json& object, const char* key, bool fallback)
    {
        bool value = object.value(key, fallback);
        if (ImGui::Checkbox(label, &value))
        {
            object[key] = value;
            return true;
        }
        return false;
    }
}

bool BuildDistributionPanel::ValidateRequest(const dist::BuildRequest& request, std::string& why)
{
    why.clear();
    static const char* modes[] = { "check", "record", "content", "full" };
    const bool deleting = request.mode == "delete" && request.deleteBuild > 0; // the window's History (MCP sets no build)
    if (!deleting &&
        std::find_if(std::begin(modes), std::end(modes), [&](const char* m) { return request.mode == m; }) == std::end(modes))
    {
        why = "mode \"" + request.mode + "\": check, record, content or full";
        return false;
    }
    const std::string profile = request.profile.empty() ? std::string("test") : request.profile;
    const fs::path profilePath = profile.find_first_of("/\\.") == std::string::npos ? RepoPath("tools/dist/" + profile + ".json")
                                                                                     : fs::path(vfs::Widen(profile));
    std::error_code ec;
    if (!fs::is_regular_file(profilePath.is_absolute() ? profilePath : vfs::ContentRoot() / profilePath, ec))
    {
        why = "no profile " + profile + " (tools/dist/<name>.json)";
        return false;
    }
    // tools/build_dist.py's VERSION_RE: it becomes a folder name, a C string and a command-line value.
    if (!request.version.empty())
    {
        const std::string& v = request.version;
        std::size_t at = 0;
        bool ok = true;
        for (int part = 0; part < 3 && ok; ++part)
        {
            const std::size_t digits = at;
            while (at < v.size() && std::isdigit(static_cast<unsigned char>(v[at]))) { ++at; }
            ok = at > digits && (part == 2 || (at < v.size() && v[at++] == '.'));
        }
        if (ok && at < v.size())
        {
            ok = v[at] == '-' && at + 1 < v.size() && std::isalnum(static_cast<unsigned char>(v[at + 1]));
            for (std::size_t i = at + 1; ok && i < v.size(); ++i)
            {
                ok = std::isalnum(static_cast<unsigned char>(v[i])) || v[i] == '.' || v[i] == '-';
            }
        }
        if (!ok)
        {
            why = "version \"" + v + "\": digits a.b.c, optionally -suffix of letters, digits, dots and dashes";
            return false;
        }
    }
    if (!request.flavour.empty() && request.flavour != "shipping" && request.flavour != "development")
    {
        why = "flavour \"" + request.flavour + "\": shipping or development";
        return false;
    }
    for (const std::string& level : request.levels)
    {
        if (!vfs::IsFile(level))
        {
            why = "no level " + level;
            return false;
        }
    }
    if (!request.bootLevel.empty())
    {
        if (!vfs::IsFile(request.bootLevel))
        {
            why = "no level " + request.bootLevel + " (bootLevel)";
            return false;
        }
        if (!request.levels.empty() &&
            std::find(request.levels.begin(), request.levels.end(), request.bootLevel) == request.levels.end())
        {
            why = "bootLevel " + request.bootLevel + " is not one of the shipped levels";
            return false;
        }
    }
    return true;
}

std::string BuildDistributionPanel::OutFolder() const
{
    return profile_.is_object() ? profile_.value("out", std::string("dist")) : std::string("dist");
}

void BuildDistributionPanel::Refresh()
{
    refreshed_ = true;
    profiles_.clear();
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(RepoPath("tools/dist"), ec))
    {
        if (e.path().extension() != L".json") { continue; }
        const nlohmann::json json = nlohmann::json::parse(ReadFileText(e.path()), nullptr, false, true);
        if (json.is_object() && json.contains("levels")) { profiles_.push_back(vfs::Narrow(e.path().stem().wstring())); }
    }
    std::sort(profiles_.begin(), profiles_.end());
    levelsOnDisk_.clear();
    for (const fs::directory_entry& e : fs::directory_iterator(RepoPath("data/levels"), ec))
    {
        if (e.path().extension() == L".json") { levelsOnDisk_.push_back("data/levels/" + vfs::Narrow(e.path().filename().wstring())); }
    }
    std::sort(levelsOnDisk_.begin(), levelsOnDisk_.end());
    if (profileName_.empty() && !profiles_.empty())
    {
        LoadProfile(std::find(profiles_.begin(), profiles_.end(), "test") != profiles_.end() ? "test" : profiles_.front());
    }
    keyPath_ = vfs::Narrow(DefaultKeyPath().wstring());
    keyGuid_.clear();
    if (fs::is_regular_file(DefaultKeyPath(), ec))
    {
        const nlohmann::json key = nlohmann::json::parse(ReadFileText(DefaultKeyPath()), nullptr, false);
        keyGuid_ = key.is_object() ? key.value("guid", std::string("?")) : std::string("?");
    }
    history_ = nlohmann::json::parse(ReadFileText(RepoPath(OutFolder()) / L"build_history.json"), nullptr, false);
    if (!history_.is_array()) { history_ = nlohmann::json::array(); }
    shaderListLines_ = 0;
    shaderListLevels_.clear();
    shaderListNamesLevels_ = false;
    {
        std::istringstream lines(ReadFileText(RepoPath("tools/dist/shader_list.txt")));
        for (std::string line; std::getline(lines, line);)
        {
            if (!line.empty() && line[0] != '#' && line != "\r") { ++shaderListLines_; }
            if (line.rfind("# levels:", 0) == 0)
            {
                shaderListNamesLevels_ = true;
                std::istringstream names(line.substr(9));
                for (std::string name; names >> name;) { shaderListLevels_.push_back(name); }
            }
        }
    }
    shaderListDate_.clear();
    const fs::file_time_type when = fs::last_write_time(RepoPath("tools/dist/shader_list.txt"), ec);
    if (!ec)
    {
        const auto system = std::chrono::clock_cast<std::chrono::system_clock>(when);
        const std::time_t t = std::chrono::system_clock::to_time_t(system);
        char text[32];
        std::tm local{};
        localtime_s(&local, &t);
        std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
        shaderListDate_ = text;
    }
    // The tree's uncommitted changes (a -dirty version), the developer's own settings file aside -- as build_dist.py counts.
    gitChanged_.clear();
    gitKnown_ = false;
    {
        SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
        HANDLE readPipe = nullptr, writePipe = nullptr;
        if (CreatePipe(&readPipe, &writePipe, &inherit, 0))
        {
            SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW startup{ sizeof(startup) };
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = writePipe;
            startup.hStdError = writePipe;
            PROCESS_INFORMATION info{};
            std::wstring cmd = L"git status --porcelain --untracked-files=no";
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                               vfs::ContentRoot().c_str(), &startup, &info))
            {
                CloseHandle(writePipe);
                writePipe = nullptr;
                std::string out;
                char buffer[4096];
                DWORD got = 0;
                while (ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) && got > 0) { out.append(buffer, got); }
                WaitForSingleObject(info.hProcess, 10000);
                CloseHandle(info.hThread);
                CloseHandle(info.hProcess);
                std::istringstream lines(out);
                for (std::string line; std::getline(lines, line);)
                {
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) { line.pop_back(); }
                    if (line.size() > 3 && line.substr(3) != "graphics_settings.json") { gitChanged_.push_back(line.substr(3)); }
                }
                gitKnown_ = true;
            }
            if (writePipe) { CloseHandle(writePipe); }
            CloseHandle(readPipe);
        }
    }
}

void BuildDistributionPanel::LoadProfile(const std::string& name)
{
    profileName_ = name;
    profile_ = nlohmann::json::parse(ReadFileText(RepoPath("tools/dist/" + name + ".json")), nullptr, false, true);
    if (!profile_.is_object()) { profile_ = nlohmann::json::object(); }
    saved_ = profile_;
    flavour_ = profile_.value("flavour", std::string("shipping")) == "development" ? "development" : "shipping";
    overrideLicences_ = false;
    includeLoaded_ = false;
    selectionChecked_ = false;
    checkErrors_.clear();
    checkWarnings_.clear();
}

bool BuildDistributionPanel::SaveProfile(const std::string& name, std::string& why)
{
    for (const char c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
        {
            why = "a profile name is letters, digits, _ and -";
            return false;
        }
    }
    if (name.empty())
    {
        why = "no name";
        return false;
    }
    const fs::path path = RepoPath("tools/dist/" + name + ".json");
    const std::string text = profile_.dump(2) + "\n";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out)
    {
        why = "cannot write " + vfs::Narrow(path.wstring());
        return false;
    }
    profileName_ = name;
    saved_ = profile_;
    LOG_INFO(logging::LogCategory::Editor, "build distribution: saved the profile {}", vfs::Narrow(path.wstring()));
    return true;
}

void BuildDistributionPanel::ReadCheckReport(const std::string& path, bool allLevels)
{
    const nlohmann::json report = nlohmann::json::parse(ReadFileText(fs::path(vfs::Widen(path))), nullptr, false);
    if (!report.is_object()) { return; }
    if (allLevels)
    {
        levelStats_.clear();
        for (const nlohmann::json& s : report.value("levelStats", nlohmann::json::array()))
        {
            LevelStat stat;
            stat.files = s.value("files", 0ull);
            stat.bytes = s.value("bytes", 0ull);
            stat.sharedFiles = s.value("sharedFiles", 0ull);
            levelStats_[s.value("level", std::string())] = stat;
        }
        allLevelsErrors_ = report.value("errors", std::vector<std::string>{});
    }
    else
    {
        checkErrors_ = report.value("errors", std::vector<std::string>{});
        checkWarnings_ = report.value("warnings", std::vector<std::string>{});
        selectionChecked_ = true;
    }
}

void BuildDistributionPanel::StartRun(const std::string& mode, const Hooks& hooks)
{
    message_.clear();
    if (hooks.playing && hooks.playing())
    {
        message_ = "Stop Play first.";
        return;
    }
    if (Dirty())
    {
        std::string why;
        if (!SaveProfile(profileName_, why))
        {
            message_ = "The profile was not saved: " + why;
            return;
        }
    }
    dist::BuildRequest request;
    request.profile = profileName_;
    request.mode = mode;
    request.flavour = flavour_;
    request.overrideLicences = LicencesOverridden();
    std::string why;
    if (!ValidateRequest(request, why) || !dist::TheBuildJob().Start(request, why))
    {
        message_ = "Not started: " + why;
        return;
    }
    sizesJob_ = false;
}

void BuildDistributionPanel::DrawProfile()
{
    ImGui::SeparatorText("Profile");
    const std::string current = profileName_ + (Dirty() ? " *" : "");
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("##profile", current.c_str()))
    {
        for (const std::string& name : profiles_)
        {
            if (ImGui::Selectable(name.c_str(), name == profileName_)) { LoadProfile(name); }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!Dirty());
    if (ImGui::Button("Save"))
    {
        std::string why;
        message_ = SaveProfile(profileName_, why) ? "Saved tools/dist/" + profileName_ + ".json" : "Not saved: " + why;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputTextWithHint("##saveas", "new name", saveAsName_, sizeof(saveAsName_));
    ImGui::SameLine();
    if (ImGui::Button("Save as"))
    {
        std::string why;
        const std::string name = saveAsName_;
        std::error_code ec;
        if (fs::exists(RepoPath("tools/dist/" + name + ".json"), ec)) { message_ = "tools/dist/" + name + ".json exists"; }
        else if (SaveProfile(name, why)) { message_ = "Saved as tools/dist/" + name + ".json"; Refresh(); }
        else { message_ = "Not saved: " + why; }
    }
    ImGui::SetNextItemWidth(160.0f);
    InputString("Product", profile_, "product", "test_cube");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    InputString("Version", profile_, "version", "0.0.0");
    ImGui::SameLine();
    if (ImGui::SmallButton("+0.0.1"))
    {
        std::string v = profile_.value("version", std::string("0.0.0"));
        int a = 0, b = 0, c = 0;
        sscanf_s(v.c_str(), "%d.%d.%d", &a, &b, &c);
        profile_["version"] = std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c + 1);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    InputString("Output folder", profile_, "out", "dist");
    DrawVersionVerdict();
}

void BuildDistributionPanel::DrawVersionVerdict()
{
    // build_dist.py's versionMustGrow before a run (owner, 2026-11-17): only a PUBLISHED build uses its version up.
    if (!profile_.value("versionMustGrow", false)) { return; }
    const std::string version = profile_.value("version", std::string("0.0.0"));
    const VersionKey key = ParseVersion(version);
    const nlohmann::json* blocker = nullptr;
    std::string builtAs;
    for (const nlohmann::json& h : history_)
    {
        if (!h.is_object() || h.value("profile", std::string()) != profileName_ || h.value("result", std::string()) != "built" ||
            h.value("flavour", std::string("shipping")) != flavour_)
        {
            continue;
        }
        if (h.value("version", std::string()) == version) { builtAs += (builtAs.empty() ? "" : ", ") + std::to_string(h.value("build", 0)); }
        if (!h.value("published", false)) { continue; }
        const VersionKey published = ParseVersion(h.value("version", std::string()));
        if (!key.valid || !published.valid || published.Tuple() >= key.Tuple())
        {
            if (!blocker || ParseVersion(blocker->value("version", std::string())).Tuple() < published.Tuple()) { blocker = &h; }
        }
    }
    if (blocker)
    {
        ImGui::TextColored(LevelColour("error"), "Version %s: build %d (%s) is published -- raise it (or untick it in History)",
                           version.c_str(), blocker->value("build", 0), blocker->value("version", std::string()).c_str());
    }
    else if (!builtAs.empty())
    {
        ImGui::TextDisabled("Version %s: built before (build %s), not published -- builds again", version.c_str(), builtAs.c_str());
    }
}

void BuildDistributionPanel::SetPublished(int build, bool published)
{
    const fs::path path = RepoPath(OutFolder()) / L"build_history.json";
    nlohmann::json history = nlohmann::json::parse(ReadFileText(path), nullptr, false);
    if (!history.is_array())
    {
        message_ = "Not marked: cannot read " + vfs::Narrow(path.wstring());
        return;
    }
    bool found = false;
    for (nlohmann::json& h : history)
    {
        if (!h.is_object() || h.value("build", 0) != build || h.value("result", std::string()) != "built") { continue; }
        if (published)
        {
            char date[32] = {};
            const std::time_t now = std::time(nullptr);
            std::tm local{};
            localtime_s(&local, &now);
            std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &local);
            h["published"] = true;
            h["publishedDate"] = date;
        }
        else
        {
            h.erase("published");
            h.erase("publishedDate");
        }
        found = true;
    }
    if (!found)
    {
        message_ = "Not marked: no built build " + std::to_string(build) + " in the history";
        return;
    }
    // The history is build_dist.py's: temp + rename, so a half-written file is never what it reads.
    fs::path tmp = path;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << history.dump(2) << "\n";
        if (!out)
        {
            message_ = "Not marked: cannot write " + vfs::Narrow(tmp.wstring());
            return;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec)
    {
        message_ = "Not marked: " + ec.message();
        return;
    }
    history_ = history;
    message_ = "Build " + std::to_string(build) + (published ? ": published" : ": not published");
    LOG_INFO(logging::LogCategory::Editor, "build distribution: build {} {}", build, published ? "published" : "not published");
}

void BuildDistributionPanel::DrawLevels(const Hooks& hooks)
{
    ImGui::SeparatorText("Levels");
    if (!profile_.contains("levels") || !profile_["levels"].is_array()) { profile_["levels"] = nlohmann::json::array(); }
    nlohmann::json& levels = profile_["levels"];
    const auto shipped = [&](const std::string& level)
    {
        return std::find(levels.begin(), levels.end(), level) != levels.end();
    };
    if (ImGui::SmallButton("All")) { levels = levelsOnDisk_; }
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) { levels = nlohmann::json::array(); }
    ImGui::SameLine();
    const bool busy = dist::TheBuildJob().Running();
    ImGui::BeginDisabled(busy);
    if (ImGui::SmallButton("Measure sizes"))
    {
        dist::BuildRequest request;
        request.profile = profileName_;
        request.mode = "check";
        request.levels = levelsOnDisk_;
        request.overrideLicences = LicencesOverridden();
        std::string why;
        if (dist::TheBuildJob().Start(request, why)) { sizesJob_ = true; }
        else { message_ = "Not measured: " + why; }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(the dependency walk over every level: before compression, files shared with another level)");

    std::uint64_t files = 0, bytes = 0;
    if (ImGui::BeginTable("levels", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 190.0f)))
    {
        ImGui::TableSetupColumn("Ship", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Level");
        ImGui::TableSetupColumn("Files", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Size (shared)", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableHeadersRow();
        for (const std::string& level : levelsOnDisk_)
        {
            ImGui::PushID(level.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool in = shipped(level);
            if (ImGui::Checkbox("##ship", &in))
            {
                if (in) { levels.push_back(level); }
                else { levels.erase(std::remove(levels.begin(), levels.end(), level), levels.end()); }
            }
            ImGui::TableNextColumn();
            const bool boot = profile_.value("bootLevel", std::string()) == level;
            if (ImGui::RadioButton("##boot", boot))
            {
                profile_["bootLevel"] = level;
                if (!shipped(level)) { levels.push_back(level); }
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(level.c_str());
            ImGui::TableNextColumn();
            const auto stat = levelStats_.find(level);
            if (stat != levelStats_.end())
            {
                ImGui::Text("%llu", static_cast<unsigned long long>(stat->second.files));
                ImGui::TableNextColumn();
                ImGui::Text("%s (%llu)", MegaBytes(stat->second.bytes).c_str(), static_cast<unsigned long long>(stat->second.sharedFiles));
                if (in)
                {
                    files += stat->second.files;
                    bytes += stat->second.bytes;
                }
            }
            else
            {
                ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                ImGui::TextDisabled("-");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Text("%zu level(s) shipped, start: %s; their own files: %llu, %s before compression (a file two levels share counts "
                "twice here)", levels.size(), profile_.value("bootLevel", std::string("-")).c_str(),
                static_cast<unsigned long long>(files), MegaBytes(bytes).c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    std::string startMode = profile_.value("startMode", std::string("play"));
    if (ImGui::BeginCombo("Start in", startMode.c_str()))
    {
        for (const char* mode : { "play", "free" })
        {
            if (ImGui::Selectable(mode, startMode == mode)) { profile_["startMode"] = mode; }
        }
        ImGui::EndCombo();
    }
    if (!allLevelsErrors_.empty() && ImGui::TreeNode("levels_errors", "%zu problem(s) in the levels on disk", allLevelsErrors_.size()))
    {
        for (const std::string& e : allLevelsErrors_) { DrawMessage(e, "error", hooks); }
        if (!selectStatus_.empty()) { ImGui::TextDisabled("%s", selectStatus_.c_str()); }
        ImGui::TreePop();
    }
}

bool BuildDistributionPanel::ObjectOfMessage(const std::string& message, std::string& level, std::string& object)
{
    const std::size_t by = message.find("(named by ");
    const std::size_t mark = by == std::string::npos ? std::string::npos : message.find(", object '", by);
    const std::size_t end = mark == std::string::npos ? std::string::npos : message.rfind("')");
    if (mark == std::string::npos || end == std::string::npos || end <= mark + 10u) { return false; }
    level = message.substr(by + 10u, mark - (by + 10u));
    object = message.substr(mark + 10u, end - (mark + 10u));
    return !level.empty() && !object.empty();
}

void BuildDistributionPanel::DrawMessage(const std::string& message, const char* level, const Hooks& hooks)
{
    // The cook names a level's object as "named by <level>, object '<name>')" (Cook.cpp Visit): such a line is a button.
    std::string levelPath;
    std::string object;
    if (!hooks.selectObject || !ObjectOfMessage(message, levelPath, object))
    {
        ImGui::TextColored(LevelColour(level), "%s", message.c_str());
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, LevelColour(level));
    ImGui::PushID(&message);
    const bool clicked = ImGui::Selectable(message.c_str(), false);
    ImGui::PopID();
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Click: select '%s' in %s", object.c_str(), levelPath.c_str()); }
    if (clicked)
    {
        std::string why;
        selectStatus_ = hooks.selectObject(levelPath, object, why) ? "selected '" + object + "'" : why;
    }
}

void BuildDistributionPanel::DrawContent()
{
    ImGui::SeparatorText("Always in the package");
    if (!includeLoaded_)
    {
        std::string text;
        for (const nlohmann::json& line : profile_.value("include", nlohmann::json::array()))
        {
            if (line.is_string()) { text += line.get<std::string>() + "\n"; }
        }
        std::snprintf(includeText_, sizeof(includeText_), "%s", text.c_str());
        includeLoaded_ = true;
    }
    if (ImGui::InputTextMultiline("##include", includeText_, sizeof(includeText_), ImVec2(-FLT_MIN, 90.0f)))
    {
        nlohmann::json include = nlohmann::json::array();
        std::istringstream lines(includeText_);
        for (std::string line; std::getline(lines, line);)
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) { line.pop_back(); }
            if (!line.empty()) { include.push_back(line); }
        }
        profile_["include"] = include;
    }
    ImGui::TextDisabled("One path or pattern a line (fonts/*.json), copied as it is, not walked. The levels bring their own "
                        "dependencies, the record list what code opens.");
    ImGui::Text("Shader permutations recorded: %llu (tools/dist/shader_list.txt, %s). A level the record runs did not see may "
                "compile some at the player's -- Record updates the list.",
                static_cast<unsigned long long>(shaderListLines_), shaderListDate_.c_str());
    // The levels chosen here that the record runs never went over: their permutations may compile at the player's.
    if (shaderListNamesLevels_ && profile_.contains("levels") && profile_["levels"].is_array())
    {
        std::string missing;
        for (const nlohmann::json& level : profile_["levels"])
        {
            if (!level.is_string()) { continue; }
            const std::string name = level.get<std::string>();
            if (std::find(shaderListLevels_.begin(), shaderListLevels_.end(), name) == shaderListLevels_.end())
            {
                missing += (missing.empty() ? "" : ", ") + name;
            }
        }
        if (!missing.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Not in the record runs: %s -- Record them before a release.", missing.c_str());
        }
    }
}

void BuildDistributionPanel::DrawOptions()
{
    ImGui::SeparatorText("Options");
    CheckboxKey("Encrypt the packages", profile_, "encrypt", false);
    ImGui::SameLine();
    if (flavour_ == "development")
    {
        ImGui::TextDisabled("(Shipping only: a Development build is never encrypted)");
    }
    else if (keyGuid_.empty())
    {
        ImGui::TextColored(LevelColour("warning"), "no key at %s", keyPath_.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Create key"))
        {
            const fs::path cook = vfs::ContentRoot() / L"x64" / L"Cook" / L"test_cube.exe";
            std::error_code ec;
            fs::create_directories(DefaultKeyPath().parent_path(), ec);
            const int code = RunAndWait(L"\"" + cook.wstring() + L"\" --pak-keygen=\"" + DefaultKeyPath().wstring() + L"\"",
                                        vfs::ContentRoot());
            message_ = code == 0 ? "A new key: " + keyPath_ + " -- keep a copy somewhere safe: without it no package built with it "
                                   "can be read or patched."
                                 : "The key was not made (x64/Cook/test_cube.exe exit " + std::to_string(code) + ")";
            Refresh();
        }
    }
    else
    {
        ImGui::TextDisabled("key %s (%s)", keyGuid_.c_str(), keyPath_.c_str());
    }
    std::string compression = profile_.value("compression", std::string("zstd:19"));
    int level = compression.rfind("zstd:", 0) == 0 ? std::atoi(compression.c_str() + 5) : 19;
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt("zstd level", &level, 1, 22)) { profile_["compression"] = "zstd:" + std::to_string(level); }
    ImGui::SameLine();
    ImGui::TextDisabled("(3 = seconds, 19 = smaller; reading speed is the same)");
    CheckboxKey("HLSL sources as the safety net", profile_, "shaderSources", true);
    ImGui::SameLine();
    CheckboxKey("Zip", profile_, "zip", false);
    ImGui::SameLine();
    CheckboxKey("Symbol archive", profile_, "symbols", false);
    ImGui::SameLine();
    CheckboxKey("Every licence known", profile_, "strictLicences", false);
    if (profile_.value("strictLicences", false))
    {
        // The release rule stays in the profile; this build ships the assets it refuses. The last check was made the
        // other way: its errors no longer say what a build would do -- Check again.
        if (ImGui::Checkbox("Override the licences for this build", &overrideLicences_))
        {
            selectionChecked_ = false;
            checkErrors_.clear();
            checkWarnings_.clear();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("An asset without a known licence in CREDITS.md (a ## section with \"license type:\" or \"License:\")\n"
                              "stops a profile that wants every licence known. Ticked, the next Check and builds ship it\n"
                              "anyway: a warning each, listed in the cook report and the history (--override-licences).\n"
                              "Not saved: the profile keeps its rule, and loading a profile clears this.");
        }
        if (overrideLicences_)
        {
            ImGui::SameLine();
            ImGui::TextColored(LevelColour("warning"), "assets without a known licence ship in this build");
        }
    }
}

void BuildDistributionPanel::DrawChecks(const Hooks& hooks)
{
    ImGui::SeparatorText("Before a build");
    const auto line = [](const char* level, const std::string& text)
    {
        ImGui::TextColored(LevelColour(level), "%s %s", std::string(level) == "error" ? "[x]" : std::string(level) == "warning" ? "[!]" : "[ok]",
                           text.c_str());
    };
    if (hooks.levelDirty && hooks.levelDirty())
    {
        line("warning", "The open level has unsaved edits: the build reads the files, edits in memory are not in it.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Save the level"))
        {
            std::string why;
            message_ = hooks.saveLevel && hooks.saveLevel(why) ? "Level saved." : "Level not saved: " + why;
        }
    }
    if (!gitKnown_) { line("warning", "git status unknown"); }
    else if (!gitChanged_.empty())
    {
        line("warning", std::to_string(gitChanged_.size()) + " uncommitted change(s) (" + gitChanged_.front() +
                        (gitChanged_.size() > 1 ? ", ..." : "") + "): the version gets -dirty");
    }
    else { line("info", "the tree is committed"); }
    const bool development = flavour_ == "development";
    if (!development && profile_.value("encrypt", false) && keyGuid_.empty()) { line("error", "encryption is on and there is no key"); }
    std::error_code ec;
    const char* exeConfig = development ? "Development" : "Shipping";
    const bool cook = fs::is_regular_file(vfs::ContentRoot() / L"x64" / L"Cook" / L"test_cube.exe", ec);
    const bool ship = fs::is_regular_file(vfs::ContentRoot() / L"x64" / vfs::Widen(exeConfig) / L"test_cube.exe", ec);
    if (!cook || !ship)
    {
        line("warning", std::string("not built: ") + (cook ? "" : "Cook ") + (ship ? "" : exeConfig) + " -- Full build builds them");
    }
    ULARGE_INTEGER freeBytes{};
    if (GetDiskFreeSpaceExW(vfs::ContentRoot().c_str(), &freeBytes, nullptr, nullptr) && freeBytes.QuadPart < (3ull << 30))
    {
        line("warning", "under 3 GB free on the build's drive");
    }
    if (LicencesOverridden())
    {
        line("warning", "the licence check is overridden for this build: assets without a known licence ship");
    }
    if (!selectionChecked_) { line("info", "Check walks the shipped levels: missing files, licences, sizes"); }
    else if (checkErrors_.empty()) { line("info", "the last check: no errors, " + std::to_string(checkWarnings_.size()) + " warning(s)"); }
    else { line("error", "the last check: " + std::to_string(checkErrors_.size()) + " error(s) -- no build until they are gone"); }
    if (selectionChecked_ && (!checkErrors_.empty() || !checkWarnings_.empty()) &&
        ImGui::TreeNode("check_list", "the check's errors and warnings"))
    {
        for (const std::string& e : checkErrors_) { DrawMessage(e, "error", hooks); }
        for (const std::string& w : checkWarnings_) { DrawMessage(w, "warning", hooks); }
        if (!selectStatus_.empty()) { ImGui::TextDisabled("%s", selectStatus_.c_str()); }
        ImGui::TreePop();
    }
}

void BuildDistributionPanel::DrawRun(const Hooks& hooks)
{
    ImGui::SeparatorText("Build");
    dist::BuildJob& job = dist::TheBuildJob();
    const bool running = job.Running();
    const bool development = flavour_ == "development";
    const bool blocked = (selectionChecked_ && !checkErrors_.empty()) ||
                         (!development && profile_.value("encrypt", false) && keyGuid_.empty());
    const char* save = Dirty() ? "Save & " : "";
    ImGui::BeginDisabled(running);
    // The two builds of the game (docs/scalability_plan.md §7): the same packages, a different exe.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Build type");
    ImGui::SameLine();
    if (ImGui::RadioButton("Shipping", !development)) { flavour_ = "shipping"; }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The game for players: x64/Shipping, the developer features compiled out (no F1, no --set),\n"
                          "the folder %s-<version>, encrypted when the profile says so.",
                          profile_.value("product", std::string("test_cube")).c_str());
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Development", development)) { flavour_ = "development"; }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The same packaged game for the team: x64/Development with Developer Controls (F1), the F keys and\n"
                          "the whole command line, \"DEVELOPMENT BUILD\" on screen, its PDB beside the exe, never encrypted,\n"
                          "the profile's devInclude cooked too (the ocean presets, ...). The folder %s-<version>-dev;\n"
                          "its files at %%LOCALAPPDATA%%\\test_cube_dev, apart from Shipping's.",
                          profile_.value("product", std::string("test_cube")).c_str());
    }
    const std::string exe = development ? "Development" : "Shipping";
    if (ImGui::Button((std::string(save) + "Check").c_str())) { StartRun("check", hooks); }
    ImGui::SameLine();
    if (ImGui::Button((std::string(save) + "Update record").c_str())) { StartRun("record", hooks); }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Release runs over the shipped levels: shader_list.txt and vfs_record.txt (minutes)"); }
    ImGui::SameLine();
    ImGui::BeginDisabled(blocked);
    if (ImGui::Button((std::string(save) + "Build content").c_str())) { StartRun("content", hooks); }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Cook, the folder from the %s exe as built, its check run, the notices", exe.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button((std::string(save) + "Full build").c_str())) { StartRun("full", hooks); }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("MSBuild Cook + %s with the version%s, then Build content, zip, symbols", exe.c_str(),
                          development ? "" : " and key");
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Cancel")) { job.Cancel(); }
    ImGui::EndDisabled();

    const dist::BuildStatus status = job.Status(200);
    if (status.id == 0) { return; }
    // A finished run's report is read once: the size check fills the level table, a check of the shipped levels the checks.
    if (status.finished && status.id != handledJobId_)
    {
        handledJobId_ = status.id;
        if (status.request.mode == "check" && !status.report.empty()) { ReadCheckReport(status.report, sizesJob_); }
        sizesJob_ = false;
        Refresh();
    }
    const std::string title = "#" + std::to_string(status.id) + " " + status.request.mode + " " + status.request.profile +
                              (status.request.flavour == "development" ? " (Development)" : "") +
                              (status.request.overrideLicences ? " (licences overridden)" : "") + ": " +
                              (status.running ? status.stage : status.cancelled ? "cancelled" : status.exitCode == 0 ? "done" :
                                                                                  "FAILED (exit " + std::to_string(status.exitCode) + ")");
    ImGui::ProgressBar(static_cast<float>(status.running ? status.fraction : 1.0), ImVec2(-FLT_MIN, 0.0f), title.c_str());
    if (status.running && (status.stage == "modules" || status.stage == "check" || status.request.mode == "record"))
    {
        ImGui::TextColored(LevelColour("warning"), "The game runs on this GPU in this stage: the editor's frames are slower.");
    }
    std::string stages;
    for (const dist::StageTime& t : status.stages)
    {
        char text[64];
        if (t.seconds < 0.0) { std::snprintf(text, sizeof(text), "%s ...", t.stage.c_str()); }
        else { std::snprintf(text, sizeof(text), "%s %.1f s", t.stage.c_str(), t.seconds); }
        stages += (stages.empty() ? "" : "  |  ") + std::string(text);
    }
    ImGui::TextWrapped("%s", stages.c_str());
    if (ImGui::BeginChild("buildlog", ImVec2(0.0f, 150.0f), ImGuiChildFlags_Borders))
    {
        for (std::size_t i = 0; i < status.events.size(); ++i)
        {
            const dist::BuildEvent& e = status.events[i];
            if (e.event == "stage_end") { continue; }
            ImGui::TextColored(LevelColour(e.level), "[%s] %s", e.stage.c_str(), e.message.c_str());
        }
        if (status.running) { ImGui::SetScrollHereY(1.0f); }
    }
    ImGui::EndChild();
}

void BuildDistributionPanel::DrawResult()
{
    const dist::BuildStatus status = dist::TheBuildJob().Status();
    if (!status.finished || status.exitCode != 0 || status.folder.empty()) { return; }
    ImGui::SeparatorText("Result");
    ImGui::Text("%s -- %s, %s", status.version.c_str(), status.folder.c_str(), MegaBytes(status.bytes).c_str());
    if (!status.zip.empty()) { ImGui::Text("zip: %s", status.zip.c_str()); }
    if (ImGui::Button("Open folder"))
    {
        ShellExecuteW(nullptr, L"open", vfs::Widen(status.folder).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("Run the build"))
    {
        // As a player starts it, with its own user folder in %TEMP% so the editor's machine-wide one stays as it is.
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const fs::path exe = fs::path(vfs::Widen(status.folder)) / L"test_cube.exe";
        const wchar_t* tryFolder = status.request.flavour == "development" ? L"test_cube_try_dev" : L"test_cube_try";
        std::wstring cmd = L"\"" + exe.wstring() + L"\" \"--user-dir=" + (fs::path(temp) / tryFolder).wstring() + L"\"";
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION info{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(), &startup, &info))
        {
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
        }
        else { message_ = "Could not start " + vfs::Narrow(exe.wstring()); }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(status.report.empty());
    if (ImGui::Button("Open the report"))
    {
        ShellExecuteW(nullptr, L"open", vfs::Widen(status.report).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::EndDisabled();
}

void BuildDistributionPanel::DrawHistory()
{
    if (history_.empty()) { return; }
    if (!ImGui::CollapsingHeader("History")) { return; }
    int toggledBuild = 0;
    bool toggledTo = false;
    if (ImGui::BeginTable("history", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 120.0f)))
    {
        ImGui::TableSetupColumn("Build", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("Version");
        ImGui::TableSetupColumn("Published", ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn("Profile", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Levels");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();
        for (auto it = history_.rbegin(); it != history_.rend(); ++it)
        {
            const nlohmann::json& h = *it;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", h.value("build", 0));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(h.value("versionString", std::string()).c_str());
            ImGui::TableNextColumn();
            // Handed to players: a profile with versionMustGrow then needs a version above it (build_dist.py). Not while a
            // run is going -- it writes the history at its end.
            if (h.value("result", std::string()) == "built")
            {
                bool published = h.value("published", false);
                ImGui::PushID(h.value("build", 0));
                ImGui::BeginDisabled(dist::TheBuildJob().Running());
                if (ImGui::Checkbox("##published", &published))
                {
                    toggledBuild = h.value("build", 0);
                    toggledTo = published;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    const std::string when = h.value("publishedDate", std::string());
                    ImGui::SetTooltip("%s\nPublished = handed to players. A profile with versionMustGrow (release) then needs a "
                                      "version above it;\na version only built (not published) can be built again.",
                                      published ? ("Published " + when).c_str() : "Not published");
                }
            }
            ImGui::TableNextColumn();
            // Builds before the flavour existed were all Shipping.
            ImGui::TextUnformatted(h.value("flavour", std::string("shipping")) == "development" ? "Development" : "Shipping");
            ImGui::TableNextColumn();
            // A build that shipped assets without a known licence (--override-licences) says so, and which.
            const nlohmann::json overridden = h.value("licencesOverridden", nlohmann::json::array());
            if (overridden.is_array() && !overridden.empty())
            {
                ImGui::TextColored(LevelColour("warning"), "%s *", h.value("profile", std::string()).c_str());
                if (ImGui::IsItemHovered())
                {
                    std::string list;
                    for (const nlohmann::json& a : overridden)
                    {
                        if (a.is_string()) { list += "\n  " + a.get<std::string>(); }
                    }
                    ImGui::SetTooltip("Shipped without a known licence (licences overridden):%s", list.c_str());
                }
            }
            else { ImGui::TextUnformatted(h.value("profile", std::string()).c_str()); }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(h.value("date", std::string()).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(MegaBytes(h.value("bytes", 0ull)).c_str());
            ImGui::TableNextColumn();
            std::string levels;
            for (const nlohmann::json& l : h.value("levels", nlohmann::json::array()))
            {
                levels += (levels.empty() ? "" : ", ") + vfs::Narrow(fs::path(vfs::Widen(l.get<std::string>())).stem().wstring());
            }
            ImGui::TextUnformatted(levels.c_str());
            ImGui::TableNextColumn();
            // Out of the history and off the disk (owner, 2026-11-17), after a confirmation that lists the files. A published
            // build is not: players may hold it, and its symbols read their crash reports.
            const bool published = h.value("published", false);
            ImGui::PushID(h.value("build", 0));
            ImGui::BeginDisabled(published || dist::TheBuildJob().Running());
            if (ImGui::SmallButton("Delete"))
            {
                deleteBuild_ = h.value("build", 0);
                openDeleteConfirm_ = true;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            if (published && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("Published: players may hold it. Untick Published first if it never reached anybody.");
            }
        }
        ImGui::EndTable();
    }
    if (toggledBuild != 0) { SetPublished(toggledBuild, toggledTo); }
}

std::vector<std::string> BuildDistributionPanel::DeletionPlan(const nlohmann::json& entry) const
{
    // build_dist.py delete_builds, read-only: a later build of the same version replaced the folder and the zip, so what
    // lies there is its own (the script also reads the folder's manifest before it deletes anything).
    std::vector<std::string> plan;
    const int build = entry.value("build", 0);
    std::error_code ec;
    const auto laterOwner = [&](const char* key, const std::string& value) -> int {
        for (const nlohmann::json& h : history_)
        {
            if (h.is_object() && h.value("build", 0) > build && h.value("result", std::string()) == "built" &&
                h.value(key, std::string()) == value)
            {
                return h.value("build", 0);
            }
        }
        return 0;
    };
    for (const char* key : { "folder", "zip" })
    {
        const nlohmann::json value = entry.value(key, nlohmann::json());
        if (!value.is_string() || value.get<std::string>().empty()) { continue; }
        const std::string path = value.get<std::string>();
        if (!fs::exists(vfs::Widen(path), ec)) { continue; }
        const int owner = laterOwner(key, path);
        plan.push_back(owner ? "kept: " + path + " (build " + std::to_string(owner) + "'s now)" : "delete: " + path);
        // A profile without symbols keeps the cook report beside the folder, and it goes with it.
        const fs::path report = RepoPath(OutFolder()) / (fs::path(vfs::Widen(path)).filename().wstring() + L".cook_report.json");
        if (std::string(key) == "folder" && !owner && fs::exists(report, ec)) { plan.push_back("delete: " + vfs::Narrow(report.wstring())); }
    }
    const std::string versionText = entry.value("versionString", std::string());
    if (!versionText.empty())
    {
        const std::string name = versionText.substr(0, versionText.find(' ')) +
                                 (entry.value("flavour", std::string("shipping")) == "shipping" ? "" : "-dev");
        const fs::path symbols = RepoPath("dist_symbols") / vfs::Widen(name);
        if (fs::exists(symbols, ec)) { plan.push_back("delete: " + vfs::Narrow(symbols.wstring()) + " (its symbols)"); }
    }
    plan.push_back("its entry in the history");
    return plan;
}

void BuildDistributionPanel::DrawDeleteConfirm()
{
    if (openDeleteConfirm_)
    {
        ImGui::OpenPopup("Delete a build");
        openDeleteConfirm_ = false;
    }
    if (!ImGui::BeginPopupModal("Delete a build", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) { return; }
    const nlohmann::json* entry = nullptr;
    for (const nlohmann::json& h : history_)
    {
        if (h.is_object() && h.value("build", 0) == deleteBuild_) { entry = &h; }
    }
    if (!entry)
    {
        ImGui::TextUnformatted("That build is no longer in the history.");
        if (ImGui::Button("Close")) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
        return;
    }
    ImGui::Text("Build %d (%s, %s): off the disk and out of the history. This cannot be undone.", deleteBuild_,
                entry->value("versionString", std::string()).c_str(), entry->value("profile", std::string()).c_str());
    for (const std::string& line : DeletionPlan(*entry)) { ImGui::BulletText("%s", line.c_str()); }
    ImGui::Spacing();
    if (ImGui::Button("Delete"))
    {
        dist::BuildRequest request;
        request.profile = profileName_;
        request.mode = "delete";
        request.deleteBuild = deleteBuild_;
        std::string why;
        if (!ValidateRequest(request, why) || !dist::TheBuildJob().Start(request, why)) { message_ = "Not deleted: " + why; }
        deleteBuild_ = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        deleteBuild_ = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void BuildDistributionPanel::Draw(bool* open, const Hooks& hooks)
{
    ImGui::SetNextWindowSize(ImVec2(1100.0f, 1250.0f), ImGuiCond_FirstUseEver);
    // Tall enough for the run and its log without scrolling past the profile (imgui.ini keeps a smaller one otherwise).
    ImGui::SetNextWindowSizeConstraints(ImVec2(860.0f, 1000.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin("Build Distribution", open))
    {
        ImGui::End();
        return;
    }
    if (!refreshed_) { Refresh(); }
    if (!openOn_.empty())
    {
        const std::size_t comma = openOn_.find(',');
        const std::string name = openOn_.substr(0, comma);
        if (std::find(profiles_.begin(), profiles_.end(), name) != profiles_.end()) { LoadProfile(name); }
        else { LOG_WARNING(logging::LogCategory::Editor, "--build-dist={}: no profile tools/dist/{}.json", openOn_, name); }
        // After the profile, comma-separated: override-licences (ticked), delete=<build> (its confirmation open).
        overrideLicences_ = false;
        for (std::size_t at = comma; at != std::string::npos;)
        {
            const std::size_t next = openOn_.find(',', at + 1);
            const std::string item = openOn_.substr(at + 1, next == std::string::npos ? std::string::npos : next - at - 1);
            if (item == "override-licences") { overrideLicences_ = true; }
            else if (item.rfind("delete=", 0) == 0)
            {
                deleteBuild_ = std::atoi(item.c_str() + 7);
                openDeleteConfirm_ = deleteBuild_ > 0;
            }
            at = next;
        }
        openOn_.clear();
    }
    // The level table's sizes, once: the walk over every level on disk (a check, a second or two) -- unless a build has run
    // in this editor already (MCP opened the window for it): its status is what the caller polls, a check would replace it.
    if (!sizesRequested_ && !profileName_.empty())
    {
        sizesRequested_ = true;
    }
    if (sizesRequested_ && !sizesStarted_ && dist::TheBuildJob().Status().id == 0)
    {
        sizesStarted_ = true;
        dist::BuildRequest request;
        request.profile = profileName_;
        request.mode = "check";
        request.levels = levelsOnDisk_;
        request.overrideLicences = LicencesOverridden();
        // Opening the window must not start a compile: this one takes x64/Cook as it is (a size estimate). Check and
        // Measure sizes bring it up to date first (build_dist.py, 2026-11-17).
        request.skipBuild = true;
        std::string why;
        if (dist::TheBuildJob().Start(request, why)) { sizesJob_ = true; }
    }
    if (ImGui::SmallButton("Refresh")) { Refresh(); }
    ImGui::SameLine();
    ImGui::TextDisabled("docs/distribution_build_plan.md -- the same profile from the console: python tools/build_dist.py --profile=%s",
                        profileName_.c_str());
    DrawProfile();
    DrawLevels(hooks);
    DrawContent();
    DrawOptions();
    DrawChecks(hooks);
    DrawRun(hooks);
    DrawResult();
    DrawHistory();
    DrawDeleteConfirm(); // outside History: its header may be folded
    if (!message_.empty()) { ImGui::TextWrapped("%s", message_.c_str()); }
    ImGui::End();
}

#else

bool BuildDistributionPanel::ValidateRequest(const dist::BuildRequest&, std::string& why)
{
    why = "no editor in this build";
    return false;
}

#endif // WITH_EDITOR
