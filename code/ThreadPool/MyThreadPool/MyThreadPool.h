#pragma once 

#include "MyBoundedRingQueue.h"
#include "MyLocalTaskQueue.h"
#include "MyMoveOnlyFunction.h"

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

class MyThreadPool {
    using Task = std::function<void()>;

private:
    void post(Task task);
    void WorkerLoop(std::size_t worker_index);
    bool TrySteal(Task& task, std::size_t worker_index);
    bool HasStealableWorker(std::size_t worker_index) const;
    bool AllLocalEmpty() const;
    bool HasAnyWork(std::size_t worker_index) const;
    void NotifyAnyWorker();

    static thread_local MyLocalTaskQueue* tls_local_queue;

    std::vector<std::thread> workers;
    std::vector<std::unique_ptr<MyLocalTaskQueue>> local_queues;
    MyBoundedRingQueue<Task> tasks;
    std::size_t queue_capacity;
    RejectPolicy reject_policy;
    std::mutex mutex;
    std::condition_variable not_empty;
    std::condition_variable not_full;
    bool stopping;

public:

    explicit MyThreadPool(std::size_t thread_count, std::size_t queue_capacity, RejectPolicy reject_policy = RejectPolicy::abort);
    
    ~MyThreadPool();

    // Try to post a task to the thread pool. If the pool is stopping or the queue is full, it will return an appropriate SubmitResult.
    SubmitResult try_post(Task task);

    // Post a task to the thread pool and wait until it can be accepted. If the pool is stopping, it will throw a runtime_error.
    template <class Rep, class Period>
    SubmitResult post_for(Task task, const std::chrono::duration<Rep, Period>& timeout);

    // Post a task to the thread pool. If the pool is stopping or the queue is full, it will throw a runtime_error.
    void post_wait(Task task);

    template <class F, class... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

    MyThreadPool(const MyThreadPool&) = delete;
    MyThreadPool& operator=(const MyThreadPool&) = delete;

};

// Implementation of the submit function
// This function allows users to submit tasks to the thread pool and get a future for the result.
template <class F, class... Args>
auto MyThreadPool::submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
    using return_type = std::invoke_result_t<F, Args...>;

    std::packaged_task<return_type()> packaged(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    std::future<return_type> result = packaged.get_future();
    post([packaged = std::move(packaged)]() mutable { packaged(); });
    return result;
}

template <class Rep, class Period>
SubmitResult MyThreadPool::post_for(Task task, const std::chrono::duration<Rep, Period>& timeout) {
    if (tls_local_queue) {
        tls_local_queue->PushOwner(std::move(task));
        NotifyAllWorkers();
        return SubmitResult::accepted;
    }

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

    NotifyAllWorkers();
    return SubmitResult::accepted;
}
