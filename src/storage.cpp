// storage.cpp — implementation of the Storage engine declared in storage.h.
//
// Every method takes the lock for its whole body using std::lock_guard, which
// locks on construction and unlocks automatically when the function returns
// (even if an exception is thrown). This "RAII" pattern is the standard, safe
// way to use a mutex in C++.
#include "storage.h"

#include "wal.h"

namespace redon {

void Storage::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Write-ahead: record the change to the log BEFORE applying it in memory.
    // Doing this under the same lock that guards the map guarantees the log's
    // order matches the apply order, so a replay rebuilds an identical state.
    if (wal_ != nullptr) {
        wal_->append_set(key, value);
    }
    map_[key] = value;
    // Phase 5 will add: replicate("SET", key, value);
}

bool Storage::get(const std::string& key, std::string* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

std::size_t Storage::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) {
        return 0;  // nothing to remove, and nothing worth logging
    }
    // Write-ahead: log the delete before removing it from memory.
    if (wal_ != nullptr) {
        wal_->append_del(key);
    }
    map_.erase(it);
    return 1;
}

void Storage::attach_wal(Wal* wal) {
    std::lock_guard<std::mutex> lock(mutex_);
    wal_ = wal;
}

bool Storage::exists(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.find(key) != map_.end();
}

std::size_t Storage::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
}

}  // namespace redon
