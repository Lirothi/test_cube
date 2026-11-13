#include "app/game/BotMemory.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>

#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"
#include "third_party/json/json.hpp"

namespace game
{
    namespace
    {
        constexpr int kFileVersion = 1;

        float Trust(float seconds) { return std::clamp(seconds / BotMemory::kTurnTrusted, 0.0f, 1.0f); }

        // A file name's part before its first dot: "small_yacht.mesh.bin" -> "small_yacht".
        std::string Stem(const std::string& path)
        {
            const std::string name = std::filesystem::path(path).filename().string();
            const std::size_t dot = name.find('.');
            return dot == std::string::npos ? name : name.substr(0, dot);
        }

        std::map<std::string, std::unique_ptr<BotMemory>>& Registry()
        {
            static std::map<std::string, std::unique_ptr<BotMemory>> memories;
            return memories;
        }
    }

    void BotMemory::Touch(int bin)
    {
        dirty_ = true;
        if (seconds_[bin] < kTurnTrusted) { return; }
        const float was = versionCurvature_[bin];
        if (!versionTrusted_[bin] || std::fabs(curvature_[bin] - was) > 0.03f * std::max(was, 1e-4f))
        {
            versionTrusted_[bin] = true;
            versionCurvature_[bin] = curvature_[bin];
            ++version_;
        }
    }

    void BotMemory::LearnTurn(float speed, float curvature, float seconds)
    {
        if (!(curvature > 0.0f) || !(seconds > 0.0f) || !std::isfinite(curvature)) { return; }
        const float at = std::clamp(std::fabs(speed) / kTurnBin, 0.0f, static_cast<float>(kTurnBins - 1));
        const int low = std::min(static_cast<int>(at), kTurnBins - 1);
        const int high = std::min(low + 1, kTurnBins - 1);
        const float upper = at - static_cast<float>(low);
        const int bins[2] = { low, high };
        const float weights[2] = { 1.0f - upper, high == low ? 0.0f : upper };
        for (int k = 0; k < 2; ++k)
        {
            const int b = bins[k];
            const float w = weights[k];
            if (w <= 0.0f) { continue; }
            if (seconds_[b] <= 0.0f) { curvature_[b] = curvature; }
            else { curvature_[b] += (curvature - curvature_[b]) * (1.0f - std::exp(-seconds * w / kTurnTau)); }
            seconds_[b] = std::min(seconds_[b] + seconds * w, 600.0f);
            Touch(b);
        }
    }

    float BotMemory::TurnTrust(float speed, float* curvature) const
    {
        const float at = std::clamp(std::fabs(speed) / kTurnBin, 0.0f, static_cast<float>(kTurnBins - 1));
        const int low = std::min(static_cast<int>(at), kTurnBins - 1);
        const int high = std::min(low + 1, kTurnBins - 1);
        const float upper = at - static_cast<float>(low);
        const float wLow = (1.0f - upper) * Trust(seconds_[low]);
        const float wHigh = (high == low ? 0.0f : upper) * Trust(seconds_[high]);
        const float trust = wLow + wHigh;
        if (trust > 0.0f && curvature) { *curvature = (wLow * curvature_[low] + wHigh * curvature_[high]) / trust; }
        return trust;
    }

    int BotMemory::TrustedBins() const
    {
        int trusted = 0;
        for (int b = 0; b < kTurnBins; ++b) { trusted += seconds_[b] >= kTurnTrusted ? 1 : 0; }
        return trusted;
    }

    void BotMemory::Clear()
    {
        const std::string path = path_;
        *this = BotMemory{};
        path_ = path;
        dirty_ = true;
    }

    bool BotMemory::Load(const std::string& path)
    {
        path_ = path;
        // Content (D6: the shipped package, under the user's own saves): through the VFS.
        if (path.empty() || !vfs::Exists(path)) { return false; }
        nlohmann::json j;
        try
        {
            vfs::InStream file(path, /*binary*/ false);
            file >> j;
        }
        catch (const std::exception& e)
        {
            LOG_WARNING(logging::LogCategory::App, "bot memory: {} unreadable ({}): starts with nothing", path, e.what());
            return false;
        }
        if (j.value("version", 0) != kFileVersion)
        {
            LOG_WARNING(logging::LogCategory::App, "bot memory: {} is version {}, this build reads {}: starts with nothing", path, j.value("version", 0), kFileVersion);
            return false;
        }
        const nlohmann::json& turn = j.value("turn", nlohmann::json::object());
        const nlohmann::json curvature = turn.value("curvature", nlohmann::json::array());
        const nlohmann::json seconds = turn.value("seconds", nlohmann::json::array());
        if (turn.value("bin", kTurnBin) == kTurnBin)
        {
            for (int b = 0; b < kTurnBins && b < static_cast<int>(curvature.size()) && b < static_cast<int>(seconds.size()); ++b)
            {
                curvature_[b] = curvature[static_cast<std::size_t>(b)].get<float>();
                seconds_[b] = seconds[static_cast<std::size_t>(b)].get<float>();
                versionCurvature_[b] = curvature_[b];
                versionTrusted_[b] = seconds_[b] >= kTurnTrusted;
            }
        }
        version_ = 1;
        dirty_ = false;
        return true;
    }

