#include "core/task/TaskSystemLockFree.h"

#ifdef TASKSYSTEM_USE_LOCKFREE

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <thread>

namespace {
constexpr std::size_t kInvalidThreadIndex = std::numeric_limits<std::size_t>::max();
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
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
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
    dependents_.reserve(depCapacity);
    completionFuture_ = completionPromise_.get_future().share();
}

void TaskSystem::TaskWithDeps::Prepare(std::size_t depCapacity)
{
    pendingDeps_.store(0, std::memory_order_relaxed);
    if (dependents_.capacity() < depCapacity) {
        dependents_.reserve(depCapacity);
    }
    dependents_.clear();
    completionPromise_ = std::promise<void>();
    completionFuture_ = completionPromise_.get_future().share();
    submitted_.store(false, std::memory_order_relaxed);
    scheduled_.store(false, std::memory_order_relaxed);
    refCount_.store(1, std::memory_order_relaxed);
    nextFree_ = nullptr;
}

void TaskSystem::TaskWithDeps::AddDependent(TaskHandle dependent)
{
    if (!dependent) {
        return;
    }
    dependents_.push_back(dependent);
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

    std::size_t chunkCount = (jobCount_ + batchSize_ - 1) / batchSize_;
    if (chunkCount == 0) {
        return;
    }

    struct RangeTaskSharedState {
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<std::size_t> remaining{0};
    };

    RangeTaskSharedState state;
    state.remaining.store(chunkCount, std::memory_order_relaxed);

    auto processChunk = [this](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            fn_(i);
        }
    };

    auto signalCompletion = [&state]() {
        if (state.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.cv.notify_one();
        }
    };

    for (std::size_t chunk = 1; chunk < chunkCount; ++chunk) {
        std::size_t begin = chunk * batchSize_;
        std::size_t end = std::min(begin + batchSize_, jobCount_);
        owner_.Retain(this);
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
    while (state.remaining.load(std::memory_order_acquire) != 0) {
        if (!owner_.RunInlineTask()) {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.cv.wait(lock, [&]() {
                return state.remaining.load(std::memory_order_acquire) == 0;
            });
        }
    }
}

TaskSystem::LambdaTaskSet* TaskSystem::AcquireLambdaTask(Task& fn, std::size_t depCapacity)
{
    while (true) {
        LambdaTaskSet* head = lambdaPool_.load(std::memory_order_acquire);
        if (!head) {
            break;
        }
        auto* next = static_cast<LambdaTaskSet*>(head->nextFree_);
        if (lambdaPool_.compare_exchange_weak(head,
                                             next,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
            head->Reset(std::move(fn), depCapacity);
            return head;
        }
    }
    return new LambdaTaskSet(*this, std::move(fn), depCapacity);
}

TaskSystem::RangeTaskSet* TaskSystem::AcquireRangeTask(std::size_t jobCount,
                                                       std::function<void(std::size_t)>& fn,
                                                       std::size_t batchSize,
                                                       std::size_t depCapacity)
{
    while (true) {
        RangeTaskSet* head = rangePool_.load(std::memory_order_acquire);
        if (!head) {
            break;
        }
        auto* next = static_cast<RangeTaskSet*>(head->nextFree_);
        if (rangePool_.compare_exchange_weak(head,
                                             next,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
            head->Reset(jobCount, std::move(fn), batchSize, depCapacity);
            return head;
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
    task->completionPromise_ = std::promise<void>();
    task->completionFuture_ = std::shared_future<void>();
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
    LambdaTaskSet* head = lambdaPool_.load(std::memory_order_acquire);
    do {
        task->nextFree_ = head;
    } while (!lambdaPool_.compare_exchange_weak(head,
                                                task,
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
    RangeTaskSet* head = rangePool_.load(std::memory_order_acquire);
    do {
        task->nextFree_ = head;
    } while (!rangePool_.compare_exchange_weak(head,
                                               task,
                                               std::memory_order_release,
                                               std::memory_order_relaxed));
}

void TaskSystem::ClearLambdaPool()
{
    LambdaTaskSet* node = lambdaPool_.exchange(nullptr, std::memory_order_acquire);
    while (node) {
        auto* next = static_cast<LambdaTaskSet*>(node->nextFree_);
        delete node;
        node = next;
    }
}

void TaskSystem::ClearRangePool()
{
    RangeTaskSet* node = rangePool_.exchange(nullptr, std::memory_order_acquire);
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

    activeTasks_.fetch_add(1, std::memory_order_acq_rel);
    try {
        task->Execute();
        task->completionPromise_.set_value();
    } catch (...) {
        try {
            task->completionPromise_.set_exception(std::current_exception());
        } catch (...) {
        }
    }

    task->NotifyDependents();
    FinishTask(task);
}

void TaskSystem::FinishTask(TaskWithDeps* task)
{
    outstandingTasks_.fetch_sub(1, std::memory_order_acq_rel);
    activeTasks_.fetch_sub(1, std::memory_order_acq_rel);
    waitCv_.notify_all();
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

    while (!queue_->push(task)) {
        std::this_thread::yield();
    }
    availableTasks_.fetch_add(1, std::memory_order_release);
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
            while (!queue_->push(nullptr)) {
                std::this_thread::yield();
            }
            availableTasks_.fetch_add(1, std::memory_order_release);
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

void TaskSystem::SetDependencies(TaskHandle handle, const std::vector<TaskHandle>& deps)
{
    if (!handle) {
        return;
    }

    bool registered = false;
    for (auto* dep : deps) {
        if (!dep) {
            continue;
        }
        handle->IncrementDependency();
        dep->AddDependent(handle);
        registered = true;
    }

    if (registered && !handle->submitted_.load(std::memory_order_acquire)) {
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
    handle->completionFuture_.wait();
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
        return outstandingTasks_.load(std::memory_order_acquire) == 0 &&
               activeTasks_.load(std::memory_order_acquire) == 0;
    });
}

std::size_t TaskSystem::ThreadIndex() const
{
    return workerIndex_;
}

#endif // TASKSYSTEM_USE_LOCKFREE

