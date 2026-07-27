#pragma once 

#include <vector>
#include <thread>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <future>
#include <stdexcept>
#include <type_traits>
#include <chrono>
#include <utility>

enum class SubmitResult {
    accepted,
    queue_full,
    timeout,
    pool_stopping
};

enum class RejectPolicy {
    abort,
    caller_runs,
    discard
};

class ThreadPool {
    using Task = std::function<void()>;

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

public:

    explicit ThreadPool(std::size_t thread_count, std::size_t queue_capacity, RejectPolicy reject_policy = RejectPolicy::abort);
    
    ~ThreadPool();

    // Try to post a task to the thread pool. If the pool is stopping or the queue is full, it will return an appropriate SubmitResult.
    SubmitResult try_post(Task task);

    // Post a task to the thread pool and wait until it can be accepted. If the pool is stopping, it will throw a runtime_error.
    template <class Rep, class Period>
    SubmitResult post_for(Task task, const std::chrono::duration<Rep, Period>& timeout);

    // Post a task to the thread pool. If the pool is stopping or the queue is full, it will throw a runtime_error.
    void post_wait(Task task);

    template <class F, class... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

};

// Implementation of the submit function
// This function allows users to submit tasks to the thread pool and get a future for the result.
template <class F, class... Args>
auto ThreadPool::submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
    using return_type = std::invoke_result_t<F, Args...>;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    std::future<return_type> result = task->get_future();
    // The task is wrapped in a lambda and posted to the thread pool's task queue.
    post([task]() { (*task)(); });

    return result;
}

template <class Rep, class Period>
SubmitResult ThreadPool::post_for(Task task, const std::chrono::duration<Rep, Period>& timeout) {
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (stopping) {
            return SubmitResult::pool_stopping;
        }

        bool ready = not_full.wait_for(lock, timeout, [this]() {
            return tasks.size() < queue_capacity || stopping;
        });
        if (!ready) {
            return SubmitResult::timeout;
        }

        if (stopping) {
            return SubmitResult::pool_stopping;
        }

        tasks.emplace(std::move(task));
    }

    not_empty.notify_one();
    return SubmitResult::accepted;
}
