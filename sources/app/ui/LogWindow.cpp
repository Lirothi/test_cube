#include "app/ui/LogWindow.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "core/logging/Log.h"
#include "imgui.h"

namespace
{
    constexpr const char* kLevelTags[logging::kLogLevelCount] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };

    ImVec4 LevelColor(logging::LogLevel level)
    {
        switch (level)
        {
        case logging::LogLevel::Trace:   return ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
        case logging::LogLevel::Debug:   return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
        case logging::LogLevel::Info:    return ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
        case logging::LogLevel::Warning: return ImVec4(1.00f, 0.80f, 0.30f, 1.0f);
        case logging::LogLevel::Error:   return ImVec4(1.00f, 0.40f, 0.35f, 1.0f);
        case logging::LogLevel::Fatal:   return ImVec4(1.00f, 0.20f, 0.60f, 1.0f);
        }
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    // Case-insensitive substring; ASCII folding is enough for log text and search terms.
    bool ContainsNoCase(std::string_view haystack, std::string_view needle)
    {
        if (needle.empty()) { return true; }
        if (needle.size() > haystack.size()) { return false; }
        const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
        for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
        {
            std::size_t j = 0;
            while (j < needle.size() && lower(haystack[i + j]) == lower(needle[j])) { ++j; }
            if (j == needle.size()) { return true; }
        }
        return false;
    }

    std::string_view BaseName(const char* path)
    {
        if (path == nullptr) { return {}; }
        const std::string_view full(path);
        const std::size_t slash = full.find_last_of("/\\");
        return slash == std::string_view::npos ? full : full.substr(slash + 1);
    }
}

void LogWindow::Draw()
{
    if (!open_)
    {
        return; // the cost rule: a closed viewer does no work at all
    }

    if (scratch_.empty())
    {
        scratch_.resize(kPullBatch);
        entries_.reserve(kMaxEntries);
    }
    if (!sessionPathKnown_)
    {
        sessionPathKnown_ = logging::GetSessionFilePath(sessionPath_, sizeof(sessionPath_)) != 0 || logging::IsInitialized();
        if (sessionPath_[0] == '\0') { std::snprintf(sessionPath_, sizeof(sessionPath_), "<no session file>"); }
    }
    if (qpcFrequency_ == 0)
    {
        logging::GetSessionEpoch(&qpcStart_, &qpcFrequency_);
    }

    if (!paused_)
    {
        PullNewRecords();
    }
    if (visibleDirty_)
    {
        RebuildVisible();
    }

    ImGui::SetNextWindowSize(ImVec2(1180.0f, 560.0f), ImGuiCond_FirstUseEver);
    bool open = open_;
    if (ImGui::Begin("Session Log###SessionLog", &open))
    {
        ui::HandleWindowTitleDoubleClickMaximize(maximize_);
        DrawToolbar();
        DrawStatusLine();
        DrawTable();
        if (showDetails_)
        {
            DrawDetails();
        }
    }
    ImGui::End();
    open_ = open;
}

void LogWindow::PullNewRecords()
{
    for (;;)
    {
        std::uint64_t next = cursor_;
        const std::size_t copied = logging::CopyRecentRecords(cursor_, scratch_.data(), scratch_.size(), &next);
        // CopySince advances the cursor past anything the ring overwrote before we got to it.
        if (next > cursor_ + copied)
        {
            skippedByRing_ += next - cursor_ - copied;
        }
        cursor_ = next;
        for (std::size_t i = 0; i < copied; ++i)
        {
            AppendRecord(scratch_[i]);
        }
        if (copied < scratch_.size())
        {
            break;
        }
    }
}

