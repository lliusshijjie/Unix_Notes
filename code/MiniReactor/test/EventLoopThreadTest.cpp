#include "net/EventLoop.h"
#include "net/EventLoopThread.h"
#include "net/EventLoopThreadPool.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>

int main() {
    using namespace std::chrono_literals;

    minireactor::EventLoopThread worker;
    minireactor::EventLoop* workerLoop = worker.startLoop();
    std::promise<void> workerRan;
    auto workerFuture = workerRan.get_future();
    workerLoop->queueInLoop([&] { workerRan.set_value(); });
    assert(workerFuture.wait_for(2s) == std::future_status::ready);
    workerLoop->quit();

    minireactor::EventLoop baseLoop;
    minireactor::EventLoopThreadPool pool(&baseLoop, 2);
    pool.start();
    minireactor::EventLoop* first = pool.getNextLoop();
    minireactor::EventLoop* second = pool.getNextLoop();
    assert(first != &baseLoop);
    assert(second != &baseLoop);
    assert(first != second);

    std::atomic<int> executed{0};
    std::promise<void> bothRan;
    auto bothFuture = bothRan.get_future();
    auto recordExecution = [&] {
        if (executed.fetch_add(1) + 1 == 2) {
            bothRan.set_value();
        }
    };
    first->queueInLoop(recordExecution);
    second->queueInLoop(recordExecution);
    assert(bothFuture.wait_for(2s) == std::future_status::ready);
    first->quit();
    second->quit();
}
