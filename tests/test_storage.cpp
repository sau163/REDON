// test_storage.cpp — unit tests for the Storage engine.
#include <atomic>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "storage.h"
#include "test_util.h"

using redon::Storage;

void test_set_then_get() {
    Storage s;
    std::string value;
    CHECK(!s.get("missing", &value));  // absent before anything is set
    s.set("name", "Saurabh");
    CHECK(s.get("name", &value));
    CHECK_EQ(value, std::string("Saurabh"));
    CHECK(s.exists("name"));
    CHECK_EQ(s.size(), static_cast<std::size_t>(1));
}

void test_overwrite_keeps_one_entry() {
    Storage s;
    s.set("k", "one");
    s.set("k", "two");  // overwrite, not insert
    std::string value;
    CHECK(s.get("k", &value));
    CHECK_EQ(value, std::string("two"));
    CHECK_EQ(s.size(), static_cast<std::size_t>(1));
}

void test_del_reports_count() {
    Storage s;
    s.set("k", "v");
    CHECK_EQ(s.del("k"), static_cast<std::size_t>(1));  // removed exactly one
    CHECK_EQ(s.del("k"), static_cast<std::size_t>(0));  // already gone
    CHECK(!s.exists("k"));
    std::string value;
    CHECK(!s.get("k", &value));
}

void test_get_leaves_out_param_untouched_when_missing() {
    Storage s;
    std::string value = "sentinel";
    CHECK(!s.get("nope", &value));
    CHECK_EQ(value, std::string("sentinel"));  // must not be modified
}

// Many threads hammer the SAME small set of keys at once. This exercises the
// internal mutex under real contention: if it were missing or wrong, the
// unordered_map could corrupt (crash) or lose data. We can't predict which
// writer wins a given key (last-writer-wins, racing), but every key must end up
// holding one of the values that was legitimately written — never garbage.
void test_concurrent_same_key_access() {
    Storage s;
    constexpr int num_threads = 8;
    constexpr int iterations = 20000;
    constexpr int num_keys = 16;

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&s, t, iterations, num_keys]() {
            const std::string value = "t" + std::to_string(t);
            std::string out;
            for (int i = 0; i < iterations; ++i) {
                const std::string key = "k" + std::to_string(i % num_keys);
                s.set(key, value);
                s.get(key, &out);  // concurrent reader; must not crash/corrupt
            }
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }

    // No data was lost and no value is corrupted: every key still exists and
    // holds a well-formed "t<id>" value from some writer.
    for (int k = 0; k < num_keys; ++k) {
        std::string out;
        CHECK(s.get("k" + std::to_string(k), &out));
        bool well_formed = out.size() >= 2 && out[0] == 't';
        CHECK(well_formed);
    }
    CHECK_EQ(s.size(), static_cast<std::size_t>(num_keys));
}

// With a capacity, inserting beyond it evicts the least-recently-used key.
void test_lru_evicts_least_recently_used() {
    Storage s(3);
    s.set("a", "1");
    s.set("b", "2");
    s.set("c", "3");
    CHECK_EQ(s.size(), static_cast<std::size_t>(3));
    s.set("d", "4");  // over capacity -> evict the LRU (a, oldest & untouched)
    CHECK_EQ(s.size(), static_cast<std::size_t>(3));
    CHECK(!s.exists("a"));  // evicted
    CHECK(s.exists("b"));
    CHECK(s.exists("c"));
    CHECK(s.exists("d"));
}

// A read (get) marks a key most-recently-used, sparing it from the next eviction.
void test_get_updates_recency() {
    Storage s(3);
    s.set("a", "1");
    s.set("b", "2");
    s.set("c", "3");
    std::string v;
    CHECK(s.get("a", &v));  // a is now most-recently-used; b is the LRU
    s.set("d", "4");        // evict the LRU, which is now b (not a)
    CHECK(s.exists("a"));    // survived because we just read it
    CHECK(!s.exists("b"));   // b evicted
    CHECK(s.exists("c"));
    CHECK(s.exists("d"));
}

// Overwriting an existing key is a use, not growth: size is unchanged and the
// key becomes most-recently-used.
void test_overwrite_is_a_use_not_growth() {
    Storage s(2);
    s.set("a", "1");
    s.set("b", "2");
    s.set("a", "11");  // update a -> now MRU; size still 2
    CHECK_EQ(s.size(), static_cast<std::size_t>(2));
    s.set("c", "3");   // evict the LRU, which is b
    CHECK(s.exists("a"));
    CHECK(!s.exists("b"));
    CHECK(s.exists("c"));
    std::string v;
    CHECK(s.get("a", &v));
    CHECK_EQ(v, std::string("11"));
}

// Capacity 0 means unbounded: nothing is ever evicted.
void test_capacity_zero_is_unbounded() {
    Storage s;  // default capacity 0
    for (int i = 0; i < 1000; ++i) {
        s.set("k" + std::to_string(i), "v");
    }
    CHECK_EQ(s.size(), static_cast<std::size_t>(1000));
    CHECK(s.exists("k0"));     // the very first key is still present
    CHECK(s.exists("k999"));
}

int main() {
    RUN(test_set_then_get);
    RUN(test_overwrite_keeps_one_entry);
    RUN(test_del_reports_count);
    RUN(test_get_leaves_out_param_untouched_when_missing);
    RUN(test_concurrent_same_key_access);
    RUN(test_lru_evicts_least_recently_used);
    RUN(test_get_updates_recency);
    RUN(test_overwrite_is_a_use_not_growth);
    RUN(test_capacity_zero_is_unbounded);
    return REPORT();
}
