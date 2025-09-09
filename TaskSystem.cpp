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
struct LambdaTaskSet : enki::ITaskSet {
    TaskSystem::Task fn;
    std::vector<enki::Dependency> deps;
    LambdaTaskSet(TaskSystem::Task&& f, size_t depCount)
        : enki::ITaskSet(1), fn(std::move(f)), deps(depCount) {}
    void ExecuteRange(enki::TaskSetPartition, uint32_t) override {
        if (fn) {
            fn();
        }
    }
};

struct RangeTaskSet : enki::ITaskSet {
    std::function<void(std::size_t)> fn;
    std::vector<enki::Dependency> deps;
    RangeTaskSet(std::size_t count, std::function<void(std::size_t)> f, size_t depCount)
        : enki::ITaskSet(static_cast<uint32_t>(count)), fn(std::move(f)), deps(depCount) {}
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

TaskSystem::TaskHandle TaskSystem::Submit(Task t, std::initializer_list<TaskHandle> deps) {
    if (!t) { return nullptr; }
    auto* taskPtr = new LambdaTaskSet(std::move(t), deps.size());
    size_t i = 0;
    for (auto* d : deps) {
        if (d) { taskPtr->SetDependency(taskPtr->deps[i], d); }
        ++i;
    }
    scheduler_.AddTaskSetToPipe(taskPtr);
    return taskPtr;
}

void TaskSystem::SubmitDetach(Task t, std::initializer_list<TaskHandle> deps) {
    if (!t) { return; }
    auto* taskPtr = new LambdaTaskSet(std::move(t), deps.size());
    size_t i = 0;
    for (auto* d : deps) {
        if (d) { taskPtr->SetDependency(taskPtr->deps[i], d); }
        ++i;
    }
    new AutoDelete(taskPtr);
    scheduler_.AddTaskSetToPipe(taskPtr);
}

TaskSystem::TaskHandle TaskSystem::Dispatch(std::size_t jobCount,
                          std::function<void(std::size_t)> fn,
                          std::size_t batchSize,
                          std::initializer_list<TaskHandle> deps) {
    if (jobCount == 0 || !fn) { return nullptr; }
    if (batchSize == 0) { batchSize = 1; }
    auto* taskPtr = new RangeTaskSet(jobCount, std::move(fn), deps.size());
    taskPtr->m_MinRange = static_cast<uint32_t>(batchSize);
    size_t i = 0;
    for (auto* d : deps) {
        if (d) { taskPtr->SetDependency(taskPtr->deps[i], d); }
        ++i;
    }
    scheduler_.AddTaskSetToPipe(taskPtr);
    return taskPtr;
}

void TaskSystem::DispatchDetach(std::size_t jobCount,
                          std::function<void(std::size_t)> fn,
                          std::size_t batchSize,
                          std::initializer_list<TaskHandle> deps) {
    if (jobCount == 0 || !fn) { return; }
    if (batchSize == 0) { batchSize = 1; }
    auto* taskPtr = new RangeTaskSet(jobCount, std::move(fn), deps.size());
    taskPtr->m_MinRange = static_cast<uint32_t>(batchSize);
    size_t i = 0;
    for (auto* d : deps) {
        if (d) { taskPtr->SetDependency(taskPtr->deps[i], d); }
        ++i;
    }
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