void LogWindow::AppendRecord(const logging::LogRecord& record)
{
    if (entries_.size() >= kMaxEntries)
    {
        // Drop the oldest half in one move rather than one erase per record.
        const std::size_t drop = kMaxEntries / 2;
        for (std::size_t i = 0; i < drop; ++i)
        {
            if (entries_[i].level == logging::LogLevel::Warning) { --warningCount_; }
            else if (entries_[i].level >= logging::LogLevel::Error) { --errorCount_; }
        }
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(drop));
    }

    Entry entry;
    entry.sequence = record.sequence;
    entry.frame = record.frame;
    entry.qpc = record.qpcTimestamp;
    entry.threadId = record.threadId;
    entry.sourceLine = record.sourceLine;
    entry.level = record.level;
    entry.category = record.category;
    entry.flags = record.flags;
    entry.sourceFile = record.sourceFile != nullptr ? record.sourceFile : "";
    entry.sourceFunction = record.sourceFunction != nullptr ? record.sourceFunction : "";
    logging::GetThreadName(record.threadId, entry.threadName, sizeof(entry.threadName));
    const std::size_t bytes = record.messageByteCount < logging::kLogMessageCapacity
        ? record.messageByteCount
        : logging::kLogMessageCapacity - 1;
    entry.message.assign(record.message, bytes);
    while (!entry.message.empty() && (entry.message.back() == '\n' || entry.message.back() == '\r'))
    {
        entry.message.pop_back();
    }
    if (entry.level == logging::LogLevel::Warning) { ++warningCount_; }
    else if (entry.level >= logging::LogLevel::Error) { ++errorCount_; }

    entries_.push_back(std::move(entry));
    visibleDirty_ = true;
    if (autoScroll_) { scrollToBottom_ = true; }
}

bool LogWindow::Passes(const Entry& entry) const
{
    if (!logging::IsValid(entry.level) || !levelEnabled_[static_cast<std::size_t>(entry.level)]) { return false; }
    if (!logging::IsValid(entry.category) || !categoryEnabled_[static_cast<std::size_t>(entry.category)]) { return false; }
    if (search_[0] != '\0' && !ContainsNoCase(entry.message, search_)) { return false; }
    return true;
}

void LogWindow::RebuildVisible()
{
    visible_.clear();
    for (std::uint32_t i = 0; i < entries_.size(); ++i)
    {
        if (Passes(entries_[i])) { visible_.push_back(i); }
    }
    visibleDirty_ = false;
}

void LogWindow::FormatLine(const Entry& entry, std::string& out) const
{
    // Same shape as the session file, with the session-relative time the viewer shows.
    char head[192];
    const double seconds = qpcFrequency_ > 0
        ? static_cast<double>(entry.qpc - qpcStart_) / static_cast<double>(qpcFrequency_)
        : 0.0;
    char frame[24];
    if (entry.frame == logging::kInvalidLogFrame) { std::snprintf(frame, sizeof(frame), "-"); }
    else { std::snprintf(frame, sizeof(frame), "%llu", static_cast<unsigned long long>(entry.frame)); }
    std::snprintf(head, sizeof(head), "%+.3f %s [%s] %s ",
                  seconds,
                  logging::IsValid(entry.level) ? kLevelTags[static_cast<std::size_t>(entry.level)] : "?",
                  logging::LogCategoryName(entry.category).data(), frame);
    out += head;
    out += entry.message;
    if (entry.level >= logging::LogLevel::Warning && entry.sourceFile[0] != '\0')
    {
        char source[96];
        std::snprintf(source, sizeof(source), " (%.*s:%u)", static_cast<int>(BaseName(entry.sourceFile).size()),
                      BaseName(entry.sourceFile).data(), entry.sourceLine);
        out += source;
    }
    out += '\n';
}

void LogWindow::CopyToClipboard(bool selectedOnly) const
{
    std::string text;
    if (selectedOnly)
    {
        for (const Entry& entry : entries_)
        {
            if (entry.sequence == selectedSequence_) { FormatLine(entry, text); break; }
        }
    }
    else
    {
        text.reserve(visible_.size() * 96);
        for (const std::uint32_t index : visible_)
        {
            FormatLine(entries_[index], text);
        }
    }
    if (!text.empty())
    {
        ImGui::SetClipboardText(text.c_str());
    }
}

