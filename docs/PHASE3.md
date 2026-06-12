# Phase 3 — explained (persistence with a Write-Ahead Log)

Phases 1–2 kept everything in a `std::unordered_map` in **RAM**. RAM is
**volatile**: the instant the process dies — a crash, a power cut, a `Ctrl+C` —
every key vanishes. Phase 3 makes data **durable**: it survives a restart. The
tool is a **Write-Ahead Log (WAL)**, the same mechanism real databases
(PostgreSQL, SQLite, etcd, Kafka) use.

## The core idea in one rule

> **Before** changing data in memory, first append a description of the change to
> a file on disk and flush it. On startup, replay the file to rebuild memory.

That's it. "Write-ahead" = the write to the *log* happens *ahead of* the write to
*memory*. The payoff is a guarantee: by the time we reply `OK` to a client, the
change is already safely on disk, so no acknowledged write can ever be lost.

```
 client: SET name Saurabh
            │
            ▼
   1. append "SET name Saurabh" to wal file   ── flush to disk ──►  [ wal.log ]
   2. apply  map["name"] = "Saurabh"   (in RAM)
   3. reply  OK
            │
         ✗ CRASH at any point
            │
            ▼  restart
   replay wal.log line by line  ──►  map rebuilt exactly  ──►  data is back
```

## Why this actually guarantees durability

Think about *when* a crash can strike:

- **Crash after step 1, before step 2/3:** the client never got `OK` (we hadn't
  replied). On restart, replay re-applies the SET. The data is there — a *bonus*,
  not a loss.
- **Crash after step 3 (`OK` sent):** the record was written and flushed in step
  1, so replay restores it. **No acknowledged write is lost.**
- **Crash *during* step 1** (the file write is half-done): the record is torn.
  Replay detects the incomplete record and skips it (see "crash recovery" below).
  Since we never replied `OK`, dropping it is correct.

In every case the restored state is consistent with what clients were told. That
reasoning *is* the value of a WAL.

## The log format — `wal.cpp`

Each record is one line, **length-prefixed**:

```
SET <key_len> <val_len> <key bytes><val bytes>\n
DEL <key_len> <key bytes>\n
```

For example `SET name Saurabh` is stored as `SET 4 7 nameSaurabh\n`.

Why prefix with lengths instead of just writing `SET name Saurabh`? Two reasons:

1. **Unambiguous content.** A value can contain spaces (`SET title Senior
   Engineer`). With lengths, the reader reads *exactly* `val_len` bytes — spaces,
   tabs, anything — with no guessing about where the value ends.
2. **Crash detection.** The reader reads the declared number of bytes and then
   expects the trailing `\n`. If a crash cut the record short, that read hits
   end-of-file and **fails** — which is exactly how we detect and skip a torn
   final record.

## Replaying on startup

`Wal::replay_into(store)` opens the file and walks it record by record, calling
`store.set(...)` / `store.del(...)` for each. A **missing file is normal** — it
just means a brand-new database (replay does nothing and returns 0).

The order of operations at startup (`Server::setup_persistence`) matters:

```cpp
Wal wal(path);
wal.replay_into(store);     // 1. rebuild memory from the old log
wal.open_for_append();      // 2. open the file to add new records
store.attach_wal(&wal);     // 3. NOW start logging live writes
```

Step 3 happens **after** replay on purpose: during replay the store has *no* WAL
attached, so the `set`/`del` calls that rebuild memory don't get re-written back
into the log. (Otherwise every restart would duplicate the entire history.)

## The subtle correctness point: log *inside* the lock

Here is the part that's easy to get wrong. The WAL append happens **inside
`Storage`'s mutex**, in the same critical section as the map update:

```cpp
void Storage::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wal_) wal_->append_set(key, value);   // log FIRST...
    map_[key] = value;                          // ...then apply, both under the lock
}
```

Why does this matter? Imagine logging *outside* the lock with many worker threads:

```
Thread A: log "SET x 1"                 Thread B: log "SET x 2"
Thread B: apply x = 2
Thread A: apply x = 1     <- live result: x == 1
```
The **log** now reads `SET x 1` then `SET x 2`, but the **live** value is `1`. On
replay you'd get `x == 2` — a different database! By doing the log-append and the
map-apply together under one lock, **the log's order is forced to equal the apply
order**, so a replay always reproduces the live state exactly. This is the single
most important invariant in the whole phase.

