#include "core/task/TaskSystem.h"

#if !defined(TASKSYSTEM_USE_TBB) && !defined(TASKSYSTEM_USE_LOCKFREE)

#include <new>
#include <utility>

TaskSystem& TaskSystem::Get() {
    static TaskSystem g;
    return g;
}

TaskSystem::~TaskSystem() {
    Stop();
    ClearPools();
}

void TaskSystem::Start(unsigned threadCount) {
    if (threadCount == 0) {
        scheduler_.Initialize();
    } else {
        scheduler_.Initialize(threadCount + 1); // include main thread
    }
}

void TaskSystem::Stop() {
    scheduler_.WaitforAllAndShutdown();
}

struct TaskSystem::TaskWithDeps : enki::ITaskSet {
    std::vector<enki::Dependency> deps;
    TaskWithDeps(uint32_t range, size_t depCount)
        : enki::ITaskSet(range), deps(depCount) {}
};

struct TaskSystem::LambdaTaskSet : TaskWithDeps {
    Task fn;
    LambdaTaskSet(Task&& f, size_t depCount)
        : TaskWithDeps(1, depCount), fn(std::move(f)) {}
    void ExecuteRange(enki::TaskSetPartition, uint32_t) override {
        if (fn) {
            fn();
        }
    }
};

struct TaskSystem::RangeTaskSet : TaskWithDeps {
    std::function<void(std::size_t)> fn;
    RangeTaskSet(std::size_t count, std::function<void(std::size_t)> f, size_t depCount)
        : TaskWithDeps(static_cast<uint32_t>(count), depCount), fn(std::move(f)) {}
    void ExecuteRange(enki::TaskSetPartition range, uint32_t) override {
        for (uint32_t i = range.start; i < range.end; ++i) {
            fn(i);
        }
    }
};

struct TaskSystem::AutoDelete : enki::ICompletable {
    TaskSystem& owner;
    enki::ITaskSet* task;
    AutoDeleteKind kind;
    enki::Dependency dep;

    AutoDelete(TaskSystem& sys, enki::ITaskSet* t, AutoDeleteKind k)
        : owner(sys), task(t), kind(k) {
        SetDependency(dep, task);
    }

    void OnDependenciesComplete(enki::TaskScheduler* pTS, uint32_t thread) override {
        ICompletable::OnDependenciesComplete(pTS, thread);
        switch (kind) {
        case AutoDeleteKind::Lambda:
            owner.RecycleLambdaTask(static_cast<LambdaTaskSet*>(task));
            break;
        case AutoDeleteKind::Range:
            owner.RecycleRangeTask(static_cast<RangeTaskSet*>(task));
            break;
        }
        owner.RecycleAutoDelete(this);
    }
};

TaskSystem::TaskHandle TaskSystem::Submit(Task t) {
    if (!t) { return nullptr; }
    auto* taskPtr = AcquireLambdaTask(std::move(t), 0);
    scheduler_.AddTaskSetToPipe(taskPtr);
    return taskPtr;
}

TaskSystem::TaskHandle TaskSystem::CreateTask(Task t, std::size_t depCount) {
    if (!t) { return nullptr; }
    auto* taskPtr = AcquireLambdaTask(std::move(t), depCount);
    return taskPtr;
}

TaskSystem::TaskHandle TaskSystem::CreateRangeTask(std::size_t jobCount,
                         std::function<void(std::size_t)> fn,
                         std::size_t batchSize,
                         std::size_t depCount) {
    if (jobCount == 0 || !fn) { return nullptr; }
    auto* taskPtr = AcquireRangeTask(jobCount, std::move(fn), batchSize, depCount);
    return taskPtr;
}

void TaskSystem::SetDependencies(TaskHandle handle, const TaskHandle* deps, std::size_t depCount) {
    if (!handle) { return; }
    auto* taskPtr = static_cast<TaskWithDeps*>(handle);
    if (taskPtr->deps.size() < depCount) {
        taskPtr->deps.resize(depCount);
    }
    for (std::size_t i = 0; i < depCount; ++i) {
        TaskHandle dep = deps[i];
        if (dep) { taskPtr->SetDependency(taskPtr->deps[i], dep); }
    }
}

void TaskSystem::Submit(TaskHandle handle) {
    if (handle) {
        scheduler_.AddTaskSetToPipe(static_cast<enki::ITaskSet*>(handle));
    }
}

void TaskSystem::SubmitDetach(Task t) {
    if (!t) { return; }
    auto* taskPtr = AcquireLambdaTask(std::move(t), 0);
    AcquireAutoDelete(taskPtr, AutoDeleteKind::Lambda);
    scheduler_.AddTaskSetToPipe(taskPtr);
}

TaskSystem::TaskHandle TaskSystem::Dispatch(std::size_t jobCount,
                          std::function<void(std::size_t)> fn,
                          std::size_t batchSize) {
    TaskHandle handle = CreateRangeTask(jobCount, std::move(fn), batchSize);
    Submit(handle);
    return handle;
}

void TaskSystem::DispatchTrack(std::size_t jobCount,
                         std::function<void(std::size_t)> fn,
                         std::size_t batchSize) {
    TaskHandle handle = Dispatch(jobCount, std::move(fn), batchSize);
    TrackFrameTask(handle);
}

void TaskSystem::DispatchWait(std::size_t jobCount,
                        std::function<void(std::size_t)> fn,
                        std::size_t batchSize) {
    TaskHandle handle = Dispatch(jobCount, std::move(fn), batchSize);
    Wait(handle);
    Release(handle);
}

