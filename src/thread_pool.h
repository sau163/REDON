// thread_pool.h — a fixed-size pool of worker threads that run queued tasks.
//
// This is the heart of Phase 2. It is the classic producer/consumer pattern:
//
//   * submit() is the PRODUCER: it pushes a task onto a shared queue and wakes
//     one sleeping worker.
//   * each worker thread is a CONSUMER: it sleeps on a condition variable until
//     a task is available, pops one, runs it, then goes back to sleep.
//
// Three standard-library tools cooperate to make that safe:
//   * std::mutex            — guards the shared queue so two threads never touch
//                             it at the same time.
//   * std::condition_variable — lets idle workers SLEEP (using no CPU) until
//                             there is work, instead of busy-spinning.
//   * std::thread           — the workers themselves.
//
// The pool owns its threads and joins all of them in the destructor, so there
// are never any leaked or detached threads.
#ifndef REDON_THREAD_POOL_H
#define REDON_THREAD_POOL_H

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace redon {

class ThreadPool {
public:
    // Start `num_threads` worker threads (forced to at least 1). `max_queue_size`
    // caps how many tasks may wait in the queue; 0 means unbounded. A bound is
    // what lets a server apply backpressure instead of accepting work forever.
    explicit ThreadPool(std::size_t num_threads, std::size_t max_queue_size = 0);

    // Stop accepting new work, let the workers finish everything already queued,
    // then join (wait for) every worker thread.
    ~ThreadPool();

    // A pool owns OS threads and a lock; copying it makes no sense.
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Hand a task to the pool for a free worker to run. Returns false (and does
    // NOT take the task) if the pool is shutting down or the queue is already at
    // its capacity — the caller then decides what to do (e.g. reject the client).
    bool submit(std::function<void()> task);

    // How many worker threads the pool runs.
    std::size_t size() const { return workers_.size(); }

private:
    // The function each worker thread runs for its whole life.
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;

    std::mutex mutex_;                  // guards tasks_ and stop_
    std::condition_variable cv_;        // workers wait here for new tasks
    bool stop_ = false;                // set true once, in the destructor
    std::size_t max_queue_size_ = 0;    // 0 = unbounded
};

}  // namespace redon

#endif  // REDON_THREAD_POOL_H
