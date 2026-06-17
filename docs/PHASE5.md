# Phase 5 — explained (replication)

This is the project's inflection point: Redon becomes **distributed**. Until now
one process held all the data — if that machine died, the service was down (and
without the WAL, the data was gone). Phase 5 keeps **copies on other machines**:
a **leader** holds the authoritative data and streams every write to one or more
**followers**. If the leader dies, a follower still has the data.

```
                 clients (writes + reads)
                        │
                        ▼
                  ┌──────────┐   stream every SET/DEL
                  │  LEADER  │──────────────┬───────────────┐
                  └──────────┘              ▼               ▼
                        ▲             ┌──────────┐    ┌──────────┐
                        │             │ FOLLOWER │    │ FOLLOWER │
                  reads (clients)     └──────────┘    └──────────┘
                                       reads only      reads only
```

## Roles

A node starts as a **leader** (default) or a **follower** (`--replica`):

```sh
redon-server 127.0.0.1 6381 8 none --replica                       # a follower
redon-server 127.0.0.1 6380 8 none --follower 127.0.0.1:6381       # a leader
```

- The **leader connects to** each follower (not the other way round) and pushes
  writes to it.
- A **follower** is **read-only to clients**: `GET` works, but `SET`/`DEL` from an
  ordinary client are rejected with `ERR READONLY`. Only the leader's stream
  writes to it.

## How a write travels

When a client does `SET name Saurabh` on the leader:

```
client → leader: SET name Saurabh
   leader applies it (memory + WAL)         (Phases 1–4, unchanged)
   leader ENQUEUES "SET name Saurabh" for each follower   ← Phase 5
   leader → client: OK
        ...meanwhile, asynchronously...
   each follower's sender thread sends "SET name Saurabh" → follower applies it
```

The hook is in `Storage::set`/`del` (and even LRU eviction), right next to the
WAL hook — under the **same lock**, so the order followers receive writes matches
the order the leader applied them.

## Asynchronous replication (and what it costs)

Replication is **asynchronous**: `Storage::set` just *enqueues* the write (fast,
under the lock) and a **background sender thread per follower** delivers it. This
matters because a follower could be slow or far away — if the leader *waited* for
each follower on every write (synchronous replication), one slow follower would
slow down *all* clients. Async keeps client latency independent of follower speed.

The trade-off, which you must be able to name:

> Because the leader replies `OK` **before** the write reaches the followers,
> there is a small window where a write is acknowledged but not yet replicated.
> If the leader crashes in that window, that write is lost on the followers. This
> is **eventual consistency** with a possible data-loss window — exactly Redis's
> default async replication.

