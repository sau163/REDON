// server.h — the TCP server that ties networking + protocol + storage together.
//
// Phase 1 behavior: bind a port, then loop forever accepting ONE client at a
// time. Each client is served fully (read commands, reply) until it disconnects
// or sends QUIT, after which the server accepts the next client. Phase 2 will
// hand each accepted client to a worker thread so many can be served at once;
// the per-client logic in handle_client() is already isolated for that change.
#ifndef REDON_SERVER_H
#define REDON_SERVER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "net.h"
#include "storage.h"

namespace redon {

class Wal;  // forward declaration; server.cpp includes wal.h

class Server {
public:
    // `num_workers` is the size of the thread pool — the number of clients that
    // can be actively served at the same time. `wal_path` is the Write-Ahead Log
    // file for persistence; pass an empty string (or "none"/"-") to run purely
    // in memory with no durability. `idle_timeout_seconds` disconnects a client
    // that sends nothing for that long (0 disables it), so a stalled connection
    // can't hold a worker forever.
    Server(std::string host, std::uint16_t port, std::size_t num_workers,
           std::string wal_path, int idle_timeout_seconds);

    // Declared (not defaulted inline) so the std::unique_ptr<Wal> member can be
    // destroyed in server.cpp, where Wal is a complete type.
    ~Server();

    // Set up the listening socket and run the accept loop. Blocks indefinitely.
    // Returns a non-zero exit code if the socket could not be set up; otherwise
    // it does not return under normal operation (stop with Ctrl+C).
    int run();

private:
    // Serve a single connected client until it disconnects or sends QUIT. Runs
    // on a worker thread, so it touches only its own socket plus the shared
    // (internally locked) Storage.
    void handle_client(net::socket_t client);

    // Print one line to stdout atomically. Worker threads log concurrently, so
    // without this their output would interleave mid-line.
    void log(const std::string& message);

    // Replay the WAL into store_ and attach it for future writes. Returns false
    // only on a fatal error (e.g. the log file can't be opened for appending).
    bool setup_persistence();

    std::string host_;
    std::uint16_t port_;
    std::size_t num_workers_;
    int idle_timeout_seconds_;        // 0 => no idle timeout
    std::string wal_path_;            // empty / "none" / "-" => persistence off
    std::unique_ptr<Wal> wal_;        // owns the log; attached to store_
    Storage store_;  // the one shared database for every client (thread-safe)

    std::mutex log_mutex_;                 // serializes log() output
    std::atomic<int> active_clients_{0};   // currently-connected client count
};

}  // namespace redon

#endif  // REDON_SERVER_H
