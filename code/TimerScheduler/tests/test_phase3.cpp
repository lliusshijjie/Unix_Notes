#include "../TimerScheduler.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

int g_failed = 0;
void Expect(bool cond, const char* name) {
    if (cond) std::cout << "[PASS] " << name << '\n';
    else { std::cout << "[FAIL] " << name << '\n'; ++g_failed; }
}

void TestDecoupledExecution() {
    ThreadPool pool(2, 1024);
    TimerScheduler scheduler(pool);
    scheduler.start();
    
    std::atomic<int> counter{0};
    
    // Schedule a task that blocks the worker
    scheduler.schedule_after(std::chrono::milliseconds(50), [&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        counter.fetch_add(1, std::memory_order_relaxed);
    });
    
    // Schedule another task that should run concurrently on the thread pool
    // If scheduler wasn't decoupled, this would be delayed by the 200ms sleep
    scheduler.schedule_after(std::chrono::milliseconds(100), [&]{
        counter.fetch_add(10, std::memory_order_relaxed);
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Expect(counter.load() == 10, "Task 2 ran concurrently while Task 1 was sleeping");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Expect(counter.load() == 11, "Task 1 completed eventually");
}

void TestBatchSubmission() {
    ThreadPool pool(4, 1024);
    TimerScheduler scheduler(pool);
    scheduler.start();
    
    std::atomic<int> counter{0};
    
    // Schedule 10 tasks to execute at the exact same time
    for (int i = 0; i < 10; ++i) {
        scheduler.schedule_after(std::chrono::milliseconds(100), [&]{
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Expect(counter.load() == 10, "All 10 batch tasks executed");
}

void TestCancelInSubmittedState() {
    ThreadPool pool(1, 1024); // 1 worker to ensure queuing
    TimerScheduler scheduler(pool);
    scheduler.start();
    
    std::atomic<int> counter{0};
    
    // Occupy the only thread pool worker
    scheduler.schedule_after(std::chrono::milliseconds(10), []{
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });
    
    // Schedule a task that will expire and be submitted to the thread pool queue
    auto id = scheduler.schedule_after(std::chrono::milliseconds(50), [&]{
        counter++;
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // The task should now be in the Submitted state in the thread pool queue
    
    bool cancelled = scheduler.cancel(id);
    Expect(cancelled, "Cancel returns true for Submitted task");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Expect(counter.load() == 0, "Submitted but cancelled task did not execute");
}

void TestStopClearsPending() {
    ThreadPool pool(2, 1024);
    
    std::atomic<int> counter{0};
    
    {
        TimerScheduler scheduler(pool);
        scheduler.start();
        
        scheduler.schedule_after(std::chrono::milliseconds(200), [&]{
            counter++;
        });
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // Scheduler is destroyed here, calling stop()
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    Expect(counter.load() == 0, "Pending tasks were cleared on stop");
}

int main() {
    TestDecoupledExecution();
    TestBatchSubmission();
    TestCancelInSubmittedState();
    TestStopClearsPending();
    
    if (g_failed == 0) {
        std::cout << "ALL PHASE 3 PASSED\n";
        return 0;
    } else {
        std::cout << g_failed << " FAILED\n";
        return 1;
    }
}
