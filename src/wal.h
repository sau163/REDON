// wal.h — the Write-Ahead Log: Redon's durability layer.
//
// Phase 1-2 kept everything in RAM, so a crash lost all data. The WAL fixes
// that with one rule: BEFORE a mutation (SET/DEL) is applied to memory, append a
// record describing it to a file and flush it to disk. Then, on the next start,
// replay the file to rebuild the exact in-memory state. Because the record is
// written *ahead* of the in-memory change (hence "write-ahead"), once we tell a
// client "OK" the data is already safe on disk.
//
// Record format (one per line, length-prefixed so keys/values with spaces are
// unambiguous and a half-written final record is detectable):
//
//     SET <key_len> <val_len> <key bytes><val bytes>\n
//     DEL <key_len> <key bytes>\n
//
// The reader uses the lengths (not delimiters) to know exactly how many bytes to
// read, then expects the trailing '\n'. A record cut short by a crash fails that
// read and is simply ignored — every complete record before it is still applied.
#ifndef REDON_WAL_H
#define REDON_WAL_H

#include <cstddef>
#include <fstream>
#include <mutex>
#include <string>

namespace redon {

class Storage;  // forward declaration; wal.cpp includes storage.h

class Wal {
public:
    explicit Wal(std::string path);

    // Read an existing log file (if any) and apply every COMPLETE record to
    // `store`, rebuilding its state. Call this BEFORE attaching the WAL to the
    // store (so replay itself doesn't write new records). Returns how many
    // records were replayed. A missing file is fine — it means a fresh start.
    std::size_t replay_into(Storage& store);

    // Open the log file for appending (creating it if needed). Returns false if
    // the file could not be opened. Call after replay_into().
    bool open_for_append();

    // Append one record and flush it so a process crash can't lose it. These are
    // called by Storage while it holds its own lock, but they also take the WAL's
    // own lock so the file is never written by two threads at once.
    void append_set(const std::string& key, const std::string& value);
    void append_del(const std::string& key);

    // False once a write has failed (e.g. the disk is full).
    bool ok() const { return ok_; }

private:
    std::string path_;
    std::ofstream out_;
    std::mutex mutex_;   // serializes writes to out_
    bool ok_ = true;
};

}  // namespace redon

#endif  // REDON_WAL_H
