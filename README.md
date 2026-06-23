# Redon — a distributed key-value store (built from scratch in C++)

Redon is a learning project that re-implements the core ideas behind real
databases like **Redis** and **etcd**, one layer at a time. You talk to it over
the network and ask it to store and retrieve data by key:

```
SET name Saurabh
GET name        ->  Saurabh
```

The project is built in **phases**. Each phase solves one hard, famous problem
that real databases must solve. You can run and use the project after *every*
phase — it is always a working program, just with more capabilities each time.

| Phase | What it adds | The hard problem it solves |
|------:|--------------|----------------------------|
| **1** ✅ | TCP server, `SET`/`GET`/`DEL`/`EXISTS`, in-memory storage | Talking to clients over a network |
| **2** ✅ | Thread pool | Serving thousands of clients at once |
| **3** ✅ | Write-Ahead Log (WAL) | Surviving a crash without losing data |
| **4** ✅ | LRU eviction | Staying within a memory budget |
| **5** ✅ | Replication | Surviving a whole machine dying |
| **6** ✅ | Raft leader election | Agreeing who is in charge (consensus) |
| **7** ✅ | Sharding | Storing more data than one machine holds |
| **8** ✅ | On-disk storage engine | Values on disk (beyond RAM), fast restart |
| **9** ✅ | Metrics / monitoring | Seeing what the system is doing |

> **All nine phases are complete.** Redon serves many clients concurrently,
> persists data, bounds memory with an LRU cache, replicates to followers, elects
> a leader via Raft, shards the keyspace across machines, can store values on disk
> beyond RAM, and exposes Prometheus metrics. Each phase was implemented,
> adversarially reviewed, hardened, tested, and explained — see the per-phase
> walkthroughs: [1](docs/PHASE1.md) · [2](docs/PHASE2.md) · [3](docs/PHASE3.md) ·
> [4](docs/PHASE4.md) · [5](docs/PHASE5.md) · [6](docs/PHASE6.md) ·
> [7](docs/PHASE7.md) · [8](docs/PHASE8.md) · [9](docs/PHASE9.md).

---

## What you get

Four programs:

- **`redon-server`** — listens on a TCP port and stores keys, serving many
  clients at once via a thread pool (Phase 2) and persisting writes to a
  Write-Ahead Log so they survive a crash (Phase 3).
- **`redon-cli`** — a tiny client you can type commands into (so you don't need
  `telnet` or `netcat` installed).
- **`redon-web`** — a small HTTP gateway that serves a **browser UI** and bridges
  it to the server, so anyone can use Redon from a web page (no terminal needed).
- **`redon-bench`** — a concurrent load generator that opens many connections at
  once and reports throughput/latency (and verifies correctness under load).

### Commands

| Command | Example | Reply |
|---------|---------|-------|
| `SET key value` | `SET name Saurabh` | `OK` |
| `GET key` | `GET name` | `Saurabh` (or `(nil)` if missing) |
| `DEL key` | `DEL name` | `(integer) 1` (number of keys removed) |
| `EXISTS key` | `EXISTS name` | `(integer) 1` or `(integer) 0` |
| `PING` | `PING` | `PONG` |
| `QUIT` | `QUIT` | closes the connection |

`value` may contain spaces: `SET title Senior Software Engineer` stores the
whole phrase.

---

## Build it

You need **CMake** and a **C++17 compiler** (Visual Studio 2022 on Windows, or
GCC/Clang on Linux/macOS). From the project root:

```sh
cmake -S . -B build
cmake --build build --config Release
```

The executables land in `build/` (exact path is printed at the end of the
build; on Windows with Visual Studio it is `build/Release/`).

## Use it in your browser (easiest — no terminal)

Start the server **and** the web UI with one command, then open the page:

```sh
# Windows
powershell -ExecutionPolicy Bypass -File scripts\run.ps1
# Linux/macOS
./scripts/run.sh
```

This builds (if needed), starts `redon-server` + `redon-web`, and opens
**http://127.0.0.1:8080** — a dark-mode web terminal with an input box,
quick-command buttons, a live output log, and a stats strip (keys, hit rate,
uptime…) that refreshes from `INFO`. See [docs/WEB.md](docs/WEB.md).

### …or with Docker (runs anywhere, nothing to install but Docker)

```sh
docker build -t redon .
docker run --rm -p 8080:8080 -p 9090:9090 redon
# open http://localhost:8080   (Prometheus metrics on :9090/metrics)
```

### …or hand someone a zip (no toolchain required)

```sh
powershell -ExecutionPolicy Bypass -File scripts\package.ps1
# produces dist\redon-win64.zip — unzip anywhere, double-click start.bat
```

## Run it (from the terminal)

Open **two terminals**.

