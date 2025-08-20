#include "TaskSystem.h"
#include <algorithm>

thread_local std::size_t TaskSystem::tlsIndex_ = static_cast<std::size_t>(-1);

TaskSystem& TaskSystem::Get() {
    static TaskSystem g;
    return g;
}

TaskSystem::~TaskSystem() {
    Stop();
}

void TaskSystem::Start(unsigned threadCount) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (running_) {
        return;
    }

    running_ = true;

    if (threadCount == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0u) {
            hc = 4u;
        }
        threadCount = std::max(1u, hc - 1u); // один поток оставим главному
    }

    workers_.reserve(threadCount);
    for (unsigned i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this, i]() {
            tlsIndex_ = i;
            WorkerLoop_(i);
        });
    }
}

void TaskSystem::Stop() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    cvWork_.notify_all();

    for (auto& th : workers_) {
        if (th.joinable()) {
            th.join();
        }
    }
    workers_.clear();

    // Очистим очередь (на всякий случай)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        while (!queue_.empty()) {
            queue_.pop();
        }
    }
}

void TaskSystem::Submit(const Task& t) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!running_) {
            return;
        }
        queue_.push(t);
        inFlight_.fetch_add(1, std::memory_order_relaxed);
    }
    cvWork_.notify_one();
}

void TaskSystem::Submit(Task&& t) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!running_) {
            return;
        }
        queue_.push(std::move(t));
        inFlight_.fetch_add(1, std::memory_order_relaxed);
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

void TaskSystem::WaitForAll() {
    std::unique_lock<std::mutex> lk(mtx_);
    cvIdle_.wait(lk, [this]() {
        return (inFlight_.load(std::memory_order_acquire) == 0) && queue_.empty();
    });
}

std::size_t TaskSystem::ThreadIndex() const {
    return tlsIndex_;
}

void TaskSystem::WorkerLoop_(std::size_t /*index*/) {
    for (;;) {
        Task task;

        {
            std::unique_lock<std::mutex> lk(mtx_);
            cvWork_.wait(lk, [this]() {
                return !running_ || !queue_.empty();
            });

            if (!running_ && queue_.empty()) {
                break;
            }

            task = std::move(queue_.front());
            queue_.pop();
        }

        // Выполняем за пределами лока
        if (task) {
            task();
        }

        // Обновляем счётчики и будим возможных ждунов
        const std::size_t left = inFlight_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (left == 0) {
            std::lock_guard<std::mutex> lk(mtx_);
            if (queue_.empty()) {
                cvIdle_.notify_all();
            }
        }
    }
}