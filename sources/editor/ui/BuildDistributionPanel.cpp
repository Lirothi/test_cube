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
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
    namespace fs = std::filesystem;

    fs::path RepoPath(const std::string& rel) { return vfs::ContentRoot() / vfs::Widen(rel); }

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
    if (std::find_if(std::begin(modes), std::end(modes), [&](const char* m) { return request.mode == m; }) == std::end(modes))
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
    {
        std::istringstream lines(ReadFileText(RepoPath("tools/dist/shader_list.txt")));
        for (std::string line; std::getline(lines, line);)
        {
            if (!line.empty() && line[0] != '#' && line != "\r") { ++shaderListLines_; }
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
}

void BuildDistributionPanel::DrawLevels()
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
        for (const std::string& e : allLevelsErrors_) { ImGui::TextColored(LevelColour("error"), "%s", e.c_str()); }
        ImGui::TreePop();
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
    if (!selectionChecked_) { line("info", "Check walks the shipped levels: missing files, licences, sizes"); }
    else if (checkErrors_.empty()) { line("info", "the last check: no errors, " + std::to_string(checkWarnings_.size()) + " warning(s)"); }
    else { line("error", "the last check: " + std::to_string(checkErrors_.size()) + " error(s) -- no build until they are gone"); }
    if (selectionChecked_ && (!checkErrors_.empty() || !checkWarnings_.empty()) &&
        ImGui::TreeNode("check_list", "the check's errors and warnings"))
    {
        for (const std::string& e : checkErrors_) { ImGui::TextColored(LevelColour("error"), "%s", e.c_str()); }
        for (const std::string& w : checkWarnings_) { ImGui::TextColored(LevelColour("warning"), "%s", w.c_str()); }
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

    const dist::BuildStatus status = job.Status();
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
                              (status.request.flavour == "development" ? " (Development)" : "") + ": " +
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
        const std::size_t from = status.events.size() > 200 ? status.events.size() - 200 : 0;
        for (std::size_t i = from; i < status.events.size(); ++i)
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
    if (ImGui::BeginTable("history", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 120.0f)))
    {
        ImGui::TableSetupColumn("Build", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("Version");
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn("Profile", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Levels");
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
            // Builds before the flavour existed were all Shipping.
            ImGui::TextUnformatted(h.value("flavour", std::string("shipping")) == "development" ? "Development" : "Shipping");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(h.value("profile", std::string()).c_str());
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
        }
        ImGui::EndTable();
    }
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
        std::string why;
        if (dist::TheBuildJob().Start(request, why)) { sizesJob_ = true; }
    }
    if (ImGui::SmallButton("Refresh")) { Refresh(); }
    ImGui::SameLine();
    ImGui::TextDisabled("docs/distribution_build_plan.md -- the same profile from the console: python tools/build_dist.py --profile=%s",
                        profileName_.c_str());
    DrawProfile();
    DrawLevels();
    DrawContent();
    DrawOptions();
    DrawChecks(hooks);
    DrawRun(hooks);
    DrawResult();
    DrawHistory();
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
