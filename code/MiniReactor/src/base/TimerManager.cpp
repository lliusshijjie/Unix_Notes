#include "base/TimerManager.h"

#include <utility>

namespace minireactor {

TimerManager::TimerManager(std::size_t workerCount, std::size_t queueCapacity)
    : pool_(workerCount, queueCapacity), scheduler_(pool_) {}

TimerManager::~TimerManager() { stop(); }

TimerManager& TimerManager::instance() {
    static TimerManager manager;
    return manager;
}

void TimerManager::start() {
    std::lock_guard<std::mutex> lock(startMutex_);
    if (started_) {
        return;
    }
    scheduler_.start();
    started_ = true;
}

void TimerManager::stop() { scheduler_.stop(); }

void TimerManager::ensureStarted() {
    std::lock_guard<std::mutex> lock(startMutex_);
    if (!started_) {
        scheduler_.start();
        started_ = true;
    }
}

TimerManager::TimerId TimerManager::runAfter(Duration delay, Task task) {
    ensureStarted();
    return scheduler_.schedule_after(delay, std::move(task));
}

TimerManager::TimerId TimerManager::runEvery(Duration interval, Task task) {
    ensureStarted();
    return scheduler_.schedule_every(interval, std::move(task));
}

bool TimerManager::cancel(TimerId id) { return scheduler_.cancel(id); }

ThreadPool& TimerManager::executor() noexcept { return pool_; }

}  // namespace minireactor
