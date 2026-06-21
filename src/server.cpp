// server.cpp — implementation of the TCP server.
//
// Phase 2: the accept loop hands each accepted client to a ThreadPool, so many
// clients are served concurrently instead of one at a time. Everything a worker
// thread touches is either local to its task (its own socket + buffers) or the
// shared Storage, which is internally locked — so no new locking is needed here.
#include "server.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <cctype>

#include "command.h"
#include "raft.h"
#include "replication.h"
#include "router.h"
#include "thread_pool.h"
#include "wal.h"

namespace redon {
namespace {

// A single command line is never expected to be huge. If a client keeps sending
// bytes with no newline, we refuse rather than buffer without bound (a basic
// guard against a misbehaving or malicious peer).
constexpr std::size_t kMaxLineLength = 64 * 1024;  // 64 KB

// Idle timeout for the leader's replication link on a follower. It must exceed
// the leader's heartbeat interval (2s); past this with no data the leader is
// presumed dead and the link is dropped, freeing the worker.
constexpr int kReplicaLinkTimeoutSeconds = 30;

// send() takes an int length, so never hand it more than this in a single call.
// Otherwise casting a >2 GB (len - sent) to int would overflow into a negative
// number. We just send such data across several calls instead.
constexpr std::size_t kMaxSendChunk = std::size_t(1) << 30;  // 1 GiB

// Send the whole buffer, looping because a single send() may transmit only part
// of it. Returns false if the connection broke mid-send.
bool send_all(net::socket_t sock, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        std::size_t remaining = len - sent;
        int to_send = remaining > kMaxSendChunk
                          ? static_cast<int>(kMaxSendChunk)
                          : static_cast<int>(remaining);
        int n = ::send(sock, data + sent, to_send, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Convenience: send a reply string followed by a newline terminator.
bool send_line(net::socket_t sock, const std::string& reply) {
    std::string out = reply;
    out.push_back('\n');
    return send_all(sock, out.data(), out.size());
}

// RAII for one served connection. It bumps the live-connection counter on
// construction and, on destruction — whether the task returns normally OR an
// exception unwinds through it — closes the socket and drops the counter. Both
// cleanup steps are noexcept, so running them in a destructor is safe.
struct ConnectionScope {
    std::atomic<int>* counter;
    net::socket_t sock;

    ConnectionScope(std::atomic<int>* c, net::socket_t s) : counter(c), sock(s) {
        counter->fetch_add(1);
    }
    ~ConnectionScope() {
        net::close_socket(sock);
        counter->fetch_sub(1);
    }
    ConnectionScope(const ConnectionScope&) = delete;
    ConnectionScope& operator=(const ConnectionScope&) = delete;
};

// ASCII whitespace, matching command.cpp's tokenizer exactly (NOT std::isspace,
// which is locale-dependent) so the router splits a key on the same byte
// boundaries the shard will — even for non-ASCII key bytes under any locale.
bool is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
           c == '\f';
}

// Pull the first two whitespace-delimited tokens out of a command line: the verb
// (upper-cased) and the key. Used by the router to decide where to forward.
void verb_and_key(const std::string& line, std::string* verb, std::string* key) {
    std::size_t i = 0;
    const std::size_t n = line.size();
    auto skip_ws = [&] {
        while (i < n && is_space(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
    };
    auto token = [&] {
        std::size_t start = i;
        while (i < n && !is_space(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        return line.substr(start, i - start);
    };
    skip_ws();
    *verb = token();
    skip_ws();
    *key = token();
    for (char& c : *verb) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
}

}  // namespace

Server::Server(ServerConfig config)
    : config_(std::move(config)), store_(config_.capacity) {}

// Defined here (where Wal/Replicator are complete) so the unique_ptr members can
// be destroyed. The Replicator stops and joins its threads in its destructor;
// declaring it after store_ means it is destroyed first, while store_ is alive.
Server::~Server() = default;

void Server::log(const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    // Flush: these are infrequent connection-level events, and a log you can't
    // see (because it's stuck in a buffer when the process is killed) is useless.
    std::cout << message << std::endl;
}

bool Server::setup_persistence() {
    const std::string& wal_path = config_.wal_path;
    // Persistence off?
    if (wal_path.empty() || wal_path == "none" || wal_path == "-") {
        std::cout << "Persistence: disabled (in-memory only)\n";
        return true;
    }

    wal_ = std::make_unique<Wal>(wal_path);

    // 1. Rebuild state from any existing log BEFORE the WAL is attached, so the
    //    replayed set()/del() calls don't get re-logged.
    std::size_t replayed = wal_->replay_into(store_);

    // 2. Open the log for appending new writes.
    if (!wal_->open_for_append()) {
        std::cerr << "error: could not open WAL file '" << wal_path
                  << "' for writing\n";
        return false;
    }

    // 3. Attach it so every future SET/DEL is recorded before it takes effect.
    store_.attach_wal(wal_.get());

    std::cout << "Persistence: " << wal_path << " (replayed " << replayed
              << " record" << (replayed == 1 ? "" : "s") << ")\n";
    return true;
}

void Server::setup_replication() {
    if (config_.is_follower) {
        std::cout << "Role: follower (read-only replica)\n";
        return;
    }
    if (config_.follower_addrs.empty()) {
        std::cout << "Role: leader (standalone, no followers)\n";
        return;
    }
    // Leader with followers: start streaming writes to them.
    replicator_ =
        std::make_unique<Replicator>(config_.follower_addrs, &store_);
    store_.attach_replicator(replicator_.get());
    replicator_->start();
    std::cout << "Role: leader, replicating to " << replicator_->follower_count()
              << " follower(s)\n";
}

void Server::setup_raft() {
    if (config_.raft_peers.empty()) {
        return;  // not a Raft cluster
    }
    RaftNode::Config rc;
    rc.self_id = config_.host + ":" + std::to_string(config_.port);
    rc.peers = config_.raft_peers;
    raft_ = std::make_unique<RaftNode>(std::move(rc));
    raft_->start();
    std::cout << "Raft: cluster member " << config_.host << ":" << config_.port
              << " with " << config_.raft_peers.size()
              << " peer(s); electing a leader...\n";
}

void Server::setup_sharding() {
    if (config_.shard_addrs.empty()) {
        return;  // not a router
    }
    router_ = std::make_unique<Router>(config_.shard_addrs);
    std::cout << "Role: router, sharding keys across " << router_->shard_count()
              << " shard(s)\n";
}

std::string Server::route_line(const std::string& line, bool* should_close) {
    *should_close = false;
    std::string verb;
    std::string key;
    verb_and_key(line, &verb, &key);
    if (verb.empty()) {
        return "";  // blank line
    }
    if (verb == "QUIT" || verb == "EXIT") {
        *should_close = true;
        return "OK";
    }
    if (verb == "PING") {
        return "PONG";  // keyless: answer locally
    }
    if (key.empty()) {
        return "ERR ROUTER: '" + verb + "' needs a key to route on";
    }
    // Hash the key to a shard and forward the whole command line there.
    return router_->forward(router_->shard_for(key), line);
}

int Server::run() {
    // 0. Set up storage (on-disk engine OR in-memory + WAL), then replication,
    //    Raft, and sharding, before accepting clients.
    if (!config_.disk_path.empty()) {
        // Phase 8: the on-disk engine is its own durability — no WAL.
        if (!store_.open_disk_backend(config_.disk_path)) {
            std::cerr << "error: could not open disk store '" << config_.disk_path
                      << "'\n";
            return 1;
        }
        std::cout << "Persistence: on-disk engine at " << config_.disk_path
                  << " (" << store_.size() << " key(s) loaded)\n";
    } else if (!setup_persistence()) {
        return 1;
    }
    setup_replication();
    setup_raft();
    setup_sharding();

    // 1. Create the listening socket (IPv4, TCP).
    net::socket_t listen_sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == net::kInvalidSocket) {
        std::cerr << "error: could not create socket: "
                  << net::last_error_str() << "\n";
        return 1;
    }
    // RAII: closes listen_sock on every path below — early error returns and the
    // stack unwinding if the ThreadPool constructor throws (e.g. out of threads).
    net::SocketCloser listen_guard(listen_sock);

    // Allow immediate reuse of the address so restarting the server doesn't fail
    // with "address already in use" while the OS holds the port in TIME_WAIT.
    int reuse = 1;
    ::setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    // 2. Bind to host:port.
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "error: invalid host address '" << config_.host << "'\n";
        return 1;
    }
    if (::bind(listen_sock, reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) != 0) {
        std::cerr << "error: could not bind " << config_.host << ":"
                  << config_.port << ": " << net::last_error_str() << "\n";
        return 1;
    }

    // 3. Mark the socket as listening for incoming connections.
    if (::listen(listen_sock, SOMAXCONN) != 0) {
        std::cerr << "error: could not listen: " << net::last_error_str()
                  << "\n";
        return 1;
    }

    std::cout << "Redon server listening on " << config_.host << ":"
              << config_.port << "\n";
    std::cout << "(thread pool of " << config_.num_workers << " workers; ";
    if (config_.idle_timeout_seconds > 0) {
        std::cout << "idle timeout " << config_.idle_timeout_seconds << "s";
    } else {
        std::cout << "no idle timeout";
    }
    if (store_.capacity() > 0) {
        std::cout << "; capacity " << store_.capacity() << " keys (LRU)";
    } else {
        std::cout << "; unbounded";
    }
    std::cout << ". Ctrl+C to stop.)\n";
    // Flush the startup banner now: stdout is block-buffered when redirected to a
    // file, and an abrupt kill would otherwise discard these lines.
    std::cout.flush();

    // Cap the queue so a connection flood can't pile up unbounded sockets faster
    // than the workers drain them. Past the cap we reject (and close) new clients.
    std::size_t max_queued = config_.num_workers * 16;
    if (max_queued < 256) {
        max_queued = 256;
    }

    try {
        // The pool lives for the whole accept loop; if run() returns, its
        // destructor drains the queue and joins the workers.
        ThreadPool pool(config_.num_workers, max_queued);

        // 4. Accept loop: take each client and HAND IT TO THE POOL, then go
        // straight back to accepting. A free worker serves the client to
        // completion while the main thread keeps accepting new connections.
        for (;;) {
            net::socket_t client = ::accept(listen_sock, nullptr, nullptr);
            if (client == net::kInvalidSocket) {
                // A failed accept() is not fatal; log and keep going.
                log("warning: accept failed: " + net::last_error_str());
                continue;
            }
            // Keepalive detects a peer that silently vanishes; the recv timeout
            // disconnects a client that just sits there sending nothing, so it
            // can't hold a worker hostage.
            net::set_keepalive(client);
            net::set_recv_timeout(client, config_.idle_timeout_seconds);

            // Owns the socket until we successfully hand it to the pool; closes
            // it if submission fails (overloaded) or throws.
            net::SocketCloser client_guard(client);

            // Capture `this` and the socket handle by value. The lambda runs on a
            // worker thread; `this` stays valid because the Server outlives the
            // pool. ConnectionScope guarantees the socket is closed and the
            // counter decremented even if handle_client throws.
            bool submitted = false;
            try {
                submitted = pool.submit([this, client] {
                    ConnectionScope scope(&active_clients_, client);
                    try {
                        log("client connected (active: " +
                            std::to_string(active_clients_.load()) + ")");
                        handle_client(client);
                        log("client disconnected (active: " +
                            std::to_string(active_clients_.load() - 1) + ")");
                    } catch (...) {
                        // One client's failure must not crash the worker;
                        // ConnectionScope still cleans up.
                    }
                });
            } catch (...) {
                submitted = false;  // e.g. allocation failure building the task
            }

            if (submitted) {
                client_guard.release();  // the task now owns the socket
            } else {
                log("warning: server overloaded, rejecting client");
                // client_guard closes the socket as it goes out of scope.
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "error: thread pool failure: " << e.what() << "\n";
        return 1;  // listen_guard closes the listening socket
    }

    // Unreachable in normal operation.
}

void Server::handle_client(net::socket_t client) {
    std::string inbuf;   // holds bytes received but not yet split into lines
    char chunk[4096];
    // Set true once this connection sends the leader's replication handshake.
    // After that it is the replicated write stream: writes are applied even on a
    // follower, and we send no replies back (the leader doesn't read them).
    bool is_replica_link = false;

    for (;;) {
        // If the unprocessed buffer already holds an over-long partial line,
        // reject before reading more into it. (A line may run up to one recv()
        // chunk past the cap before we notice it here, which is acceptable.)
        if (inbuf.size() > kMaxLineLength) {
            send_line(client, "ERR line too long");
            return;
        }

        int n = ::recv(client, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (n == 0) {
            return;  // peer closed the connection cleanly
        }
        if (n < 0) {
            // A receive timeout means the client went idle; close it rather than
            // hold the worker. Anything else is a genuine connection error.
            if (net::is_timeout_error(net::last_error())) {
                log("client idle timeout, closing connection");
            } else {
                log("warning: recv failed: " + net::last_error_str());
            }
            return;
        }
        inbuf.append(chunk, static_cast<std::size_t>(n));

        // Pull out every complete line (terminated by '\n') we now have. TCP is
        // a byte stream, so one recv() may contain several lines, a partial
        // line, or both — this loop handles all cases.
        std::size_t newline;
        while ((newline = inbuf.find('\n')) != std::string::npos) {
            std::string line = inbuf.substr(0, newline);
            inbuf.erase(0, newline + 1);

            const bool was_replica_link = is_replica_link;
            bool should_close = false;
            std::string reply =
                router_ != nullptr
                    ? route_line(line, &should_close)  // forward to the owning shard
                    : execute_line(line, store_, &should_close,
                                   config_.is_follower, &is_replica_link,
                                   raft_.get());
            if (!was_replica_link && is_replica_link) {
                // This connection just became the leader's replication stream,
                // not an ordinary client: use the longer replication timeout so a
                // quiet period doesn't tear it down, but a truly dead leader (no
                // heartbeat for kReplicaLinkTimeoutSeconds) is still detected.
                net::set_recv_timeout(client, kReplicaLinkTimeoutSeconds);
            }
            // Reply to ordinary clients and to the handshake itself, but stay
            // silent for the replicated stream that follows (was_replica_link).
            if (!was_replica_link) {
                if (!send_line(client, reply)) {
                    return;  // connection broke while replying
                }
            }
            if (should_close) {
                return;  // client asked to QUIT
            }
        }
    }
}

}  // namespace redon
