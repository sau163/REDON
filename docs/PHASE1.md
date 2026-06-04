# Phase 1 — explained

Phase 1 is the smallest version of Redon that is still a *real* networked
database. This document explains every moving part so you understand not just
*what* the code does, but *why*.

## The big picture

```
   redon-cli  (you type here)                 redon-server
   ──────────                                  ────────────
   "SET name Saurabh\n"  ───── TCP socket ────►  read the line
                                                 parse it -> {SET, name, Saurabh}
                                                 run it   -> storage[name] = Saurabh
   "OK\n"               ◄───── TCP socket ─────  send the reply
```

There are four ideas, and one file owns each idea:

1. **Networking** (`net.h`, `server.cpp`) — how two programs talk over a socket.
2. **The protocol** (`command.cpp`) — the agreed text format for requests/replies.
3. **Storage** (`storage.cpp`) — the actual key→value map (the "database").
4. **Glue** (`main.cpp`, `client_main.cpp`) — the entry points that wire it up.

---

## 1. What is a TCP socket?

A **socket** is a software "phone line" between two programs. The pattern a
server follows is always the same four steps:

| Step | Function | Plain-English meaning |
|------|----------|-----------------------|
| 1 | `socket()` | Buy a phone |
| 2 | `bind()`   | Get a phone number (an IP + port, e.g. `127.0.0.1:6380`) |
| 3 | `listen()` | Turn the ringer on |
| 4 | `accept()` | Pick up when someone calls |

After `accept()` you get a *second* socket that represents that one caller. You
`recv()` (listen) and `send()` (talk) on it until they hang up.

The client is simpler: `socket()` then `connect()` to the server's number, then
`send()`/`recv()`.

**Why `net.h` exists:** Windows and Linux have *almost* the same socket
functions, but with annoying differences (Windows calls them through a library
that must be started with `WSAStartup`, uses `SOCKET` instead of `int`, and
`closesocket()` instead of `close()`). `net.h` hides those differences behind
one set of names so `server.cpp` and `client_main.cpp` stay clean.

---

## 2. The protocol (the "language" client and server share)

A protocol is just an agreed format. Ours is deliberately simple — **one command
per line**, ending in `\n`:

```
SET name Saurabh
GET name
DEL name
EXISTS name
PING
QUIT
```

`command.cpp` does two jobs:

- **Parse**: split `"SET name Saurabh"` into a `Command{ verb=SET, key="name",
  value="Saurabh" }`. Note the value keeps its spaces, so
  `SET title Senior Engineer` works.
- **Execute**: look at the verb and call the matching storage method, then format
  a reply string. Replies imitate the real `redis-cli`:
  - `OK` for a successful `SET`
  - the value, or `(nil)`, for `GET`
  - `(integer) N` for `DEL`/`EXISTS` (how many keys matched)
  - `ERR ...` for anything malformed

Keeping parse/execute in one place means **when we add new commands in later
phases, we only touch this one file.**

---

## 3. The storage engine

This is the database's heart, and in Phase 1 it is wonderfully small: a
`std::unordered_map<std::string, std::string>` — a hash table giving **O(1)**
average `set`/`get`/`del`.

Two design choices worth understanding:

- **It is wrapped in a class** (`Storage`) instead of using the map directly.
  Why? Because in later phases `set()` must *also* write to the WAL (Phase 3) and
  forward to replicas (Phase 5). Hiding the map behind methods means callers
  never change — only the inside of `set()` grows.

- **It is already thread-safe.** Every method locks a `std::mutex`. Phase 1 only
  serves one client at a time so it isn't strictly needed *yet*, but Phase 2 adds
  many threads hitting the same map simultaneously — and an `unordered_map` being
  read and written by two threads at once is undefined behavior (a crash). Adding
  the lock now means Phase 2 doesn't have to rewrite this file. The lock is
  cheap and correct; this is how real engines are built.

---

## 4. The server loop

`server.cpp` ties it together:

```
start():
    create + bind + listen on the port
    forever:
        client = accept()           # wait for a connection
        handle_client(client)       # serve it fully, then loop for the next
```

`handle_client` reads bytes, splits them into complete lines (TCP does not
guarantee one `recv()` == one line — data can arrive split or merged, so we keep
a small buffer and pull out whole lines), runs each line through `command.cpp`,
and sends the reply back.

> **Phase 1 limitation (on purpose):** the server handles **one client at a
> time**. The next caller waits until the current one disconnects. That's fine
> for learning, and **Phase 2** removes the limit by handing each accepted client
> to a worker thread from a pool. The code is structured so Phase 2 only has to
> wrap the `handle_client(...)` call in a thread — nothing else changes.

---

## How the pieces map to skills (for your resume / interviews)

| File | Skill demonstrated |
|------|--------------------|
| `net.h`, `server.cpp` | Berkeley/Winsock socket programming, TCP basics |
| `command.cpp` | Protocol design, parsing |
| `storage.cpp` | Data structures (hash maps), encapsulation, thread-safety |
| `CMakeLists.txt` | Cross-platform builds |
| `tests/` | Writing tests for a stateful component |

## What to try next

1. Build and run it (see the [README](../README.md)).
2. Open **two** `redon-cli` windows at once and notice the second one blocks
   until the first disconnects — that is the Phase 1 limitation you'll fix in
   Phase 2.
3. Read `storage.cpp` and `command.cpp` first; they are the easiest entry points.
