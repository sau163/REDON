# Phase 2 — explained (concurrency)

Phase 1 served **one client at a time**: while you talked to the server, everyone
else waited in line. Phase 2 removes that limit with a **thread pool**, so many
clients are served *simultaneously*. This is the project's first taste of
**concurrency** — the art (and hazard) of doing several things at once correctly.

## The one change that matters

In Phase 1 the accept loop was:

```cpp
for (;;) {
    client = accept();      // wait for a caller
    handle_client(client);  // serve them FULLY before looping  <-- the bottleneck
    close(client);
}
```

In Phase 2 it becomes:

```cpp
ThreadPool pool(num_workers);
for (;;) {
    client = accept();                          // wait for a caller
    pool.submit([this, client] {                // hand them to a worker thread...
        handle_client(client);
        close(client);
    });                                          // ...and immediately accept the next
}
```

The main thread now does almost nothing but `accept()` and hand the connection
off. The actual work happens on **worker threads**, several at once. That's the
whole idea — everything else is making it *safe*.

## What is a thread?

A **thread** is an independent flow of execution inside your program. A normal
program has one (it runs `main` top to bottom). With threads, several flows run
at the same time — truly in parallel if you have multiple CPU cores. Your laptop
has, say, 16 logical cores, so up to 16 threads can literally run at the same
instant.

The catch: threads in the same program **share memory**. If two threads touch the
same data at the same time and at least one is writing, you get a **data race** —
undefined behavior, meaning anything from a wrong answer to a crash. Managing
that shared access safely is the entire skill of concurrency.

## The ThreadPool — `thread_pool.h` / `.cpp`

Why a *pool* of threads instead of spawning a fresh thread per client? Creating
and destroying threads is expensive, and an unbounded number of them could
exhaust the machine. A **pool** creates a fixed set of workers once, then feeds
them a never-ending stream of tasks. It's the **producer/consumer** pattern:

```
          submit(task)                          worker threads
 main  ───────────────►  ┌──────────────┐   ┌─► [w1] runs a task
 thread                  │  task queue   │   ├─► [w2] runs a task
 (producer)              │  [t][t][t]... │───┼─► [w3] sleeps (queue empty)
                         └──────────────┘   └─► [w4] runs a task
                          (consumers pull from the front)
```

Three standard-library tools make this safe and efficient:

| Tool | Role | Plain meaning |
|------|------|---------------|
| `std::thread` | the workers | the independent flows of execution |
| `std::mutex` | guards the queue | "only one thread may touch the queue at a time" |
| `std::condition_variable` | the workers' alarm clock | lets idle workers **sleep** (0% CPU) until a task arrives, instead of busy-spinning |

The worker loop is the heart of it:

```cpp
void ThreadPool::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });  // sleep until work
            if (tasks_.empty()) return;        // woken only to shut down -> exit
            task = std::move(tasks_.front());
            tasks_.pop();
        }                                       // <- lock released HERE
        task();                                 // run the task WITHOUT the lock
    }
}
```

Two subtleties worth really understanding:

1. **`cv_.wait(lock, predicate)`** atomically *unlocks* the mutex and puts the
   thread to sleep, then *re-locks* it when woken. The predicate
   (`stop_ || !tasks_.empty()`) is re-checked on every wake — this defends
   against **spurious wakeups** (a thread can wake for no reason) and makes the
   logic robust.
2. **The task runs after the lock is released** (`}` closes the scope before
   `task()`). If we held the lock while running the task, only one worker could
   ever run at a time — defeating the whole purpose. Releasing it first is what
   lets the workers run in parallel.

`submit()` is the mirror image: lock, push the task, unlock, then
`cv_.notify_one()` to wake exactly one sleeping worker. The destructor sets
`stop_`, wakes everyone with `notify_all()`, and `join()`s every thread (waits
for them to finish) — so no thread is ever leaked.

## Why we needed almost no new locking

Here's the payoff from how Phase 1 was built. A worker thread running
`handle_client` touches exactly two kinds of data:

- **Its own** socket and buffers — local to its task, so no other thread can see
  them. Safe by construction.
- **The shared `Storage`** — and that was *already* mutex-protected back in
  Phase 1 (the "already thread-safe" comment). So concurrent `SET`/`GET` from
  many workers is already safe.

The only genuinely shared, previously-unprotected resource was **stdout**: if
several workers printed log lines at once, the characters would interleave into
gibberish. So we added one small `log()` method that takes a mutex around the
write, plus a `std::atomic<int>` counter for the live-connection count (atomics
are the lightweight way to share a single number across threads without a full
mutex). That's the entire concurrency surface — small precisely because the data
layer was designed for it in advance.

## The model's limitation (be honest about it)

Each task handles **one connection for its entire lifetime**, and a worker is
busy for that whole time (mostly blocked waiting on `recv`). So a pool of N
workers serves **N connections at once**; connection N+1 is accepted but its
task waits in the queue until a worker frees up. Consequences:

- To serve 1000+ truly simultaneous clients you raise the worker count:
  `redon-server 127.0.0.1 6380 1024`. Each thread costs ~1 MB of stack, so 1000
  threads ≈ 1 GB — feasible but heavy.
- A single idle client that connects but never sends anything ties up a worker
  forever (a "slowloris"). A few of those could exhaust a small pool.

The production-grade answer to both is **asynchronous I/O** (one thread juggling
thousands of connections via `epoll`/`IOCP`), which trades simplicity for scale —
a topic for a much later phase. The thread pool is the right tool to *learn* the
concurrency primitives first.

## Proving it works — `redon-bench`

Tests for concurrent code must assert facts that hold regardless of timing:

- **`tests/test_thread_pool.cpp`** checks the pool itself: every submitted task
  runs *exactly once* (1000 tasks, 4 workers), tasks genuinely *overlap* in time
  (high-water concurrency ≥ 2), and an empty pool shuts down cleanly.
- **`bench/redon-bench`** is an end-to-end stress test: it opens many connections
  at once and has each verify that every `GET` returns the value its own `SET`
  stored. Running 50 connections × 1000 iterations = **100,000 requests with
  zero errors** is strong evidence the shared store stays correct under real
  concurrent load. It also reports throughput, which scaled ~8× from 1 to 16
  workers on a 16-core machine before plateauing — exactly the shape you'd
  expect.

## Skills this phase demonstrates (for interviews)

- `std::thread`, `std::mutex`, `std::condition_variable`, `std::atomic`
- The producer/consumer / thread-pool pattern and *why* it beats thread-per-task
- Data races and how encapsulation + a single lock prevent them
- Writing tests and benchmarks for non-deterministic, concurrent code
- Knowing the limits of the blocking model and what comes after it (async I/O)

## What to try yourself

1. Run `redon-bench` against `redon-server` started with different thread counts
   and watch throughput climb, then plateau around your core count.
2. Start the server with `... 2` (two workers), open three `redon-cli` windows,
   and watch the third one's commands stall until you `QUIT` one of the first two.
   That is the pool-size limit, live.
