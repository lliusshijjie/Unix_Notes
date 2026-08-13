#pragma once

#include "base/NonCopyable.h"

#include "ThreadPool.h"
#include "TimerScheduler.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace minireactor {

// 定时器管理器：封装上一级目录的两个轮子——
//   TimerScheduler（时间堆调度）+ ThreadPool（回调执行）。
//
// 定时器回调在 ThreadPool 的 worker 线程执行；EventLoop::runAfter/runEvery
// 会把回调 marshal 回 loop 线程，从而获得与 muduo 一致的"回调在 loop 线程执行"
// 语义。ThreadPool 同时也暴露为业务线程池（executor()）。
//
// 进程级单例：TimerManager::instance()，首次调度时自动启动。
class TimerManager : private NonCopyable {
public:
    using TimerId = ::TimerScheduler::TimerId;
    using Task = ::TimerScheduler::Task;
    using Duration = std::chrono::steady_clock::duration;

    explicit TimerManager(std::size_t workerCount = 2, std::size_t queueCapacity = 4096);
    ~TimerManager();

    static TimerManager& instance();

    void start();
    void stop();

    TimerId runAfter(Duration delay, Task task);
    TimerId runEvery(Duration interval, Task task);
    bool cancel(TimerId id);

    // 业务任务提交（与定时回调共用线程池）
    ThreadPool& executor() noexcept;

private:
    void ensureStarted();

    ThreadPool pool_;
    TimerScheduler scheduler_;
    std::mutex startMutex_;
    bool started_ = false;
};

}  // namespace minireactor
