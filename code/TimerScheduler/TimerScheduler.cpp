#include "TimerScheduler.h"

#include <stdexcept>
#include <utility>

namespace {

thread_local const void *current_timer_state = nullptr;

class CallbackContextGuard {
  public:
    explicit CallbackContextGuard(const void *state) noexcept : previous_(current_timer_state) {
        current_timer_state = state;
    }

    ~CallbackContextGuard() { current_timer_state = previous_; }

    CallbackContextGuard(const CallbackContextGuard &) = delete;
    CallbackContextGuard &operator=(const CallbackContextGuard &) = delete;

  private:
    const void *previous_;
};

} // namespace

TimerScheduler::TimerScheduler(ThreadPool &executor)
    : executor_(executor), state_(std::make_shared<SharedState>()) {}

TimerScheduler::~TimerScheduler() { stop(); }

void TimerScheduler::start() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->scheduler_state != SchedulerState::Created) {
        throw std::logic_error("TimerScheduler can only be started once");
    }
    state_->scheduler_state = SchedulerState::Running;
    try {
        auto state = state_;
        auto *executor = &executor_;
        worker_ = std::thread([state, executor] { worker_loop(state, *executor); });
    } catch (...) {
        state_->scheduler_state = SchedulerState::Created;
        throw;
    }
}

void TimerScheduler::request_stop() noexcept {
    bool need_notify = false;

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        switch (state_->scheduler_state) {
        case SchedulerState::Created:
            state_->scheduler_state = SchedulerState::Stopped;
            break;
        case SchedulerState::Running:
            state_->scheduler_state = SchedulerState::Stopping;
            for (auto &[id, timer_state] : state_->states) {
                if (timer_state == TimerState::Pending) {
                    timer_state = TimerState::Cancelled;
                }
            }
            need_notify = true;
            break;
        case SchedulerState::Stopping:
        case SchedulerState::Stopped:
            break;
        }
    }

    if (need_notify) {
        state_->cv.notify_all();
    }
}

void TimerScheduler::join_worker() {
    std::lock_guard<std::mutex> lock(join_mutex_);
    if (!worker_.joinable()) {
        return;
    }
    if (worker_.get_id() == std::this_thread::get_id()) {
        throw std::logic_error("TimerScheduler cannot join its own"
                               "worker thread");
    }
    worker_.join();
}

void TimerScheduler::wait() {
    join_worker();

    /*
     * 当前线程正处于该调度器提交的回调中时，
     * 不能等待 in_flight == 0，否则会等待自己。
     */
    if (current_timer_state == state_.get()) {
        return;
    }

    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->finished_cv.wait(lock, [state = state_] { return state->in_flight == 0; });
    state_->tasks = TaskQueue{};
    state_->states.clear();
    state_->scheduler_state = SchedulerState::Stopped;
}

void TimerScheduler::stop() {
    request_stop();

    /*
     * 即使 stop() 在回调内部调用，也必须 join
     * 调度线程，否则 TimerScheduler 析构时
     * std::thread 仍为 joinable 会触发 terminate。
     */
    join_worker();

    /*
     * 回调线程不能等待自身完成。
     * SharedState 被回调持有，因此对象析构后
     * 回调仍然可以安全完成清理。
     */
    if (current_timer_state == state_.get()) {
        return;
    }

    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->finished_cv.wait(lock, [state = state_] { return state->in_flight == 0; });
    state_->tasks = TaskQueue{};
    state_->states.clear();
    state_->scheduler_state = SchedulerState::Stopped;
}

TimerScheduler::TimerId TimerScheduler::schedule_after(Duration delay, Task task) {
    if (delay < Duration::zero()) {
        throw std::invalid_argument("Timer delay cannot be negative");
    }

    if (!task) {
        throw std::invalid_argument("Timer callback cannot be empty");
    }

    const auto expiration = Clock::now() + delay;

    TimerId id;
    bool need_wakeup;

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->scheduler_state != SchedulerState::Running) {
            throw std::logic_error("TimerScheduler is not running");
        }
        id = state_->next_id++;
        need_wakeup = state_->tasks.empty() || expiration < state_->tasks.top().expiration;
        state_->states.emplace(id, TimerState::Pending);
        state_->tasks.push(TimerTask{id, expiration, Duration::zero(), false, std::move(task)});
    }

    if (need_wakeup) {
        state_->cv.notify_one();
    }

    return id;
}

