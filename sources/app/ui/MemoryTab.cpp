#include "app/ui/MemoryTab.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"

namespace
{
double Mb(std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

double DeltaMb(std::uint64_t now, std::uint64_t then)
{
    return (static_cast<double>(now) - static_cast<double>(then)) / (1024.0 * 1024.0);
}

bool ContainsNoCase(const char* text, const char* needle)
{
    if (needle == nullptr || needle[0] == '\0') { return true; }
    const std::size_t n = std::strlen(needle);
    for (const char* p = text; *p != '\0'; ++p)
    {
        std::size_t i = 0;
        while (i < n && p[i] != '\0' &&
               std::tolower(static_cast<unsigned char>(p[i])) == std::tolower(static_cast<unsigned char>(needle[i])))
        {
            ++i;
        }
        if (i == n) { return true; }
    }
    return false;
}

// A tag's own totals plus its children's: the census keeps each tag separately, the tree sums.
gpu::TagTotals Subtree(const gpu::CensusTotals& census, std::uint32_t tag)
{
    gpu::TagTotals sum = census.tags[tag];
    for (std::uint32_t c = 0; c < kMemTagCount; ++c)
    {
        if (static_cast<std::uint32_t>(kMemTagParents[c]) != tag) { continue; }
        const gpu::TagTotals& ct = census.tags[c];
        sum.bytes[0] += ct.bytes[0];
        sum.bytes[1] += ct.bytes[1];
        sum.resources += ct.resources;
        sum.heaps += ct.heaps;
    }
    return sum;
}

// The change since the snapshot, growth in a warm colour: a column that only moves when something
// moved is read at a glance.
void DeltaCell(double mb)
{
    if (mb > 0.05) { ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%+.1f", mb); }
    else if (mb < -0.05) { ImGui::TextColored(ImVec4(0.45f, 0.8f, 1.0f, 1.0f), "%+.1f", mb); }
    else { ImGui::TextDisabled("0"); }
}
} // namespace

void MemoryTab::Refresh(ID3D12Device* device)
{
    platform_ = render::ReadMemoryPlatform(device);
    gpu::ReadCensus(census_);
    gpu::SnapshotRecords(records_);
    orderDirty_ = true;
}

void MemoryTab::RebuildOrder()
{
    order_.clear();
    order_.reserve(records_.size());
    for (int i = 0; i < static_cast<int>(records_.size()); ++i)
    {
        const gpu::RecordInfo& r = records_[static_cast<std::size_t>(i)];
        if (ContainsNoCase(r.name.c_str(), filter_) || ContainsNoCase(MemTagName(r.tag), filter_))
        {
            order_.push_back(i);
        }
    }
    const int column = sortColumn_;
    const bool ascending = sortAscending_;
    std::stable_sort(order_.begin(), order_.end(), [&](int ia, int ib) {
        const gpu::RecordInfo& a = records_[static_cast<std::size_t>(ia)];
        const gpu::RecordInfo& b = records_[static_cast<std::size_t>(ib)];
        int c = 0;
        switch (column)
        {
        case 0: c = a.name.compare(b.name); break;
        case 1: c = std::strcmp(MemTagName(a.tag), MemTagName(b.tag)); break;
        case 2: c = static_cast<int>(a.pool) - static_cast<int>(b.pool); break;
        case 3: c = std::strcmp(a.kind, b.kind); break;
        default: c = a.bytes < b.bytes ? -1 : (a.bytes > b.bytes ? 1 : 0); break;
        }
        return ascending ? c < 0 : c > 0;
    });
    orderDirty_ = false;
}

void MemoryTab::Draw(ID3D12Device* device)
{
    const double now = render::MemoryNowSec();
    if (lastRefreshSec_ < 0.0 || now - lastRefreshSec_ >= 0.5)
    {
        Refresh(device);
        lastRefreshSec_ = now;
    }

    ImGui::TextWrapped("Memory profiling (docs/memory_profiling_plan.md): the process, the adapter and the GPU census "
        "by owner tag, re-read twice a second. MCP get_memory reads the same; --memreport writes it all to the log.");

    ImGui::SeparatorText("Process");
    ImGui::Text("Private %.0f MB | working set %.0f MB", Mb(platform_.privateBytes), Mb(platform_.workingSetBytes));
    if (platform_.mimallocCommitBytes > 0)
    {
        ImGui::Text("mimalloc commit %.0f MB | NT heaps %.0f MB (%u)", Mb(platform_.mimallocCommitBytes),
                    Mb(platform_.ntHeapsBytes), platform_.ntHeapCount);
    }
    else
    {
        ImGui::Text("mimalloc commit n/a | NT heaps %.0f MB (%u)", Mb(platform_.ntHeapsBytes), platform_.ntHeapCount);
    }
    ImGui::Text("operator new per frame %.0f (main thread %.0f), %.2f MB/frame | live allocations %llu",
                platform_.newPerFrame, platform_.mainNewPerFrame, Mb(static_cast<std::uint64_t>(platform_.newBytesPerFrame)),
                static_cast<unsigned long long>(platform_.liveAllocations));
    ImGui::TextDisabled("Private also counts the VRAM through its write-combined mappings (M1): CPU memory = mimalloc + NT heaps.");

    if (platform_.haveVram)
    {
        ImGui::SeparatorText("Adapter (this process)");
        const float used = platform_.vramBudgetBytes > 0
            ? static_cast<float>(static_cast<double>(platform_.vramLocalBytes) / static_cast<double>(platform_.vramBudgetBytes))
            : 0.0f;
        char label[96];
        std::snprintf(label, sizeof(label), "local %.0f / budget %.0f MB", Mb(platform_.vramLocalBytes), Mb(platform_.vramBudgetBytes));
        ImGui::ProgressBar(std::clamp(used, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), label);
        ImGui::Text("non-local %.0f MB", Mb(platform_.vramNonLocalBytes));
    }

    const gpu::TagTotals& all = census_.all;
    ImGui::SeparatorText("GPU census");
    ImGui::Text("local %.0f MB, non-local %.0f MB | %u resources + %u heaps (descriptors ~%.1f MB) | buffer slack %.1f MB",
                Mb(all.bytes[0]), Mb(all.bytes[1]), all.resources, all.heaps, Mb(all.descriptorBytes), Mb(all.bufferSlack));
    if (platform_.haveVram)
    {
        ImGui::Text("untracked local %.0f MB, non-local %.0f MB (driver, DLSS, the ImGui backend)",
                    DeltaMb(platform_.vramLocalBytes, all.bytes[0]), DeltaMb(platform_.vramNonLocalBytes, all.bytes[1]));
    }
    if (ImGui::Button("Snapshot"))
    {
        render::SaveMemorySnapshot(device, "tab");
        haveSnapshot_ = render::FindMemorySnapshot("tab", snapshot_);
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Remember now: the tables then show the change since this moment."); }
    if (haveSnapshot_)
    {
        ImGui::SameLine();
        if (ImGui::Button("Clear snapshot")) { haveSnapshot_ = false; }
    }
    ImGui::SameLine();
    if (ImGui::Button("Report to session log")) { render::RequestMemoryReport(); }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Every GPU object by size, the tag tree and the platform, as memreport: lines."); }
    if (haveSnapshot_)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("change since the snapshot %.0f s ago: private %+.1f MB, gpu %+.1f MB, %+lld resources",
                            now - snapshot_.timeSec, DeltaMb(platform_.privateBytes, snapshot_.platform.privateBytes),
                            DeltaMb(all.bytes[0] + all.bytes[1], snapshot_.census.all.bytes[0] + snapshot_.census.all.bytes[1]),
                            static_cast<long long>(all.resources) - static_cast<long long>(snapshot_.census.all.resources));
    }

    const ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_SizingStretchProp;
    const int tagColumns = haveSnapshot_ ? 8 : 5;
    if (ImGui::BeginTable("memoryTags", tagColumns, tableFlags, ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 16.0f)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Tag", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Local MB");
        ImGui::TableSetupColumn("Non-local MB");
        ImGui::TableSetupColumn("Resources");
        ImGui::TableSetupColumn("Heaps");
        if (haveSnapshot_)
        {
            ImGui::TableSetupColumn("d Local");
            ImGui::TableSetupColumn("d Non-local");
            ImGui::TableSetupColumn("d Res");
        }
        ImGui::TableHeadersRow();
        const auto row = [&](std::uint32_t tag, const gpu::TagTotals& t, const gpu::TagTotals& then, bool hasChildren) -> bool {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen;
            if (!hasChildren) { flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen; }
            const bool open = ImGui::TreeNodeEx(kMemTagNames[tag], flags);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", Mb(t.bytes[0]));
            ImGui::TableNextColumn(); ImGui::Text("%.1f", Mb(t.bytes[1]));
            ImGui::TableNextColumn(); ImGui::Text("%u", t.resources);
            ImGui::TableNextColumn(); ImGui::Text("%u", t.heaps);
            if (haveSnapshot_)
            {
                ImGui::TableNextColumn(); DeltaCell(DeltaMb(t.bytes[0], then.bytes[0]));
                ImGui::TableNextColumn(); DeltaCell(DeltaMb(t.bytes[1], then.bytes[1]));
                ImGui::TableNextColumn();
                const long long dr = static_cast<long long>(t.resources) - static_cast<long long>(then.resources);
                if (dr != 0) { ImGui::Text("%+lld", dr); } else { ImGui::TextDisabled("0"); }
            }
            return open && hasChildren;
        };
        for (std::uint32_t top = 0; top < kMemTagCount; ++top)
        {
            if (kMemTagParents[top] != MemTag::Root) { continue; }
            const gpu::TagTotals sum = Subtree(census_, top);
            const gpu::TagTotals sumThen = haveSnapshot_ ? Subtree(snapshot_.census, top) : gpu::TagTotals{};
            if (sum.resources == 0 && sum.heaps == 0 && sumThen.resources == 0 && sumThen.heaps == 0) { continue; }
            bool hasChildren = false;
            for (std::uint32_t c = 0; c < kMemTagCount; ++c)
            {
                if (static_cast<std::uint32_t>(kMemTagParents[c]) == top &&
                    (census_.tags[c].resources > 0 || census_.tags[c].heaps > 0)) { hasChildren = true; }
            }
            if (row(top, sum, sumThen, hasChildren))
            {
                for (std::uint32_t c = 0; c < kMemTagCount; ++c)
                {
                    if (static_cast<std::uint32_t>(kMemTagParents[c]) != top) { continue; }
                    const gpu::TagTotals& ct = census_.tags[c];
                    const gpu::TagTotals cThen = haveSnapshot_ ? snapshot_.census.tags[c] : gpu::TagTotals{};
                    if (ct.resources == 0 && ct.heaps == 0 && cThen.resources == 0 && cThen.heaps == 0) { continue; }
                    row(c, ct, cThen, false);
                }
                ImGui::TreePop();
            }
        }
        ImGui::EndTable();
    }

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    if (ImGui::InputTextWithHint("##memoryFilter", "filter: object name or tag", filter_, sizeof(filter_)))
    {
        orderDirty_ = true;
    }
    if (orderDirty_) { RebuildOrder(); }
    ImGui::SameLine();
    ImGui::Text("%zu of %zu GPU objects", order_.size(), records_.size());
    if (ImGui::BeginTable("memoryObjects", 5, tableFlags | ImGuiTableFlags_Sortable,
                          ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 20.0f)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::TableSetupColumn("Tag");
        ImGui::TableSetupColumn("Pool");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("MB", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsDirty)
        {
            if (specs->SpecsCount > 0)
            {
                sortColumn_ = specs->Specs[0].ColumnIndex;
                sortAscending_ = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            }
            specs->SpecsDirty = false;
            RebuildOrder();
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(order_.size()));
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const gpu::RecordInfo& r = records_[static_cast<std::size_t>(order_[static_cast<std::size_t>(i)])];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.name.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(MemTagName(r.tag));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.pool == gpu::MemPool::Local ? "local" : "non-local");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.kind);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", Mb(r.bytes));
            }
        }
        ImGui::EndTable();
    }
}
