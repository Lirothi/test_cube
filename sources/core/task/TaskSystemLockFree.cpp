#include "core/logging/Log.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/task/TaskSystem.h"

#ifdef TASKSYSTEM_USE_LOCKFREE

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <immintrin.h>
#include <limits>
#include <thread>

namespace {
constexpr std::size_t kInvalidThreadIndex = std::numeric_limits<std::size_t>::max();

// Tagged freelist heads: user-mode x64 pointers fit in 48 bits, leaving 16 bits
// for a counter that is bumped on every pop. A node popped and pushed back
// between another thread's head load and its CAS then carries a different tag,
// so the stale CAS fails instead of corrupting the list (classic ABA).
constexpr std::uintptr_t kTagShift = 48;
constexpr std::uintptr_t kPtrMask = (std::uintptr_t(1) << kTagShift) - 1;

template <typename T>
T* TaggedPtr(std::uintptr_t value)
{
    return reinterpret_cast<T*>(value & kPtrMask);
}

std::uintptr_t MakeTagged(const void* ptr, std::uintptr_t tag)
{
    return (reinterpret_cast<std::uintptr_t>(ptr) & kPtrMask) | (tag << kTagShift);
}

std::uintptr_t TagOf(std::uintptr_t value)
{
    return value >> kTagShift;
}
}

thread_local std::size_t TaskSystem::workerIndex_ = kInvalidThreadIndex;

class TaskSystem::LockFreeQueue {
public:
    explicit LockFreeQueue(std::size_t capacity)
        : capacity_(capacity)
        , mask_(capacity - 1)
        , buffer_(capacity)
    {
        for (std::size_t i = 0; i < capacity_; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
            buffer_[i].data = nullptr;
        }
    }

    bool push(TaskWithDeps* value)
    {
        Cell* cell;
        std::size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & mask_];
            std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    cell->data = value;
                    cell->sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    bool pop(TaskWithDeps*& value)
    {
        Cell* cell;
        std::size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & mask_];
            std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    value = cell->data;
                    cell->sequence.store(pos + capacity_, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }

private:
    struct Cell {
        std::atomic<std::size_t> sequence;
        TaskWithDeps* data;
    };

