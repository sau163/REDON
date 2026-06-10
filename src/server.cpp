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
#include <string>
#include <utility>

#include "command.h"
#include "thread_pool.h"

namespace redon {
namespace {

// A single command line is never expected to be huge. If a client keeps sending
// bytes with no newline, we refuse rather than buffer without bound (a basic
// guard against a misbehaving or malicious peer).
constexpr std::size_t kMaxLineLength = 64 * 1024;  // 64 KB

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

}  // namespace

Server::Server(std::string host, std::uint16_t port, std::size_t num_workers)
    : host_(std::move(host)), port_(port), num_workers_(num_workers) {}

void Server::log(const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    std::cout << message << "\n";
}

int Server::run() {
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
    addr.sin_port = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "error: invalid host address '" << host_ << "'\n";
        return 1;
    }
    if (::bind(listen_sock, reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) != 0) {
        std::cerr << "error: could not bind " << host_ << ":" << port_ << ": "
                  << net::last_error_str() << "\n";
        return 1;
    }

    // 3. Mark the socket as listening for incoming connections.
    if (::listen(listen_sock, SOMAXCONN) != 0) {
        std::cerr << "error: could not listen: " << net::last_error_str()
                  << "\n";
        return 1;
    }

    std::cout << "Redon server listening on " << host_ << ":" << port_ << "\n";
    std::cout << "(Phase 2: thread pool of " << num_workers_
              << " workers. Ctrl+C to stop.)\n";

    // Cap the queue so a connection flood can't pile up unbounded sockets faster
    // than the workers drain them. Past the cap we reject (and close) new clients.
    std::size_t max_queued = num_workers_ * 16;
    if (max_queued < 256) {
        max_queued = 256;
    }

    try {
        // The pool lives for the whole accept loop; if run() returns, its
        // destructor drains the queue and joins the workers.
        ThreadPool pool(num_workers_, max_queued);

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
            log("warning: recv failed: " + net::last_error_str());
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

            bool should_close = false;
            std::string reply = execute_line(line, store_, &should_close);
            if (!send_line(client, reply)) {
                return;  // connection broke while replying
            }
            if (should_close) {
                return;  // client asked to QUIT
            }
        }
    }
}

}  // namespace redon
