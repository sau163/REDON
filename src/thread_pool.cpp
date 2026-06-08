// thread_pool.cpp — implementation of the worker pool declared in thread_pool.h.
#include "thread_pool.h"

#include <utility>

namespace redon {

ThreadPool::ThreadPool(std::size_t num_threads) {
    if (num_threads == 0) {
        num_threads = 1;  // a pool with zero workers would never run anything
    }
    workers_.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i) {
        // Each thread runs worker_loop(). `this` lets it reach the shared queue.
        workers_.emplace_back([this] { worker_loop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        // Flip the stop flag under the lock, then wake everyone so each worker
        // re-checks its wait condition and drains/exits.
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : workers_) {
        if (t.joinable()) {
            t.join();  // wait for the worker to actually finish
        }
    }
}

void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            return;  // shutting down: don't accept more work
        }
        tasks_.push(std::move(task));
    }
    // Wake ONE worker. notify_one (not notify_all) because a single task can be
    // run by a single worker — waking them all would just cause a stampede.
    cv_.notify_one();
}

void ThreadPool::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Sleep until either there is a task to run or we are shutting down.
            // The predicate also protects against "spurious wakeups" — the
            // thread may wake for no reason, and the lambda re-checks the real
            // condition before proceeding.
            cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

            if (tasks_.empty()) {
                // The queue is empty AND we were woken — only happens when
                // stop_ is set. All work is drained, so this worker can exit.
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }
        // Run the task with the lock RELEASED, so other workers can pull the
        // next task and run in parallel while this one executes.
        task();
    }
}

}  // namespace redon
