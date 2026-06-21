// metrics.h — server metrics + a Prometheus /metrics HTTP endpoint.
//
// Phase 9: make the server observable. A real database lets you watch it work —
// requests/sec, hit rate, connections, latency, key count. Metrics holds
// lock-free atomic counters updated on the hot path; it renders them two ways:
//   * a single-line summary for the INFO command (over the normal protocol), and
//   * the multi-line Prometheus exposition format for an HTTP /metrics endpoint
//     that Prometheus scrapes and Grafana graphs.
#ifndef REDON_METRICS_H
#define REDON_METRICS_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "net.h"

namespace redon {

class Metrics {
public:
    // Connection lifecycle.
    void on_connect() {
        connections_total_.fetch_add(1, std::memory_order_relaxed);
        connected_.fetch_add(1, std::memory_order_relaxed);
    }
    void on_disconnect() { connected_.fetch_sub(1, std::memory_order_relaxed); }
    long long connected() const {
        return connected_.load(std::memory_order_relaxed);
    }

    // One processed command (verb already upper-cased; is_error = reply was ERR).
    void on_command(const std::string& verb, bool is_error);
    void on_get(bool hit);          // a GET resolved to a hit or a miss
    void on_latency(long long ns);  // how long the command took

    // `keys` is the live key count from the store (the metrics object doesn't
    // own it). info_text is one line (for the INFO command); prometheus_text is
    // the multi-line exposition format (for the HTTP endpoint).
    std::string info_text(std::size_t keys) const;
    std::string prometheus_text(std::size_t keys) const;

private:
    long long uptime_seconds() const;

    std::chrono::steady_clock::time_point start_ =
        std::chrono::steady_clock::now();
    std::atomic<long long> commands_{0};
    std::atomic<long long> gets_{0};
    std::atomic<long long> sets_{0};
    std::atomic<long long> dels_{0};
    std::atomic<long long> get_hits_{0};
    std::atomic<long long> get_misses_{0};
    std::atomic<long long> errors_{0};
    std::atomic<long long> connections_total_{0};
    std::atomic<long long> connected_{0};
    std::atomic<long long> latency_ns_total_{0};
    std::atomic<long long> latency_count_{0};
};

// A minimal HTTP server that answers any request with one text body — the
// Prometheus metrics. It runs a background listener thread (Prometheus scrapes
// it every few seconds, so a single accept-at-a-time loop is plenty).
class MetricsHttp {
public:
    MetricsHttp(std::string host, std::uint16_t port,
                std::function<std::string()> render);
    ~MetricsHttp();

    MetricsHttp(const MetricsHttp&) = delete;
    MetricsHttp& operator=(const MetricsHttp&) = delete;

    bool start();  // bind+listen+spawn; false if the port couldn't be bound

private:
    void loop();

    std::string host_;
    std::uint16_t port_;
    std::function<std::string()> render_;
    net::socket_t listen_ = net::kInvalidSocket;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace redon

#endif  // REDON_METRICS_H
