// storage.h — Redon's storage engine (the "database" itself).
//
// Phase 1: an in-memory key->value map. We hide it behind a class so that in
// later phases set()/del() can ALSO write to a Write-Ahead Log (Phase 3) and
// forward changes to replicas (Phase 5) without any caller having to change.
//
// It is already thread-safe (every method locks a mutex). Phase 1 serves one
// client at a time so this isn't strictly required yet, but Phase 2 adds many
// worker threads sharing this one Storage object, and an std::unordered_map
// read+written by two threads at once is undefined behavior. Locking now means
// Phase 2 needs no changes here.
#ifndef REDON_STORAGE_H
#define REDON_STORAGE_H

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace redon {

class Wal;  // forward declaration; storage.cpp includes wal.h

class Storage {
public:
    // Store value under key (overwriting any previous value). Returns true on
    // success; returns false WITHOUT changing memory if a Write-Ahead Log is
    // attached and the durable write failed — so memory never diverges from the
    // log, and the caller can tell the client the write was not persisted.
    bool set(const std::string& key, const std::string& value);

    // Look up key. Writes the value into *out and returns true if found;
    // returns false and leaves *out untouched if the key is absent.
    // (We return a bool rather than a value so callers can tell "" apart from
    // "missing" — an empty string is a perfectly valid stored value.)
    bool get(const std::string& key, std::string* out) const;

    // Remove key. Returns the number of keys actually removed (0 or 1), which
    // mirrors how Redis's DEL reports its result. If `durable` is given, it is
    // set to false (and the removal is NOT applied) when a WAL write failed;
    // otherwise true. A delete of a missing key is always durable (it's a no-op).
    std::size_t del(const std::string& key, bool* durable = nullptr);

    // Returns true if key is present.
    bool exists(const std::string& key) const;

    // Number of keys currently stored (handy for tests and future metrics).
    std::size_t size() const;

    // Attach a Write-Ahead Log so that future set()/del() calls are recorded to
    // disk before they take effect. Pass nullptr (the default) for no logging —
    // which is exactly the state used while REPLAYING a log, so replay doesn't
    // re-write what it is reading. Not thread-safe; call once at startup before
    // any client is served.
    void attach_wal(Wal* wal);

private:
    // `mutable` so we can lock it inside const methods (get/exists/size).
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> map_;
    Wal* wal_ = nullptr;  // not owned; nullptr means "don't log"
};

}  // namespace redon

#endif  // REDON_STORAGE_H
