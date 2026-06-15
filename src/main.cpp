// main.cpp — entry point for the `redon-server` executable.
//
// Usage:
//   redon-server                                 # 127.0.0.1:6380, default workers
//   redon-server <port>                          # 127.0.0.1:<port>
//   redon-server <host> <port>                   # <host>:<port>
//   redon-server <host> <port> <threads>                  # ...worker-pool size
//   redon-server <host> <port> <threads> <wal>            # ...WAL path
//   redon-server <host> <port> <threads> <wal> <idle>     # ...idle timeout
//   redon-server <host> <port> <threads> <wal> <idle> <cap># ...LRU capacity
//
// The WAL (Write-Ahead Log) file defaults to "redon.wal" — data persists across
// restarts. Pass "none" as the <wal> argument for an in-memory-only server.
// <idle> is the seconds a client may sit idle before being disconnected
// (default 300; 0 disables it), like Redis's `timeout` directive. <cap> is the
// LRU capacity in keys (default 0 = unbounded), like Redis's `maxmemory`.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "net.h"
#include "server.h"

namespace {

constexpr std::uint16_t kDefaultPort = 6380;  // Redis uses 6379; +1 to coexist
const char kDefaultHost[] = "127.0.0.1";

// Parse a port from text. Returns false if it isn't a number in 1..65535.
bool parse_port(const std::string& text, std::uint16_t* out) {
    try {
        std::size_t consumed = 0;
        int value = std::stoi(text, &consumed);
        if (consumed != text.size() || value < 1 || value > 65535) {
            return false;
        }
        *out = static_cast<std::uint16_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;  // not a number / out of range
    }
}

// Parse a worker-thread count from text. Returns false unless it is 1..4096.
bool parse_workers(const std::string& text, std::size_t* out) {
    try {
        std::size_t consumed = 0;
        long value = std::stol(text, &consumed);
        if (consumed != text.size() || value < 1 || value > 4096) {
            return false;
        }
        *out = static_cast<std::size_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// A sensible default pool size: one worker per hardware thread, or 8 if the
// system can't tell us. Override on the command line for many concurrent clients.
std::size_t default_workers() {
    unsigned hw = std::thread::hardware_concurrency();
    return hw != 0 ? static_cast<std::size_t>(hw) : 8;
}

// Parse an idle-timeout in seconds: 0 (disabled) up to one day.
bool parse_timeout(const std::string& text, int* out) {
    try {
        std::size_t consumed = 0;
        long value = std::stol(text, &consumed);
        if (consumed != text.size() || value < 0 || value > 86400) {
            return false;
        }
        *out = static_cast<int>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Parse an LRU capacity in keys: 0 (unbounded) or any positive count. Parse as
// signed so a negative like "-1" is rejected rather than wrapping to a huge
// unsigned value (std::stoull would silently accept "-1" as SIZE_MAX).
bool parse_capacity(const std::string& text, std::size_t* out) {
    try {
        std::size_t consumed = 0;
        long long value = std::stoll(text, &consumed);
        if (consumed != text.size() || value < 0) {
            return false;
        }
        *out = static_cast<std::size_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;  // not a number / out of range
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = kDefaultHost;
    std::uint16_t port = kDefaultPort;
    std::size_t workers = default_workers();
    std::string wal_path = "redon.wal";  // persistence on by default
    int idle_timeout = 300;              // seconds; 0 disables
    std::size_t capacity = 0;            // LRU bound in keys; 0 = unbounded

    // Positional args: [host] [port] [threads] [wal]. A bare port is allowed as
    // the single-arg form; later options require the earlier ones to be given.
    if (argc >= 2) {
        // For argc==2 the lone argument is the port; otherwise argv[1] is host.
        if (argc == 2) {
            if (!parse_port(argv[1], &port)) {
                std::cerr << "error: invalid port '" << argv[1] << "'\n";
                return 1;
            }
        } else {
            host = argv[1];
            if (!parse_port(argv[2], &port)) {
                std::cerr << "error: invalid port '" << argv[2] << "'\n";
                return 1;
            }
        }
    }
    if (argc >= 4) {
        if (!parse_workers(argv[3], &workers)) {
            std::cerr << "error: invalid thread count '" << argv[3]
                      << "' (expected 1..4096)\n";
            return 1;
        }
    }
    if (argc >= 5) {
        wal_path = argv[4];
    }
    if (argc >= 6) {
        if (!parse_timeout(argv[5], &idle_timeout)) {
            std::cerr << "error: invalid idle timeout '" << argv[5]
                      << "' (expected 0..86400 seconds)\n";
            return 1;
        }
    }
    if (argc >= 7) {
        if (!parse_capacity(argv[6], &capacity)) {
            std::cerr << "error: invalid capacity '" << argv[6]
                      << "' (expected a non-negative integer)\n";
            return 1;
        }
    }
    if (argc > 7) {
        std::cerr << "usage: redon-server [host] [port] [threads] [wal] [idle] "
                     "[capacity]\n";
        return 1;
    }

    // Start the sockets library for the lifetime of the program (Windows needs
    // this; on POSIX it is a no-op).
    redon::net::Init net_init;
    if (!net_init.ok()) {
        std::cerr << "error: failed to initialize sockets library\n";
        return 1;
    }

    redon::Server server(host, port, workers, wal_path, idle_timeout, capacity);
    return server.run();
}
