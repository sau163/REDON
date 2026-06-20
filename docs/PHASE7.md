# Phase 7 — explained (sharding)

Replication (Phase 5) gave us **copies** of the data — every node holds the
*same* keys, for fault tolerance. But it doesn't help with *size*: if the dataset
is bigger than one machine's RAM, copying it everywhere doesn't fit. **Sharding**
solves the size problem the opposite way: **split** the keys across machines so
each holds only a *slice*. Three machines hold ~3× the data. This is **horizontal
scaling** — add machines to add capacity.

```
                         client
                           │  (every command)
                           ▼
                       ┌────────┐   shard = hash(key) % 3
                       │ ROUTER │───────────┬───────────┐
                       └────────┘           │           │
                            ▼               ▼           ▼
                       ┌────────┐      ┌────────┐  ┌────────┐
                       │ shard0 │      │ shard1 │  │ shard2 │
                       │ user:1 │      │ user:3 │  │ user:5 │
                       │ user:2 │      │ user:4 │  │ user:6 │
                       └────────┘      └────────┘  └────────┘
```

## The one rule

> Each key belongs to exactly one shard, chosen by **`shard = hash(key) % N`**.

Because the hash is **deterministic**, the same key always maps to the same
shard — so after you `SET user:1` on shard 0, a later `GET user:1` is sent to
shard 0 and finds it. That's the entire idea; everything else is plumbing.

## Who does the routing? A router node ([router.cpp](../src/router.cpp))

There are three ways to get a command to the right shard:

1. **Smart client** — the client computes the shard itself. Needs every client to
   know the shard map.
2. **Router / proxy** — a node in front computes the shard and forwards. Clients
   stay dumb. *(This is what we built — like twemproxy in front of Redis.)*
3. **Redirect** — any node, on a key it doesn't own, tells the client "go to shard
   2" (Redis Cluster's `MOVED`). Needs a redirect-following client.

Our **router** is a `redon-server` started with `--shard` addresses. It stores no
data; for each command it picks the shard and forwards:

```cpp
std::string Server::route_line(const std::string& line, bool* should_close) {
    auto [verb, key] = verb_and_key(line);     // first two tokens
    if (verb == "QUIT") { *should_close = true; return "OK"; }
    if (verb == "PING") return "PONG";          // keyless: answer locally
    if (key.empty())    return "ERR ROUTER: needs a key";
    return router_->forward(router_->shard_for(key), line);  // hash → shard → forward
}
```

The shards themselves are **ordinary, unmodified** `redon-server` instances —
they have no idea they're shards. The router is the only thing that knows the
layout. (Notice it routes on the **key**, the *second* token — `SET user:1 Bob`
hashes `user:1`, not `Bob`.)

## The hash: FNV-1a, not `std::hash` ([router.cpp](../src/router.cpp))

```cpp
std::uint64_t fnv1a(const std::string& s) {
    std::uint64_t h = 1469598103934665603ULL;     // FNV offset basis
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }  // xor, then FNV prime
    return h;
}
size_t shard_for(key) { return fnv1a(key) % shards_.size(); }
```

Why not just `std::hash<std::string>`? Because `std::hash` is allowed to differ
between platforms and even between program runs (some implementations randomize
it). A shard map **must be stable** — `user:1` has to hash to the same shard
forever, or you'd lose track of where keys live. **FNV-1a** is tiny, well-spread,
and identical everywhere, which is exactly what sharding needs. In the demo it
split 6 keys cleanly 2-2-2 across 3 shards.

## Forwarding ([router.cpp](../src/router.cpp))

```cpp
std::string Router::forward(size_t i, const std::string& command_line) {
    sock = connect_tcp(host, port);                 // a FRESH connection per call
    if (!sock) return "ERR ROUTER: shard ... unreachable";
    send_all(sock, command_line + "\n");
    recv_line(sock, &reply);                          // one request → one reply line
    close(sock);
    return reply;                                     // relay it to the client
}
```

Opening a **new connection per command** is deliberately the simplest thing that's
**correct under concurrency**: many worker threads route many clients at once, and
because no socket is shared between them, there's nothing to lock. (A production
router would *pool* connections to avoid the connect cost — the natural next
optimization.) A dead shard just makes `connect_tcp` fail, and the client gets a
clear `ERR ROUTER: shard ... unreachable`.

## See it work

```sh
# three plain shard servers, then a router in front of them
redon-server 127.0.0.1 6701 4 none      # shard 0
redon-server 127.0.0.1 6702 4 none      # shard 1
redon-server 127.0.0.1 6703 4 none      # shard 2
redon-server 127.0.0.1 6700 4 none 0 0 \
    --shard 127.0.0.1:6701 --shard 127.0.0.1:6702 --shard 127.0.0.1:6703   # router
```
```
# talk only to the router (6700):
SET user:1 Bob   ->  OK        # lands on whichever shard hash(user:1) picks
GET user:1       ->  Bob       # routed back to the same shard

# connect to a shard directly and you'll see only ITS slice of the keys.
```

## What this build does and doesn't do (honesty)

- ✅ **Splits the keyspace** across N shards by a stable hash; the router makes it
  transparent to clients; capacity scales with the number of shards.
- ⚠️ **`% N` rehashes everything when N changes.** Add or remove a shard and almost
  every key maps somewhere new — a giant reshuffle. The real-world fix is
  **consistent hashing** (a hash ring) or **hash slots** (Redis Cluster's 16384
  slots), which move only a small fraction of keys when the cluster resizes. A
  great next step.
- ⚠️ **No replication of shards.** Each shard is a single point of failure — if
  shard 1 dies, its slice is gone. The complete system **combines** sharding with
  replication: each shard is itself a replicated (Phase 5) or Raft (Phase 6)
  group. That layering — *shard for size, replicate each shard for safety* — is
  how real distributed databases (Redis Cluster, Cassandra, etc.) are built.
- ⚠️ **Single-key commands only.** A command touching two keys on two shards (a
  cross-shard transaction) isn't supported — that's a famously hard problem.
- ⚠️ **The router is one process.** It's stateless, so you can run several behind a
  load balancer, but here it's a single hop.

## New ideas this phase introduces

| Concept | Meaning here |
|---|---|
| **sharding / partitioning** | split the keyspace so each node holds a slice |
| **horizontal scaling** | add machines to add capacity (vs. a bigger machine) |
| **hash(key) % N** | the deterministic key → shard mapping |
| **stable hash (FNV-1a)** | a hash that's identical across runs/platforms |
| **router / proxy** | the node that hides the shard map from clients |
| **replication vs sharding** | *copy* for safety vs *split* for size — they compose |
| **consistent hashing** (future) | resize the cluster without rehashing everything |

**Recap:** Redon now scales *out*. A router hashes each key to one of N shards and
forwards the command there, so the cluster holds far more than any single machine.
The two big directions left — **consistent hashing** (so resizing doesn't reshuffle
everything) and **replicating each shard** (so a shard isn't a single point of
failure) — are exactly how a toy sharding layer grows into a real distributed
database.
