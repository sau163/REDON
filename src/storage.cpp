// storage.cpp — implementation of the LRU storage engine declared in storage.h.
#include "storage.h"

#include <memory>
#include <utility>

#include "disk_store.h"
#include "replication.h"
#include "wal.h"

namespace redon {

Storage::Storage(std::size_t capacity) : capacity_(capacity) {}

// Defined here (where DiskStore is complete) so unique_ptr<DiskStore> destroys.
Storage::~Storage() = default;

bool Storage::open_disk_backend(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    disk_ = std::make_unique<DiskStore>(path);
    if (!disk_->ok()) {
        disk_.reset();
        return false;
    }
    return true;
}

bool Storage::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    // On-disk backend (Phase 8): it persists the write itself; the WAL and LRU
    // don't apply. Replication still happens.
    if (disk_) {
        if (!disk_->set(key, value)) {
            return false;  // the durable disk write failed
        }
        if (replicator_ != nullptr) {
            replicator_->replicate_set(key, value);
        }
        return true;
    }
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
        if (replicator_ != nullptr) {
            replicator_->replicate_set(key, value);
        }
        return true;
    }

    // New key: insert at the front, replicate it, then evict the least-recently-
    // used while we are over capacity (live only; replay suppresses eviction).
    items_.emplace_front(key, value);
    index_.emplace(key, items_.begin());
    if (replicator_ != nullptr) {
        replicator_->replicate_set(key, value);
    }
    while (!replaying_ && capacity_ > 0 && index_.size() > capacity_) {
        evict_lru_locked(true);
    }
    return true;
}

bool Storage::get(const std::string& key, std::string* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (disk_) {
        return disk_->get(key, out);  // read the value from disk; no recency
    }
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
    if (disk_) {
        const bool removed = disk_->del(key);
        if (!disk_->ok()) {  // a write failure (not just a missing key)
            if (durable != nullptr) {
                *durable = false;
            }
            return 0;
        }
        if (removed && replicator_ != nullptr) {
            replicator_->replicate_del(key);
        }
        return removed ? 1 : 0;
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
    if (replicator_ != nullptr) {
        replicator_->replicate_del(key);
    }
    items_.erase(it->second);
    index_.erase(it);
    return 1;
}

bool Storage::exists(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (disk_) {
        return disk_->exists(key);
    }
    return index_.find(key) != index_.end();
}

std::size_t Storage::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (disk_) {
        return disk_->size();
    }
    return index_.size();
}

void Storage::attach_wal(Wal* wal) {
    std::lock_guard<std::mutex> lock(mutex_);
    wal_ = wal;
}

void Storage::attach_replicator(Replicator* replicator) {
    std::lock_guard<std::mutex> lock(mutex_);
    replicator_ = replicator;
}

std::vector<std::pair<std::string, std::string>> Storage::snapshot_locked(
    const std::function<void()>& while_locked) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, std::string>> snap;
    if (disk_) {
        snap = disk_->snapshot();  // reads every value from disk
    } else {
        snap.assign(items_.begin(), items_.end());
    }
    if (while_locked) {
        while_locked();  // e.g. the replicator marks the follower "streaming"
    }
    return snap;
}

bool Storage::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (disk_) {
        return disk_->clear();
    }
    items_.clear();
    index_.clear();
    return true;
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
    if (log_eviction) {
        // Keep the log AND the followers consistent with memory: record the
        // eviction as a delete. Best-effort for the WAL — if it fails the key is
        // still evicted to honor capacity, and the WAL marks itself unhealthy so
        // the next client write is rejected.
        if (wal_ != nullptr) {
            wal_->append_del(evicted);
        }
        if (replicator_ != nullptr) {
            replicator_->replicate_del(evicted);
        }
    }
    index_.erase(evicted);
    items_.pop_back();
}

}  // namespace redon