**Terminal 1 — start the server:**
```sh
./build/Release/redon-server          # Windows
./build/redon-server                  # Linux/macOS
```
It prints `Redon server listening on 127.0.0.1:6380`.

**Terminal 2 — start the client and type commands:**
```sh
./build/Release/redon-cli             # Windows
./build/redon-cli                     # Linux/macOS
```
```
> SET name Saurabh
OK
> GET name
Saurabh
> EXISTS name
(integer) 1
> DEL name
(integer) 1
> GET name
(nil)
> QUIT
```

The default port is **6380** (Redis uses 6379 — we pick 6380 to avoid clashing
if you also have Redis installed). Override the host, port, and worker-thread
count:

```sh
redon-server 7000                       # listen on port 7000, default workers
redon-server 127.0.0.1 7000 1024        # ...with 1024 workers (for many clients)
redon-server 127.0.0.1 7000 64 my.wal   # ...with a custom WAL file
redon-server 127.0.0.1 7000 64 none     # ...with persistence OFF (in-memory only)
redon-server 127.0.0.1 7000 64 my.wal 60 # ...disconnect clients idle > 60s
redon-server 127.0.0.1 7000 64 my.wal 300 1000 # ...cap the cache at 1000 keys
redon-cli 127.0.0.1 7000                # client connects to that port
```

