// The profiler's HUD and its capture key (architecture plan R1.2: moved from core/profiling/ProfilerScopes.cpp, so
// the collector in core includes no text, input or app header).
#include "core/profiling/ProfilerScopes.h"

// HUD and input integration live here so the collector has no renderer/UI dependency.
#if PROF_ENABLED
#include "text/TextManager.h"
#include "input/InputManager.h"
#include "input/InputActions.h"
#include "app/Systems.h"
#include <cwchar>

namespace {
void FormatOverlayRow(Profiler::OverlayRow& row, const std::wstring* name) {
    const double perUse = (row.usages ? (row.avgMs / static_cast<double>(row.usages)) : 0.0);
    wchar_t buf[192];
    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
        L"%-50s  avg:%6.2f  max:%6.2f  p/u:%6.3f  usages:%u",
        name->c_str(), row.avgMs, row.maxMs, perUse, row.usages);
    row.formatted.assign(buf);
}

const Profiler::ScopeNameKey kProfilerEmitOverlayKey = Profiler::RegisterTraceLiteral(L"Profiler::EmitOverlay");
}

void Profiler::EmitOverlay(TextManager* tm, int x, int y, int maxLines) {
    if (!tm || !GetEnabled()) { return; }
    CPU_SCOPE(kProfilerEmitOverlayKey);

    const auto now = CoolClock::now();
    if (std::chrono::duration<double>(now - lastHudUpdate_).count() >= 0.1 ||
        hudRows_.empty() || hudRows_.size() > static_cast<size_t>((std::max)(0, maxLines))) {
        auto cache = [maxLines](const auto& source, auto& dest) {
            const size_t count = (std::min)(source.size(), static_cast<size_t>((std::max)(0, maxLines)));
            dest.resize(count);
            for (size_t i = 0; i < count; ++i) {
                auto& row = dest[i];
                row.avgMs = source[i].avgMs;
                row.maxMs = source[i].maxMs;
                row.usages = source[i].usages;
                row.name = source[i].name;
                FormatOverlayRow(row, &row.name);
            }
        };
        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            cache(overlayRows_, hudRows_);
            cache(gpuOverlayRows_, gpuHudRows_);
        }
        lastHudUpdate_ = now;
    }
    const auto& rows = hudRows_;
    const auto& gpuRows = gpuHudRows_;

    bool captureActive = false;
    bool capturePending = false;
    uint32_t captureRemaining = 0;
    uint32_t captureTotal = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        captureActive = traceCapturing_;
        capturePending = traceCaptureRequested_;
        captureRemaining = traceFramesRemaining_;
        captureTotal = traceRequestFrameCount_;
    }

    // Estimate the region width directly, without measuring individual lines
    // Row format: "%-35s  avg:%6.2f  max:%6.2f  p/u:%6.2f  usages:%u"
    const int namePad = 35;
    const int otherCols = 1 + 28;
    const int lineCols = namePad + otherCols + 16;
    const double charW = 0.60 * 21.0;
    const double needW = charW * (double)lineCols;
    overlayWidthPx_ = overlayWidthPx_ * 0.9 + needW * 0.1;
    const double boxW = std::max(overlayWidthPx_, 480.0);
#if PROF_GPU_ENABLED
    const bool hasGpuRows = !gpuRows.empty();
    double gpuBoxW = 0.0;
    if (hasGpuRows) {
        gpuOverlayWidthPx_ = gpuOverlayWidthPx_ * 0.9 + needW * 0.1;
        gpuBoxW = std::max(gpuOverlayWidthPx_, 480.0);
    }
#endif

    // CPU region
    auto reg = tm->CreateRegion(x, y, TextManager::Align::Left);
    tm->RegionSetPadding(reg, 8, 6);
    tm->RegionSetBackground(reg, float4(0.00f, 0.00f, 0.05f, 0.75f));
    tm->RegionSetFixedWidth(reg, (float)boxW);
    tm->RegionSetAutoMeasure(reg, false);
    tm->RegionSetKerning(reg, false);

    // Header
    tm->AddTextfShadow(reg, 18.0f, float4(1, 1, 0.6f, 0.95f), true,
        L"[CPU profiler] frame=%llu  (max reset: %.1fs, sort every: %.2fs)",
        (unsigned long long)frameNo_, GetMaxCooldownSeconds(), GetOverlayResortIntervalSeconds());

    if (captureActive || capturePending) {
        if (captureActive) {
            const uint32_t recorded = (captureTotal > captureRemaining) ? (captureTotal - captureRemaining) : 0u;
            tm->AddTextfShadow(reg, 16.0f, float4(1.0f, 0.85f, 0.25f, 0.95f), true,
                L"[Trace capture active] recorded:%u remaining:%u  (press F10 to stop)",
                recorded, captureRemaining);
        }
        else {
            tm->AddTextfShadow(reg, 16.0f, float4(0.7f, 0.85f, 1.0f, 0.95f), true,
                L"[Trace capture pending] frames:%u  (press F10 again to cancel)",
                captureTotal);
        }
    }

    tm->AddText(reg, 20.0f, float4(1, 1, 0.6f, 0.95f), L" ");

    // Rows
    int shown = 0;
    const float4 colOdd = { 1, 1, 1, 0.92f };
    const float4 colEven = { 0.5f, 0.5f, 0.5f, 0.92f };
    for (const auto& r : rows) {
        if (shown >= maxLines) { break; }
        tm->AddText(reg, 18.0f, (shown & 1) ? colOdd : colEven, r.formatted, true);
        shown++;
    }

#if PROF_GPU_ENABLED
    if (hasGpuRows) {
        // GPU region to the right of CPU
        const int gpuX = x + (int)boxW + 16;
        auto regGpu = tm->CreateRegion(gpuX, y, TextManager::Align::Left);
        tm->RegionSetPadding(regGpu, 8, 6);
        tm->RegionSetBackground(regGpu, float4(0.00f, 0.05f, 0.00f, 0.75f));
        tm->RegionSetFixedWidth(regGpu, (float)gpuBoxW);
        tm->RegionSetAutoMeasure(regGpu, false);
        tm->RegionSetKerning(regGpu, false);

        tm->AddTextfShadow(regGpu, 18.0f, float4(0.6f, 1, 0.6f, 0.95f), true,
            L"[GPU profiler] frame=%llu  (max reset: %.1fs, sort every: %.2fs)",
            (unsigned long long)frameNo_, GetMaxCooldownSeconds(), GetOverlayResortIntervalSeconds());
        tm->AddText(regGpu, 20.0f, float4(0.6f, 1, 0.6f, 0.95f), L" ");

        int gshown = 0;
        for (const auto& r : gpuRows) {
            if (gshown >= maxLines) { break; }
            tm->AddText(regGpu, 18.0f, (gshown & 1) ? colOdd : colEven, r.formatted, true);
            gshown++;
        }
    }
#endif
}

void Profiler::Tick()
{
#if PROF_ENABLED && !TC_SHIPPING
    auto& input = Systems::GetInput();
    if (input.WasActionPressed(actions::kCaptureTrace)) { // F11; not in a shipped build (§7)
        constexpr uint32_t kTraceFrames = 120;
        RequestTraceCapture(kTraceFrames);
    }
#endif
}

#endif
