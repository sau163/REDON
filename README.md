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
| 2 | Thread pool | Serving thousands of clients at once |
| 3 | Write-Ahead Log (WAL) | Surviving a crash without losing data |
| 4 | LRU eviction | Staying within a memory budget |
| 5 | Replication | Surviving a whole machine dying |
| 6 | Raft leader election | Agreeing who is in charge (consensus) |
| 7 | Sharding | Storing more data than one machine holds |
| 8 | RocksDB backend | Millions of keys, fast restart |
| 9 | Metrics dashboard | Seeing what the system is doing |

> **You are here: Phase 1 is complete.** See [docs/PHASE1.md](docs/PHASE1.md)
> for a line-by-line explanation of how it works.

---

## What Phase 1 gives you

Two programs:

- **`redon-server`** — listens on a TCP port and stores keys in memory.
- **`redon-cli`** — a tiny client you can type commands into (so you don't need
  `telnet` or `netcat` installed).

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

The two executables land in `build/` (exact path is printed at the end of the
build; on Windows with Visual Studio it is `build/Release/`).

## Run it

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
if you also have Redis installed). Override it:

```sh
redon-server 7000           # server listens on port 7000
redon-cli 127.0.0.1 7000    # client connects to that port
```

## Project layout

```
Redon/
├── src/
│   ├── net.h          # cross-platform socket helpers (Winsock <-> POSIX)
│   ├── storage.h/.cpp # the key-value storage engine (the "database" itself)
│   ├── command.h/.cpp # parse a line of text into a command and run it
│   ├── server.h/.cpp  # the TCP server: accept clients, read/reply in a loop
│   ├── main.cpp       # entry point for redon-server
│   └── client_main.cpp# entry point for redon-cli
├── tests/
│   └── test_storage.cpp  # self-checking tests for the storage engine
├── docs/PHASE1.md     # how Phase 1 works, explained
└── CMakeLists.txt     # build configuration
```
