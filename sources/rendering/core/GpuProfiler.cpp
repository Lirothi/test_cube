// The profiler's GPU half (architecture plan R1.2: moved from core/profiling/Profiler.cpp): timestamp query heaps,
// their readback, the clock calibration and the samples. The class and its scope interface stay in
// core/profiling/Profiler.h; what creates GPU resources lives here, in the renderer's layer.
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerDetail.h"
#include "core/memory/MemScope.h"
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string_view>
#include <algorithm>
#include <cmath>
#include <memory>
#include <limits>
#include "third_party/robin_hood.h"
#include "core/logging/Log.h"
#include "core/threading/ThreadRole.h"
#include "rendering/core/GpuAlloc.h"
#include <Windows.h>

using namespace profiler_detail;

#if PROF_ENABLED

namespace {
const Profiler::ScopeNameKey kGpuFrameKey = Profiler::RegisterTraceLiteral(L"GPU.Frame");
}

#if PROF_GPU_ENABLED
bool Profiler::CaptureGpuTraceCalibration(UINT queueIndex, GpuTraceCalibration& calibration) const {
    if (queueIndex >= kGpuQueueCount) { return false; }
    if (!gpuInitialized_ || !gpuQueue_[queueIndex] || gpuFreq_[queueIndex] == 0 || gpuTraceQpcFreq_ == 0) {
        return false;
    }
    UINT64 gpuTimestamp = 0;
    UINT64 cpuQpc = 0;
    if (FAILED(gpuQueue_[queueIndex]->GetClockCalibration(&gpuTimestamp, &cpuQpc))) {
        return false;
    }
    calibration.gpuTimestamp = gpuTimestamp;
    calibration.cpuQpc = cpuQpc;
    return true;
}

uint64_t Profiler::GpuTimestampToCpuUs(UINT64 gpuTimestamp, const GpuTraceCalibration& calibration,
                                       UINT queueIndex) const {
    if (queueIndex >= kGpuQueueCount || gpuFreq_[queueIndex] == 0 || gpuTraceQpcFreq_ == 0) {
        return 0;
    }

    const double gpuDelta = static_cast<double>(gpuTimestamp) - static_cast<double>(calibration.gpuTimestamp);
    const double cpuQpc =
        static_cast<double>(calibration.cpuQpc) +
        gpuDelta * static_cast<double>(gpuTraceQpcFreq_) / static_cast<double>(gpuFreq_[queueIndex]);
    const double cpuUs =
        static_cast<double>(gpuTraceCpuOriginUs_) +
        (cpuQpc - static_cast<double>(gpuTraceQpcOrigin_)) * 1000000.0 / static_cast<double>(gpuTraceQpcFreq_);
    if (cpuUs <= 0.0) {
        return 0;
    }
    return static_cast<uint64_t>(std::llround(cpuUs));
}

void Profiler::CollectGpuResolvedSamples(std::vector<ScopeSample>& sampleOut,
    std::vector<TraceEvent>* traceOut, uint64_t traceStartUs) {
    if (!gpuInitialized_) { return; }
    UINT64 completed = UINT64_MAX;
    for (UINT q = 0; q < kGpuQueueCount; ++q) {
        if (gpuDrainFence_[q]) { completed = (std::min)(completed, gpuDrainFence_[q]->GetCompletedValue()); }
    }
    std::array<UINT, kGpuReadbackFrameSlots> ready{};
    size_t count = 0;
    for (UINT i = 0; i < kGpuReadbackFrameSlots; ++i) {
        const auto& batch = gpuResolvedBatches_[i];
        if (batch.queryCount && batch.fenceValue != UINT64_MAX && completed >= batch.fenceValue) { ready[count++] = i; }
    }
    if (!count) { return; }
    std::sort(ready.begin(), ready.begin() + count, [this](UINT a, UINT b) {
        return gpuResolvedBatches_[a].fenceValue < gpuResolvedBatches_[b].fenceValue;
    });
    // Durations need frequency only; calibration is solely for captured events. One calibration a capture: GetClockCalibration is good to a microsecond
    // or two, and taken anew per batch it put a GPU-bound frame's start before the previous frame's end (Perfetto: slice_spill_overlapping_complete_event,
    // GPU.Frame N against N + 1, 2 us). The two clocks drift far less than that over a capture's few seconds.
    if (traceOut && traceStartUs) {
        for (UINT q = 0; q < kGpuQueueCount; ++q) {
            if (!gpuTraceCalibrated_[q]) { gpuTraceCalibrated_[q] = CaptureGpuTraceCalibration(q, gpuTraceCalibration_[q]); }
        }
    }
    for (size_t i = 0; i < count; ++i) {
        const UINT slot = ready[i];
        auto& batch = gpuResolvedBatches_[slot];
        const size_t base = static_cast<size_t>(slot) * maxGpuQueries_;
        for (const auto& range : batch.ranges) {
            const UINT q = range.queueIndex;
            if (!range.completed || q >= kGpuQueueCount || !gpuMappedReadback_[q] || !gpuFreq_[q]) { continue; }
            const UINT64 a = gpuMappedReadback_[q][base + range.start];
            const UINT64 b = gpuMappedReadback_[q][base + range.end];
            if (b < a) { continue; }
            sampleOut.push_back({ range.key, static_cast<double>(b - a) * 1000.0 / gpuFreq_[q], range.frameNo });
            if (!traceOut || !gpuTraceCalibrated_[q] || range.frameNo < traceFirstFrame_ || range.frameNo > traceLastFrame_) { continue; }
            const uint64_t start = GpuTimestampToCpuUs(a, gpuTraceCalibration_[q], q);
            const uint64_t end = GpuTimestampToCpuUs(b, gpuTraceCalibration_[q], q);
            if (end <= traceStartUs || end < start) { continue; }
            TraceEvent ev;
            ev.key = range.key;
            if (range.key.name == kGpuFrameKey.name) {
                ev.key = RegisterTraceDynamic(L"GPU.Frame " + std::to_wstring(range.frameNo));
            }
            ev.frameNumber = range.frameNo;
            ev.hasFrameNumber = true;
            ev.tsUs = (std::max)(start, traceStartUs) - traceStartUs;
            ev.durUs = end - (std::max)(start, traceStartUs);
            ev.threadIndex = q == 0 ? kGpuTraceThreadIndex : kGpuTraceThreadIndexCompute;
            ev.category = TraceEvent::Category::Gpu;
            traceOut->push_back(ev);
        }
        batch.ranges.clear(); // retain per-slot capacity
        batch.queryCount = 0;
        batch.fenceValue = 0;
    }
}

