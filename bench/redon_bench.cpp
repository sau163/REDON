// redon_bench.cpp — a concurrent load generator for redon-server.
//
// It opens N connections at once, each on its own thread, and fires a stream of
// SET/GET requests, measuring throughput and latency. Besides reporting numbers,
// it is a stress test for Phase 2: every connection hammers the one shared
// Storage at the same time, so if the storage mutex were missing or the thread
// pool mishandled a connection, the verification below would catch it.
//
// Usage:
//   redon-bench [host] [port] [connections] [requests-per-connection]
//   redon-bench 127.0.0.1 6380 50 1000
//
// Each "iteration" performs one SET and one GET (two requests). The GET is
// checked against the value the SET stored, so a mismatch (data corruption or a
// crossed-wire reply) is reported as an error.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "net.h"

namespace {

constexpr std::size_t kMaxLineLength = 64 * 1024;
constexpr std::size_t kMaxSendChunk = std::size_t(1) << 30;

bool parse_long(const std::string& text, long min, long max, long* out) {
    try {
        std::size_t consumed = 0;
        long value = std::stol(text, &consumed);
        if (consumed != text.size() || value < min || value > max) {
            return false;
        }
        *out = value;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Open a TCP connection to host:port. Returns kInvalidSocket on failure.
redon::net::socket_t connect_to(const std::string& host, std::uint16_t port) {
    redon::net::socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == redon::net::kInvalidSocket) {
        return redon::net::kInvalidSocket;
    }
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        redon::net::close_socket(sock);
        return redon::net::kInvalidSocket;
    }
    return sock;
}

bool send_all(redon::net::socket_t sock, const char* data, std::size_t len) {
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

bool recv_line(redon::net::socket_t sock, std::string* inbuf, std::string* out) {
    std::size_t newline;
    while ((newline = inbuf->find('\n')) == std::string::npos) {
        char chunk[4096];
        int n = ::recv(sock, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (n <= 0) {
            return false;
        }
        inbuf->append(chunk, static_cast<std::size_t>(n));
        if (inbuf->size() > kMaxLineLength) {
            return false;
        }
    }
    *out = inbuf->substr(0, newline);
    inbuf->erase(0, newline + 1);
    if (!out->empty() && out->back() == '\r') {
        out->pop_back();
    }
    return true;
}

// Per-connection results, combined at the end.
struct Result {
    long long requests = 0;
    long long errors = 0;
    long long total_ns = 0;
    long long max_ns = 0;
};

// One round trip: send `line`, read the reply, time it, and check the reply.
void timed_request(redon::net::socket_t sock, std::string* inbuf,
                   const std::string& line, const std::string& expected,
                   Result* r) {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    std::string reply;
    bool ok = send_all(sock, line.data(), line.size()) &&
              recv_line(sock, inbuf, &reply);
    auto t1 = clock::now();

    long long ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    r->requests += 1;
    r->total_ns += ns;
    if (ns > r->max_ns) {
        r->max_ns = ns;
    }
    if (!ok || reply != expected) {
        r->errors += 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6380;
    long connections = 50;
    long requests_per_conn = 1000;

    long tmp = 0;
    if (argc >= 2) host = argv[1];
    if (argc >= 3) {
        if (!parse_long(argv[2], 1, 65535, &tmp)) {
            std::cerr << "error: invalid port\n";
            return 1;
        }
        port = static_cast<std::uint16_t>(tmp);
    }
    if (argc >= 4) {
        if (!parse_long(argv[3], 1, 4096, &connections)) {
            std::cerr << "error: invalid connection count (1..4096)\n";
            return 1;
        }
    }
    if (argc >= 5) {
        if (!parse_long(argv[4], 1, 10000000, &requests_per_conn)) {
            std::cerr << "error: invalid requests-per-connection\n";
            return 1;
        }
    }

    redon::net::Init net_init;
    if (!net_init.ok()) {
        std::cerr << "error: failed to initialize sockets library\n";
        return 1;
    }

    std::cout << "Benchmarking " << host << ":" << port << " with "
              << connections << " connections x " << requests_per_conn
              << " iterations (SET+GET each)...\n";

    std::vector<Result> results(static_cast<std::size_t>(connections));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(connections));
    std::atomic<long long> connect_failures{0};

    auto worker = [&](long id) {
        Result& r = results[static_cast<std::size_t>(id)];
        redon::net::socket_t sock = connect_to(host, port);
        if (sock == redon::net::kInvalidSocket) {
            connect_failures.fetch_add(1);
            return;
        }
        std::string inbuf;
        const std::string prefix = "k_" + std::to_string(id) + "_";
        for (long i = 0; i < requests_per_conn; ++i) {
            const std::string key = prefix + std::to_string(i);
            const std::string val = "v_" + std::to_string(id) + "_" +
                                    std::to_string(i);
            timed_request(sock, &inbuf, "SET " + key + " " + val + "\n", "OK",
                          &r);
            timed_request(sock, &inbuf, "GET " + key + "\n", val, &r);
        }
        send_all(sock, "QUIT\n", 5);
        std::string reply;
        recv_line(sock, &inbuf, &reply);
        redon::net::close_socket(sock);
    };

    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    for (long id = 0; id < connections; ++id) {
        threads.emplace_back(worker, id);
    }
    for (std::thread& t : threads) {
        t.join();
    }
    auto end = clock::now();

    // Combine the per-connection results.
    long long total_requests = 0;
    long long total_errors = 0;
    long long sum_ns = 0;
    long long max_ns = 0;
    for (const Result& r : results) {
        total_requests += r.requests;
        total_errors += r.errors;
        sum_ns += r.total_ns;
        if (r.max_ns > max_ns) {
            max_ns = r.max_ns;
        }
    }

    double elapsed_s =
        std::chrono::duration_cast<std::chrono::duration<double>>(end - start)
            .count();
    double throughput = elapsed_s > 0 ? total_requests / elapsed_s : 0.0;
    double avg_us =
        total_requests > 0 ? (static_cast<double>(sum_ns) / total_requests) /
                                 1000.0
                           : 0.0;

    std::cout << "------------------------------------------\n";
    std::cout << "connections        : " << connections << "\n";
    std::cout << "connect failures   : " << connect_failures.load() << "\n";
    std::cout << "total requests     : " << total_requests << "\n";
    std::cout << "request errors     : " << total_errors << "\n";
    std::cout << "elapsed            : " << elapsed_s << " s\n";
    std::cout << "throughput         : " << static_cast<long long>(throughput)
              << " req/s\n";
    std::cout << "avg latency        : " << avg_us << " us\n";
    std::cout << "max latency        : " << (max_ns / 1000) << " us\n";

    // Exit non-zero if anything went wrong, so scripts can detect failure.
    return (total_errors == 0 && connect_failures.load() == 0) ? 0 : 1;
}
