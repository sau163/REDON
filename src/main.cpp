// main.cpp — entry point for the `redon-server` executable.
//
// Positional args (each requires the earlier ones):
//   redon-server [host] [port] [threads] [wal] [idle] [capacity]
//   redon-server <port>                          # shorthand for a bare port
//
// Replication flags (Phase 5):
//   --replica                 run as a read-only follower (the leader connects in)
//   --follower <host:port>    run as a leader and replicate to this follower
//                             (repeat the flag for several followers)
// Raft flag (Phase 6) — mutually exclusive with the replication flags:
//   --raft <host:port>        join a Raft cluster; list the OTHER nodes' addresses
//                             (repeat the flag). The cluster elects a leader; only
//                             the leader accepts writes. Try the ROLE command.
// Sharding flag (Phase 7) — run as a router instead of a data node:
//   --shard <host:port>       forward each command to the shard owning its key
//                             (hash(key) %% N over the listed shards). Repeat the
//                             flag once per shard. The shards are plain servers.
// Storage-engine flag (Phase 8):
//   --disk <path>             use the on-disk storage engine (values live on disk,
//                             survive restarts) instead of the in-memory map.
//                             Replaces the WAL; not combinable with --shard.
// Monitoring flag (Phase 9):
//   --metrics-port <n>        serve Prometheus metrics at http://host:n/metrics.
//                             The INFO command also reports stats on the main port.
//
// Defaults: WAL "redon.wal" (use "none" for in-memory), idle timeout 300s
// (0 disables, like Redis `timeout`), capacity 0 = unbounded (like Redis
// `maxmemory`). Examples:
//   redon-server 127.0.0.1 6380 8 redon.wal 300 0 --follower 127.0.0.1:6381
//   redon-server 127.0.0.1 6381 8 none --replica
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
    redon::ServerConfig config;
    config.host = kDefaultHost;
    config.port = kDefaultPort;
    config.num_workers = default_workers();

    // Separate replication flags from the positional arguments.
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--replica") {
            config.is_follower = true;
        } else if (arg == "--follower") {
            if (i + 1 >= argc) {
                std::cerr << "error: --follower needs a host:port argument\n";
                return 1;
            }
            config.follower_addrs.push_back(argv[++i]);
        } else if (arg == "--raft") {
            if (i + 1 >= argc) {
                std::cerr << "error: --raft needs a host:port argument\n";
                return 1;
            }
            config.raft_peers.push_back(argv[++i]);
        } else if (arg == "--shard") {
            if (i + 1 >= argc) {
                std::cerr << "error: --shard needs a host:port argument\n";
                return 1;
            }
            config.shard_addrs.push_back(argv[++i]);
        } else if (arg == "--disk") {
            if (i + 1 >= argc) {
                std::cerr << "error: --disk needs a file path argument\n";
                return 1;
            }
            config.disk_path = argv[++i];
        } else if (arg == "--metrics-port") {
            if (i + 1 >= argc) {
                std::cerr << "error: --metrics-port needs a port number\n";
                return 1;
            }
            std::uint16_t mp = 0;
            if (!parse_port(argv[++i], &mp)) {
                std::cerr << "error: invalid metrics port '" << argv[i] << "'\n";
                return 1;
            }
            config.metrics_port = mp;
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "error: unknown option '" << arg << "'\n";
            return 1;
        } else {
            pos.push_back(arg);
        }
    }

    // Positional: [host] [port] [threads] [wal] [idle] [capacity]. A single
    // positional is treated as the port (back-compat with the early phases).
    if (pos.size() == 1) {
        if (!parse_port(pos[0], &config.port)) {
            std::cerr << "error: invalid port '" << pos[0] << "'\n";
            return 1;
        }
    } else if (pos.size() >= 2) {
        config.host = pos[0];
        if (!parse_port(pos[1], &config.port)) {
            std::cerr << "error: invalid port '" << pos[1] << "'\n";
            return 1;
        }
    }
    if (pos.size() >= 3 && !parse_workers(pos[2], &config.num_workers)) {
        std::cerr << "error: invalid thread count '" << pos[2]
                  << "' (expected 1..4096)\n";
        return 1;
    }
    if (pos.size() >= 4) {
        config.wal_path = pos[3];
    }
    if (pos.size() >= 5 &&
        !parse_timeout(pos[4], &config.idle_timeout_seconds)) {
        std::cerr << "error: invalid idle timeout '" << pos[4]
                  << "' (expected 0..86400 seconds)\n";
        return 1;
    }
    if (pos.size() >= 6 && !parse_capacity(pos[5], &config.capacity)) {
        std::cerr << "error: invalid capacity '" << pos[5]
                  << "' (expected a non-negative integer)\n";
        return 1;
    }
    if (pos.size() > 6) {
        std::cerr << "usage: redon-server [host] [port] [threads] [wal] [idle] "
                     "[capacity] [--replica] [--follower host:port ...]\n";
        return 1;
    }
    if (config.is_follower && !config.follower_addrs.empty()) {
        std::cerr << "error: a --replica cannot also have --follower targets\n";
        return 1;
    }
    if (!config.raft_peers.empty() &&
        (config.is_follower || !config.follower_addrs.empty())) {
        std::cerr << "error: --raft cannot be combined with --replica/--follower\n";
        return 1;
    }
    if (!config.shard_addrs.empty() &&
        (config.is_follower || !config.follower_addrs.empty() ||
         !config.raft_peers.empty())) {
        std::cerr << "error: --shard (router) cannot be combined with "
                     "--replica/--follower/--raft\n";
        return 1;
    }
    if (!config.disk_path.empty() && !config.shard_addrs.empty()) {
        std::cerr << "error: --disk cannot be combined with --shard "
                     "(a router stores no data)\n";
        return 1;
    }

    // Start the sockets library for the lifetime of the program (Windows needs
    // this; on POSIX it is a no-op).
    redon::net::Init net_init;
    if (!net_init.ok()) {
        std::cerr << "error: failed to initialize sockets library\n";
        return 1;
    }

    redon::Server server(std::move(config));
    return server.run();
}
