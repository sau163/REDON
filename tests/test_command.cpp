// test_command.cpp — tests for the protocol layer (parsing + execution).
//
// execute_line() is pure with respect to a Storage object — no sockets — so we
// can exercise the entire command language here without any networking.
#include <string>

#include "command.h"
#include "storage.h"
#include "test_util.h"

using redon::execute_line;
using redon::Storage;

// Helper: run one line against `store`, optionally reporting the close flag.
static std::string run(Storage& store, const std::string& line,
                       bool* closed = nullptr) {
    bool c = false;
    std::string reply = execute_line(line, store, &c);
    if (closed != nullptr) {
        *closed = c;
    }
    return reply;
}

void test_basic_flow() {
    Storage s;
    CHECK_EQ(run(s, "SET name Saurabh"), std::string("OK"));
    CHECK_EQ(run(s, "GET name"), std::string("Saurabh"));
    CHECK_EQ(run(s, "EXISTS name"), std::string("(integer) 1"));
    CHECK_EQ(run(s, "DEL name"), std::string("(integer) 1"));
    CHECK_EQ(run(s, "GET name"), std::string("(nil)"));
    CHECK_EQ(run(s, "EXISTS name"), std::string("(integer) 0"));
}

void test_value_may_contain_spaces() {
    Storage s;
    CHECK_EQ(run(s, "SET title Senior Software Engineer"), std::string("OK"));
    CHECK_EQ(run(s, "GET title"), std::string("Senior Software Engineer"));
}

void test_verbs_case_insensitive_and_synonyms() {
    Storage s;
    CHECK_EQ(run(s, "set k v"), std::string("OK"));
    CHECK_EQ(run(s, "GeT k"), std::string("v"));
    CHECK_EQ(run(s, "delete k"), std::string("(integer) 1"));  // DELETE == DEL
}

void test_crlf_is_tolerated() {
    Storage s;
    CHECK_EQ(run(s, "SET k v\r"), std::string("OK"));  // trailing CR trimmed
    CHECK_EQ(run(s, "GET k\r"), std::string("v"));
}

void test_extra_whitespace_is_tolerated() {
    Storage s;
    CHECK_EQ(run(s, "   SET   k   hello world  "), std::string("OK"));
    CHECK_EQ(run(s, "GET k"), std::string("hello world"));
}

void test_leading_utf8_bom_is_tolerated() {
    Storage s;
    // A UTF-8 BOM (EF BB BF) in front of the first command must be ignored, as
    // happens when piping a Notepad-saved "UTF-8" command file.
    CHECK_EQ(run(s, "\xEF\xBB\xBF" "SET k v"), std::string("OK"));
    CHECK_EQ(run(s, "GET k"), std::string("v"));
}

void test_ping_and_quit() {
    Storage s;
    CHECK_EQ(run(s, "PING"), std::string("PONG"));

    bool closed = false;
    CHECK_EQ(run(s, "QUIT", &closed), std::string("OK"));
    CHECK(closed);  // QUIT must request connection close

    closed = false;
    run(s, "GET k", &closed);
    CHECK(!closed);  // ordinary commands must not request close
}

void test_no_arg_commands_reject_extra_args() {
    Storage s;
    // PING and QUIT take no arguments; extra tokens must be rejected, matching
    // the strictness of GET/DEL/EXISTS.
    CHECK_EQ(run(s, "PING extra"),
             std::string("ERR wrong number of arguments for 'ping' command"));
    bool closed = false;
    CHECK_EQ(run(s, "QUIT now", &closed),
             std::string("ERR wrong number of arguments for 'quit' command"));
    CHECK(!closed);  // a rejected QUIT must NOT close the connection
}

void test_argument_and_unknown_errors() {
    Storage s;
    CHECK_EQ(run(s, "SET"),
             std::string("ERR wrong number of arguments for 'set' command"));
    CHECK_EQ(run(s, "SET onlykey"),
             std::string("ERR wrong number of arguments for 'set' command"));
    CHECK_EQ(run(s, "GET"),
             std::string("ERR wrong number of arguments for 'get' command"));
    CHECK_EQ(run(s, "GET a b"),
             std::string("ERR wrong number of arguments for 'get' command"));
    CHECK_EQ(run(s, "BOGUS x"), std::string("ERR unknown command 'BOGUS'"));
    CHECK_EQ(run(s, ""), std::string(""));  // blank line -> empty reply
}

// Phase 5: a follower is read-only to clients, but the replication handshake
// turns a connection into the write stream (and resets the store for a full
// sync), after which writes on that link are applied.
void test_follower_read_only_and_replica_link() {
    Storage s;
    bool closed = false;
    bool replica = false;

    // On a follower, client writes are rejected; reads are allowed.
    CHECK_EQ(execute_line("SET k v", s, &closed, true, &replica),
             std::string("ERR READONLY this node is a read-only replica"));
    CHECK_EQ(execute_line("DEL k", s, &closed, true, &replica),
             std::string("ERR READONLY this node is a read-only replica"));
    CHECK_EQ(execute_line("GET k", s, &closed, true, &replica),
             std::string("(nil)"));

    // The replication handshake flags the link and clears the store for a sync.
    s.set("old", "data");
    CHECK_EQ(execute_line("__REPLSYNC__", s, &closed, true, &replica),
             std::string("OK"));
    CHECK(replica);
    CHECK(!s.exists("old"));

    // Writes ON THE LINK now apply even though the node is a follower.
    CHECK_EQ(execute_line("SET k v", s, &closed, true, &replica),
             std::string("OK"));
    CHECK(s.exists("k"));

    // A leader (node_is_follower=false) always accepts client writes.
    Storage leader;
    bool not_replica = false;
    CHECK_EQ(execute_line("SET a 1", leader, &closed, false, &not_replica),
             std::string("OK"));
}

int main() {
    RUN(test_follower_read_only_and_replica_link);
    RUN(test_basic_flow);
    RUN(test_value_may_contain_spaces);
    RUN(test_verbs_case_insensitive_and_synonyms);
    RUN(test_crlf_is_tolerated);
    RUN(test_extra_whitespace_is_tolerated);
    RUN(test_leading_utf8_bom_is_tolerated);
    RUN(test_ping_and_quit);
    RUN(test_no_arg_commands_reject_extra_args);
    RUN(test_argument_and_unknown_errors);
    return REPORT();
}
