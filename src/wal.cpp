// wal.cpp — implementation of the Write-Ahead Log.
#include "wal.h"

#include <utility>

#include "storage.h"

namespace redon {
namespace {

// A corrupt log could claim an absurd field length; cap it so we fail cleanly
// instead of trying to allocate gigabytes. Our values are line-bounded (<64 KB),
// so this is generously above anything legitimate.
constexpr std::size_t kMaxField = 256u * 1024 * 1024;  // 256 MB

// RAII: put a Storage into replay mode for the duration of a replay and restore
// live mode in the destructor — even if an exception unwinds the stack. Without
// this, a throw mid-replay (e.g. std::bad_alloc) would leave replaying_ stuck
// true, permanently disabling eviction.
struct ReplayGuard {
    Storage& store;
    explicit ReplayGuard(Storage& s) : store(s) { store.set_replaying(true); }
    ~ReplayGuard() { store.set_replaying(false); }
    ReplayGuard(const ReplayGuard&) = delete;
    ReplayGuard& operator=(const ReplayGuard&) = delete;
};

}  // namespace

Wal::Wal(std::string path) : path_(std::move(path)) {}

std::size_t Wal::replay_into(Storage& store) {
    // Open for reading in binary mode so byte counts are exact on every platform.
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return 0;  // no log yet: a fresh database
    }

    // Suppress LRU eviction while replaying: the log already records exactly
    // which keys were evicted (as DELs), so the cache must not re-derive
    // evictions from the (read-blind) replay recency. The guard restores live
    // mode (which also trims to capacity if the bound shrank) on every exit path,
    // including an exception.
    ReplayGuard replay_guard(store);

    std::size_t applied = 0;
    std::string op;
    while (in >> op) {  // reads a whitespace-delimited token (SET / DEL)
        if (op == "SET") {
            std::size_t key_len = 0;
            std::size_t val_len = 0;
            if (!(in >> key_len >> val_len)) {
                break;  // header truncated or non-numeric
            }
            // Lengths must be non-zero (keys/values are never empty) and sane.
            // A zero or absurd length means corruption, not a real record.
            if (key_len == 0 || val_len == 0 ||
                key_len > kMaxField || val_len > kMaxField) {
                break;
            }
            if (in.get() != ' ') {
                break;  // missing/corrupt delimiter before the data
            }
            std::string key(key_len, '\0');
            std::string val(val_len, '\0');
            in.read(&key[0], static_cast<std::streamsize>(key_len));
            in.read(&val[0], static_cast<std::streamsize>(val_len));
            if (!in) {
                break;  // record cut short by a crash: ignore this partial record
            }
            if (in.get() != '\n') {
                break;  // missing terminator: incomplete or corrupt record
            }
            store.set(key, val);
            ++applied;
        } else if (op == "DEL") {
            std::size_t key_len = 0;
            if (!(in >> key_len)) {
                break;
            }
            if (key_len == 0 || key_len > kMaxField) {
                break;
            }
            if (in.get() != ' ') {
                break;
            }
            std::string key(key_len, '\0');
            in.read(&key[0], static_cast<std::streamsize>(key_len));
            if (!in) {
                break;
            }
            if (in.get() != '\n') {
                break;
            }
            store.del(key);
            ++applied;
        } else {
            break;  // unrecognized op: stop at the first sign of corruption
        }
    }

    return applied;  // replay_guard restores live mode (and trims) on the way out
}

bool Wal::open_for_append() {
    // Append mode: every write goes to the end, preserving the replayed history.
    out_.open(path_, std::ios::out | std::ios::app | std::ios::binary);
    ok_ = out_.is_open();
    return ok_;
}

bool Wal::append_set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ok_) {
        return false;  // a prior write failed; we can't promise durability
    }
    out_ << "SET " << key.size() << " " << value.size() << " ";
    out_.write(key.data(), static_cast<std::streamsize>(key.size()));
    out_.write(value.data(), static_cast<std::streamsize>(value.size()));
    out_.put('\n');
    out_.flush();  // push to the OS so a process crash can't lose this write
    if (!out_) {
        ok_ = false;  // a failed write means we can no longer guarantee durability
        return false;
    }
    return true;
}

bool Wal::append_del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ok_) {
        return false;
    }
    out_ << "DEL " << key.size() << " ";
    out_.write(key.data(), static_cast<std::streamsize>(key.size()));
    out_.put('\n');
    out_.flush();
    if (!out_) {
        ok_ = false;
        return false;
    }
    return true;
}

}  // namespace redon
