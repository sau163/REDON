# Phase 4 — explained (LRU cache eviction)

So far the store grew forever: every new key just took more RAM. A real cache has
a **memory budget** and, when full, must **throw something away** to make room.
Phase 4 adds that: a **capacity**, and an **LRU (Least-Recently-Used)** eviction
policy — when the cache is full and you insert a new key, the key that hasn't been
touched for the longest time is dropped. This is the classic data-structures
interview problem, and it interacts with the WAL in a genuinely interesting way.

## The idea in one line

> When the cache is at capacity and a new key arrives, evict the
> **least-recently-used** key to make room. "Used" = read (GET) or written (SET).

```
capacity = 3
SET a ; SET b ; SET c        cache (MRU→LRU):  c  b  a
GET a                        touch a:          a  c  b      (a moves to front)
SET d   → full, evict LRU=b  insert d:         d  a  c      (b is gone)
```

Notice `b` was evicted, **not** `a` — because reading `a` marked it "recently
used." That recency tracking is the whole trick.

## The O(1) data structure ([storage.h](../src/storage.h))

The naive way to find "the least-recently-used key" is to scan everything — O(n),
too slow. The classic trick gets it to **O(1)** with *two* structures working
together:

```
  items_ (doubly-linked list, ordered by recency)
     front ─► [d] ⇄ [a] ⇄ [c] ◄─ back
              MRU            LRU
                ▲    ▲    ▲
  index_ (hash map):  "d"→●  "a"→●  "c"→●     key → its node in the list
```

- **`std::list<pair<string,string>> items_`** — keys in recency order, most-recent
  at the **front**, least-recent at the **back**.
- **`std::unordered_map<string, list-iterator> index_`** — maps each key straight
  to its node in the list.

Why two? Each covers the other's weakness:

| Operation | How | Cost |
|-----------|-----|------|
| find a key's value | `index_` lookup | O(1) |
| mark a key most-recently-used | `index_` gives the node → **splice** it to the front | O(1) |
| evict the least-recently-used | take `items_.back()` → erase from both | O(1) |

The magic word is **`splice`**: `std::list::splice` moves a node to the front by
just relinking pointers — it doesn't copy the data and, crucially, **doesn't
invalidate the iterator**, so the pointer stored in `index_` stays valid. That's
what makes "touch a key" O(1).

### The methods

```cpp
bool Storage::get(const std::string& key, std::string* out) {
    auto it = index_.find(key);
    if (it == index_.end()) return false;           // miss
    items_.splice(items_.begin(), items_, it->second);  // mark most-recently-used
    *out = it->second->second;
    return true;
}
```

`get` is **no longer `const`** — a successful read *changes* the cache (it bumps
recency). That's an honest consequence of LRU. (`exists` stays `const`: just
checking presence isn't really a "use.")

```cpp
bool Storage::set(const std::string& key, const std::string& value) {
    if (wal_ && !wal_->append_set(key, value)) return false;  // Phase 3 durability
    auto it = index_.find(key);
    if (it != index_.end()) {                       // existing key:
        it->second->second = value;                 //   overwrite value
        items_.splice(items_.begin(), items_, it->second);  // and mark MRU
        return true;
    }
    items_.emplace_front(key, value);               // new key at the front
    index_.emplace(key, items_.begin());
    while (!replaying_ && capacity_ > 0 && index_.size() > capacity_)
        evict_lru_locked(true);                     // over budget → drop the LRU
    return true;
}
```

Capacity **0 means unbounded** (the `while` never runs) — that's the default, and
it matches Redis's `maxmemory 0`.

## The interesting part: eviction vs. the Write-Ahead Log

Here's the question I flagged when we started Phase 4: **the cache evicts a key to
save memory — but the WAL (Phase 3) still has that key's `SET` on disk. What
happens on restart?**

If we did nothing, replay would re-add the evicted key from the log, and the cache
on restart would *not* match the cache before the crash. Worse, it can't simply
"re-evict" correctly, because **eviction depends on read (GET) recency, and GETs
aren't logged** — so a replay (which only sees SETs/DELs) would evict *different*
keys than the live server did. The restored database would silently differ from
the real one.

