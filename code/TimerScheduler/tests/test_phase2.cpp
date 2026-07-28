#include "../TimerScheduler.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "../ThreadPool/ThreadPool.h"

int g_failed = 0;
void Expect(bool cond, const char* name) {
    if (cond) std::cout << "[PASS] " << name << '\n';
    else { std::cout << "[FAIL] " << name << '\n'; ++g_failed; }
}

void TestCancelOneShot() {
    ThreadPool pool(2, 1024);
    TimerScheduler scheduler(pool);
    scheduler.start();
    std::atomic<bool> executed{false};
    
    auto id = scheduler.schedule_after(std::chrono::milliseconds(100), [&]{
        executed = true;
    });
    
    bool cancelled = scheduler.cancel(id);
    Expect(cancelled, "Cancel returns true");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Expect(!executed.load(), "Cancelled task did not execute");
}

void TestPeriodicTask() {
    ThreadPool pool(2, 1024);
    TimerScheduler scheduler(pool);
    scheduler.start();
    std::atomic<int> counter{0};
    
    auto id = scheduler.schedule_every(std::chrono::milliseconds(50), [&]{
        counter++;
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(220)); // Should run ~4 times
    scheduler.cancel(id);
    
    int count = counter.load();
    Expect(count >= 3 && count <= 5, "Periodic task repeats");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Expect(counter.load() == count, "Periodic task stops after cancel");
}

void TestCancelTopTaskReevaluates() {
    ThreadPool pool(2, 1024);
    TimerScheduler scheduler(pool);
    scheduler.start();
    std::atomic<int> executed{0};
    
    auto id1 = scheduler.schedule_after(std::chrono::milliseconds(100), [&]{
        executed = 1;
    });
    scheduler.schedule_after(std::chrono::milliseconds(200), [&]{
        if (executed.load() == 0) executed = 2;
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    scheduler.cancel(id1); // Cancel the top task
    
    std::this_thread::sleep_for(std::chrono::milliseconds(220));
    Expect(executed.load() == 2, "Re-evaluated wait time after cancel");
}

void TestCancelExecutingTask() {
    ThreadPool pool(2, 1024);
    TimerScheduler scheduler(pool);
    scheduler.start();
    std::atomic<int> counter{0};
    
    auto id = scheduler.schedule_every(std::chrono::milliseconds(50), [&]{
        counter++;
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // occupy the worker
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(70)); // let it enter the callback
    Expect(counter.load() == 1, "Task is executing");
    
    bool cancelled = scheduler.cancel(id); // cancel while executing
    Expect(cancelled, "Cancel executing task returns true");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // wait for it to finish and NOT reschedule
    int current = counter.load();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Expect(counter.load() == current, "Cancelled executing task did not reschedule");
}

int main() {
    TestCancelOneShot();
    TestPeriodicTask();
    TestCancelTopTaskReevaluates();
    TestCancelExecutingTask();
    
    if (g_failed == 0) {
        std::cout << "ALL PHASE 2 PASSED\n";
        return 0;
    } else {
        std::cout << g_failed << " FAILED\n";
        return 1;
    }
}
