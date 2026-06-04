// storage.cpp — implementation of the Storage engine declared in storage.h.
//
// Every method takes the lock for its whole body using std::lock_guard, which
// locks on construction and unlocks automatically when the function returns
// (even if an exception is thrown). This "RAII" pattern is the standard, safe
// way to use a mutex in C++.
#include "storage.h"

namespace redon {

void Storage::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    map_[key] = value;
    // Phase 3 will add: wal_.append("SET", key, value);
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
    // unordered_map::erase(key) returns the number of elements removed (0 or 1
    // for a non-multimap), which is exactly the count we want to report.
    return map_.erase(key);
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