## Crash recovery: tolerating a torn record

A crash can interrupt a write so the last record is incomplete. The reader
handles it gracefully — after reading the header it tries to read exactly the
declared bytes; if the file ends early, the read fails and replay **stops**,
keeping every complete record before it. The test
`test_truncated_trailing_record_is_ignored` proves this by hand-appending a
record that claims a 10-byte value but supplies 3: replay applies the earlier
good records and ignores the torn one.

It also guards against **corruption**: a record claiming an absurd length (say
from a flipped bit) is rejected by a sanity cap rather than trying to allocate
gigabytes.

## How durable, exactly? (flush vs. fsync)

After each record we call `out_.flush()`, which pushes the bytes out of our
program's buffer and into the **operating system**. That means:

- ✅ **Survives a process crash** (segfault, `kill -9`, our force-kill demo): the
  OS still holds the data and writes it to disk even though our process is gone.
- ⚠️ **May not survive a power loss / OS crash**: for that you must also call
  `fsync` (`FlushFileBuffers` on Windows) to force the OS to push the data onto
  the physical platters. We don't, because `fsync`-per-write is slow, and the
  standard `std::ofstream` doesn't expose the file handle it needs. Real databases
  get both speed and power-loss safety with **group commit** — batching many
  writes into one `fsync` — which is a worthwhile later improvement.

Our demo (force-killing the process) is exactly the "process crash" case, which
`flush()` covers — which is why the data came back.

> **A nice consequence:** because every acknowledged write is already flushed, we
> get durability **without** needing a graceful shutdown. Phase 2's notes said
> shutdown might "earn its keep" here — it turns out the WAL makes abrupt
> termination safe on its own, which is a stronger and simpler property.

## Honest limitations (and where they get fixed)

- **The log grows forever.** Every SET is appended, including overwrites and keys
  that were later deleted, so the file only ever gets bigger and replay gets
  slower over time. Real systems **compact**: periodically snapshot the current
  state and discard the old log. That's a natural next step (and is essentially
  what the Phase 8 RocksDB backend does for free).
- **No `fsync`** → not power-loss-proof (see above).
- **The WAL serializes writes.** Because logging happens under the storage lock
  and flushes to disk, all mutations are serialized on that disk write — so SET
  throughput drops once persistence is on (the cost of durability). Reads (GET)
  that don't touch the WAL are unaffected in spirit, though they share the lock.
- **A failed write is only flagged, not recovered.** If the disk fills mid-write,
  the WAL marks itself not-`ok()` and stops; we don't yet surface that to clients.

## Proving it works

- **`tests/test_wal.cpp`** (no networking): replay reconstructs an exact state
  through overwrites and deletes; a missing file is a clean fresh start; a value
  with spaces survives the round trip; a truncated trailing record is ignored
  while earlier records still apply.
- **The crash/restart demo**: start the server with a WAL, `SET` three keys,
  **force-kill** the process, restart, and `GET` them back — the data returns,
  having been replayed from disk.

## New ideas this phase introduces

| Concept | Meaning here |
|--------|--------------|
| **durability** | acknowledged data survives a crash/restart |
| **volatile vs. persistent** | RAM is lost on exit; a file on disk is not |
| **write-ahead logging** | record the change *before* applying it |
| **replay / recovery** | rebuild in-memory state by re-running the log |
| **flush vs. fsync** | reach the OS (survives process crash) vs. the platter (survives power loss) |
| **log order = apply order** | why the WAL append must sit inside the storage lock |
| **serialization / file format** | length-prefixed records, torn-record detection |
| **compaction** (future) | bounding log growth with snapshots |

## Try it yourself

1. Start `redon-server` (the WAL defaults to `redon.wal`), `SET` a few keys,
   close the client, then **kill the server window** and start it again — your
   keys are still there.
2. `Get-Content redon.wal` (or `cat redon.wal`) to see the human-readable log.
3. Start with `redon-server 127.0.0.1 6380 8 none` to turn persistence **off** and
   confirm the data does *not* survive a restart — the contrast makes the WAL's
   job obvious.
