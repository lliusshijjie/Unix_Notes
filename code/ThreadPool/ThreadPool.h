#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

enum class SubmitResult
{
    accepted,
    queue_full,
    timeout,
    pool_stopping
};

enum class RejectPolicy
{
    abort,
    caller_runs,
    discard
};

class ThreadPool
{
public:
    using Task = std::function<void()>;

    ThreadPool(std::size_t worker_count,
               std::size_t queue_capacity,
               RejectPolicy reject_policy = RejectPolicy::abort);

    ~ThreadPool();

    SubmitResult try_post(Task task);

    void post_wait(Task task);

    template <class Rep, class Period>
    SubmitResult post_for(Task task, const std::chrono::duration<Rep, Period>& timeout);

    template <class F, class... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    void post(Task task);

    std::vector<std::thread> workers;
    std::queue<Task> tasks;
    std::size_t queue_capacity;
    RejectPolicy reject_policy;
    std::mutex mutex;
    std::condition_variable not_empty;
    std::condition_variable not_full;
    bool stopping;
};

template <class F, class... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>>
{
    using return_type = std::invoke_result_t<F, Args...>;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    std::future<return_type> result = task->get_future();
    post([task]() { (*task)(); });
    return result;
}

template <class Rep, class Period>
SubmitResult ThreadPool::post_for(Task task, const std::chrono::duration<Rep, Period>& timeout)
{
    {
        std::unique_lock<std::mutex> lock(mutex);
        const bool ready = not_full.wait_for(lock, timeout, [this]() {
            return stopping || tasks.size() < queue_capacity;
        });
        if (stopping)
        {
            return SubmitResult::pool_stopping;
        }
        if (!ready)
        {
            return SubmitResult::timeout;
        }
        tasks.emplace(std::move(task));
    }
    not_empty.notify_one();
    return SubmitResult::accepted;
}
