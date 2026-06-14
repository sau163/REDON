// storage.cpp — implementation of the LRU storage engine declared in storage.h.
#include "storage.h"

#include <utility>

#include "wal.h"

namespace redon {

Storage::Storage(std::size_t capacity) : capacity_(capacity) {}

bool Storage::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Write-ahead: log the change BEFORE applying it (Phase 3). Refuse the
    // mutation if the durable write failed, so memory never diverges from the
    // log. Logging under the same lock as the apply keeps log order == apply
    // order — the invariant a replay depends on.
    if (wal_ != nullptr && !wal_->append_set(key, value)) {
        return false;
    }

    auto it = index_.find(key);
    if (it != index_.end()) {
        // Existing key: overwrite its value and move it to the front (MRU).
        it->second->second = value;
        items_.splice(items_.begin(), items_, it->second);
        return true;
    }

    // New key: insert at the front, then evict the least-recently-used while we
    // are over capacity (live only; replay suppresses eviction).
    items_.emplace_front(key, value);
    index_.emplace(key, items_.begin());
    while (!replaying_ && capacity_ > 0 && index_.size() > capacity_) {
        evict_lru_locked(true);
    }
    return true;
}

bool Storage::get(const std::string& key, std::string* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    // A read counts as a use: move the node to the front (most-recently-used).
    items_.splice(items_.begin(), items_, it->second);
    *out = it->second->second;
    return true;
}

std::size_t Storage::del(const std::string& key, bool* durable) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (durable != nullptr) {
        *durable = true;
    }
    auto it = index_.find(key);
    if (it == index_.end()) {
        return 0;  // nothing to remove (a no-op is trivially durable)
    }
    // Write-ahead: log the delete; refuse it (leaving memory == log) on failure.
    if (wal_ != nullptr && !wal_->append_del(key)) {
        if (durable != nullptr) {
            *durable = false;
        }
        return 0;
    }
    items_.erase(it->second);
    index_.erase(it);
    return 1;
}

bool Storage::exists(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_.find(key) != index_.end();
}

std::size_t Storage::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_.size();
}

void Storage::attach_wal(Wal* wal) {
    std::lock_guard<std::mutex> lock(mutex_);
    wal_ = wal;
}

void Storage::set_replaying(bool replaying) {
    std::lock_guard<std::mutex> lock(mutex_);
    replaying_ = replaying;
    if (!replaying) {
        // Leaving replay: fit the loaded data to capacity. This only does
        // anything if capacity was reduced since the log was written; the trims
        // are NOT logged (they just shrink the loaded snapshot to the new bound).
        while (capacity_ > 0 && index_.size() > capacity_) {
            evict_lru_locked(false);
        }
    }
}

void Storage::evict_lru_locked(bool log_eviction) {
    // Copy the key out before pop_back destroys the node.
    const std::string evicted = items_.back().first;
    // Keep the log consistent with memory: record the eviction as a delete.
    // Best-effort — if it fails the key is still evicted to honor capacity, and
    // the WAL marks itself unhealthy so the next client write is rejected.
    if (log_eviction && wal_ != nullptr) {
        wal_->append_del(evicted);
    }
    index_.erase(evicted);
    items_.pop_back();
}

}  // namespace redon
