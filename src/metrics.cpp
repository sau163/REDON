// metrics.cpp — implementation of server metrics and the HTTP endpoint.
#include "metrics.h"

#include <iomanip>
#include <sstream>
#include <utility>

namespace redon {

void Metrics::on_command(const std::string& verb, bool is_error) {
    commands_.fetch_add(1, std::memory_order_relaxed);
    if (is_error) {
        errors_.fetch_add(1, std::memory_order_relaxed);
    }
    if (verb == "GET") {
        gets_.fetch_add(1, std::memory_order_relaxed);
    } else if (verb == "SET") {
        sets_.fetch_add(1, std::memory_order_relaxed);
    } else if (verb == "DEL" || verb == "DELETE") {
        dels_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Metrics::on_get(bool hit) {
    (hit ? get_hits_ : get_misses_).fetch_add(1, std::memory_order_relaxed);
}

void Metrics::on_latency(long long ns) {
    latency_ns_total_.fetch_add(ns, std::memory_order_relaxed);
    latency_count_.fetch_add(1, std::memory_order_relaxed);
}

long long Metrics::uptime_seconds() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - start_)
        .count();
}

std::string Metrics::info_text(std::size_t keys) const {
    const long long hits = get_hits_.load(std::memory_order_relaxed);
    const long long misses = get_misses_.load(std::memory_order_relaxed);
    const long long reads = hits + misses;
    const double hit_rate = reads > 0 ? static_cast<double>(hits) / reads : 0.0;
    const long long lat_n = latency_count_.load(std::memory_order_relaxed);
    const double avg_us =
        lat_n > 0 ? static_cast<double>(latency_ns_total_.load(
                        std::memory_order_relaxed)) /
                        lat_n / 1000.0
                  : 0.0;

    std::ostringstream os;
    os << std::fixed << std::setprecision(3);
    os << "uptime_s=" << uptime_seconds()
       << " clients=" << connected()
       << " conns_total=" << connections_total_.load(std::memory_order_relaxed)
       << " commands=" << commands_.load(std::memory_order_relaxed)
       << " get=" << gets_.load(std::memory_order_relaxed)
       << " set=" << sets_.load(std::memory_order_relaxed)
       << " del=" << dels_.load(std::memory_order_relaxed)
       << " hits=" << hits << " misses=" << misses
       << " hit_rate=" << hit_rate
       << " errors=" << errors_.load(std::memory_order_relaxed)
       << " avg_latency_us=" << avg_us
       << " keys=" << keys;
    return os.str();
}

std::string Metrics::prometheus_text(std::size_t keys) const {
    const long long hits = get_hits_.load(std::memory_order_relaxed);
    const long long misses = get_misses_.load(std::memory_order_relaxed);
    const long long lat_n = latency_count_.load(std::memory_order_relaxed);
    const double avg_us =
        lat_n > 0 ? static_cast<double>(latency_ns_total_.load(
                        std::memory_order_relaxed)) /
                        lat_n / 1000.0
                  : 0.0;

    std::ostringstream os;
    os << std::fixed << std::setprecision(3);
    os << "# HELP redon_uptime_seconds Server uptime in seconds.\n"
          "# TYPE redon_uptime_seconds gauge\n"
          "redon_uptime_seconds " << uptime_seconds() << "\n";
    os << "# HELP redon_connected_clients Currently connected clients.\n"
          "# TYPE redon_connected_clients gauge\n"
          "redon_connected_clients " << connected() << "\n";
    os << "# HELP redon_connections_total Connections accepted since start.\n"
          "# TYPE redon_connections_total counter\n"
          "redon_connections_total "
       << connections_total_.load(std::memory_order_relaxed) << "\n";
    os << "# HELP redon_commands_total Commands processed since start.\n"
          "# TYPE redon_commands_total counter\n"
          "redon_commands_total "
       << commands_.load(std::memory_order_relaxed) << "\n";
    os << "# HELP redon_command_total Commands processed, by verb.\n"
          "# TYPE redon_command_total counter\n"
          "redon_command_total{cmd=\"get\"} "
       << gets_.load(std::memory_order_relaxed) << "\n"
       << "redon_command_total{cmd=\"set\"} "
       << sets_.load(std::memory_order_relaxed) << "\n"
       << "redon_command_total{cmd=\"del\"} "
       << dels_.load(std::memory_order_relaxed) << "\n";
    os << "# HELP redon_keyspace_hits_total GET hits.\n"
          "# TYPE redon_keyspace_hits_total counter\n"
          "redon_keyspace_hits_total " << hits << "\n";
    os << "# HELP redon_keyspace_misses_total GET misses.\n"
          "# TYPE redon_keyspace_misses_total counter\n"
          "redon_keyspace_misses_total " << misses << "\n";
    os << "# HELP redon_errors_total Commands that returned an error.\n"
          "# TYPE redon_errors_total counter\n"
          "redon_errors_total "
       << errors_.load(std::memory_order_relaxed) << "\n";
    os << "# HELP redon_command_latency_microseconds_avg Mean command latency.\n"
          "# TYPE redon_command_latency_microseconds_avg gauge\n"
          "redon_command_latency_microseconds_avg " << avg_us << "\n";
    os << "# HELP redon_keys Number of keys stored.\n"
          "# TYPE redon_keys gauge\n"
          "redon_keys " << keys << "\n";
    return os.str();
}

// ---------------------------------------------------------------------------

MetricsHttp::MetricsHttp(std::string host, std::uint16_t port,
                         std::function<std::string()> render)
    : host_(std::move(host)), port_(port), render_(std::move(render)) {}

MetricsHttp::~MetricsHttp() {
    running_.store(false);
    if (listen_ != net::kInvalidSocket) {
        net::close_socket(listen_);  // unblocks accept() in loop()
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool MetricsHttp::start() {
    listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_ == net::kInvalidSocket) {
        return false;
    }
    int reuse = 1;
    ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1 ||
        ::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listen_, 16) != 0) {
        net::close_socket(listen_);
        listen_ = net::kInvalidSocket;
        return false;
    }
    running_.store(true);
    thread_ = std::thread([this] { loop(); });
    return true;
}

void MetricsHttp::loop() {
    while (running_.load()) {
        net::socket_t client = ::accept(listen_, nullptr, nullptr);
        if (client == net::kInvalidSocket) {
            if (!running_.load()) {
                break;  // shutting down: the listen socket was closed
            }
            continue;
        }
        // Read and discard the HTTP request (we serve metrics at any path). A
        // short timeout keeps a misbehaving client from stalling the loop.
        net::set_recv_timeout(client, 2);
        char buf[2048];
        ::recv(client, buf, static_cast<int>(sizeof(buf)), 0);

        const std::string body = render_ ? render_() : std::string();
        std::string resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
            "Content-Length: " +
            std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;

        std::size_t sent = 0;
        while (sent < resp.size()) {
            int n = ::send(client, resp.data() + sent,
                           static_cast<int>(resp.size() - sent), 0);
            if (n <= 0) {
                break;
            }
            sent += static_cast<std::size_t>(n);
        }
        net::close_socket(client);
    }
}

}  // namespace redon
