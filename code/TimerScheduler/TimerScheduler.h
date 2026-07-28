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

#include "../ThreadPool/MoveOnlyFunction.h"
#include "../ThreadPool/ThreadPool.h"

class TimerScheduler {
public:
    using TimerId = std::uint64_t;
    using Task = MoveOnlyFunction<void()>;
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    explicit TimerScheduler(ThreadPool& executor);
    ~TimerScheduler();

    TimerScheduler(const TimerScheduler&) = delete;
    TimerScheduler& operator=(const TimerScheduler&) = delete;

    void start();

    void request_stop() noexcept;
    void wait();
    void stop();

    TimerId schedule_after(Duration delay, Task task);
    TimerId schedule_every(Duration interval, Task task);

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