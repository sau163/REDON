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

int main() {
    RUN(test_set_then_get);
    RUN(test_overwrite_keeps_one_entry);
    RUN(test_del_reports_count);
    RUN(test_get_leaves_out_param_untouched_when_missing);
    RUN(test_concurrent_same_key_access);
    return REPORT();
}
