#include "core/task/TaskSystem.h"

#ifdef TASKSYSTEM_USE_TBB

#include <exception>
#include <utility>

TaskSystem& TaskSystem::Get()
{
    static TaskSystem instance;
    return instance;
}

TaskSystem::TaskWithDeps::TaskWithDeps(TaskSystem& owner, std::size_t depCapacity)
    : owner_(owner), dependents_()
{
    dependents_.reserve(depCapacity);
    completionFuture_ = completionPromise_.get_future().share();
}

void TaskSystem::TaskWithDeps::AddDependent(TaskHandle dependent)
{
    if (!dependent) { return; }
    std::lock_guard<std::mutex> lock(dependentsMutex_);
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
    std::vector<TaskHandle> dependents;
    {
        std::lock_guard<std::mutex> lock(dependentsMutex_);
        dependents.swap(dependents_);
    }

    for (auto* dependent : dependents) {
        owner_.OnDependencyComplete(dependent);
    }
}

TaskSystem::LambdaTaskSet::LambdaTaskSet(TaskSystem& owner, Task&& fn, std::size_t depCapacity)
    : TaskWithDeps(owner, depCapacity), fn_(std::move(fn))
{
}

void TaskSystem::LambdaTaskSet::Execute()
{
    if (fn_) {
        fn_();
    }
}

TaskSystem::RangeTaskSet::RangeTaskSet(TaskSystem& owner,
                                       std::size_t jobCount,
                                       std::function<void(std::size_t)> fn,
                                       std::size_t batchSize,
                                       std::size_t depCapacity)
    : TaskWithDeps(owner, depCapacity)
    , fn_(std::move(fn))
    , jobCount_(jobCount)
    , batchSize_(batchSize == 0 ? 1 : batchSize)
{
}

void TaskSystem::RangeTaskSet::Execute()
{
    if (!fn_ || jobCount_ == 0) { return; }

    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, jobCount_, batchSize_),
                      [&](const tbb::blocked_range<std::size_t>& range) {
                          for (std::size_t i = range.begin(); i != range.end(); ++i) {
                              fn_(i);
                          }
                      });
}

void TaskSystem::Retain(TaskWithDeps* task)
{
    if (task) {
        task->refCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

void TaskSystem::ReleaseRef(TaskWithDeps* task)
{
    if (!task) { return; }
    if (task->refCount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete task;
    }
}

void TaskSystem::Schedule(TaskWithDeps* task)
{
    if (!task) { return; }

    bool expected = false;
    if (!task->scheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    if (!started_.load(std::memory_order_acquire)) {
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
        outstandingTasks_.fetch_sub(1, std::memory_order_acq_rel);
        activeCv_.notify_all();
        return;
    }

    Retain(task); // worker reference

    {
        std::lock_guard<std::mutex> lock(activeMutex_);
        ++activeTasks_;
    }

    arena_.enqueue([this, task]() {
        try {
            task->Execute();
            task->completionPromise_.set_value();
        } catch (...) {
            try {
                task->completionPromise_.set_exception(std::current_exception());
            } catch (...) {
                // set_exception may throw if promise already satisfied
            }
        }

        task->NotifyDependents();
        FinishTask(task);
    });
}

void TaskSystem::OnDependencyComplete(TaskWithDeps* dependent)
{
    if (!dependent) { return; }
    dependent->DependencySatisfied();
}

void TaskSystem::FinishTask(TaskWithDeps* task)
{
    outstandingTasks_.fetch_sub(1, std::memory_order_acq_rel);

    {
        std::lock_guard<std::mutex> lock(activeMutex_);
        if (activeTasks_ > 0) {
            --activeTasks_;
        }
    }

    activeCv_.notify_all();
    ReleaseRef(task); // release worker reference
}

TaskSystem::TaskHandle TaskSystem::Submit(Task t)
{
    if (!t) { return nullptr; }
    auto* task = new LambdaTaskSet(*this, std::move(t), 0);
    Submit(task);
    return task;
}

TaskSystem::TaskHandle TaskSystem::CreateTask(Task t, std::size_t depCount)
{
    if (!t) { return nullptr; }
    return new LambdaTaskSet(*this, std::move(t), depCount);
}

TaskSystem::TaskHandle TaskSystem::CreateRangeTask(std::size_t jobCount,
                                                   std::function<void(std::size_t)> fn,
                                                   std::size_t batchSize,
                                                   std::size_t depCount)
{
    if (jobCount == 0 || !fn) { return nullptr; }
    return new RangeTaskSet(*this, jobCount, std::move(fn), batchSize, depCount);
}

void TaskSystem::SetDependencies(TaskHandle handle, const std::vector<TaskHandle>& deps)
{
    if (!handle) { return; }

    bool registeredDependency = false;
    for (auto* dep : deps) {
        if (!dep) { continue; }
        handle->IncrementDependency();
        dep->AddDependent(handle);
        registeredDependency = true;
    }

    if (registeredDependency && !handle->submitted_.load(std::memory_order_acquire)) {
        Submit(handle);
    }
}

void TaskSystem::Submit(TaskHandle handle)
{
    if (!handle) { return; }

    bool wasSubmitted = handle->submitted_.exchange(true, std::memory_order_acq_rel);
    if (!wasSubmitted) {
        outstandingTasks_.fetch_add(1, std::memory_order_acq_rel);
    }

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
    if (!handle) { return; }
    handle->completionFuture_.wait();
}

void TaskSystem::Release(TaskHandle& handle)
{
    if (!handle) { return; }
    TaskHandle task = handle;
    handle = nullptr;
    ReleaseRef(task);
}

void TaskSystem::TrackFrameTask(TaskHandle handle)
{
    if (!handle) { return; }
    std::lock_guard<std::mutex> lock(trackedFrameMutex_);
    trackedFrameTasks_.push_back(handle);
}

void TaskSystem::WaitForTrackedAsyncTasks()
{
    std::vector<TaskHandle> handles;
    {
        std::lock_guard<std::mutex> lock(trackedFrameMutex_);
        if (trackedFrameTasks_.empty()) {
            return;
        }
        handles.swap(trackedFrameTasks_);
    }

    for (auto& handle : handles) {
        Wait(handle);
    }
    for (auto& handle : handles) {
        Release(handle);
    }
}

void TaskSystem::WaitForAll()
{
    std::unique_lock<std::mutex> lock(activeMutex_);
    activeCv_.wait(lock, [&]() {
        return outstandingTasks_.load(std::memory_order_acquire) == 0 && activeTasks_ == 0;
    });
}

std::size_t TaskSystem::ThreadIndex() const
{
    int index = tbb::this_task_arena::current_thread_index();
    if (index < 0) {
        return 0;
    }
    return static_cast<std::size_t>(index);
}

void TaskSystem::Start(unsigned threadCount)
{
    if (started_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    if (threadCount == 0) {
        arena_.initialize();
    } else {
        std::size_t totalThreads = threadCount + 1; // include main thread
        globalControl_ = std::make_unique<tbb::global_control>(
            tbb::global_control::max_allowed_parallelism,
            static_cast<int>(totalThreads));
        arena_.initialize(static_cast<int>(totalThreads));
    }

    outstandingTasks_.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(activeMutex_);
        activeTasks_ = 0;
    }
}

void TaskSystem::Stop()
{
    if (!started_.load(std::memory_order_acquire)) {
        return;
    }

    WaitForAll();
    arena_.terminate();
    globalControl_.reset();
    started_.store(false, std::memory_order_release);
}

#endif // TASKSYSTEM_USE_TBB

