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
#include <exception>
#include <immintrin.h>
#include <limits>
#include <thread>

namespace {
constexpr std::size_t kInvalidThreadIndex = std::numeric_limits<std::size_t>::max();

constexpr std::size_t kPoolCapacity = 4096;
}

thread_local std::size_t TaskSystem::workerIndex_ = kInvalidThreadIndex;
thread_local TaskSystem::TaskWithDeps* TaskSystem::currentTask_ = nullptr;

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

TaskSystem::TaskSystem()
    : lambdaPool_(std::make_unique<LockFreeQueue>(kPoolCapacity))
    , rangePool_(std::make_unique<LockFreeQueue>(kPoolCapacity))
{
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
    prerequisites_.reserve(depCapacity);
}

void TaskSystem::TaskWithDeps::Prepare(std::size_t depCapacity)
{
    pendingDeps_.store(0, std::memory_order_relaxed);
    prerequisites_.reserve(depCapacity);
    dependents_.clear();
    completed_.store(0, std::memory_order_relaxed);
    submitted_.store(false, std::memory_order_relaxed);
    scheduled_.store(false, std::memory_order_relaxed);
    refCount_.store(1, std::memory_order_relaxed);
    ready_.store(false, std::memory_order_relaxed);
    executionClaimed_.store(false, std::memory_order_relaxed);
    nextReady_ = nullptr;
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
}

