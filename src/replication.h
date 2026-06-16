// replication.h — leader-side replication: stream writes to follower nodes.
//
// Phase 5 makes Redon distributed. A LEADER keeps the authoritative data and,
// for each FOLLOWER address, runs a background "sender" thread that:
//   1. connects to the follower and sends a handshake telling it to reset,
//   2. sends a full SNAPSHOT of the current data (so the follower catches up),
//   3. STREAMS every subsequent write (SET/DEL, including LRU evictions).
//
// Replication is ASYNCHRONOUS: Storage::set/del just enqueue the write (fast,
// under the storage lock) and the sender thread delivers it. A slow or down
// follower therefore never stalls clients — the trade-off is a replication
// "lag", and writes acknowledged just before a leader crash may not have reached
// the followers yet. (This is exactly Redis's default async replication.)
//
// Consistency: on (re)connect the sender takes an ATOMIC CUT via
// Storage::snapshot_locked — under the storage lock it copies the snapshot AND
// flips the follower to "streaming" — so every write either lands in the
// snapshot or in the stream, never both and never neither.
#ifndef REDON_REPLICATION_H
#define REDON_REPLICATION_H

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace redon {

class Storage;

class Replicator {
public:
    // `follower_addrs` are "host:port" strings. `storage` is the leader's store,
    // used to snapshot on (re)connect; it must outlive the Replicator.
    Replicator(std::vector<std::string> follower_addrs, Storage* storage);
    ~Replicator();

    Replicator(const Replicator&) = delete;
    Replicator& operator=(const Replicator&) = delete;

    void start();  // spawn one sender thread per follower
    void stop();   // signal all senders to stop, then join them

    // Enqueue a write to every follower that is currently streaming. Called by
    // Storage WHILE it holds its lock, so replication order == apply order.
    void replicate_set(const std::string& key, const std::string& value);
    void replicate_del(const std::string& key);

    std::size_t follower_count() const { return links_.size(); }

private:
    // One follower connection's state.
    struct Link {
        std::string host;
        std::uint16_t port = 0;
        std::mutex mutex;                // guards queue/streaming/resync/stop
        std::condition_variable cv;
        std::deque<std::string> queue;   // pending wire lines, oldest first
        bool streaming = false;          // past the full-sync cut, delivering live
        bool resync_needed = false;      // backlog overflowed -> drop & re-sync
        bool stop = false;
        std::thread thread;
    };

    void sender_loop(Link* link);
    void enqueue(const std::string& line);  // append to each streaming link

    std::vector<std::unique_ptr<Link>> links_;
    Storage* storage_;
    bool started_ = false;
};

}  // namespace redon

#endif  // REDON_REPLICATION_H