TimerScheduler::TimerId TimerScheduler::schedule_every(Duration interval, Task task) {
    if (interval <= Duration::zero()) {
        throw std::invalid_argument("Timer interval must be positive");
    }

    if (!task) {
        throw std::invalid_argument("Timer callback cannot be empty");
    }

    const auto expiration = Clock::now() + interval;

    TimerId id;
    bool need_wakeup;

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->scheduler_state != SchedulerState::Running) {
            throw std::logic_error("TimerScheduler is not running");
        }
        id = state_->next_id++;
        need_wakeup = state_->tasks.empty() || expiration < state_->tasks.top().expiration;
        state_->states.emplace(id, TimerState::Pending);
        state_->tasks.push(TimerTask{id, expiration, interval, true, std::move(task)});
    }

    if (need_wakeup) {
        state_->cv.notify_one();
    }

    return id;
}

bool TimerScheduler::cancel(TimerId id) {
    bool need_wakeup = false;

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto it = state_->states.find(id);

        if (it == state_->states.end() || it->second == TimerState::Cancelled) {
            return false;
        }
        it->second = TimerState::Cancelled;
        need_wakeup = !state_->tasks.empty() && state_->tasks.top().id == id;
    }

    if (need_wakeup) {
        state_->cv.notify_one();
    }

    return true;
}

void TimerScheduler::finish_in_flight(const std::shared_ptr<SharedState> &state) noexcept {
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->in_flight > 0) {
            --state->in_flight;
        }
        finished = state->in_flight == 0;
    }

    if (finished) {
        state->finished_cv.notify_all();
    }
}