**The fix is what Redis does with its AOF: log the eviction.** When the cache
evicts key `E`, we append a `DEL E` to the WAL:

```cpp
void Storage::evict_lru_locked(bool log_eviction) {
    const std::string evicted = items_.back().first;
    if (log_eviction && wal_) wal_->append_del(evicted);  // keep the log honest
    index_.erase(evicted);
    items_.pop_back();
}
```

Now the log records the *exact* sequence of changes memory went through — every
SET, every user DEL, and every **eviction DEL**. You saw it in the demo:

```
SET 1 1 a1
SET 1 1 b2
SET 1 1 c3
SET 1 1 d4
DEL 1 b      ← the eviction of b, recorded as a delete
```

### ...and replay must NOT re-evict

There's a second half to the trick. During **replay**, eviction is **suppressed**
(`replaying_ = true`). Replay just applies the logged records faithfully — and
because every eviction is already in the log as a DEL, that faithfully reproduces
the live key set. If replay *also* ran the eviction logic, it would double-evict
(drop a key for being over capacity *and* drop the logged one) — and pick the
wrong victim, since it has no GET history. Suppressing eviction and trusting the
logged DELs is what keeps replay correct.

Trace it (capacity 2, the demo): log is `SET a, SET b, SET c, DEL b`. Replay with
eviction off: `{a} → {a,b} → {a,b,c}` (transiently 3, that's fine) `→ {a,c}` after
`DEL b`. Exactly the live result. The eviction-then-replay test asserts this.

When replay finishes, we flip back to live mode, which **trims to capacity** once
(unlogged) — this only does anything if you *reduced* the capacity since the log
was written, in which case it shrinks the loaded data to fit.

## What this design does and doesn't guarantee (honesty)

- ✅ **Same-capacity restart reproduces the live key set exactly** (verified by a
  design review and a test).
- ✅ **No acknowledged write is lost** and **no double-eviction**.
- ⚠️ **Read-recency is not persisted.** After a restart, recency is rebuilt from
  *log order* (SETs), not the original GETs — so which keys get evicted *next*
  may differ from an uncrashed run. The *contents* are correct; the future
  eviction *order* is approximate. That's normal, acceptable cache behavior (a
  cache is allowed to forget any key) and matches Redis reloading its AOF.
- ⚠️ **Eviction makes the log grow faster** (a DEL per eviction on a hot, full
  cache). The fix is **compaction** — periodically rewrite the log as a snapshot
  of current contents — which is a great next improvement (and what Phase 8's
  RocksDB does for free).
- ⚠️ **Tiny crash window:** if we crash after logging a `SET` but before its
  eviction `DEL`, replay loads one extra key and the post-replay trim drops one
  LRU to fit — bounded (+1) and safe, though the trimmed key may differ from the
  one live evicted. Acceptable cache semantics.

## Try it

```sh
# capacity 3, persistence on
redon-server 127.0.0.1 6380 4 redon.wal 300 3
```
```
SET a 1 ; SET b 2 ; SET c 3      # cache full (3 keys)
GET a                            # touch a -> a is most-recently-used
SET d 4                          # evicts b (the least-recently-used), not a
GET b   ->  (nil)                # b is gone
GET a   ->  1                    # a survived
cat redon.wal                    # see the "DEL ... b" eviction record
```
Now kill the server and restart it with the same command — `GET b` is *still*
`(nil)`: the eviction was replayed.

## New ideas this phase introduces

| Concept | Meaning here |
|--------|--------------|
| **eviction / capacity** | bound memory by dropping keys when full |
| **LRU policy** | drop the key unused for the longest; reads and writes count as use |
| **list + hash map = O(1) LRU** | the map finds the node, the list orders by recency, `splice` moves in O(1) |
| **recency mutates on read** | why `get` can't be `const` |
| **eviction is logged (DEL)** | keep the durable log consistent with the in-memory cache (Redis AOF style) |
| **suppress eviction on replay** | replay trusts logged DELs instead of re-deriving evictions it can't see |
| **compaction** (future) | bound log growth by snapshotting |