(Synchronous or quorum replication closes that window at a latency cost; that's
the territory of Phase 6's Raft.)

## The `Replicator` ([replication.cpp](../src/replication.cpp))

For each follower the leader runs one `sender_loop` thread. Its life:

```
forever:
    connect to the follower
    send "__REPLSYNC__"            → follower clears its data, becomes our replica link
    SNAPSHOT + start streaming     (the atomic cut — see below)
    send the snapshot as SET commands
    stream queued writes; PING when idle
    on disconnect / stall: drop the backlog, wait, reconnect (full sync again)
```

Three ideas make it correct and robust:

### 1. Full sync on (re)connect

A follower that just connected (or reconnected after a crash) has **stale or no
data**. So the first thing the leader does is send a **snapshot** of all current
keys, after telling the follower (via the `__REPLSYNC__` handshake) to **clear**
itself first. Now the follower matches the leader, and live writes stream on top.
That's why, in the demo, a *restarted* follower gets all its data back.

### 2. The atomic "cut" (no missed or duplicated writes)

The hard part of replication: when the sender takes a snapshot and then starts
streaming, how do we guarantee no write slips through the gap (missed) or appears
in *both* the snapshot and the stream (duplicated)?

The answer is to make "take the snapshot" and "start streaming" **one atomic
step**, under the storage lock:

```cpp
snapshot = storage_->snapshot_locked([&]{          // holds the storage lock...
    link->queue.clear();
    link->streaming = true;                        // ...flips "streaming" on
});                                                // ...all in one critical section
```

A client write also runs under the storage lock and only enqueues *if streaming
is on*. So any write either **completed before the cut** (and is in the snapshot)
or **runs after the cut** (sees `streaming == true`, lands in the stream) — never
both, never neither. The single lock is the whole trick.

### 3. Heartbeats detect a dead follower

A subtle bug, which the first demo hit: if the leader is **idle** (no writes), its
sender just waits — so it never notices a follower died, and never reconnects to a
restarted one. The fix (also Redis's) is a **heartbeat**: every couple of seconds
an idle sender sends a `PING`. If the follower is gone, that send fails → the
sender reconnects and re-syncs. The heartbeat also keeps the link from being torn
down by the follower's idle timeout.

## Eviction replicates too

A neat detail: when the leader's LRU cache **evicts** a key, it sends a `DEL` to
the followers — just like it logs one to the WAL. So followers mirror the leader's
*bounded* key set. (Run followers with capacity 0 / unbounded; they stay bounded
by mirroring the leader, which controls exactly what exists.)

## What's guaranteed, and what isn't (honesty)

- ✅ A follower that is connected and caught up holds a **copy** of the leader's
  data; reads from it are served locally.
- ✅ A (re)connecting follower **converges** to the leader via a full sync.
- ✅ Replication order == leader apply order (same-lock hook).
- ⚠️ **Async = a data-loss window**: writes acked just before a leader crash may
  not have reached followers. No write is *corrupted*, but recent ones can be
  lost on failover.
- ⚠️ **No automatic failover.** If the leader dies, a follower has the data but
  nothing *promotes* it to leader automatically — that's **Phase 6 (Raft)**. For
  now you'd repoint clients/promote manually.
- ⚠️ **Full re-sync on every reconnect** (no incremental catch-up / partial
  resync like Redis's PSYNC). Simple and correct, but heavier for big datasets.
- ⚠️ **Followers should be in-memory** (`wal none`) and unbounded — they are
  replicas reset by the leader on each sync; a follower with its own WAL/capacity
  could diverge.
- ⚠️ **A narrow clear-then-sync window.** A follower clears its data the moment
  it accepts `__REPLSYNC__`, then receives the snapshot. If the leader crashes in
  the ~1-round-trip gap *between* those, the follower is left empty until a leader
  comes back to re-sync it. We keep the immediate clear because it guarantees the
  follower ends up *exactly* matching the leader (including an empty leader);
  buffering the whole snapshot to swap it in atomically would be the way to close
  the gap, at a memory cost.
- ⚠️ **Shutdown can lag a few seconds.** If a follower is stalled when the leader
  shuts down, a `send()` already in flight blocks until its 5 s timeout before the
  sender thread notices the stop. Harmless (the process normally just exits), but
  a fully responsive shutdown would need an interruptible send.

## Try it

```sh
# two followers, then a leader replicating to both
redon-server 127.0.0.1 6381 4 none 0 0 --replica
redon-server 127.0.0.1 6382 4 none 0 0 --replica
redon-server 127.0.0.1 6380 4 none 0 0 --follower 127.0.0.1:6381 --follower 127.0.0.1:6382
```
```
# on the leader (6380):
SET name Saurabh
# on a follower (6381):
GET name      ->  Saurabh        # replicated!
SET x 1       ->  ERR READONLY this node is a read-only replica
```
Kill a follower and restart it — within a few seconds the leader's heartbeat
notices, reconnects, and re-syncs it.

## New ideas this phase introduces

| Concept | Meaning here |
|---|---|
| **replication** | keep copies of the data on other nodes |
| **leader / follower** | one authoritative writer; read-only replicas |
| **asynchronous replication** | ack the client before followers confirm (lower latency, loss window) |
| **eventual consistency** | followers converge to the leader, slightly behind |
| **full sync (snapshot)** | bring a new/reconnected follower up to date |
| **the atomic cut** | snapshot + start-stream under one lock so no write is missed/duplicated |
| **heartbeat** | periodic PING to detect a dead peer and keep links alive |
| **no auto-failover (yet)** | a dead leader needs manual promotion — Raft (Phase 6) automates it |