void TimerScheduler::worker_loop(std::shared_ptr<SharedState> state, ThreadPool &executor) {
    std::unique_lock<std::mutex> lock(state->mutex);

    while (state->scheduler_state == SchedulerState::Running) {
        if (state->tasks.empty()) {
            state->cv.wait(lock, [&state] {
                return state->scheduler_state != SchedulerState::Running || !state->tasks.empty();
            });

            continue;
        }

        const auto waiting_id = state->tasks.top().id;
        const auto expiration = state->tasks.top().expiration;

        const bool interrupted =
            state->cv.wait_until(lock, expiration, [&state, waiting_id, expiration] {
                if (state->scheduler_state != SchedulerState::Running) {
                    return true;
                }
                if (state->tasks.empty()) {
                    return true;
                }
                if (state->tasks.top().id != waiting_id) {
                    return true;
                }
                if (state->tasks.top().expiration < expiration) {
                    return true;
                }
                const auto it = state->states.find(waiting_id);
                return it == state->states.end() || it->second == TimerState::Cancelled;
            });

        if (interrupted) {
            /*
             * 修复：堆顶任务被取消且尚未到期时，wait_until 的谓词
             * （Cancelled）立即为真，wait_until 会带着锁立即返回，
             * 导致调度线程持锁忙等（后续 stop()/schedule_*() 全部死锁）。
             * 这里先把堆顶已取消的任务弹出，再重新评估。
             */
            for (;;) {
                if (state->tasks.empty()) {
                    break;
                }
                const auto top_id = state->tasks.top().id;
                const auto state_it = state->states.find(top_id);
                if (state_it != state->states.end() &&
                    state_it->second == TimerState::Cancelled) {
                    state->tasks.pop();
                    state->states.erase(state_it);
                    continue;
                }
                break;
            }
            continue;
        }

        const auto now = Clock::now();

        if (state->tasks.empty() || state->tasks.top().expiration > now) {
            continue;
        }

        std::vector<TimerTask> ready_tasks;

        while (!state->tasks.empty() && state->tasks.top().expiration <= now) {
            const auto id = state->tasks.top().id;
            auto state_it = state->states.find(id);

            if (state_it == state->states.end()) {
                state->tasks.pop();
                continue;
            }
            if (state_it->second == TimerState::Cancelled) {
                state->tasks.pop();
                state->states.erase(state_it);
                continue;
            }
            if (state_it->second != TimerState::Pending) {
                /*
                 * 正常状态机下不会发生。
                 * 将异常的堆节点丢弃，但不删除
                 * 可能仍由线程池持有的状态记录。
                 */
                state->tasks.pop();
                continue;
            }

            state_it->second = TimerState::Submitted;
            const auto &top = state->tasks.top();
            ready_tasks.push_back(TimerTask{top.id, top.expiration, top.interval, top.repeated,
                                            std::move(top.callback)});
            state->tasks.pop();
        }

        lock.unlock();

        for (auto &task : ready_tasks) {
            const TimerId id = task.id;
            const Duration interval = task.interval;
            const bool repeated = task.repeated;

            /*
             * 在真正提交线程池前进行一次状态检查。
             * 如果 stop() 已经开始，尚未投递的
             * ready task 直接清理。
             */
            {
                std::lock_guard<std::mutex> state_lock(state->mutex);
                auto it = state->states.find(id);

                if (it == state->states.end() || it->second == TimerState::Cancelled ||
                    state->scheduler_state != SchedulerState::Running) {
                    if (it != state->states.end()) {
                        state->states.erase(it);
                    }
                    continue;
                }
                if (it->second != TimerState::Submitted) {
                    continue;
                }
                ++state->in_flight;
            }

            std::shared_ptr<void> completion;

            try {
                completion = std::shared_ptr<void>(
                    nullptr, [state](void *) noexcept { finish_in_flight(state); });
            } catch (...) {
                {
                    std::lock_guard<std::mutex> state_lock(state->mutex);
                    state->states.erase(id);
                }
                finish_in_flight(state);
                continue;
            }

            auto wrapper = [state, id, interval, repeated, cb = std::move(task.callback),
                            completion]() mutable {
                CallbackContextGuard context(state.get());

                /*
                 * completion 的析构负责减少
                 * in_flight。即使任务在队列中
                 * 被线程池销毁，也会正确归零。
                 */
                (void)completion;

                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    auto it = state->states.find(id);
                    if (it == state->states.end()) {
                        return;
                    }
                    if (it->second == TimerState::Cancelled) {
                        state->states.erase(it);
                        return;
                    }
                    if (it->second != TimerState::Submitted) {
                        return;
                    }
                    it->second = TimerState::Executing;
                }

                try {
                    if (cb) {
                        cb();
                    }
                } catch (...) {
                    /*
                     * 后续可接入日志或异常处理器。
                     */
                }

                bool need_wakeup = false;

                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    auto it = state->states.find(id);

                    if (it == state->states.end()) {
                        return;
                    }
                    if (it->second == TimerState::Cancelled) {
                        state->states.erase(it);
                        return;
                    }
                    if (it->second != TimerState::Executing) {
                        state->states.erase(it);
                        return;
                    }

                    if (repeated && state->scheduler_state == SchedulerState::Running) {
                        const auto next_expiration = Clock::now() + interval;
                        need_wakeup =
                            state->tasks.empty() || next_expiration < state->tasks.top().expiration;
                        it->second = TimerState::Pending;
                        state->tasks.push(
                            TimerTask{id, next_expiration, interval, true, std::move(cb)});
                    } else {
                        state->states.erase(it);
                    }
                }

                if (need_wakeup) {
                    state->cv.notify_one();
                }
            };

            SubmitResult result;

            try {
                result = executor.try_post(std::move(wrapper));
            } catch (...) {
            }

            if (result != SubmitResult::accepted) {
                {
                    std::lock_guard<std::mutex> state_lock(state->mutex);
                    state->states.erase(id);
                }
            }

            /*
             * accepted：
             * wrapper 持有 completion。
             *
             * rejected：
             * wrapper 已销毁或即将销毁，
             * reset 后 completion 的最后一个引用
             * 会减少 in_flight。
             */
            completion.reset();
        }

        lock.lock();
    }
}