void TaskSystem::DispatchDetach(std::size_t jobCount,
                          std::function<void(std::size_t)> fn,
                          std::size_t batchSize) {
    TaskHandle taskHandle = CreateRangeTask(jobCount, std::move(fn), batchSize);
    auto* taskPtr = static_cast<RangeTaskSet*>(taskHandle);
    if (!taskPtr) { return; }
    AcquireAutoDelete(taskPtr, AutoDeleteKind::Range);
    scheduler_.AddTaskSetToPipe(taskPtr);
}

void TaskSystem::Wait(TaskHandle handle) {
    if (handle) {
        scheduler_.WaitforTask(handle);
    }
}

void TaskSystem::Release(TaskHandle& handle) {
    if (handle) {
        if (auto* lambda = dynamic_cast<LambdaTaskSet*>(handle)) {
            RecycleLambdaTask(lambda);
        } else if (auto* range = dynamic_cast<RangeTaskSet*>(handle)) {
            RecycleRangeTask(range);
        } else {
            delete handle;
        }
        handle = nullptr;
    }
}

void TaskSystem::TrackFrameTask(TaskHandle handle) {
    if (!handle) { return; }
    std::lock_guard<std::mutex> lk(trackedFrameMutex_);
    trackedFrameTasks_.push_back(handle);
}

void TaskSystem::WaitForTrackedAsyncTasks() {
    std::vector<TaskHandle> handles;
    {
        std::lock_guard<std::mutex> lk(trackedFrameMutex_);
        if (trackedFrameTasks_.empty()) {
            return;
        }
        handles.swap(trackedFrameTasks_);
    }

    for (auto& h : handles) {
        Wait(h);
    }
    for (auto& h : handles) {
        Release(h);
    }
}

void TaskSystem::WaitForAll() {
    scheduler_.WaitforAll();
}

std::size_t TaskSystem::ThreadIndex() const {
    return scheduler_.GetThreadNum();
}

TaskSystem::LambdaTaskSet* TaskSystem::AcquireLambdaTask(Task&& f, std::size_t depCount) {
    LambdaTaskSet* task = nullptr;
    {
        std::lock_guard<std::mutex> lock(lambdaPoolMutex_);
        if (!lambdaTaskPool_.empty()) {
            task = lambdaTaskPool_.back();
            lambdaTaskPool_.pop_back();
        }
    }

    if (!task) {
        return new LambdaTaskSet(std::move(f), depCount);
    }

    return new (task) LambdaTaskSet(std::move(f), depCount);
}

TaskSystem::RangeTaskSet* TaskSystem::AcquireRangeTask(std::size_t jobCount,
                                                       std::function<void(std::size_t)> fn,
                                                       std::size_t batchSize,
                                                       std::size_t depCount) {
    if (batchSize == 0) {
        batchSize = 1;
    }

    RangeTaskSet* task = nullptr;
    {
        std::lock_guard<std::mutex> lock(rangePoolMutex_);
        if (!rangeTaskPool_.empty()) {
            task = rangeTaskPool_.back();
            rangeTaskPool_.pop_back();
        }
    }

    if (!task) {
        task = new RangeTaskSet(jobCount, std::move(fn), depCount);
    } else {
        task = new (task) RangeTaskSet(jobCount, std::move(fn), depCount);
    }

    task->m_MinRange = static_cast<uint32_t>(batchSize);
    return task;
}

void TaskSystem::RecycleLambdaTask(LambdaTaskSet* task) {
    if (!task) { return; }

    task->~LambdaTaskSet();
    std::lock_guard<std::mutex> lock(lambdaPoolMutex_);
    lambdaTaskPool_.push_back(task);
}

void TaskSystem::RecycleRangeTask(RangeTaskSet* task) {
    if (!task) { return; }

    task->m_MinRange = 1;
    task->~RangeTaskSet();
    std::lock_guard<std::mutex> lock(rangePoolMutex_);
    rangeTaskPool_.push_back(task);
}

TaskSystem::AutoDelete* TaskSystem::AcquireAutoDelete(enki::ITaskSet* task, AutoDeleteKind kind) {
    AutoDelete* autoDelete = nullptr;
    {
        std::lock_guard<std::mutex> lock(autoDeletePoolMutex_);
        if (!autoDeletePool_.empty()) {
            autoDelete = autoDeletePool_.back();
            autoDeletePool_.pop_back();
        }
    }

    if (!autoDelete) {
        return new AutoDelete(*this, task, kind);
    }

    return new (autoDelete) AutoDelete(*this, task, kind);
}

void TaskSystem::RecycleAutoDelete(AutoDelete* autoDelete) {
    if (!autoDelete) { return; }

    autoDelete->~AutoDelete();
    std::lock_guard<std::mutex> lock(autoDeletePoolMutex_);
    autoDeletePool_.push_back(autoDelete);
}

void TaskSystem::ClearPools() {
    {
        std::lock_guard<std::mutex> lock(lambdaPoolMutex_);
        for (auto* task : lambdaTaskPool_) {
            ::operator delete(task);
        }
        lambdaTaskPool_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(rangePoolMutex_);
        for (auto* task : rangeTaskPool_) {
            ::operator delete(task);
        }
        rangeTaskPool_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(autoDeletePoolMutex_);
        for (auto* autoDelete : autoDeletePool_) {
            ::operator delete(autoDelete);
        }
        autoDeletePool_.clear();
    }
}

#endif // !TASKSYSTEM_USE_TBB

