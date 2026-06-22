# Phase 9 — explained (monitoring)

A database that *works* but that you can't *see* is operationally blind — you
can't tell if it's healthy, busy, or about to fall over. Phase 9 makes Redon
**observable**: it counts what it's doing (commands, hits, errors, connections,
latency) and exposes those numbers two ways:

- an **`INFO` command** over the normal protocol — the quick human view, and
- a **Prometheus `/metrics` HTTP endpoint** — the machine view that Prometheus
  scrapes and **Grafana** graphs.

This is the final piece that makes Redon "look like a real database."

## What we measure ([metrics.cpp](../src/metrics.cpp))

A single `Metrics` object holds a handful of counters:

| Metric | Meaning |
|---|---|
| `commands_total` | every command processed |
| `command{cmd=get/set/del}` | a breakdown by verb |
| `keyspace_hits / misses` | `GET`s that found vs missed a key → **hit rate** |
| `errors_total` | commands that returned `ERR` |
| `connections_total` / `connected_clients` | lifetime vs currently-open connections |
| `command_latency_microseconds_avg` | mean time to handle a command |
| `uptime_seconds`, `keys` | how long it's been up; how many keys it holds |

## Lock-free counters

These counters are updated on the **hot path** — every single command bumps at
least two of them, from any of the worker threads at once. Taking a mutex for
each would add contention to every request. Instead they're **`std::atomic`**:

```cpp
std::atomic<long long> commands_{0}, gets_{0}, get_hits_{0}, ...;
void on_command(const std::string& verb, bool is_error) {
    commands_.fetch_add(1, std::memory_order_relaxed);     // atomic, lock-free
    if (is_error) errors_.fetch_add(1, std::memory_order_relaxed);
    if (verb == "GET") gets_.fetch_add(1, std::memory_order_relaxed);
    ...
}
```

We use **`memory_order_relaxed`** because these are independent counters — we only
need each increment to be atomic, not ordered relative to the others. That's the
cheapest correct atomic, and exactly right for metrics.

## Where the numbers come from

The server records metrics in `handle_client`, around each command:

```cpp
verb_and_key(line, &verb, &key);                  // what command is this?
auto t0 = steady_clock::now();
reply = (verb == "INFO") ? metrics_.info_text(store_.size())
      : router_         ? route_line(line, ...)
      :                   execute_line(line, ...);
auto t1 = steady_clock::now();
if (real client command) {                         // skip blanks, replica stream, __RPC__
    metrics_.on_latency(t1 - t0);
    metrics_.on_command(verb, reply starts with "ERR");
    if (verb == "GET") metrics_.on_get(reply != "(nil)");   // hit vs miss
}
```

Connection counts come from the `ConnectionScope` RAII guard (the same one that
closes the socket) calling `on_connect`/`on_disconnect`, so they're correct even
if a handler throws.

> A real bug the demo caught: `verb_and_key` didn't strip a leading UTF-8 **BOM**
> the way the command parser does. So a first command carrying a BOM (a
> Notepad-saved file) was *stored* correctly but *mis-counted* — and worse, the
> **router** would have hashed the BOM-prefixed key to the *wrong shard*. Fixed by
> stripping the BOM in `verb_and_key` too, so routing, storage, and metrics all
> agree on the key. (Same lesson as Phase 7: every component must parse a key
> identically.)

## The `INFO` command

`INFO` returns a **single line** of `key=value` pairs:
```
uptime_s=0 clients=1 conns_total=1 commands=5 get=2 set=3 del=0 hits=1 misses=1
hit_rate=0.500 errors=0 avg_latency_us=9.14 keys=3
```
It has to be one line on purpose — our reply protocol is line-based (one reply =
one line), so an `INFO` with embedded newlines would desync the client. (Redis can
use multi-line because its replies are length-prefixed; ours aren't.)

## The Prometheus endpoint ([metrics.cpp](../src/metrics.cpp))

`MetricsHttp` is a tiny HTTP server on its own port (`--metrics-port`). A
background thread accepts a connection, ignores the request (it serves metrics at
any path), and writes a normal HTTP response whose body is the **Prometheus
exposition format**:

```
# HELP redon_commands_total Commands processed since start.
# TYPE redon_commands_total counter
redon_commands_total 7
redon_command_total{cmd="get"} 2
redon_command_total{cmd="set"} 3
redon_keyspace_hits_total 1
...
redon_keys 3
```

`# HELP`/`# TYPE` lines and `name value` pairs are all Prometheus needs. Point a
Prometheus server at `host:port/metrics`, add a Grafana dashboard, and you have
graphs of requests/sec, hit rate, and latency.

The listener thread shuts down cleanly: the destructor sets a `running_` flag
false and **closes the listening socket**, which makes the blocked `accept()`
return so the thread can exit and be joined — the same interruptible-accept
pattern real servers use (and which we deliberately skipped for the main loop).

```sh
redon-server 127.0.0.1 6380 4 none 0 0 --metrics-port 9090
# then:  curl http://127.0.0.1:9090/metrics
#        redon-cli 6380   ->   INFO
```

## What this build does and doesn't do (honesty)

- ✅ Counts the things that matter, with negligible overhead (lock-free atomics),
  and exposes them in the two standard ways (a human command + Prometheus).
- ⚠️ **Averages, not histograms.** We report *mean* latency; real monitoring wants
  **percentiles** (p50/p99), which need a histogram (Prometheus has a histogram
  type for exactly this). A clear next step.
- ⚠️ **No replication-lag metric.** In a leader/follower setup you'd want to graph
  how far behind each follower is; we don't expose that yet.
- ⚠️ **The HTTP server is minimal** — it doesn't parse the request path or method,
  serves metrics to anyone who connects, and isn't authenticated. Fine on a
  private network behind Prometheus; a real one would scope it.

## New ideas this phase introduces

| Concept | Meaning here |
|---|---|
| **observability / metrics** | numbers that describe what the system is doing |
| **lock-free counters** | `std::atomic` updates so metrics don't slow the hot path |
| **memory_order_relaxed** | the cheapest atomic — fine for independent counters |
| **hit rate** | fraction of `GET`s that found a key — the key cache health metric |
| **Prometheus exposition format** | the text format scrapers read (`# HELP/# TYPE/name value`) |
| **scrape / dashboard** | Prometheus pulls `/metrics`; Grafana graphs it |

**Recap:** Redon is now observable. Lock-free atomic counters track commands, hit
rate, errors, connections, and latency with no hot-path locking; an `INFO` command
gives the human view; and a Prometheus `/metrics` HTTP endpoint lets Prometheus and
Grafana watch the server in real time. With this, the project covers the full arc
of a real distributed database — and the natural next steps (latency *percentiles*,
a replication-lag gauge) are clear.
