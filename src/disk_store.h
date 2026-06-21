// disk_store.h — a persistent, on-disk storage engine (log-structured / Bitcask).
//
// Phases 1-7 kept every value in RAM. This engine keeps VALUES ON DISK and only a
// small index in RAM, so the dataset can be far larger than memory — the headline
// benefit the plan wanted from RocksDB, built from scratch.
//
// How it works (the Bitcask design):
//   * Writes are APPENDED to one data file, as length-prefixed records
//     (SET <klen> <vlen> <key><value>\n, DEL <klen> <key>\n) — never overwritten.
//   * An in-RAM index maps each key -> {offset, length} of its LATEST value in the
//     file. A read looks up the index and seeks/reads just that value from disk.
//   * On startup, RECOVERY rebuilds the index by scanning the record headers and
//     SKIPPING the value bytes — so it's fast (proportional to the key count, not
//     the data size), and the file is the durable source of truth.
//   * COMPACTION rewrites the file keeping only live keys, dropping the garbage
//     left by overwrites and deletes.
//
// This class is NOT internally synchronized — Storage calls it under its own lock.
#ifndef REDON_DISK_STORE_H
#define REDON_DISK_STORE_H

#include <cstddef>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace redon {

class DiskStore {
public:
    explicit DiskStore(std::string path);

    bool ok() const { return ok_; }  // false if the data file couldn't be opened

    bool set(const std::string& key, const std::string& value);  // append + index
    bool get(const std::string& key, std::string* out);          // index -> disk read
    bool del(const std::string& key);                            // append tombstone
    bool exists(const std::string& key) const { return index_.count(key) != 0; }
    std::size_t size() const { return index_.size(); }
    bool clear();  // false if the file couldn't be reopened (backend now unusable)

    // (key, value) for every live key — reads each value from disk. Used by
    // replication's snapshot.
    std::vector<std::pair<std::string, std::string>> snapshot();

private:
    // Where a key's current value lives in the data file.
    struct Entry {
        std::streampos value_pos;
        std::size_t value_len;
    };

    void recover();   // rebuild the index from the file (and compact if wasteful)
    void compact();   // rewrite the file dropping superseded/deleted records
    bool read_value(const Entry& e, std::string* out);

    std::string path_;
    std::fstream file_;  // single read+write handle, binary mode
    std::unordered_map<std::string, Entry> index_;
    bool ok_ = false;
};

}  // namespace redon

#endif  // REDON_DISK_STORE_H