void Profiler::InitGpu(ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12CommandQueue* computeQueue, UINT maxQueries) {
    if (!device || !queue || maxQueries < 2) { return; }
    maxGpuQueries_ = maxQueries & ~1u;
    gpuQueue_[0] = queue;
    gpuQueue_[1] = computeQueue;
    D3D12_QUERY_HEAP_DESC queryDesc{};
    queryDesc.Count = maxGpuQueries_;
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource.Width = static_cast<UINT64>(maxGpuQueries_) * sizeof(UINT64) * kGpuReadbackFrameSlots;
    resource.Height = resource.DepthOrArraySize = resource.MipLevels = resource.SampleDesc.Count = 1;
    resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (UINT q = 0; q < kGpuQueueCount; ++q) {
        if (!gpuQueue_[q]) { continue; }
        const D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(resource.Width) };
        if (FAILED(gpu::CreateQueryHeap(device, &queryDesc, MemTag::Profiler,
                q == 0 ? L"Profiler.QueryHeap.Graphics" : L"Profiler.QueryHeap.Compute",
                IID_PPV_ARGS(&gpuQueryHeap_[q]))) ||
            FAILED(gpu::CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &resource,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, MemTag::Profiler,
                q == 0 ? L"Profiler.Readback.Graphics" : L"Profiler.Readback.Compute",
                IID_PPV_ARGS(&gpuReadback_[q]))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpuDrainFence_[q]))) ||
            FAILED(gpuQueue_[q]->GetTimestampFrequency(&gpuFreq_[q])) ||
            FAILED(gpuReadback_[q]->Map(0, &readRange, reinterpret_cast<void**>(&gpuMappedReadback_[q])))) {
            LOG_ERROR(logging::LogCategory::Profiling, "GPU profiler initialization failed on queue {}", q);
            ShutdownGpu();
            return;
        }
    }
    gpuPending_.resize(maxGpuQueries_ / 2);
    for (auto& batch : gpuResolvedBatches_) { batch.ranges.reserve(maxGpuQueries_ / 2); }
    LARGE_INTEGER frequency{}, now{};
    if (QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&now)) {
        gpuTraceQpcFreq_ = frequency.QuadPart;
        gpuTraceQpcOrigin_ = now.QuadPart;
        gpuTraceCpuOriginUs_ = ToMicroseconds(CpuClock::now());
    }
    gpuInitialized_ = true;
}

void Profiler::CollectGpuResults() {
    bool capture = false;
    uint64_t startUs = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        capture = traceCapturing_ || traceDraining_;
        startUs = traceStartUs_;
    }
    std::vector<TraceEvent> events;
    CollectGpuResolvedSamples(gpuFrameSamples_, capture ? &events : nullptr, startUs);
    if (!events.empty()) {
        std::lock_guard<std::mutex> lock(mtx_);
        const size_t room = TraceEventRoom_Locked(events.size());
        traceEvents_.insert(traceEvents_.end(), events.begin(), events.begin() + static_cast<ptrdiff_t>(room));
    }
    // BeginFrame precedes the renderer's fence wait: recheck after readback.
    // Never overwrite an unread ring slot when a queue is more than 8 frames late.
    gpuRecordingReady_ = gpuInitialized_ && frameOpen_ && gpuResolvedBatches_[gpuRecordingReadbackSlot_].queryCount == 0;
    FinishTraceIfReady();
}

