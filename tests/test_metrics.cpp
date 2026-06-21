// test_metrics.cpp — tests for the metrics counters and renderers.
#include <string>

#include "metrics.h"
#include "test_util.h"

using redon::Metrics;

namespace {
bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}
}  // namespace

// Counters add up and the single-line INFO summary reflects them.
void test_counters_and_info() {
    Metrics m;
    m.on_connect();
    m.on_connect();  // 2 active
    CHECK_EQ(m.connected(), static_cast<long long>(2));
    m.on_disconnect();  // 1 active
    CHECK_EQ(m.connected(), static_cast<long long>(1));

    m.on_command("GET", false);
    m.on_get(true);  // a hit
    m.on_command("GET", false);
    m.on_get(false);  // a miss
    m.on_command("SET", false);
    m.on_command("DEL", false);
    m.on_command("BOGUS", true);  // an error
    m.on_latency(1000);
    m.on_latency(3000);

    const std::string info = m.info_text(42);
    CHECK(contains(info, "clients=1"));
    CHECK(contains(info, "commands=5"));
    CHECK(contains(info, "get=2"));
    CHECK(contains(info, "set=1"));
    CHECK(contains(info, "del=1"));
    CHECK(contains(info, "hits=1"));
    CHECK(contains(info, "misses=1"));
    CHECK(contains(info, "errors=1"));
    CHECK(contains(info, "keys=42"));
    // INFO must be ONE line so it fits the line-based reply protocol.
    CHECK(info.find('\n') == std::string::npos);
}

// The Prometheus exposition format has the expected HELP/TYPE lines and values.
void test_prometheus_format() {
    Metrics m;
    m.on_command("SET", false);
    const std::string p = m.prometheus_text(7);
    CHECK(contains(p, "# TYPE redon_commands_total counter"));
    CHECK(contains(p, "redon_commands_total 1"));
    CHECK(contains(p, "redon_command_total{cmd=\"set\"} 1"));
    CHECK(contains(p, "redon_keys 7"));
    CHECK(contains(p, "redon_connected_clients 0"));
}

int main() {
    RUN(test_counters_and_info);
    RUN(test_prometheus_format);
    return REPORT();
}
