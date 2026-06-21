# Phase 8 — explained (an on-disk storage engine)

The plan calls this "RocksDB integration." RocksDB is a real C++ storage-engine
library — but pulling it in means a heavy multi-dependency build, and it cuts
against this project's *build-it-from-scratch* spirit. So instead we built **our
own persistent storage engine** that delivers RocksDB's headline benefits:

> **Values live on disk, not in RAM** — so the dataset can be far bigger than
> memory. Data **survives a restart**. **Recovery is fast.**

We used the **log-structured** design (specifically the **Bitcask** model used by
Riak) — the simplest engine with these properties.

## The problem with everything before this

Phases 1–7 kept *every value* in a `std::unordered_map` in RAM. The Phase 3 WAL
made it *durable* (replay on restart), but it still loaded **all values back into
RAM**. So your dataset could never exceed memory. A real database holds far more
data than RAM — the values must live **on disk**, with only enough in memory to
find them.

## The design: a log + an index ([disk_store.cpp](../src/disk_store.cpp))

Two pieces:

```
   data file on DISK (append-only)            index in RAM (small)
   ┌────────────────────────────┐            key      → {offset, length}
0: │ SET 4 7 nameSaurabh         │            "name"   → {8, 7}    ─┐
20:│ SET 7 6 companyOpenAI       │            "company"→ {31, 6}    │ just enough
40:│ SET 4 3 langC++             │            "lang"   → {51, 3}   ─┘ to find the value
58:│ SET 4 6 nameSourav    ◄─────│── overwrite: index["name"] now → {72, 6}
   └────────────────────────────┘
```

- **The data file** is **append-only**: every `SET`/`DEL` is a new record at the
  end (same length-prefixed format as the WAL), never an in-place edit.
- **The index** (`unordered_map<string, {offset, length}>`) holds, for each key,
  *where its latest value is* in the file — the offset and length, **not the value
  itself**. So the index is tiny (a key + ~16 bytes), and the *values stay on
  disk*.

The operations fall right out:

| Op | What it does | Cost |
|----|--------------|------|
| `set(k,v)` | append a record, flush, set `index[k] = {value's offset, length}` | O(1) + one disk append |
| `get(k)` | look up `index[k]`, **seek** to the offset, **read** `length` bytes from disk | O(1) + one disk read |
| `del(k)` | append a `DEL` tombstone, erase `k` from the index | O(1) + one disk append |
| overwrite | just another append; the index points at the new record; the old one is now dead weight | O(1) |

```cpp
bool DiskStore::set(const string& key, const string& value) {
    file_.seekp(0, ios::end);                       // append at the end
    file_ << "SET " << key.size() << " " << value.size() << " ";
    file_.write(key.data(), key.size());
    streampos value_pos = file_.tellp();            // value starts HERE
    file_.write(value.data(), value.size());
    file_.put('\n');
    file_.flush();                                   // durable before we ack
    index_[key] = {value_pos, value.size()};
    return true;
}
bool DiskStore::get(const string& key, string* out) {
    auto it = index_.find(key); if (it == end) return false;
    file_.seekg(it->second.value_pos);              // jump to the value on disk
    out->resize(it->second.value_len);
    file_.read(&(*out)[0], it->second.value_len);   // read just that value
    return true;
}
```

## Fast recovery: rebuild the index without reading the values

On startup we don't have the index — it's in RAM, which is gone. So we **recover**
it by scanning the data file. The trick that makes it *fast*: read each record's
**header and key**, note where the value *would be*, and **`seek` past the value
bytes** instead of reading them.

```cpp
// for each record while scanning:
read "SET", klen, vlen, key (klen bytes)
streampos value_pos = file_.tellg();         // where the value is
file_.seekg(vlen, ios::cur);                  // SKIP the value — don't read it
index_[key] = {value_pos, vlen};              // just remember where it is
```

So recovery costs ~one disk pass reading only keys, **proportional to the number
of keys, not the size of the data** — exactly RocksDB's "faster recovery." (In the
demo, the restart logged "3 key(s) loaded" and the values stayed on disk.) An
overwrite or delete encountered later in the scan just updates/removes the index
entry, so the final index reflects the latest state — and a record truncated by a
crash fails to parse, so recovery stops cleanly, keeping every complete record
before it (same robustness as the WAL).

## Compaction: reclaiming the garbage

Because the file is append-only, overwrites and deletes leave **dead records**
behind — the file grows forever. **Compaction** fixes this: rewrite the file
keeping only each live key's latest value, dropping the rest.

```cpp
// rewrite live entries to a fresh file, then swap it in
for (auto& [key, entry] : index_) { read its value; write it to path.tmp; }
remove(path); rename(path.tmp, path); reopen; adopt the new offsets;
```

We trigger it on startup when the file is mostly garbage (file size ≫ live data).
The test hammers one key with 2000 overwrites, reopens, and confirms compaction
ran and the value survived. (This is the same idea as RocksDB's background
compaction of its sorted files — ours is simpler: one level, run at startup.)

## How it plugs in ([storage.cpp](../src/storage.cpp))

`Storage` gained an optional `DiskStore` backend. When you pass `--disk <path>`,
`Storage::set/get/del/...` **delegate** to the disk engine and the in-memory LRU
map and the WAL are bypassed (the engine is its *own* durability). Everything else
is unchanged — and nicely, **replication still composes**: a disk-backed leader's
`set` still replicates, and its replication snapshot reads the values from disk.
So you can run a disk-backed node that's also a replica leader.

```sh
redon-server 127.0.0.1 6380 4 none 0 0 --disk redon.db
```
Write some keys, kill the server, restart with the same `--disk redon.db`: the
data is back (no WAL involved — the data file *is* the database), and
`Get-Content redon.db` shows the human-readable records.

## What this build does and doesn't do (honesty)

- ✅ **Values on disk**, dataset can exceed RAM; **survives a crash**; **fast,
  key-count-proportional recovery**; **compaction** reclaims space.
- ⚠️ **Keys are still in RAM** (the index). This is the Bitcask trade-off: values
  scale past RAM, but the *number* of keys is bounded by RAM for the index
  (≈ tens of bytes per key, so still tens of millions of keys). RocksDB's
  **LSM-tree** keeps even the keys on disk (sorted files + a memtable + bloom
  filters) — strictly more powerful, and substantially more code.
- ⚠️ **`flush`, not `fsync`** — process-crash durable (the demo), not power-loss
  durable, same boundary as the WAL.
- ⚠️ **One disk read per `get`, no caching** — a real engine caches hot blocks; we
  rely on the OS page cache.
- ⚠️ **Startup-only, single-level compaction** vs RocksDB's continuous multi-level
  compaction.

## New ideas this phase introduces

| Concept | Meaning here |
|---|---|
| **storage engine** | the component that actually stores/retrieves the bytes |
| **log-structured storage** | only ever append; never edit in place |
| **values on disk + RAM index** | keep values on disk, keep just key→offset in memory (Bitcask) |
| **fast recovery** | rebuild the index by reading headers, skipping values |
| **compaction** | rewrite the log dropping dead records to reclaim space |
| **LSM-tree** (RocksDB) | the more powerful design that keeps keys on disk too |

**Recap:** Redon now has a real persistent storage engine. Values live on disk in
an append-only log; a small in-RAM index points at each key's latest value; a
restart rebuilds that index quickly by skipping over the value bytes; and
compaction reclaims the space left by overwrites and deletes. It's the Bitcask
design — the simplest engine that gets values out of RAM — and the step up from
here is the full **LSM-tree** that powers RocksDB.