    const std::size_t capacity_;
    const std::size_t mask_;
    std::vector<Cell> buffer_;
    // Consumers hammer head_, producers hammer tail_: keep them on separate
    // cache lines, and off the read-mostly capacity_/mask_/buffer_ line.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

TaskSystem& TaskSystem::Get()
{
    static TaskSystem instance;
    return instance;
}

TaskSystem::~TaskSystem()
{
    Stop();
    ClearLambdaPool();
    ClearRangePool();
}

TaskSystem::TaskWithDeps::TaskWithDeps(TaskSystem& owner, TaskKind kind, std::size_t depCapacity)
    : owner_(owner)
    , kind_(kind)
{
    (void)depCapacity;
}

void TaskSystem::TaskWithDeps::Prepare(std::size_t depCapacity)
{
    pendingDeps_.store(0, std::memory_order_relaxed);
    if (dependents_.capacity() < depCapacity) {
        assert(false && "dependents_.capacity() < depCapacity");
    }
    dependents_.clear();
    completed_.store(0, std::memory_order_relaxed);
    submitted_.store(false, std::memory_order_relaxed);
    scheduled_.store(false, std::memory_order_relaxed);
    refCount_.store(1, std::memory_order_relaxed);
    nextFree_ = nullptr;
}

bool TaskSystem::TaskWithDeps::AddDependent(TaskHandle dependent)
{
    if (!dependent) {
        return true; // nothing to register
    }
    if (dependents_.size() >= dependents_.capacity()) {
        return false; // outbound fan-out exceeds the inline capacity
    }
    dependents_.push_back(dependent);
    return true;
}

void TaskSystem::TaskWithDeps::IncrementDependency()
{
    pendingDeps_.fetch_add(1, std::memory_order_relaxed);
    owner_.Retain(this);
}

void TaskSystem::TaskWithDeps::DependencySatisfied()
{
    if (pendingDeps_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (submitted_.load(std::memory_order_acquire)) {
            owner_.Schedule(this);
        }
    }
    owner_.ReleaseRef(this);
}

void TaskSystem::TaskWithDeps::NotifyDependents()
{
    for (auto* dependent : dependents_) {
        owner_.OnDependencyComplete(dependent);
    }
    dependents_.clear();
}

TaskSystem::LambdaTaskSet::LambdaTaskSet(TaskSystem& owner, Task&& fn, std::size_t depCapacity)
    : TaskWithDeps(owner, TaskKind::Lambda, depCapacity)
    , fn_(std::move(fn))
{
}

void TaskSystem::LambdaTaskSet::Reset(Task&& fn, std::size_t depCapacity)
{
    Prepare(depCapacity);
    fn_ = std::move(fn);
}

void TaskSystem::LambdaTaskSet::Execute()
{
    if (fn_) {
        fn_();
    }
}

TaskSystem::RangeTaskSet::RangeTaskSet(TaskSystem& owner,
                                       std::size_t jobCount,
                                       std::function<void(std::size_t)>&& fn,
                                       std::size_t batchSize,
                                       std::size_t depCapacity)
    : TaskWithDeps(owner, TaskKind::Range, depCapacity)
    , fn_(std::move(fn))
    , jobCount_(jobCount)
    , batchSize_(batchSize == 0 ? 1 : batchSize)
{
}

void TaskSystem::RangeTaskSet::Reset(std::size_t jobCount,
                                     std::function<void(std::size_t)>&& fn,
                                     std::size_t batchSize,
                                     std::size_t depCapacity)
{
    Prepare(depCapacity);
    fn_ = std::move(fn);
    jobCount_ = jobCount;
    batchSize_ = batchSize == 0 ? 1 : batchSize;
    pendingChunks_.store(0, std::memory_order_relaxed);
}

void TaskSystem::RangeTaskSet::Execute()
{
    if (!fn_ || jobCount_ == 0) {
        return;
    }

    std::size_t chunkCount = (jobCount_ + batchSize_ - 1) / batchSize_;
    if (chunkCount == 0) {
        return;
    }

    pendingChunks_.store(chunkCount, std::memory_order_relaxed);

    auto processChunk = [this](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            fn_(i);
        }
    };

    auto signalCompletion = [this]() {
        if (pendingChunks_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(chunkMutex_);
            chunkCv_.notify_one();
        }
    };

    for (std::size_t chunk = 1; chunk < chunkCount; ++chunk) {
        std::size_t begin = chunk * batchSize_;
        std::size_t end = std::min(begin + batchSize_, jobCount_);
        owner_.Retain(this); // ensure the range task outlives detached chunk work
        owner_.SubmitDetach([this, begin, end, signalCompletion]() {
            for (std::size_t i = begin; i < end; ++i) {
                fn_(i);
            }
            signalCompletion();
            owner_.ReleaseRef(this);
        });
    }

    std::size_t firstEnd = std::min(batchSize_, jobCount_);
    processChunk(0, firstEnd);
    signalCompletion();
    while (pendingChunks_.load(std::memory_order_acquire) != 0) {
        if (!owner_.RunInlineTask()) {
            std::unique_lock<std::mutex> lock(chunkMutex_);
            chunkCv_.wait(lock, [&]() {
                return pendingChunks_.load(std::memory_order_acquire) == 0;
            });
        }
    }
}

