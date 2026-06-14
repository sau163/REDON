// test_wal.cpp — tests for the Write-Ahead Log: append, replay, and recovery.
#include <cstdio>    // std::remove
#include <fstream>
#include <string>

#include "storage.h"
#include "test_util.h"
#include "wal.h"

using redon::Storage;
using redon::Wal;

namespace {
const char* kPath = "test_redon_wal.tmp";
void cleanup() { std::remove(kPath); }
}  // namespace

// Writing through an attached WAL and then replaying into a fresh Storage must
// reproduce the exact same state — including overwrites and deletes.
void test_replay_reconstructs_state() {
    cleanup();
    {
        Storage s;
        Wal wal(kPath);
        wal.replay_into(s);      // nothing to replay yet
        CHECK(wal.open_for_append());
        s.attach_wal(&wal);

        s.set("name", "Saurabh");
        s.set("city", "Kolkata");
        s.set("name", "Sourav");  // overwrite
        s.del("city");            // delete
        s.set("lang", "C++");
    }  // wal goes out of scope; file is flushed and closed

    Storage restored;
    Wal wal2(kPath);
    std::size_t replayed = wal2.replay_into(restored);

    CHECK(replayed >= static_cast<std::size_t>(4));
    std::string v;
    CHECK(restored.get("name", &v));
    CHECK_EQ(v, std::string("Sourav"));   // last write wins
    CHECK(!restored.exists("city"));        // deleted
    CHECK(restored.get("lang", &v));
    CHECK_EQ(v, std::string("C++"));
    CHECK_EQ(restored.size(), static_cast<std::size_t>(2));  // name, lang
    cleanup();
}

// If the WAL can't be written (here: never opened for append), a SET must FAIL
// rather than silently apply to memory and lie to the client. Memory must stay
// consistent with the (empty) log.
void test_set_fails_when_wal_not_writable() {
    cleanup();
    Storage s;
    Wal wal(kPath);
    // Deliberately skip open_for_append(): the stream is not writable.
    s.attach_wal(&wal);

    CHECK(!s.set("k", "v"));    // durable write failed -> set reports failure
    CHECK(!s.exists("k"));       // ...and the mutation was NOT applied
    CHECK(!wal.ok());            // the WAL marks itself unhealthy

    // DEL of an existing key must likewise refuse and report non-durable. (Seed
    // a key directly via a writable WAL would re-open it; instead verify a
    // missing-key delete is still treated as durable no-op.)
    bool durable = true;
    CHECK_EQ(s.del("absent", &durable), static_cast<std::size_t>(0));
    CHECK(durable);             // deleting a missing key is a durable no-op
    cleanup();
}

// A missing log file is a normal first start: replay does nothing, no error.
void test_missing_file_is_a_fresh_start() {
    cleanup();
    Storage s;
    Wal wal(kPath);
    CHECK_EQ(wal.replay_into(s), static_cast<std::size_t>(0));
    CHECK_EQ(s.size(), static_cast<std::size_t>(0));
    cleanup();
}

// Values containing spaces must survive the round trip (length-prefixed format).
void test_value_with_spaces_survives() {
    cleanup();
    {
        Storage s;
        Wal wal(kPath);
        wal.replay_into(s);
        CHECK(wal.open_for_append());
        s.attach_wal(&wal);
        s.set("title", "Senior Software Engineer");
    }
    Storage restored;
    Wal wal2(kPath);
    wal2.replay_into(restored);
    std::string v;
    CHECK(restored.get("title", &v));
    CHECK_EQ(v, std::string("Senior Software Engineer"));
    cleanup();
}