void TaskSystem::RangeTaskSet::Execute()
{
    if (!fn_ || jobCount_ == 0) {
        return;
    }

    // Avoid overflow in jobCount + batchSize - 1. Each child is a normal task:
    // RunTask catches its exception and completes it even when its body throws.
    const std::size_t chunkCount = 1 + (jobCount_ - 1) / batchSize_;
    std::vector<TaskHandle> chunks;
    chunks.reserve(chunkCount - 1);
    std::exception_ptr failure;
    try {
        for (std::size_t chunk = 1; chunk < chunkCount; ++chunk) {
            const std::size_t begin = chunk * batchSize_;
            const std::size_t end = begin + std::min(batchSize_, jobCount_ - begin);
            auto child = owner_.CreateTask([this, begin, end]() {
                for (std::size_t i = begin; i < end; ++i) {
                    fn_(i);
                }
            });
            chunks.push_back(child); // reserved above; ownership cannot be lost here
            owner_.Submit(child);
        }
        for (std::size_t i = 0; i < std::min(batchSize_, jobCount_); ++i) {
            fn_(i);
        }
    } catch (...) {
        failure = std::current_exception();
    }

    // Join even if the first chunk or child allocation threw. This Execute's
    // reference keeps this and fn_ alive; completion never precedes child work.
    for (auto& child : chunks) {
        owner_.Wait(child);
        owner_.Release(child);
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TaskSystem::LambdaTaskSet* TaskSystem::AcquireLambdaTask(Task& fn, std::size_t depCapacity)
{
    TaskWithDeps* cached = nullptr;
    if (lambdaPool_->pop(cached)) {
        auto* node = static_cast<LambdaTaskSet*>(cached);
        try {
            node->Reset(std::move(fn), depCapacity);
        } catch (...) {
            delete node;
            throw;
        }
        return node;
    }
    return new LambdaTaskSet(*this, std::move(fn), depCapacity);
}

TaskSystem::RangeTaskSet* TaskSystem::AcquireRangeTask(std::size_t jobCount,
                                                       std::function<void(std::size_t)>& fn,
                                                       std::size_t batchSize,
                                                       std::size_t depCapacity)
{
    TaskWithDeps* cached = nullptr;
    if (rangePool_->pop(cached)) {
        auto* node = static_cast<RangeTaskSet*>(cached);
        try {
            node->Reset(jobCount, std::move(fn), batchSize, depCapacity);
        } catch (...) {
            delete node;
            throw;
        }
        return node;
    }
    return new RangeTaskSet(*this, jobCount, std::move(fn), batchSize, depCapacity);
}

void TaskSystem::RecycleTask(TaskWithDeps* task)
{
    if (!task) {
        return;
    }

    task->dependents_.clear();
    for (auto* prerequisite : task->prerequisites_) {
        ReleaseRef(prerequisite);
    }
    task->prerequisites_.clear();
    task->pendingDeps_.store(0, std::memory_order_relaxed);
    task->submitted_.store(false, std::memory_order_relaxed);
    task->scheduled_.store(false, std::memory_order_relaxed);
    task->refCount_.store(0, std::memory_order_relaxed);
    task->nextReady_ = nullptr;

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
    task->fn_ = Task();
    if (!lambdaPool_->push(task)) {
        delete task;
    }
}

void TaskSystem::RecycleRangeTask(RangeTaskSet* task)
{
    task->fn_ = nullptr;
    if (!rangePool_->push(task)) {
        delete task;
    }
}

void TaskSystem::ClearLambdaPool()
{
    TaskWithDeps* task = nullptr;
    while (lambdaPool_->pop(task)) {
        delete task;
    }
}

void TaskSystem::ClearRangePool()
{
    TaskWithDeps* task = nullptr;
    while (rangePool_->pop(task)) {
        delete task;
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

    auto* previousTask = currentTask_;
    currentTask_ = task;
    try {
        task->Execute();
    } catch (const std::exception& error) {
        // Wait remains non-throwing, as before; failures are session events.
        LOG_ERROR_ONCE_PER_MESSAGE(logging::LogCategory::Task, "Task callback threw: {}", error.what());
    } catch (...) {
        LOG_ERROR_ONCE(logging::LogCategory::Task, "Task callback threw a non-standard exception");
    }
    currentTask_ = previousTask;

    task->completed_.store(1, std::memory_order_release);
    task->completed_.notify_all();

    task->NotifyDependents();
    FinishTask(task);
}

bool TaskSystem::TryRunTask(TaskWithDeps* task)
{
    if (!task->ready_.load(std::memory_order_acquire)) {
        return false;
    }
    bool expected = false;
    if (!task->executionClaimed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    // Execution and the queue entry own separate references: a targeted waiter
    // may finish this task long before a worker removes its queued pointer.
    Retain(task);
    RunTask(task);
    return true;
}

bool TaskSystem::HelpTask(TaskWithDeps* task)
{
    if (task->completed_.load(std::memory_order_acquire) != 0) {
        return false;
    }
    if (TryRunTask(task)) {
        return true;
    }
    if (task->pendingDeps_.load(std::memory_order_acquire) != 0) {
        for (auto* prerequisite : task->prerequisites_) {
            if (HelpTask(prerequisite)) {
                return true;
            }
        }
    }
    return false;
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

    Retain(task); // ready-queue reference, independent of execution
    task->ready_.store(true, std::memory_order_release);
    if (!running_.load(std::memory_order_acquire) || !queue_) {
        TryRunTask(task);
        ReleaseRef(task);
        return;
    }

    if (!queue_->push(task)) {
        // Never execute arbitrary work on the producer's stack and never wait
        // for a consumer: either can deadlock when producers are themselves tasks.
        auto* head = overflow_.load(std::memory_order_relaxed);
        do {
            task->nextReady_ = head;
        } while (!overflow_.compare_exchange_weak(head, task,
            std::memory_order_release, std::memory_order_relaxed));
    }
    workEpoch_.fetch_add(1, std::memory_order_release);
    workEpoch_.notify_one();
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
    char name[32];
    std::snprintf(name, sizeof(name), "Worker%zu", index);
    logging::SetCurrentThreadName(name);
    while (true) {
        // Snapshot BEFORE looking for work: a publication/shutdown between the
        // last failed pop and wait changes the value and cannot be missed.
        const auto epoch = workEpoch_.load(std::memory_order_acquire);
        TaskWithDeps* task = nullptr;
        if (queue_->pop(task)) {
            TryRunTask(task);
            ReleaseRef(task);
        } else if (auto* batch = overflow_.exchange(nullptr, std::memory_order_acquire)) {
            // The whole list is exclusively ours. Read each link while its
            // queue reference is held; neither helpers nor recycling can change it.
            while (batch) {
                auto* next = batch->nextReady_;
                TryRunTask(batch);
                ReleaseRef(batch);
                batch = next;
            }
        } else if (shutdown_.load(std::memory_order_acquire)) {
            break;
        } else {
            workEpoch_.wait(epoch, std::memory_order_acquire);
        }
    }
    workerIndex_ = kInvalidThreadIndex;
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

    workers_.reserve(threadCount);
    for (std::size_t i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this, i]() { WorkerLoop(i); });
    }
}

void TaskSystem::Stop()
{
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    // Keep scheduling normally while draining: tasks may still spawn children.
    WaitForAll();
    running_.store(false, std::memory_order_release);
    shutdown_.store(true, std::memory_order_release);
    workEpoch_.fetch_add(1, std::memory_order_release);
    workEpoch_.notify_all();

    // Workers drain even already-completed queue entries and the overflow list
    // before exiting, releasing their queue references before queue_ is destroyed.

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    queue_.reset();
    shutdown_.store(false, std::memory_order_release);
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

    // Reserve before registering any edges, so allocation failure cannot leave
    // a partially registered graph. Stored prerequisites stay alive for Wait.
    handle->prerequisites_.reserve(depCount);
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
        Retain(dep);
        handle->prerequisites_.push_back(dep);
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
    if (!handle) {
        return;
    }
    while (handle->completed_.load(std::memory_order_acquire) == 0) {
        if (HelpTask(handle)) {
            continue;
        }
        if (!currentTask_) {
            // External waiters can park. A task executing on any thread (also
            // the main thread) keeps checking its target for newly-ready work.
            handle->completed_.wait(0, std::memory_order_acquire);
        } else {
            for (int i = 0; i < 64; ++i) {
                if (handle->completed_.load(std::memory_order_acquire) != 0) {
                    return;
                }
                _mm_pause();
            }
            std::this_thread::yield();
        }
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
    std::vector<TaskHandle> pending;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(trackedFrameMutex_);
            if (trackedFrameTasks_.empty()) {
                // Keep the reusable frame list's capacity after the unlocked join.
                if (pending.capacity() > trackedFrameTasks_.capacity()) {
                    pending.swap(trackedFrameTasks_);
                }
                return;
            }
            pending.swap(trackedFrameTasks_);
        }
        for (auto& handle : pending) {
            Wait(handle);
            Release(handle);
        }
        pending.clear();
        // Tasks we just joined may themselves have registered tracked work.
    }
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

