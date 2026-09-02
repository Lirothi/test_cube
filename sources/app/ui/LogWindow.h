#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/logging/LogRecord.h"
#include "ui/ImGuiWindowUtils.h"

// Session-log viewer (logging plan L8). Reads the logger's in-memory ring through the cursor API
// (core/logging/Log.h: CopyRecentRecords) and keeps its own compact copies, so the core never
// sees ImGui and the viewer never touches the writer thread.
//
// COST RULE: while the window is closed, Draw() returns before doing anything — no snapshot, no
// string copy, no filtering, no formatting. While open it pulls only records newer than its
// cursor, and re-filters only when the entry set or a filter changed.
class LogWindow
{
public:
    bool IsOpen() const { return open_; }
    void SetOpen(bool open) { open_ = open; }
    void ToggleOpen() { open_ = !open_; }

    void Draw();

private:
    struct Entry
    {
        std::uint64_t sequence = 0;
        std::uint64_t frame = 0;
        std::int64_t qpc = 0;
        std::uint32_t threadId = 0;
        std::uint32_t sourceLine = 0;
        logging::LogLevel level = logging::LogLevel::Info;
        logging::LogCategory category = logging::LogCategory::Core;
        std::uint8_t flags = 0;
        const char* sourceFile = "";     // static strings from std::source_location
        const char* sourceFunction = "";
        char threadName[32] = {};
        std::string message;
    };

    void PullNewRecords();
    void AppendRecord(const logging::LogRecord& record);
    bool Passes(const Entry& entry) const;
    void RebuildVisible();
    void FormatLine(const Entry& entry, std::string& out) const;
    void CopyToClipboard(bool selectedOnly) const;
    void DrawToolbar();
    void DrawStatusLine();
    void DrawTable();
    void DrawDetails();
    void EmitTestRecords(int count);

    static constexpr std::size_t kMaxEntries = 8192;   // twice the core ring, so a paused view keeps history
    static constexpr std::size_t kPullBatch = 256;

    ui::ImGuiWindowMaximizeState maximize_;
    std::vector<Entry> entries_;
    std::vector<std::uint32_t> visible_;               // indices into entries_ that pass the filter
    std::vector<logging::LogRecord> scratch_;          // pull buffer, allocated on first open
    std::uint64_t cursor_ = 0;
    std::uint64_t skippedByRing_ = 0;                  // overwritten in the core ring before we pulled
    std::uint64_t selectedSequence_ = 0;               // 0 = nothing selected
    std::size_t warningCount_ = 0;                     // over ALL retained entries, not just visible
    std::size_t errorCount_ = 0;
    std::int64_t qpcStart_ = 0;
    std::int64_t qpcFrequency_ = 0;
    bool levelEnabled_[logging::kLogLevelCount] = { true, true, true, true, true, true };
    bool categoryEnabled_[logging::kLogCategoryCount] = {
        true, true, true, true, true, true, true, true, true, true, true, true, true, true, true };
    char search_[128] = "";
    char sessionPath_[512] = "";
    bool sessionPathKnown_ = false;
    bool paused_ = false;
    bool autoScroll_ = true;
    bool showDetails_ = true;
    bool visibleDirty_ = true;
    bool scrollToBottom_ = false;
    bool open_ = false;
};
