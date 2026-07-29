#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../../ThreadPool/MoveOnlyFunction.h"
#include "../../ThreadPool/ThreadPool.h"

class MyTimerScheduler {
public:
    using TimerId = std::uint64_t;
    using Task = MoveOnlyFunction<void()>;
    using Clock = std::chrono::steady_clock;
    using TimePoint = std::chrono::steady_clock::time_point;
    using Duration = std::chrono::steady_clock::duration;

    explicit MyTimerScheduler(ThreadPool& executor);
    ~MyTimerScheduler();

    MyTimerScheduler(const MyTimerScheduler&) = delete;
    MyTimerScheduler& operator=(const MyTimerScheduler&) = delete;

    void start();

    void request_stop() noexcept;
    void wait();
    void stop();

    TimerId scheduler_after(Duration delay, Task task);
    TimerId scheduler_every(Duration interval, Task task);

    bool cancel(TimerId id);

private:
    enum class SchedulerState {
        Created,
        Running,
        Stopping,
        Stopped
    };

    enum class TimerState {
        Pending,
        Submitted,
        Executing,
        Cancelled
    };

    struct TimerTask {
        TimerId id;
        TimePoint expiration;
        Duration interval;
        bool repeated;
        mutable Task callback;
    };

    struct TimerTaskCompare {
        bool operator()(const TimerTask& lhs, const TimerTask& rhs) const noexcept {
            if (lhs.expiration != rhs.expiration) {
                return lhs.expiration > rhs.expiration;
            }
            return lhs.id > rhs.id;
        }
    };

    using TaskQueue = std::priority_queue<TimerTask, std::vector<TimerTask>, TimerTaskCompare>;

    struct SharedState {
        std::mutex mutex;
        std::condition_variable cv;
        std::condition_variable finished_cv;

        TaskQueue tasks;
        std::unordered_map<TimerId, TimerState> states;

        SchedulerState scheduler_state{ SchedulerState::Created };

        TimerId next_id{1};
        std::size_t in_flight{0};
    };

    static void worker_loop(std::shared_ptr<SharedState> state, ThreadPool& executor);
    static void finish_in_flight(const std::shared_ptr<SharedState>& state) noexcept;

    void join_worker();

private:
    ThreadPool& executor_;
    std::shared_ptr<SharedState> state_;

    std::thread worker_;
    std::mutex join_mutex_;
};
