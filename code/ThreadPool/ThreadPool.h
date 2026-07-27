#pragma once

#include "BoundedRingQueue.h"
#include "LocalTaskQueue.h"
#include "MoveOnlyFunction.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
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
    using Task = MoveOnlyFunction<void()>;

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
    void WorkerLoop(std::size_t worker_index);
    bool TrySteal(std::size_t worker_index, Task& task);
    bool HasStealableWork(std::size_t worker_index) const;
    bool AllLocalEmpty() const;
    bool HasAnyWork(std::size_t worker_index) const;
    void NotifyWorkers();

    static thread_local LocalTaskQueue* tls_local;

    std::vector<std::thread> workers;
    std::vector<std::unique_ptr<LocalTaskQueue>> local_queues;
    BoundedRingQueue<Task> tasks;
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

    std::packaged_task<return_type()> packaged(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    std::future<return_type> result = packaged.get_future();
    post([packaged = std::move(packaged)]() mutable { packaged(); });
    return result;
}

template <class Rep, class Period>
SubmitResult ThreadPool::post_for(Task task, const std::chrono::duration<Rep, Period>& timeout)
{
    if (tls_local != nullptr)
    {
        tls_local->PushOwner(std::move(task));
        NotifyWorkers();
        return SubmitResult::accepted;
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        const bool ready = not_full.wait_for(lock, timeout, [this]() {
            return stopping || !tasks.full();
        });
        if (stopping)
        {
            return SubmitResult::pool_stopping;
        }
        if (!ready)
        {
            return SubmitResult::timeout;
        }
        tasks.push(std::move(task));
    }
    NotifyWorkers();
    return SubmitResult::accepted;
}
