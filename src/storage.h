// storage.h — Redon's storage engine, now an LRU (least-recently-used) cache.
//
// Phase 1-3 used a plain hash map. Phase 4 bounds memory: when a capacity is set
// and the cache is full, inserting a NEW key evicts the least-recently-used one.
//
// The classic O(1) LRU design, two structures kept in sync:
//   * a doubly-linked list of (key,value) ordered by recency — most-recently-used
//     at the FRONT, least-recently-used at the BACK.
//   * a hash map from key -> that key's node in the list.
// Moving a node to the front (on use) and dropping the back node (on eviction)
// are both O(1) because the map hands us the node directly. "Used" = read (get)
// or written (set). Capacity 0 means unbounded (no eviction) — the default.
//
// Persistence interaction (Phase 3 + 4): an eviction is logged to the WAL as a
// DEL so the log stays consistent with memory; during replay eviction is
// SUPPRESSED so only the logged DELs remove keys (the log already records exactly
// which keys were evicted — replay can't re-derive that, since it never sees the
// read-driven recency).
#ifndef REDON_STORAGE_H
#define REDON_STORAGE_H

#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace redon {

class Wal;          // forward declaration; storage.cpp includes wal.h
class Replicator;   // forward declaration; storage.cpp includes replication.h
class DiskStore;    // forward declaration; storage.cpp includes disk_store.h

class Storage {
public:
    // capacity 0 = unbounded (never evict). Otherwise the cache holds at most
    // `capacity` keys, evicting the least-recently-used to stay within it.
    explicit Storage(std::size_t capacity = 0);

    // Declared (defaulted in storage.cpp, where DiskStore is complete) so the
    // unique_ptr<DiskStore> member can be destroyed.
    ~Storage();

    // Switch this Storage to a persistent on-disk backend at `path` (Phase 8):
    // values live on disk, not in RAM, and survive restarts. When enabled, the
    // in-memory LRU map and the Write-Ahead Log are bypassed (the engine is its
    // own durability). Returns false if the file couldn't be opened. Call once at
    // startup before serving clients.
    bool open_disk_backend(const std::string& path);

    // Store value under key and mark it most-recently-used. Inserting a new key
    // when full evicts the least-recently-used key. Returns false WITHOUT
    // changing memory if a WAL is attached and the durable write failed.
    bool set(const std::string& key, const std::string& value);

    // Look up key. On a hit, mark it most-recently-used and write the value to
    // *out; on a miss return false and leave *out untouched. NOT const: a
    // successful read updates recency.
    bool get(const std::string& key, std::string* out);

    // Remove key. Returns the number removed (0 or 1). If `durable` is given it
    // is set false (and the removal skipped) when a WAL write failed; deleting a
    // missing key is always a durable no-op.
    std::size_t del(const std::string& key, bool* durable = nullptr);

    // True if key is present. Does NOT change recency — a mere existence check
    // isn't really a "use" — so it stays const.
    bool exists(const std::string& key) const;

    // Number of keys currently stored.
    std::size_t size() const;

    // The capacity bound (0 = unbounded).
    std::size_t capacity() const { return capacity_; }

    // Attach a Write-Ahead Log so future set/del (and evictions) are recorded.
    // Pass nullptr (the default) for no logging — the state used during replay.
    void attach_wal(Wal* wal);

    // Attach a Replicator (leader only) so future set/del (and evictions) are
    // forwarded to followers. nullptr (the default) means "don't replicate".
    void attach_replicator(Replicator* replicator);

    // Take a consistent snapshot of all (key, value) pairs while holding the
    // lock, running `while_locked` (if given) in the SAME critical section. The
    // replicator uses this to atomically snapshot the data AND start streaming
    // new writes, so a (re)syncing follower misses no write and sees no
    // duplicate. Order is most-recently-used first.
    std::vector<std::pair<std::string, std::string>> snapshot_locked(
        const std::function<void()>& while_locked = {});

    // Remove all keys (used by a follower when the leader starts a full sync).
    // Does NOT touch the WAL — replicas should run without one. Returns false
    // only if an attached disk backend failed to reset (then it's unusable).
    bool clear();

    // Toggle replay mode. While replaying a log, eviction is SUPPRESSED so only
    // the explicitly-logged DEL records remove keys (replay can't see the
    // original read-driven recency, so re-deriving evictions would pick the wrong
    // keys). Turning replay mode OFF trims any excess down to capacity (unlogged)
    // — which only matters if capacity was reduced since the log was written.
    void set_replaying(bool replaying);

private:
    using Item = std::pair<std::string, std::string>;  // (key, value)
    using ListIt = std::list<Item>::iterator;

    // Evict the least-recently-used key. Caller must hold mutex_ and ensure the
    // cache is non-empty. `log_eviction` controls whether a DEL is written.
    void evict_lru_locked(bool log_eviction);

    mutable std::mutex mutex_;
    std::list<Item> items_;                          // front = MRU, back = LRU
    std::unordered_map<std::string, ListIt> index_;  // key -> its list node
    std::size_t capacity_ = 0;                       // 0 = unbounded
    bool replaying_ = false;                          // suppress eviction in replay
    Wal* wal_ = nullptr;                             // not owned; nullptr = no log
    Replicator* replicator_ = nullptr;               // not owned; nullptr = no repl
    std::unique_ptr<DiskStore> disk_;                // non-null = on-disk backend
};

}  // namespace redon

#endif  // REDON_STORAGE_H