void LogWindow::EmitTestRecords(int count)
{
    // The 10k-record responsiveness check from the plan's L8 gate, reachable from the window
    // itself: real records through the real producer path, spread over levels and categories.
    for (int i = 0; i < count; ++i)
    {
        const auto category = static_cast<logging::LogCategory>(i % static_cast<int>(logging::kLogCategoryCount));
        switch (i % 10)
        {
        case 0:  LOG_WARNING(category, "viewer test record {} of {} (warning)", i, count); break;
        case 1:  LOG_ERROR(category, "viewer test record {} of {} (error)", i, count); break;
        case 2:  LOG_DEBUG(category, "viewer test record {} of {} (debug)", i, count); break;
        default: LOG_INFO(category, "viewer test record {} of {}", i, count); break;
        }
    }
}

void LogWindow::DrawToolbar()
{
    if (ImGui::Checkbox("Pause", &paused_)) { /* resuming pulls whatever the ring still holds */ }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll_);
    ImGui::SameLine();
    ImGui::Checkbox("Details", &showDetails_);
    ImGui::SameLine();
    if (ImGui::Button("Clear view"))
    {
        // View only: the session file and the core ring are untouched.
        entries_.clear();
        visible_.clear();
        warningCount_ = 0;
        errorCount_ = 0;
        selectedSequence_ = 0;
        visibleDirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy visible")) { CopyToClipboard(false); }
    ImGui::SameLine();
    ImGui::BeginDisabled(selectedSequence_ == 0);
    if (ImGui::Button("Copy selected")) { CopyToClipboard(true); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Emit 10k")) { EmitTestRecords(10000); }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Responsiveness check: 10 000 real records through the producer path"); }

    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::InputTextWithHint("##search", "search message (case-insensitive)", search_, sizeof(search_)))
    {
        visibleDirty_ = true;
    }
    ImGui::SameLine();
    for (std::size_t i = 0; i < logging::kLogLevelCount; ++i)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(static_cast<logging::LogLevel>(i)));
        if (ImGui::Checkbox(kLevelTags[i], &levelEnabled_[i])) { visibleDirty_ = true; }
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    if (ImGui::Button("Categories"))
    {
        ImGui::OpenPopup("LogCategories");
    }
    if (ImGui::BeginPopup("LogCategories"))
    {
        if (ImGui::SmallButton("All")) { std::fill(std::begin(categoryEnabled_), std::end(categoryEnabled_), true); visibleDirty_ = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) { std::fill(std::begin(categoryEnabled_), std::end(categoryEnabled_), false); visibleDirty_ = true; }
        ImGui::Separator();
        for (std::size_t i = 0; i < logging::kLogCategoryCount; ++i)
        {
            const std::string_view name = logging::LogCategoryName(static_cast<logging::LogCategory>(i));
            char label[48];
            std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(name.size()), name.data());
            if (ImGui::Checkbox(label, &categoryEnabled_[i])) { visibleDirty_ = true; }
        }
        ImGui::EndPopup();
    }
}

void LogWindow::DrawStatusLine()
{
    logging::LogStatistics stats{};
    logging::GetStatistics(stats);
    ImGui::TextDisabled("%s", sessionPath_);
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Session file (click Copy path)"); }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy path")) { ImGui::SetClipboardText(sessionPath_); }
    ImGui::Text("%zu records (%zu visible)", entries_.size(), visible_.size());
    ImGui::SameLine();
    ImGui::TextColored(LevelColor(logging::LogLevel::Warning), "warnings %zu", warningCount_);
    ImGui::SameLine();
    ImGui::TextColored(LevelColor(logging::LogLevel::Error), "errors %zu", errorCount_);
    ImGui::SameLine();
    const std::uint64_t dropped = stats.DroppedTotal();
    if (dropped != 0)
    {
        ImGui::TextColored(LevelColor(logging::LogLevel::Error), "dropped %llu", static_cast<unsigned long long>(dropped));
    }
    else
    {
        ImGui::TextDisabled("dropped 0");
    }
    if (skippedByRing_ != 0)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(%llu overwritten in the ring before the viewer read them)",
                            static_cast<unsigned long long>(skippedByRing_));
    }
    if (paused_)
    {
        ImGui::SameLine();
        ImGui::TextColored(LevelColor(logging::LogLevel::Warning), "PAUSED");
    }
}

