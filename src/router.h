// router.h — sharding: spread the keyspace across many shard nodes.
//
// One machine can only hold so many keys. Sharding scales HORIZONTALLY: split the
// keys across N independent shard servers, so the cluster holds ~N times as much.
// A router node sits in front: for each command it computes which shard owns the
// key — `shard = hash(key) % N` — and forwards the command there, relaying the
// reply. The shard servers are ordinary redon-servers, unaware they're shards.
//
// The hash is FNV-1a, chosen over std::hash because it is simple, well-spread,
// and STABLE across runs and platforms — so a given key always maps to the same
// shard.
#ifndef REDON_ROUTER_H
#define REDON_ROUTER_H

#include <cstddef>
#include <string>
#include <vector>

namespace redon {

class Router {
public:
    explicit Router(std::vector<std::string> shards);  // "host:port" of each shard

    std::size_t shard_count() const { return shards_.size(); }
    const std::string& shard_addr(std::size_t i) const { return shards_[i]; }

    // Which shard owns `key`: FNV-1a(key) % N.
    std::size_t shard_for(const std::string& key) const;

    // Forward `command_line` (no trailing newline) to shard `i` and return its
    // reply line (newline stripped), or an "ERR ROUTER ..." string if the shard
    // is unreachable. Opens a fresh connection per call, so it is naturally safe
    // to call from many worker threads at once.
    std::string forward(std::size_t i, const std::string& command_line);

private:
    std::vector<std::string> shards_;
};

}  // namespace redon

#endif  // REDON_ROUTER_H
