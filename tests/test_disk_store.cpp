// test_disk_store.cpp — tests for the on-disk storage engine.
#include <cstdio>  // std::remove
#include <fstream>
#include <string>

#include "disk_store.h"
#include "test_util.h"

using redon::DiskStore;

namespace {
const char* kPath = "test_redon_disk.tmp";
void cleanup() {
    std::remove(kPath);
    std::remove((std::string(kPath) + ".tmp").c_str());
}
}  // namespace

// Writes survive: a DiskStore reopened on the same file recovers all live keys
// (overwrites and deletes resolved) from disk.
void test_basic_and_persistence() {
    cleanup();
    {
        DiskStore d(kPath);
        CHECK(d.ok());
        CHECK(d.set("name", "Saurabh"));
        CHECK(d.set("city", "Kolkata West Bengal"));  // value with spaces
        CHECK(d.set("name", "Sourav"));               // overwrite
        CHECK(d.del("city"));                          // delete
        std::string v;
        CHECK(d.get("name", &v));
        CHECK_EQ(v, std::string("Sourav"));
        CHECK(!d.exists("city"));
        CHECK_EQ(d.size(), static_cast<std::size_t>(1));
    }  // closes the file

    {
        DiskStore d(kPath);  // reopen: rebuild the index from disk
        CHECK(d.ok());
        std::string v;
        CHECK(d.get("name", &v));
        CHECK_EQ(v, std::string("Sourav"));  // last write won, on disk
        CHECK(!d.exists("city"));             // delete persisted
        CHECK_EQ(d.size(), static_cast<std::size_t>(1));
    }
    cleanup();
}

// A missing key leaves the out-param untouched, and deleting it removes nothing.
void test_missing_key() {
    cleanup();
    DiskStore d(kPath);
    std::string v = "sentinel";
    CHECK(!d.get("nope", &v));
    CHECK_EQ(v, std::string("sentinel"));
    CHECK(!d.del("nope"));
    cleanup();
}

// clear() empties the store and it stays usable afterward.
void test_clear() {
    cleanup();
    DiskStore d(kPath);
    d.set("a", "1");
    d.set("b", "2");
    CHECK_EQ(d.size(), static_cast<std::size_t>(2));
    d.clear();
    CHECK_EQ(d.size(), static_cast<std::size_t>(0));
    std::string v;
    CHECK(!d.get("a", &v));
    CHECK(d.set("c", "3"));  // still works
    CHECK(d.get("c", &v));
    CHECK_EQ(v, std::string("3"));
    cleanup();
}

// snapshot() returns every live key/value (used by replication).
void test_snapshot() {
    cleanup();
    DiskStore d(kPath);
    d.set("a", "1");
    d.set("b", "2");
    auto snap = d.snapshot();
    CHECK_EQ(snap.size(), static_cast<std::size_t>(2));
    cleanup();
}

// Compaction reclaims the garbage from many overwrites while preserving data:
// after 2000 overwrites of one key, a reopen compacts and the value is intact.
void test_compaction_preserves_data() {
    cleanup();
    {
        DiskStore d(kPath);
        for (int i = 0; i < 2000; ++i) {
            d.set("k", "v" + std::to_string(i));
        }
        std::string v;
        CHECK(d.get("k", &v));
        CHECK_EQ(v, std::string("v1999"));
        CHECK_EQ(d.size(), static_cast<std::size_t>(1));
    }
    {
        DiskStore d(kPath);  // reopen triggers compaction (file >> live data)
        std::string v;
        CHECK(d.get("k", &v));
        CHECK_EQ(v, std::string("v1999"));  // value survived compaction
        CHECK_EQ(d.size(), static_cast<std::size_t>(1));
    }
    cleanup();
}

// A corrupt/truncated tail (from a crash mid-append) must be HEALED on recovery,
// so writes appended afterward survive the next restart too. (Regression for the
// "tail left in place -> later writes lost" bug.)
void test_corrupt_tail_is_healed() {
    cleanup();
    {
        DiskStore d(kPath);
        d.set("a", "1");
        d.set("b", "2");
    }
    // Append a truncated record (header says a 10-byte value, only 2 bytes given,
    // no newline) — small enough that the compaction threshold won't trip.
    {
        std::ofstream f(kPath, std::ios::out | std::ios::app | std::ios::binary);
        f << "SET 1 10 cXY";
    }
    {
        DiskStore d(kPath);  // recover() heals the tail
        CHECK(d.ok());
        std::string v;
        CHECK(d.get("a", &v));
        CHECK(!d.exists("c"));  // torn record dropped
        CHECK(d.set("z", "9"));  // append after healing
    }
    {
        DiskStore d(kPath);  // the post-heal write must survive
        std::string v;
        CHECK(d.get("a", &v));
        CHECK_EQ(v, std::string("1"));
        CHECK(d.get("z", &v));
        CHECK_EQ(v, std::string("9"));
        CHECK(!d.exists("c"));
    }
    cleanup();
}

int main() {
    RUN(test_basic_and_persistence);
    RUN(test_corrupt_tail_is_healed);
    RUN(test_missing_key);
    RUN(test_clear);
    RUN(test_snapshot);
    RUN(test_compaction_preserves_data);
    return REPORT();
}
