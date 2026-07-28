#include "MyThreadPool.h"

thread_local MyLocalTaskQueue* MyThreadPool::tls_local_queue = nullptr;

MyThreadPool::MyThreadPool(std::size_t worker_count, std::size_t queue_capacity, RejectPolicy reject_policy) 
    : stopping(false)
    , tasks(queue_capacity)
    , queue_capacity(queue_capacity) 
    , reject_policy(reject_policy) {
    if (queue_capacity == 0) {
        throw std::runtime_error("queue_capacity must be greater than 0");
    }
    
    if (worker_count == 0) {
        throw std::runtime_error("worker_count must be greater than 0");
    }

    local_queues.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; i++) {
        local_queues.emplace_back(std::make_unique<MyLocalTaskQueue>());
    }

    workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; i++) {
        workers.emplace_back([this, i]() { WorkerLoop(i); });
    }
}

MyThreadPool::~MyThreadPool() {
    {
        std::unique_lock<std::mutex> lock(mutex);
        stopping = true;
    }

    not_empty.notify_all();
    not_full.notify_all();
    
    for (std::thread& worker : workers) {
        worker.join();
    }
}

void MyThreadPool::NotifyAnyWorker() {
    not_empty.notify_one();
}

bool MyThreadPool::TrySteal(Task& task, std::size_t worker_index) {
    const std::size_t count = local_queues.size();
    for (std::size_t offset = 1; offset < count; offset++) {
        const std::size_t victim = (worker_index + offset) % count;
        if (local_queues[victim]->TrySteal(task)) {
            return true;
        }
    }
    return false;
}

bool MyThreadPool::HasStealableWorker(std::size_t worker_index) const {
    const std::size_t count = local_queues.size();
    for (std::size_t offset = 1; offset < count; offset++) {
        const std::size_t victim = (worker_index + offset) % count;
        if (!local_queues[victim]->Empty()) {
            return true;
        }
    }
    return false;
}

bool MyThreadPool::AllLocalEmpty() const {
    for (const auto& local_queue : local_queues) {
        if (!local_queue->Empty()) {
            return false;
        }
    }
    return true;
}

bool MyThreadPool::HasAnyWork(std::size_t worker_index) const {
    if (!tasks.empty()) {
        return true;
    }
    if (!local_queues[worker_index]->Empty()) {
        return true;
    }
    return HasStealableWorker(worker_index);
}


/*
    Main worker loop for each thread in the pool.
*/
void MyThreadPool::WorkerLoop(std::size_t worker_index) {
    tls_local_queue = local_queues[worker_index].get();

    for (;;) {
        Task task;

        if (tls_local_queue->TryPopOwner(task)) {
            task();
            continue;
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!tasks.empty()) {
                task = tasks.pop();
                lock.unlock();
                not_full.notify_one();
                task();
                continue;
            }
        }

        if (TrySteal(task, worker_index)) {
            task();
            continue;
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            not_empty.wait(lock, [this, worker_index]() {
                return stopping || HasAnyWork(worker_index);
            });
            if (stopping && tasks.empty() && AllLocalEmpty()) {
                return;
            }
        }
    }
}

SubmitResult MyThreadPool::try_post(Task task) {
    if (tls_local_queue) {
        tls_local_queue->PushOwner(std::move(task));
        NotifyAnyWorker();
        return SubmitResult::accepted;
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (stopping) {
            return SubmitResult::pool_stopping;
        }
        if (tasks.full()) {
            return SubmitResult::queue_full;
        }
        tasks.push(std::move(task));
    }

    NotifyAnyWorker();
    return SubmitResult::accepted;
}

void MyThreadPool::post_wait(Task task) {
    if (tls_local_queue) {
        tls_local_queue->PushOwner(std::move(task));
        NotifyAnyWorker();
        return;
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        not_full.wait(lock, [this]() {
            return stopping || !tasks.full();
        });
        if (stopping) {
            throw std::runtime_error("submit on stopped MyThreadPool");
        }
        tasks.push(std::move(task));
    }

    NotifyAnyWorker();
}

void MyThreadPool::post(std::function<void()> task) {
    if (tls_local_queue) {
        tls_local_queue->PushOwner(std::move(task));
        NotifyAnyWorker();
        return;
    }

    bool accepted = false;
    bool run_here = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (stopping)
        {
            throw std::runtime_error("submit on stopped ThreadPool");
        }
        if (tasks.full())
        {
            if (reject_policy == RejectPolicy::abort)
            {
                throw std::runtime_error("task queue is full");
            }
            if (reject_policy == RejectPolicy::discard)
            {
                return;
            }
            run_here = true;
        }
        else
        {
            tasks.push(std::move(task));
            accepted = true;
        }
    }
    if (accepted)
    {
        NotifyAnyWorker();
        return;
    }
    if (run_here)
    {
        task();
    }
}