// A record cut short by a crash (here, a deliberately truncated trailing record)
// must be ignored, while every complete record before it is still applied.
void test_truncated_trailing_record_is_ignored() {
    cleanup();
    {
        Storage s;
        Wal wal(kPath);
        wal.replay_into(s);
        CHECK(wal.open_for_append());
        s.attach_wal(&wal);
        s.set("a", "1");
        s.set("b", "2");
    }
    // Hand-append a record that claims a 10-byte value but supplies only 3,
    // with no trailing newline — exactly what a crash mid-write looks like.
    {
        std::ofstream f(kPath, std::ios::out | std::ios::app | std::ios::binary);
        f << "SET 1 10 cXYZ";
    }

    Storage restored;
    Wal wal2(kPath);
    std::size_t replayed = wal2.replay_into(restored);

    CHECK_EQ(replayed, static_cast<std::size_t>(2));  // only the 2 good records
    std::string v;
    CHECK(restored.get("a", &v));
    CHECK_EQ(v, std::string("1"));
    CHECK(restored.get("b", &v));
    CHECK_EQ(v, std::string("2"));
    CHECK(!restored.exists("c"));  // the torn record was not applied
    cleanup();
}

// A record whose data is fully present but whose trailing newline is missing
// (a crash that wrote the bytes but not the terminator) must be ignored.
void test_record_without_terminator_is_ignored() {
    cleanup();
    {
        Storage s;
        Wal wal(kPath);
        wal.replay_into(s);
        CHECK(wal.open_for_append());
        s.attach_wal(&wal);
        s.set("a", "1");
    }
    {
        std::ofstream f(kPath, std::ios::out | std::ios::app | std::ios::binary);
        f << "SET 1 1 b2";  // complete data, but NO trailing newline
    }
    Storage restored;
    Wal wal2(kPath);
    std::size_t replayed = wal2.replay_into(restored);
    CHECK_EQ(replayed, static_cast<std::size_t>(1));
    CHECK(restored.exists("a"));
    CHECK(!restored.exists("b"));  // unterminated record not applied
    cleanup();
}

// A corrupt record (here a zero-length key) halts replay safely: the good
// record before it is applied, and replay stops rather than misreading on.
void test_corrupt_record_halts_replay() {
    cleanup();
    {
        std::ofstream f(kPath, std::ios::out | std::ios::binary);
        f << "SET 1 1 a1\n";  // valid
        f << "SET 0 1 b\n";   // corrupt: zero-length key
        f << "SET 1 1 c3\n";  // valid bytes, but sits after the corruption
    }
    Storage s;
    Wal wal(kPath);
    std::size_t replayed = wal.replay_into(s);
    CHECK_EQ(replayed, static_cast<std::size_t>(1));  // stops at the bad record
    CHECK(s.exists("a"));
    CHECK(!s.exists("c"));  // conservative: nothing after corruption is applied
    cleanup();
}

// An LRU eviction is logged as a DEL, and replay reconstructs the same key set
// (the evicted key stays gone, the survivors come back) without double-evicting.
void test_eviction_is_logged_and_replayed() {
    cleanup();
    {
        Storage s(2);  // capacity 2
        Wal wal(kPath);
        wal.replay_into(s);
        CHECK(wal.open_for_append());
        s.attach_wal(&wal);
        s.set("a", "1");
        s.set("b", "2");
        s.set("c", "3");  // over capacity -> evict "a", logged as DEL a
        CHECK(!s.exists("a"));
    }
    Storage restored(2);
    Wal wal2(kPath);
    wal2.replay_into(restored);
    CHECK(!restored.exists("a"));  // the eviction was logged and replayed
    CHECK(restored.exists("b"));
    CHECK(restored.exists("c"));
    CHECK_EQ(restored.size(), static_cast<std::size_t>(2));  // no double-evict
    cleanup();
}

int main() {
    RUN(test_replay_reconstructs_state);
    RUN(test_eviction_is_logged_and_replayed);
    RUN(test_set_fails_when_wal_not_writable);
    RUN(test_missing_file_is_a_fresh_start);
    RUN(test_value_with_spaces_survives);
    RUN(test_truncated_trailing_record_is_ignored);
    RUN(test_record_without_terminator_is_ignored);
    RUN(test_corrupt_record_halts_replay);
    return REPORT();
}
