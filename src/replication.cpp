// replication.cpp — implementation of the leader-side Replicator.
#include "replication.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <utility>

#include "net.h"
#include "storage.h"

namespace redon {
namespace {

constexpr std::size_t kMaxQueue = 100000;  // per-follower backlog before resync
constexpr int kReconnectSeconds = 1;       // delay between connection attempts
constexpr int kIoTimeoutSeconds = 5;       // give up on a stalled follower
constexpr int kHeartbeatSeconds = 2;       // ping an idle follower this often

// Split "host:port" into its parts. Returns false if malformed.
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

net::socket_t connect_to(const std::string& host, std::uint16_t port) {
    net::socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == net::kInvalidSocket) {
        return net::kInvalidSocket;
    }
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        net::close_socket(sock);
        return net::kInvalidSocket;
    }
    return sock;
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

// Read one '\n'-terminated line (used only for the small handshake reply).
bool recv_line(net::socket_t sock, std::string* line) {
    line->clear();
    char c;
    for (;;) {
        int n = ::recv(sock, &c, 1, 0);
        if (n <= 0) {
            return false;
        }
        if (c == '\n') {
            return true;
        }
        if (line->size() < 4096) {
            line->push_back(c);
        }
    }
}

std::string set_line(const std::string& key, const std::string& value) {
    return "SET " + key + " " + value + "\n";
}
std::string del_line(const std::string& key) { return "DEL " + key + "\n"; }

}  // namespace

Replicator::Replicator(std::vector<std::string> follower_addrs, Storage* storage)
    : storage_(storage) {
    for (const std::string& addr : follower_addrs) {
        auto link = std::unique_ptr<Link>(new Link());
        if (!split_host_port(addr, &link->host, &link->port)) {
            std::cerr << "[repl] ignoring invalid follower address '" << addr
                      << "'\n";
            continue;
        }
        links_.push_back(std::move(link));
    }
}

Replicator::~Replicator() { stop(); }

void Replicator::start() {
    started_ = true;
    for (auto& link : links_) {
        Link* l = link.get();
        l->thread = std::thread([this, l] { sender_loop(l); });
    }
}

void Replicator::stop() {
    for (auto& link : links_) {
        {
            std::lock_guard<std::mutex> lk(link->mutex);
            link->stop = true;
        }
        link->cv.notify_all();
    }
    for (auto& link : links_) {
        if (link->thread.joinable()) {
            link->thread.join();
        }
    }
}

void Replicator::replicate_set(const std::string& key,
                               const std::string& value) {
    enqueue(set_line(key, value));
}

void Replicator::replicate_del(const std::string& key) {
    enqueue(del_line(key));
}

void Replicator::enqueue(const std::string& line) {
    for (auto& link : links_) {
        std::lock_guard<std::mutex> lk(link->mutex);
        if (!link->streaming) {
            continue;  // not synced yet: the snapshot will cover this write
        }
        if (link->queue.size() >= kMaxQueue) {
            // The follower can't keep up. Drop the backlog and force a fresh full
            // sync so it converges again rather than buffering without bound.
            link->queue.clear();
            link->streaming = false;
            link->resync_needed = true;
            link->cv.notify_all();
            continue;
        }
        link->queue.push_back(line);
        link->cv.notify_one();
    }
}

void Replicator::sender_loop(Link* link) {
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(link->mutex);
            if (link->stop) {
                return;
            }
        }

        net::socket_t sock = connect_to(link->host, link->port);
        bool connected = (sock != net::kInvalidSocket);

        if (connected) {
            net::set_keepalive(sock);
            net::set_send_timeout(sock, kIoTimeoutSeconds);
            net::set_recv_timeout(sock, kIoTimeoutSeconds);

            // Handshake: tell the follower to reset and become our replica link.
            std::string ack;
            if (send_all(sock, "__REPLSYNC__\n") && recv_line(sock, &ack)) {
                std::cerr << "[repl] synced with follower " << link->host << ":"
                          << link->port << "\n";

                // Atomic cut: snapshot AND start streaming under the storage lock
                // so no write is missed or duplicated across the handover.
                std::vector<std::pair<std::string, std::string>> snapshot =
                    storage_->snapshot_locked([link] {
                        std::lock_guard<std::mutex> lk(link->mutex);
                        link->queue.clear();
                        link->resync_needed = false;
                        link->streaming = true;
                    });

                bool ok = true;
                for (const auto& kv : snapshot) {
                    if (!send_all(sock, set_line(kv.first, kv.second))) {
                        ok = false;
                        break;
                    }
                }

                // Stream live writes until disconnect / resync / shutdown. When
                // idle, send a PING heartbeat so the link stays alive AND a dead
                // follower is detected promptly (the send fails) instead of only
                // when the next real write happens to arrive.
                while (ok) {
                    std::string line;
                    bool have_line = false;
                    {
                        std::unique_lock<std::mutex> lk(link->mutex);
                        link->cv.wait_for(
                            lk, std::chrono::seconds(kHeartbeatSeconds), [link] {
                                return link->stop || link->resync_needed ||
                                       !link->queue.empty();
                            });
                        if (link->stop) {
                            net::close_socket(sock);
                            return;
                        }
                        if (link->resync_needed) {
                            link->resync_needed = false;
                            ok = false;  // reconnect & full-sync again
                            break;
                        }
                        if (!link->queue.empty()) {
                            line = std::move(link->queue.front());
                            link->queue.pop_front();
                            have_line = true;
                        }
                    }
                    if (!send_all(sock, have_line ? line : std::string("PING\n"))) {
                        ok = false;
                    }
                }
            }
            net::close_socket(sock);
        }

        // Stop streaming and drop any backlog; the next connect does a full sync.
        {
            std::lock_guard<std::mutex> lk(link->mutex);
            link->streaming = false;
            link->queue.clear();
            if (link->stop) {
                return;
            }
        }

        // Wait before reconnecting (interruptible on stop).
        std::unique_lock<std::mutex> lk(link->mutex);
        link->cv.wait_for(lk, std::chrono::seconds(kReconnectSeconds),
                          [link] { return link->stop; });
        if (link->stop) {
            return;
        }
    }
}

}  // namespace redon
