// test_router.cpp — tests for the sharding key->shard mapping (no networking).
#include <string>
#include <vector>

#include "router.h"
#include "test_util.h"

using redon::Router;

// Every key maps into [0, N) and the mapping is deterministic (same key, same
// shard every time) — the property that makes a key findable again.
void test_shard_for_in_range_and_deterministic() {
    Router r({"a:1", "b:2", "c:3"});
    CHECK_EQ(r.shard_count(), static_cast<std::size_t>(3));
    for (int i = 0; i < 200; ++i) {
        const std::string key = "user:" + std::to_string(i);
        const std::size_t s = r.shard_for(key);
        CHECK(s < 3u);
        CHECK_EQ(r.shard_for(key), s);  // stable across calls
    }
}

// FNV-1a spreads keys, so with many keys every shard gets a share (not all to one).
void test_keys_are_distributed() {
    Router r({"a:1", "b:2", "c:3"});
    std::vector<int> counts(3, 0);
    for (int i = 0; i < 600; ++i) {
        counts[r.shard_for("k" + std::to_string(i))]++;
    }
    for (int c : counts) {
        CHECK(c > 0);
    }
}

// With a single shard, everything maps to shard 0.
void test_single_shard() {
    Router r({"only:1"});
    CHECK_EQ(r.shard_for("anything"), static_cast<std::size_t>(0));
    CHECK_EQ(r.shard_for("else"), static_cast<std::size_t>(0));
}

int main() {
    RUN(test_shard_for_in_range_and_deterministic);
    RUN(test_keys_are_distributed);
    RUN(test_single_shard);
    return REPORT();
}