The 5th argument is the **idle timeout** in seconds (default 300, `0` to
disable) — a client that sends nothing for that long is disconnected, like
Redis's `timeout`. Connections also use TCP keepalive to detect dead peers.
The 6th argument is the **LRU capacity** in keys (default `0` = unbounded, like
Redis's `maxmemory`): past it, the least-recently-used key is evicted. See
[docs/PHASE4.md](docs/PHASE4.md).

## Replication (survives a machine dying)

A **leader** streams every write to one or more read-only **followers** (Phase 5):

```sh
# start followers (read-only, in-memory), then a leader that replicates to them
redon-server 127.0.0.1 6381 8 none 0 0 --replica
redon-server 127.0.0.1 6380 8 redon.wal 300 0 --follower 127.0.0.1:6381
```

`SET` on the leader appears on the followers within milliseconds; `SET` on a
follower is rejected (`ERR READONLY`). Replication is asynchronous (low latency,
small loss window on a leader crash) and a (re)connecting follower is brought up
to date with a full snapshot sync. See [docs/PHASE5.md](docs/PHASE5.md).

## Raft leader election (automatic failover)

A cluster of nodes **elects its own leader** and re-elects automatically when the
leader dies — the leader-election half of the Raft consensus algorithm (Phase 6):

```sh
# three nodes, each listing the OTHER two as --raft peers
redon-server 127.0.0.1 6510 4 none 0 0 --raft 127.0.0.1:6511 --raft 127.0.0.1:6512
redon-server 127.0.0.1 6511 4 none 0 0 --raft 127.0.0.1:6510 --raft 127.0.0.1:6512
redon-server 127.0.0.1 6512 4 none 0 0 --raft 127.0.0.1:6510 --raft 127.0.0.1:6511
```

`ROLE` reports each node's `role`/`term`/`leader`; only the elected leader accepts
writes (others reply `ERR NOTLEADER <addr>`). **Kill the leader** and within ~1s
the survivors elect a new one. This phase implements leader election (consensus on
*who* leads); carrying the data through Raft's log is the documented next step. See
[docs/PHASE6.md](docs/PHASE6.md).

## Sharding (storing more than one machine holds)

A **router** splits the keyspace across several **shard** servers by
`hash(key) % N`, so the cluster holds far more than any single machine (Phase 7):

```sh
# three plain shard servers, then a router in front of them
redon-server 127.0.0.1 6701 4 none
redon-server 127.0.0.1 6702 4 none
redon-server 127.0.0.1 6703 4 none
redon-server 127.0.0.1 6700 4 none 0 0 --shard 127.0.0.1:6701 --shard 127.0.0.1:6702 --shard 127.0.0.1:6703
```

Talk only to the router (6700): `SET user:1 Bob` lands on one shard, `GET user:1`
is routed back to it. Different keys spread evenly across the shards. (`% N`
reshuffles everything if you change N — consistent hashing is the next step; and
each shard is a single point of failure until you replicate it.) See
[docs/PHASE7.md](docs/PHASE7.md).

## On-disk storage engine (data bigger than RAM)

By default values live in RAM. Pass `--disk <path>` to use a from-scratch
**log-structured** storage engine that keeps **values on disk** with only a small
key→offset index in memory (Phase 8):

```sh
redon-server 127.0.0.1 6380 4 none 0 0 --disk redon.db
```

Write keys, kill the server, restart with the same `--disk redon.db` — the data is
back (no WAL needed; the data file *is* the database), and recovery rebuilds the
index quickly by skipping over the value bytes. `cat redon.db` shows the records.
See [docs/PHASE8.md](docs/PHASE8.md).

## Monitoring (Prometheus + INFO)

`--metrics-port <n>` serves Prometheus metrics, and the `INFO` command gives a
quick human view (Phase 9):

```sh
redon-server 127.0.0.1 6380 4 none 0 0 --metrics-port 9090
redon-cli 6380          # INFO -> uptime_s=.. commands=.. get=.. hit_rate=.. keys=..
curl http://127.0.0.1:9090/metrics    # Prometheus exposition format
```

Counters (commands by verb, hit rate, errors, connections, latency) are lock-free
atomics, so they don't slow the hot path. Point Prometheus at `/metrics` and graph
it in Grafana. See [docs/PHASE9.md](docs/PHASE9.md).

The worker-thread count is how many clients are served **at the same time**
(defaults to your CPU's thread count). See [docs/PHASE2.md](docs/PHASE2.md).

## Persistence (it survives a crash)

By default the server writes every `SET`/`DEL` to a **Write-Ahead Log**
(`redon.wal`) and replays it on startup, so your data survives a restart — even
an abrupt one:

```sh
redon-server &                 # WAL on by default (redon.wal)
redon-cli  # SET name Saurabh ; QUIT
# ...kill the server however you like...
redon-server &                 # replays redon.wal on startup
redon-cli  # GET name  ->  Saurabh     (it came back!)
```

Pass `none` as the 4th argument to disable persistence. See
[docs/PHASE3.md](docs/PHASE3.md) for how the WAL works.

## Benchmark it

With a server running, drive concurrent load:

```sh
redon-bench 127.0.0.1 6380 50 1000   # 50 connections x 1000 SET+GET iterations
```

It prints throughput and latency and exits non-zero if any reply was wrong —
so it doubles as a concurrency correctness check. Throughput scales with the
server's worker count up to roughly your core count.

## Project layout

```
Redon/
├── src/
│   ├── net.h             # cross-platform socket helpers (Winsock <-> POSIX)
│   ├── storage.h/.cpp    # storage front-end: O(1) LRU cache (P4) or disk backend
│   ├── disk_store.h/.cpp # on-disk log-structured engine, values off RAM (Phase 8)
│   ├── command.h/.cpp    # parse a line of text into a command and run it
│   ├── thread_pool.h/.cpp# worker pool: queue + mutex + condition_variable (Phase 2)
│   ├── wal.h/.cpp        # Write-Ahead Log: append + replay for durability (Phase 3)
│   ├── replication.h/.cpp# leader-side replication to follower nodes (Phase 5)
│   ├── raft.h/.cpp       # Raft leader election (terms, votes, quorum) (Phase 6)
│   ├── router.h/.cpp     # sharding: hash(key) %% N, forward to the shard (Phase 7)
│   ├── metrics.h/.cpp    # atomic counters, INFO, Prometheus /metrics (Phase 9)
│   ├── server.h/.cpp     # TCP server: accept clients, hand each to the pool
│   ├── web.h/.cpp        # browser-UI HTTP gateway (bridges browser <-> server)
│   ├── main.cpp          # entry point for redon-server
│   ├── web_main.cpp      # entry point for redon-web
│   └── client_main.cpp   # entry point for redon-cli
├── bench/
│   └── redon_bench.cpp   # concurrent load generator (redon-bench)
├── tests/
│   ├── test_storage.cpp     # tests for the storage engine (+ concurrency)
│   ├── test_command.cpp     # tests for the protocol (parse + execute)
│   ├── test_thread_pool.cpp # tests for the worker pool
│   └── test_wal.cpp         # tests for the Write-Ahead Log (replay + recovery)
├── docs/
│   ├── PHASE1.md         # how Phase 1 works, explained
│   ├── PHASE2.md         # how Phase 2 (concurrency) works, explained
│   ├── PHASE3.md         # how Phase 3 (persistence) works, explained
│   ├── PHASE4.md         # how Phase 4 (LRU eviction) works, explained
│   ├── PHASE5.md         # how Phase 5 (replication) works, explained
│   ├── PHASE6.md         # how Phase 6 (Raft leader election) works, explained
│   ├── PHASE7.md         # how Phase 7 (sharding) works, explained
│   ├── PHASE8.md         # how Phase 8 (on-disk storage engine) works, explained
│   ├── PHASE9.md         # how Phase 9 (metrics / monitoring) works, explained
│   └── WEB.md            # the browser UI gateway (redon-web), explained
├── scripts/
│   ├── run.ps1 / run.sh  # build + start server & web UI locally
│   └── package.ps1       # build Release and zip a shareable bundle
├── docker/entrypoint.sh  # starts server + web UI inside the container
├── Dockerfile            # one-command containerized build & run
└── CMakeLists.txt        # build configuration
```
