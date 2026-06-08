// test_thread_pool.cpp — tests for the worker pool.
//
// Concurrency tests are inherently a little timing-based, so these are written
// to be robust: they assert facts that must hold regardless of scheduling
// (e.g. "every task ran exactly once") and only loosely assert timing facts
// (e.g. "at least two tasks overlapped").
#include <atomic>
#include <chrono>
#include <thread>

#include "test_util.h"
#include "thread_pool.h"

using redon::ThreadPool;

// Every submitted task must run exactly once, and the destructor must wait for
// them all (it drains the queue before joining).
void test_every_task_runs_once() {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(4);
        for (int i = 0; i < 1000; ++i) {
            pool.submit([&counter] { counter.fetch_add(1); });
        }
    }  // <- pool destructor drains the queue and joins all workers here
    CHECK_EQ(counter.load(), 1000);
}

// Tasks must actually run on several threads at the same time, not serially.
void test_tasks_overlap() {
    ThreadPool pool(4);
    std::atomic<int> running{0};      // how many tasks are running right now
    std::atomic<int> max_running{0};  // the most we ever saw running at once
    std::atomic<int> finished{0};

    for (int i = 0; i < 8; ++i) {
        pool.submit([&] {
            int now = running.fetch_add(1) + 1;
            // Record a new high-water mark for concurrency, if this is one.
            int prev = max_running.load();
            while (prev < now && !max_running.compare_exchange_weak(prev, now)) {
                // compare_exchange_weak reloads `prev` on failure; loop again.
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            running.fetch_sub(1);
            finished.fetch_add(1);
        });
    }

    // Wait for all eight tasks to complete.
    while (finished.load() < 8) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(max_running.load() >= 2);  // proof that real parallelism happened
}

// A pool that is created and destroyed with no work must not hang or crash.
void test_empty_pool_shuts_down_cleanly() {
    { ThreadPool pool(4); }
    CHECK(true);  // reaching here means the destructor returned
}

// size() reports the requested worker count, and 0 is bumped to 1.
void test_size_reporting() {
    ThreadPool pool(3);
    CHECK_EQ(pool.size(), static_cast<std::size_t>(3));
    ThreadPool one(0);
    CHECK_EQ(one.size(), static_cast<std::size_t>(1));
}

int main() {
    RUN(test_every_task_runs_once);
    RUN(test_tasks_overlap);
    RUN(test_empty_pool_shuts_down_cleanly);
    RUN(test_size_reporting);
    return REPORT();
}