void Profiler::BeginGpuFrame(ID3D12GraphicsCommandList* cl) {
    gpuFrameSampleIdx_ = BeginGpuSample(cl, kGpuFrameKey);
}

void Profiler::EndGpuFrame(ID3D12GraphicsCommandList* cl) {
    EndGpuSample(cl, gpuFrameSampleIdx_);
    gpuFrameSampleIdx_ = SIZE_MAX;
}

size_t Profiler::BeginGpuSample(ID3D12GraphicsCommandList* cl, const ScopeNameKey& key) {
    if (!gpuInitialized_ || !gpuRecordingReady_ || !cl || !GetEnabled() || !sampleEpoch_.load(std::memory_order_acquire)) { return SIZE_MAX; }
    const UINT q = cl->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE ? 1u : 0u;
    if (!gpuQueryHeap_[q]) { return SIZE_MAX; }
    const UINT start = nextGpuQuery_.fetch_add(2, std::memory_order_relaxed);
    if (start >= maxGpuQueries_) { return SIZE_MAX; }
    auto& groups = GetThreadSamples().gpuGroups;
    GpuResolveGroup* group = nullptr;
    for (auto& candidate : groups) {
        if (candidate->depth && candidate->cl == cl) { group = candidate.get(); break; }
    }
    if (!group) {
        for (auto& candidate : groups) {
            if (!candidate->depth) { group = candidate.get(); break; }
        }
        if (!group) {
            groups.push_back(std::make_unique<GpuResolveGroup>());
            group = groups.back().get();
            group->starts.reserve(16);
        }
        group->cl = cl;
        group->starts.clear();
    }
    ++group->depth;
    group->starts.push_back(start);
    const size_t idx = start / 2;
    gpuPending_[idx] = { key, start, start + 1, false, frameNo_, q, group };
    cl->EndQuery(gpuQueryHeap_[q].Get(), D3D12_QUERY_TYPE_TIMESTAMP, start);
    return idx;
}

void Profiler::EndGpuSample(ID3D12GraphicsCommandList* cl, size_t idx) {
    if (!gpuInitialized_ || !cl || idx == SIZE_MAX || idx >= gpuPending_.size()) { return; }
    auto& range = gpuPending_[idx];
    if (range.completed) { return; }
    const UINT q = range.queueIndex;
    cl->EndQuery(gpuQueryHeap_[q].Get(), D3D12_QUERY_TYPE_TIMESTAMP, range.end);
    range.completed = true;
    auto& group = *range.group;
    if (--group.depth) { return; }
    // Coalesce only consecutive pairs of completed nested scopes on this list.
    // Other recording threads can leave gaps; those queries must not be resolved.
    for (size_t i = 0; i < group.starts.size();) {
        const UINT first = group.starts[i++];
        UINT count = 2;
        while (i < group.starts.size() && group.starts[i] == first + count) { count += 2; ++i; }
        const UINT64 offset = (static_cast<UINT64>(gpuRecordingReadbackSlot_) * maxGpuQueries_ + first) * sizeof(UINT64);
        cl->ResolveQueryData(gpuQueryHeap_[q].Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, count, gpuReadback_[q].Get(), offset);
    }
}

void Profiler::ShutdownGpu() {
    // The renderer joins both queues before releasing GPU resources. Only
    // shutdown joins file I/O; interactive Stop merely retires completed batches.
    CollectGpuResults();
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (traceCapturing_) {
            traceCapturing_ = false;
            traceRecording_.store(false, std::memory_order_release);
            traceDraining_ = true;
            traceLastFrame_ = frameNo_;
            traceCapturingAtomic_.store(false, std::memory_order_release);
        }
    }
    FinishTraceIfReady();
    StopTraceWriter();
    TaskSystem::Get().Wait(overlayTask_);
    TaskSystem::Get().Release(overlayTask_);
    gpuFrameSampleIdx_ = SIZE_MAX;
    for (UINT q = 0; q < kGpuQueueCount; ++q) {
        if (gpuMappedReadback_[q] && gpuReadback_[q]) {
            const D3D12_RANGE written{ 0, 0 };
            gpuReadback_[q]->Unmap(0, &written);
        }
        gpuMappedReadback_[q] = nullptr;
        gpuQueryHeap_[q].Reset();
        gpuReadback_[q].Reset();
        gpuDrainFence_[q].Reset();
        gpuQueue_[q] = nullptr;
        gpuFreq_[q] = 0;
    }
    for (auto& batch : gpuResolvedBatches_) { batch.ranges.clear(); batch.queryCount = 0; batch.fenceValue = 0; }
    gpuPending_.clear();
    gpuTraceQpcFreq_ = gpuTraceQpcOrigin_ = gpuTraceCpuOriginUs_ = 0;
    gpuDrainFenceValue_ = 1;
    maxGpuQueries_ = gpuRecordingReadbackSlot_ = 0;
    nextGpuQuery_.store(0, std::memory_order_relaxed);
    gpuInitialized_ = gpuRecordingReady_ = false;
}
#endif // PROF_GPU_ENABLED

#endif // PROF_ENABLED
