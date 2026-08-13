// TimerManager（TimerScheduler + ThreadPool 轮子）与 EventLoop 定时器测试。
#include "base/TimerManager.h"
#include "net/EventLoop.h"
#include "net/EventLoopThread.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <thread>

int main() {
    using namespace std::chrono_literals;

    // 1) runAfter：一次性定时器触发一次
    {
        minireactor::TimerManager timers(2, 256);
        timers.start();
        std::promise<void> fired;
        auto future = fired.get_future();
        timers.runAfter(50ms, [&fired] { fired.set_value(); });
        assert(future.wait_for(2s) == std::future_status::ready);
        timers.stop();
    }

    // 2) runEvery：周期定时器；cancel 后不再触发
    {
        minireactor::TimerManager timers(2, 256);
        timers.start();
        std::atomic<int> count{0};
        std::promise<void> enough;
        auto future = enough.get_future();
        const auto id = timers.runEvery(20ms, [&] {
            if (count.fetch_add(1) + 1 == 3) {
                enough.set_value();
            }
        });
        assert(future.wait_for(2s) == std::future_status::ready);
        assert(timers.cancel(id));
        const int snapshot = count.load();
        std::this_thread::sleep_for(100ms);
        assert(count.load() == snapshot);
        timers.stop();
    }

    // 3) 触发前 cancel：回调不会执行
    {
        minireactor::TimerManager timers(2, 256);
        timers.start();
        std::atomic<int> count{0};
        const auto id = timers.runAfter(30ms, [&count] { count.fetch_add(1); });
        assert(timers.cancel(id));
        std::this_thread::sleep_for(150ms);
        assert(count.load() == 0);
        timers.stop();
    }

    // 4) EventLoop::runAfter：回调被 marshal 回 loop 线程执行
    {
        minireactor::EventLoopThread thread;
        minireactor::EventLoop* loop = thread.startLoop();

        std::promise<std::thread::id> loopThreadId;
        loop->queueInLoop([&loopThreadId] { loopThreadId.set_value(std::this_thread::get_id()); });
        const std::thread::id expected = loopThreadId.get_future().get();

        std::promise<std::thread::id> timerThreadId;
        loop->runAfter(0.05, [&timerThreadId] {
            timerThreadId.set_value(std::this_thread::get_id());
        });
        const std::thread::id actual = timerThreadId.get_future().get();
        assert(actual == expected);

        loop->quit();
    }

    // 5) EventLoop::runEvery + cancel
    {
        minireactor::EventLoopThread thread;
        minireactor::EventLoop* loop = thread.startLoop();
        std::atomic<int> count{0};
        std::promise<void> enough;
        auto future = enough.get_future();
        const auto id = loop->runEvery(0.02, [&] {
            if (count.fetch_add(1) + 1 >= 3) {
                enough.set_value();
            }
        });
        assert(future.wait_for(2s) == std::future_status::ready);
        loop->cancel(id);
        std::this_thread::sleep_for(100ms);
        loop->quit();
    }

    return 0;
}