TaskSystem::LambdaTaskSet* TaskSystem::AcquireLambdaTask(Task& fn, std::size_t depCapacity)
{
    std::uintptr_t head = lambdaPool_.load(std::memory_order_acquire);
    while (true) {
        auto* node = TaggedPtr<LambdaTaskSet>(head);
        if (!node) {
            break;
        }
        auto* next = static_cast<LambdaTaskSet*>(node->nextFree_);
        const std::uintptr_t desired = MakeTagged(next, TagOf(head) + 1);
        if (lambdaPool_.compare_exchange_weak(head,
                                             desired,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            node->Reset(std::move(fn), depCapacity);
            return node;
        }
    }
    return new LambdaTaskSet(*this, std::move(fn), depCapacity);
}

TaskSystem::RangeTaskSet* TaskSystem::AcquireRangeTask(std::size_t jobCount,
                                                       std::function<void(std::size_t)>& fn,
                                                       std::size_t batchSize,
                                                       std::size_t depCapacity)
{
    std::uintptr_t head = rangePool_.load(std::memory_order_acquire);
    while (true) {
        auto* node = TaggedPtr<RangeTaskSet>(head);
        if (!node) {
            break;
        }
        auto* next = static_cast<RangeTaskSet*>(node->nextFree_);
        const std::uintptr_t desired = MakeTagged(next, TagOf(head) + 1);
        if (rangePool_.compare_exchange_weak(head,
                                             desired,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            node->Reset(jobCount, std::move(fn), batchSize, depCapacity);
            return node;
        }
    }
    return new RangeTaskSet(*this, jobCount, std::move(fn), batchSize, depCapacity);
}

void TaskSystem::RecycleTask(TaskWithDeps* task)
{
    if (!task) {
        return;
    }

    task->dependents_.clear();
    task->pendingDeps_.store(0, std::memory_order_relaxed);
    task->submitted_.store(false, std::memory_order_relaxed);
    task->scheduled_.store(false, std::memory_order_relaxed);
    task->refCount_.store(0, std::memory_order_relaxed);
    task->nextFree_ = nullptr;

    switch (task->kind_) {
    case TaskKind::Lambda:
        RecycleLambdaTask(static_cast<LambdaTaskSet*>(task));
        break;
    case TaskKind::Range:
        RecycleRangeTask(static_cast<RangeTaskSet*>(task));
        break;
    }
}

void TaskSystem::RecycleLambdaTask(LambdaTaskSet* task)
{
    if (!task) {
        return;
    }

    task->fn_ = Task();
    std::uintptr_t head = lambdaPool_.load(std::memory_order_relaxed);
    std::uintptr_t desired;
    do {
        task->nextFree_ = TaggedPtr<LambdaTaskSet>(head);
        desired = MakeTagged(task, TagOf(head));
    } while (!lambdaPool_.compare_exchange_weak(head,
                                                desired,
                                                std::memory_order_release,
                                                std::memory_order_relaxed));
}

void TaskSystem::RecycleRangeTask(RangeTaskSet* task)
{
    if (!task) {
        return;
    }

    task->fn_ = nullptr;
    task->jobCount_ = 0;
    task->batchSize_ = 1;
    std::uintptr_t head = rangePool_.load(std::memory_order_relaxed);
    std::uintptr_t desired;
    do {
        task->nextFree_ = TaggedPtr<RangeTaskSet>(head);
        desired = MakeTagged(task, TagOf(head));
    } while (!rangePool_.compare_exchange_weak(head,
                                               desired,
                                               std::memory_order_release,
                                               std::memory_order_relaxed));
}

void TaskSystem::ClearLambdaPool()
{
    auto* node = TaggedPtr<LambdaTaskSet>(lambdaPool_.exchange(0, std::memory_order_acquire));
    while (node) {
        auto* next = static_cast<LambdaTaskSet*>(node->nextFree_);
        delete node;
        node = next;
    }
}

void TaskSystem::ClearRangePool()
{
    auto* node = TaggedPtr<RangeTaskSet>(rangePool_.exchange(0, std::memory_order_acquire));
    while (node) {
        auto* next = static_cast<RangeTaskSet*>(node->nextFree_);
        delete node;
        node = next;
    }
}

void TaskSystem::Retain(TaskWithDeps* task)
{
    if (task) {
        task->refCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

void TaskSystem::ReleaseRef(TaskWithDeps* task)
{
    if (!task) {
        return;
    }
    if (task->refCount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        RecycleTask(task);
    }
}

void TaskSystem::RunTask(TaskWithDeps* task)
{
    if (!task) {
        return;
    }

    try {
        task->Execute();
    } catch (...) {
        // Swallowed by design: the previous promise-based path stored the
        // exception but no caller ever rethrew it (Wait never called get()).
    }

    task->completed_.store(1, std::memory_order_release);
    task->completed_.notify_all();

    task->NotifyDependents();
    FinishTask(task);
}

void TaskSystem::FinishTask(TaskWithDeps* task)
{
    if (outstandingTasks_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Last outstanding task: wake WaitForAll. The lock closes the race where
        // a waiter checks the predicate, we decrement + notify, and only then the
        // waiter goes to sleep — missing the notification forever.
        std::lock_guard<std::mutex> lock(waitMutex_);
        waitCv_.notify_all();
    }
    ReleaseRef(task);
}

void TaskSystem::Schedule(TaskWithDeps* task)
{
    if (!task) {
        return;
    }

    bool expected = false;
    if (!task->scheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    Retain(task);
    if (!running_.load(std::memory_order_acquire) || !queue_) {
        RunTask(task);
        return;
    }

    // Count before publishing: a consumer can pop the task the instant push
    // completes, and decrementing before our increment would wrap the unsigned
    // counter. Counting first keeps it >= the real item count at all times; the
    // worst case is a brief spurious worker wake during the increment->push gap.
    availableTasks_.fetch_add(1, std::memory_order_release);
    while (!queue_->push(task)) {
        std::this_thread::yield();
    }
    availableTasks_.notify_one();
}

void TaskSystem::OnDependencyComplete(TaskWithDeps* dependent)
{
    if (!dependent) {
        return;
    }
    dependent->DependencySatisfied();
}

void TaskSystem::WorkerLoop(std::size_t index)
{
    workerIndex_ = index;
    {
        // Name for the session log's [tid=.../Name] column and the debugger. Once per thread
        // start; does not touch scheduling.
        char name[32];
        std::snprintf(name, sizeof(name), "Worker%zu", index);
        logging::SetCurrentThreadName(name);
    }
    while (true) {
        TaskWithDeps* task = nullptr;
        if (queue_ && queue_->pop(task)) {
            availableTasks_.fetch_sub(1, std::memory_order_acq_rel);
            if (!task) {
                break;
            }
            RunTask(task);
        } else if (shutdown_.load(std::memory_order_acquire)) {
            break;
        } else {
            WaitForWork();
        }
    }
    workerIndex_ = kInvalidThreadIndex;
}

void TaskSystem::WaitForWork()
{
    while (!shutdown_.load(std::memory_order_acquire)) {
        std::size_t expected = availableTasks_.load(std::memory_order_acquire);
        if (expected != 0) {
            return;
        }
        availableTasks_.wait(expected, std::memory_order_acquire);
    }
}

bool TaskSystem::RunInlineTask()
{
    if (!queue_) {
        return false;
    }

    TaskWithDeps* task = nullptr;
    if (!queue_->pop(task)) {
        return false;
    }

    availableTasks_.fetch_sub(1, std::memory_order_acq_rel);
    if (!task) {
        return true;
    }

    RunTask(task);
    return true;
}

std::size_t TaskSystem::NextPowerOfTwo(std::size_t value) const
{
    if (value < 2) {
        return 2;
    }
    --value;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    if constexpr (sizeof(std::size_t) >= 8) {
        value |= value >> 32;
    }
    return value + 1;
}

void TaskSystem::Start(unsigned threadCount)
{
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    if (threadCount == 0) {
        threadCount = std::max(1u, std::thread::hardware_concurrency());
    }

    std::size_t capacity = NextPowerOfTwo(static_cast<std::size_t>(threadCount) * 256);
    queue_ = std::make_unique<LockFreeQueue>(capacity);
    shutdown_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    workerCount_.store(threadCount, std::memory_order_release);
    availableTasks_.store(0, std::memory_order_release);

    workers_.reserve(threadCount);
    for (std::size_t i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this, i]() { WorkerLoop(i); });
    }
}

void TaskSystem::Stop()
{
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    WaitForAll();
    shutdown_.store(true, std::memory_order_release);
    availableTasks_.notify_all();

    if (queue_) {
        for (std::size_t i = 0; i < workers_.size(); ++i) {
            availableTasks_.fetch_add(1, std::memory_order_release);
            while (!queue_->push(nullptr)) {
                std::this_thread::yield();
            }
            availableTasks_.notify_one();
        }
    }

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    queue_.reset();
    shutdown_.store(false, std::memory_order_release);
    workerCount_.store(0, std::memory_order_release);
}

TaskSystem::TaskHandle TaskSystem::Submit(Task t)
{
    TaskHandle handle = CreateTask(std::move(t));
    if (!handle) {
        return nullptr;
    }
    Submit(handle);
    return handle;
}

TaskSystem::TaskHandle TaskSystem::CreateTask(Task t, std::size_t depCount)
{
    if (!t) {
        return nullptr;
    }
    return AcquireLambdaTask(t, depCount);
}

TaskSystem::TaskHandle TaskSystem::CreateRangeTask(std::size_t jobCount,
                                                   std::function<void(std::size_t)> fn,
                                                   std::size_t batchSize,
                                                   std::size_t depCount)
{
    if (jobCount == 0 || !fn) {
        return nullptr;
    }
    return AcquireRangeTask(jobCount, fn, batchSize, depCount);
}

// Registers dependencies and — side effect — auto-submits the handle once any
// dependency is registered. This is why RenderGraph only explicitly submits root
// passes: every pass with mt-deps is already submitted here, and gets scheduled
// when its last dependency completes.
//
// Contract (asserted below): dependency setup is a single-threaded phase that
// happens before anything can execute. The target must not have been submitted
// yet, and no dependency may already be scheduled/executing — dependents_ is
// unsynchronized and NotifyDependents iterates it on completion, so registering
// on a running dependency is a data race.
void TaskSystem::SetDependencies(TaskHandle handle, const TaskHandle* deps, std::size_t depCount)
{
    if (!handle) {
        return;
    }

    assert(!handle->submitted_.load(std::memory_order_acquire) &&
           "SetDependencies must run before the target task is submitted");

    bool registered = false;
    for (std::size_t i = 0; i < depCount; ++i) {
        TaskHandle dep = deps[i];
        if (!dep) {
            continue;
        }
        // scheduled_ (not submitted_): a dep with its own dependencies is legally
        // auto-submitted by its own SetDependencies call; the race only exists
        // once it can actually run.
        assert(!dep->scheduled_.load(std::memory_order_acquire) &&
               "Cannot add a dependent to a task that is already scheduled");
        handle->IncrementDependency();
        if (!dep->AddDependent(handle)) {
            // Fan-out overflow is fatal in every build config: the alternatives
            // are an out-of-bounds write (raw push) or a permanent hang of the
            // dependent (silent drop — pendingDeps_ already counts this edge).
            assert(false && "Task dependent fan-out exceeds dependents_ capacity");
            std::abort();
        }
        registered = true;
    }

    if (registered) {
        Submit(handle);
    }
}

void TaskSystem::Submit(TaskHandle handle)
{
    if (!handle) {
        return;
    }

    bool expected = false;
    if (!handle->submitted_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    outstandingTasks_.fetch_add(1, std::memory_order_acq_rel);
    if (handle->pendingDeps_.load(std::memory_order_acquire) == 0) {
        Schedule(handle);
    }
}

TaskSystem::TaskHandle TaskSystem::Dispatch(std::size_t jobCount,
                                            std::function<void(std::size_t)> fn,
                                            std::size_t batchSize)
{
    TaskHandle handle = CreateRangeTask(jobCount, std::move(fn), batchSize);
    Submit(handle);
    return handle;
}

void TaskSystem::DispatchTrack(std::size_t jobCount,
                               std::function<void(std::size_t)> fn,
                               std::size_t batchSize)
{
    TaskHandle handle = Dispatch(jobCount, std::move(fn), batchSize);
    TrackFrameTask(handle);
}

void TaskSystem::DispatchWait(std::size_t jobCount,
                              std::function<void(std::size_t)> fn,
                              std::size_t batchSize)
{
    TaskHandle handle = Dispatch(jobCount, std::move(fn), batchSize);
    Wait(handle);
    Release(handle);
}

void TaskSystem::SubmitDetach(Task t)
{
    TaskHandle handle = Submit(std::move(t));
    Release(handle);
}

void TaskSystem::DispatchDetach(std::size_t jobCount,
                                std::function<void(std::size_t)> fn,
                                std::size_t batchSize)
{
    TaskHandle handle = Dispatch(jobCount, std::move(fn), batchSize);
    Release(handle);
}

void TaskSystem::Wait(TaskHandle handle)
{
    //CPU_SCOPE(ProfilerScopes::kService2);
    if (!handle) {
        return;
    }

    if (workerIndex_ == kInvalidThreadIndex) {
        // Non-worker (main thread): help drain the queue while the task is
        // pending; once there is no inline work left, block on the completion
        // flag (futex-backed, woken by RunTask's notify_all). Workers handle
        // anything queued after we block.
        while (handle->completed_.load(std::memory_order_acquire) == 0) {
            if (!RunInlineTask()) {
                handle->completed_.wait(0, std::memory_order_acquire);
            }
        }
        return;
    }

    // Worker context (e.g. CSM/spot-shadow passes calling DispatchWait from a
    // render-graph task): never fully block. A worker parked on completed_ stops
    // watching availableTasks_, so work pushed after its last failed pop would be
    // invisible to it — with every worker parked that way, the queue would never
    // drain. Keep helping; between attempts spin briefly, then yield.
    while (handle->completed_.load(std::memory_order_acquire) == 0) {
        if (RunInlineTask()) {
            continue;
        }
        for (int i = 0; i < 64; ++i) {
            if (handle->completed_.load(std::memory_order_acquire) != 0) {
                return;
            }
            _mm_pause();
        }
        std::this_thread::yield();
    }
}

void TaskSystem::Release(TaskHandle& handle)
{
    if (!handle) {
        return;
    }
    TaskHandle task = handle;
    handle = nullptr;
    ReleaseRef(task);
}

void TaskSystem::TrackFrameTask(TaskHandle handle)
{
    if (!handle) {
        return;
    }
    std::lock_guard<std::mutex> lock(trackedFrameMutex_);
    trackedFrameTasks_.push_back(handle);
}

void TaskSystem::WaitForTrackedAsyncTasks()
{
    std::lock_guard<std::mutex> lock(trackedFrameMutex_);
    for (TaskHandle& handle : trackedFrameTasks_) {
        Wait(handle);
        Release(handle);
    }
    trackedFrameTasks_.clear();
}

void TaskSystem::WaitForAll()
{
    std::unique_lock<std::mutex> lock(waitMutex_);
    waitCv_.wait(lock, [&]() {
        return outstandingTasks_.load(std::memory_order_acquire) == 0;
    });
}

std::size_t TaskSystem::ThreadIndex() const
{
    return workerIndex_;
}

#endif // TASKSYSTEM_USE_LOCKFREE

