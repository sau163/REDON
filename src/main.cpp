// main.cpp — entry point for the `redon-server` executable.
//
// Usage:
//   redon-server                          # 127.0.0.1:6380, default thread count
//   redon-server <port>                   # 127.0.0.1:<port>
//   redon-server <host> <port>            # <host>:<port>
//   redon-server <host> <port> <threads>  # ...with a custom worker-pool size
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

}  // namespace

int main(int argc, char** argv) {
    std::string host = kDefaultHost;
    std::uint16_t port = kDefaultPort;
    std::size_t workers = default_workers();

    // Positional args: [host] [port] [threads]. A bare port is allowed as the
    // single-arg form; the thread count requires host and port to be given too.
    if (argc == 2) {
        if (!parse_port(argv[1], &port)) {
            std::cerr << "error: invalid port '" << argv[1] << "'\n";
            return 1;
        }
    } else if (argc == 3) {
        host = argv[1];
        if (!parse_port(argv[2], &port)) {
            std::cerr << "error: invalid port '" << argv[2] << "'\n";
            return 1;
        }
    } else if (argc == 4) {
        host = argv[1];
        if (!parse_port(argv[2], &port)) {
            std::cerr << "error: invalid port '" << argv[2] << "'\n";
            return 1;
        }
        if (!parse_workers(argv[3], &workers)) {
            std::cerr << "error: invalid thread count '" << argv[3]
                      << "' (expected 1..4096)\n";
            return 1;
        }
    } else if (argc > 4) {
        std::cerr << "usage: redon-server [host] [port] [threads]\n";
        return 1;
    }

    // Start the sockets library for the lifetime of the program (Windows needs
    // this; on POSIX it is a no-op).
    redon::net::Init net_init;
    if (!net_init.ok()) {
        std::cerr << "error: failed to initialize sockets library\n";
        return 1;
    }

    redon::Server server(host, port, workers);
    return server.run();
}
