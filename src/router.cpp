// router.cpp — implementation of the sharding router.
#include "router.h"

#include <cstdint>
#include <utility>

#include "net.h"

namespace redon {
namespace {

constexpr int kShardTimeoutMs = 2000;        // per-forward send/recv timeout
constexpr std::size_t kMaxReply = 128 * 1024;  // a shard reply (a value) is bounded

// FNV-1a: a tiny, well-distributed, deterministic string hash. Unlike std::hash
// it is identical across platforms and runs, so the key->shard mapping is stable.
std::uint64_t fnv1a(const std::string& s) {
    std::uint64_t h = 1469598103934665603ULL;  // FNV offset basis
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;  // FNV prime
    }
    return h;
}

bool split_host_port(const std::string& addr, std::string* host,
                     std::uint16_t* port) {
    std::size_t colon = addr.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= addr.size()) {
        return false;
    }
    *host = addr.substr(0, colon);
    try {
        int p = std::stoi(addr.substr(colon + 1));
        if (p < 1 || p > 65535) {
            return false;
        }
        *port = static_cast<std::uint16_t>(p);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool send_all(net::socket_t sock, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        int n = ::send(sock, data.data() + sent,
                       static_cast<int>(data.size() - sent), 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool recv_line(net::socket_t sock, std::string* line) {
    line->clear();
    char chunk[4096];
    for (;;) {
        int n = ::recv(sock, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (n <= 0) {
            return false;
        }
        for (int i = 0; i < n; ++i) {
            if (chunk[i] == '\n') {
                if (!line->empty() && line->back() == '\r') {
                    line->pop_back();
                }
                return true;
            }
            line->push_back(chunk[i]);
            if (line->size() > kMaxReply) {
                return false;
            }
        }
    }
}

}  // namespace

Router::Router(std::vector<std::string> shards) : shards_(std::move(shards)) {}

std::size_t Router::shard_for(const std::string& key) const {
    if (shards_.empty()) {
        return 0;
    }
    return static_cast<std::size_t>(fnv1a(key) % shards_.size());
}

std::string Router::forward(std::size_t i, const std::string& command_line) {
    if (i >= shards_.size()) {
        return "ERR ROUTER: no such shard";
    }
    std::string host;
    std::uint16_t port = 0;
    if (!split_host_port(shards_[i], &host, &port)) {
        return "ERR ROUTER: invalid shard address '" + shards_[i] + "'";
    }
    net::socket_t sock = net::connect_tcp(host, port);
    if (sock == net::kInvalidSocket) {
        return "ERR ROUTER: shard " + shards_[i] + " is unreachable";
    }
    net::set_io_timeout_ms(sock, kShardTimeoutMs);
    std::string reply;
    bool ok = send_all(sock, command_line + "\n") && recv_line(sock, &reply);
    net::close_socket(sock);
    if (!ok) {
        return "ERR ROUTER: shard " + shards_[i] + " request failed";
    }
    return reply;
}

}  // namespace redon
