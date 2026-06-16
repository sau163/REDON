// server.h — the TCP server that ties networking + protocol + storage together.
//
// It binds a port and runs an accept loop that hands each accepted client to a
// thread pool (Phase 2), so many clients are served at once. Each client is
// served until it disconnects or sends QUIT. Phase 5 adds roles: a node is a
// leader (serves writes and replicates them) or a read-only follower.
#ifndef REDON_SERVER_H
#define REDON_SERVER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "net.h"
#include "storage.h"

namespace redon {

class Wal;          // forward declaration; server.cpp includes wal.h
class Replicator;   // forward declaration; server.cpp includes replication.h

// All the knobs for one server node, bundled so the constructor isn't a dozen
// positional arguments.
struct ServerConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6380;
    std::size_t num_workers = 8;
    std::string wal_path = "redon.wal";       // "" / "none" / "-" => no persistence
    int idle_timeout_seconds = 300;           // 0 => no idle timeout
    std::size_t capacity = 0;                 // LRU bound; 0 => unbounded
    bool is_follower = false;                 // read-only replica?
    std::vector<std::string> follower_addrs;  // leader: "host:port" of each follower
};

class Server {
public:
    explicit Server(ServerConfig config);

    // Declared out-of-line so the unique_ptr<Wal>/<Replicator> members can be
    // destroyed in server.cpp, where those types are complete.
    ~Server();

    // Set up persistence + replication, then run the accept loop. Blocks
    // indefinitely; returns non-zero only if startup failed (stop with Ctrl+C).
    int run();

private:
    // Serve a single connected client until it disconnects or sends QUIT. Runs on
    // a worker thread, touching only its own socket plus the shared Storage.
    void handle_client(net::socket_t client);

    // Print one line to stdout atomically (worker threads log concurrently).
    void log(const std::string& message);

    // Replay the WAL into store_ and attach it. Returns false on a fatal error.
    bool setup_persistence();

    // Leader only: if followers are configured, start streaming writes to them.
    void setup_replication();

    ServerConfig config_;
    std::unique_ptr<Wal> wal_;          // owns the log; attached to store_
    Storage store_;                     // the one shared database (thread-safe)
    std::unique_ptr<Replicator> replicator_;  // leader: streams to followers

    std::mutex log_mutex_;                 // serializes log() output
    std::atomic<int> active_clients_{0};   // currently-connected client count
};

}  // namespace redon

#endif  // REDON_SERVER_H