    bool BotMemory::Save()
    {
        if (path_.empty()) { return false; }
        const auto round = [](float v) { return std::round(v * 1.0e5f) / 1.0e5f; };
        nlohmann::json j;
        j["version"] = kFileVersion;
        nlohmann::json curvature = nlohmann::json::array();
        nlohmann::json seconds = nlohmann::json::array();
        for (int b = 0; b < kTurnBins; ++b)
        {
            curvature.push_back(round(curvature_[b]));
            seconds.push_back(std::round(seconds_[b] * 10.0f) / 10.0f);
        }
        j["turn"] = { { "bin", kTurnBin }, { "curvature", curvature }, { "seconds", seconds } };
        // The user's file: where the game keeps what it learns (dev: the same path under the repo root).
        std::error_code ec;
        const std::filesystem::path target = vfs::WritablePath(vfs::Writable::Saved, path_);
        std::filesystem::create_directories(target.parent_path(), ec);
        const std::filesystem::path temp = std::filesystem::path(target).concat(L".tmp");
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                LOG_WARNING(logging::LogCategory::App, "bot memory: cannot write {}", temp.string());
                return false;
            }
            file << j.dump(1) << "\n";
        }
        std::filesystem::rename(temp, target, ec);
        if (ec)
        {
            LOG_WARNING(logging::LogCategory::App, "bot memory: cannot put {} in place ({})", path_, ec.message());
            return false;
        }
        dirty_ = false;
        return true;
    }

    std::string BotMemory::Describe() const
    {
        int first = -1;
        int last = -1;
        for (int b = 0; b < kTurnBins; ++b)
        {
            if (seconds_[b] >= kTurnTrusted)
            {
                if (first < 0) { first = b; }
                last = b;
            }
        }
        char line[256];
        if (first >= 0)
        {
            std::snprintf(line, sizeof(line), "turn map: %d of %d speeds trusted (%d-%d m/s)", TrustedBins(), kTurnBins, first, last);
        }
        else { std::snprintf(line, sizeof(line), "turn map: nothing trusted yet"); }
        return line;
    }

    std::string BotMemoryPath(const std::string& level, const std::string& boat)
    {
        return "data/bot_memory/" + Stem(level) + "/" + Stem(boat) + ".json";
    }

    BotMemory& BotMemoryFor(const std::string& level, const std::string& boat)
    {
        std::unique_ptr<BotMemory>& memory = Registry()[level + "|" + boat];
        if (!memory)
        {
            memory = std::make_unique<BotMemory>();
            if (!level.empty())
            {
                const std::string path = BotMemoryPath(level, boat);
                if (memory->Load(path)) { LOG_INFO(logging::LogCategory::App, "bot memory: read {} -- {}", path, memory->Describe()); }
                else { LOG_INFO(logging::LogCategory::App, "bot memory: none yet for {} on {} ({} on the first save)", Stem(boat), Stem(level), path); }
            }
        }
        return *memory;
    }

    void ForgetBotMemories(const std::string& level)
    {
        // The level's files, the boats not read yet among them, and the ones read: emptied.
        std::error_code ec;
        const std::filesystem::path folder =
            vfs::WritablePath(vfs::Writable::Saved, BotMemoryPath(level, "x")).parent_path();
        for (const std::filesystem::directory_entry& file : std::filesystem::directory_iterator(folder, ec)) // vfs-ok: the user's own saves
        {
            if (file.path().extension() == ".json") { std::filesystem::remove(file.path(), ec); }
        }
        for (auto& [key, memory] : Registry())
        {
            if (!memory || key.compare(0, level.size() + 1, level + "|") != 0) { continue; }
            memory->Clear();
            memory->Save();
            LOG_INFO(logging::LogCategory::App, "bot memory: forgot {}", memory->Path());
        }
    }

    void SaveBotMemories()
    {
        for (auto& [key, memory] : Registry())
        {
            if (memory && memory->Dirty() && memory->Save()) { LOG_INFO(logging::LogCategory::App, "bot memory: wrote {} -- {}", memory->Path(), memory->Describe()); }
        }
    }
}
