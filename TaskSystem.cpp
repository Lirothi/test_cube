#include "TaskSystem.h"

TaskSystem& TaskSystem::Get() {
    static TaskSystem g;
    return g;
}

TaskSystem::~TaskSystem() {
    Stop();
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

namespace {
struct TaskWithDeps : enki::ITaskSet {
    std::vector<enki::Dependency> deps;
    TaskWithDeps(uint32_t range, size_t depCount)
        : enki::ITaskSet(range), deps(depCount) {}
};

struct LambdaTaskSet : TaskWithDeps {
    TaskSystem::Task fn;
    LambdaTaskSet(TaskSystem::Task&& f, size_t depCount)
        : TaskWithDeps(1, depCount), fn(std::move(f)) {}
    void ExecuteRange(enki::TaskSetPartition, uint32_t) override {
        if (fn) {
            fn();
        }
    }
};

struct RangeTaskSet : TaskWithDeps {
    std::function<void(std::size_t)> fn;
    RangeTaskSet(std::size_t count, std::function<void(std::size_t)> f, size_t depCount)
        : TaskWithDeps(static_cast<uint32_t>(count), depCount), fn(std::move(f)) {}
    void ExecuteRange(enki::TaskSetPartition range, uint32_t) override {
        for (uint32_t i = range.start; i < range.end; ++i) {
            fn(i);
        }
    }
};

// Helper which deletes the associated task once it completes.
struct AutoDelete : enki::ICompletable {
    enki::ITaskSet* task;
    enki::Dependency dep;
    explicit AutoDelete(enki::ITaskSet* t) : task(t) {
        SetDependency(dep, task);
    }
    void OnDependenciesComplete(enki::TaskScheduler* pTS, uint32_t thread) override {
        ICompletable::OnDependenciesComplete(pTS, thread);
        delete task;
        delete this;
    }
};
}

TaskSystem::TaskHandle TaskSystem::Submit(Task t) {
    if (!t) { return nullptr; }
    auto* taskPtr = new LambdaTaskSet(std::move(t), 0);
    scheduler_.AddTaskSetToPipe(taskPtr);
    return taskPtr;
}

TaskSystem::TaskHandle TaskSystem::CreateTask(Task t, std::size_t depCount) {
    if (!t) { return nullptr; }
    auto* taskPtr = new LambdaTaskSet(std::move(t), depCount);
    return taskPtr;
}

TaskSystem::TaskHandle TaskSystem::CreateRangeTask(std::size_t jobCount,
                         std::function<void(std::size_t)> fn,
                         std::size_t batchSize,
                         std::size_t depCount) {
    if (jobCount == 0 || !fn) { return nullptr; }
    if (batchSize == 0) { batchSize = 1; }
    auto* taskPtr = new RangeTaskSet(jobCount, std::move(fn), depCount);
    taskPtr->m_MinRange = static_cast<uint32_t>(batchSize);
    return taskPtr;
}

void TaskSystem::SetDependencies(TaskHandle handle, const std::vector<TaskHandle>& deps) {
    if (!handle) { return; }
    auto* taskPtr = static_cast<TaskWithDeps*>(handle);
    if (taskPtr->deps.size() < deps.size()) {
        taskPtr->deps.resize(deps.size());
    }
    size_t i = 0;
    for (auto* d : deps) {
        if (d) { taskPtr->SetDependency(taskPtr->deps[i], d); }
        ++i;
    }
}

void TaskSystem::Submit(TaskHandle handle) {
    if (handle && handle->GetIsComplete()) {
        scheduler_.AddTaskSetToPipe(static_cast<enki::ITaskSet*>(handle));
    }
}

void TaskSystem::SubmitDetach(Task t) {
    if (!t) { return; }
    auto* taskPtr = new LambdaTaskSet(std::move(t), 0);
    new AutoDelete(taskPtr);
    scheduler_.AddTaskSetToPipe(taskPtr);
}

TaskSystem::TaskHandle TaskSystem::Dispatch(std::size_t jobCount,
                          std::function<void(std::size_t)> fn,
                          std::size_t batchSize) {
    TaskHandle handle = CreateRangeTask(jobCount, std::move(fn), batchSize);
    Submit(handle);
    return handle;
}

void TaskSystem::DispatchDetach(std::size_t jobCount,
                          std::function<void(std::size_t)> fn,
                          std::size_t batchSize) {
    auto* taskPtr = static_cast<enki::ITaskSet*>(CreateRangeTask(jobCount, std::move(fn), batchSize));
    if (!taskPtr) { return; }
    new AutoDelete(taskPtr);
    scheduler_.AddTaskSetToPipe(taskPtr);
}

void TaskSystem::Wait(TaskHandle handle) {
    if (handle) {
        scheduler_.WaitforTask(handle);
        delete handle;
    }
}

void TaskSystem::WaitForAll() {
    scheduler_.WaitforAll();
}

std::size_t TaskSystem::ThreadIndex() const {
    return scheduler_.GetThreadNum();
}

