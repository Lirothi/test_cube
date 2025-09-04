#include "TaskSystem.h"
#include <algorithm>
#include "Profiler.h"

thread_local std::size_t TaskSystem::tlsIndex_ = static_cast<std::size_t>(-1);

TaskSystem& TaskSystem::Get() {
    static TaskSystem g;
    return g;
}

TaskSystem::~TaskSystem() {
    Stop();
}

void TaskSystem::Start(unsigned threadCount) {
    std::lock_guard<std::mutex> lk(startStopMtx_);
    if (running_) {
        return;
    }

    running_ = true;

    queues_.clear();
    globalQueue_.clear();

    if (threadCount == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0u) {
            hc = 4u;
        }
        threadCount = std::max(1u, hc - 1u);
    }

    workers_.reserve(threadCount);
    queues_.resize(threadCount);
    for (unsigned i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this, i]() {
            tlsIndex_ = i;
            WorkerLoop_(i);
        });
    }
}

void TaskSystem::Stop() {
    {
        std::lock_guard<std::mutex> lk(startStopMtx_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    cvWork_.notify_all();
    inFlight_.notify_all();

    for (auto& th : workers_) {
        if (th.joinable()) {
            th.join();
        }
    }
    workers_.clear();

    for (auto& q : queues_) {
        std::lock_guard<std::mutex> lk(q.m);
        q.q.clear();
    }
    queues_.clear();
    {
        std::lock_guard<std::mutex> lk(globalMtx_);
        globalQueue_.clear();
    }
}

void TaskSystem::Submit(const Task& t) {
    std::size_t idx = ThreadIndex();
    if (!running_) {
        return;
    }

    inFlight_.fetch_add(1, std::memory_order_relaxed);

    if (idx < queues_.size()) {
        auto& q = queues_[idx];
        {
            std::lock_guard<std::mutex> lk(q.m);
            q.q.push_back(t);
        }
    }
    else {
        std::lock_guard<std::mutex> lg(globalMtx_);
        globalQueue_.push_back(t);
    }
    cvWork_.notify_one();
}

void TaskSystem::Submit(Task&& t) {
    std::size_t idx = ThreadIndex();
    if (!running_) {
        return;
    }

    inFlight_.fetch_add(1, std::memory_order_relaxed);

    if (idx < queues_.size()) {
        auto& q = queues_[idx];
        {
            std::lock_guard<std::mutex> lk(q.m);
            q.q.push_back(std::move(t));
        }
    }
    else {
        std::lock_guard<std::mutex> lg(globalMtx_);
        globalQueue_.push_back(std::move(t));
    }
    cvWork_.notify_one();
}

void TaskSystem::Dispatch(std::size_t jobCount,
                          std::function<void(std::size_t)> fn,
                          std::size_t batchSize) {
    if (jobCount == 0 || !fn) {
        return;
    }
    if (batchSize == 0) {
        batchSize = 1;
    }

    // Держим fn живой даже после выхода из Dispatch
    auto fnShared = std::make_shared<std::function<void(std::size_t)>>(std::move(fn));

    const std::size_t batches = (jobCount + batchSize - 1) / batchSize;
    for (std::size_t b = 0; b < batches; ++b) {
        const std::size_t begin = b * batchSize;
        const std::size_t end = std::min(jobCount, begin + batchSize);

        Submit([begin, end, fnShared]() {
            for (std::size_t i = begin; i < end; ++i) {
                (*fnShared)(i);
            }
            });
    }
}

void TaskSystem::Submit(const Task& t, TaskGroup* group) {
    std::shared_ptr<TaskGroup::State> st = group ? group->state : nullptr;

    std::size_t idx = ThreadIndex();
    if (!running_) { return; }
    if (st) { st->pending.fetch_add(1, std::memory_order_relaxed); }

    Task wrapped = [self = this, t, st]() {
        if (t) { t(); }

        if (st) {
            auto prev = st->pending.fetch_sub(1, std::memory_order_acq_rel);
            if (prev == 1) {
                st->pending.notify_all();
            }
        }
        self->cvWork_.notify_all();
    };

    inFlight_.fetch_add(1, std::memory_order_relaxed);

    if (idx < queues_.size()) {
        auto& q = queues_[idx];
        {
            std::lock_guard<std::mutex> lk(q.m);
            q.q.push_back(std::move(wrapped));
        }
    }
    else {
        std::lock_guard<std::mutex> lg(globalMtx_);
        globalQueue_.push_back(std::move(wrapped));
    }

    cvWork_.notify_one();
}

void TaskSystem::Submit(Task&& t, TaskGroup* group) {
    std::shared_ptr<TaskGroup::State> st = group ? group->state : nullptr;

    std::size_t idx = ThreadIndex();
    if (!running_) { return; }
    if (st) { st->pending.fetch_add(1, std::memory_order_relaxed); }

    Task wrapped = [self = this, tt = std::move(t), st]() mutable {
        if (tt) { tt(); }

        if (st) {
            auto prev = st->pending.fetch_sub(1, std::memory_order_acq_rel);
            if (prev == 1) {
                st->pending.notify_all();
            }
        }
        self->cvWork_.notify_all();
    };

    inFlight_.fetch_add(1, std::memory_order_relaxed);

    if (idx < queues_.size()) {
        auto& q = queues_[idx];
        {
            std::lock_guard<std::mutex> lk(q.m);
            q.q.push_back(std::move(wrapped));
        }
    }
    else {
        std::lock_guard<std::mutex> lg(globalMtx_);
        globalQueue_.push_back(std::move(wrapped));
    }

    cvWork_.notify_one();
}

void TaskSystem::Dispatch(std::size_t jobCount,
    std::function<void(std::size_t)> fn,
    std::size_t batchSize,
    TaskGroup* group)
{
    if (jobCount == 0 || !fn) {
        return;
    }
    if (batchSize == 0) {
        batchSize = 1;
    }

    auto fnShared = std::make_shared<std::function<void(std::size_t)>>(std::move(fn));
    const std::size_t batches = (jobCount + batchSize - 1) / batchSize;

    for (std::size_t b = 0; b < batches; ++b) {
        const std::size_t begin = b * batchSize;
        const std::size_t end = std::min(jobCount, begin + batchSize);

        Submit([begin, end, fnShared]() {
            for (std::size_t i = begin; i < end; ++i) {
                (*fnShared)(i);
            }
            }, group);
    }
}

void TaskSystem::WaitGroup(TaskGroup* group) {
    if (!group) { return; }
    std::shared_ptr<TaskGroup::State> st = group->state;

    for (;;) {
        if (st->pending.load(std::memory_order_acquire) == 0) {
            return;
        }

        Task task;
        {
            std::lock_guard<std::mutex> lg(globalMtx_);
            if (!globalQueue_.empty()) {
                task = std::move(globalQueue_.front());
                globalQueue_.pop_front();
            }
        }
        if (!task) {
            for (std::size_t i = 0; i < queues_.size(); ++i) {
                auto& q = queues_[i];
                std::lock_guard<std::mutex> lk(q.m);
                if (!q.q.empty()) {
                    task = std::move(q.q.front());
                    q.q.pop_front();
                    break;
                }
            }
        }

        if (task) {
            task();
            const std::size_t left =
                inFlight_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            inFlight_.notify_all();
            (void)left;
            continue;
        }

        std::size_t expected = st->pending.load(std::memory_order_acquire);
        if (expected != 0) {
            st->pending.wait(expected, std::memory_order_acquire);
        }
        if (!running_) { return; }
    }
}

void TaskSystem::WaitForAll() {
    for (std::size_t expected = inFlight_.load(std::memory_order_acquire);
         expected != 0;
         expected = inFlight_.load(std::memory_order_acquire)) {
        inFlight_.wait(expected, std::memory_order_acquire);
    }
}

std::size_t TaskSystem::ThreadIndex() const {
    return tlsIndex_;
}

void TaskSystem::WorkerLoop_(std::size_t index) {
    for (;;) {
        Task task;

        {
            auto& q = queues_[index];
            std::lock_guard<std::mutex> lk(q.m);
            if (!q.q.empty()) {
                task = std::move(q.q.back());
                q.q.pop_back();
            }
        }
        if (!task) {
            std::lock_guard<std::mutex> lg(globalMtx_);
            if (!globalQueue_.empty()) {
                task = std::move(globalQueue_.front());
                globalQueue_.pop_front();
            }
        }
        if (!task) {
            for (std::size_t i = 0; i < queues_.size(); ++i) {
                if (i == index) { continue; }
                auto& q = queues_[i];
                std::lock_guard<std::mutex> lk(q.m);
                if (!q.q.empty()) {
                    task = std::move(q.q.front());
                    q.q.pop_front();
                    break;
                }
            }
        }
        if (!task) {
            std::unique_lock<std::mutex> lk(workMtx_);
            cvWork_.wait(lk, [this]() {
                return !running_.load(std::memory_order_acquire) ||
                    inFlight_.load(std::memory_order_acquire) > 0;
            });
            if (!running_.load(std::memory_order_acquire) &&
                inFlight_.load(std::memory_order_acquire) == 0) {
                return;
            }
            else {
                continue;
            }
        }

        task();

        const std::size_t left = inFlight_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        inFlight_.notify_all();
        (void)left;
    }
}