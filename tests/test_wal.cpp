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

int main() {
    RUN(test_replay_reconstructs_state);
    RUN(test_missing_file_is_a_fresh_start);
    RUN(test_value_with_spaces_survives);
    RUN(test_truncated_trailing_record_is_ignored);
    return REPORT();
}
