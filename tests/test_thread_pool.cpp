// test_thread_pool.cpp — tests for the worker pool.
//
// Concurrency tests are inherently a little timing-based, so these are written
// to be robust: they assert facts that must hold regardless of scheduling
// (e.g. "every task ran exactly once") and only loosely assert timing facts
// (e.g. "at least two tasks overlapped").
#include <atomic>
#include <chrono>
#include <stdexcept>
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

// A task that throws must not crash a worker: the pool keeps running and every
// other task still completes.
void test_throwing_task_does_not_crash_pool() {
    std::atomic<int> ran{0};
    {
        ThreadPool pool(2);
        pool.submit([] { throw std::runtime_error("intentional test exception"); });
        for (int i = 0; i < 100; ++i) {
            pool.submit([&ran] { ran.fetch_add(1); });
        }
    }  // destructor drains and joins
    CHECK_EQ(ran.load(), 100);  // all normal tasks ran despite the throwing one
}

// A bounded queue rejects submissions once it is full, returning false instead
// of growing without limit.
void test_bounded_queue_rejects_when_full() {
    ThreadPool pool(1, 2);  // 1 worker, queue capacity 2
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};

    // Occupy the single worker with a task that blocks until released.
    CHECK(pool.submit([&] {
        started.store(true);
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }));
    while (!started.load()) {  // wait until the worker is actually busy
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Worker is busy; now fill the 2-slot queue, then expect a rejection.
    CHECK(pool.submit([] {}));    // queued (1/2)
    CHECK(pool.submit([] {}));    // queued (2/2)
    CHECK(!pool.submit([] {}));   // full -> rejected

    release.store(true);  // let the worker drain so the destructor can finish
}

int main() {
    RUN(test_every_task_runs_once);
    RUN(test_tasks_overlap);
    RUN(test_empty_pool_shuts_down_cleanly);
    RUN(test_size_reporting);
    RUN(test_throwing_task_does_not_crash_pool);
    RUN(test_bounded_queue_rejects_when_full);
    return REPORT();
}