void LogWindow::DrawTable()
{
    const float detailsHeight = showDetails_ ? 110.0f : 0.0f;
    const ImVec2 size(0.0f, -detailsHeight);
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("LogRows", 6, kFlags, size))
    {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 52.0f);
    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 128.0f);
    ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthFixed, 56.0f);
    ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible_.size()));
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
        {
            const Entry& entry = entries_[visible_[static_cast<std::size_t>(row)]];
            ImGui::TableNextRow();
            ImGui::PushID(row);
            ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(entry.level));

            ImGui::TableSetColumnIndex(0);
            const double seconds = qpcFrequency_ > 0
                ? static_cast<double>(entry.qpc - qpcStart_) / static_cast<double>(qpcFrequency_)
                : 0.0;
            char time[32];
            std::snprintf(time, sizeof(time), "%+.3f", seconds);
            const bool selected = entry.sequence == selectedSequence_;
            if (ImGui::Selectable(time, selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            {
                selectedSequence_ = selected ? 0 : entry.sequence;
            }
            if (ImGui::IsItemHovered() && entry.sourceFile[0] != '\0')
            {
                ImGui::SetTooltip("%s:%u\n%s", entry.sourceFile, entry.sourceLine, entry.sourceFunction);
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(logging::IsValid(entry.level) ? kLevelTags[static_cast<std::size_t>(entry.level)] : "?");
            ImGui::TableSetColumnIndex(2);
            const std::string_view category = logging::LogCategoryName(entry.category);
            ImGui::TextUnformatted(category.data(), category.data() + category.size());
            ImGui::TableSetColumnIndex(3);
            if (entry.frame == logging::kInvalidLogFrame) { ImGui::TextUnformatted("-"); }
            else { ImGui::Text("%llu", static_cast<unsigned long long>(entry.frame)); }
            ImGui::TableSetColumnIndex(4);
            if (entry.threadName[0] != '\0') { ImGui::Text("%s", entry.threadName); }
            else { ImGui::Text("%u", entry.threadId); }
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(entry.message.c_str());
            if ((entry.flags & logging::LogRecordFlagTruncated) != 0)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("[truncated]");
            }

            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    if (scrollToBottom_ && autoScroll_ && !paused_)
    {
        ImGui::SetScrollHereY(1.0f);
    }
    scrollToBottom_ = false;
    ImGui::EndTable();
}

void LogWindow::DrawDetails()
{
    ImGui::Separator();
    const Entry* selected = nullptr;
    if (selectedSequence_ != 0)
    {
        for (const Entry& entry : entries_)
        {
            if (entry.sequence == selectedSequence_) { selected = &entry; break; }
        }
    }
    if (selected == nullptr)
    {
        ImGui::TextDisabled("Select a row to see its source location and full text.");
        return;
    }
    ImGui::Text("%s:%u", selected->sourceFile, selected->sourceLine);
    ImGui::TextDisabled("%s", selected->sourceFunction);
    ImGui::TextWrapped("%s", selected->message.c_str());
    if (selected->flags != 0)
    {
        ImGui::TextDisabled("flags:%s%s%s",
                            (selected->flags & logging::LogRecordFlagTruncated) ? " truncated" : "",
                            (selected->flags & logging::LogRecordFlagFormatError) ? " format-error" : "",
                            (selected->flags & logging::LogRecordFlagEmergency) ? " emergency" : "");
    }
}
